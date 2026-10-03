#include "ApplicationController.h"
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

namespace pettime {
class InteractionApplicationTest : public QObject {
    Q_OBJECT
    std::unique_ptr<AnimationLibrary> animation_;
    static unsigned rewardSeed(bool win) {
        for (unsigned seed = 1; ; ++seed) {
            std::mt19937 rng(seed);
            if ((std::uniform_int_distribution<int>(1, 10)(rng) == 1) == win)
                return seed;
        }
    }
    static void advanceApplication(ApplicationController &app, double seconds) {
        while (seconds > 1e-9) {
            const double dt = std::min(seconds, .5);
            app.last_ = app.clock_.nsecsElapsed() / 1e9 - dt;
            app.tick();
            seconds -= dt;
        }
    }
  private slots:
    void initTestCase() { animation_ = std::make_unique<AnimationLibrary>(); }
    void panelTargetsPetAndSharesCooldown() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 3;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        app.petTable_->selectRow(1);
        app.interactionPet_->click();
        QCOMPARE(app.interactionTarget_->currentData().toULongLong(), quint64(2));
        QCOMPARE(app.interactionAction_->count(), 2);
        QCOMPARE(app.primary_.affection, 40);
        auto *p = app.swarm_.find(2);
        app.rewardRng_.seed(rewardSeed(false));
        app.interactionUse_->click();
        QVERIFY(p->petting);
        QCOMPARE(app.interactionCooldown_, 10.0);
        QVERIFY(!app.useFood(2));
        QVERIFY(!app.interact(3, ApplicationController::Interaction::Petting));
        p->advance(2, {});
        QVERIFY(!p->petting);
        QVERIFY(p->affection >= 42 && p->affection <= 44);
        QCOMPARE(app.swarm_.find(3)->affection, 40);
        app.persist();
        QCOMPARE(app.food_.count(), 3);
        QVERIFY(app.pettingSerials_.isEmpty());
        app.showInteractions(3);
        QVERIFY(!app.interactionUse_->isEnabled());
        advanceApplication(app, 10.1);
        app.refreshInteractions();
        QVERIFY(app.interactionUse_->isEnabled());
        app.interactionUse_->click();
        QVERIFY(app.swarm_.find(3)->petting);
    }
    void completionRewardCommitsOnceWithAffection() {
        QTemporaryDir dir;
        int affection = 0;
        {
            ApplicationController app(*animation_, SettingsStore(dir.path()), {});
            app.start();
            app.timer_.stop();
            app.rewardRng_.seed(rewardSeed(true));
            QVERIFY(app.interact(1, ApplicationController::Interaction::Petting));
            app.primary_.advance(2, {});
            affection = app.primary_.affection;
            QVERIFY(affection >= 42 && affection <= 44);
            app.persist();
            QCOMPARE(app.food_.count(), 4);
            app.persist();
            QCOMPARE(app.food_.count(), 4);
            const auto saved = app.store_.loadJson("progress.json");
            QCOMPARE(saved["food"].toObject()["count"].toInt(), 4);
            QCOMPARE(saved["pets"].toArray()[0].toObject()["affection"].toInt(), affection);
            QCOMPARE(saved["interaction"].toObject()["cooldownRemaining"].toDouble(), 10.0);
        }
        ApplicationController restored(*animation_, SettingsStore(dir.path()), {});
        QCOMPARE(restored.primary_.affection, affection);
        QCOMPARE(restored.food_.count(), 4);
        QCOMPARE(restored.interactionCooldown_, 10.0);
        QVERIFY(!restored.primary_.petting);
        QVERIFY(restored.pettingSerials_.isEmpty());
        QVERIFY(!restored.interact(1, ApplicationController::Interaction::Petting));
    }
    void unfinishedRestartCancelsWithoutRewardAndRetainsCooldown() {
        QTemporaryDir dir;
        {
            ApplicationController app(*animation_, SettingsStore(dir.path()), {});
            app.start();
            app.timer_.stop();
            app.rewardRng_.seed(rewardSeed(true));
            QVERIFY(app.interact(1, ApplicationController::Interaction::Petting));
            advanceApplication(app, .5);
            QVERIFY(app.primary_.petting);
        }
        ApplicationController restored(*animation_, SettingsStore(dir.path()), {});
        QCOMPARE(restored.primary_.affection, 40);
        QCOMPARE(restored.food_.count(), 3);
        QVERIFY(restored.interactionCooldown_ > 9 && restored.interactionCooldown_ <= 9.5);
        QVERIFY(!restored.primary_.petting);
    }
    void frozenCancelledAndDeletedInteractions() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 4;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.rewardRng_.seed(rewardSeed(true));
        auto *p = app.swarm_.find(2);
        QVERIFY(app.interact(2, ApplicationController::Interaction::Petting));
        p->dispatchPaused = true;
        advanceApplication(app, 10.1);
        QVERIFY(p->petting);
        QCOMPARE(p->pettingAge, 0.0);
        QCOMPARE(p->affection, 40);
        QCOMPARE(app.food_.count(), 3);
        QVERIFY(!app.interact(2, ApplicationController::Interaction::Petting));
        p->dispatchPaused = false;
        QVERIFY(p->beginExit());
        app.persist();
        QVERIFY(!p->petting);
        QCOMPARE(app.food_.count(), 3);
        QVERIFY(app.interact(3, ApplicationController::Interaction::Petting));
        QVERIFY(app.clearPet(3));
        QCOMPARE(app.food_.count(), 3);
        advanceApplication(app, 10.1);
        QVERIFY(app.interact(4, ApplicationController::Interaction::Petting));
        app.swarm_.find(4)->advance(2, {});
        QVERIFY(app.clearPet(4));
        QCOMPARE(app.food_.count(), 4); // Completion is settled before removing its model.
        app.persist();
        QCOMPARE(app.food_.count(), 4);
    }
    void fullAffectionStillRollsAndFullStockIncludesReservations() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 2;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.primary_.affection = 100;
        app.rewardRng_.seed(rewardSeed(true));
        QVERIFY(app.interact(1, ApplicationController::Interaction::Petting));
        app.primary_.advance(2, {});
        app.persist();
        QCOMPARE(app.primary_.affection, 100);
        QCOMPARE(app.food_.count(), 4);
        app.food_.grantInteractionReward(30);
        QVERIFY(app.useFood(2));
        QCOMPARE(app.food_.count(), 29);
        QCOMPARE(app.food_.reservedCount(), 1);
        // Keep this reservation frozen while the shared interaction cooldown elapses.
        app.swarm_.find(2)->dispatchPaused = true;
        advanceApplication(app, 10.1);
        app.rewardRng_.seed(rewardSeed(true));
        QVERIFY(app.interact(1, ApplicationController::Interaction::Petting));
        app.primary_.advance(2, {});
        app.persist();
        QCOMPARE(app.food_.count(), 29);
        QCOMPARE(app.food_.reservedCount(), 1);
        QVERIFY(app.lastInteractionResult_.contains("库存已满"));
    }
    void reproduceKeepsParentPersistsChildAndCooldown() {
        QTemporaryDir dir;
        double cooldown = 0;
        {
            ApplicationController app(*animation_, SettingsStore(dir.path()), {});
            app.start();
            app.timer_.stop();
            app.showInteractions(1);
            app.interactionAction_->setCurrentIndex(1);
            QVERIFY(app.interactionUse_->isEnabled());
            const auto parent = &app.primary_;
            app.interactionUse_->click();
            QCOMPARE(&app.primary_, parent);
            QCOMPARE(app.swarm_.totalCount(), 2);
            QCOMPARE(app.primary_.growth(), 100.0);
            QCOMPARE(app.primary_.affection, 40);
            QCOMPARE(app.food_.count(), 3);
            QCOMPARE(app.swarm_.find(2)->growth(), 0.0);
            QCOMPARE(app.swarm_.find(2)->affection, 40);
            cooldown = app.primary_.reproductionRemaining();
            QVERIFY(cooldown >= 600 && cooldown <= 1200);
            QVERIFY(!app.interactionUse_->isEnabled());
            QVERIFY(!app.interact(1, ApplicationController::Interaction::Reproduce));
            app.showInteractions(2);
            QVERIFY(!app.interactionUse_->isEnabled());
        }
        ApplicationController restored(*animation_, SettingsStore(dir.path()), {});
        QCOMPARE(restored.swarm_.totalCount(), 2);
        QCOMPARE(restored.primary_.reproductionRemaining(), cooldown);
        QCOMPARE(restored.swarm_.find(2)->growth(), 0.0);
    }
    void reproduceRejectsFullRemoteAndConflictingActions() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 2;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.swarm_.setLimit(2);
        QVERIFY(!app.interact(1, ApplicationController::Interaction::Reproduce));
        QCOMPARE(app.primary_.reproductionRemaining(), 0.0);
        QCOMPARE(app.swarm_.totalCount(), 2);
        auto *p = app.swarm_.find(2);
        p->advance(p->growthDuration(), {});
        app.swarm_.setLimit(3);
        p->motionGroup = "remote";
        QVERIFY(!app.interact(2, ApplicationController::Interaction::Reproduce));
        QVERIFY(app.interact(2, ApplicationController::Interaction::Petting));
        QVERIFY(!app.interact(2, ApplicationController::Interaction::Reproduce));
        p->advance(2, {});
        app.persist();
        p->motionGroup.clear();
        p->dispatchPaused = true;
        QVERIFY(!app.interact(2, ApplicationController::Interaction::Reproduce));
        p->dispatchPaused = false;
        QVERIFY(app.interact(2, ApplicationController::Interaction::Reproduce));
        QCOMPARE(app.swarm_.totalCount(), 3);
        QCOMPARE(app.swarm_.find(2), p);
        QCOMPARE(p->growth(), 100.0);
    }
    void bottomPauseAndLongGapTiming() {
        QTemporaryDir dir;
        ApplicationController app(*animation_, SettingsStore(dir.path()), {});
        app.start();
        app.timer_.stop();
        app.setBottomMode(true);
        app.primary_.paused = true;
        QVERIFY(app.interact(1, ApplicationController::Interaction::Reproduce));
        QVERIFY(app.interact(1, ApplicationController::Interaction::Petting));
        app.showInteractions(1);
        QVERIFY(!app.interactionDialog_->windowFlags().testFlag(Qt::WindowTransparentForInput));
        const double reproduction = app.primary_.reproductionRemaining();
        const double decay = app.primary_.affectionDecayRemaining();
        app.last_ = app.clock_.nsecsElapsed() / 1e9 - 3600;
        app.tick();
        QCOMPARE(app.primary_.pettingAge, 0.0);
        QCOMPARE(app.interactionCooldown_, 10.0);
        QCOMPARE(app.primary_.reproductionRemaining(), reproduction);
        QCOMPARE(app.primary_.affectionDecayRemaining(), decay);
        advanceApplication(app, 61);
        QCOMPARE(app.interactionCooldown_, 0.0);
        QVERIFY(!app.primary_.petting);
        QVERIFY(app.primary_.reproductionRemaining() < reproduction - 60);
        QVERIFY(app.primary_.affection < 40 + app.primary_.lastPettingGain());
        QVERIFY(app.mainWindow_.bottomMode());
        QVERIFY(app.primary_.paused);
    }
};
} // namespace pettime
QTEST_MAIN(pettime::InteractionApplicationTest)
#include "TestInteractionApplication.moc"
