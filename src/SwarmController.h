#pragma once
#include "PetModel.h"
#include <QJsonObject>
#include <deque>
#include <functional>
#include <memory>

namespace pettime {
class SwarmController {
  public:
    static constexpr int MaxPets = 72; // Includes the primary pet, even after it becomes a nymph.
    explicit SwarmController(std::uint32_t seed = 1, QPointF center = {500, 500},
                             QRectF area = {0, 0, 1000, 1000});
    QJsonObject saveProgress() const;
    void restoreProgress(const QJsonObject &object);
    PetModel &primary() const { return *pets_.front(); }
    PetModel *find(std::uint64_t id) const;
    bool removeLocalPet(std::uint64_t id);
    bool rename(std::uint64_t id, const QString &name);
    bool canReproduce(quint64 id) const;
    bool reproduce(quint64 id);
    bool splitPrimary();
    void setLimit(int limit);
    int limit() const { return limit_; }
    std::function<void(std::uint64_t)> beforeRemove;
    void advance(double dt);
    void spawn(QPointF center, QRectF area, int generation, int count);
    const std::vector<std::unique_ptr<PetModel>> &pets() const { return pets_; }
    int totalCount() const { return static_cast<int>(pets_.size()); }
    int intervalMs() const { return totalCount() >= 48 ? 66 : totalCount() >= 24 ? 50 : 33; }
    std::size_t pendingCount() const { return pending_.size(); }

  private:
    struct Burst {
        QPointF center;
        QRectF area;
        int generation;
        int count;
    };
    void pump();
    void eraseExpired();
    int limit_ = MaxPets;
    std::uint64_t nextId_ = 2;
    std::mt19937 rng_;
    std::deque<Burst> pending_;
    std::vector<std::unique_ptr<PetModel>> pets_;
};
} // namespace pettime
