#include "AnimationLibrary.h"
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pettime {
namespace {
QImage canvas(int size) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}
void quality(QPainter &p) {
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
}
QImage load(const char *name) {
    QImage image(QStringLiteral(":/assets/") + QString::fromLatin1(name));
    if (image.isNull())
        throw std::runtime_error(std::string("Cannot load embedded image: ") + name);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
QImage cell(const QImage &atlas, int column, int row, int columns, int rows, int bleed) {
    const int left = column * atlas.width() / columns,
              right = (column + 1) * atlas.width() / columns;
    const int top = row * atlas.height() / rows, bottom = (row + 1) * atlas.height() / rows;
    return atlas.copy(
        QRect(left - bleed, top - bleed, right - left + 2 * bleed, bottom - top + 2 * bleed)
            .intersected(atlas.rect()));
}
void segment(QPainter &p, QPointF a, QPointF b, double width) {
    const QPointF delta = b - a;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < .1)
        return;
    const QPointF normal(-delta.y() / length, delta.x() / length);
    const double h = width * .55, tip = width * .16, inner = width * .28;
    const QPolygonF shell{a + normal * h, b + normal * tip, b - normal * tip, a - normal * h};
    const QPolygonF chitin{a + delta * .13 + normal * inner, b - delta * .07 + normal * tip * .58,
                           b - delta * .07 - normal * tip * .58, a + delta * .13 - normal * inner};
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(39, 22, 14));
    p.drawPolygon(shell);
    p.setBrush(QColor(111, 58, 28));
    p.drawPolygon(chitin);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(48, 26, 15, 240), .48));
    p.drawPolygon(shell);
    p.setPen(QPen(QColor(193, 131, 75, 165), std::max(.35, width * .12)));
    p.drawLine(a + delta * .1 - normal * inner * .22, b - delta * .11 - normal * tip * .38);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(76, 39, 22));
    p.drawEllipse(a, width * .34, width * .34);
}
// Keep the main silhouette plus a two-pixel antialiasing margin, as in the original atlas cleanup.
void removeIslands(QImage &image) {
    const int w = image.width(), h = image.height();
    std::vector<int> labels(w * h), queue(w * h);
    int next = 0, best = 0, bestSize = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int index = y * w + x;
            if (labels[index] || qAlpha(image.pixel(x, y)) <= 24)
                continue;
            const int label = ++next;
            int head = 0, tail = 0;
            queue[tail++] = index;
            labels[index] = label;
            while (head < tail) {
                const int i = queue[head++], px = i % w, py = i / w;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = px + dx, yy = py + dy;
                        if (xx < 0 || xx >= w || yy < 0 || yy >= h)
                            continue;
                        const int j = yy * w + xx;
                        if (!labels[j] && qAlpha(image.pixel(xx, yy)) > 24) {
                            labels[j] = label;
                            queue[tail++] = j;
                        }
                    }
            }
            if (tail > bestSize) {
                bestSize = tail;
                best = label;
            }
        }
    if (!best)
        throw std::runtime_error("Empty animation silhouette");
    std::vector<bool> keep(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (labels[y * w + x] == best)
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && xx < w && yy >= 0 && yy < h)
                            keep[yy * w + xx] = true;
                    }
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x)
            if (!keep[y * w + x])
                line[x] = 0;
    }
}
int timedFrame(double progress, int count) {
    return std::clamp(static_cast<int>(progress * count), 0, count - 1);
}
int flightFrame(double age, double duration) {
    if (age < .2)
        return timedFrame(age / .2, 4);
    if (age >= duration - .22)
        return 18 + timedFrame((age - duration + .22) / .22, 6);
    return 3 + static_cast<int>((age - .2) * 20) % 15;
}
double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
void crumb(QPainter &p, QPointF center) {
    p.save();
    p.translate(center);
    const QPolygonF points{{-9, -1}, {-7, -4}, {-4, -4.5}, {-2, -7}, {1, -5}, {5, -5.5}, {7, -2},
                           {9, 0},   {6, 2},   {5, 5},     {1, 4},   {-2, 6}, {-4, 3},   {-8, 3}};
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(25, 17, 10, 72));
    p.drawEllipse(QRectF(-8, 2, 17, 7));
    QPainterPath path;
    path.addPolygon(points);
    path.closeSubpath();
    QRadialGradient gradient(QPointF(-1, -1), 11);
    gradient.setColorAt(0, QColor(209, 157, 91));
    gradient.setColorAt(1, QColor(132, 81, 42));
    p.fillPath(path, gradient);
    p.save();
    p.setClipPath(path);
    for (int i = 0; i < 24; ++i) {
        const double size = .45 + (i % 4) * .18;
        p.setBrush(i % 3 == 0 ? QColor(105, 62, 32, 205) : QColor(233, 190, 132, 205));
        p.drawEllipse(
            QRectF(std::sin(i * 12.9898) * 7.2, std::cos(i * 7.123) * 3.9, size, size * .72));
    }
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(91, 55, 31, 185), .8));
    p.drawPath(path);
    p.restore();
}
} // namespace

