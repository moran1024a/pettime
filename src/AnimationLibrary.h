#pragma once
#include "PetRenderState.h"
#include <QImage>
#include <QString>
#include <vector>

namespace pettime {
class AnimationLibrary {
  public:
    AnimationLibrary();
    QImage render(const PetModel &pet, int canvas = 320, double dpr = 1) const;
    QImage render(const PetRenderState &pet, int canvas = 320, double dpr = 1) const;
    bool exportFrames(const QString &directory, QString *error) const;
    static QRect alphaBounds(const QImage &image, int threshold = 24, int padding = 3);
    std::vector<QImage> walk, idle, nymph, nymphIdle, pounce, frontPounce, crush, nymphCrush,
        nymphPounce;

  private:
    QImage gait(const QImage &body, QRect crop, double phase, bool walking, bool young) const;
    static QImage normalize(QImage raw);
    static QImage frontPose(QImage raw, QRect reference);
    QImage frontFrame(const std::vector<QImage> &poses, int index) const;
    static std::vector<QImage> matchSize(const std::vector<QImage> &poses,
                                         const std::vector<QImage> &reference,
                                         const std::vector<QImage> &rest, int canvas);
};
} // namespace pettime
