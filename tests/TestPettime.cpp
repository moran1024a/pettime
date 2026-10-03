#include "AnimationLibrary.h"
#include "ApplicationController.h"
#include "PetWindow.h"
#include "SettingsStore.h"
#include "SwarmController.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

using namespace pettime;
class TestPettime : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { animation_ = std::make_unique<AnimationLibrary>(); }
    void feedingCompletesOnlyAfterAnimation() {
        PetModel p(5, {500, 500}, {0, 0, 1000, 1000}, true);
        p.affection = 99;
        QVERIFY(p.playFeeding());
        QVERIFY(!p.playFeeding());
        p.advance(2.64, {});
        QVERIFY(p.cosmeticFeeding);
        QCOMPARE(p.feedingCompletions(), quint64(0));
        p.advance(.02, {});
        QVERIFY(!p.cosmeticFeeding);
        QCOMPARE(p.feedingCompletions(), quint64(1));
        QCOMPARE(p.affection, 99);
        p.advance(1, {});
        QCOMPARE(p.feedingCompletions(), quint64(1));
    }
    void pausedFlightFreezesFrame() {
        PetModel p(7, {500, 500}, {0, 0, 1000, 1000});
        p.demoPounce();
        p.advance(.1, {});
        p.paused = true;
        const auto state = p.state;
        const auto pos = p.position;
        const auto age = p.actionAge;
        const QImage image = animation_->render(p);
        for (int i = 0; i < 500; ++i)
            p.advance(.02, {});
        QCOMPARE(p.state, state);
        QCOMPARE(p.actionAge, age);
        QCOMPARE(p.position, pos);
        QCOMPARE(animation_->render(p), image);
        p.paused = false;
        bool impact = false;
        for (int i = 0; i < 200; ++i) {
            p.advance(.02, {-1000, -1000});
            impact |= p.impact;
        }
        QVERIFY(impact);
    }
    void feedingAtEdgeWhileMenuOpen() {
        PetModel p(4, {20, 20}, {0, 0, 800, 600}, true);
        p.menuOpen = true;
        QVERIFY(p.playFeeding());
        const auto position = p.position;
        for (int i = 0; i < 133; ++i)
            p.advance(.02, {});
        QVERIFY(!p.cosmeticFeeding);
        QCOMPARE(p.feedingCompletions(), quint64(1));
        QCOMPARE(p.position, position);
        QCOMPARE(p.affection, 0);
    }
    void growthBoundaries() {
        QCOMPARE(PetModel::growthScale(-1), MinimumScale);
        QCOMPARE(PetModel::growthScale(0), MinimumScale);
        QVERIFY(PetModel::growthScale(60) > MinimumScale);
        QCOMPARE(PetModel::growthScale(100), AdultScale);
        QCOMPARE(PetModel::growthScale(100000), AdultScale);
    }
    void growthMaturesIndependentlyOfLifetime() {
        PetModel p(3, {500, 500}, {0, 0, 1000, 1000}, true);
        QCOMPARE(p.growth(), 0.0);
        const auto duration = p.growthDuration();
        QVERIFY(duration >= 1800 && duration <= 3600);
        p.age = 10000;
        p.advance(duration * .249, {});
        QVERIFY(p.growth() < 25);
        QCOMPARE(PetModel::growthState(p.growth()), QString("幼体"));
        p.advance(duration * .001, {});
        QCOMPARE(PetModel::growthState(p.growth()), QString("成长中"));
        p.advance(duration * .75, {});
        QCOMPARE(p.growth(), 100.0);
        QVERIFY(!p.juvenile);
        QCOMPARE(p.scale, AdultScale);
        p.scale = 1;
        p.advance(20, {});
        QCOMPARE(p.scale, 1.0);
        p.id = 1;
        p.affection = 42;
        p.crush();
        p.advance(1.1, {});
        const auto lifetime = p.age;
        p.becomeNymph();
        QCOMPARE(p.id, quint64(1));
        QCOMPARE(p.affection, 42);
        QCOMPARE(p.age, lifetime);
        QCOMPARE(p.growth(), 0.0);
        QCOMPARE(p.scale, MinimumScale);
        QVERIFY(p.juvenile);
        QVERIFY(p.growthDuration() >= 1800 && p.growthDuration() <= 3600);
    }
    void growthRandomnessIsIndependentOfMovement() {
        QSet<int> durations;
        for (int seed = 1; seed <= 80; ++seed) {
            PetModel p(seed, {500, 500}, {0, 0, 1000, 1000}, true);
            QVERIFY(p.growthDuration() >= 1800 && p.growthDuration() <= 3600);
            durations.insert(int(p.growthDuration()));
            PetModel q = p;
            for (int i = 0; i < seed; ++i)
                q.random(0, 1);
            QVERIFY(p.playFeeding());
            QVERIFY(q.playFeeding());
            p.advance(2.65, {});
            q.advance(2.65, {});
            QCOMPARE(q.growth(), p.growth());
            p.becomeNymph();
            q.becomeNymph();
            QCOMPARE(q.growthDuration(), p.growthDuration());
            QCOMPARE(p.feedingCompletions(), quint64(1));
        }
        QVERIFY(durations.size() > 1);
    }
    void feedingGrowthIsLocalRandomAndOnce() {
        QSet<int> bonuses;
        for (int seed = 1; seed <= 160; ++seed) {
            PetModel p(seed, {500, 500}, {0, 0, 1000, 1000}, true);
            PetModel other = p;
            QVERIFY(p.playFeeding());
            p.advance(2.65, {});
            const double bonus = p.growth() - 2.65 * 100 / p.growthDuration();
            QVERIFY(std::abs(bonus * 10 - std::round(bonus * 10)) < 1e-9);
            QVERIFY(bonus >= .5 - 1e-9 && bonus <= 1.5 + 1e-9);
            bonuses.insert(qRound(bonus * 10));
            QCOMPARE(other.growth(), 0.0);
            QCOMPARE(p.affection, 0);
            QCOMPARE(p.feedingCompletions(), quint64(1));
            const auto grown = p.growth();
            p.advance(.1, {});
            QVERIFY(std::abs(p.growth() - grown - .1 * 100 / p.growthDuration()) < 1e-9);
            QCOMPARE(p.feedingCompletions(), quint64(1));
        }
        QCOMPARE(bonuses.size(), 11);
        PetModel adult(4, {500, 500}, {0, 0, 1000, 1000});
        QVERIFY(!adult.playFeeding());
        QCOMPARE(adult.feedingCompletions(), quint64(0));
    }
    void feedingCancellationFreezeAndCap() {
        PetModel p(5, {500, 500}, {0, 0, 1000, 1000}, true);
        QVERIFY(p.playFeeding());
        p.advance(1, {});
        const auto natural = p.growth();
        p.crush();
        p.advance(1.1, {});
        QCOMPARE(p.growth(), natural);
        QCOMPARE(p.feedingCompletions(), quint64(0));
        p.becomeNymph();
        QVERIFY(p.playFeeding());
        p.advance(2.65, {});
        QCOMPARE(p.feedingCompletions(), quint64(1));
        QCOMPARE(p.affection, 0);
        p.becomeNymph();
        QCOMPARE(p.feedingCompletions(), quint64(1));
        p.advance(p.growthDuration() - 2, {});
        QVERIFY(p.playFeeding());
        p.advance(2.65, {});
        QCOMPARE(p.growth(), 100.0);
        QVERIFY(!p.juvenile);
        QVERIFY(!p.playFeeding());
        QCOMPARE(p.feedingCompletions(), quint64(2));
        SwarmController swarm(8);
        swarm.spawn({400, 400}, {0, 0, 1000, 1000}, 1, 1);
        auto *child = swarm.find(2);
        QVERIFY(child->playFeeding());
        child->dispatchPaused = true;
        child->advance(20, {}); // Direct model callers must honor the same freeze.
        swarm.advance(20);
        QCOMPARE(child->growth(), 0.0);
        QCOMPARE(child->cosmeticAge, 0.0);
        QCOMPARE(child->age, 0.0);
        QCOMPARE(child->feedingCompletions(), quint64(0));
        QVERIFY(child->cosmeticFeeding);
        QVERIFY(!child->playFeeding());
        child->dispatchPaused = false;
        swarm.advance(2.65);
        QVERIFY(child->growth() >= .5);
        QCOMPARE(child->feedingCompletions(), quint64(1));
    }
    void matureChildSplitsIntoMinimumGrowthChildren() {
        SwarmController swarm(3);
        swarm.spawn({400, 400}, {0, 0, 1000, 1000}, 1, 1);
        auto *p = swarm.find(2);
        p->advance(p->growthDuration(), {});
        QVERIFY(!p->juvenile);
        QVERIFY(!p->primary);
        p->crush();
        swarm.advance(1.1);
        QVERIFY(!swarm.find(2));
        for (const auto &child : swarm.pets()) {
            if (child->primary)
                continue;
            QCOMPARE(child->growth(), 0.0);
            QCOMPARE(child->scale, MinimumScale);
            QVERIFY(child->juvenile);
        }
        QCOMPARE(swarm.primary().growth(), 100.0);
    }
    void chaseProbability() {
        QCOMPARE(PetModel::chaseChance(2), 0.0);
        QCOMPARE(PetModel::chaseChance(3), .12);
        QCOMPARE(PetModel::chaseChance(100), .7);
        PetModel p(1, {500, 500}, {0, 0, 1000, 1000});
        p.advance(.01, p.position + QPointF(30, 0));
        p.advance(.01, p.position + QPointF(40, 0));
        QCOMPARE(p.evadeCount, 1);
        p.paused = true;
        p.advance(6, {});
        QCOMPARE(p.evadeCount, 0);
    }
    void crushAndSwarmLimit() {
        PetModel main(12, {500, 500}, {0, 0, 1000, 1000});
        main.crush();
        main.advance(1.06, {});
        QVERIFY(main.splitReady);
        main.becomeNymph();
        SwarmController swarm(12);
        swarm.spawn(main.position, main.area, main.generation, 9);
        QCOMPARE(swarm.totalCount(), 10);
        for (int round = 0; round < 12; ++round) {
            for (const auto &p : swarm.pets())
                p->crush();
            for (int i = 0; i < 400; ++i) {
                swarm.advance(.02);
                QVERIFY(swarm.totalCount() <= 72);
            }
            QCOMPARE(swarm.pendingCount(), std::size_t(0));
        }
        QVERIFY(swarm.totalCount() > 48);
    }
    void swarmOverflowDoesNotOverRetire() {
        SwarmController swarm(1);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 1, 71);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 2, 10);
        for (int i = 0; i < 10; ++i)
            swarm.advance(.02);
        int retiring = 0;
        for (const auto &p : swarm.pets())
            if (p->state == State::Fade)
                ++retiring;
        QCOMPARE(retiring, 10);
        for (int i = 0; i < 40; ++i)
            swarm.advance(.02);
        QCOMPARE(swarm.totalCount(), 72);
        QCOMPARE(swarm.pendingCount(), std::size_t(0));
    }
    void identityAndDynamicLimit() {
        SwarmController swarm(4);
        auto *primary = &swarm.primary();
        primary->affection = 45;
        QCOMPARE(primary->id, std::uint64_t(1));
        QCOMPARE(primary->displayName(), QSysInfo::machineHostName());
        QVERIFY(!swarm.rename(1, "other"));
        swarm.spawn(primary->position, primary->area, 1, 71);
        QVERIFY(swarm.rename(2, "  小强  "));
        QCOMPARE(swarm.find(2)->displayName(), QString("小强"));
        QVERIFY(swarm.rename(2, ""));
        QCOMPARE(swarm.find(2)->displayName(), QString("蟑螂 #2"));
        std::vector<std::uint64_t> removed;
        swarm.beforeRemove = [&](std::uint64_t id) {
            QVERIFY(swarm.find(id)); // Notification precedes model destruction.
            QVERIFY(id != 1);
            removed.push_back(id);
        };
        swarm.spawn(primary->position, primary->area, 2, 12);
        swarm.setLimit(3);
        QCOMPARE(swarm.totalCount(), 3);
        QCOMPARE(swarm.pendingCount(), std::size_t(0));
        QCOMPARE(removed.front(), std::uint64_t(2));
        swarm.setLimit(1);
        for (int i = 0; i < 20; ++i) {
            primary->crush();
            primary->advance(1.1, {});
            QVERIFY(primary->splitReady);
            primary->becomeNymph();
            swarm.spawn(primary->position, primary->area, primary->generation, 12);
            swarm.advance(2);
            QCOMPARE(swarm.totalCount(), 1);
            QCOMPARE(&swarm.primary(), primary);
            QCOMPARE(primary->affection, 45);
        }
        swarm.setLimit(2);
        swarm.spawn(primary->position, primary->area, 1, 12);
        QVERIFY(swarm.pets().back()->id > 72);
        for (int i = 0; i < 15; ++i) {
            swarm.pets().back()->crush();
            swarm.advance(1.1);
            QCOMPARE(swarm.totalCount(), 2);
            QCOMPARE(swarm.pendingCount(), std::size_t(0));
        }
    }
    void listFeedingPreservesAffectionAndAction() {
        for (bool isPrimary : {false, true}) {
            PetModel p(9, {500, 500}, {0, 0, 1000, 1000}, true);
            p.primary = isPrimary;
            p.affection = 38;
            const PetModel before = p;
            QVERIFY(p.playFeeding());
            QVERIFY(!p.playFeeding());
            for (int i = 0; i < 133; ++i)
                p.advance(.02, {});
            QVERIFY(!p.cosmeticFeeding);
            QCOMPARE(p.affection, 38);
            QCOMPARE(p.state, before.state);
            QCOMPARE(p.position, before.position);
            QCOMPARE(p.actionAge, before.actionAge);
            QCOMPARE(p.generation, before.generation);
            QCOMPARE(p.splitReady, false);
            QVERIFY(p.age > before.age); // Natural growth still uses wall time.
            const double bonus = p.growth() - (p.age - before.age) * 100 / p.growthDuration();
            QVERIFY(bonus >= .5 - 1e-9 && bonus <= 1.5 + 1e-9);
            QCOMPARE(p.feedingCompletions(), quint64(1));
            p.crush();
            QVERIFY(!p.playFeeding());
        }
    }
    void entityListAndLimitMenu() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QVERIFY(store.saveLimit(3));
        RunOptions options;
        options.population = 3;
        ApplicationController controller(*animation_, store, options);
        controller.start();
        QAction *listAction = nullptr, *limitAction = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *menu = qobject_cast<QMenu *>(widget))
                for (auto *action : menu->actions()) {
                    if (action->text() == "蟑螂列表…")
                        listAction = action;
                    if (action->text().startsWith("数量上限…"))
                        limitAction = action;
                }
        QVERIFY(listAction && limitAction);
        listAction->trigger();
        QDialog *dialog = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->windowTitle() == "蟑螂列表")
                dialog = qobject_cast<QDialog *>(widget);
        QVERIFY(dialog);
        auto *table = dialog->findChild<QTableWidget *>();
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->currentRow(), 0);
        QCOMPARE(table->item(0, 1)->text(), QSysInfo::machineHostName());
        QPushButton *rename = nullptr, *food = nullptr;
        for (auto *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() == "重命名")
                rename = button;
            if (button->text() == "食物…")
                food = button;
        }
        QVERIFY(rename && food);
        QVERIFY(!rename->isEnabled());
        table->selectRow(1);
        QVERIFY(rename->isEnabled());
        QTimer::singleShot(0, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *input = qobject_cast<QInputDialog *>(widget)) {
                    input->setTextValue("测试小强");
                    input->accept();
                }
        });
        rename->click();
        QCOMPARE(table->item(1, 1)->text(), QString("测试小强"));
        food->click();
        QDialog *foodDialog = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->windowTitle() == "食物面板")
                foodDialog = qobject_cast<QDialog *>(widget);
        QVERIFY(foodDialog);
        auto *use = foodDialog->findChild<QPushButton *>("useBreadcrumb");
        QVERIFY(use && use->isEnabled());
        use->click();
        QCOMPARE(table->item(1, 3)->text(), QString("投喂动画"));
        QVERIFY(!use->isEnabled());
        QTimer::singleShot(0, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *input = qobject_cast<QInputDialog *>(widget)) {
                    input->setIntValue(1);
                    input->accept();
                }
        });
        limitAction->trigger();
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->currentRow(), -1);
        QVERIFY(!food->isEnabled());
        QCOMPARE(store.loadLimit(), 1);
        QCOMPARE(store.loadAffinity(), 0);
        listAction->trigger();
        QCOMPARE(table->currentRow(), 0);
    }
    void networkMenuOpensWithoutStartingService() {
        QTemporaryDir dir;
        ApplicationController controller(*animation_, SettingsStore(dir.path()), RunOptions{});
        QAction *networkAction = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *menu = qobject_cast<QMenu *>(widget))
                for (auto *action : menu->actions())
                    if (action->text() == "网络面板…") networkAction = action;
        QVERIFY(networkAction);
        networkAction->trigger();
        NetworkPanel *panel = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<NetworkPanel *>(widget)) panel = candidate;
        QVERIFY(panel); QVERIFY(panel->isVisible());
        QVERIFY(!panel->findChild<QCheckBox *>("networkEnabled")->isChecked());
        QVERIFY(!QFile::exists(dir.path() + "/network-device-id.dat"));
        panel->close(); networkAction->trigger(); QVERIFY(panel->isVisible());
    }
    void screenRecoveryAndDeterminism() {
        PetModel a(42, {1000, 500}, {0, 0, 1920, 1080}), b = a;
        for (int i = 0; i < 1000; ++i) {
            a.advance(.016, {-1000, -1000});
            b.advance(.016, {-1000, -1000});
        }
        QCOMPARE(a.position, b.position);
        a.setArea({-640, -480, 640, 480});
        QVERIFY(a.area.contains(a.position));
        a.setArea({0, 0, 20, 20});
        a.advance(.02, {});
        QVERIFY(std::isfinite(a.position.x()));
        QVERIFY(a.area.contains(a.position));
    }
    void exitChoosesNearestEdgeAndStableTies() {
        const QRectF area(-500, 20, 900, 600);
        const std::vector<std::pair<QPointF, int>> cases{
            {{area.left() + 40, area.center().y()}, 0},
            {{area.right() - 40, area.center().y()}, 1},
            {{area.center().x(), area.top() + 40}, 2},
            {{area.center().x(), area.bottom() - 40}, 3},
            {{area.left() + 40, area.top() + 40}, 0},
            {{area.right() - 40, area.top() + 40}, 1},
            {{area.left() + 40, area.bottom() - 40}, 0},
            {{area.right() - 40, area.bottom() - 40}, 1}};
        for (const auto &[position, edge] : cases) {
            PetModel p(7, position, area);
            p.primary = false;
            QVERIFY(p.beginExit());
            QCOMPARE(p.position, position);
            QCOMPARE(p.transitionEdge(), edge);
            const double fraction = edge < 2 ? (position.y() - area.top()) / area.height()
                                              : (position.x() - area.left()) / area.width();
            QCOMPARE(p.transitionFraction(), fraction);
            QVERIFY(p.transitioning());
            QVERIFY(p.exiting());
            QVERIFY(!p.entering());
            QVERIFY(!p.exitComplete());
            QVERIFY(!p.beginExit());
        }
        PetModel tied(8, {500, 500}, {0, 0, 1000, 1000});
        tied.primary = false;
        QVERIFY(tied.beginExit());
        QCOMPARE(tied.transitionEdge(), 0);
    }
    void edgeEntryStartsOutsideAndEndsSafeAtCorners() {
        const QRectF area(-500, 20, 900, 600);
        for (int edge = 0; edge < 4; ++edge)
            for (double fraction : {0.0, .01, .5, .99, 1.0}) {
                PetModel p(7, area.center(), area);
                p.primary = false;
                p.beginEntry(area, edge, fraction);
                QVERIFY(p.entering());
                QCOMPARE(p.transitionEdge(), edge);
                QCOMPARE(p.transitionFraction(), fraction);
                const QRectF window(p.position - QPointF(PetWindowExtent / 2, PetWindowExtent / 2),
                                    QSizeF(PetWindowExtent, PetWindowExtent));
                QVERIFY(!window.intersects(area));
                const auto start = p.position;
                p.setArea(area);
                QCOMPARE(p.position, start);
                for (double dt : {.2, .05, .3})
                    p.advance(dt, {});
                QVERIFY(p.entering());
                p.advance(.45, {});
                QVERIFY(!p.transitioning());
                QVERIFY(area.adjusted(35, 35, -35, -35).contains(p.position));
                QVERIFY(p.traveled >= PetWindowExtent / 2 + 2 + 99);
                QVERIFY(p.setManualControl(true));
            }
        PetModel small(7, {10, 10}, {0, 0, 20, 20});
        small.primary = false;
        small.beginEntry(small.area, 0, 0);
        small.advance(1, {});
        QVERIFY(small.area.contains(small.position));
        QVERIFY(std::isfinite(small.position.x()) && std::isfinite(small.position.y()));
    }
    void edgeExitEndsFullyOutsideAndWaitsForHandoff() {
        const QRectF area(-500, 20, 900, 600);
        const std::vector<QPointF> starts{{area.left() + 60, area.center().y()},
                                         {area.right() - 60, area.center().y()},
                                         {area.center().x(), area.top() + 60},
                                         {area.center().x(), area.bottom() - 60}};
        for (int edge = 0; edge < 4; ++edge) {
            PetModel p(7, starts[edge], area);
            p.primary = false;
            QVERIFY(p.beginExit());
            QCOMPARE(p.transitionEdge(), edge);
            QCOMPARE(p.actionDuration, 1.0);
            p.advance(.6, {});
            QVERIFY(!p.exitComplete());
            p.advance(.4, {});
            QVERIFY(p.exitComplete());
            QVERIFY(p.transitioning());
            QVERIFY(p.exiting());
            const QRectF window(p.position - QPointF(PetWindowExtent / 2, PetWindowExtent / 2),
                                QSizeF(PetWindowExtent, PetWindowExtent));
            QVERIFY(!window.intersects(area));
            QVERIFY(std::abs(p.traveled - (60 + PetWindowExtent / 2 + 2)) < 1e-9);
            QCOMPARE(p.speed, 0.0);
            const auto endpoint = p.position;
            const auto phase = p.phase;
            p.advance(10, {});
            QCOMPARE(p.position, endpoint);
            QCOMPARE(p.phase, phase);
            QVERIFY(!p.playFeeding());
            QVERIFY(!p.setManualControl(true));
            p.crush();
            QVERIFY(p.active());
            QVERIFY(p.exitComplete());
        }
    }
    void edgeTransitionsConsumeVariableDtAndTrackActualDistance() {
        const QRectF area(0, 0, 1000, 1000);
        PetModel delayed(7, area.center(), area);
        delayed.primary = false;
        delayed.beginEntry(area, 0, .5);
        PetModel frequent = delayed;
        const auto start = delayed.position;
        delayed.advance(.2, {});
        QVERIFY(std::abs(delayed.position.x() - start.x() - 42.8) < 1e-9);
        for (int i = 0; i < 4; ++i)
            frequent.advance(.05, {});
        QVERIFY(std::hypot((delayed.position - frequent.position).x(),
                           (delayed.position - frequent.position).y()) < 1e-9);
        delayed.advance(.3, {});
        delayed.advance(.5, {});
        for (int i = 0; i < 8; ++i)
            frequent.advance(.1, {});
        QVERIFY(!delayed.entering());
        QVERIFY(!frequent.entering());
        QCOMPARE(delayed.position, frequent.position);
        QVERIFY(std::abs(delayed.traveled - frequent.traveled) < 1e-9);
        QVERIFY(std::abs(std::remainder(delayed.phase - frequent.phase, 1)) < 1e-9);
        for (double side : {1000.0, 3000.0}) {
            PetModel p(7, {side / 2, side / 2}, {0, 0, side, side});
            p.primary = false;
            QVERIFY(p.beginExit());
            const double duration = std::clamp((side / 2 + PetWindowExtent / 2 + 2) / 300,
                                               1.0, 3.0);
            QCOMPARE(p.actionDuration, duration);
            PetModel q = p;
            p.advance(duration * .4, {});
            for (int i = 0; i < 6; ++i)
                q.advance(duration * .4 / 6, {});
            QVERIFY(std::abs(p.position.x() - q.position.x()) < 1e-9);
            QVERIFY(!p.exitComplete());
            p.advance(duration * .6 + .2, {});
            q.advance(duration * .6, {});
            QVERIFY(p.exitComplete());
            QVERIFY(q.exitComplete());
            QCOMPARE(p.position, q.position);
            QVERIFY(std::abs(p.traveled - q.traveled) < 1e-9);
            QVERIFY(std::abs(std::remainder(p.phase - q.phase, 1)) < 1e-9);
        }
    }
    void entryCanReverseToExitWithoutJumpingOrCrossingScreen() {
        const QRectF area(0, 0, 1000, 1000);
        for (int edge = 0; edge < 4; ++edge) {
            PetModel p(7, area.center(), area);
            p.primary = false;
            p.beginEntry(area, edge, .5);
            p.advance(.2, {});
            const auto before = p.position;
            QVERIFY(p.beginExit());
            QCOMPARE(p.position, before);
            QCOMPARE(p.transitionEdge(), edge);
            QVERIFY(!p.entering());
            p.advance(1, {});
            QVERIFY(p.exitComplete());
            QVERIFY(std::hypot((p.position - before).x(), (p.position - before).y()) < 43);
            p.beginEntry(area, edge, .5);
            QVERIFY(p.entering());
            QVERIFY(!p.exiting());
        }
        PetModel corner(7, area.center(), area);
        corner.primary = false;
        corner.beginEntry(area, 0, 0);
        const auto outsideCorner = corner.position;
        QVERIFY(corner.beginExit());
        QCOMPARE(corner.position, outsideCorner);
        QCOMPARE(corner.transitionEdge(), 2); // Distance to the top edge line is zero.
        corner.advance(1, {});
        QVERIFY(corner.exitComplete());
        QCOMPARE(corner.position.x(), outsideCorner.x());
    }
    void transitionLocksActionsCancelsFeedingAndAllowsRetirement() {
        {
            PetModel p(7, {500, 500}, {0, 0, 1000, 1000}, true);
            p.primary = false;
            QVERIFY(p.setManualControl(true));
            QVERIFY(p.playFeeding());
            p.advance(.2, {});
            p.dispatchLocked = true;
            QVERIFY(p.beginExit());
            QVERIFY(!p.manuallyControlled());
            QVERIFY(!p.cosmeticFeeding);
            QVERIFY(!p.mealActive);
            QVERIFY(!p.eating);
            QVERIFY(!p.playFeeding());
            QVERIFY(!p.setManualControl(true));
            p.crush();
            QCOMPARE(p.state, State::Probe);
            p.advance(3, {});
            QVERIFY(p.exitComplete());
            QVERIFY(std::abs(p.growth() - p.age * 100 / p.growthDuration()) < 1e-9);
            QCOMPARE(p.affection, 0);
            QCOMPARE(p.feedingCompletions(), quint64(0));
            p.cancelTransition();
            QVERIFY(!p.transitioning());
            QVERIFY(p.dispatchLocked);
            QVERIFY(!p.playFeeding());
            QVERIFY(!p.setManualControl(true));
            p.crush();
            QCOMPARE(p.state, State::Probe);
            p.dispatchLocked = false;
            p.beginEntry(p.area, 0, .5);
            QVERIFY(p.entering());
            p.cancelEntry();
            QVERIFY(!p.transitioning());
            QVERIFY(p.setManualControl(true));
            QVERIFY(p.playFeeding());
            p.dispatchLocked = true;
            p.beginEntry(p.area, 0, .5);
            QVERIFY(!p.cosmeticFeeding);
            p.retire();
            QVERIFY(!p.transitioning());
            QCOMPARE(p.state, State::Fade);
            p.advance(.4, {});
            QVERIFY(p.expired);
            QCOMPARE(p.feedingCompletions(), quint64(0));
        }
        PetModel rejected(7, {500, 500}, {0, 0, 1000, 1000});
        QVERIFY(!rejected.beginExit());
        rejected.primary = false;
        rejected.expired = true;
        QVERIFY(!rejected.beginExit());
        rejected.expired = false;
        rejected.splitReady = true;
        QVERIFY(!rejected.beginExit());
        rejected.splitReady = false;
        rejected.retire();
        QVERIFY(!rejected.beginExit());
    }
    void transitionScreenChangesReplanWithoutTeleporting() {
        const QRectF area(-500, 20, 900, 600);
        PetModel entry(7, area.center(), area);
        entry.primary = false;
        entry.beginEntry(area, 0, .5);
        entry.advance(.2, {});
        const auto entryPosition = entry.position;
        entry.setArea(area);
        QCOMPARE(entry.position, entryPosition);
        const QRectF shifted(-600, 20, 900, 600);
        entry.setArea(shifted);
        QCOMPARE(entry.position, entryPosition);
        QCOMPARE(entry.transitionEdge(), 0);
        entry.advance(.8, {});
        QVERIFY(!entry.entering());
        QVERIFY(shifted.adjusted(35, 35, -35, -35).contains(entry.position));
        PetModel exit(7, {area.left() + 40, area.center().y()}, area);
        exit.primary = false;
        QVERIFY(exit.beginExit());
        exit.advance(.2, {});
        const auto exitPosition = exit.position;
        exit.setArea(area);
        QCOMPARE(exit.position, exitPosition);
        const QRectF replacement(-1200, 20, 600, 600);
        exit.setArea(replacement);
        QCOMPARE(exit.position, exitPosition);
        QCOMPARE(exit.transitionEdge(), 1);
        exit.advance(1, {});
        QVERIFY(exit.exitComplete());
        const auto endpoint = exit.position;
        exit.setArea(replacement);
        QCOMPARE(exit.position, endpoint);
        QVERIFY(exit.exitComplete());
        const QRectF expanded(-1200, 20, 1200, 600);
        QVERIFY(expanded.contains(endpoint));
        exit.setArea(expanded);
        QCOMPARE(exit.position, endpoint);
        QVERIFY(!exit.exitComplete());
        exit.advance(3, {});
        QVERIFY(exit.exitComplete());
        const QRectF window(exit.position - QPointF(PetWindowExtent / 2, PetWindowExtent / 2),
                            QSizeF(PetWindowExtent, PetWindowExtent));
        QVERIFY(!window.intersects(expanded));
    }
    void transitionGrowthUsesActiveTimeAndDispatchFreeze() {
        SwarmController swarm(8);
        swarm.spawn({400, 400}, {0, 0, 1000, 1000}, 1, 1);
        auto *p = swarm.find(2);
        QVERIFY(p);
        p->beginEntry(p->area, 0, .5);
        const auto start = p->position;
        p->dispatchPaused = true;
        swarm.advance(20);
        QCOMPARE(p->position, start);
        QCOMPARE(p->age, 0.0);
        QCOMPARE(p->growth(), 0.0);
        QVERIFY(p->entering());
        p->dispatchPaused = false;
        swarm.advance(.2);
        const auto beforeExit = p->position;
        QVERIFY(p->beginExit());
        p->dispatchLocked = true;
        p->dispatchPaused = true;
        swarm.advance(20);
        QCOMPARE(p->position, beforeExit);
        QVERIFY(!p->exitComplete());
        QCOMPARE(p->age, .2);
        p->dispatchPaused = false;
        swarm.advance(1);
        QVERIFY(p->exitComplete());
        QVERIFY(std::abs(p->growth() - 1.2 * 100 / p->growthDuration()) < 1e-9);
        const auto grown = p->growth();
        swarm.advance(.2);
        QVERIFY(std::abs(p->growth() - grown - .2 * 100 / p->growthDuration()) < 1e-9);
    }
    void animationFrames() {
        QCOMPARE(animation_->walk.size(), std::size_t(72));
        QCOMPARE(animation_->idle.size(), std::size_t(48));
        QCOMPARE(animation_->nymph.size(), std::size_t(48));
        QCOMPARE(animation_->frontPounce.size(), std::size_t(72));
        QCOMPARE(animation_->crush.size(), std::size_t(16));
        for (const auto *group :
             {&animation_->walk, &animation_->idle, &animation_->nymph, &animation_->nymphIdle,
              &animation_->pounce, &animation_->frontPounce, &animation_->crush,
              &animation_->nymphCrush, &animation_->nymphPounce})
            for (const auto &frame : *group) {
                QVERIFY(!frame.isNull());
                QVERIFY(!AnimationLibrary::alphaBounds(frame).isEmpty());
                QCOMPARE(qAlpha(frame.pixel(0, 0)), 0);
                QCOMPARE(frame.format(), QImage::Format_ARGB32_Premultiplied);
            }
    }
    void renderAndInputMask() {
        PetModel p(1, {0, 0}, {-1000, -1000, 2000, 2000});
        for (double dpr : {1.0, 1.5, 2.0}) {
            const QImage image = animation_->render(p, 320, dpr);
            QCOMPARE(image.width(), qCeil(320 * dpr));
            const auto region = PetWindow::inputRegion(image, {320, 320});
            QVERIFY(!region.contains({0, 0}));
            QVERIFY(region.contains({160, 160}));
        }
        p.becomeNymph();
        p.advance(p.growthDuration(), {});
        QVERIFY(!p.juvenile);
        for (int i = 0; i < 36; ++i) {
            p.heading = i * Pi / 18;
            const auto image = animation_->render(p, 224);
            const auto bounds = AnimationLibrary::alphaBounds(image, 0, 0);
            QVERIFY(bounds.left() > 0 && bounds.top() > 0 && bounds.right() < 223 &&
                    bounds.bottom() < 223);
        }
    }
    void portableProgressRoundTripAndTransfer() {
        QCOMPARE(SettingsStore().directory(), QCoreApplication::applicationDirPath());
        QTemporaryDir source, destination;
        SettingsStore store(source.path());
        SwarmController swarm(5);
        swarm.setLimit(12);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 3, 3);
        QVERIFY(swarm.removeLocalPet(4));
        swarm.primary().becomeNymph();
        swarm.primary().affection = 67;
        auto *child = swarm.find(2);
        child->advance(180, {-10000, -10000});
        child->customName = "可转移的小强";
        child->affection = 12;
        child->motionGroup = "another-device";
        child->dispatchPaused = true;
        auto progress = swarm.saveProgress();
        QVERIFY(store.saveJson("progress.json", progress));
        QVERIFY(store.saveJson("config.json", {{"populationLimit", 12}}));
        QVERIFY(QFile::copy(source.filePath("progress.json"), destination.filePath("progress.json")));
        SettingsStore transferred(destination.path());
        SwarmController restored(7, {200, 200}, {0, 0, 400, 400});
        auto *primaryAddress = &restored.primary();
        restored.restoreProgress(transferred.loadJson("progress.json"));
        QCOMPARE(&restored.primary(), primaryAddress);
        QCOMPARE(restored.totalCount(), 3);
        QCOMPARE(restored.primary().id, quint64(1));
        QCOMPARE(restored.primary().displayName(), QSysInfo::machineHostName());
        QCOMPARE(restored.primary().affection, 67);
        QCOMPARE(restored.primary().growth(), 0.0);
        QCOMPARE(restored.primary().growthDuration(), swarm.primary().growthDuration());
        QCOMPARE(restored.find(2)->customName, child->customName);
        QCOMPARE(restored.find(2)->growth(), child->growth());
        QCOMPARE(restored.find(2)->growthDuration(), child->growthDuration());
        QCOMPARE(restored.find(2)->generation, 3);
        QCOMPARE(restored.find(2)->age, child->age);
        QCOMPARE(restored.find(2)->affection, 12);
        QVERIFY(restored.find(2)->motionGroup.isEmpty());
        QVERIFY(!restored.find(2)->dispatchPaused);
        QVERIFY(restored.find(2)->area.contains(restored.find(2)->position));
        restored.spawn({200, 200}, {0, 0, 400, 400}, 1, 1);
        QVERIFY(restored.find(5)); // Deleted IDs remain consumed after restart.
        QVERIFY(!restored.find(4));
        QCOMPARE(store.loadAffinity(), 67);
        QCOMPARE(store.loadLimit(), 12);
    }
    void portableMalformedDataAndBackup() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QFile bad(dir.filePath("progress.json"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not json");
        bad.close();
        QString warning;
        QVERIFY(store.loadJson("progress.json", &warning).isEmpty());
        QVERIFY(!warning.isEmpty());
        QVERIFY(store.saveJson("progress.json", {{"count", 1}}));
        QFile backup(dir.filePath("progress.json.invalid"));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), QByteArray("not json"));
        QCOMPARE(store.loadJson("progress.json").value("count").toInt(), 1);
        QFile blocker(dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("keep");
        blocker.close();
        QVERIFY(!SettingsStore(blocker.fileName() + "/child").saveJson("config.json", {}, &warning));
        QVERIFY(!warning.isEmpty());
        SwarmController restored;
        restored.setLimit(2);
        QJsonArray pets{QJsonObject{{"id", "2"}, {"growth", 200}, {"affection", -8}},
                        QJsonObject{{"id", "2"}}, QJsonObject{{"id", "0"}},
                        QJsonObject{{"id", "3"}}, QJsonObject{{"id", "1"}, {"growth", 25}}};
        restored.restoreProgress({{"pets", pets}});
        QCOMPARE(restored.totalCount(), 2);
        QCOMPARE(restored.primary().growth(), 25.0);
        QVERIFY(restored.primary().juvenile);
        QCOMPARE(restored.find(2)->growth(), 100.0);
        QVERIFY(!restored.find(2)->juvenile);
        QCOMPARE(restored.find(2)->affection, 0);
        restored.restoreProgress({{"pets", QJsonArray{}}});
        QCOMPARE(restored.totalCount(), 1);
        QVERIFY(restored.primary().primary);
    }
    void growthDurationMigrationKeepsProgressAndAdults() {
        SwarmController swarm(14);
        const QJsonArray pets{
            QJsonObject{{"id", "1"}, {"growth", 25}, {"age", 5000}},
            QJsonObject{{"id", "2"}, {"growth", 75}, {"growthDuration", 2400}},
            QJsonObject{{"id", "3"}, {"growth", 100}, {"scale", .9}},
            QJsonObject{{"id", "4"}, {"growth", 10}, {"growthDuration", -1}},
            QJsonObject{{"id", "5"}, {"growth", 20}, {"growthDuration", 3601}},
            QJsonObject{{"id", "6"}, {"growth", 30}, {"growthDuration", "invalid"}}};
        swarm.restoreProgress({{"pets", pets}});
        QCOMPARE(swarm.primary().growth(), 25.0);
        QCOMPARE(swarm.primary().age, 5000.0);
        QCOMPARE(swarm.find(2)->growth(), 75.0);
        QCOMPARE(swarm.find(2)->growthDuration(), 2400.0);
        QCOMPARE(swarm.find(3)->growth(), 100.0);
        QCOMPARE(swarm.find(3)->scale, .9);
        QVERIFY(!swarm.find(3)->playFeeding());
        for (quint64 id : {1, 4, 5, 6}) {
            auto *p = swarm.find(id);
            QVERIFY(p->growthDuration() >= 1800 && p->growthDuration() <= 3600);
            const double before = p->growth();
            p->advance(1, {});
            QVERIFY(std::abs(p->growth() - before - 100 / p->growthDuration()) < 1e-9);
        }
        SwarmController restarted(99);
        restarted.restoreProgress(swarm.saveProgress());
        for (const auto &p : swarm.pets()) {
            QCOMPARE(restarted.find(p->id)->growth(), p->growth());
            QCOMPARE(restarted.find(p->id)->growthDuration(), p->growthDuration());
        }
        restarted.find(2)->advance(600, {});
        QCOMPARE(restarted.find(2)->growth(), 100.0);
    }
    void storageFailureAndRecovery() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("data");
        SettingsStore store(path);
        QString error;
        QCOMPARE(store.loadAffinity(), 0);
        QCOMPARE(store.loadLimit(), 72);
        QVERIFY(store.saveLimit(1, &error));
        QCOMPARE(store.loadLimit(), 1);
        QVERIFY(store.saveLimit(72, &error));
        QCOMPARE(store.loadLimit(), 72);
        QFile limitFile(path + "/population-limit.dat");
        QVERIFY(limitFile.open(QIODevice::WriteOnly));
        limitFile.write("0");
        limitFile.close();
        QCOMPARE(store.loadLimit(&error), 72);
        QVERIFY(!error.isEmpty());
        QVERIFY(store.saveAffinity(24, &error));
        QCOMPARE(store.loadAffinity(), 24);
        QVERIFY(store.saveAffinity(200, &error));
        QCOMPARE(store.loadAffinity(), 100);
        QFile blocker(dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("unchanged");
        blocker.close();
        SettingsStore invalid(blocker.fileName() + "/child");
        QVERIFY(!invalid.saveAffinity(42, &error));
        QVERIFY(!invalid.saveLimit(12, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("unchanged"));
        QFile corrupt(path + "/affinity.dat");
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("invalid");
        corrupt.close();
        error.clear();
        QCOMPARE(store.loadAffinity(&error), 0);
        QVERIFY(!error.isEmpty());
    }
    void singleInstanceRecallAndCrashRecovery() {
        QTemporaryDir dir;
        const QString executable = QCoreApplication::applicationDirPath() + "/instance_probe";
        QProcess first;
        first.start(executable, {dir.path()});
        QVERIFY(first.waitForStarted());
        QVERIFY(first.waitForReadyRead(5000));
        QCOMPARE(first.readLine(), QByteArray("READY\n"));
        QProcess second;
        second.start(executable, {dir.path()});
        QVERIFY(second.waitForFinished(7000));
        QCOMPARE(second.exitCode(), 0);
        QCOMPARE(second.readAllStandardOutput(), QByteArray("RECALLED\n"));
        first.kill();
        QVERIFY(first.waitForFinished());
        QProcess recovery;
        recovery.start(executable, {dir.path()});
        QVERIFY(recovery.waitForStarted());
        QVERIFY(recovery.waitForReadyRead(5000));
        QCOMPARE(recovery.readLine(), QByteArray("READY\n"));
        recovery.kill();
        QVERIFY(recovery.waitForFinished());
    }

  private:
    std::unique_ptr<AnimationLibrary> animation_;
};
QTEST_MAIN(TestPettime)
#include "TestPettime.moc"
