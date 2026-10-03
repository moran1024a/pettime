#include "SwarmController.h"
#include <algorithm>
#include <QJsonArray>
#include <QSet>
#include <limits>
#include <cmath>
#include <map>

namespace pettime {
SwarmController::SwarmController(std::uint32_t seed, QPointF center, QRectF area) : rng_(seed) {
    auto p = std::make_unique<PetModel>(seed, center, area);
    p->id = 1;
    pets_.push_back(std::move(p));
}
QJsonObject SwarmController::saveProgress() const {
    QJsonArray pets;
    for (const auto &p : pets_) {
        if (!p->primary && (p->expired || p->state == State::Fade))
            continue;
        pets.append(QJsonObject{{"id", QString::number(p->id)}, {"name", p->customName},
            {"growth", p->growth()}, {"growthState", PetModel::growthState(p->growth())},
            {"growthDuration", p->growthDuration()},
            {"affection", p->affection}, {"generation", p->generation}, {"age", p->age},
            {"scale", p->scale}, {"x", p->position.x()}, {"y", p->position.y()},
            {"heading", p->heading}});
    }
    return {{"formatVersion", 1}, {"count", pets.size()},
            {"nextId", QString::number(nextId_)}, {"pets", pets}};
}
void SwarmController::restoreProgress(const QJsonObject &object) {
    // Startup only: keep the primary object's address because its window already references it.
    const QRectF localArea = primary().area;
    pets_.erase(pets_.begin() + 1, pets_.end());
    pending_.clear();
    nextId_ = 2;
    QSet<quint64> seen;
    const auto number = [](const QJsonObject &o, const char *key, double fallback,
                           double low, double high) {
        const double value = o.value(QLatin1String(key)).toDouble(fallback);
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    const auto readId = [](const QJsonValue &v) -> quint64 {
        bool ok = false;
        const quint64 id = v.isString() ? v.toString().toULongLong(&ok) : quint64(0);
        if (ok && id > 0 && id < std::numeric_limits<quint64>::max() - MaxPets)
            return id;
        if (v.isDouble() && v.toDouble() >= 1 && v.toDouble() <= 9007199254740991.0 &&
            std::floor(v.toDouble()) == v.toDouble())
            return quint64(v.toDouble());
        return 0;
    };
    for (const auto &value : object.value("pets").toArray()) {
        if (!value.isObject()) continue;
        const auto data = value.toObject();
        const auto id = readId(data.value("id"));
        if (!id || seen.contains(id)) continue;
        seen.insert(id);
        if (id != 1 && totalCount() >= limit_) continue;
        auto restored = std::make_unique<PetModel>(rng_(), localArea.center(), localArea);
        restored->id = id;
        restored->primary = id == 1;
        restored->customName = id == 1 ? QString{} : data.value("name").toString().trimmed().left(64);
        restored->growth_ = number(data, "growth", id == 1 ? 100 : 0, 0, 100);
        restored->juvenile = restored->growth_ < 100;
        const double duration = data.value("growthDuration").toDouble(0);
        restored->growthDuration_ = std::isfinite(duration) && duration >= 1800 && duration <= 3600
            ? duration : restored->juvenile ? restored->nextGrowthDuration() : 0;
        restored->scale = restored->juvenile ? PetModel::growthScale(restored->growth_)
            : number(data, "scale", AdultScale, MinimumScale, 1);
        restored->affection = int(number(data, "affection", 0, 0, 100));
        restored->generation = int(number(data, "generation", 0, 0, 1000000));
        restored->age = number(data, "age", 0, 0, 1e12);
        restored->heading = number(data, "heading", -Pi / 2, -2 * Pi, 2 * Pi);
        restored->moveTo({number(data, "x", localArea.center().x(), -1e7, 1e7),
                          number(data, "y", localArea.center().y(), -1e7, 1e7)}, localArea);
        if (id == 1)
            primary() = *restored;
        else
            pets_.push_back(std::move(restored));
        nextId_ = std::max(nextId_, std::uint64_t(id + 1));
    }
    nextId_ = std::max(nextId_, std::uint64_t(readId(object.value("nextId"))));
}
PetModel *SwarmController::find(std::uint64_t id) const {
    for (const auto &p : pets_)
        if (p->id == id)
            return p.get();
    return nullptr;
}
bool SwarmController::removeLocalPet(std::uint64_t id) {
    for (auto it = pets_.begin() + 1; it != pets_.end(); ++it) {
        const auto &p = *it;
        if (p->id != id)
            continue;
        if (p->primary || !p->motionGroup.isEmpty() || p->dispatchPaused)
            return false;
        if (beforeRemove)
            beforeRemove(id);
        pets_.erase(it);
        return true;
    }
    return false;
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
    std::map<QString, std::vector<QPointF>> neighbors;
    for (const auto &p : pets_)
        if (p->active())
            neighbors[p->motionGroup].push_back(p->position);
    for (auto &p : pets_)
        if (!p->primary && !p->dispatchPaused)
            p->advance(dt, {}, neighbors[p->motionGroup]);
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
        // Retirement is ownership cleanup, not remote simulation; a lost connection must
        // not leave a fading entity permanently reserving a birth slot.
        for (auto &p : pets_)
            if (!p->primary && p->dispatchPaused && p->state == State::Fade)
                p->expired = true;
        eraseExpired();
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
                    if (p->dispatchPaused)
                        p->expired = true;
                    else
                        p->retire();
                    ++retiring;
                }
            }
            const int before = totalCount();
            eraseExpired();
            if (totalCount() < before)
                continue;
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
