#include "ApplicationController.h"
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QHeaderView>
#include <QInputDialog>
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
        if (hasProgress)
            swarm_.restoreProgress(store_.loadJson("progress.json", &warning));
        else
            primary_.affection = legacy.loadAffinity(&warning);
    }
    appendWarning(warning);
    primary_.paused = config.value("paused").toBool(false);
    const double pace = config.value("pace").toDouble(1.5);
    primary_.pace = pace == .65 || pace == 1 || pace == 1.5 ? pace : 1.5;
    restoreNetwork_ = !options_.smoke && config.value("networkEnabled").toBool(false);
    swarm_.beforeRemove = [this](std::uint64_t id) {
        if (controlledPet_ == id)
            stopControl();
        if (dispatch_)
            dispatch_->removeEntity(id);
        windows_.erase(id);
    };
    network_ = std::make_unique<NetworkService>(store_, [this] {
        return NetworkService::Info{swarm_.totalCount(), swarm_.limit(),
                                    dispatch_ ? int(dispatch_->outgoing().size()) : 0,
                                    dispatch_ ? int(dispatch_->visitors().size()) : 0};
    });
    dispatch_ = std::make_unique<DispatchController>(*network_, swarm_, [] {
        auto *screen = QGuiApplication::primaryScreen();
        return screen ? QRectF(screen->availableGeometry()) : QRectF{};
    });
    connect(dispatch_.get(), &DispatchController::changed, this, [this] {
        validateControl();
        // Defer reconciliation: a removal callback may still be traversing the swarm.
        QTimer::singleShot(0, this, [this] {
            reconcileWindows();
            reconcileVisitors();
            refreshPets();
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
    if (!networkPanel_)
        networkPanel_ = std::make_unique<NetworkPanel>(*network_);
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
    feed_ = menu_.addAction("投喂一粒面包屑");
    connect(feed_, &QAction::triggered, this, [this] { primary_.feed(); });
    demo_ = menu_.addAction("立即演示飞扑");
    connect(demo_, &QAction::triggered, this, [this] { primary_.demoPounce(); });
    menu_.addSeparator();
    menu_.addAction("蟑螂列表…", this, &ApplicationController::showPets);
    limitAction_ = menu_.addAction("数量上限…", this, &ApplicationController::changeLimit);
    menu_.addAction("网络面板…", this, &ApplicationController::showNetwork);
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
    feed_->setEnabled(!primary_.mealActive && !primary_.cosmeticFeeding && primary_.active());
    demo_->setEnabled(!primary_.juvenile && !primary_.cosmeticFeeding && primary_.active());
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
    return p && !p->primary && !p->entering() && p->active() && !p->expired && !p->splitReady &&
           !p->dispatchPaused && dispatch_->canControl(id);
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
           !dispatch_->outgoing().contains(id);
}
bool ApplicationController::clearPet(quint64 id) {
    if (!canClearPet(id) || !swarm_.removeLocalPet(id))
        return false;
    refreshPets();
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
        feedPet_ = new QPushButton("投喂");
        clearPet_ = new QPushButton("清除");
        clearPet_->setToolTip("立即清除仍在本机的自有非主实体，不触发分裂。");
        connect(clearPet_, &QPushButton::clicked, this, [this] {
            clearPet(selectedPet());
        });
        for (auto *button : {renamePet_, highlightPet_, feedPet_, clearPet_})
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
        layout->addWidget(new QLabel("此处投喂完成后仅为所选实体随机增加 1～5 点成长，不增加好感度。名称仅在本次运行保留。"));
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
            if (dispatch_->isAway(id))
                dispatch_->highlightPet(id);
            else if (id == 1)
                mainWindow_.highlight();
            else if (auto it = windows_.find(id); it != windows_.end())
                it->second->highlight();
        });
        connect(feedPet_, &QPushButton::clicked, this, [this] {
            if (dispatch_->canControl(selectedPet()))
                if (auto *p = swarm_.find(selectedPet()))
                    p->playFeeding();
            refreshPets();
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
        if (id)
            ids.append(id);
    }
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
                           .arg(dispatch_->outgoing().size())
                           .arg(dispatch_->visitors().size())
                           .arg(DispatchController::VisitorLimit));
    petTable_->setRowCount(swarm_.totalCount() + dispatch_->visitors().size());
    petTable_->clearSelection();
    petTable_->setCurrentCell(-1, -1);
    int row = 0;
    const QStringList states{"休息", "爬行", "疾走", "躲避", "进食", "开心",
                             "起飞", "飞行", "俯冲", "回弹", "踩扁", "退场"};
    auto add = [&](const QStringList &values, quint64 id, const QString &key) {
        for (int col = 0; col < values.size(); ++col) {
            auto *item = petTable_->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                petTable_->setItem(row, col, item);
            }
            item->setText(values[col]);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setData(Qt::UserRole + 1, key);
        }
        if (selected.contains(key))
            petTable_->selectionModel()->select(petTable_->model()->index(row, 0),
                                                QItemSelectionModel::Select |
                                                    QItemSelectionModel::Rows);
        if (key == current)
            petTable_->selectionModel()->setCurrentIndex(petTable_->model()->index(row, 0),
                                                         QItemSelectionModel::NoUpdate);
        ++row;
    };
    for (const auto &p : swarm_.pets())
        add({QString::number(p->id), p->displayName(), p->primary ? "主实体" : "自有",
             p->entering() ? "入场中" : p->cosmeticFeeding ? "投喂动画" : states.at(int(p->state)), dispatch_->location(p->id),
             dispatch_->stateText(p->id), QString::number(std::floor(p->growth() * 10) / 10, 'f', 1) + "%",
             PetModel::growthState(p->growth())},
            p->id, "local:" + QString::number(p->id));
    auto keys = dispatch_->visitors().keys();
    std::sort(keys.begin(), keys.end());
    for (const auto &key : keys) {
        const auto &v = dispatch_->visitors()[key];
        add({v.entity, v.name, "访客 · " + v.sourceName, states.at(int(v.current.state)), "本机",
             !v.active  ? "预留中"
             : v.frozen ? "等待重连"
                        : "只读访客",
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
    highlightPet_->setEnabled(p && dispatch_->canControl(id));
    feedPet_->setEnabled(p && dispatch_->canControl(id) && p->active() && !p->entering() && !p->mealActive &&
                         !p->cosmeticFeeding && !p->frontal());
    bool eligible = !selected.isEmpty();
    for (const auto &index : petTable_->selectionModel()->selectedRows()) {
        const auto n = petTable_->item(index.row(), 0)->data(Qt::UserRole).toULongLong();
        const auto *candidate = swarm_.find(n);
        if (!candidate || candidate->primary || !candidate->active() ||
            dispatch_->outgoing().contains(n))
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
        if (!dispatch_->visitors().contains(it->first) || !dispatch_->visitors()[it->first].active)
            it = visitorWindows_.erase(it);
        else
            ++it;
    }
    for (auto it = dispatch_->visitors().cbegin(); it != dispatch_->visitors().cend(); ++it) {
        const auto &v = it.value();
        if (!v.active)
            continue;
        auto &window = visitorWindows_[it.key()];
        if (!window)
            window = std::make_unique<PetWindow>(animation_);
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
    QString error;
    const QJsonObject config{{"formatVersion", 1}, {"populationLimit", swarm_.limit()},
        {"paused", primary_.paused}, {"pace", primary_.pace},
        {"networkEnabled", network_->running()}};
    // Each file is replaced atomically; progress contains its own complete entity table.
    if (store_.saveJson("progress.json", swarm_.saveProgress(), &error) &&
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
    primary_.setArea(areaAt(primary_.position));
    for (const auto &p : swarm_.pets())
        if (!dispatch_->isAway(p->id))
            p->setArea(areaAt(p->position));
}
void ApplicationController::reconcileWindows() {
    std::set<std::uint64_t> alive;
    for (const auto &p : swarm_.pets())
        if (!dispatch_->isAway(p->id))
            alive.insert(p->id);
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!alive.count(it->first))
            it = windows_.erase(it);
        else
            ++it;
    }
    for (const auto &p : swarm_.pets()) {
        if (p->primary || dispatch_->isAway(p->id))
            continue;
        if (!windows_.count(p->id)) {
            p->setArea(areaAt(p->position));
            auto window = std::make_unique<PetWindow>(*p, animation_);
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
    const double dt = elapsed;
    dispatch_->tick();
    validateControl();
    if (now >= nextVisitors_) {
        reconcileVisitors();
        const int visibleCount = int(dispatch_->visitors().size()) + swarm_.totalCount() -
                                 int(dispatch_->outgoing().size());
        nextVisitors_ = now + (visibleCount >= 48 ? .066 : visibleCount >= 24 ? .05 : .033);
    }
    const int before = primary_.affection;
    primary_.menuOpen = menu_.isVisible();
    primary_.advance(dt, options_.smoke ? QPointF(-100000, -100000) : QPointF(QCursor::pos()));
    if (primary_.impact) {
        crack_.trigger(primary_.position);
        ++impacts_;
    }
    crack_.advance(dt);
    if (primary_.splitReady) {
        const int count = primary_.juvenile ? 8 + int(primary_.random(0, 5)) : 10;
        primary_.becomeNymph();
        swarm_.spawn(primary_.position, primary_.area, primary_.generation, count - 1);
    }
    if (before != primary_.affection) {
        dirty_ = true;
        persist();
    }
    if ((dirty_ && now >= retryAt_) || (!dirty_ && now >= nextSave_)) {
        persist();
        nextSave_ = now + 5;
    }
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
    if (ticks_ % 15 == 0)
        refreshPets();
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
