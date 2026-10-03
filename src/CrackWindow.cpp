#include "CrackWindow.h"
#include "PetModel.h"
#include <QGuiApplication>
#include <QPainter>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace pettime {
CrackWindow::CrackWindow(std::uint32_t seed)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput),
      rng_(seed) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(640, 640);
    setWindowTitle("Pettime · 裂纹");
}
double CrackWindow::random(double low, double high) {
    return low + (high - low) * (double(rng_()) / 4294967296.0);
}
void CrackWindow::setBottomMode(bool enabled) {
    if (bottomMode_ == enabled)
        return;
    bottomMode_ = enabled;
    const bool visible = isVisible();
    const QRect bounds = geometry();
    auto flags = windowFlags();
    const bool x11Bottom = enabled && QGuiApplication::platformName() == "xcb";
    flags = (flags & ~Qt::WindowType_Mask) | (x11Bottom ? Qt::Window : Qt::Tool);
    flags.setFlag(Qt::WindowStaysOnTopHint, !enabled);
    flags.setFlag(Qt::WindowStaysOnBottomHint, enabled);
    if (windowHandle())
        windowHandle()->destroy();
    setAttribute(Qt::WA_X11NetWmWindowTypeUtility, x11Bottom);
    setWindowFlags(flags);
    setGeometry(bounds);
    if (visible) {
        show();
        if (enabled)
            lower();
    }
}
void CrackWindow::trigger(QPointF center) {
    fractures_.clear();
    const double base = random(0, 2 * Pi);
    const int rays = 7 + int(random(0, 2));
    const QPointF origin(320, 320);
    for (int ray = 0; ray < rays; ++ray) {
        const double angle = base + ray * 2 * Pi / rays + random(-.19, .19),
                     reach = random(115, 290);
        const int pieces = 6 + int(random(0, 4));
        QPolygonF points{origin};
        double drift = 0;
        for (int n = 1; n <= pieces; ++n) {
            drift += random(-.065, .065);
            const double radius = reach * n / pieces * random(.92, 1.08);
            points << origin + QPointF(std::cos(angle + drift), std::sin(angle + drift)) * radius;
        }
        fractures_.push_back({points, true});
        const int branches = 2 + int(random(0, 3));
        for (int b = 0; b < branches; ++b) {
            const QPointF start = points[2 + int(random(0, std::max(1, pieces - 3)))];
            const double branchAngle =
                             angle + drift + (random(0, 1) < .5 ? -1 : 1) * random(.4, 1.25),
                         length = random(32, 118);
            const int count = 3 + int(random(0, 3));
            QPolygonF branch{start};
            double bend = 0;
            for (int n = 1; n < count; ++n) {
                bend += random(-.1, .1);
                branch << start +
                              QPointF(std::cos(branchAngle + bend), std::sin(branchAngle + bend)) *
                                  (length * n / (count - 1));
            }
            fractures_.push_back({branch, false});
        }
    }
    for (int ring = 0; ring < 2; ++ring) {
        QPolygonF points;
        const int sides = 9 + ring * 5;
        for (int i = 0; i <= sides; ++i) {
            const double angle = base + i * 2 * Pi / sides,
                         radius = (ring ? 72 : 35) * random(.82, 1.16);
            points << origin + QPointF(std::cos(angle), std::sin(angle)) * radius;
        }
        fractures_.push_back({points, false});
    }
    age_ = 0;
    move(qRound(center.x()) - 320, qRound(center.y()) - 320);
    show();
    if (bottomMode_)
        lower();
    else
        raise();
    update();
}
void CrackWindow::advance(double dt) {
    if (!isVisible())
        return;
    age_ += dt;
    if (age_ >= 1.35)
        hide();
    else
        update();
}
void CrackWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double a = std::clamp(1 - age_ / 1.35, 0.0, 1.0);
    for (const auto &f : fractures_) {
        p.setPen(QPen(QColor(29, 33, 38, int((f.major ? 74 : 48) * a)), f.major ? 1.65 : .95));
        p.drawPolyline(f.points);
        p.save();
        p.translate(.65, -.45);
        p.setPen(QPen(QColor(224, 237, 246, int((f.major ? 205 : 152) * a)), f.major ? .62 : .42));
        p.drawPolyline(f.points);
        p.restore();
    }
    const double flash = std::max(0.0, 1 - age_ / .2);
    if (flash > 0) {
        p.setPen(QPen(QColor(247, 251, 255, int(95 * flash)), 1.1));
        p.drawEllipse(QPointF(320, 320), 17 * flash, 17 * flash);
    }
}
} // namespace pettime
