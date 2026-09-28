#include "ApplicationController.h"
#include "DispatchController.h"
#include "PetWindow.h"
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace pettime {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
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
        o.home = {400, 400};
        o.homeArea = {0, 0, 1280, 720};
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
  private slots:
    void renderSnapshotAndValidation() {
        AnimationLibrary animation;
        PetModel model(3, {200, 200}, {0, 0, 1280, 720});
        for (int st = 0; st <= int(State::Fade); ++st) {
            model.state = State(st);
            model.crashQueued = true;
            QCOMPARE(animation.render(model), animation.render(PetRenderState::from(model)));
        }
        model.primary = false;
        model.juvenile = true;
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
        QCOMPARE(f.swarm.find(2)->position, QPointF(400, 400));
        QCOMPARE(f.swarm.primary().id, quint64(1));
    }
    void timeoutNeverRevivesAndRecallIsIdempotent() {
        Fixture f;
        health(f);
        const auto o = outgoing(f);
        f.controller.lost(peer);
        f.controller.recallPet(2);
        f.controller.recallPet(2);
        QCOMPARE(f.controller.outgoing().size(), 0);
        f.controller.receive(peer, session, {{"op", "active"}, {"id", o.id}, {"stream", o.stream}});
        QCOMPARE(f.controller.outgoing().size(), 0);
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
                                                               state.encode(), "renamed"}}}};
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
        app.petTable_->selectRow(3);
        QCOMPARE(app.selectedPet(), quint64(0));
        QVERIFY(!app.renamePet_->isEnabled());
        QVERIFY(!app.feedPet_->isEnabled());
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
        QVERIFY(!app.renamePet_->isEnabled());
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
                              {"state", v.current.encode()},
                              {"name", "guest"}});
        f.controller.receive(
            peer, session,
            {{"op", "states"},
             {"items", QJsonArray{QJsonArray{v.id, stream, "1", v.current.encode(), "guest"}}}});
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
