#include "SwarmController.h"
#include <algorithm>
#include <cmath>

namespace pettime {
SwarmController::SwarmController(std::uint32_t seed, QPointF center, QRectF area) : rng_(seed) {
    auto p = std::make_unique<PetModel>(seed, center, area);
    p->id = 1;
    pets_.push_back(std::move(p));
}
PetModel *SwarmController::find(std::uint64_t id) const {
    for (const auto &p : pets_)
        if (p->id == id)
            return p.get();
    return nullptr;
}
bool SwarmController::rename(std::uint64_t id, const QString &name) {
    auto *p = find(id);
    if (!p || p->primary)
        return false;
    p->customName = name.trimmed().left(64);
    return true;
}
void SwarmController::eraseExpired() {
    for (auto it = pets_.begin() + 1; it != pets_.end();) {
        if ((*it)->expired) {
            if (beforeRemove)
                beforeRemove((*it)->id);
            it = pets_.erase(it);
        } else
            ++it;
    }
}
void SwarmController::setLimit(int value) {
    limit_ = std::clamp(value, 1, MaxPets);
    pending_.clear();
    int excess = totalCount() - limit_;
    for (auto it = pets_.begin() + 1; it != pets_.end() && excess > 0; ++it, --excess)
        (*it)->expired = true;
    eraseExpired();
}
void SwarmController::spawn(QPointF center, QRectF area, int generation, int count) {
    if (count <= 0 || limit_ == 1)
        return;
    pending_.push_back({center, area, generation, std::min(count, limit_ - 1)});
    pump();
}
void SwarmController::advance(double dt) {
    std::vector<QPointF> neighbors;
    for (const auto &p : pets_)
        if (p->active())
            neighbors.push_back(p->position);
    for (auto &p : pets_)
        if (!p->primary)
            p->advance(dt, {}, neighbors);
    // Notify window owners before destroying expired models; never remove the primary.
    for (auto &p : pets_)
        if (!p->primary && p->splitReady) {
            pending_.push_back({p->position, p->area, p->generation + 1,
                                std::min(limit_ - 1, 8 + static_cast<int>(rng_() % 5))});
            p->expired = true;
        }
    eraseExpired();
    pump();
}
void SwarmController::pump() {
    while (!pending_.empty()) {
        const auto b = pending_.front();
        int requested = 0;
        for (const auto &pending : pending_)
            requested = std::min(limit_ - 1, requested + pending.count);
        const int excess = totalCount() + requested - limit_;
        if (totalCount() + b.count > limit_) {
            int retiring = 0;
            for (const auto &p : pets_)
                if (p->state == State::Fade)
                    ++retiring;
            for (auto &p : pets_) {
                if (retiring >= excess)
                    break;
                if (!p->primary && p->active()) {
                    p->retire();
                    ++retiring;
                }
            }
            return;
        }
        pending_.pop_front();
        for (int i = 0; i < b.count; ++i) {
            const double angle = 2 * Pi * i / b.count;
            auto p = std::make_unique<PetModel>(
                rng_(), b.center + QPointF(std::cos(angle), std::sin(angle)) * 45, b.area, true);
            p->primary = false;
            p->id = nextId_++;
            p->generation = b.generation;
            p->heading = angle;
            pets_.push_back(std::move(p));
        }
    }
}
} // namespace pettime