QRect AnimationLibrary::alphaBounds(const QImage &image, int threshold, int padding) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(line[x]) > threshold) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    }
    if (right < left)
        return {};
    return QRect(QPoint(left, top), QPoint(right, bottom))
        .adjusted(-padding, -padding, padding, padding)
        .intersected(image.rect());
}
AnimationLibrary::AnimationLibrary() {
    const QImage body = load("body-clean.png"), young = load("nymph-body.png");
    const QRect crop = alphaBounds(body, 32, 0), youngCrop = alphaBounds(young, 24, 0);
    if (crop.isEmpty() || youngCrop.isEmpty())
        throw std::runtime_error("Empty body image");
    for (int i = 0; i < 72; ++i)
        walk.push_back(gait(body, crop, i / 72.0, true, false));
    for (int i = 0; i < 48; ++i)
        idle.push_back(gait(body, crop, i / 48.0, false, false));
    for (int i = 0; i < 48; ++i)
        nymph.push_back(gait(young, youngCrop, i / 48.0, true, true));
    for (int i = 0; i < 12; ++i)
        nymphIdle.push_back(gait(young, youngCrop, i / 12.0, false, true));
    const QImage sideAtlas = load("pounce-atlas.png"), crushAtlas = load("crush-atlas.png");
    for (int i = 0; i < 24; ++i)
        pounce.push_back(normalize(cell(sideAtlas, i % 6, i / 6, 6, 4, 24)));
    std::vector<QImage> rawCrush;
    for (int i = 0; i < 16; ++i)
        rawCrush.push_back(normalize(cell(crushAtlas, i % 4, i / 4, 4, 4, 32)));
    crush = matchSize(rawCrush, walk, idle, 512);
    nymphCrush = matchSize(rawCrush, nymph, nymphIdle, 256);
    nymphPounce = matchSize(pounce, nymph, nymphIdle, 256);
    const QImage base = load("front-pounce-base.png");
    const QRect reference = alphaBounds(base, 24, 1);
    const std::vector<QImage> poses{frontPose(base, reference),
                                    frontPose(load("front-pounce-upstroke.png"), reference),
                                    frontPose(load("front-pounce-downstroke.png"), reference),
                                    frontPose(load("front-pounce-crouch.png"), reference),
                                    frontPose(load("front-pounce-transition.png"), reference)};
    for (int i = 0; i < 72; ++i)
        frontPounce.push_back(frontFrame(poses, i));
}

