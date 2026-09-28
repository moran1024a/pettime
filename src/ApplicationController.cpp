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
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScreen>
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
    primary_.affection = store_.loadAffinity(&startupWarning_);
    QString limitWarning;
    swarm_.setLimit(options_.smoke ? 72 : store_.loadLimit(&limitWarning));
    if (!limitWarning.isEmpty())
        startupWarning_ += "\n" + limitWarning;
    swarm_.beforeRemove = [this](std::uint64_t id) { windows_.erase(id); };
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
    timer_.stop();
    tray_.hide();
    if (dirty_)
        persist();
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
        action->setChecked(i == 2);
        speedGroup->addAction(action);
        const double value = paces[i];
        connect(action, &QAction::triggered, this, [this, value] { primary_.pace = value; });
    }
    menu_.addAction("回到鼠标所在屏幕中央", this, &ApplicationController::recall);
    retry_ = menu_.addAction("重试保存好感度", this, &ApplicationController::persist);
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
    if (auto *sizes = menu_.findChild<QMenu *>("sizes"))
        sizes->setEnabled(!primary_.juvenile);
    retry_->setVisible(dirty_);
}
std::uint64_t ApplicationController::selectedPet() const {
    if (!petTable_ || petTable_->currentRow() < 0)
        return 0;
    auto *item = petTable_->item(petTable_->currentRow(), 0);
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
    QString error;
    if (!store_.saveLimit(value, &error))
        warn("数量上限已在本次运行生效，但未能保存。请通过数量上限菜单重试。\n" + error);
}
void ApplicationController::showPets() {
    if (!petDialog_) {
        petDialog_ = std::make_unique<QDialog>();
        petDialog_->setWindowTitle("蟑螂列表");
        petDialog_->resize(580, 420);
        auto *layout = new QVBoxLayout(petDialog_.get());
        petCount_ = new QLabel;
        layout->addWidget(petCount_);
        petTable_ = new QTableWidget(0, 4);
        petTable_->setHorizontalHeaderLabels({"编号", "名称", "身份", "状态"});
        petTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        petTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        petTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        petTable_->verticalHeader()->hide();
        petTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        petTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        layout->addWidget(petTable_);
        auto *buttons = new QHBoxLayout;
        renamePet_ = new QPushButton("重命名");
        highlightPet_ = new QPushButton("高亮定位");
        feedPet_ = new QPushButton("播放投喂动画");
        for (auto *button : {renamePet_, highlightPet_, feedPet_})
            buttons->addWidget(button);
        layout->addLayout(buttons);
        layout->addWidget(new QLabel("此处投喂仅播放动画，不增加好感度。名称仅在本次运行保留。"));
        connect(petTable_, &QTableWidget::itemSelectionChanged, this,
                &ApplicationController::refreshPets);
        connect(renamePet_, &QPushButton::clicked, this, [this] {
            const auto id = selectedPet();
            auto *p = swarm_.find(id);
            if (!p || p->primary)
                return;
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
            if (id == 1)
                mainWindow_.highlight();
            else if (auto it = windows_.find(id); it != windows_.end())
                it->second->highlight();
        });
        connect(feedPet_, &QPushButton::clicked, this, [this] {
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
void ApplicationController::refreshPets() {
    if (!petTable_)
        return;
    const auto selected = selectedPet();
    const QSignalBlocker blocker(petTable_);
    petCount_->setText(
        QString("当前 %1 只 / 上限 %2").arg(swarm_.totalCount()).arg(swarm_.limit()));
    petTable_->setRowCount(swarm_.totalCount());
    int row = 0, selectedRow = -1;
    const QStringList states{"休息", "爬行", "疾走", "躲避", "进食", "开心",
                             "起飞", "飞行", "俯冲", "回弹", "踩扁", "退场"};
    for (const auto &p : swarm_.pets()) {
        const QStringList values{
            QString::number(p->id), p->displayName(), p->primary ? "主实体" : "普通",
            p->cosmeticFeeding ? "投喂动画" : states.at(static_cast<int>(p->state))};
        for (int col = 0; col < 4; ++col) {
            auto *item = petTable_->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                petTable_->setItem(row, col, item);
            }
            item->setText(values[col]);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(p->id));
        }
        if (p->id == selected)
            selectedRow = row;
        ++row;
    }
    if (selectedRow >= 0)
        petTable_->selectRow(selectedRow);
    else {
        petTable_->clearSelection();
        petTable_->setCurrentCell(-1, -1);
    }
    const auto *p = swarm_.find(selected);
    renamePet_->setEnabled(p && !p->primary);
    highlightPet_->setEnabled(p != nullptr);
    feedPet_->setEnabled(p && p->active() && !p->mealActive && !p->cosmeticFeeding &&
                         !p->frontal());
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
    if (store_.saveAffinity(primary_.affection, &error)) {
        dirty_ = saveWarning_ = false;
    } else {
        dirty_ = true;
        retryAt_ = clock_.isValid() ? clock_.elapsed() / 1000.0 + 30 : 30;
        if (!saveWarning_) {
            saveWarning_ = true;
            warn("好感度未能保存，当前进度仍保留在内存中。可从菜单重试。\n" + error);
        }
    }
    refreshMenu();
}
void ApplicationController::updateScreens() {
    primary_.setArea(areaAt(primary_.position));
    for (const auto &p : swarm_.pets())
        p->setArea(areaAt(p->position));
}
void ApplicationController::reconcileWindows() {
    std::set<std::uint64_t> alive;
    for (const auto &p : swarm_.pets())
        alive.insert(p->id);
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (!alive.count(it->first))
            it = windows_.erase(it);
        else
            ++it;
    }
    for (const auto &p : swarm_.pets()) {
        if (p->primary)
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
    if (dirty_ && now >= retryAt_)
        persist();
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
