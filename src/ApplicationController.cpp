#include "ApplicationController.h"
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

namespace pettime {
QRectF ApplicationController::areaAt(QPointF point) {
    if (auto *screen = QGuiApplication::screenAt(point.toPoint()))
        return screen->availableGeometry();
    QScreen *nearest = QGuiApplication::primaryScreen();
    double best = 1e30;
    for (auto *screen : QGuiApplication::screens()) {
        const QRectF r = screen->availableGeometry();
        const QPointF clamped(std::clamp(point.x(), r.left(), r.right()),
                              std::clamp(point.y(), r.top(), r.bottom()));
        const auto d = point - clamped;
        const double distance = d.x() * d.x() + d.y() * d.y();
        if (distance < best) {
            best = distance;
            nearest = screen;
        }
    }
    return nearest ? QRectF(nearest->availableGeometry()) : QRectF(0, 0, 1280, 720);
}
ApplicationController::ApplicationController(const AnimationLibrary &animation, SettingsStore store,
                                             RunOptions options, QObject *parent)
    : QObject(parent), animation_(animation), store_(std::move(store)),
      options_(std::move(options)),
      swarm_(options_.seed, areaAt(QCursor::pos()).center(), areaAt(QCursor::pos())),
      food_(options_.seed + 3),
      rewardRng_(options_.seed ^ 0xc2b2ae35U),
      primary_(swarm_.primary()), mainWindow_(primary_, animation_), crack_(options_.seed + 2) {
    QString warning;
    const bool hasConfig = store_.hasFile("config.json");
    const bool hasProgress = store_.hasFile("progress.json");
    const auto config = options_.smoke ? QJsonObject{} : store_.loadJson("config.json", &warning);
    startupWarning_ = warning;
    auto appendWarning = [this](const QString &text) {
        if (!text.isEmpty()) startupWarning_ += "\n" + text;
    };
    const SettingsStore legacy(store_.legacyDirectory());
    warning.clear();
    swarm_.setLimit(options_.smoke ? 72 : hasConfig
        ? config.value("populationLimit").toInt(72) : legacy.loadLimit(&warning));
    appendWarning(warning);
    warning.clear();
    if (!options_.smoke) {
        if (hasProgress) {
            const auto progress = store_.loadJson("progress.json", &warning);
            swarm_.restoreProgress(progress);
            food_.restore(progress.value("food").toObject());
            const double cooldown = progress.value("interaction").toObject()
                .value("cooldownRemaining").toDouble(0);
            interactionCooldown_ = std::isfinite(cooldown) ? std::clamp(cooldown, 0.0, 10.0) : 0;
        } else
            primary_.affection = legacy.loadAffinity(&warning);
    }
    appendWarning(warning);
    primary_.paused = config.value("paused").toBool(false);
    const double pace = config.value("pace").toDouble(1.5);
    primary_.pace = pace == .65 || pace == 1 || pace == 1.5 ? pace : 1.5;
    restoreNetwork_ = !options_.smoke && config.value("networkEnabled").toBool(false);
    bottomMode_ = config.value("bottomMode").toBool(false);
    mainWindow_.setBottomMode(bottomMode_);
    crack_.setBottomMode(bottomMode_);
    swarm_.beforeRemove = [this](std::uint64_t id) {
        settleFood(id);
        settleInteractions(id);
        progressChanged_ = true;
        if (controlledPet_ == id)
            stopControl();
        if (dispatch_)
            dispatch_->removeEntity(id);
        windows_.erase(id);
    };
    network_ = std::make_unique<NetworkService>(store_, [this] {
        return NetworkService::Info{swarm_.totalCount(), swarm_.limit(),
                                    dispatch_ ? dispatch_->awayCount() : 0,
                                    dispatch_ ? int(dispatch_->visitors().size()) : 0};
    });
    dispatch_ = std::make_unique<DispatchController>(*network_, swarm_, [] {
        auto *screen = QGuiApplication::primaryScreen();
        return screen ? QRectF(screen->availableGeometry()) : QRectF{};
    });
    dispatch_->localSurface = [](QPointF position) { return areaAt(position); };
    dispatch_->hideLocal = [this](quint64 id) {
        if (id == primary_.id)
            mainWindow_.hidePet();
        else if (const auto it = windows_.find(id); it != windows_.end())
            it->second->hidePet();
    };
    dispatch_->hideVisitor = [this](const QString &id) {
        if (const auto it = visitorWindows_.find(id); it != visitorWindows_.end())
            it->second->hidePet();
    };
    connect(dispatch_.get(), &DispatchController::changed, this, [this] {
        validateControl();
        if (networkPanel_)
            networkPanel_->setClosing(dispatch_->stoppingService());
        // Defer reconciliation: a removal callback may still be traversing the swarm.
        QTimer::singleShot(0, this, [this] {
            reconcileWindows();
            reconcileVisitors();
            if (petDialog_ && petDialog_->isVisible())
                refreshPets();
            refreshFood();
            refreshInteractions();
        });
    });
    connect(dispatch_.get(), &DispatchController::notice, this, [this](const QString &text) {
        store_.log(text);
        if (dispatchStatus_)
            dispatchStatus_->setText(text);
        if (tray_.isVisible())
            tray_.showMessage("网络派遣", text);
    });
    connect(dispatch_.get(), &DispatchController::visitorHighlight, this,
            [this](const QString &id) {
                reconcileVisitors();
                auto it = visitorWindows_.find(id);
                if (it != visitorWindows_.end())
                    it->second->highlight();
            });
    qApp->installEventFilter(this);
    makeMenu();
    connect(&mainWindow_, &PetWindow::contextRequested, this, [this](QPoint point) {
        contextPet_ = 1;
        menu_.popup(point);
    });
    connect(&mainWindow_, &PetWindow::exitRequested, qApp, &QApplication::quit);
    connect(&mainWindow_, &PetWindow::crushed, this, [this] { store_.log("Primary pet crushed"); });
    connect(&timer_, &QTimer::timeout, this, &ApplicationController::tick);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(16);
    auto watch = [this](QScreen *screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { updateScreens(); });
        connect(screen, &QScreen::geometryChanged, this, [this] { updateScreens(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { updateScreens(); });
    };
    for (auto *screen : QGuiApplication::screens())
        watch(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, watch](QScreen *screen) {
        watch(screen);
        updateScreens();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { updateScreens(); });
}
ApplicationController::~ApplicationController() {
    qApp->removeEventFilter(this);
    stopControl();
    timer_.stop();
    persist();
    dispatch_->shutdown();
    network_->stop();
    tray_.hide();
}
void ApplicationController::showNetwork() {
    if (!networkPanel_) {
        networkPanel_ = std::make_unique<NetworkPanel>(*network_);
        connect(networkPanel_.get(), &NetworkPanel::stopRequested, dispatch_.get(),
                &DispatchController::requestStop);
    }
    networkPanel_->setClosing(dispatch_->stoppingService());
    networkPanel_->show();
    networkPanel_->raise();
    networkPanel_->activateWindow();
}
void ApplicationController::makeMenu() {
    pause_ = menu_.addAction("暂停爬行");
    pause_->setCheckable(true);
    connect(pause_, &QAction::triggered, this, [this](bool checked) {
        primary_.paused = checked;
        refreshMenu();
    });
    affection_ = menu_.addAction("");
    affection_->setEnabled(false);
    menu_.addAction("互动面板…", this, [this] { showInteractions(contextPet_); });
    menu_.addAction("食物面板…", this, [this] { showFood(contextPet_); });
    demo_ = menu_.addAction("立即演示飞扑");
    connect(demo_, &QAction::triggered, this, [this] { primary_.demoPounce(); });
    menu_.addSeparator();
    menu_.addAction("蟑螂列表…", this, &ApplicationController::showPets);
    limitAction_ = menu_.addAction("数量上限…", this, &ApplicationController::changeLimit);
    menu_.addAction("网络面板…", this, &ApplicationController::showNetwork);
    bottomAction_ = menu_.addAction("隐藏蟑螂（置底）");
    bottomAction_->setCheckable(true);
    bottomAction_->setChecked(bottomMode_);
    connect(bottomAction_, &QAction::triggered, this, &ApplicationController::setBottomMode);
    menu_.addSeparator();
    QMenu *sizes = menu_.addMenu("大小");
    auto *sizeGroup = new QActionGroup(sizes);
    const double scales[]{.55, .75, 1};
    const QStringList labels{"小 · 接近实物", "中", "大"};
    for (int i = 0; i < 3; ++i) {
        auto *action = sizes->addAction(labels[i]);
        action->setCheckable(true);
        action->setChecked(i == 1);
        sizeGroup->addAction(action);
        const double value = scales[i];
        action->setData(value);
        connect(action, &QAction::triggered, this, [this, value] {
            if (!primary_.juvenile)
                primary_.scale = value;
        });
    }
    sizes->setObjectName("sizes");
    QMenu *speeds = menu_.addMenu("活跃程度");
    auto *speedGroup = new QActionGroup(speeds);
    const double paces[]{.65, 1, 1.5};
    const QStringList names{"安静", "自然", "活跃"};
    for (int i = 0; i < 3; ++i) {
        auto *action = speeds->addAction(names[i]);
        action->setCheckable(true);
        action->setChecked(std::abs(primary_.pace - paces[i]) < 1e-9);
        speedGroup->addAction(action);
        const double value = paces[i];
        connect(action, &QAction::triggered, this, [this, value] { primary_.pace = value; });
    }
    menu_.addAction("回到鼠标所在屏幕中央", this, &ApplicationController::recall);
    retry_ = menu_.addAction("重试保存配置与进度", this, &ApplicationController::persist);
    retry_->setVisible(false);
    menu_.addSeparator();
    menu_.addAction("退出桌宠", qApp, &QApplication::quit);
    // GNOME's D-Bus tray menu asks Qt to populate the menu without displaying
    // this QMenu. aboutToShow is therefore not evidence of a visible popup.
    connect(&menu_, &QMenu::aboutToShow, this, &ApplicationController::refreshMenu);
    tray_.setIcon(QIcon(QPixmap::fromImage(animation_.walk.front())));
    tray_.setToolTip("Pettime · 右键控制，左键拖动");
    tray_.setContextMenu(&menu_);
    connect(&menu_, &QMenu::aboutToHide, this,
            [this] { QTimer::singleShot(0, this, [this] { contextPet_ = 1; }); });
    connect(&tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick)
                    recall();
            });
    refreshMenu();
}
void ApplicationController::refreshMenu() {
    limitAction_->setText(
        QString("数量上限…（当前 %1 / 上限 %2）").arg(swarm_.totalCount()).arg(swarm_.limit()));
    pause_->setChecked(primary_.paused);
    pause_->setText(primary_.paused ? "继续爬行" : "暂停爬行");
    const int a = primary_.affection;
    const QString mood = a < 10   ? "陌生"
                         : a < 30 ? "认得你"
                         : a < 60 ? "熟悉"
                         : a < 85 ? "喜欢"
                                  : "很亲近";
    affection_->setText(QString("好感度 %1 / 100 · %2").arg(a).arg(mood));
    bottomAction_->setChecked(bottomMode_);
    demo_->setEnabled(!primary_.juvenile && !primary_.cosmeticFeeding && !primary_.petting && primary_.active());
    if (auto *sizes = menu_.findChild<QMenu *>("sizes")) {
        sizes->setEnabled(!primary_.juvenile);
        for (auto *action : sizes->actions())
            action->setChecked(std::abs(action->data().toDouble() - primary_.scale) < 1e-9);
    }
    retry_->setVisible(dirty_);
}
std::uint64_t ApplicationController::selectedPet() const {
    if (!petTable_)
        return 0;
    const auto rows = petTable_->selectionModel()->selectedRows();
    if (rows.size() != 1)
        return 0;
    auto *item = petTable_->item(rows[0].row(), 0);
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}
void ApplicationController::changeLimit() {
    bool ok = false;
    const int value = QInputDialog::getInt(
        nullptr, "数量上限", "包含主实体，范围 1～72。降低上限会移除最早生成的超额普通实体。",
        swarm_.limit(), 1, 72, 1, &ok);
    if (!ok)
        return;
    swarm_.setLimit(value);
    reconcileWindows();
    refreshPets();
    refreshMenu();
    persist();
}
QString ApplicationController::selectedVisitor() const {
    if (!petTable_)
        return {};
    const auto rows = petTable_->selectionModel()->selectedRows();
    if (rows.size() != 1)
        return {};
    const auto key = petTable_->item(rows[0].row(), 0)->data(Qt::UserRole + 1).toString();
    return key.startsWith("visitor:") ? key.mid(8) : QString{};
}
bool ApplicationController::canStartControl(quint64 id) const {
    const auto *p = swarm_.find(id);
    return p && !p->primary && !p->petting && !p->transitioning() && !p->dispatchLocked && p->active() &&
           !p->expired && !p->splitReady && !p->dispatchPaused && dispatch_->canControl(id);
}
bool ApplicationController::canFeedPet(quint64 id) const {
    const auto *p = swarm_.find(id);
    return p && p->growth() < 100 && food_.count() > 0 && !food_.pending().contains(id) &&
           dispatch_->canControl(id) && p->active() && !p->transitioning() &&
           !p->dispatchLocked && !p->dispatchPaused && !p->expired && !p->splitReady &&
           !p->mealActive && !p->cosmeticFeeding && !p->petting && !p->frontal();
}
bool ApplicationController::useFood(quint64 id) {
    if (!canFeedPet(id) || !food_.reserve(id))
        return false;
    auto *p = swarm_.find(id);
    if (!p->playFeeding()) {
        food_.cancel(id);
        return false;
    }
    feedingSerials_.insert(id, p->feedingCompletions());
    // The debit and reservation are saved alongside the entity table. Startup
    // refunds unfinished reservations; completed rewards and debits commit together.
    persist();
    refreshFood();
    refreshPets();
    return true;
}
bool ApplicationController::settleFood(quint64 removing) {
    bool changed = false;
    const auto pending = food_.pending();
    for (auto id : pending) {
        const auto *p = swarm_.find(id);
        if (p && feedingSerials_.contains(id) &&
            p->feedingCompletions() > feedingSerials_.value(id)) {
            food_.complete(id);
        } else if (!p || id == removing || !p->cosmeticFeeding || p->expired) {
            food_.cancel(id);
        } else {
            continue;
        }
        feedingSerials_.remove(id);
        changed = true;
    }
    return changed;
}
QString ApplicationController::interactionBlockReason(quint64 id, Interaction action) const {
    const auto *p = swarm_.find(id);
    if (!p)
        return "请选择自己的蟑螂，访客不可互动。";
    if (!p->active() || p->expired || p->splitReady)
        return "该蟑螂正在分裂或退场。";
    if (p->dispatchPaused)
        return "等待重连，互动暂时冻结。";
    if (!dispatch_->canControl(id) || p->dispatchLocked || p->transitioning())
        return "正在派遣交接或入离场，请稍后再试。";
    if (p->petting)
        return "正在摸摸，完成后结算好感度和食物奖励。";
    if (p->cosmeticFeeding || p->mealActive || p->eating || p->frontal() || p->dragging)
        return "正在进食、飞扑或拖动，请稍后再试。";
    if (action == Interaction::Petting) {
        if (interactionCooldown_ > 1e-9)
            return QString("摸摸冷却：还需 %1 秒（所有蟑螂共用）。")
                .arg(int(std::ceil(interactionCooldown_)));
        return {};
    }
    if (dispatch_->outgoing().contains(id) || !p->motionGroup.isEmpty())
        return "生殖仅限本机自有蟑螂，请先召回。";
    if (p->growth() < 100)
        return "成年后才能生殖；幼体双击也不会分裂。";
    if (p->reproductionRemaining() > 1e-9) {
        const int seconds = int(std::ceil(p->reproductionRemaining()));
        return QString("生殖冷却：还需 %1 分 %2 秒。").arg(seconds / 60).arg(seconds % 60);
    }
    if (swarm_.pendingCount() || swarm_.totalCount() >= swarm_.limit())
        return "数量已满或有幼体等待出生，暂不能生殖。";
    return swarm_.canReproduce(id) ? QString{} : QStringLiteral("当前状态暂不可生殖。");
}
bool ApplicationController::interact(quint64 id, Interaction action) {
    if (!interactionBlockReason(id, action).isEmpty()) {
        refreshInteractions();
        return false;
    }
    auto *p = swarm_.find(id);
    clearControlKeys();
    if (action == Interaction::Petting) {
        if (!p->startPetting())
            return false;
        pettingSerials_.insert(id, p->pettingCompletions());
        interactionCooldown_ = 10;
        lastInteractionResult_ = QString("正在摸摸 %1…").arg(p->displayName());
    } else {
        if (!swarm_.reproduce(id))
            return false;
        lastInteractionResult_ = QString("%1 生下了 1 只幼体，亲体保留，生殖冷却已开始。")
            .arg(p->displayName());
        reconcileWindows();
    }
    persist();
    refreshPets();
    refreshFood();
    refreshInteractions();
    return true;
}
bool ApplicationController::settleInteractions(quint64 removing) {
    bool changed = false;
    const auto pending = pettingSerials_;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const auto *p = swarm_.find(it.key());
        if (p && p->pettingCompletions() > it.value()) {
            const bool won = std::uniform_int_distribution<int>(1, 10)(rewardRng_) == 1;
            const int added = won ? food_.grantInteractionReward() : 0;
            lastInteractionResult_ = QString("%1：摸摸完成，好感度 +%2。%3")
                .arg(p->displayName()).arg(p->lastPettingGain())
                .arg(added ? "获得 1 粒面包屑！" : won ? "抽中了面包屑，但库存已满。" : "本次没有掉落食物。");
        } else if (!p || it.key() == removing || !p->petting || p->expired) {
            lastInteractionResult_ = "摸摸已中断，没有增加好感度或发放食物。";
        } else {
            continue;
        }
        pettingSerials_.remove(it.key());
        changed = true;
    }
    progressChanged_ = progressChanged_ || changed;
    return changed;
}
void ApplicationController::showInteractions(quint64 id) {
    clearControlKeys();
    if (!interactionDialog_) {
        interactionDialog_ = std::make_unique<QDialog>();
        interactionDialog_->setWindowTitle("互动面板");
        interactionDialog_->resize(480, 440);
        auto *layout = new QVBoxLayout(interactionDialog_.get());
        layout->addWidget(new QLabel("选择蟑螂："));
        interactionTarget_ = new QComboBox;
        interactionTarget_->setObjectName("interactionTarget");
        layout->addWidget(interactionTarget_);
        interactionDetails_ = new QLabel;
        interactionDetails_->setTextFormat(Qt::PlainText);
        interactionDetails_->setWordWrap(true);
        layout->addWidget(interactionDetails_);
        interactionAction_ = new QComboBox;
        interactionAction_->setObjectName("interactionAction");
        interactionAction_->addItem("摸摸", int(Interaction::Petting));
        interactionAction_->addItem("生殖", int(Interaction::Reproduce));
        layout->addWidget(interactionAction_);
        interactionReason_ = new QLabel;
        interactionReason_->setWordWrap(true);
        layout->addWidget(interactionReason_);
        interactionUse_ = new QPushButton;
        interactionUse_->setObjectName("performInteraction");
        layout->addWidget(interactionUse_);
        interactionResult_ = new QLabel;
        interactionResult_->setObjectName("interactionResult");
        interactionResult_->setTextFormat(Qt::PlainText);
        interactionResult_->setWordWrap(true);
        layout->addWidget(interactionResult_);
        auto *restore = new QPushButton("恢复蟑螂显示");
        layout->addWidget(restore);
        connect(restore, &QPushButton::clicked, this, [this] { setBottomMode(false); });
        connect(interactionTarget_, &QComboBox::currentIndexChanged, this,
                [this] { refreshInteractions(); });
        connect(interactionAction_, &QComboBox::currentIndexChanged, this,
                [this] { refreshInteractions(); });
        connect(interactionUse_, &QPushButton::clicked, this, [this] {
            interact(interactionTarget_->currentData().toULongLong(),
                     Interaction(interactionAction_->currentData().toInt()));
        });
    }
    refreshInteractions();
    const int index = interactionTarget_->findData(QVariant::fromValue<qulonglong>(id));
    if (index >= 0)
        interactionTarget_->setCurrentIndex(index);
    refreshInteractions();
    interactionDialog_->show();
    interactionDialog_->raise();
    interactionDialog_->activateWindow();
}
void ApplicationController::refreshInteractions() {
    if (!interactionDialog_)
        return;
    const QSignalBlocker blocker(interactionTarget_);
    const auto selected = interactionTarget_->currentData().toULongLong();
    int row = 0;
    for (const auto &p : swarm_.pets()) {
        if (p->expired || !p->active())
            continue;
        const auto text = QString("%1 · %2").arg(p->displayName(), dispatch_->location(p->id));
        if (row == interactionTarget_->count())
            interactionTarget_->addItem(text);
        else if (interactionTarget_->itemText(row) != text)
            interactionTarget_->setItemText(row, text);
        interactionTarget_->setItemData(row++, QVariant::fromValue<qulonglong>(p->id));
    }
    while (interactionTarget_->count() > row)
        interactionTarget_->removeItem(row);
    const int index = interactionTarget_->findData(QVariant::fromValue<qulonglong>(selected));
    interactionTarget_->setCurrentIndex(index >= 0 ? index : (row ? 0 : -1));
    const auto id = interactionTarget_->currentData().toULongLong();
    const auto *p = swarm_.find(id);
    interactionDetails_->setText(p ? QString("好感度 %1 / 100 · %2 · 成长 %3%\n"
        "好感度每隔随机 30～60 秒下降 1～2 点。面包屑 %4 / %5（含使用中）。")
        .arg(p->affection).arg(PetModel::growthState(p->growth()))
        .arg(std::floor(p->growth() * 10) / 10, 0, 'f', 1)
        .arg(food_.count() + food_.reservedCount()).arg(FoodInventory::Capacity) : "暂无可选择的蟑螂。");
    const auto action = Interaction(interactionAction_->currentData().toInt());
    const auto reason = interactionBlockReason(id, action);
    const QString description = action == Interaction::Petting
        ? "摸摸约 2 秒，完成后好感度 +2～4（最高 100），10% 概率获得 1 粒面包屑。所有蟑螂共用 10 秒冷却。"
        : "仅本机成体可生殖：保留亲体，新增 1 只幼体，随后独立冷却 10～20 分钟。满员不生殖。";
    interactionReason_->setText(description + "\n" + (reason.isEmpty() ? "可以开始。" : reason));
    interactionUse_->setText(action == Interaction::Petting ? "摸摸" : "生殖 · 新增 1 只幼体");
    interactionUse_->setEnabled(reason.isEmpty());
    interactionResult_->setText(lastInteractionResult_);
}
void ApplicationController::showFood(quint64 id) {
    clearControlKeys();
    if (!foodDialog_) {
        foodDialog_ = std::make_unique<QDialog>();
        foodDialog_->setWindowTitle("食物面板");
        foodDialog_->resize(420, 260);
        auto *layout = new QVBoxLayout(foodDialog_.get());
        foodStock_ = new QLabel;
        foodStock_->setObjectName("foodStock");
        layout->addWidget(foodStock_);
        auto *description = new QLabel("面包屑 · 每粒随机增加 0.5～1.5 个百分点成长。\n"
            "成长最高 100%，临近成熟时实际增量可能减少。\n"
            "运行期间每隔随机 3～5 分钟获得一粒，库存满时停止累计。\n"
            "离线及休眠不补发。只能喂自己的幼体，不增加好感度。");
        description->setWordWrap(true);
        layout->addWidget(description);
        layout->addWidget(new QLabel("选择蟑螂："));
        foodTarget_ = new QComboBox;
        foodTarget_->setObjectName("foodTarget");
        layout->addWidget(foodTarget_);
        foodReason_ = new QLabel;
        foodReason_->setWordWrap(true);
        layout->addWidget(foodReason_);
        foodUse_ = new QPushButton("使用 1 粒面包屑");
        foodUse_->setObjectName("useBreadcrumb");
        layout->addWidget(foodUse_);
        auto *restore = new QPushButton("恢复蟑螂显示");
        connect(restore, &QPushButton::clicked, this, [this] { setBottomMode(false); });
        layout->addWidget(restore);
        connect(foodTarget_, &QComboBox::currentIndexChanged, this,
                [this] { refreshFood(); });
        connect(foodUse_, &QPushButton::clicked, this,
                [this] { useFood(foodTarget_->currentData().toULongLong()); });
    }
    refreshFood();
    const int index = foodTarget_->findData(QVariant::fromValue<qulonglong>(id));
    if (index >= 0)
        foodTarget_->setCurrentIndex(index);
    refreshFood();
    foodDialog_->show();
    foodDialog_->raise();
    foodDialog_->activateWindow();
}
void ApplicationController::refreshFood() {
    if (!foodDialog_)
        return;
    const QSignalBlocker blocker(foodTarget_);
    const auto selected = foodTarget_->currentData().toULongLong();
    int row = 0;
    for (const auto &p : swarm_.pets()) {
        if (p->expired || !p->active())
            continue;
        const QString text = QString("%1 · %2% · %3").arg(p->displayName())
            .arg(std::floor(p->growth() * 10) / 10, 0, 'f', 1).arg(dispatch_->location(p->id));
        if (row == foodTarget_->count())
            foodTarget_->addItem(text);
        else if (foodTarget_->itemText(row) != text)
            foodTarget_->setItemText(row, text);
        foodTarget_->setItemData(row++, QVariant::fromValue<qulonglong>(p->id));
    }
    while (foodTarget_->count() > row)
        foodTarget_->removeItem(row);
    const int index = foodTarget_->findData(QVariant::fromValue<qulonglong>(selected));
    foodTarget_->setCurrentIndex(index >= 0 ? index : (row ? 0 : -1));
    foodStock_->setText(QString("面包屑：可用 %1 · 使用中 %2 / 上限 %3")
        .arg(food_.count()).arg(food_.reservedCount()).arg(FoodInventory::Capacity));
    const auto id = foodTarget_->currentData().toULongLong();
    const auto *p = swarm_.find(id);
    foodUse_->setEnabled(canFeedPet(id));
    foodReason_->setText(!p ? "暂无可选择的蟑螂。" : p->growth() >= 100 ? "已成年，无需增加成长。"
        : food_.pending().contains(id) ? "正在进食；完成后结算，取消会返还面包屑。"
        : food_.count() == 0 ? "面包屑不足，请等待下一次掉落。"
        : canFeedPet(id) ? "可以使用面包屑。" : "当前处于交接、冻结或其他动作中，请稍后再试。");
}
void ApplicationController::setBottomMode(bool enabled) {
    if (bottomMode_ == enabled)
        return;
    bottomMode_ = enabled;
    mainWindow_.setBottomMode(enabled);
    crack_.setBottomMode(enabled);
    for (auto &entry : windows_)
        entry.second->setBottomMode(enabled);
    for (auto &entry : visitorWindows_)
        entry.second->setBottomMode(enabled);
    persist();
}
bool ApplicationController::canDispatchPet(quint64 id) const {
    const auto *p = swarm_.find(id);
    return p && !p->primary && !p->petting && p->active() && !p->transitioning() && !p->dispatchLocked &&
           !p->dispatchPaused && !p->expired && !p->splitReady && p->motionGroup.isEmpty() &&
           !dispatch_->outgoing().contains(id) && !dispatch_->stoppingService();
}
void ApplicationController::clearControlKeys() {
    controlKeys_.clear();
    if (auto *p = swarm_.find(controlledPet_))
        p->setManualDirection({});
}
void ApplicationController::stopControl() {
    clearControlKeys();
    if (auto *p = swarm_.find(controlledPet_))
        p->setManualControl(false);
    controlledPet_ = 0;
    controlledGroup_.clear();
}
void ApplicationController::validateControl() {
    if (!controlledPet_)
        return;
    const auto *p = swarm_.find(controlledPet_);
    if (selectedPet() != controlledPet_ || !canStartControl(controlledPet_) ||
        p->motionGroup != controlledGroup_)
        stopControl();
}
bool ApplicationController::eventFilter(QObject *object, QEvent *event) {
    if (!petDialog_ || !controlledPet_)
        return QObject::eventFilter(object, event);
    if (object == petDialog_.get()) {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Close) {
            stopControl();
            return false;
        }
        if (event->type() == QEvent::WindowDeactivate)
            clearControlKeys();
    }
    // Modal editors and other windows must retain their normal keyboard behavior.
    const auto *widget = qobject_cast<QWidget *>(object);
    if (!widget || widget->window() != petDialog_.get() || !petDialog_->isActiveWindow() ||
        (QApplication::activeModalWidget() && QApplication::activeModalWidget() != petDialog_.get()))
        return false;
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease &&
        event->type() != QEvent::ShortcutOverride)
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    const int k = key->key();
    if (k != Qt::Key_W && k != Qt::Key_A && k != Qt::Key_S && k != Qt::Key_D &&
        k != Qt::Key_Up && k != Qt::Key_Left && k != Qt::Key_Down && k != Qt::Key_Right)
        return false;
    key->accept();
    if (event->type() == QEvent::ShortcutOverride || key->isAutoRepeat())
        return true;
    validateControl();
    if (!controlledPet_)
        return true;
    if (event->type() == QEvent::KeyPress)
        controlKeys_.insert(k);
    else
        controlKeys_.remove(k);
    const auto down = [this](int a, int b) { return controlKeys_.contains(a) || controlKeys_.contains(b); };
    const QPointF direction(int(down(Qt::Key_D, Qt::Key_Right)) - int(down(Qt::Key_A, Qt::Key_Left)),
                            int(down(Qt::Key_S, Qt::Key_Down)) - int(down(Qt::Key_W, Qt::Key_Up)));
    swarm_.find(controlledPet_)->setManualDirection(direction);
    return true;
}

