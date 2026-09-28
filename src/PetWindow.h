#pragma once
#include "AnimationLibrary.h"
#include <QElapsedTimer>
#include <QLabel>
#include <QRegion>
#include <QWidget>
#include <memory>

namespace pettime {
class PetWindow : public QWidget {
    Q_OBJECT
  public:
    PetWindow(PetModel &model, const AnimationLibrary &animation);
    void present();
    void highlight();
    const QImage &image() const { return image_; }
    int paintedFrames() const { return painted_; }
    static QRegion inputRegion(const QImage &image, QSize logicalSize);
  signals:
    void contextRequested(QPoint globalPosition);
    void crushed();
    void exitRequested();

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void closeEvent(QCloseEvent *) override;

  private:
    PetModel &model_;
    const AnimationLibrary &animation_;
    QImage image_;
    QRegion mask_;
    QPointF dragOffset_;
    int painted_ = 0;
    std::unique_ptr<QLabel> highlight_;
    QElapsedTimer highlightClock_;
};
} // namespace pettime
