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
bool validArea(QRectF area) {
    return std::isfinite(area.x()) && std::isfinite(area.y()) &&
           std::isfinite(area.width()) && std::isfinite(area.height()) &&
           area.width() > 0 && area.height() > 0;
}
int nearestEdge(QPointF position, QRectF area) {
    const double distances[] = {std::abs(position.x() - area.left()),
                                std::abs(position.x() - area.right()),
                                std::abs(position.y() - area.top()),
                                std::abs(position.y() - area.bottom())};
    // Equal distances keep the first edge: left, right, top, then bottom.
    return int(std::min_element(distances, distances + 4) - distances);
}
double edgeFraction(QPointF position, QRectF area, int edge) {
    return std::clamp(edge < 2 ? (position.y() - area.top()) / area.height()
                               : (position.x() - area.left()) / area.width(), 0.0, 1.0);
}
QPointF edgePoint(QRectF area, int edge, double fraction, double inset) {
    const double x = area.left() + area.width() * fraction;
    const double y = area.top() + area.height() * fraction;
    switch (edge) {
    case 0: return {area.left() + inset, y};
    case 1: return {area.right() - inset, y};
    case 2: return {x, area.top() + inset};
    default: return {x, area.bottom() - inset};
    }
}
QPointF entryTarget(QRectF area, int edge, double fraction) {
    const double inset = std::min(100.0, (edge < 2 ? area.width() : area.height()) * .45);
    return clampPoint(edgePoint(area, edge, fraction, inset), area, 36);
}
QPointF exitTarget(QPointF position, QRectF area, int edge) {
    constexpr double outside = PetWindowExtent * .5 + 2;
    switch (edge) {
    case 0: return {area.left() - outside, position.y()};
    case 1: return {area.right() + outside, position.y()};
    case 2: return {position.x(), area.top() - outside};
    default: return {position.x(), area.bottom() + outside};
    }
}
} // namespace