bool ApplicationController::canClearPet(quint64 id) const {
    const auto *p = swarm_.find(id);
    return p && !p->primary && p->motionGroup.isEmpty() && !p->dispatchPaused &&
           !p->transitioning() && !p->dispatchLocked &&
           !dispatch_->outgoing().contains(id);
}
bool ApplicationController::clearPet(quint64 id) {
    if (!canClearPet(id) || !swarm_.removeLocalPet(id))
        return false;
    persist();
    refreshPets();
    refreshFood();
    return true;
}

void ApplicationController::selectPets(bool visitors) {
    if (!petTable_)
        return;
    const QSignalBlocker blocker(petTable_);
    QItemSelection selection;
    for (int row = 0; row < petTable_->rowCount(); ++row) {
        const auto *item = petTable_->item(row, 0);
        const auto id = item->data(Qt::UserRole).toULongLong();
        const auto *p = swarm_.find(id);
        const auto key = item->data(Qt::UserRole + 1).toString();
        const bool match = visitors
            ? key.startsWith("visitor:") && dispatch_->visitors().contains(key.mid(8))
            : p && !p->primary && !dispatch_->isAway(id);
        if (match)
            selection.select(petTable_->model()->index(row, 0),
                             petTable_->model()->index(row, petTable_->columnCount() - 1));
    }
    petTable_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    refreshPets();
}

