#include "ApplicationController.h"
#include "DispatchController.h"
#include "PetWindow.h"
#include <QTemporaryDir>
#include <QScreen>
#include <QScrollBar>
#include <QPointer>
#include <QUuid>
#include <QtTest>

namespace pettime {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
quint16 freePort() {
    QTcpServer server;
    return server.listen(QHostAddress::LocalHost, 0) ? server.serverPort() : 0;
}
const QString peer = "10000000-0000-4000-8000-000000000001";
const QString session = "20000000-0000-4000-8000-000000000002";
struct Fixture {
    QTemporaryDir dir;
    qint64 time = 100000;
    SwarmController swarm;
    NetworkService network{SettingsStore(dir.path()), [this] {
                               return NetworkService::Info{swarm.totalCount(), swarm.limit()};
                           }};
    DispatchController controller{network, swarm, [] { return QRectF(0, 0, 1280, 720); },
                                  [this] { return time; }};
    Fixture() { swarm.spawn({500, 500}, {0, 0, 1280, 720}, 1, 2); }
};
} // namespace
class DispatchTest : public QObject {
    Q_OBJECT
    static DispatchController::Outgoing outgoing(Fixture &f, quint64 entity = 2) {
        DispatchController::Outgoing o;
        o.entity = entity;
        o.id = uuid();
        o.stream = uuid();
        o.peer = peer;
        o.session = session;
        o.phase = DispatchController::Phase::Active;
        o.localHidden = true;
        o.home = {400, 400};
        o.homeArea = {0, 0, 1280, 720};
        o.remoteArea = o.homeArea;
        f.controller.outgoing_[entity] = o;
        f.swarm.find(entity)->motionGroup = peer;
        return o;
    }
    static void health(Fixture &f) { f.controller.health_[peer].session = session; }
    static DispatchController::Visitor visitor(Fixture &f) {
        DispatchController::Visitor v;
        v.id = uuid();
        v.peer = peer;
        v.session = session;
        v.entity = "2";
        v.stream = uuid();
        v.active = true;
        v.created = v.lastState = f.time;
        v.current = PetRenderState::from(*f.swarm.find(2));
        v.previous = v.current;
        f.controller.visitors_[v.id] = v;
        return v;
    }
    static QLabel *highlightLabel(const QString &name) {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *label = qobject_cast<QLabel *>(widget); label && label->text() == name)
                return label;
        return nullptr;
    }
  private slots:
    void portableApplicationMigrationAutosaveAndRestart() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QVERIFY(store.saveAffinity(41));
        QVERIFY(store.saveLimit(9));
        AnimationLibrary animation;
        double savedGrowth = 0;
        {
            ApplicationController app(animation, store, RunOptions{});
            QCOMPARE(app.primary_.affection, 41);
            QCOMPARE(app.swarm_.limit(), 9);
            app.start();
            app.timer_.stop();
            QVERIFY(store.hasFile("config.json"));
            QVERIFY(store.hasFile("progress.json"));
            app.swarm_.spawn({300, 300}, app.primary_.area, 2, 2);
            auto *p = app.swarm_.find(2);
            p->customName = "保存测试";
            p->advance(120, {-10000, -10000});
            savedGrowth = p->growth();
            p->motionGroup = "remote";
            p->dispatchPaused = true;
            QVERIFY(app.swarm_.removeLocalPet(3));
            app.primary_.paused = true;
            app.primary_.pace = .65;
            app.primary_.scale = 1;
            app.nextSave_ = 0;
            app.tick();
            QCOMPARE(store.loadJson("progress.json").value("count").toInt(), 2);
            QCOMPARE(store.loadJson("config.json").value("pace").toDouble(), .65);
            // The destructor must save even when affection did not change.
            p->customName = "退出时保存";
        }
        {
            ApplicationController restored(animation, store, RunOptions{});
            QCOMPARE(restored.swarm_.totalCount(), 2);
            QCOMPARE(restored.swarm_.limit(), 9);
            QCOMPARE(restored.primary_.affection, 41);
            QCOMPARE(restored.primary_.scale, 1.0);
            QCOMPARE(restored.primary_.pace, .65);
            QVERIFY(restored.primary_.paused);
            auto *p = restored.swarm_.find(2);
            QVERIFY(p);
            QCOMPARE(p->customName, QString("退出时保存"));
            QCOMPARE(p->growth(), savedGrowth);
            QVERIFY(p->motionGroup.isEmpty());
            QVERIFY(!p->dispatchPaused);
            QVERIFY(restored.dispatch_->visitors().isEmpty());
            QVERIFY(restored.dispatch_->outgoing().isEmpty());
            QVERIFY(!restored.network_->running());
        }
        QFile old(dir.filePath("affinity.dat"));
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(old.readAll(), QByteArray("41\n"));
    }

    void clearLocalPetNeverSplitsOrReusesId() {
        Fixture f;
        auto *p = f.swarm.find(2);
        p->crush();
        p->advance(1.1, {});
        QVERIFY(p->splitReady);
        int removals = 0;
        f.swarm.beforeRemove = [&](quint64 id) {
            QCOMPARE(id, quint64(2));
            QVERIFY(f.swarm.find(id)); // Cleanup precedes destruction.
            ++removals;
        };
        QVERIFY(!f.swarm.removeLocalPet(1));
        QVERIFY(!f.swarm.removeLocalPet(0));
        QVERIFY(f.swarm.removeLocalPet(2));
        QVERIFY(!f.swarm.removeLocalPet(2));
        QCOMPARE(removals, 1);
        f.swarm.advance(2);
        QCOMPARE(f.swarm.totalCount(), 2);
        QCOMPARE(f.swarm.pendingCount(), std::size_t(0));
        f.swarm.spawn({500, 500}, {0, 0, 1280, 720}, 1, 1);
        QVERIFY(f.swarm.find(4));
        QVERIFY(!f.swarm.find(2));
    }
    void clearRejectsPrimaryVisitorsAndEveryDispatchPhase() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        QVERIFY(!app.clearPet_->isEnabled());
        QVERIFY(!app.clearPet(1));
        app.petTable_->selectRow(1);
        QVERIFY(app.clearPet_->isEnabled());
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        for (auto phase : {DispatchController::Phase::Offering,
                           DispatchController::Phase::LeavingLocal,
                           DispatchController::Phase::Activating,
                           DispatchController::Phase::Active,
                           DispatchController::Phase::LeavingRemote,
                           DispatchController::Phase::AwaitingHidden}) {
            o.phase = phase;
            o.localHidden = phase >= DispatchController::Phase::Activating;
            app.dispatch_->outgoing_.insert(2, o);
            // Recheck at action time, even before the enabled button refreshes.
            app.clearPet_->click();
            QVERIFY(app.swarm_.find(2));
            QVERIFY(!app.clearPet(2));
            app.refreshPets();
            QVERIFY(!app.clearPet_->isEnabled());
        }
        app.dispatch_->outgoing_.clear();
        auto *p = app.swarm_.find(2);
        p->beginEntry(p->area, 0, .5);
        app.refreshPets();
        QVERIFY(!app.clearPet(2));
        QVERIFY(!app.clearPet_->isEnabled());
        QVERIFY(!app.controlPet_->isEnabled());
        QVERIFY(!app.canFeedPet(app.selectedPet()));
        QVERIFY(app.highlightPet_->isEnabled());
        QVERIFY(!app.dispatchPet_->isEnabled());
        p->cancelTransition();
        p->moveTo(p->area.center(), p->area);
        p->dispatchLocked = true;
        app.refreshPets();
        QVERIFY(!app.clearPet(2));
        QVERIFY(!app.clearPet_->isEnabled());
        QVERIFY(!app.controlPet_->isEnabled());
        QVERIFY(!app.canFeedPet(app.selectedPet()));
        QVERIFY(app.highlightPet_->isEnabled());
        QVERIFY(!app.dispatchPet_->isEnabled());
        p->dispatchLocked = false;
        DispatchController::Visitor v;
        v.id = uuid();
        v.entity = "2";
        v.active = true;
        app.dispatch_->visitors_.insert(v.id, v);
        app.refreshPets();
        app.petTable_->selectRow(2);
        QVERIFY(!app.clearPet_->isEnabled());
        QVERIFY(!app.clearPet(app.selectedPet()));
        QVERIFY(app.swarm_.find(2));
        QVERIFY(app.dispatch_->visitors_.contains(v.id));
        app.petTable_->selectRow(1);
        app.petTable_->selectionModel()->select(app.petTable_->model()->index(2, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        app.refreshPets();
        QVERIFY(!app.clearPet_->isEnabled());
        app.petTable_->clearSelection();
        QVERIFY(!app.clearPet_->isEnabled());
    }
    void clearStopsControlFeedingAndHighlightWithoutScrolling() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 30;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        app.petTable_->selectRow(1);
        auto *p = app.swarm_.find(2);
        p->customName = "clear-test-highlight";
        app.controlPet_->click();
        app.foodPet_->click();
        app.foodUse_->click();
        app.highlightPet_->click();
        QVERIFY(p->manuallyControlled());
        QVERIFY(p->cosmeticFeeding);
        QPointer<PetWindow> window = app.windows_.at(2).get();
        QPointer<QLabel> highlight;
        for (auto *w : QApplication::topLevelWidgets())
            if (auto *label = qobject_cast<QLabel *>(w))
                if (label->text() == p->customName)
                    highlight = label;
        QVERIFY(highlight);
        QTest::qWait(30);
        auto *scroll = app.petTable_->verticalScrollBar();
        QVERIFY(scroll->maximum() > 0);
        scroll->setValue(scroll->maximum() / 2);
        const int before = scroll->value();
        app.clearPet_->click();
        QVERIFY(!app.swarm_.find(2));
        QVERIFY(window.isNull());
        QVERIFY(highlight.isNull());
        QCOMPARE(app.controlledPet_, quint64(0));
        QVERIFY(app.controlKeys_.isEmpty());
        QCOMPARE(app.swarm_.totalCount(), 29);
        QCOMPARE(app.petTable_->rowCount(), 29);
        QCOMPARE(app.swarm_.limit(), 72);
        QVERIFY(app.petTable_->selectionModel()->selectedRows().isEmpty());
        QVERIFY(!app.clearPet_->isEnabled());
        QVERIFY(!app.clearPet(2));
        QTest::qWait(30);
        QCOMPARE(scroll->value(), before);
        QCOMPARE(app.swarm_.pendingCount(), std::size_t(0));
        QCOMPARE(app.primary_.id, quint64(1));
    }
    void feedingTargetsSelectedRowRatherThanCurrentRow() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        QCOMPARE(app.petTable_->currentRow(), 0);
        app.selectPets(false);
        QCOMPARE(app.selectedPet(), quint64(2));
        app.foodPet_->click();
        app.foodUse_->click();
        auto *child = app.swarm_.find(2);
        QVERIFY(child->cosmeticFeeding);
        QVERIFY(!app.primary_.cosmeticFeeding);
        child->advance(2.65, {});
        QVERIFY(child->growth() >= .5);
        QCOMPARE(app.primary_.growth(), 100.0);
        app.primary_.becomeNymph();
        const auto childGrowth = child->growth();
        app.petTable_->selectRow(0);
        app.foodPet_->click();
        app.foodUse_->click();
        QVERIFY(app.primary_.cosmeticFeeding);
        app.primary_.advance(2.65, {});
        QVERIFY(app.primary_.growth() >= .5);
        QCOMPARE(child->growth(), childGrowth);
        QCOMPARE(app.primary_.affection, 0);
        app.primary_.advance(app.primary_.growthDuration(), {});
        app.refreshMenu();
        QVERIFY(app.demo_->isEnabled());
        const auto *sizes = app.menu_.findChild<QMenu *>("sizes");
        QVERIFY(sizes && sizes->isEnabled());
        for (const auto *action : sizes->actions())
            QCOMPARE(action->isChecked(), action->data().toDouble() == AdultScale);
    }
    void bulkSelectionKeepsViewportAndOwnership() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 30;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        DispatchController::Outgoing away;
        away.entity = 2;
        away.phase = DispatchController::Phase::Active;
        away.localHidden = true;
        app.dispatch_->outgoing_.insert(2, away);
        for (int i = 0; i < 3; ++i) {
            DispatchController::Visitor v;
            v.id = uuid();
            v.entity = QString::number(i + 2);
            v.active = true;
            app.dispatch_->visitors_.insert(v.id, v);
        }
        app.showPets();
        QTest::qWait(30);
        auto *bar = app.petTable_->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum() / 2);
        const int before = bar->value();
        auto *local = app.petDialog_->findChild<QPushButton *>("selectLocalPets");
        auto *visitors = app.petDialog_->findChild<QPushButton *>("selectVisitorPets");
        QVERIFY(local && visitors);
        local->click();
        auto rows = app.petTable_->selectionModel()->selectedRows();
        QCOMPARE(rows.size(), 28);
        for (const auto &row : rows) {
            const auto id = app.petTable_->item(row.row(), 0)->data(Qt::UserRole).toULongLong();
            QVERIFY(id > 2);
        }
        QCOMPARE(bar->value(), before);
        visitors->click();
        QCOMPARE(app.petTable_->selectionModel()->selectedRows().size(), 3);
        QVERIFY(!app.controlPet_->isEnabled());
        QVERIFY(!app.dispatchPet_->isEnabled());
        app.refreshPets();
        QTest::qWait(30);
        QCOMPARE(bar->value(), before);
        app.petTable_->setCurrentCell(0, 0);
        app.refreshPets();
        QTest::qWait(30);
        QCOMPARE(bar->value(), before);
        app.dispatch_->visitors_.clear();
        app.refreshPets();
        visitors->click();
        QVERIFY(app.petTable_->selectionModel()->selectedRows().isEmpty());
    }
    void visitorWindowClipsAtEntryEdge() {
        AnimationLibrary animation;
        PetWindow window(animation);
        const auto area = QGuiApplication::primaryScreen()->availableGeometry();
        PetRenderState state;
        state.position = {double(area.left()), double(area.center().y())};
        window.presentRemote(state, "entry", false);
        window.show();
        QTest::qWait(30);
        window.presentRemote(state, "entry", false);
        const auto visible = window.mask().translated(window.pos());
        QVERIFY(!visible.isEmpty());
        QVERIFY(QRegion(area).contains(visible.boundingRect()));
        QCOMPARE(window.pos().x(), area.left() - window.width() / 2);
    }
    void refreshChangesCellsWithoutTouchingSelection() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 30;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        QTest::qWait(30);
        app.petTable_->selectRow(1);
        app.petTable_->selectionModel()->select(app.petTable_->model()->index(3, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        auto *selection = app.petTable_->selectionModel();
        const auto current = selection->currentIndex();
        QSignalSpy selected(selection, &QItemSelectionModel::selectionChanged);
        QSignalSpy changedCurrent(selection, &QItemSelectionModel::currentChanged);
        QSignalSpy inserted(app.petTable_->model(), &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(app.petTable_->model(), &QAbstractItemModel::rowsRemoved);
        auto *nameCell = app.petTable_->item(1, 1);
        auto *bar = app.petTable_->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum() / 2);
        const int scroll = bar->value();
        app.swarm_.find(2)->customName = "只更新名称";
        app.refreshPets();
        QCOMPARE(app.petTable_->item(1, 1), nameCell);
        QCOMPARE(nameCell->text(), QString("只更新名称"));
        QCOMPARE(selection->currentIndex(), current);
        QCOMPARE(selection->selectedRows().size(), 2);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changedCurrent.count(), 0);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(bar->value(), scroll);
        app.petDialog_->hide();
        app.swarm_.find(2)->customName = "再次显示即刷新";
        app.nextSave_ = app.nextSwarm_ = app.nextVisitors_ = 1000;
        app.ticks_ = 0;
        app.tick();
        QCOMPARE(nameCell->text(), QString("只更新名称"));
        app.contextPet_ = 2;
        app.showPets();
        QCOMPARE(app.petTable_->item(1, 1)->text(), QString("再次显示即刷新"));
    }
    void localHandoffSynchronouslyHidesWindowAndHighlight() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        auto *p = app.swarm_.find(2);
        p->customName = "local-handoff-highlight";
        QPointer<PetWindow> window = app.windows_.at(2).get();
        QVERIFY(window->isVisible());
        window->highlight();
        QPointer<QLabel> highlight = highlightLabel(p->displayName());
        QVERIFY(highlight);
        QVERIFY(highlight->isVisible());
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        o.stream = uuid();
        o.peer = peer;
        o.session = session;
        o.home = p->position;
        o.homeArea = p->area;
        o.remoteArea = {5000, 2000, 1280, 720};
        o.phase = DispatchController::Phase::LeavingLocal;
        app.dispatch_->outgoing_.insert(2, o);
        p->dispatchLocked = true;
        QVERIFY(p->beginExit());
        app.swarm_.advance(10);
        QVERIFY(p->exitComplete());
        QVERIFY(window->isVisible());
        QVERIFY(highlight->isVisible());
        app.dispatch_->activate(2);
        // No event processing or reconciliation may be required for the hide.
        QVERIFY(window);
        QVERIFY(highlight);
        QVERIFY(!window->isVisible());
        QVERIFY(!highlight->isVisible());
        QCOMPARE(app.windows_.at(2).get(), window.data());
        QVERIFY(app.dispatch_->outgoing()[2].localHidden);
        QCOMPARE(app.dispatch_->outgoing()[2].phase, DispatchController::Phase::Activating);
        QVERIFY(app.dispatch_->isAway(2));
        QVERIFY(p->entering());
        QVERIFY(!p->area.contains(p->position));
        app.reconcileWindows();
        QVERIFY(window.isNull());
        QVERIFY(highlight.isNull());
    }
    void localTransitionHighlightClipsAndFrozenDispatchDisablesIt() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        app.petTable_->selectRow(1);
        auto *p = app.swarm_.find(2);
        p->customName = "transition-clip-highlight";
        const QRectF area = p->area;
        p->beginEntry(area, 0, .5);
        const auto outside = p->position;
        app.windows_.erase(2);
        app.updateScreens();
        QCOMPARE(p->area, area);
        QCOMPARE(p->position, outside);
        app.reconcileWindows();
        QCOMPARE(p->area, area);
        QCOMPARE(p->position, outside);
        auto *window = app.windows_.at(2).get();
        QVERIFY(window->mask().intersected(QRegion(window->rect())).isEmpty());
        p->advance(.6, {});
        QVERIFY(p->entering());
        window->present();
        app.refreshPets();
        QVERIFY(app.highlightPet_->isEnabled());
        app.highlightPet_->click();
        auto *highlight = highlightLabel(p->displayName());
        QVERIFY(highlight);
        QVERIFY(highlight->isVisible());
        const auto visibleHighlight = highlight->mask().translated(highlight->pos());
        QVERIFY(!visibleHighlight.isEmpty());
        QVERIFY(QRegion(area.toAlignedRect()).contains(visibleHighlight.boundingRect()));
        const auto visiblePet = window->mask().translated(window->pos());
        QVERIFY(!visiblePet.isEmpty());
        QVERIFY(QRegion(area.toAlignedRect()).contains(visiblePet.boundingRect()));
        QSignalSpy crushed(window, &PetWindow::crushed);
        QTest::mouseDClick(window, Qt::LeftButton);
        QCOMPARE(crushed.count(), 0);
        QVERIFY(p->entering());
        DispatchController::Outgoing o;
        o.entity = 2;
        o.peer = peer;
        o.home = area.center();
        o.homeArea = area;
        o.phase = DispatchController::Phase::LeavingLocal;
        app.dispatch_->outgoing_.insert(2, o);
        app.dispatch_->freeze(app.dispatch_->outgoing_[2]);
        app.refreshPets();
        QVERIFY(!app.highlightPet_->isEnabled());
        QVERIFY(!app.dispatch_->canHighlight(2));
    }
    void visitorExitSynchronouslyHidesWindowAndHighlight() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        qint64 time = 100000;
        ApplicationController app(animation, SettingsStore(dir.path()), RunOptions{});
        app.start();
        app.timer_.stop();
        app.dispatch_->now_ = [&] { return time; };
        app.dispatch_->health_[peer].session = session;
        DispatchController::Visitor v;
        v.id = uuid();
        v.stream = uuid();
        v.peer = peer;
        v.session = session;
        v.entity = "2";
        v.name = "visitor-exit-highlight";
        v.active = true;
        v.lastState = v.created = time;
        v.previous = v.current = PetRenderState::from(app.primary_);
        app.dispatch_->visitors_.insert(v.id, v);
        app.reconcileVisitors();
        QPointer<PetWindow> window = app.visitorWindows_.at(v.id).get();
        window->highlight();
        QPointer<QLabel> highlight = highlightLabel(v.name);
        QVERIFY(highlight);
        QVERIFY(window->isVisible());
        QVERIFY(highlight->isVisible());
        auto finalState = v.current;
        finalState.position = {app.primary_.area.left() - PetWindowExtent * .5 - 2,
                               app.primary_.area.center().y()};
        app.dispatch_->receive(peer, session, {{"op", "finishExit"}, {"id", v.id},
            {"stream", v.stream}, {"sequence", "1"}, {"state", finalState.encode()}});
        time += 99;
        app.dispatch_->tick();
        QVERIFY(window->isVisible());
        QVERIFY(highlight->isVisible());
        time++;
        app.dispatch_->tick();
        QVERIFY(window);
        QVERIFY(highlight);
        QVERIFY(!window->isVisible());
        QVERIFY(!highlight->isVisible());
        QVERIFY(app.dispatch_->visitors().isEmpty());
        QCOMPARE(app.visitorWindows_.at(v.id).get(), window.data());
        app.reconcileVisitors();
        QVERIFY(window.isNull());
        QVERIFY(highlight.isNull());
        app.reconcileVisitors();
        QVERIFY(app.visitorWindows_.empty());
    }
    void autosaveUsesLocalPositionDuringRemoteAndEdgeTransitions() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, store, options);
        app.start();
        app.timer_.stop();
        auto *p = app.swarm_.find(2);
        const QRectF localArea = p->area;
        const QPointF home = localArea.center();
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        o.stream = uuid();
        o.peer = peer;
        o.session = session;
        o.phase = DispatchController::Phase::Active;
        o.localHidden = true;
        o.home = home;
        o.homeArea = localArea;
        o.remoteArea = {8000, 4000, 1280, 720};
        app.dispatch_->outgoing_.insert(2, o);
        p->motionGroup = peer;
        p->moveTo(o.remoteArea.center(), o.remoteArea);
        const auto savedPet = [&] {
            const auto progress = store.loadJson("progress.json");
            for (const auto &value : progress.value("pets").toArray())
                if (value.toObject().value("id").toString() == "2")
                    return value.toObject();
            return QJsonObject{};
        };
        const auto save = [&] {
            app.nextSave_ = 0;
            app.nextSwarm_ = app.nextVisitors_ = 1000;
            app.tick();
        };
        const auto remote = p->position;
        save();
        QVERIFY(!savedPet().isEmpty());
        QCOMPARE(QPointF(savedPet()["x"].toDouble(), savedPet()["y"].toDouble()), home);
        QCOMPARE(p->position, remote);
        QCOMPARE(app.dispatch_->savedPosition(2), home);
        QVERIFY(p->beginExit());
        app.swarm_.advance(10);
        QVERIFY(!p->area.contains(p->position));
        app.dispatch_->outgoing_[2].phase = DispatchController::Phase::LeavingRemote;
        const auto outsideRemote = p->position;
        save();
        QCOMPARE(QPointF(savedPet()["x"].toDouble(), savedPet()["y"].toDouble()), home);
        QCOMPARE(p->position, outsideRemote);
        app.dispatch_->finish(2);
        QVERIFY(p->entering());
        QVERIFY(!localArea.contains(p->position));
        const auto outsideLocal = p->position;
        save();
        const QPointF saved(savedPet()["x"].toDouble(), savedPet()["y"].toDouble());
        QVERIFY(localArea.contains(saved));
        QVERIFY(saved != outsideLocal);
        QCOMPARE(p->position, outsideLocal);
        QCOMPARE(store.loadJson("progress.json").value("count").toInt(), 2);
    }
    void edgeEntryAndFreeze() {
        const QRectF area(-500, 20, 900, 600);
        for (int edge = 0; edge < 4; ++edge) {
            Fixture f;
            auto *p = f.swarm.find(2);
        p->beginEntry(area, edge, .5);
            const auto start = p->position;
            QVERIFY(p->entering());
            QVERIFY(!area.contains(start));
            const double outside = PetWindowExtent * .5 + 2;
            const QPointF expected[] = {{area.left() - outside, area.center().y()},
                                         {area.right() + outside, area.center().y()},
                                         {area.center().x(), area.top() - outside},
                                         {area.center().x(), area.bottom() + outside}};
            QCOMPARE(start, expected[edge]);
            QVERIFY(!p->setManualControl(true));
            QVERIFY(!p->playFeeding());
            p->dispatchPaused = true;
            f.swarm.advance(10);
            QCOMPARE(p->position, start);
            QVERIFY(p->entering());
            p->setArea(area); // Resume must not clamp an entry to the interior.
            QCOMPARE(p->position, start);
            p->dispatchPaused = false;
            for (int i = 0; i < 10; ++i)
                f.swarm.advance(.1);
            QVERIFY(!p->entering());
            QVERIFY(area.adjusted(35, 35, -35, -35).contains(p->position));
            QVERIFY(QLineF(start, p->position).length() > 90);
            QVERIFY(p->setManualControl(true));
            QVERIFY(p->playFeeding());
        }
    }
    void offlineExpulsionAfterReturnRequestRejectsLateActivationAndWrongOwner() {
        Fixture f;
        health(f);
        auto v = visitor(f);
        f.controller.visitors_[v.id].returnRequested = true;
        f.controller.visitors_[v.id].frozen = true;
        QVERIFY(f.controller.expelVisitor(v.id));
        QVERIFY(!f.controller.expelVisitor(v.id));
        QVERIFY(f.controller.visitors_.isEmpty());
        QVERIFY(f.controller.ended_[peer].contains(v.id));
        f.controller.receive(peer, session, {{"op", "activate"}, {"id", v.id},
            {"stream", v.stream}, {"name", "late"}, {"state", v.current.encode()}});
        QVERIFY(f.controller.visitors_.isEmpty());
        auto o = outgoing(f);
        auto *p = f.swarm.find(2);
        p->beginEntry({0, 0, 1280, 720}, 0, .5);
        f.controller.receive(peer, uuid(), {{"op", "gone"}, {"id", o.id}});
        QVERIFY(f.controller.outgoing_.contains(2));
        f.controller.receive(peer, session, {{"op", "gone"}, {"id", o.id}});
        QVERIFY(f.controller.outgoing_.isEmpty());
        QVERIFY(p->entering());
        QVERIFY(!o.homeArea.contains(p->position));
        const auto start = p->position;
        f.controller.receive(peer, session, {{"op", "gone"}, {"id", o.id}});
        QCOMPARE(p->position, start);
    }
    void manualMovementAndLifecycle() {
        PetModel p(7, {500, 400}, {0, 0, 1280, 720}, true);
        QVERIFY(!p.setManualControl(true)); // Primary is never controllable.
        p.primary = false;
        QVERIFY(p.setManualControl(true));
        const auto start = p.position;
        p.setManualDirection({1, 0});
        p.advance(.1, {});
        QVERIFY(std::abs(p.position.x() - start.x() - 14) < .001);
        QCOMPARE(p.position.y(), start.y());
        const auto diagonal = p.position;
        p.setManualDirection({1, -1});
        p.advance(.1, {});
        QVERIFY(std::abs(QLineF(diagonal, p.position).length() - 14) < .001);
        QVERIFY(p.heading < 0);
        p.setManualDirection({});
        const auto stopped = p.position;
        p.advance(.1, {});
        QCOMPARE(p.position, stopped);
        QCOMPARE(p.state, State::Rest);
        p.setManualDirection({1, 0});
        QVERIFY(p.playFeeding());
        p.advance(2.7, {});
        QCOMPARE(p.position, stopped);
        p.advance(.1, {});
        QVERIFY(p.position.x() > stopped.x());
        QCOMPARE(p.affection, 0);
        p.setManualDirection({-1, -1});
        for (int i = 0; i < 100; ++i)
            p.advance(.1, {});
        QVERIFY(p.area.contains(p.position));
        p.advance(60, {});
        QVERIFY(p.scale > MinimumScale);
        p.retire();
        p.advance(.5, {});
        QVERIFY(p.expired);
        QVERIFY(!p.setManualControl(true));
    }
    void manualPanelKeysAndCleanup() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 3;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        QTRY_VERIFY(app.petDialog_->isActiveWindow());
        app.petTable_->selectRow(0);
        QVERIFY(!app.controlPet_->isEnabled());
        app.petTable_->selectRow(1);
        app.controlPet_->click();
        QCOMPARE(app.controlledPet_, quint64(2));
        auto *p = app.swarm_.find(2);
        p->moveTo({400, 400}, {0, 0, 1280, 720});
        QTest::keyPress(app.petTable_, Qt::Key_Right);
        QTest::keyPress(app.petTable_, Qt::Key_D);
        QTest::keyRelease(app.petTable_, Qt::Key_Right);
        p->advance(.1, {});
        QVERIFY(p->position.x() > 400); // Aliases tracked independently.
        QCOMPARE(app.selectedPet(), quint64(2));
        app.refreshPets();
        QCOMPARE(app.controlledPet_, quint64(2));
        QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_D, Qt::NoModifier, "d", true);
        QApplication::sendEvent(app.petTable_, &repeatRelease);
        const auto beforeRepeat = p->position;
        p->advance(.1, {});
        QVERIFY(p->position.x() > beforeRepeat.x());
        QTest::keyPress(app.petTable_, Qt::Key_A);
        const auto stopped = p->position;
        p->advance(.1, {});
        QCOMPARE(p->position, stopped); // Opposite directions cancel.
        QTest::keyRelease(app.petTable_, Qt::Key_A);
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(app.petDialog_.get(), &deactivate);
        p->advance(.1, {});
        QCOMPARE(p->position, stopped);
        QVERIFY(app.controlKeys_.isEmpty());
        QDialog editor(app.petDialog_.get());
        editor.setModal(true);
        editor.show();
        QTRY_VERIFY(editor.isActiveWindow());
        QTest::keyPress(&editor, Qt::Key_D);
        p->advance(.1, {});
        QCOMPARE(p->position, stopped);
        editor.hide();
        app.petDialog_->activateWindow();
        QTRY_VERIFY(app.petDialog_->isActiveWindow());
        QTest::keyPress(app.petTable_, Qt::Key_Left);
        p->advance(.1, {});
        QVERIFY(p->position.x() < stopped.x());
        QTest::keyRelease(app.petTable_, Qt::Key_Left);
        app.petTable_->selectRow(2);
        QCOMPARE(app.controlledPet_, quint64(0));
        QVERIFY(!p->manuallyControlled());
        app.controlPet_->click();
        app.petDialog_->hide();
        QCOMPARE(app.controlledPet_, quint64(0));
        app.showPets();
        app.petTable_->selectRow(1);
        app.controlPet_->click();
        QVERIFY(app.controlledPet_);
        app.swarm_.setLimit(1);
        QCOMPARE(app.controlledPet_, quint64(0));
    }
    void manualDispatchTransitionsStopInput() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        app.petTable_->selectRow(1);
        auto *p = app.swarm_.find(2);
        DispatchController::Outgoing o;
        o.entity = 2;
        o.peer = peer;
        o.phase = DispatchController::Phase::Active;
        o.localHidden = true;
        app.dispatch_->outgoing_.insert(2, o);
        p->motionGroup = peer;
        app.refreshPets();
        app.controlPet_->click();
        QVERIFY(p->manuallyControlled());
        p->setManualDirection({1, 0});
        app.dispatch_->freeze(app.dispatch_->outgoing_[2]);
        app.validateControl();
        QCOMPARE(app.controlledPet_, quint64(0));
        QVERIFY(!p->manuallyControlled());
        QCOMPARE(app.dispatch_->outgoing_[2].phase, DispatchController::Phase::Active);
        app.dispatch_->outgoing_[2].frozenAt = -1;
        p->dispatchPaused = false;
        p->dispatchLocked = false;
        app.refreshPets();
        app.controlPet_->click();
        QVERIFY(p->manuallyControlled());
        app.dispatch_->recallPet(2);
        QCOMPARE(app.controlledPet_, quint64(0));
        QVERIFY(!p->manuallyControlled());
    }
    void renderSnapshotAndValidation() {
        AnimationLibrary animation;
        PetModel model(3, {200, 200}, {0, 0, 1280, 720});
        for (int st = 0; st <= int(State::Fade); ++st) {
            model.state = State(st);
            model.crashQueued = true;
            QCOMPARE(animation.render(model), animation.render(PetRenderState::from(model)));
        }
        model.primary = false;
        model.becomeNymph();
        model.state = State::Probe;
        QVERIFY(model.playFeeding());
        const auto snap = PetRenderState::from(model);
        QCOMPARE(snap.state, State::Food);
        PetRenderState decoded;
        QVERIFY(PetRenderState::decode(snap.encode(), decoded));
        QCOMPARE(animation.render(snap), animation.render(decoded));
        auto bad = snap.encode();
        bad[2] = 99;
        QVERIFY(!PetRenderState::decode(bad, decoded));
        bad = snap.encode();
        bad[7] = -1;
        QVERIFY(!PetRenderState::decode(bad, decoded));
        bad = snap.encode();
        bad[10] = 0;
        QVERIFY(!PetRenderState::decode(bad, decoded));
        QCOMPARE(decoded.growth, snap.growth);
        bad = snap.encode();
        bad[16] = 101;
        QVERIFY(!PetRenderState::decode(bad, decoded));
        bad[16] = -1;
        QVERIFY(!PetRenderState::decode(bad, decoded));
        bad[16] = 100; // Juvenile flag cannot disagree with maturity.
        QVERIFY(!PetRenderState::decode(bad, decoded));
        bad = snap.encode();
        bad.removeLast();
        QVERIFY(!PetRenderState::decode(bad, decoded));
    }
    void visitorWindowIsReadOnly() {
        AnimationLibrary animation;
        PetWindow window(animation);
        window.presentRemote({}, "host #2", false);
        QVERIFY(window.windowFlags().testFlag(Qt::WindowTransparentForInput));
        QSignalSpy context(&window, &PetWindow::contextRequested),
            exit(&window, &PetWindow::exitRequested);
        QTest::mouseClick(&window, Qt::RightButton);
        window.close();
        QCOMPARE(context.count(), 0);
        QCOMPARE(exit.count(), 0);
        QVERIFY(!window.image().isNull());
    }
    void freezeGraceAndReturn() {
        Fixture f;
        outgoing(f);
        f.controller.lost(peer);
        const auto age = f.swarm.find(2)->age;
        f.swarm.advance(10);
        QCOMPARE(f.swarm.find(2)->age, age);
        f.time += 29999;
        f.controller.tick();
        QVERIFY(f.controller.isAway(2));
        f.controller.lost(peer);
        f.time += 1;
        f.controller.tick();
        QVERIFY(!f.controller.isAway(2));
        QVERIFY(!f.swarm.find(2)->dispatchPaused);
        QVERIFY(f.swarm.find(2)->entering());
        QVERIFY(!f.swarm.find(2)->area.contains(f.swarm.find(2)->position));
        QVERIFY(f.swarm.find(2)->position != QPointF(400, 400));
        QCOMPARE(f.swarm.primary().id, quint64(1));
    }
    void finishExitKeepsTerminalStateAndHidesAfterInterpolation() {
        QStringList order;
        Fixture f;
        health(f);
        const auto v = visitor(f);
        f.controller.visitors_[v.id].sequence = 8;
        auto finalState = v.current;
        finalState.position = {-PetWindowExtent * .5 - 2, 400};
        f.controller.hideVisitor = [&, visitorId = v.id, finalPosition = finalState.position](const QString &id) {
            QCOMPARE(id, visitorId);
            QVERIFY(f.controller.visitors().contains(id));
            QVERIFY(f.controller.visitors()[id].hidden);
            QVERIFY(f.controller.visitors()[id].finishing);
            QCOMPARE(f.controller.visitors()[id].current.position, finalPosition);
            order.append("hide");
        };
        connect(&f.controller, &DispatchController::changed, &f.controller, [&, id = v.id] {
            if (!f.controller.visitors().contains(id))
                order.append("removed");
        });
        const auto finish = [&](const QString &stream, const QString &sequence) {
            f.controller.receive(peer, session, {{"op", "finishExit"}, {"id", v.id},
                {"stream", stream}, {"sequence", sequence}, {"state", finalState.encode()}});
        };
        finish(uuid(), "9");
        QVERIFY(!f.controller.visitors()[v.id].finishing);
        finish(v.stream, "8");
        QVERIFY(!f.controller.visitors()[v.id].finishing);
        finish(v.stream, "9");
        QVERIFY(f.controller.visitors()[v.id].finishing);
        QCOMPARE(f.controller.visitors()[v.id].sequence, quint64(9));
        QCOMPARE(f.controller.interpolated(f.controller.visitors()[v.id]).position, v.current.position);
        auto lateState = v.current;
        lateState.position = {900, 600};
        f.controller.receive(peer, session, {{"op", "states"},
            {"items", QJsonArray{QJsonArray{v.id, v.stream, "10", lateState.encode(), "late", 0}}}});
        finish(v.stream, "11");
        QCOMPARE(f.controller.visitors()[v.id].sequence, quint64(9));
        QCOMPARE(f.controller.visitors()[v.id].current.position, finalState.position);
        f.time += 50;
        QCOMPARE(f.controller.interpolated(f.controller.visitors()[v.id]).position,
                 (v.current.position + finalState.position) * .5);
        f.time += 49;
        f.controller.tick();
        QVERIFY(f.controller.visitors().contains(v.id));
        QVERIFY(order.isEmpty());
        f.time++;
        f.controller.tick();
        QVERIFY(f.controller.visitors().isEmpty());
        QCOMPARE(order, QStringList({"hide", "removed"}));
        QVERIFY(f.controller.ended_[peer].contains(v.id));
        f.controller.receive(peer, session, {{"op", "states"},
            {"items", QJsonArray{QJsonArray{v.id, v.stream, "12", lateState.encode(), "late", 0}}}});
        QVERIFY(f.controller.visitors().isEmpty());
        QCOMPARE(order, QStringList({"hide", "removed"}));
    }
    void hiddenAcknowledgementArrivesOnlyAfterSynchronousVisitorHide() {
        qint64 time = 100000;
        QStringList order;
        int hiddenAcknowledgements = 0;
        QTemporaryDir dir;
        SwarmController ownerSwarm, guestSwarm;
        ownerSwarm.spawn({500, 500}, {0, 0, 1280, 720}, 1, 1);
        NetworkService::Options ownerOptions, guestOptions;
        ownerOptions.loopbackOnly = guestOptions.loopbackOnly = true;
        ownerOptions.port = freePort();
        do {
            guestOptions.port = freePort();
        } while (ownerOptions.port == guestOptions.port && guestOptions.port);
        QVERIFY(ownerOptions.port);
        QVERIFY(guestOptions.port);
        ownerOptions.discoveryTargets = {{QHostAddress::LocalHost, guestOptions.port}};
        guestOptions.discoveryTargets = {{QHostAddress::LocalHost, ownerOptions.port}};
        ownerOptions.now = guestOptions.now = [&] { return time; };
        NetworkService ownerNetwork(SettingsStore(dir.path() + "/owner"),
            [&] { return NetworkService::Info{ownerSwarm.totalCount(), ownerSwarm.limit()}; }, ownerOptions);
        NetworkService guestNetwork(SettingsStore(dir.path() + "/guest"),
            [&] { return NetworkService::Info{guestSwarm.totalCount(), guestSwarm.limit()}; }, guestOptions);
        DispatchController owner(ownerNetwork, ownerSwarm, [] { return QRectF(0, 0, 1280, 720); },
                                 [&] { return time; });
        DispatchController guest(guestNetwork, guestSwarm, [] { return QRectF(0, 0, 1280, 720); },
                                 [&] { return time; });
        QSignalSpy ownerMessages(&ownerNetwork, &NetworkService::dispatchMessage);
        QSignalSpy guestMessages(&guestNetwork, &NetworkService::dispatchMessage);
        QVERIFY(ownerNetwork.start());
        QVERIFY(guestNetwork.start());
        QTRY_VERIFY(ownerNetwork.dispatchReady(guestNetwork.deviceId()));
        QTRY_VERIFY(guestNetwork.dispatchReady(ownerNetwork.deviceId()));
        const auto inventoryReceived = [](const QSignalSpy &messages) {
            for (const auto &arguments : messages)
                if (qvariant_cast<QJsonObject>(arguments.at(2))["op"] == "inventory")
                    return true;
            return false;
        };
        // Initial empty inventories must drain before inserting a synthetic dispatch.
        QTRY_VERIFY(inventoryReceived(ownerMessages));
        QTRY_VERIFY(inventoryReceived(guestMessages));
        auto *p = ownerSwarm.find(2);
        const auto visibleState = PetRenderState::from(*p);
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        o.stream = uuid();
        o.peer = guestNetwork.deviceId();
        o.session = guestNetwork.sessionId();
        o.home = p->position;
        o.homeArea = o.remoteArea = p->area;
        o.phase = DispatchController::Phase::AwaitingHidden;
        o.localHidden = true;
        o.sequence = 9;
        owner.outgoing_.insert(2, o);
        p->motionGroup = o.peer;
        QVERIFY(p->beginExit());
        ownerSwarm.advance(10);
        QVERIFY(p->exitComplete());
        p->dispatchPaused = p->dispatchLocked = true;
        DispatchController::Visitor v;
        v.id = o.id;
        v.stream = o.stream;
        v.peer = ownerNetwork.deviceId();
        v.session = ownerNetwork.sessionId();
        v.entity = "2";
        v.active = true;
        v.sequence = 8;
        v.created = v.lastState = time;
        v.previous = v.current = visibleState;
        guest.visitors_.insert(v.id, v);
        guest.hideVisitor = [&, id = v.id](const QString &hiddenId) {
            QCOMPARE(hiddenId, id);
            QVERIFY(guest.visitors().contains(id));
            QVERIFY(guest.visitors()[id].hidden);
            QVERIFY(owner.isAway(2));
            order.append("hide");
        };
        connect(&ownerNetwork, &NetworkService::dispatchMessage, &owner,
            [&, id = o.id, stream = o.stream](const QString &, const QString &, const QJsonObject &message) {
                if (message["op"] != "hidden")
                    return;
                QCOMPARE(message["id"].toString(), id);
                QCOMPARE(message["stream"].toString(), stream);
                QCOMPARE(message["sequence"].toString(), QString("9"));
                QCOMPARE(order, QStringList({"hide"}));
                QVERIFY(!guest.visitors().contains(id));
                QVERIFY(owner.isLocalVisible(2));
                QVERIFY(p->entering());
                order.append("hidden");
                ++hiddenAcknowledgements;
            });
        QVERIFY(ownerNetwork.sendDispatch(o.peer, {{"op", "finishExit"}, {"id", o.id},
            {"stream", o.stream}, {"sequence", "9"}, {"state", PetRenderState::from(*p).encode()}}));
        QTRY_VERIFY(guest.visitors()[v.id].finishing);
        time += 99;
        guest.tick();
        QTest::qWait(20);
        QVERIFY(guest.visitors().contains(v.id));
        QVERIFY(owner.isAway(2));
        QCOMPARE(hiddenAcknowledgements, 0);
        time++;
        guest.tick();
        QTRY_COMPARE(hiddenAcknowledgements, 1);
        QCOMPARE(order, QStringList({"hide", "hidden"}));
        QVERIFY(!p->area.contains(p->position));
        QVERIFY(owner.outgoing().isEmpty());
    }
    void hiddenConfirmationRequiresSessionStreamAndSequence() {
        Fixture f;
        health(f);
        const auto o = outgoing(f);
        auto *p = f.swarm.find(2);
        QVERIFY(p->beginExit());
        f.swarm.advance(10);
        QVERIFY(p->exitComplete());
        p->dispatchPaused = p->dispatchLocked = true;
        f.controller.outgoing_[2].phase = DispatchController::Phase::AwaitingHidden;
        f.controller.outgoing_[2].sequence = 9;
        const auto hidden = [&](const QString &sessionId, const QString &stream, const QString &sequence) {
            f.controller.receive(peer, sessionId, {{"op", "hidden"}, {"id", o.id},
                {"stream", stream}, {"sequence", sequence}});
        };
        hidden(uuid(), o.stream, "9");
        hidden(session, uuid(), "9");
        hidden(session, o.stream, "8");
        QVERIFY(f.controller.isAway(2));
        QVERIFY(p->exiting());
        hidden(session, o.stream, "9");
        QVERIFY(!f.controller.isAway(2));
        QVERIFY(p->entering());
        QVERIFY(!o.homeArea.contains(p->position));
        QVERIFY(p->motionGroup.isEmpty());
        QVERIFY(!p->dispatchPaused);
        QVERIFY(!p->dispatchLocked);
        f.swarm.advance(.1);
        const auto returned = p->position;
        hidden(session, o.stream, "9");
        QCOMPARE(p->position, returned);
    }
    void frozenReturnPreservesEveryTransitionPhaseAndGraceDeadline() {
        for (const auto phase : {DispatchController::Phase::LeavingLocal,
                                DispatchController::Phase::Activating,
                                DispatchController::Phase::Active,
                                DispatchController::Phase::LeavingRemote,
                                DispatchController::Phase::AwaitingHidden}) {
            Fixture f;
            outgoing(f);
            f.controller.outgoing_[2].phase = phase;
            f.controller.outgoing_[2].localHidden = phase != DispatchController::Phase::LeavingLocal;
            f.controller.lost(peer);
            QCOMPARE(f.controller.outgoing()[2].phase, phase);
            QCOMPARE(f.controller.outgoing()[2].frozenAt, qint64(100000));
            f.time += 1000;
            f.controller.lost(peer);
            QCOMPARE(f.controller.outgoing()[2].phase, phase);
            QCOMPARE(f.controller.outgoing()[2].frozenAt, qint64(100000));
            QVERIFY(f.swarm.find(2)->dispatchPaused);
            QVERIFY(f.swarm.find(2)->dispatchLocked);
        }
    }
    void expandedRemoteSurfaceResumesExitBeforeHiddenConfirmation() {
        Fixture f;
        health(f);
        const auto o = outgoing(f);
        auto *p = f.swarm.find(2);
        p->moveTo({350, 200}, {0, 0, 400, 400});
        QVERIFY(p->beginExit());
        f.swarm.advance(10);
        QVERIFY(p->exitComplete());
        QCOMPARE(p->position, QPointF(514, 200));
        f.controller.outgoing_[2].phase = DispatchController::Phase::AwaitingHidden;
        f.controller.lost(peer);
        f.controller.resume(f.controller.outgoing_[2]);
        const auto stream = f.controller.outgoing()[2].stream;
        QVERIFY(stream != o.stream);
        f.controller.receive(peer, session, {{"op", "resumeAck"}, {"id", o.id},
            {"stream", stream}, {"area", QJsonArray{0, 0, 1000, 400}}});
        QCOMPARE(f.controller.outgoing()[2].phase, DispatchController::Phase::LeavingRemote);
        QVERIFY(f.controller.outgoing()[2].awaitingActive);
        QVERIFY(!p->exitComplete());
        QCOMPARE(p->position, QPointF(514, 200));
        QVERIFY(p->area.contains(p->position));
        f.controller.receive(peer, session, {{"op", "active"}, {"id", o.id}, {"stream", stream}});
        QCOMPARE(f.controller.outgoing()[2].phase, DispatchController::Phase::LeavingRemote);
        QVERIFY(!p->dispatchPaused);
        QVERIFY(p->dispatchLocked);
        QVERIFY(!p->exitComplete());
        const auto resumed = p->position;
        f.swarm.advance(.1);
        QVERIFY(p->position != resumed);
        f.time += 100;
        f.controller.tick();
        QCOMPARE(f.controller.outgoing()[2].phase, DispatchController::Phase::LeavingRemote);
        QVERIFY(f.controller.isAway(2));
    }
    void remoteRetirementNeverCreatesLocalWindowBeforeSwarmRemoval() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 2;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.swarm_.setLimit(2);
        auto *p = app.swarm_.find(2);
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        o.peer = peer;
        o.phase = DispatchController::Phase::Active;
        o.localHidden = true;
        o.home = p->position;
        o.homeArea = p->area;
        app.dispatch_->outgoing_.insert(2, o);
        p->motionGroup = peer;
        app.reconcileWindows();
        QVERIFY(!app.windows_.count(2));
        app.swarm_.spawn(app.primary_.position, app.primary_.area, 2, 1);
        QCOMPARE(p->state, State::Fade);
        QVERIFY(!p->expired);
        QCOMPARE(app.swarm_.pendingCount(), std::size_t(1));
        app.dispatch_->tick();
        QCOMPARE(app.swarm_.find(2), p);
        QVERIFY(p->expired);
        QVERIFY(app.dispatch_->outgoing().isEmpty());
        app.reconcileWindows();
        QVERIFY(!app.windows_.count(2));
        app.swarm_.advance(0);
        QVERIFY(!app.swarm_.find(2));
        QVERIFY(app.swarm_.find(3));
        QCOMPARE(app.swarm_.totalCount(), 2);
        QCOMPARE(app.swarm_.pendingCount(), std::size_t(0));
    }
    void visitorEntryStageReturnsToRegularBeforeExit() {
        Fixture f;
        health(f);
        const auto v = visitor(f);
        const auto state = [&](int sequence, int stage) {
            f.controller.receive(peer, session, {{"op", "states"},
                {"items", QJsonArray{QJsonArray{v.id, v.stream, QString::number(sequence),
                                                v.current.encode(), "guest", stage}}}});
        };
        state(1, 2);
        QCOMPARE(f.controller.visitors()[v.id].stage, 2);
        QVERIFY(f.controller.visitorStateText(f.controller.visitors()[v.id]).contains("入场"));
        state(2, 0);
        QCOMPARE(f.controller.visitors()[v.id].stage, 0);
        state(3, 1);
        QCOMPARE(f.controller.visitors()[v.id].stage, 1);
        state(4, 0);
        QCOMPARE(f.controller.visitors()[v.id].stage, 1);
    }
    void frozenRecallWaitsForGraceAndNeverRevivesAfterTimeout() {
        Fixture f;
        health(f);
        const auto o = outgoing(f);
        f.controller.lost(peer);
        f.controller.recallPet(2);
        f.controller.recallPet(2);
        QCOMPARE(f.controller.outgoing().size(), 1);
        QVERIFY(f.controller.outgoing()[2].recallRequested);
        QCOMPARE(f.controller.outgoing()[2].phase, DispatchController::Phase::Active);
        QCOMPARE(f.controller.outgoing()[2].frozenAt, qint64(100000));
        QVERIFY(f.controller.isAway(2));
        QVERIFY(!f.swarm.find(2)->entering());
        f.time += 29999;
        f.controller.tick();
        QVERIFY(f.controller.isAway(2));
        f.time++;
        f.controller.tick();
        QVERIFY(f.controller.outgoing().isEmpty());
        QVERIFY(f.swarm.find(2)->entering());
        QVERIFY(!f.swarm.find(2)->area.contains(f.swarm.find(2)->position));
        const auto start = f.swarm.find(2)->position;
        f.controller.receive(peer, session, {{"op", "active"}, {"id", o.id}, {"stream", o.stream}});
        QCOMPARE(f.controller.outgoing().size(), 0);
        QCOMPARE(f.swarm.find(2)->position, start);
        QCOMPARE(f.swarm.totalCount(), 3);
    }
    void visitorExpiresAndRejectsOldActivation() {
        Fixture f;
        health(f);
        const auto v = visitor(f);
        f.controller.lost(peer);
        f.time += 29999;
        f.controller.tick();
        QCOMPARE(f.controller.visitors().size(), 1);
        f.time++;
        f.controller.tick();
        QCOMPARE(f.controller.visitors().size(), 0);
        f.controller.receive(peer, session,
                             {{"op", "activate"},
                              {"id", v.id},
                              {"stream", v.stream},
                              {"state", v.current.encode()},
                              {"name", "old"}});
        QCOMPARE(f.controller.visitors().size(), 0);
        f.controller.receive(peer, session,
                             {{"op", "offer"}, {"id", v.id}, {"entity", "2"}, {"name", "old"}});
        QCOMPARE(f.controller.visitors().size(), 0);
    }
    void capacityAndOwnership() {
        Fixture f;
        health(f);
        f.swarm.setLimit(1);
        for (int i = 0; i < DispatchController::VisitorLimit + 1; ++i)
            f.controller.receive(peer, session,
                                 {{"op", "offer"},
                                  {"id", uuid()},
                                  {"entity", QString::number(i + 2)},
                                  {"name", "guest"}});
        QCOMPARE(f.controller.visitors().size(), DispatchController::VisitorLimit);
        QCOMPARE(f.swarm.totalCount(), 1);
        f.time += 3000;
        f.controller.tick();
        QCOMPARE(f.controller.visitors().size(), 0);
    }
    void staleSnapshotsAndHighlightDeduplication() {
        Fixture f;
        health(f);
        const auto v = visitor(f);
        auto state = v.current;
        state.position = {800, 300};
        auto message = [&](quint64 seq) {
            return QJsonObject{{"op", "states"},
                               {"items", QJsonArray{QJsonArray{v.id, v.stream, QString::number(seq),
                                                               state.encode(), "renamed", 0}}}};
        };
        f.controller.receive(peer, session, message(2));
        QCOMPARE(f.controller.visitors()[v.id].current.position, QPointF(800, 300));
        state.position = {0, 0};
        f.controller.receive(peer, session, message(1));
        QCOMPARE(f.controller.visitors()[v.id].current.position, QPointF(800, 300));
        QSignalSpy spy(&f.controller, &DispatchController::visitorHighlight);
        const QJsonObject high{
            {"op", "highlight"}, {"id", v.id}, {"stream", v.stream}, {"event", "1"}};
        f.controller.receive(peer, session, high);
        f.controller.receive(peer, session, high);
        QCOMPARE(spy.count(), 1);
        f.controller.lost(peer);
        f.controller.receive(peer, session, message(3));
        QCOMPARE(f.controller.visitors()[v.id].current.position, QPointF(800, 300));
    }
    void recoveryRequiresMatchingStreamAndSession() {
        Fixture f;
        health(f);
        const auto o = outgoing(f);
        f.controller.lost(peer);
        f.controller.receive(peer, session, {{"op", "active"}, {"id", o.id}, {"stream", uuid()}});
        QVERIFY(f.swarm.find(2)->dispatchPaused);
        f.controller.receive(peer, uuid(), {{"op", "active"}, {"id", o.id}, {"stream", o.stream}});
        QVERIFY(f.swarm.find(2)->dispatchPaused);
        f.controller.ready(peer, uuid());
        QVERIFY(!f.controller.isAway(2));
    }
    void bottomModePreservesDispatchVisibilityAndRemoteFoodOwnership() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 3;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        DispatchController::Outgoing o;
        o.entity = 2;
        o.id = uuid();
        o.peer = peer;
        o.session = session;
        o.stream = uuid();
        o.phase = DispatchController::Phase::Active;
        o.localHidden = true;
        o.home = app.swarm_.find(2)->position;
        o.homeArea = app.swarm_.find(2)->area;
        app.dispatch_->outgoing_[2] = o;
        app.swarm_.find(2)->motionGroup = peer;
        app.reconcileWindows();
        QVERIFY(!app.windows_.count(2));
        DispatchController::Visitor v;
        v.id = uuid();
        v.entity = "2";
        v.active = true;
        v.name = "访客食物只读";
        v.current = PetRenderState::from(*app.swarm_.find(3));
        v.previous = v.current;
        app.dispatch_->visitors_[v.id] = v;
        app.setBottomMode(true);
        app.reconcileVisitors();
        QVERIFY(app.visitorWindows_.at(v.id)->bottomMode());
        QVERIFY(app.canFeedPet(2));
        QVERIFY(app.useFood(2));
        QCOMPARE(app.food_.count(), 2);
        QVERIFY(app.swarm_.find(2)->cosmeticFeeding);
        app.swarm_.find(2)->advance(2.65, {});
        app.persist();
        QCOMPARE(app.food_.reservedCount(), 0);
        QCOMPARE(app.food_.count(), 2);
        QCOMPARE(app.dispatch_->visitors_[v.id].current.growth, v.current.growth);
        QVERIFY(!app.windows_.count(2));
        app.dispatch_->outgoing_[2].frozenAt = 1;
        QVERIFY(!app.canFeedPet(2));
        app.dispatch_->outgoing_[2].frozenAt = -1;
        app.dispatch_->outgoing_[2].phase = DispatchController::Phase::LeavingRemote;
        QVERIFY(!app.canFeedPet(2));
        app.dispatch_->visitors_[v.id].hidden = true;
        app.reconcileVisitors();
        QVERIFY(!app.visitorWindows_.count(v.id));
        app.setBottomMode(false);
        app.reconcileWindows();
        app.reconcileVisitors();
        QVERIFY(!app.windows_.count(2));
        QVERIFY(!app.visitorWindows_.count(v.id));
        app.dispatch_->visitors_[v.id].hidden = false;
        app.reconcileVisitors();
        QVERIFY(!app.visitorWindows_.at(v.id)->bottomMode());
        QVERIFY(app.visitorWindows_.at(v.id)->windowFlags().testFlag(Qt::WindowTransparentForInput));
    }
    void visitorListCannotOperateLocalEntityWithSameNumber() {
        QTemporaryDir dir;
        AnimationLibrary animation;
        RunOptions options;
        options.population = 3;
        ApplicationController app(animation, SettingsStore(dir.path()), options);
        app.start();
        DispatchController::Visitor v;
        v.id = uuid();
        v.entity = "2";
        v.name = "访客";
        v.sourceName = "other-host";
        v.peer = peer;
        v.session = session;
        v.active = true;
        app.dispatch_->visitors_.insert(v.id, v);
        app.showPets();
        QCOMPARE(app.petTable_->rowCount(), 4);
        QCOMPARE(app.petTable_->columnCount(), 8);
        QCOMPARE(app.petTable_->item(0, 6)->text(), QString("100.0%"));
        QCOMPARE(app.petTable_->item(0, 7)->text(), QString("成体"));
        QCOMPARE(app.petTable_->item(1, 7)->text(), QString("幼体"));
        QCOMPARE(app.petTable_->item(3, 6)->text(), QString("100.0%"));
        app.petTable_->selectRow(3);
        QCOMPARE(app.selectedPet(), quint64(0));
        QVERIFY(app.expelPet_->isEnabled());
        QVERIFY(!app.controlPet_->isEnabled());
        QVERIFY(!app.renamePet_->isEnabled());
        QVERIFY(!app.foodPet_->isEnabled());
        QVERIFY(!app.canFeedPet(app.selectedPet()));
        QVERIFY(!app.highlightPet_->isEnabled());
        QVERIFY(!app.dispatchPet_->isEnabled());
        QVERIFY(!app.recallPet_->isEnabled());
        app.petTable_->selectRow(1);
        app.petTable_->selectionModel()->select(app.petTable_->model()->index(2, 0),
                                                QItemSelectionModel::Select |
                                                    QItemSelectionModel::Rows);
        app.refreshPets();
        QCOMPARE(app.petTable_->selectionModel()->selectedRows().size(), 2);
        QVERIFY(app.dispatchPet_->isEnabled());
        QVERIFY(!app.expelPet_->isEnabled());
        QVERIFY(!app.controlPet_->isEnabled());
        QVERIFY(!app.renamePet_->isEnabled());
        app.petTable_->clearSelection();
        app.petTable_->selectRow(3);
        QVERIFY(app.expelPet_->isEnabled());
        app.expelPet_->click();
        QVERIFY(app.dispatch_->visitors_.isEmpty());
        QCOMPARE(app.petTable_->rowCount(), 3);
    }
    void resumeCommitDoesNotExtendGraceWithoutSourceConfirmation() {
        Fixture f;
        health(f);
        const auto v = visitor(f);
        f.controller.lost(peer);
        const auto stream = uuid();
        f.time += 29000;
        f.controller.receive(peer, session, {{"op", "resume"}, {"id", v.id}, {"stream", stream}});
        f.controller.receive(peer, session,
                             {{"op", "commit"},
                              {"id", v.id},
                              {"stream", stream},
                              {"phase", int(DispatchController::Phase::Active)},
                              {"stage", 0},
                              {"state", v.current.encode()},
                              {"name", "guest"}});
        QVERIFY(f.controller.visitors()[v.id].frozen);
        QVERIFY(f.controller.visitors()[v.id].awaitingState);
        QCOMPARE(f.controller.visitors()[v.id].frozenAt, qint64(100000));
        f.time += 1000;
        f.controller.tick();
        QVERIFY(f.controller.visitors().isEmpty());
    }
    void freshSnapshotCompletesResume() {
        Fixture f;
        health(f);
        const auto v = visitor(f);
        f.controller.lost(peer);
        const auto stream = uuid();
        f.controller.receive(peer, session, {{"op", "resume"}, {"id", v.id}, {"stream", stream}});
        f.controller.receive(peer, session,
                             {{"op", "commit"},
                              {"id", v.id},
                              {"stream", stream},
                              {"phase", int(DispatchController::Phase::Active)},
                              {"stage", 0},
                              {"state", v.current.encode()},
                              {"name", "guest"}});
        f.controller.receive(
            peer, session,
            {{"op", "states"},
             {"items", QJsonArray{QJsonArray{v.id, stream, "1", v.current.encode(), "guest", 0}}}});
        QVERIFY(!f.controller.visitors()[v.id].frozen);
        QCOMPARE(f.controller.visitors()[v.id].frozenAt, qint64(-1));
    }
    void reservationDeadlineAndWrongStreamCannotActivate() {
        Fixture f;
        health(f);
        const auto id = uuid();
        f.controller.receive(peer, session,
                             {{"op", "offer"}, {"id", id}, {"entity", "2"}, {"name", "guest"}});
        QCOMPARE(f.controller.visitors().size(), 1);
        f.time += 3000;
        f.controller.receive(peer, session,
                             {{"op", "activate"},
                              {"id", id},
                              {"stream", uuid()},
                              {"state", PetRenderState{}.encode()},
                              {"name", "guest"}});
        QVERIFY(f.controller.visitors().isEmpty());
        const auto o = outgoing(f);
        f.controller.lost(peer);
        f.controller.receive(peer, session, {{"op", "active"}, {"id", o.id}, {"stream", o.stream}});
        QVERIFY(f.swarm.find(2)->dispatchPaused);
    }
    void fadingBeforeDisconnectionDoesNotBlockBirth() {
        Fixture f;
        f.swarm.setLimit(3);
        f.swarm.beforeRemove = [&](quint64 id) { f.controller.removeEntity(id); };
        outgoing(f, 2);
        outgoing(f, 3);
        f.swarm.find(2)->retire();
        f.swarm.find(3)->retire();
        f.controller.lost(peer);
        f.swarm.spawn({400, 400}, {0, 0, 1280, 720}, 1, 2);
        QCOMPARE(f.swarm.pendingCount(), size_t(0));
        QCOMPARE(f.controller.outgoing().size(), 0);
        QCOMPARE(f.swarm.totalCount(), 3);
    }
    void deletionAndBirthQueueWhileFrozen() {
        Fixture f;
        f.swarm.beforeRemove = [&](quint64 id) { f.controller.removeEntity(id); };
        f.swarm.setLimit(3);
        outgoing(f, 2);
        outgoing(f, 3);
        f.controller.lost(peer);
        f.swarm.spawn({400, 400}, {0, 0, 1280, 720}, 1, 2);
        QCOMPARE(f.swarm.totalCount(), 3);
        QCOMPARE(f.swarm.pendingCount(), size_t(0));
        QCOMPARE(f.controller.outgoing().size(), 0);
        QCOMPARE(f.swarm.primary().id, quint64(1));
    }
};
} // namespace pettime
QTEST_MAIN(pettime::DispatchTest)
#include "TestDispatch.moc"
