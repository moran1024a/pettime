#pragma once

#include <QJsonObject>
#include <QSet>
#include <cstdint>
#include <random>

namespace pettime {
class FoodInventory {
  public:
    explicit FoodInventory(std::uint32_t seed = 1);
    static constexpr int Capacity = 30; // Available and reserved food share this capacity.
    int count() const { return count_; }
    int reservedCount() const { return pending_.size(); }
    double dropRemaining() const { return dropRemaining_; }
    bool reserve(quint64 id);
    void complete(quint64 id);
    void cancel(quint64 id);
    const QSet<quint64> &pending() const { return pending_; }
    // Returns true when food is added. Gaps above one second count as suspension,
    // not running time, so sleeping or a stalled event loop cannot stockpile food.
    bool advance(double dt);
    int grantInteractionReward(int amount = 1);
    QJsonObject save() const;
    void restore(const QJsonObject &object);

  private:
    int total() const { return count_ + reservedCount(); }
    double nextDropDuration();
    int count_ = 3;
    double dropRemaining_ = 0;
    QSet<quint64> pending_;
    std::mt19937 rng_;
};
} // namespace pettime