QImage AnimationLibrary::gait(const QImage &body, QRect crop, double phase, bool walking,
                              bool young) const {
    QImage image = canvas(young ? 256 : 512);
    QPainter p(&image);
    quality(p);
    if (!young)
        p.scale(2, 2);
    p.translate(128, young ? 126 : 141);
    for (int side = -1; side <= 1; side += 2)
        for (int leg = 0; leg < 3; ++leg) {
            const bool first = (leg % 2 == 0) == (side == 1);
            const double cycle = std::fmod(phase + (first ? 0 : .5), 1);
            double stride = 0, lift = 0;
            if (walking) {
                const double range = young ? 6 : 12;
                if (cycle < .64)
                    stride = -range + 2 * range * cycle / .64;
                else {
                    const double swing = (cycle - .64) / .36;
                    stride = range - 2 * range * smooth(swing);
                    lift = std::sin(swing * Pi) * (young ? 3.4 : 7);
                }
            }
            static const double knees[]{-16, 1, 18}, feet[]{-27, 7, 39}, youngKnees[]{-10, 1, 12},
                youngFeet[]{-18, 5, 29};
            const double hipY = young ? -17 + leg * 13 : -29 + leg * 19;
            const double kneeY = hipY + (young ? youngKnees[leg] : knees[leg]) +
                                 stride * (young ? .2 : .22) - lift * (young ? .2 : .18);
            const double footY = hipY + (young ? youngFeet[leg] : feet[leg]) + stride;
            const QPointF hip(side * (young ? 8 : 15), hipY),
                knee(side * ((young ? 17 : 31) + lift * (young ? .25 : .35)), kneeY);
            const QPointF ankle(side * ((young ? 28 : 52) - lift * (young ? .4 : .65)), footY);
            const QPointF toe(side * ((young ? 35 : 59) - lift * (young ? .3 : .45)),
                              footY + (young ? 2 : 3) + stride * (young ? .08 : .1));
            segment(p, hip, knee, young ? 2 : 3.1);
            segment(p, knee, ankle, young ? 1.05 : 1.55);
            segment(p, ankle, toe, young ? .52 : .72);
            p.setPen(QPen(QColor(67, 36, 19, 225), young ? .42 : .48));
            for (int n = 1; n < (young ? 5 : 6); ++n) {
                const double f = n / (young ? 5.0 : 6.0);
                const QPointF q = knee + (ankle - knee) * f;
                p.drawLine(q, q + QPointF(side * (young ? 1.4 : 2.2 - f), young ? -1.2 : -1.7));
            }
            if (!young) {
                p.setPen(QPen(QColor(57, 31, 18, 220), .55));
                p.drawLine(toe, toe + QPointF(side * 1.8, 1.4));
            }
        }
    for (int side = -1; side <= 1; side += 2) {
        const double sway = std::sin(phase * 2 * Pi + side * (young ? .7 : .8)) * (young ? 5 : 9);
        QPainterPath path;
        if (young) {
            path.moveTo(side * 4, -28);
            path.cubicTo(side * (10 + sway * .2), -44, side * (22 + sway), -67, side * (31 + sway),
                         -71 + std::sin(phase * 2 * Pi + side) * 4);
        } else {
            path.moveTo(side * 7, -54);
            path.cubicTo(side * (14 + sway * .25), -82, side * (29 + sway), -120,
                         side * (51 + sway), -124 + std::sin(phase * 2 * Pi + side) * 7);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(83, 45, 22, 245), young ? .8 : 1));
        p.drawPath(path);
        p.setPen(QPen(QColor(163, 112, 64, 150), young ? .25 : .32));
        p.drawPath(path);
    }
    const double motion = walking ? 1 : 0;
    p.translate(young ? 0 : std::sin(phase * 4 * Pi) * .35 * motion,
                std::abs(std::sin(phase * 4 * Pi)) * (young ? .55 : 1.1) * motion);
    if (!young)
        p.rotate(std::sin(phase * 4 * Pi) * .38 * motion);
    p.drawImage(young ? QRectF(-17, -33, 34, 72) : QRectF(-23, -61, 46, 119), body, crop);
    return image;
}
QImage AnimationLibrary::normalize(QImage raw) {
    removeIslands(raw);
    const QRect bounds = alphaBounds(raw);
    if (bounds.isEmpty())
        throw std::runtime_error("Empty atlas cell");
    const double scale = std::min({1.84, 476.0 / bounds.width(), 476.0 / bounds.height()});
    const double w = bounds.width() * scale, h = bounds.height() * scale;
    QImage image = canvas(512);
    QPainter p(&image);
    quality(p);
    p.drawImage(QRectF((512 - w) / 2, (512 - h) / 2, w, h), raw, bounds);
    return image;
}
std::vector<QImage> AnimationLibrary::matchSize(const std::vector<QImage> &poses,
                                                const std::vector<QImage> &reference,
                                                const std::vector<QImage> &rest, int size) {
    int area = 1;
    for (const auto *frames : {&reference, &rest}) {
        for (std::size_t i = 0; i < frames->size();
             i += std::max<std::size_t>(1, frames->size() / 4)) {
            const QRect r = alphaBounds((*frames)[i]);
            area = std::max(area, r.width() * r.height());
        }
        const QRect r = alphaBounds(frames->back());
        area = std::max(area, r.width() * r.height());
    }
    const double diameter = std::sqrt(area);
    std::vector<QImage> result;
    for (const auto &pose : poses) {
        QImage image = canvas(size);
        QPainter p(&image);
        quality(p);
        p.drawImage(QRectF((size - diameter) / 2, (size - diameter) / 2, diameter, diameter), pose);
        p.end();
        result.push_back(std::move(image));
    }
    return result;
}
QImage AnimationLibrary::frontPose(QImage raw, QRect reference) {
    raw = raw.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < raw.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(raw.scanLine(y));
        for (int x = 0; x < raw.width(); ++x) {
            const auto c = row[x];
            const int r = qRed(c), g = qGreen(c), b = qBlue(c);
            row[x] = qRgba(int(.84 * r + .02 * g), int(.03 * r + .8 * g + .04 * b),
                           int(.02 * g + .76 * b), qAlpha(c));
        }
    }
    const double scale = std::min(270.0 / reference.width(), 270.0 / reference.height());
    const QPointF anchor(reference.x() + reference.width() * .5,
                         reference.y() + reference.height() * .5);
    QImage image = canvas(384);
    QPainter p(&image);
    quality(p);
    p.drawImage(QRectF(192 - anchor.x() * scale, 192 - anchor.y() * scale, raw.width() * scale,
                       raw.height() * scale),
                raw);
    return image;
}
QImage AnimationLibrary::frontFrame(const std::vector<QImage> &poses, int i) const {
    int pose = 0;
    bool ground = false;
    int groundFrame = 0;
    if (i < 14) {
        if (i < 4) {
            ground = true;
            groundFrame = i * 11 % 72;
        } else
            pose = i < 8 ? 3 : i < 12 ? 4 : 1;
    } else if (i < 46) {
        const int flap = (i - 14) % 6;
        pose = flap < 2 ? 1 : flap == 2 ? 4 : flap < 5 ? 2 : 0;
    } else if (i < 62) {
        const int dive = (i - 46) % 4;
        pose = dive < 2 ? 2 : dive == 2 ? 4 : 1;
    } else {
        const int recoil = i - 62;
        if (recoil < 4)
            pose = 1;
        else if (recoil < 7)
            pose = 4;
        else if (recoil == 7)
            pose = 3;
        else {
            ground = true;
            groundFrame = recoil * 13 % 72;
        }
    }
    double scale = 1;
    if (!ground) {
        if (i < 8)
            scale = .78 + .08 * (i - 4) / 3;
        else if (i < 12)
            scale = .86 + .08 * (i - 8) / 3;
        else if (i < 14)
            scale = .94 + .04 * (i - 12);
        else if (i < 46)
            scale = .98 + .10 * (i - 14) / 31;
        else if (i < 62)
            scale = 1.08 + .02 * std::sin(Pi * (i - 46) / 15);
        else
            scale = 1.08 - .30 * smooth((i - 62) / 9.0);
    }
    const double phase = i < 14   ? i * .36
                         : i < 46 ? (i - 14) * 2 * Pi / 6
                         : i < 62 ? i * .61
                                  : i * .42;
    const double sx = 384 * scale * (!ground && pose == 2 ? 1.008 : 1),
                 sy = 384 * scale * (!ground && pose == 1 ? 1.008 : 1);
    QImage image = canvas(384);
    QPainter p(&image);
    quality(p);
    p.translate(192 + std::sin(phase * .7) * .85, 192 + std::sin(phase) * 1.3);
    p.rotate(std::sin(phase + .45) * .4);
    p.drawImage(QRectF(-sx / 2, -sy / 2, sx, sy), ground ? walk[groundFrame] : poses[pose]);
    return image;
}

