#include "PetModel.h"
#include <QSysInfo>
#include <algorithm>
#include <cmath>

namespace pettime {
namespace {
double length(QPointF p) { return std::hypot(p.x(), p.y()); }
double angle(QPointF p) { return std::atan2(p.y(), p.x()); }
double wrap(double a) { return std::remainder(a, 2 * Pi); }
QPointF direction(double a) { return {std::cos(a), std::sin(a)}; }
QPointF clampPoint(QPointF p, QRectF area, double margin) {
    const double mx = std::min(margin, area.width() * .45);
    const double my = std::min(margin, area.height() * .45);
    return {std::clamp(p.x(), area.left() + mx, area.right() - mx),
            std::clamp(p.y(), area.top() + my, area.bottom() - my)};
}
} // namespace

PetModel::PetModel(std::uint32_t seed, QPointF center, QRectF bounds, bool young)
    : position(center), area(bounds), juvenile(young), rng_(seed) {
    scale = young ? MinimumScale : AdultScale;
    nextSpecial_ = young ? random(1.8, 4.2) : random(20, 50);
    setArea(bounds);
    if (young)
        roamGoal_ = clampPoint(
            {random(area.left(), area.right()), random(area.top(), area.bottom())}, area, 48);
}
double PetModel::random(double low, double high) {
    return low + (high - low) * (static_cast<double>(rng_()) / 4294967296.0);
}
double PetModel::chaseChance(int count) {
    return count < 3 ? 0 : std::min(.7, .12 + (count - 3) * .1);
}
double PetModel::growthScale(double age) {
    const int stage = std::min(6, static_cast<int>(std::max(0.0, age) / 60));
    return MinimumScale + (AdultScale - MinimumScale) * stage / 6;
}
bool PetModel::frontal() const {
    return !juvenile && crashQueued &&
           (state == State::Launch || state == State::Flight || state == State::Dive ||
            state == State::Recoil);
}
double PetModel::progress() const {
    return std::clamp(actionAge / std::max(.01, actionDuration), 0.0, 1.0);
}
void PetModel::enter(State next, double duration) {
    state = next;
    actionAge = 0;
    actionDuration = duration;
}
void PetModel::setArea(QRectF bounds) {
    if (bounds.width() <= 0 || bounds.height() <= 0)
        return;
    area = bounds;
    position = clampPoint(position, area, 36);
    roamGoal_ = clampPoint(roamGoal_, area, 48);
}
void PetModel::moveTo(QPointF p, QRectF bounds) {
    position = p;
    setArea(bounds);
    speed = 0;
}
void PetModel::reveal(QRectF bounds) {
    setArea(bounds);
    position = area.center();
    paused = dragging = false;
    speed = targetSpeed_ = 0;
    revealLeft_ = 3;
    cooldown_ = 4;
    haveCursor_ = false;
}
void PetModel::chooseBehavior() {
    const double chance = random(0, 1);
    if (chance < .32) {
        enter(State::Rest, random(.6, 2.6));
        targetSpeed_ = 0;
    } else if (chance < .75) {
        enter(State::Probe, random(.6, 1.8));
        targetSpeed_ = random(18, 48) * pace;
        desired_ = heading + random(-1, 1);
    } else {
        enter(State::Dash, random(.35, .9));
        targetSpeed_ = random(115, 205) * pace;
        desired_ = heading + random(-.5, .5);
    }
}
QString PetModel::displayName() const {
    return primary                ? QSysInfo::machineHostName()
           : customName.isEmpty() ? QString("蟑螂 #%1").arg(id)
                                  : customName;
}
bool PetModel::playFeeding() {
    if (!active() || mealActive || cosmeticFeeding || frontal())
        return false;
    cosmeticFeeding = true;
    cosmeticAge = 0;
    return true;
}
bool PetModel::feed() {
    if (mealActive || cosmeticFeeding || !active())
        return false;
    mealActive = true;
    eating = false;
    mealAge_ = eatingAge_ = 0;
    mealGain_ = 3 + static_cast<int>(random(0, 3));
    mealPosition = clampPoint(position + direction(heading) * 83, area, 75 * scale);
    crashQueued = manualPounce = false;
    enter(State::Food, 25);
    targetSpeed_ = random(35, 55);
    desired_ = angle(mealPosition - position);
    return true;
}
void PetModel::startFlight(bool front, bool manual) {
    crashQueued = front;
    manualPounce = manual;
    const double roll = random(0, 1);
    pounceSpeed = front ? (roll < .25 ? 1.7 : roll < .6 ? 1.3 : 1) : 1;
    flightDuration = juvenile ? random(.85, 1.35)
                     : front  ? random(1, 1.4) / pounceSpeed
                              : random(1.7, 2.5);
    enter(front ? State::Launch : State::Flight, front ? .48 / pounceSpeed : flightDuration);
    targetSpeed_ = front ? 0 : random(180, 250);
    desired_ = heading + (manual ? 0 : random(-.85, .85));
    cooldown_ = 3;
}
void PetModel::demoPounce() {
    if (juvenile || cosmeticFeeding || !active())
        return;
    paused = mealActive = eating = false;
    revealLeft_ = 0;
    startFlight(true, true);
    speed = std::max(speed, 40.0);
}
void PetModel::crush() {
    cosmeticFeeding = false;
    if (!active())
        return;
    paused = dragging = mealActive = eating = crashQueued = manualPounce = false;
    revealLeft_ = 0;
    splitReady = false;
    enter(State::Crush, primary ? 1.05 : 1);
    speed = targetSpeed_ = 0;
}
void PetModel::becomeNymph() {
    juvenile = true;
    ++generation;
    scale = MinimumScale;
    age = 0;
    splitReady = false;
    enter(State::Probe, random(.55, 1.4));
    speed = 0;
    targetSpeed_ = random(28, 58);
    desired_ = heading + random(-1.4, 1.4);
    nextSpecial_ = random(2.5, 5.5);
}
void PetModel::retire() {
    cosmeticFeeding = false;
    if (active()) {
        enter(State::Fade, .38);
        speed = targetSpeed_ = 0;
    }
}

void PetModel::advance(double dt, QPointF cursor, const std::vector<QPointF> &neighbors) {
    impact = false;
    if (!std::isfinite(dt) || dt <= 0 || expired || splitReady)
        return;
    // Growth follows lifetime; paused actions and their visual frames share actionAge.
    age += dt;
    sinceEvade_ += dt;
    if (sinceEvade_ >= 5)
        evadeCount = 0;
    if (juvenile)
        scale = growthScale(age);
    if (cosmeticFeeding) {
        cosmeticAge += dt;
        if (cosmeticAge >= 2.65)
            cosmeticFeeding = false;
        previousCursor_ = cursor;
        haveCursor_ = true;
        return;
    }
    if (primary && (paused || dragging || menuOpen)) {
        previousCursor_ = cursor;
        haveCursor_ = true;
        return;
    }
    idlePhase = std::fmod(idlePhase + dt * .38, 1);
    if (revealLeft_ > 0) {
        revealLeft_ -= dt;
        previousCursor_ = cursor;
        haveCursor_ = true;
        return;
    }
    actionAge += dt;
    if (state == State::Crush) {
        splitReady = actionAge >= actionDuration;
        return;
    }
    if (state == State::Fade) {
        expired = actionAge >= actionDuration;
        return;
    }
    cooldown_ -= dt;
    nextSpecial_ -= dt;
    if (primary)
        updatePrimary(dt, cursor);
    else
        updateClone(dt, neighbors);
}

void PetModel::updatePrimary(double dt, QPointF cursor) {
    if (mealActive) {
        mealAge_ += dt;
        if (!eating && mealAge_ > 25) {
            mealActive = false;
            chooseBehavior();
        } else if (!eating) {
            const double distance = length(mealPosition - position);
            desired_ = angle(mealPosition - position);
            if (distance < 31) {
                eating = true;
                eatingAge_ = 0;
                speed = targetSpeed_ = 0;
            } else
                targetSpeed_ = std::min(78.0, distance * 1.25);
        } else {
            targetSpeed_ = 0;
            eatingAge_ += dt;
            if (eatingAge_ >= 1.15) {
                affection = std::min(100, affection + mealGain_);
                mealActive = eating = false;
                enter(State::Happy, affection < 20   ? 1.5
                                    : affection < 50 ? 2.1
                                    : affection < 80 ? 2.8
                                                     : 3.5);
                speed = targetSpeed_ = 0;
                desired_ = heading;
                nextSpecial_ = random(12, 26);
            }
        }
    } else if (actionAge >= actionDuration) {
        switch (state) {
        case State::Launch:
            enter(State::Flight, flightDuration);
            targetSpeed_ = manualPounce ? 70 * pounceSpeed : random(180, 250);
            break;
        case State::Flight:
            if (crashQueued) {
                enter(State::Dive, .55 / pounceSpeed);
                targetSpeed_ = (manualPounce ? 120 : 290) * pounceSpeed;
                desired_ = heading;
            } else {
                chooseBehavior();
                nextSpecial_ = juvenile ? random(2.5, 5.5) : random(15, 35);
            }
            break;
        case State::Dive:
            enter(State::Recoil, .42 / pounceSpeed);
            speed = targetSpeed_ = 0;
            impact = true;
            break;
        case State::Recoil:
            crashQueued = manualPounce = false;
            chooseBehavior();
            nextSpecial_ = random(15, 35);
            break;
        default:
            if (juvenile && nextSpecial_ <= 0) {
                nextSpecial_ = random(2.5, 5.5);
                if (random(0, 1) < .34)
                    startFlight(false, false);
                else
                    chooseBehavior();
            } else if (!juvenile && affection >= 10 && nextSpecial_ <= 0) {
                nextSpecial_ = random(.75, 1.25);
                if (random(0, 1) < .0015 + affection * .00012)
                    startFlight(affection >= 20 && random(0, 1) < .04 + affection * .003, false);
                else
                    chooseBehavior();
            } else
                chooseBehavior();
        }
    }
    const QPointF away = position - cursor;
    const QPointF cursorDelta = cursor - previousCursor_;
    const double mouseMove = std::abs(cursorDelta.x()) + std::abs(cursorDelta.y());
    if (haveCursor_ && !mealActive && !frontal() && length(away) < 100 && mouseMove > 3 &&
        cooldown_ <= 0) {
        ++evadeCount;
        sinceEvade_ = 0;
        if (!juvenile && random(0, 1) < chaseChance(evadeCount))
            startFlight(true, false);
        else {
            const double response = random(0, 1);
            desired_ = angle(away) + random(-.5, .5);
            if (response < .18) {
                enter(State::Rest, random(.35, .8));
                targetSpeed_ = 0;
                desired_ = heading + random(-1.2, 1.2);
            } else if (response < .68) {
                enter(State::Escape, random(.35, .7));
                targetSpeed_ = random(145, 215) * pace;
            } else {
                enter(State::Probe, random(.4, .9));
                targetSpeed_ = random(35, 78) * pace;
            }
            cooldown_ = random(1.8, 2.7);
        }
    }
    previousCursor_ = cursor;
    haveCursor_ = true;
    const double margin = frontal() ? 168 * scale : 80 * scale;
    const QPointF ahead = position + direction(heading) * (margin + speed * .35);
    const QRectF safe = area.adjusted(
        std::min(margin, area.width() * .45), std::min(margin, area.height() * .45),
        -std::min(margin, area.width() * .45), -std::min(margin, area.height() * .45));
    if (!safe.contains(ahead) && targetSpeed_ > 0)
        desired_ = angle(area.center() - position) + std::sin(age * .9) * .3;
    turnNoise_ += (random(-1, 1) - turnNoise_) * std::min(1.0, dt * 3);
    const double savedDesired = desired_;
    desired_ += turnNoise_ * .18;
    move(dt, margin,
         state == State::Escape                           ? 7
         : state == State::Dive                           ? 5
         : state == State::Dash || state == State::Flight ? 3.5
                                                          : 2.7);
    desired_ = savedDesired;
}

void PetModel::updateClone(double dt, const std::vector<QPointF> &neighbors) {
    if (state == State::Flight && actionAge >= actionDuration) {
        enter(State::Probe, 1);
        nextSpecial_ = random(2.5, 5.5);
    }
    if (state != State::Flight && nextSpecial_ <= 0) {
        nextSpecial_ = random(2.3, 4.8);
        if (random(0, 1) < .52) {
            flightDuration = random(.9, 1.45);
            enter(State::Flight, flightDuration);
        }
    }
    nextTurn_ -= dt;
    if (nextTurn_ <= 0) {
        if (length(roamGoal_ - position) < 100 || !area.contains(roamGoal_))
            roamGoal_ = clampPoint(
                {random(area.left(), area.right()), random(area.top(), area.bottom())}, area, 48);
        const QPointF goal = roamGoal_ - position;
        QPointF velocity =
            goal / std::max(1.0, length(goal)) + QPointF(random(-.48, .48), random(-.48, .48));
        for (auto other : neighbors) {
            const QPointF away = position - other;
            const double distance = length(away);
            if (distance > 1 && distance < 140)
                velocity += away / distance * ((140 - distance) / 140 * 2.6);
        }
        if (position.x() < area.left() + 95)
            velocity.rx() += (area.left() + 95 - position.x()) / 95 * 1.8;
        if (position.x() > area.right() - 95)
            velocity.rx() -= (position.x() - area.right() + 95) / 95 * 1.8;
        if (position.y() < area.top() + 95)
            velocity.ry() += (area.top() + 95 - position.y()) / 95 * 1.8;
        if (position.y() > area.bottom() - 95)
            velocity.ry() -= (position.y() - area.bottom() + 95) / 95 * 1.8;
        desired_ = angle(velocity) + random(-.42, .42);
        targetSpeed_ = state == State::Flight ? random(135, 220) : random(75, 180);
        nextTurn_ = random(.25, .8);
    }
    const double look = 36 + speed * .3;
    if (!area.adjusted(look, look, -look, -look).contains(position))
        desired_ = angle(area.center() - position) + random(-.4, .4);
    move(dt, 36, 3.6, false);
}
void PetModel::move(double dt, double margin, double turnRate, bool smoothCorner) {
    // Wall time drives growth/actions; a delayed event must not teleport a pet.
    dt = std::min(dt, .1);
    const double error = wrap(desired_ - heading);
    if (targetSpeed_ > 0)
        heading = wrap(heading + std::clamp(error, -turnRate * dt, turnRate * dt));
    const double wanted =
        targetSpeed_ * (smoothCorner ? 1 - std::min(.65, std::abs(error) * .22) : 1);
    speed += (wanted - speed) * std::min(1.0, dt * (wanted > speed ? 5 : 9));
    const QPointF old = position;
    position = clampPoint(position + direction(heading) * (speed * dt), area, margin);
    const double distance = length(position - old);
    traveled += distance;
    phase = std::fmod(phase + distance / std::max(9.0, 27 * scale), 1);
}
} // namespace pettime