void ApplicationController::showPets() {
    if (!petDialog_) {
        petDialog_ = std::make_unique<QDialog>();
        petDialog_->setWindowTitle("蟑螂列表");
        petDialog_->resize(920, 460);
        auto *layout = new QVBoxLayout(petDialog_.get());
        petCount_ = new QLabel;
        layout->addWidget(petCount_);
        petTable_ = new QTableWidget(0, 8);
        petTable_->setHorizontalHeaderLabels(
            {"编号", "名称", "归属／来源", "动作", "所在设备", "派遣状态", "成长度", "生长状态"});
        petTable_->setAutoScroll(false);
        petTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        petTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        petTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        petTable_->verticalHeader()->hide();
        petTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        petTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        layout->addWidget(petTable_);
        auto *selectionButtons = new QHBoxLayout;
        auto *selectLocal = new QPushButton("全选本机自有子体");
        selectLocal->setObjectName("selectLocalPets");
        auto *selectVisitors = new QPushButton("全选访客");
        selectVisitors->setObjectName("selectVisitorPets");
        selectionButtons->addWidget(selectLocal);
        selectionButtons->addWidget(selectVisitors);
        auto *restore = new QPushButton("恢复蟑螂显示");
        selectionButtons->addWidget(restore);
        connect(restore, &QPushButton::clicked, this, [this] { setBottomMode(false); });
        layout->addLayout(selectionButtons);
        connect(selectLocal, &QPushButton::clicked, this, [this] { selectPets(false); });
        connect(selectVisitors, &QPushButton::clicked, this, [this] { selectPets(true); });
        controlPet_ = new QPushButton("开始控制");
        controlStatus_ = new QLabel("单选自己的非主实体后开始控制；WASD／方向键移动。");
        controlStatus_->setTextFormat(Qt::PlainText);
        layout->addWidget(controlPet_);
        layout->addWidget(controlStatus_);
        connect(controlPet_, &QPushButton::clicked, this, [this] {
            if (controlledPet_) {
                stopControl();
            } else {
                const auto id = selectedPet();
                if (canStartControl(id)) {
                    auto *p = swarm_.find(id);
                    if (p->setManualControl(true)) {
                        controlledPet_ = id;
                        controlledGroup_ = p->motionGroup;
                    }
                }
            }
            refreshPets();
        });
        auto *buttons = new QHBoxLayout;
        renamePet_ = new QPushButton("重命名");
        highlightPet_ = new QPushButton("高亮定位");
        foodPet_ = new QPushButton("食物…");
        interactionPet_ = new QPushButton("互动…");
        interactionPet_->setObjectName("openInteractions");
        clearPet_ = new QPushButton("清除");
        clearPet_->setToolTip("立即清除仍在本机的自有非主实体，不触发分裂。");
        connect(clearPet_, &QPushButton::clicked, this, [this] {
            clearPet(selectedPet());
        });
        for (auto *button : {renamePet_, highlightPet_, foodPet_, interactionPet_, clearPet_})
            buttons->addWidget(button);
        layout->addLayout(buttons);
        auto *dispatchButtons = new QHBoxLayout;
        dispatchPet_ = new QPushButton("派遣…");
        recallPet_ = new QPushButton("召回");
        recallAllPets_ = new QPushButton("全部召回");
        expelPet_ = new QPushButton("驱赶回原设备");
        connect(expelPet_, &QPushButton::clicked, this, [this] {
            dispatch_->expelVisitor(selectedVisitor());
            reconcileVisitors();
            refreshPets();
        });
        for (auto *button : {dispatchPet_, recallPet_, recallAllPets_, expelPet_})
            dispatchButtons->addWidget(button);
        layout->addLayout(dispatchButtons);
        dispatchStatus_ = new QLabel;
        dispatchStatus_->setTextFormat(Qt::PlainText);
        dispatchStatus_->setWordWrap(true);
        layout->addWidget(dispatchStatus_);
        connect(dispatchPet_, &QPushButton::clicked, this,
                &ApplicationController::dispatchSelected);
        connect(recallPet_, &QPushButton::clicked, this,
                [this] { dispatch_->recallPet(selectedPet()); });
        connect(recallAllPets_, &QPushButton::clicked, dispatch_.get(),
                &DispatchController::recallAll);
        layout->addWidget(new QLabel("食物面板可消耗面包屑加速幼体成长。名称与成长会随进度保存。"));
        connect(petTable_, &QTableWidget::itemSelectionChanged, this,
                &ApplicationController::refreshPets);
        connect(renamePet_, &QPushButton::clicked, this, [this] {
            const auto id = selectedPet();
            auto *p = swarm_.find(id);
            if (!p || p->primary)
                return;
            clearControlKeys();
            bool ok = false;
            const QString name = QInputDialog::getText(petDialog_.get(), "重命名",
                                                       "名称（最多 64 字符，留空恢复编号名称）",
                                                       QLineEdit::Normal, p->customName, &ok);
            if (ok)
                swarm_.rename(id, name);
            refreshPets();
        });
        connect(highlightPet_, &QPushButton::clicked, this, [this] {
            const auto id = selectedPet();
            const auto *p = swarm_.find(id);
            if (!p || !dispatch_->canHighlight(id))
                return;
            if (dispatch_->isAway(id))
                dispatch_->highlightPet(id);
            else if (id == 1)
                mainWindow_.highlight();
            else if (auto it = windows_.find(id); it != windows_.end())
                it->second->highlight();
        });
        connect(foodPet_, &QPushButton::clicked, this, [this] {
            const auto id = selectedPet();
            if (swarm_.find(id))
                showFood(id);
        });
        connect(interactionPet_, &QPushButton::clicked, this, [this] {
            const auto id = selectedPet();
            if (swarm_.find(id))
                showInteractions(id);
        });
    }
    refreshPets();
    petTable_->clearSelection();
    petTable_->setCurrentCell(-1, -1);
    for (int row = 0; row < petTable_->rowCount(); ++row)
        if (petTable_->item(row, 0)->data(Qt::UserRole).toULongLong() == contextPet_)
            petTable_->selectRow(row);
    petDialog_->show();
    petDialog_->raise();
    petDialog_->activateWindow();
}
void ApplicationController::dispatchSelected() {
    QList<quint64> ids;
    for (const auto &index : petTable_->selectionModel()->selectedRows()) {
        const auto id = petTable_->item(index.row(), 0)->data(Qt::UserRole).toULongLong();
        if (canDispatchPet(id))
            ids.append(id);
    }
    if (ids.isEmpty())
        return;
    QStringList choices, peers;
    for (const auto &d : network_->devices()) {
        if (!network_->dispatchReady(d.id))
            continue;
        choices.append(QString("%1 — %2:%3（剩余访客名额约 %4）")
                           .arg(d.name, d.address)
                           .arg(d.port)
                           .arg(DispatchController::VisitorLimit - d.visitors));
        peers.append(d.id);
    }
    if (choices.isEmpty()) {
        warn("没有支持派遣的在线设备，请在双方网络面板中开启服务。");
        return;
    }
    bool ok = false;
    const auto choice =
        QInputDialog::getItem(petDialog_.get(), "派遣到设备", "目标设备", choices, 0, false, &ok);
    if (!ok)
        return;
    const auto text = dispatch_->dispatchPets(ids, peers.at(choices.indexOf(choice)));
    dispatchStatus_->setText(text);
    refreshPets();
}
void ApplicationController::refreshPets() {
    if (!petTable_)
        return;
    const int scrollY = petTable_->verticalScrollBar()->value();
    const int scrollX = petTable_->horizontalScrollBar()->value();
    QSet<QString> selected;
    QString current;
    for (const auto &index : petTable_->selectionModel()->selectedRows())
        selected.insert(petTable_->item(index.row(), 0)->data(Qt::UserRole + 1).toString());
    if (petTable_->currentRow() >= 0 && petTable_->item(petTable_->currentRow(), 0))
        current = petTable_->item(petTable_->currentRow(), 0)->data(Qt::UserRole + 1).toString();
    const QSignalBlocker blocker(petTable_);
    petCount_->setText(QString("自有 %1 / %2 · 派出 %3 · 访客 %4 / %5")
                           .arg(swarm_.totalCount())
                           .arg(swarm_.limit())
                           .arg(dispatch_->awayCount())
                           .arg(dispatch_->visitors().size())
                           .arg(DispatchController::VisitorLimit));
    QStringList keys;
    for (const auto &p : swarm_.pets())
        keys.append("local:" + QString::number(p->id));
    auto visitors = dispatch_->visitors().keys();
    std::sort(visitors.begin(), visitors.end());
    for (const auto &key : visitors)
        keys.append("visitor:" + key);
    QHash<QString, int> rows;
    for (int row = 0; row < petTable_->rowCount(); ++row)
        if (const auto *item = petTable_->item(row, 0))
            rows.insert(item->data(Qt::UserRole + 1).toString(), row);
    const bool samePets = rows.size() == keys.size() &&
        std::all_of(keys.cbegin(), keys.cend(), [&](const auto &key) { return rows.contains(key); });
    if (!samePets) {
        petTable_->setRowCount(keys.size());
        petTable_->clearSelection();
        petTable_->setCurrentCell(-1, -1);
    }
    int nextRow = 0;
    const QStringList states{"休息", "爬行", "疾走", "躲避", "进食", "开心",
                             "起飞", "飞行", "俯冲", "回弹", "踩扁", "退场"};
    auto add = [&](const QStringList &values, quint64 id, const QString &key) {
        const int row = samePets ? rows.value(key) : nextRow;
        ++nextRow;
        for (int col = 0; col < values.size(); ++col) {
            auto *item = petTable_->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                petTable_->setItem(row, col, item);
            }
            if (item->text() != values[col])
                item->setText(values[col]);
            const auto value = QVariant::fromValue<qulonglong>(id);
            if (item->data(Qt::UserRole) != value)
                item->setData(Qt::UserRole, value);
            if (item->data(Qt::UserRole + 1).toString() != key)
                item->setData(Qt::UserRole + 1, key);
        }
        if (!samePets && selected.contains(key))
            petTable_->selectionModel()->select(petTable_->model()->index(row, 0),
                                                QItemSelectionModel::Select |
                                                    QItemSelectionModel::Rows);
        if (!samePets && key == current)
            petTable_->selectionModel()->setCurrentIndex(petTable_->model()->index(row, 0),
                                                         QItemSelectionModel::NoUpdate);
    };
    for (const auto &p : swarm_.pets()) {
        const QString action = p->exiting() ? "离场中" : p->entering() ? "入场中"
            : p->dispatchLocked ? "交接中" : p->petting ? "摸摸中" : p->cosmeticFeeding ? "投喂动画"
                                                                      : states.at(int(p->state));
        add({QString::number(p->id), p->displayName(), p->primary ? "主实体" : "自有",
             action, dispatch_->location(p->id),
             dispatch_->stateText(p->id), QString::number(std::floor(p->growth() * 10) / 10, 'f', 1) + "%",
             PetModel::growthState(p->growth())},
            p->id, "local:" + QString::number(p->id));
    }
    for (const auto &key : visitors) {
        const auto &v = dispatch_->visitors()[key];
        add({v.entity, v.name, "访客 · " + v.sourceName,
             v.hidden ? "交接中" : v.stage == 1 ? "离场中" : states.at(int(v.current.state)), "本机",
             dispatch_->visitorStateText(v),
             QString::number(std::floor(v.current.growth * 10) / 10, 'f', 1) + "%",
             PetModel::growthState(v.current.growth)},
            0, "visitor:" + key);
    }
    validateControl();
    const auto id = selectedPet();
    const auto *p = swarm_.find(id);
    controlPet_->setEnabled(controlledPet_ || canStartControl(id));
    controlPet_->setText(controlledPet_ ? "停止控制" : "开始控制");
    controlStatus_->setText(controlledPet_
        ? QString("正在控制：%1 · WASD／方向键移动，松开停止").arg(p->displayName())
        : QStringLiteral("单选自己的非主实体后开始控制；WASD／方向键移动。"));
    clearPet_->setEnabled(canClearPet(id));
    expelPet_->setEnabled(dispatch_->visitors().contains(selectedVisitor()));
    renamePet_->setEnabled(p && !p->primary);
    highlightPet_->setEnabled(p && dispatch_->canHighlight(id));
    foodPet_->setEnabled(p != nullptr);
    interactionPet_->setEnabled(p != nullptr);
    bool eligible = !petTable_->selectionModel()->selectedRows().isEmpty();
    for (const auto &index : petTable_->selectionModel()->selectedRows()) {
        const auto n = petTable_->item(index.row(), 0)->data(Qt::UserRole).toULongLong();
        if (!canDispatchPet(n))
            eligible = false;
    }
    dispatchPet_->setEnabled(eligible);
    recallPet_->setEnabled(p && dispatch_->outgoing().contains(id));
    recallAllPets_->setEnabled(!dispatch_->outgoing().isEmpty());
    petTable_->verticalScrollBar()->setValue(scrollY);
    petTable_->horizontalScrollBar()->setValue(scrollX);
}
void ApplicationController::reconcileVisitors() {
    for (auto it = visitorWindows_.begin(); it != visitorWindows_.end();) {
        if (!dispatch_->visitors().contains(it->first) || !dispatch_->visitors()[it->first].active ||
            dispatch_->visitors()[it->first].hidden)
            it = visitorWindows_.erase(it);
        else
            ++it;
    }
    for (auto it = dispatch_->visitors().cbegin(); it != dispatch_->visitors().cend(); ++it) {
        const auto &v = it.value();
        if (!v.active || v.hidden)
            continue;
        auto &window = visitorWindows_[it.key()];
        if (!window) {
            window = std::make_unique<PetWindow>(animation_);
            window->setBottomMode(bottomMode_);
        }
        window->presentRemote(dispatch_->interpolated(v), v.name, v.frozen);
        if (!window->isVisible())
            window->show();
    }
}
void ApplicationController::start() {
    store_.log("Starting Qt " + QString::fromLatin1(qVersion()) +
               " platform=" + QGuiApplication::platformName());
    if (!options_.smoke && QSystemTrayIcon::isSystemTrayAvailable())
        tray_.show();
    else if (!options_.smoke)
        store_.log("No system tray; pet context menu and relaunch recall remain available");
    mainWindow_.present();
    mainWindow_.show();
    if (options_.population > 1)
        swarm_.spawn(primary_.position, primary_.area, 1, options_.population - 1);
    reconcileWindows();
    if (restoreNetwork_ && !network_->start())
        warn("恢复网络服务失败：" + network_->status());
    persist();
    clock_.start();
    timer_.start();
    if (!startupWarning_.isEmpty())
        QTimer::singleShot(0, this, [this] { warn(startupWarning_); });
}
void ApplicationController::recall() {
    setBottomMode(false);
    primary_.reveal(areaAt(QCursor::pos()));
    mainWindow_.present();
    mainWindow_.show();
    mainWindow_.raise();
    ++recalls_;
    refreshMenu();
    store_.log("RECALL center=" + QString::number(primary_.position.x()) + "," +
               QString::number(primary_.position.y()));
}
void ApplicationController::warn(const QString &message) {
    store_.log("WARNING " + message);
    qWarning().noquote() << message;
    if (options_.smoke)
        return;
    auto *box = new QMessageBox(QMessageBox::Warning, "Pettime", message, QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->show();
}
void ApplicationController::persist() {
    settleFood();
    settleInteractions();
    progressChanged_ = false;
    QString error;
    const QJsonObject config{{"formatVersion", 1}, {"populationLimit", swarm_.limit()},
        {"paused", primary_.paused}, {"pace", primary_.pace},
        {"networkEnabled", network_->running() && !dispatch_->stoppingService()},
        {"bottomMode", bottomMode_}};
    auto progress = swarm_.saveProgress();
    progress["food"] = food_.save();
    progress["interaction"] = QJsonObject{{"cooldownRemaining", interactionCooldown_}};
    auto pets = progress.value("pets").toArray();
    for (int index = 0; index < pets.size(); ++index) {
        auto pet = pets[index].toObject();
        const auto position = dispatch_->savedPosition(pet.value("id").toString().toULongLong());
        pet["x"] = position.x();
        pet["y"] = position.y();
        pets[index] = pet;
    }
    progress["pets"] = pets;
    // Each file is replaced atomically; progress contains its own complete entity table.
    if (store_.saveJson("progress.json", progress, &error) &&
        store_.saveJson("config.json", config, &error)) {
        dirty_ = saveWarning_ = false;
    } else {
        dirty_ = true;
        retryAt_ = clock_.isValid() ? clock_.elapsed() / 1000.0 + 30 : 30;
        if (!saveWarning_) {
            saveWarning_ = true;
            warn("配置或进度未能保存，请确认程序目录可写。当前进度仍保留在内存中，可从菜单重试。\n" + error);
        }
    }
    refreshMenu();
}
void ApplicationController::updateScreens() {
    const auto screens = QGuiApplication::screens();
    for (const auto &p : swarm_.pets()) {
        if (dispatch_->isLocalVisible(p->id)) {
            const bool existingArea = std::any_of(screens.cbegin(), screens.cend(),
                [&](const auto *screen) { return QRectF(screen->availableGeometry()) == p->area; });
            if (!p->transitioning())
                p->setArea(areaAt(p->position));
            else if (!existingArea)
                p->setArea(areaAt(p->area.center()));
        }
        if (dispatch_->outgoing().contains(p->id))
            dispatch_->updateLocalSurface(p->id, areaAt(dispatch_->savedPosition(p->id)));
    }
}
void ApplicationController::reconcileWindows() {
    std::set<std::uint64_t> alive;
    for (const auto &p : swarm_.pets())
        if (!p->expired && dispatch_->isLocalVisible(p->id))
            alive.insert(p->id);
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!alive.count(it->first))
            it = windows_.erase(it);
        else
            ++it;
    }
    for (const auto &p : swarm_.pets()) {
        if (p->primary || p->expired || !dispatch_->isLocalVisible(p->id))
            continue;
        if (!windows_.count(p->id)) {
            if (!p->transitioning())
                p->setArea(areaAt(p->position));
            auto window = std::make_unique<PetWindow>(*p, animation_);
            window->setBottomMode(bottomMode_);
            connect(window.get(), &PetWindow::contextRequested, this,
                    [this, id = p->id](QPoint point) {
                        contextPet_ = id;
                        menu_.popup(point);
                    });
            connect(window.get(), &PetWindow::exitRequested, qApp, &QApplication::quit);
            window->present();
            window->show();
            windows_.emplace(p->id, std::move(window));
        }
    }
}
void ApplicationController::tick() {
    QElapsedTimer work;
    work.start();
    const double now = clock_.nsecsElapsed() / 1e9;
    const double elapsed = std::max(0.0, now - last_);
    last_ = now;
    // Do not credit suspension or a long stopped event loop as active play time.
    const double dt = elapsed <= 1 ? elapsed : 0;
    interactionCooldown_ = std::max(0.0, interactionCooldown_ - dt);
    if (elapsed > 1)
        lastSwarm_ = now;
    dispatch_->tick();
    validateControl();
    if (now >= nextVisitors_) {
        reconcileVisitors();
        const int visibleCount = int(dispatch_->visitors().size()) + swarm_.totalCount() -
                                 dispatch_->awayCount();
        nextVisitors_ = now + (visibleCount >= 48 ? .066 : visibleCount >= 24 ? .05 : .033);
    }
    primary_.menuOpen = menu_.isVisible();
    primary_.advance(dt, options_.smoke ? QPointF(-100000, -100000) : QPointF(QCursor::pos()));
    if (primary_.impact) {
        crack_.trigger(primary_.position);
        ++impacts_;
    }
    crack_.advance(dt);
    if (swarm_.splitPrimary())
        progressChanged_ = true;
    if (now >= nextSwarm_) {
        swarm_.advance(now - lastSwarm_);
        lastSwarm_ = now;
        const double interval = swarm_.intervalMs() / 1000.0;
        nextSwarm_ += interval;
        if (nextSwarm_ <= now)
            nextSwarm_ = now + interval;
        ++swarmUpdates_;
        reconcileWindows();
        for (auto &pair : windows_)
            pair.second->present();
    }
    mainWindow_.present();
    const bool foodChanged = settleFood();
    const bool interactionChanged = settleInteractions();
    const bool dropped = food_.advance(dt);
    if (foodChanged || dropped || progressChanged_ || (dirty_ && now >= retryAt_) || (!dirty_ && now >= nextSave_)) {
        persist();
        nextSave_ = now + 5;
    }
    if (ticks_ % 15 == 0 || foodChanged || dropped || interactionChanged) {
        if (petDialog_ && petDialog_->isVisible())
            refreshPets();
        if (foodDialog_ && foodDialog_->isVisible())
            refreshFood();
        if (interactionDialog_ && interactionDialog_->isVisible())
            refreshInteractions();
    }
    ++ticks_;
    const double workMs = work.nsecsElapsed() / 1e6;
    totalTickMs_ += workMs;
    maxTickMs_ = std::max(maxTickMs_, workMs);
    if (options_.smoke) {
        if (!smokePounce_ && now >= .5) {
            smokePounce_ = true;
            primary_.demoPounce();
        }
        if (now >= options_.duration)
            finishSmoke();
    }
}
void ApplicationController::finishSmoke() {
    timer_.stop();
    QDir dir(options_.output);
    bool ok = dir.mkpath(".");
    const QImage image = mainWindow_.image();
    ok = ok && !image.isNull() && qAlpha(image.pixel(0, 0)) == 0 && primary_.traveled > 0 &&
         mainWindow_.paintedFrames() > 0 && swarm_.totalCount() == options_.population;
    for (const auto &entry : windows_)
        ok = ok && entry.second->isVisible() && entry.second->paintedFrames() > 0;
    const bool imageSaved = image.save(dir.filePath("live-buffer.png"));
    ok = ok && imageSaved;
    const QJsonObject report{{"passed", ok},
                             {"platform", QGuiApplication::platformName()},
                             {"qt", QString::fromLatin1(qVersion())},
                             {"elapsed_seconds", clock_.elapsed() / 1000.0},
                             {"ticks", ticks_},
                             {"swarm_updates", swarmUpdates_},
                             {"painted_frames", mainWindow_.paintedFrames()},
                             {"travel_pixels", primary_.traveled},
                             {"population", swarm_.totalCount()},
                             {"mean_tick_ms", ticks_ ? totalTickMs_ / ticks_ : 0},
                             {"max_tick_ms", maxTickMs_},
                             {"impacts", impacts_},
                             {"recalls", recalls_},
                             {"corner_alpha", qAlpha(image.pixel(0, 0))}};
    QSaveFile file(dir.filePath("smoke.json"));
    const QByteArray json = QJsonDocument(report).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
        ok = false;
    qInfo().noquote() << json;
    store_.log(ok ? "Smoke passed" : "Smoke FAILED");
    QCoreApplication::exit(ok ? 0 : 1);
}
} // namespace pettime