QImage AnimationLibrary::render(const PetModel &m, int size, double dpr) const {
    const bool moving = !m.paused && !m.dragging && !m.menuOpen && m.speed > 3;
    const QImage *frame = nullptr;
    auto cyclic = [](double phase, const std::vector<QImage> &frames) -> const QImage * {
        return &frames[static_cast<std::size_t>(std::max(0.0, phase) * frames.size()) %
                       frames.size()];
    };
    if (m.state == State::Crush)
        frame = &(m.juvenile ? nymphCrush : crush)[timedFrame(m.progress(), 16)];
    else if (m.juvenile && m.state == State::Flight)
        frame = &nymphPounce[flightFrame(m.actionAge, m.flightDuration)];
    else if (m.juvenile)
        frame = m.state == State::Happy ? &nymph[int(m.actionAge * 14) % 48]
                : moving                ? cyclic(m.phase, nymph)
                                        : cyclic(m.idlePhase, nymphIdle);
    else if (m.frontal()) {
        const int start = m.state == State::Launch   ? 0
                          : m.state == State::Flight ? 14
                          : m.state == State::Dive   ? 46
                                                     : 62;
        const int count = m.state == State::Launch   ? 14
                          : m.state == State::Flight ? 32
                          : m.state == State::Dive   ? 16
                                                     : 10;
        frame = &frontPounce[start + timedFrame(m.progress(), count)];
    } else if (m.state == State::Flight)
        frame = &pounce[3 + int(m.actionAge * 14) % 15];
    else if (m.state == State::Happy) {
        if (m.affection < 20)
            frame = &walk[int(m.actionAge * 16) % 72];
        else {
            const int first = m.affection < 50 ? 3 : 8, count = m.affection < 50 ? 5 : 10,
                      rate = m.affection < 50   ? 8
                             : m.affection < 80 ? 10
                                                : 14;
            frame = &pounce[first + int(m.actionAge * rate) % count];
        }
    } else
        frame = moving ? cyclic(m.phase, walk) : cyclic(m.idlePhase, idle);
    QImage image = canvas(qCeil(size * dpr));
    image.setDevicePixelRatio(dpr);
    QPainter p(&image);
    quality(p);
    p.translate(size * .5, size * .5);
    const double heading = m.heading * 180 / Pi + 90;
    if (!m.frontal())
        p.rotate(heading);
    else if (m.state == State::Launch)
        p.rotate(heading * (1 - smooth(m.progress())));
    else if (m.state == State::Recoil)
        p.rotate(heading * smooth((m.actionAge - .25 / m.pounceSpeed) / (.17 / m.pounceSpeed)));
    double bob = 0;
    if (m.state == State::Happy) {
        const double rate = m.affection < 20   ? 8
                            : m.affection < 50 ? 10
                            : m.affection < 80 ? 12
                                               : 16;
        bob = std::abs(std::sin(m.actionAge * rate)) * (m.affection < 20   ? 2
                                                        : m.affection < 50 ? 4
                                                        : m.affection < 80 ? 7
                                                                           : 10);
        p.rotate(std::sin(m.actionAge * rate * .5) * (m.affection < 20   ? 1
                                                      : m.affection < 50 ? 1.7
                                                                         : 2.6));
    }
    if (m.state == State::Recoil) {
        const double shake = std::max(0.0, 1.35 - m.actionAge) * 2.2;
        p.translate(std::sin(m.actionAge * 43) * shake, std::cos(m.actionAge * 51) * shake);
    }
    const double lift = m.juvenile && m.state == State::Flight
                            ? std::sin(Pi * m.progress()) * (m.primary ? 9 : 11)
                            : 0;
    p.translate(0, -bob - lift);
    double zoom = m.state == State::Launch ? .98 : m.state == State::Dive ? 1.05 : 1;
    if (m.frontal())
        zoom *= m.state == State::Launch   ? 1 + .45 * smooth(m.progress())
                : m.state == State::Recoil ? 1.45 - .45 * smooth(m.progress())
                                           : 1.45;
    const double drawSize = std::min(double(size), 256 * m.scale * zoom);
    if (m.state == State::Fade)
        p.setOpacity(1 - m.progress());
    p.drawImage(QRectF(-drawSize / 2, -drawSize / 2, drawSize, drawSize), *frame);
    p.resetTransform();
    p.setOpacity(1);
    if (m.mealActive)
        crumb(p, QPointF(size * .5, size * .5) + m.mealPosition - m.position);
    return image;
}

