#include "FoodInventory.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <cmath>
#include <limits>

using namespace pettime;
class TestFood : public QObject {
    Q_OBJECT
  private slots:
    void initialInventoryAndRandomIntervals() {
        QSet<int> intervals;
        for (std::uint32_t seed = 1; seed <= 80; ++seed) {
            FoodInventory inventory(seed);
            QCOMPARE(inventory.count(), 3);
            QCOMPARE(inventory.reservedCount(), 0);
            QVERIFY(inventory.pending().isEmpty());
            QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
            intervals.insert(int(inventory.dropRemaining()));
        }
        QVERIFY(intervals.size() > 1);
    }
    void runningTimeBoundaryAndInvalidGaps() {
        FoodInventory inventory(6);
        const auto before = inventory.save();
        for (double dt : {0.0, -1.0, 1.001, 3600.0,
                          std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
            QVERIFY(!inventory.advance(dt));
        QCOMPARE(inventory.save(), before);
        while (inventory.dropRemaining() > 1)
            QVERIFY(!inventory.advance(1));
        QVERIFY(!inventory.advance(inventory.dropRemaining() - .001));
        QCOMPARE(inventory.count(), 3);
        QVERIFY(inventory.advance(.001));
        QCOMPARE(inventory.count(), 4);
        QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
        QVERIFY(!inventory.advance(.25));
        QCOMPARE(inventory.count(), 4);
    }
    void reservationsRefundAndConsumptionAreIdempotent() {
        FoodInventory inventory;
        const double remaining = inventory.dropRemaining();
        QVERIFY(!inventory.reserve(0));
        QVERIFY(inventory.reserve(7));
        QCOMPARE(inventory.count(), 2);
        QCOMPARE(inventory.reservedCount(), 1);
        QVERIFY(inventory.pending().contains(7));
        QVERIFY(!inventory.reserve(7));
        QVERIFY(inventory.reserve(8));
        QVERIFY(inventory.reserve(9));
        QVERIFY(!inventory.reserve(10));
        inventory.cancel(7);
        inventory.cancel(7);
        inventory.complete(7);
        QCOMPARE(inventory.count(), 1);
        QCOMPARE(inventory.reservedCount(), 2);
        inventory.complete(8);
        inventory.complete(8);
        inventory.cancel(8);
        QCOMPARE(inventory.count(), 1);
        QCOMPARE(inventory.reservedCount(), 1);
        inventory.cancel(9);
        inventory.cancel(0);
        inventory.complete(10);
        QCOMPARE(inventory.count(), 2);
        QCOMPARE(inventory.reservedCount(), 0);
        QCOMPARE(inventory.dropRemaining(), remaining);
    }
    void fullCapacityIncludesReservationsAndRestartsFresh() {
        FoodInventory inventory(3);
        FoodInventory comparison(3);
        QCOMPARE(inventory.grantInteractionReward(100), 27);
        QCOMPARE(comparison.grantInteractionReward(100), 27);
        QCOMPARE(inventory.count(), FoodInventory::Capacity);
        QCOMPARE(inventory.dropRemaining(), 0.0);
        QVERIFY(inventory.reserve(1));
        QVERIFY(comparison.reserve(1));
        QCOMPARE(inventory.count(), 29);
        QCOMPARE(inventory.reservedCount(), 1);
        QCOMPARE(inventory.grantInteractionReward(), 0);
        for (int i = 0; i < 1000; ++i)
            QVERIFY(!inventory.advance(1));
        inventory.cancel(1);
        QCOMPARE(inventory.count(), 30);
        QCOMPARE(inventory.dropRemaining(), 0.0);
        QVERIFY(inventory.reserve(1));
        inventory.complete(1);
        comparison.complete(1);
        QCOMPARE(inventory.count(), 29);
        QCOMPARE(inventory.reservedCount(), 0);
        QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
        QCOMPARE(inventory.dropRemaining(), comparison.dropRemaining());
        while (inventory.dropRemaining() > 1)
            QVERIFY(!inventory.advance(1));
        QVERIFY(inventory.advance(inventory.dropRemaining()));
        QCOMPARE(inventory.count(), 30);
        QCOMPARE(inventory.dropRemaining(), 0.0);
        QCOMPARE(inventory.grantInteractionReward(-2), 0);
        QCOMPARE(inventory.count(), 30);
    }
    void pendingReservationsRefundOnceAcrossRestart() {
        FoodInventory inventory(5);
        QVERIFY(!inventory.advance(.75));
        QVERIFY(inventory.reserve(10));
        QVERIFY(inventory.reserve(std::numeric_limits<quint64>::max()));
        const auto saved = inventory.save();
        const auto json = QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object();
        FoodInventory restored(9);
        restored.restore(json);
        QCOMPARE(restored.count(), 3);
        QCOMPARE(restored.reservedCount(), 0);
        QCOMPARE(restored.dropRemaining(), inventory.dropRemaining());
        restored.cancel(10);
        restored.complete(std::numeric_limits<quint64>::max());
        QCOMPARE(restored.count(), 3);
        restored.restore(json);
        QCOMPARE(restored.count(), 3);
        FoodInventory restarted;
        restarted.restore(restored.save());
        QCOMPARE(restarted.count(), 3);
        QCOMPARE(restarted.save(), restored.save());
    }
    void malformedReservationsAndCapacityAreValidated() {
        FoodInventory inventory;
        const QJsonArray pending{"7", "07", 7, "8", "0", 0, -1, 1.5,
                                 "-2", "+3", " 4", "invalid", true, QJsonObject{},
                                 "18446744073709551616", 9007199254740992.0};
        inventory.restore({{"count", 1}, {"dropRemaining", 42.5}, {"pending", pending}});
        QCOMPARE(inventory.count(), 3); // Only distinct valid IDs 7 and 8 are refunded.
        QCOMPARE(inventory.reservedCount(), 0);
        QCOMPARE(inventory.dropRemaining(), 42.5);
        inventory.restore({{"count", 29}, {"dropRemaining", 42.5}, {"pending", pending}});
        QCOMPARE(inventory.count(), 30);
        QCOMPARE(inventory.dropRemaining(), 0.0);
        inventory.restore({{"count", 1000}, {"dropRemaining", 100}});
        QCOMPARE(inventory.count(), 30);
        QCOMPARE(inventory.dropRemaining(), 0.0);
        inventory.restore({{"count", -5}, {"dropRemaining", -1}});
        QCOMPARE(inventory.count(), 0);
        QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
        inventory.restore({{"count", 1.5}, {"dropRemaining", 301}, {"pending", "7"}});
        QCOMPARE(inventory.count(), 3);
        QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
        inventory.restore({{"count", "invalid"}, {"dropRemaining", "invalid"}});
        QCOMPARE(inventory.count(), 3);
        QVERIFY(inventory.dropRemaining() >= 180 && inventory.dropRemaining() <= 300);
        inventory.restore({});
        QCOMPARE(inventory.count(), 3);
        QCOMPARE(inventory.reservedCount(), 0);
    }
    void interactionRewardUsesOnlyFreeCapacity() {
        FoodInventory inventory;
        QVERIFY(inventory.reserve(1));
        QCOMPARE(inventory.grantInteractionReward(0), 0);
        QCOMPARE(inventory.grantInteractionReward(-1), 0);
        QCOMPARE(inventory.grantInteractionReward(), 1);
        QCOMPARE(inventory.count(), 3);
        QCOMPARE(inventory.grantInteractionReward(std::numeric_limits<int>::max()), 26);
        QCOMPARE(inventory.count(), 29);
        QCOMPARE(inventory.reservedCount(), 1);
        inventory.cancel(1);
        QCOMPARE(inventory.count(), 30);
        QCOMPARE(inventory.grantInteractionReward(), 0);
    }
};
QTEST_APPLESS_MAIN(TestFood)
#include "TestFood.moc"
