#include "ApplicationController.h"
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

namespace pettime {
class FoodApplicationTest : public QObject {
    Q_OBJECT
    std::unique_ptr<AnimationLibrary> animation_;
  private slots:
    void initTestCase() { animation_ = std::make_unique<AnimationLibrary>(); }
    void panelUsesInventoryAndSettlesExactlyOnce() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 3;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        app.showPets();
        app.petTable_->selectRow(1);
        app.foodPet_->click();
        QCOMPARE(app.foodTarget_->currentData().toULongLong(), quint64(2));
        QCOMPARE(app.food_.count(), 3);
        for (auto *action : app.menu_.actions())
            QVERIFY(!action->text().contains("投喂"));
        auto *p = app.swarm_.find(2);
        const double before = p->growth();
        app.foodUse_->click();
        QVERIFY(p->cosmeticFeeding);
        QCOMPARE(app.food_.count(), 2);
        QCOMPARE(app.food_.reservedCount(), 1);
        QVERIFY(!app.useFood(2));
        p->advance(2.65, {});
        const double bonus = p->growth() - before - 2.65 * 100 / p->growthDuration();
        QVERIFY(bonus >= .5 - 1e-9 && bonus <= 1.5 + 1e-9);
        app.persist();
        QCOMPARE(app.food_.reservedCount(), 0);
        QCOMPARE(app.food_.count(), 2);
        const auto growth = p->growth();
        app.persist();
        QCOMPARE(p->growth(), growth);
        QCOMPARE(app.food_.count(), 2);
        QCOMPARE(app.swarm_.find(3)->growth(), 0.0);
        QCOMPARE(app.primary_.growth(), 100.0);
        app.showFood(1);
        QVERIFY(!app.foodUse_->isEnabled());
        QVERIFY(!app.useFood(1));
        QCOMPARE(app.food_.count(), 2);
        const auto saved = app.store_.loadJson("progress.json");
        QCOMPARE(saved["food"].toObject()["count"].toInt(), 2);
        QVERIFY(saved["food"].toObject()["pending"].toArray().isEmpty());
    }
    void unfinishedFoodRefundsOnRestartWhileProgressIsPreserved() {
        QTemporaryDir dir;
        double savedGrowth = 0, duration = 0;
        {
            RunOptions options;
            options.population = 2;
            ApplicationController app(*animation_, SettingsStore(dir.path()), options);
            app.start();
            app.timer_.stop();
            app.primary_.affection = 42;
            QVERIFY(app.useFood(2));
            auto *p = app.swarm_.find(2);
            p->advance(1, {});
            savedGrowth = p->growth();
            duration = p->growthDuration();
            app.persist();
            const auto food = app.store_.loadJson("progress.json")["food"].toObject();
            QCOMPARE(food["count"].toInt(), 2);
            QCOMPARE(food["pending"].toArray().size(), 1);
        }
        {
            ApplicationController app(*animation_, SettingsStore(dir.path()), {});
            QCOMPARE(app.food_.count(), 3);
            QCOMPARE(app.food_.reservedCount(), 0);
            QCOMPARE(app.swarm_.totalCount(), 2);
            const auto *p = app.swarm_.find(2);
            QCOMPARE(p->growth(), savedGrowth);
            QCOMPARE(p->growthDuration(), duration);
            QVERIFY(!p->cosmeticFeeding);
            QCOMPARE(app.primary_.affection, 42);
            app.start();
            app.timer_.stop();
        }
        ApplicationController again(*animation_, SettingsStore(dir.path()), {});
        QCOMPARE(again.food_.count(), 3);
        QCOMPARE(again.swarm_.find(2)->growth(), savedGrowth);
    }
    void completedFoodPersistsWithGrowthWithoutRefund() {
        QTemporaryDir dir;
        double growth = 0;
        {
            RunOptions options;
            options.population = 2;
            ApplicationController app(*animation_, SettingsStore(dir.path()), options);
            app.start();
            app.timer_.stop();
            QVERIFY(app.useFood(2));
            app.swarm_.find(2)->advance(2.65, {});
            growth = app.swarm_.find(2)->growth();
            // Destructor must settle completed food before its atomic progress save.
        }
        ApplicationController restored(*animation_, SettingsStore(dir.path()), {});
        QCOMPARE(restored.food_.count(), 2);
        QCOMPARE(restored.food_.reservedCount(), 0);
        QCOMPARE(restored.swarm_.find(2)->growth(), growth);
    }
    void cancellationDeletionAndFreezeHaveDistinctOutcomes() {
        QTemporaryDir dir;
        RunOptions options;
        options.population = 4;
        ApplicationController app(*animation_, SettingsStore(dir.path()), options);
        app.start();
        app.timer_.stop();
        QVERIFY(app.useFood(2));
        auto *p = app.swarm_.find(2);
        p->dispatchPaused = true;
        app.swarm_.advance(10);
        app.persist();
        QVERIFY(p->cosmeticFeeding);
        QCOMPARE(p->growth(), 0.0);
        QCOMPARE(app.food_.reservedCount(), 1);
        QVERIFY(!app.useFood(2));
        p->dispatchPaused = false;
        QVERIFY(p->beginExit());
        app.persist();
        QCOMPARE(app.food_.count(), 3);
        QCOMPARE(app.food_.reservedCount(), 0);
        QCOMPARE(p->growth(), 0.0);
        QVERIFY(app.useFood(3));
        QVERIFY(app.clearPet(3));
        QCOMPARE(app.food_.count(), 3);
        QCOMPARE(app.food_.reservedCount(), 0);
        QVERIFY(app.useFood(4));
        auto *completed = app.swarm_.find(4);
        completed->advance(2.65, {});
        QVERIFY(app.clearPet(4));
        QCOMPARE(app.food_.count(), 2); // Deleting a pet after eating cannot refund spent food.
        QCOMPARE(app.food_.reservedCount(), 0);
    }
    void naturalDropWorksInBottomModeWithoutInteractionRewards() {
        QTemporaryDir dir;
        ApplicationController app(*animation_, SettingsStore(dir.path()), {});
        app.start();
        app.timer_.stop();
        app.setBottomMode(true);
        QVERIFY(app.mainWindow_.bottomMode());
        QCOMPARE(app.food_.count(), 3);
        for (int i = 0; i < 8; ++i) {
            app.recall();
            app.setBottomMode(true);
            app.showFood(1);
        }
        QCOMPARE(app.food_.count(), 3);
        const auto remaining = app.food_.dropRemaining();
        // A delayed event loop after suspension must not catch up drops.
        app.last_ = app.clock_.nsecsElapsed() / 1e9 - 3600;
        app.tick();
        QCOMPARE(app.food_.count(), 3);
        QCOMPARE(app.food_.dropRemaining(), remaining);
        // Use the persisted countdown to exercise the application's regular tick path.
        app.food_.restore({{"count", 3}, {"dropRemaining", .05}});
        app.last_ = app.clock_.nsecsElapsed() / 1e9 - .1;
        app.tick();
        QCOMPARE(app.food_.count(), 4);
        QVERIFY(app.mainWindow_.bottomMode());
        QCOMPARE(app.store_.loadJson("progress.json")["food"].toObject()["count"].toInt(), 4);
    }
    void bottomSettingRestoresAndNewPetsFollowIt() {
        QTemporaryDir dir;
        {
            RunOptions options;
            options.population = 2;
            ApplicationController app(*animation_, SettingsStore(dir.path()), options);
            app.start();
            app.timer_.stop();
            app.setBottomMode(true);
            QVERIFY(app.mainWindow_.bottomMode());
            QVERIFY(app.crack_.bottomMode());
            QVERIFY(app.windows_.at(2)->bottomMode());
            app.swarm_.spawn(app.primary_.position, app.primary_.area, 1, 1);
            app.reconcileWindows();
            QVERIFY(app.windows_.at(3)->bottomMode());
            app.showPets();
            app.showFood(2);
            QVERIFY(!app.petDialog_->windowFlags().testFlag(Qt::WindowStaysOnBottomHint));
            QVERIFY(!app.foodDialog_->windowFlags().testFlag(Qt::WindowStaysOnBottomHint));
        }
        ApplicationController restored(*animation_, SettingsStore(dir.path()), {});
        QVERIFY(restored.bottomMode_);
        restored.start();
        restored.timer_.stop();
        QVERIFY(restored.mainWindow_.isVisible());
        QVERIFY(restored.mainWindow_.bottomMode());
        for (const auto &entry : restored.windows_)
            QVERIFY(entry.second->bottomMode());
        restored.recall();
        QVERIFY(!restored.bottomMode_);
        QVERIFY(!restored.mainWindow_.bottomMode());
        for (const auto &entry : restored.windows_)
            QVERIFY(!entry.second->bottomMode());
        QVERIFY(!restored.store_.loadJson("config.json")["bottomMode"].toBool());
    }
};
} // namespace pettime
QTEST_MAIN(pettime::FoodApplicationTest)
#include "TestFoodApplication.moc"