bool AnimationLibrary::exportFrames(const QString &directory, QString *error) const {
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = "Cannot create export directory: " + directory;
        return false;
    }
    const std::vector<std::pair<QString, const std::vector<QImage> *>> groups{
        {"walk", &walk},
        {"idle", &idle},
        {"nymph", &nymph},
        {"nymph-idle", &nymphIdle},
        {"pounce", &pounce},
        {"front-pounce", &frontPounce},
        {"crush", &crush},
        {"nymph-crush", &nymphCrush},
        {"nymph-flight", &nymphPounce}};
    auto save = [&](const QImage &image, const QString &name) {
        const QString path = QDir(directory).filePath(name + ".png");
        if (image.save(path))
            return true;
        if (error)
            *error = "Cannot write " + path;
        return false;
    };
    for (const auto &group : groups) {
        const auto &frames = *group.second;
        const int columns = 8;
        QImage sheet(columns * 256, ((int(frames.size()) + columns - 1) / columns) * 256,
                     QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::transparent);
        QPainter p(&sheet);
        quality(p);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (!save(frames[i], group.first + QString("-%1").arg(i, 2, 10, QChar('0'))))
                return false;
            p.drawImage(QRectF((i % columns) * 256, (i / columns) * 256, 256, 256), frames[i]);
        }
        p.end();
        if (!save(sheet, group.first + "-sheet"))
            return false;
    }
    QImage backgrounds(1000, 380, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&backgrounds);
    quality(p);
    const QColor colors[]{Qt::white, QColor(22, 27, 32), QColor(136, 156, 119),
                          QColor(230, 213, 182)};
    for (int i = 0; i < 4; ++i) {
        p.fillRect(i * 250, 0, 250, 380, colors[i]);
        p.drawImage(QRectF(i * 250, 55, 250, 250), walk[i * 11]);
    }
    p.end();
    return save(backgrounds, "background-check");
}
} // namespace pettime
