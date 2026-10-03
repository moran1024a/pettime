#pragma once
#include "PetModel.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace pettime {
// Drawing data only. Remote windows never own or advance a PetModel.
struct PetRenderState {
    QPointF position, mealPosition;
    State state = State::Probe;
    bool juvenile = false, primary = false, paused = false, dragging = false, menuOpen = false,
         mealActive = false, crashQueued = false;
    int affection = 0;
    double growth = 100;
    double scale = AdultScale, heading = 0, speed = 0, phase = 0, idlePhase = 0, actionAge = 0,
           actionDuration = 1, flightDuration = 1.15, pounceSpeed = 1;
    bool frontal() const {
        return crashQueued && !juvenile &&
               (state == State::Launch || state == State::Flight || state == State::Dive ||
                state == State::Recoil);
    }
    double progress() const {
        return std::clamp(actionAge / std::max(.01, actionDuration), 0.0, 1.0);
    }
    static PetRenderState from(const PetModel &p) {
        PetRenderState s;
        s.position = p.position;
        s.growth = p.growth();
        s.mealPosition = p.mealPosition;
        s.state = p.state;
        s.juvenile = p.juvenile;
        s.primary = p.primary;
        s.paused = p.paused;
        s.dragging = p.dragging;
        s.menuOpen = p.menuOpen;
        s.mealActive = p.mealActive;
        s.affection = p.affection;
        s.crashQueued = p.crashQueued;
        s.scale = p.scale;
        s.heading = p.heading;
        s.speed = p.speed;
        s.phase = p.phase;
        s.idlePhase = p.idlePhase;
        s.actionAge = p.actionAge;
        s.actionDuration = p.actionDuration;
        s.flightDuration = p.flightDuration;
        s.pounceSpeed = p.pounceSpeed;
        if (p.cosmeticFeeding) {
            s.affection = 0;
            s.state = p.cosmeticAge < 1.15 ? State::Food : State::Happy;
            s.mealActive = p.cosmeticAge < 1.15;
            s.mealPosition = s.position;
            s.speed = 0;
            s.actionAge = p.cosmeticAge < 1.15 ? p.cosmeticAge : p.cosmeticAge - 1.15;
        } else if (p.petting) {
            s.state = State::Happy;
            s.mealActive = false;
            s.speed = 0;
            s.actionAge = p.pettingAge;
            s.actionDuration = 2;
        }
        return s;
    }
    QJsonArray encode() const {
        return {position.x(),    position.y(),   int(state),  juvenile,   scale,
                heading,         speed,          phase,       idlePhase,  actionAge,
                actionDuration,  flightDuration, pounceSpeed, mealActive, mealPosition.x(),
                mealPosition.y(), growth, affection};
    }
    static bool decode(const QJsonValue &v, PetRenderState &s) {
        if (!v.isArray())
            return false;
        const auto a = v.toArray();
        if (a.size() != 18 || !a[3].isBool() || !a[13].isBool())
            return false;
        for (int i = 0; i < a.size(); ++i)
            if (i != 3 && i != 13 &&
                (!a[i].isDouble() || !std::isfinite(a[i].toDouble()) ||
                 std::abs(a[i].toDouble()) > 10000000))
                return false;
        const double st = a[2].toDouble();
        if (st != std::floor(st) || st < 0 || st > int(State::Fade) || a[4].toDouble() < .1 ||
            a[4].toDouble() > 2 || a[9].toDouble() < 0 || a[10].toDouble() <= 0 ||
            a[11].toDouble() < .5 || a[12].toDouble() <= 0 || a[7].toDouble() < 0 ||
            a[8].toDouble() < 0 || a[16].toDouble() < 0 || a[16].toDouble() > 100 ||
            a[3].toBool() != (a[16].toDouble() < 100) || a[17].toDouble() < 0 ||
            a[17].toDouble() > 100 || a[17].toDouble() != std::floor(a[17].toDouble()))
            return false;
        s = {};
        s.position = {a[0].toDouble(), a[1].toDouble()};
        s.state = State(int(st));
        s.juvenile = a[3].toBool();
        s.scale = a[4].toDouble();
        s.growth = a[16].toDouble();
        s.affection = a[17].toInt();
        s.heading = a[5].toDouble();
        s.speed = a[6].toDouble();
        s.phase = a[7].toDouble();
        s.idlePhase = a[8].toDouble();
        s.actionAge = a[9].toDouble();
        s.actionDuration = a[10].toDouble();
        s.flightDuration = a[11].toDouble();
        s.pounceSpeed = a[12].toDouble();
        s.mealActive = a[13].toBool();
        s.mealPosition = {a[14].toDouble(), a[15].toDouble()};
        return true;
    }
};
} // namespace pettime
