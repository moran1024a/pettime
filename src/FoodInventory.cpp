#include "FoodInventory.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace pettime {
namespace {
quint64 readId(const QJsonValue &value) {
    if (value.isString()) {
        const QString text = value.toString();
        if (text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) {
                return c >= QLatin1Char('0') && c <= QLatin1Char('9');
            }))
            return 0;
        bool ok = false;
        const auto id = text.toULongLong(&ok);
        return ok ? id : 0;
    }
    const double number = value.toDouble(0);
    if (value.isDouble() && std::isfinite(number) && number >= 1 &&
        number <= 9007199254740991.0 && std::floor(number) == number)
        return quint64(number);
    return 0;
}
} // namespace

FoodInventory::FoodInventory(std::uint32_t seed) : rng_(seed) {
    dropRemaining_ = nextDropDuration();
}
double FoodInventory::nextDropDuration() {
    return std::uniform_int_distribution<int>(180, 300)(rng_);
}
bool FoodInventory::reserve(quint64 id) {
    if (!id || !count_ || pending_.contains(id))
        return false;
    --count_;
    pending_.insert(id);
    return true;
}
void FoodInventory::complete(quint64 id) {
    const bool wasFull = total() == Capacity;
    if (!pending_.remove(id))
        return;
    if (wasFull)
        dropRemaining_ = nextDropDuration();
}
void FoodInventory::cancel(quint64 id) {
    if (pending_.remove(id))
        ++count_;
}
bool FoodInventory::advance(double dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 1 || total() == Capacity)
        return false;
    dropRemaining_ -= dt;
    if (dropRemaining_ > 1e-9)
        return false;
    const double leftover = std::max(0.0, -dropRemaining_);
    ++count_;
    dropRemaining_ = total() == Capacity ? 0 : nextDropDuration() - leftover;
    return true;
}
int FoodInventory::grantInteractionReward(int amount) {
    const int granted = std::clamp(amount, 0, Capacity - total());
    count_ += granted;
    if (total() == Capacity)
        dropRemaining_ = 0;
    return granted;
}
QJsonObject FoodInventory::save() const {
    QJsonArray pending;
    auto ids = pending_.values();
    std::sort(ids.begin(), ids.end());
    for (auto id : ids)
        pending.append(QString::number(id));
    return {{"count", count_}, {"dropRemaining", dropRemaining_}, {"pending", pending}};
}
void FoodInventory::restore(const QJsonObject &object) {
    const double savedCount = object.value("count").toDouble(3);
    count_ = std::isfinite(savedCount) && std::floor(savedCount) == savedCount
        ? int(std::clamp(savedCount, 0.0, double(Capacity))) : 3;
    pending_.clear();
    QSet<quint64> refunded;
    for (const auto &value : object.value("pending").toArray()) {
        const auto id = readId(value);
        if (id && !refunded.contains(id)) {
            refunded.insert(id);
            if (count_ < Capacity)
                ++count_;
        }
    }
    const double remaining = object.value("dropRemaining").toDouble(0);
    dropRemaining_ = total() == Capacity ? 0
        : std::isfinite(remaining) && remaining > 0 && remaining <= 300
            ? remaining : nextDropDuration();
}
} // namespace pettime