PetModel::PetModel(std::uint32_t seed, QPointF center, QRectF bounds, bool young)
    : position(center), area(bounds), juvenile(young), rng_(seed), growthRng_(seed ^ 0x9e3779b9U),
      affectionRng_(seed ^ 0x85ebca6bU) {
    growth_ = young ? 0 : 100;
    growthDuration_ = young ? nextGrowthDuration() : 0;
    scale = growthScale(growth_);
    affectionDecayRemaining_ = nextAffectionDecay();
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
double PetModel::growthScale(double growth) {
    return MinimumScale + (AdultScale - MinimumScale) * std::clamp(growth, 0.0, 100.0) / 100;
}
QString PetModel::growthState(double growth) {
    return growth >= 100 ? QStringLiteral("成体")
         : growth >= 25 ? QStringLiteral("成长中") : QStringLiteral("幼体");
}
void PetModel::addGrowth(double amount) {
    if (growth_ >= 100)
        return; // Preserve the adult primary's user-selected display size.
    growth_ = std::min(100.0, growth_ + amount);
    if (growth_ > 100 - 1e-9)
        growth_ = 100;
    juvenile = growth_ < 100;
    scale = growthScale(growth_);
}
double PetModel::nextGrowthDuration() {
    return std::uniform_int_distribution<int>(1800, 3600)(growthRng_);
}
void PetModel::feedingGrowth() {
    addGrowth(std::uniform_int_distribution<int>(5, 15)(growthRng_) / 10.0);
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
    if (!validArea(bounds))
        return;
    const bool changed = area != bounds;
    area = bounds;
    roamGoal_ = clampPoint(roamGoal_, area, 48);
    if (!transitioning()) {
        position = clampPoint(position, area, 36);
        return;
    }
    if (!changed)
        return; // Off-screen transition coordinates are deliberate.
    transitionEdge_ = nearestEdge(position, area);
    transitionFraction_ = edgeFraction(position, area, transitionEdge_);
    if (entering()) {
        const double remaining = std::max(.001, transitionDuration_ - transitionElapsed_);
        startTransition(Transition::Entry,
                        entryTarget(area, transitionEdge_, transitionFraction_), remaining);
    } else {
        const QPointF target = exitTarget(position, area, transitionEdge_);
        startTransition(Transition::Exit, target,
                        std::clamp(length(target - position) / 300, 1.0, 3.0));
    }
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
    if (!juvenile || !active() || expired || splitReady || dispatchPaused || dispatchLocked ||
        transitioning() || cosmeticFeeding || frontal() || petting)
        return false;
    cosmeticFeeding = true;
    cosmeticAge = 0;
    return true;
}
bool PetModel::startPetting() {
    if (!active() || expired || splitReady || dispatchPaused || dispatchLocked || transitioning() ||
        mealActive || eating || cosmeticFeeding || frontal() || dragging || petting)
        return false;
    petting = true;
    pettingAge = 0;
    return true;
}
void PetModel::cancelPetting() {
    petting = false;
    pettingAge = 0;
}
double PetModel::nextAffectionDecay() {
    return std::uniform_int_distribution<int>(30, 60)(affectionRng_);
}
void PetModel::updateAffection(double dt) {
    // These clocks use running time only, so suspension and offline time are never replayed.
    if (dt > 1 || !active() || exiting())
        return;
    reproductionRemaining_ = std::max(0.0, reproductionRemaining_ - dt);
    affectionDecayRemaining_ -= dt;
    if (affectionDecayRemaining_ <= 1e-9) {
        affection = std::max(0, affection - std::uniform_int_distribution<int>(1, 2)(affectionRng_));
        affectionDecayRemaining_ = std::min(0.0, affectionDecayRemaining_) + nextAffectionDecay();
    }
}
bool PetModel::canReproduce() const {
    return !juvenile && active() && !expired && !splitReady && !dispatchPaused &&
           !dispatchLocked && !transitioning() && motionGroup.isEmpty() && !mealActive &&
           !eating && !cosmeticFeeding && !frontal() && !dragging && !petting &&
           reproductionRemaining_ <= 0;
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
    if (juvenile || cosmeticFeeding || petting || dispatchLocked || transitioning() || !active())
        return;
    paused = mealActive = eating = false;
    revealLeft_ = 0;
    startFlight(true, true);
    speed = std::max(speed, 40.0);
}
void PetModel::crush() {
    if (juvenile || !motionGroup.isEmpty() || petting || cosmeticFeeding || mealActive || eating ||
        frontal() || !active() || dispatchPaused || dispatchLocked || expired || splitReady ||
        transitioning())
        return;
    cosmeticFeeding = false;
    paused = dragging = mealActive = eating = crashQueued = manualPounce = false;
    revealLeft_ = 0;
    splitReady = false;
    enter(State::Crush, primary ? 1.05 : 1);
    speed = targetSpeed_ = 0;
}
void PetModel::becomeNymph() {
    cancelPetting();
    juvenile = true;
    ++generation;
    scale = MinimumScale;
    growth_ = 0;
    growthDuration_ = nextGrowthDuration();
    cosmeticFeeding = mealActive = eating = false;
    splitReady = false;
    enter(State::Probe, random(.55, 1.4));
    speed = 0;
    targetSpeed_ = random(28, 58);
    desired_ = heading + random(-1.4, 1.4);
    nextSpecial_ = random(2.5, 5.5);
}
void PetModel::retire() {
    cancelPetting();
    cancelTransition();
    setManualControl(false);
    cosmeticFeeding = false;
    mealActive = eating = false;
    if (active()) {
        enter(State::Fade, .38);
        speed = targetSpeed_ = 0;
    }
}

void PetModel::beginEntry(QRectF bounds, int edge, double fraction) {
    if (primary || !active() || expired || splitReady || !validArea(bounds) ||
        !std::isfinite(fraction))
        return;
    setManualControl(false);
    cosmeticFeeding = mealActive = eating = crashQueued = manualPounce = false;
    revealLeft_ = 0;
    area = bounds;
    transitionEdge_ = std::clamp(edge, 0, 3);
    transitionFraction_ = std::clamp(fraction, 0.0, 1.0);
    position = edgePoint(area, transitionEdge_, transitionFraction_, -PetWindowExtent * .5 - 2);
    roamGoal_ = clampPoint(roamGoal_, area, 48);
    startTransition(Transition::Entry,
                    entryTarget(area, transitionEdge_, transitionFraction_), 1);
}
bool PetModel::beginExit() {
    if (primary || !active() || expired || splitReady || exiting() || !validArea(area))
        return false;
    setManualControl(false);
    cosmeticFeeding = mealActive = eating = crashQueued = manualPounce = false;
    revealLeft_ = 0;
    transitionEdge_ = nearestEdge(position, area);
    transitionFraction_ = edgeFraction(position, area, transitionEdge_);
    const QPointF target = exitTarget(position, area, transitionEdge_);
    startTransition(Transition::Exit, target,
                    std::clamp(length(target - position) / 300, 1.0, 3.0));
    return true;
}
void PetModel::startTransition(Transition next, QPointF target, double duration) {
    cancelPetting();
    transition_ = next;
    transitionStart_ = position;
    transitionEnd_ = target;
    transitionElapsed_ = 0;
    transitionDuration_ = duration;
    const QPointF delta = target - position;
    if (!delta.isNull())
        heading = desired_ = angle(delta);
    speed = targetSpeed_ = length(delta) / duration;
    enter(State::Probe, duration);
}
void PetModel::cancelTransition() {
    if (!transitioning())
        return;
    transition_ = Transition::None;
    transitionElapsed_ = 0;
    speed = targetSpeed_ = 0;
    nextTurn_ = 0;
    nextSpecial_ = random(2.3, 4.8);
    if (active())
        enter(State::Probe, 1);
}
void PetModel::updateTransition(double dt) {
    const QPointF old = position;
    transitionElapsed_ = std::min(transitionDuration_, transitionElapsed_ + dt);
    if (transitionDuration_ - transitionElapsed_ < 1e-9)
        transitionElapsed_ = transitionDuration_;
    position = transitionStart_ +
               (transitionEnd_ - transitionStart_) * (transitionElapsed_ / transitionDuration_);
    const double distance = length(position - old);
    traveled += distance;
    phase = std::fmod(phase + distance / std::max(9.0, 27 * scale), 1);
    if (transitionElapsed_ == transitionDuration_) {
        position = transitionEnd_;
        if (entering())
            cancelTransition();
        else
            speed = targetSpeed_ = 0; // The controller owns the eventual handoff.
    }
}

bool PetModel::setManualControl(bool enabled) {
    if (enabled && (primary || transitioning() || !active() || expired || splitReady ||
                    dispatchPaused || dispatchLocked))
        return false;
    if (manualControl_ == enabled)
        return true;
    manualControl_ = enabled;
    manualDirection_ = {};
    speed = 0;
    if (active()) {
        if (enabled) {
            enter(State::Rest, 1);
            targetSpeed_ = 0;
        } else {
            enter(State::Probe, 1);
            nextTurn_ = 0;
            nextSpecial_ = random(2.3, 4.8);
        }
    }
    return true;
}
void PetModel::setManualDirection(QPointF direction) {
    if (!manualControl_)
        return;
    if (!std::isfinite(direction.x()) || !std::isfinite(direction.y()))
        direction = {};
    const double magnitude = length(direction);
    manualDirection_ = magnitude > 0 ? direction / magnitude : QPointF{};
    if (magnitude == 0)
        speed = 0;
}
void PetModel::updateManual(double dt) {
    const bool moving = !manualDirection_.isNull();
    const State next = moving ? State::Probe : State::Rest;
    if (state != next)
        enter(next, 1);
    speed = moving ? 140 : 0;
    if (!moving)
        return;
    heading = desired_ = angle(manualDirection_);
    const auto old = position;
    position = clampPoint(position + manualDirection_ * (speed * std::min(dt, .1)), area, 36);
    const double distance = length(position - old);
    traveled += distance;
    phase = std::fmod(phase + distance / std::max(9.0, 27 * scale), 1);
}

void PetModel::advance(double dt, QPointF cursor, const std::vector<QPointF> &neighbors) {
    impact = false;
    if (petting && (!active() || expired || splitReady || transitioning()))
        cancelPetting();
    if (!std::isfinite(dt) || dt <= 0 || dispatchPaused || expired || splitReady)
        return;
    updateAffection(dt);
    // Lifetime and growth are independent; frozen dispatch simulation does not advance.
    age += dt;
    sinceEvade_ += dt;
    if (sinceEvade_ >= 5)
        evadeCount = 0;
    if (active() && juvenile)
        addGrowth(dt * (100.0 / growthDuration_));
    if (petting) {
        pettingAge += dt;
        if (pettingAge >= 2) {
            petting = false;
            const int before = affection;
            affection = std::clamp(affection +
                std::uniform_int_distribution<int>(2, 4)(affectionRng_), 0, 100);
            lastPettingGain_ = affection - before;
            ++pettingCompletions_;
        }
        previousCursor_ = cursor;
        haveCursor_ = true;
        return;
    }
    if (cosmeticFeeding) {
        cosmeticAge += dt;
        if (cosmeticAge >= 2.65) {
            cosmeticFeeding = false;
            feedingGrowth();
            ++feedingCompletions_;
        }
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
    if (transitioning()) {
        updateTransition(dt);
        return;
    }
    if (!primary && manualControl_) {
        updateManual(dt);
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
    if (actionAge >= actionDuration) {
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
    if (haveCursor_ && !frontal() && length(away) < 100 && mouseMove > 3 &&
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
