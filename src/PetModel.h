#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <random>
#include <vector>

namespace pettime {
constexpr double Pi = 3.14159265358979323846;
constexpr double AdultScale = .75;
constexpr double MinimumScale = .24;
constexpr int PetWindowExtent = 224;
enum class State {
    Rest,
    Probe,
    Dash,
    Escape,
    Food,
    Happy,
    Launch,
    Flight,
    Dive,
    Recoil,
    Crush,
    Fade
};

// Data and simulation only: no windows, clocks, filesystem or global cursor calls.
class PetModel {
  public:
    PetModel(std::uint32_t seed, QPointF center, QRectF area, bool juvenile = false);
    void advance(double dt, QPointF cursor, const std::vector<QPointF> &neighbors = {});
    void beginEntry(QRectF bounds, int edge, double fraction);
    bool beginExit();
    void cancelTransition();
    void cancelEntry() { cancelTransition(); }
    bool entering() const { return transition_ == Transition::Entry; }
    bool transitioning() const { return transition_ != Transition::None; }
    bool exiting() const { return transition_ == Transition::Exit; }
    bool exitComplete() const {
        return exiting() && transitionElapsed_ >= transitionDuration_;
    }
    int transitionEdge() const { return transitionEdge_; }
    double transitionFraction() const { return transitionFraction_; }
    bool setManualControl(bool enabled);
    void setManualDirection(QPointF direction);
    bool manuallyControlled() const { return manualControl_; }
    bool playFeeding();
    QString displayName() const;
    std::uint64_t id = 0;
    QString customName;
    QString motionGroup; // Empty means the local desktop.
    bool dispatchPaused = false;
    bool dispatchLocked = false;
    bool cosmeticFeeding = false;
    double cosmeticAge = 0;
    void demoPounce();
    void crush();
    void reveal(QRectF area);
    void becomeNymph();
    void retire();
    void setArea(QRectF area);
    void moveTo(QPointF center, QRectF area);
    double random(double low, double high);
    static double chaseChance(int count);
    static double growthScale(double growth);
    static QString growthState(double growth);
    double growth() const { return growth_; }
    double growthDuration() const { return growthDuration_; }
    quint64 feedingCompletions() const { return feedingCompletions_; }
    bool frontal() const;
    bool active() const { return state != State::Crush && state != State::Fade; }
    double progress() const;

    QPointF position;
    QRectF area;
    State state = State::Probe;
    bool juvenile = false, primary = true;
    bool paused = false, dragging = false, menuOpen = false;
    bool mealActive = false, eating = false, crashQueued = false, manualPounce = false;
    bool splitReady = false, expired = false, impact = false;
    int affection = 0, generation = 0, evadeCount = 0;
    double scale = AdultScale, pace = 1.5;
    double heading = -Pi / 2, speed = 0, phase = 0, idlePhase = 0;
    double age = 0, actionAge = 0, actionDuration = 1, traveled = 0;
    double pounceSpeed = 1, flightDuration = 1.15;
    QPointF mealPosition;

  private:
    friend class SwarmController;
    double growth_ = 100;
    double growthDuration_ = 0;
    quint64 feedingCompletions_ = 0;
    double nextGrowthDuration();
    void addGrowth(double amount);
    void feedingGrowth();
    enum class Transition { None, Entry, Exit };
    Transition transition_ = Transition::None;
    int transitionEdge_ = 0;
    double transitionFraction_ = .5;
    double transitionElapsed_ = 0, transitionDuration_ = 1;
    QPointF transitionStart_, transitionEnd_;
    void startTransition(Transition next, QPointF target, double duration);
    void updateTransition(double dt);
    bool manualControl_ = false;
    QPointF manualDirection_;
    void updateManual(double dt);
    void enter(State next, double duration);
    void chooseBehavior();
    void startFlight(bool frontal, bool manual);
    void updatePrimary(double dt, QPointF cursor);
    void updateClone(double dt, const std::vector<QPointF> &neighbors);
    void move(double dt, double margin, double turnRate, bool smoothCorner = true);
    std::mt19937 rng_;
    std::mt19937 growthRng_;
    double desired_ = -Pi / 2, targetSpeed_ = 32, turnNoise_ = 0;
    double cooldown_ = 0, sinceEvade_ = 10, nextSpecial_ = 30, revealLeft_ = 0;
    double nextTurn_ = 0;
    QPointF previousCursor_, roamGoal_;
    bool haveCursor_ = false;
};
} // namespace pettime
