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
#include <limits>
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
    void pettingCompletesOnceAndPreservesBehavior() {
        for (bool primary : {false, true})
            for (bool juvenile : {false, true}) {
                PetModel p(8, {500, 500}, {0, 0, 1000, 1000}, juvenile);
                p.primary = primary;
                p.paused = p.menuOpen = true;
                const PetModel before = p;
                QCOMPARE(p.affection, 40);
                QVERIFY(p.startPetting());
                QVERIFY(!p.startPetting());
                QVERIFY(!p.playFeeding());
                p.demoPounce();
                p.crush();
                QCOMPARE(p.state, before.state);
                p.advance(1.99, {});
                QVERIFY(p.petting);
                QCOMPARE(p.pettingCompletions(), quint64(0));
                QCOMPARE(p.affection, 40);
                p.advance(.01, {});
                QVERIFY(!p.petting);
                QCOMPARE(p.pettingCompletions(), quint64(1));
                QVERIFY(p.lastPettingGain() >= 2 && p.lastPettingGain() <= 4);
                QCOMPARE(p.affection, 40 + p.lastPettingGain());
                QCOMPARE(p.state, before.state);
                QCOMPARE(p.actionAge, before.actionAge);
                QCOMPARE(p.position, before.position);
                QCOMPARE(p.age, 2.0);
                if (juvenile)
                    QVERIFY(std::abs(p.growth() - 2 * 100 / p.growthDuration()) < 1e-9);
                p.advance(.01, {});
                QCOMPARE(p.pettingCompletions(), quint64(1));
            }
    }
    void pettingCancellationFreezeAndCap() {
        PetModel p(6, {500, 500}, {0, 0, 1000, 1000}, true);
        QVERIFY(p.startPetting());
        p.advance(.5, {});
        const PetModel frozen = p;
        p.dispatchPaused = true;
        p.advance(100, {});
        QCOMPARE(p.pettingAge, frozen.pettingAge);
        QCOMPARE(p.age, frozen.age);
        QCOMPARE(p.growth(), frozen.growth());
        QCOMPARE(p.affectionDecayRemaining(), frozen.affectionDecayRemaining());
        QVERIFY(p.petting);
        p.dispatchPaused = false;
        p.advance(1.5, {});
        QCOMPARE(p.pettingCompletions(), quint64(1));
        p.affection = 99;
        QVERIFY(p.startPetting());
        p.advance(2, {});
        QCOMPARE(p.affection, 100);
        QCOMPARE(p.lastPettingGain(), 1);
        QCOMPARE(p.pettingCompletions(), quint64(2));
        QVERIFY(p.startPetting());
        p.advance(2, {});
        QCOMPARE(p.affection, 100);
        QCOMPARE(p.lastPettingGain(), 0);
        QCOMPARE(p.pettingCompletions(), quint64(3));
        p.becomeNymph();
        QCOMPARE(p.pettingCompletions(), quint64(3));
        QCOMPARE(p.lastPettingGain(), 0);
        for (int cancellation = 0; cancellation < 5; ++cancellation) {
            PetModel cancelled(7, {500, 500}, {0, 0, 1000, 1000}, true);
            cancelled.primary = false;
            QVERIFY(cancelled.startPetting());
            cancelled.advance(.5, {});
            if (cancellation == 0)
                cancelled.retire();
            else if (cancellation == 1)
                QVERIFY(cancelled.beginExit());
            else if (cancellation == 2)
                cancelled.beginEntry(cancelled.area, 0, .5);
            else if (cancellation == 3)
                cancelled.expired = true;
            else
                cancelled.splitReady = true;
            cancelled.advance(3, {});
            QVERIFY(!cancelled.petting);
            QCOMPARE(cancelled.affection, 40);
            QCOMPARE(cancelled.pettingCompletions(), quint64(0));
        }
    }
    void pettingRejectsConflictingActions() {
        for (int condition = 0; condition < 11; ++condition) {
            PetModel p(7, {500, 500}, {0, 0, 1000, 1000});
            p.primary = false;
            switch (condition) {
            case 0: p.expired = true; break;
            case 1: p.splitReady = true; break;
            case 2: p.dispatchPaused = true; break;
            case 3: p.dispatchLocked = true; break;
            case 4: p.beginEntry(p.area, 0, .5); break;
            case 5: p.mealActive = true; break;
            case 6: p.eating = true; break;
            case 7: p.cosmeticFeeding = true; break;
            case 8: p.demoPounce(); QVERIFY(p.frontal()); break;
            case 9: p.dragging = true; break;
            case 10: p.retire(); break;
            }
            QVERIFY(!p.startPetting());
            QCOMPARE(p.pettingCompletions(), quint64(0));
        }
        PetModel feeding(7, {500, 500}, {0, 0, 1000, 1000}, true);
        QVERIFY(feeding.playFeeding());
        QVERIFY(!feeding.startPetting());
    }
    void affectionRandomRangesAndIndependentRng() {
        QSet<int> gains, intervals, losses;
        for (int seed = 1; seed <= 180; ++seed) {
            PetModel p(seed, {500, 500}, {0, 0, 1000, 1000}, true);
            PetModel q = p;
            QVERIFY(p.affectionDecayRemaining() >= 30 && p.affectionDecayRemaining() <= 60);
            intervals.insert(int(p.affectionDecayRemaining()));
            for (int i = 0; i < seed; ++i)
                q.random(0, 1);
            QVERIFY(q.playFeeding());
            q.advance(2.65, {}); // Consume growth RNG independently of affection RNG.
            QVERIFY(p.startPetting());
            QVERIFY(q.startPetting());
            p.advance(2, {});
            q.advance(2, {});
            QCOMPARE(q.lastPettingGain(), p.lastPettingGain());
            QCOMPARE(q.affectionDecayRemaining(), p.affectionDecayRemaining());
            gains.insert(p.lastPettingGain());
            PetModel decaying(seed, {500, 500}, {0, 0, 1000, 1000});
            decaying.paused = true;
            while (decaying.affectionDecayRemaining() > 1)
                decaying.advance(1, {});
            decaying.advance(decaying.affectionDecayRemaining(), {});
            const int loss = 40 - decaying.affection;
            QVERIFY(loss >= 1 && loss <= 2);
            losses.insert(loss);
            QVERIFY(decaying.affectionDecayRemaining() >= 30 &&
                    decaying.affectionDecayRemaining() <= 60);
        }
        QCOMPARE(gains.size(), 3);
        QCOMPARE(losses.size(), 2);
        QVERIFY(intervals.size() > 1);
    }
    void affectionClocksUseActiveTimeAndIgnoreLongGaps() {
        SwarmController swarm(8);
        QVERIFY(swarm.reproduce(1));
        auto &p = swarm.primary();
        const double decay = p.affectionDecayRemaining();
        const double reproduction = p.reproductionRemaining();
        p.paused = p.menuOpen = true;
        p.advance(.5, {});
        QCOMPARE(p.affectionDecayRemaining(), decay - .5);
        QCOMPARE(p.reproductionRemaining(), reproduction - .5);
        const PetModel beforeGap = p;
        for (double dt : {0.0, -1.0, 1.001, 3600.0,
                          std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
            p.advance(dt, {});
        QCOMPARE(p.affectionDecayRemaining(), beforeGap.affectionDecayRemaining());
        QCOMPARE(p.reproductionRemaining(), beforeGap.reproductionRemaining());
        for (int condition = 0; condition < 4; ++condition) {
            PetModel frozen = beforeGap;
            frozen.primary = false;
            if (condition == 0) frozen.dispatchPaused = true;
            if (condition == 1) frozen.expired = true;
            if (condition == 2) frozen.splitReady = true;
            if (condition == 3) QVERIFY(frozen.beginExit());
            frozen.advance(.5, {});
            QCOMPARE(frozen.affectionDecayRemaining(), beforeGap.affectionDecayRemaining());
            QCOMPARE(frozen.reproductionRemaining(), beforeGap.reproductionRemaining());
        }
        p.affection = 100;
        for (int i = 0; i < 5000; ++i) {
            p.advance(1, {});
            QVERIFY(p.affection >= 0 && p.affection <= 100);
        }
        QCOMPARE(p.affection, 0);
        QCOMPARE(p.reproductionRemaining(), 0.0);
        QVERIFY(p.canReproduce());
    }
    void directReproductionPreservesParentsAndHasIndependentCooldowns() {
        SwarmController swarm(20);
        swarm.setLimit(4);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 3, 1);
        auto *p = swarm.find(2);
        p->advance(p->growthDuration(), {});
        p->customName = "亲体";
        p->affection = 72;
        const auto address = p;
        QVERIFY(swarm.canReproduce(1));
        QVERIFY(swarm.canReproduce(2));
        QVERIFY(swarm.reproduce(2));
        QCOMPARE(swarm.find(2), address);
        QCOMPARE(p->customName, QString("亲体"));
        QCOMPARE(p->growth(), 100.0);
        QCOMPARE(p->generation, 3);
        QCOMPARE(p->affection, 72);
        QCOMPARE(swarm.totalCount(), 3);
        QCOMPARE(swarm.pendingCount(), std::size_t(0));
        auto *child = swarm.find(3);
        QVERIFY(child && child->juvenile);
        QCOMPARE(child->growth(), 0.0);
        QCOMPARE(child->generation, 4);
        QCOMPARE(child->affection, 40);
        QCOMPARE(child->reproductionRemaining(), 0.0);
        QVERIFY(p->reproductionRemaining() >= 600 && p->reproductionRemaining() <= 1200);
        QCOMPARE(swarm.primary().reproductionRemaining(), 0.0);
        const double cooldown = p->reproductionRemaining();
        QVERIFY(!swarm.reproduce(2));
        QCOMPARE(p->reproductionRemaining(), cooldown);
        QVERIFY(swarm.reproduce(1));
        QCOMPARE(swarm.totalCount(), 4);
        QVERIFY(swarm.primary().reproductionRemaining() >= 600 &&
                swarm.primary().reproductionRemaining() <= 1200);
        QCOMPARE(p->reproductionRemaining(), cooldown);
        QVERIFY(!swarm.canReproduce(999));
    }
    void reproductionFailureDoesNotRetireOrStartCooldown() {
        SwarmController full(5);
        full.setLimit(2);
        full.spawn({500, 500}, {0, 0, 1000, 1000}, 1, 1);
        const auto before = full.saveProgress();
        QVERIFY(!full.canReproduce(1));
        QVERIFY(!full.reproduce(1));
        QCOMPARE(full.saveProgress(), before);
        QVERIFY(full.find(2)->active());
        SwarmController pending(5);
        pending.setLimit(3);
        pending.spawn({500, 500}, {0, 0, 1000, 1000}, 1, 2);
        pending.spawn({500, 500}, {0, 0, 1000, 1000}, 2, 1);
        QCOMPARE(pending.pendingCount(), std::size_t(1));
        QVERIFY(!pending.reproduce(1));
        QCOMPARE(pending.primary().reproductionRemaining(), 0.0);
        QCOMPARE(pending.pendingCount(), std::size_t(1));
        pending.find(2)->expired = true;
        QVERIFY(pending.removeLocalPet(2)); // Capacity alone must not bypass a pending birth.
        QCOMPARE(pending.totalCount(), 2);
        QVERIFY(!pending.canReproduce(1));
        QVERIFY(!pending.reproduce(1));
        QCOMPARE(pending.primary().reproductionRemaining(), 0.0);
    }
    void reproductionRechecksModelActionAndOwnership() {
        for (int condition = 0; condition < 13; ++condition) {
            SwarmController swarm(7);
            auto &p = swarm.primary();
            switch (condition) {
            case 0: p.becomeNymph(); break;
            case 1: p.motionGroup = "remote"; break;
            case 2: p.dispatchPaused = true; break;
            case 3: p.dispatchLocked = true; break;
            case 4: p.expired = true; break;
            case 5: p.splitReady = true; break;
            case 6: p.mealActive = true; break;
            case 7: p.eating = true; break;
            case 8: p.cosmeticFeeding = true; break;
            case 9: p.demoPounce(); break;
            case 10: p.dragging = true; break;
            case 11: QVERIFY(p.startPetting()); break;
            case 12: p.crush(); break;
            }
            QVERIFY(!p.canReproduce());
            QVERIFY(!swarm.canReproduce(1));
            QVERIFY(!swarm.reproduce(1));
            QCOMPARE(p.reproductionRemaining(), 0.0);
            QCOMPARE(swarm.totalCount(), 1);
        }
        SwarmController transitioning(7);
        transitioning.spawn({500, 500}, {0, 0, 1000, 1000}, 1, 1);
        auto *p = transitioning.find(2);
        p->advance(p->growthDuration(), {});
        p->beginEntry(p->area, 0, .5);
        QVERIFY(!transitioning.reproduce(2));
        QCOMPARE(p->reproductionRemaining(), 0.0);
    }
    void juvenileAndRemotePetsCannotCrushOrSplit() {
        SwarmController swarm(7);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 2, 1);
        auto *p = swarm.find(2);
        p->crush();
        QVERIFY(p->active());
        QVERIFY(!p->splitReady);
        swarm.advance(2);
        QCOMPARE(swarm.totalCount(), 2);
        QCOMPARE(swarm.find(2), p);
        p->advance(p->growthDuration(), {});
        p->motionGroup = "remote";
        p->crush();
        QVERIFY(p->active());
        QVERIFY(!p->splitReady);
        p->motionGroup.clear();
        p->crush();
        QCOMPARE(p->state, State::Crush);
        swarm.advance(1.1);
        QCOMPARE(swarm.totalCount(), 5);
        QVERIFY(!swarm.find(2));
        for (int condition = 0; condition < 9; ++condition) {
            PetModel blocked(7, {500, 500}, {0, 0, 1000, 1000});
            blocked.primary = false;
            switch (condition) {
            case 0: blocked.cosmeticFeeding = true; break;
            case 1: blocked.mealActive = true; break;
            case 2: blocked.eating = true; break;
            case 3: blocked.demoPounce(); break;
            case 4: blocked.dispatchPaused = true; break;
            case 5: blocked.dispatchLocked = true; break;
            case 6: blocked.expired = true; break;
            case 7: blocked.splitReady = true; break;
            case 8: blocked.beginEntry(blocked.area, 0, .5); break;
            }
            const auto state = blocked.state;
            blocked.crush();
            QCOMPARE(blocked.state, state);
        }
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
        QCOMPARE(p.affection, 40);
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
            QCOMPARE(p.affection, 40);
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
        p.retire();
        p.advance(.1, {});
        QCOMPARE(p.growth(), natural);
        QCOMPARE(p.feedingCompletions(), quint64(0));
        p.becomeNymph();
        QVERIFY(p.playFeeding());
        p.advance(2.65, {});
        QCOMPARE(p.feedingCompletions(), quint64(1));
        QCOMPARE(p.affection, 40);
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
        QCOMPARE(swarm.totalCount(), 5); // The adult parent is replaced by four children.
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
        SwarmController swarm(12);
        auto &main = swarm.primary();
        QVERIFY(!swarm.splitPrimary());
        main.crush();
        main.advance(1.06, {});
        QVERIFY(main.splitReady);
        QVERIFY(swarm.splitPrimary());
        QVERIFY(!swarm.splitPrimary());
        QCOMPARE(swarm.totalCount(), 4);
        QCOMPARE(main.growth(), 0.0);
        for (int round = 0; round < 12; ++round) {
            for (const auto &p : swarm.pets()) {
                if (p->juvenile) {
                    p->paused = true;
                    p->advance(p->growthDuration(), {});
                }
                p->crush();
            }
            main.advance(1.1, {});
            QVERIFY(swarm.splitPrimary());
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
            if (primary->juvenile) {
                primary->paused = true;
                primary->advance(primary->growthDuration(), {});
            }
            primary->crush();
            primary->advance(1.1, {});
            QVERIFY(primary->splitReady);
            QVERIFY(swarm.splitPrimary());
            swarm.advance(2);
            QCOMPARE(swarm.totalCount(), 1);
            QCOMPARE(&swarm.primary(), primary);
            QCOMPARE(primary->affection, 45);
        }
        swarm.setLimit(2);
        swarm.spawn(primary->position, primary->area, 1, 12);
        QVERIFY(swarm.pets().back()->id > 72);
        for (int i = 0; i < 15; ++i) {
            auto *child = swarm.pets().back().get();
            child->advance(child->growthDuration(), {});
            child->crush();
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
            p.paused = true;
            p.advance(p.growthDuration(), {});
            p.crush();
            QCOMPARE(p.state, State::Crush);
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
        QCOMPARE(store.loadAffinity(), 40);
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
            QCOMPARE(p.affection, 40);
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
    void affectionAndReproductionProgressRoundTripAndMigration() {
        SwarmController source(17);
        QVERIFY(source.reproduce(1));
        source.primary().paused = true;
        source.primary().advance(.75, {});
        const double cooldown = source.primary().reproductionRemaining();
        const double decay = source.primary().affectionDecayRemaining();
        source.primary().becomeNymph();
        QCOMPARE(source.primary().reproductionRemaining(), cooldown);
        QCOMPARE(source.primary().affectionDecayRemaining(), decay);
        const auto json = QJsonDocument::fromJson(
            QJsonDocument(source.saveProgress()).toJson()).object();
        SwarmController restored(92);
        restored.restoreProgress(json);
        QCOMPARE(restored.primary().reproductionRemaining(), cooldown);
        QCOMPARE(restored.primary().affectionDecayRemaining(), decay);
        QCOMPARE(restored.find(2)->reproductionRemaining(), 0.0);
        QCOMPARE(restored.find(2)->affectionDecayRemaining(),
                 source.find(2)->affectionDecayRemaining());
        QCOMPARE(restored.find(2)->affection, 40);
        restored.primary().advance(3600, {}); // Offline or suspended time does not count.
        QCOMPARE(restored.primary().reproductionRemaining(), cooldown);
        QCOMPARE(restored.primary().affectionDecayRemaining(), decay);
        restored.primary().advance(.25, {});
        QCOMPARE(restored.primary().reproductionRemaining(), cooldown - .25);
        QCOMPARE(restored.primary().affectionDecayRemaining(), decay - .25);
        QJsonArray oldPets{
            QJsonObject{{"id", "1"}, {"affection", 0}},
            QJsonObject{{"id", "2"}},
            QJsonObject{{"id", "3"}, {"affection", "invalid"},
                        {"affectionDecayRemaining", -1}, {"reproductionRemaining", 1201}},
            QJsonObject{{"id", "4"}, {"affection", 100},
                        {"affectionDecayRemaining", 0}, {"reproductionRemaining", 0}},
            QJsonObject{{"id", "5"}, {"affectionDecayRemaining", 60.1},
                        {"reproductionRemaining", "invalid"}}};
        restored.restoreProgress({{"pets", oldPets}});
        QCOMPARE(restored.primary().affection, 0);
        QCOMPARE(restored.find(2)->affection, 40);
        QCOMPARE(restored.find(3)->affection, 40);
        QCOMPARE(restored.find(4)->affection, 100);
        QCOMPARE(restored.find(4)->affectionDecayRemaining(), 0.0);
        for (quint64 id : {1, 2, 3, 5}) {
            const auto *p = restored.find(id);
            QVERIFY(p->affectionDecayRemaining() >= 30 && p->affectionDecayRemaining() <= 60);
            QCOMPARE(p->reproductionRemaining(), 0.0);
        }
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QCOMPARE(store.loadAffinity(), 40);
        QVERIFY(store.saveAffinity(0));
        QCOMPARE(store.loadAffinity(), 0);
        QVERIFY(store.saveJson("progress.json", {{"pets", QJsonArray{QJsonObject{{"id", "1"}}}}}));
        QCOMPARE(store.loadAffinity(), 40);
        QVERIFY(store.saveJson("progress.json",
                              {{"pets", QJsonArray{QJsonObject{{"id", 1}, {"affection", 0}}}}}));
        QCOMPARE(store.loadAffinity(), 0);
        QVERIFY(store.saveJson("progress.json", {}));
        QCOMPARE(store.loadAffinity(), 40);
    }
    void reproductionCooldownRangeAndIndependentRng() {
        QSet<int> cooldowns;
        for (int seed = 1; seed <= 80; ++seed) {
            SwarmController first(seed), second(seed);
            for (int i = 0; i < seed; ++i)
                second.primary().random(0, 1);
            second.primary().becomeNymph();
            QVERIFY(second.primary().playFeeding());
            second.primary().advance(2.65, {});
            second.primary().paused = true;
            second.primary().advance(second.primary().growthDuration(), {});
            QVERIFY(first.reproduce(1));
            QVERIFY(second.reproduce(1));
            const double remaining = first.primary().reproductionRemaining();
            QVERIFY(remaining >= 600 && remaining <= 1200);
            QCOMPARE(second.primary().reproductionRemaining(), remaining);
            cooldowns.insert(int(remaining));
            QCOMPARE(first.find(2)->reproductionRemaining(), 0.0);
        }
        QVERIFY(cooldowns.size() > 1);
    }
    void storageFailureAndRecovery() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("data");
        SettingsStore store(path);
        QString error;
        QCOMPARE(store.loadAffinity(), 40);
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
        QCOMPARE(store.loadAffinity(&error), 40);
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
