#pragma once
#include <QPolygonF>
#include <QWidget>
#include <random>
#include <vector>

namespace pettime {
class CrackWindow : public QWidget {
  public:
    explicit CrackWindow(std::uint32_t seed = 1);
    void trigger(QPointF center);
    void advance(double dt);
    void setBottomMode(bool enabled);
    bool bottomMode() const { return bottomMode_; }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    struct Fracture {
        QPolygonF points;
        bool major;
    };
    std::vector<Fracture> fractures_;
    std::mt19937 rng_;
    double age_ = 2;
    bool bottomMode_ = false;
    double random(double low, double high);
};
} // namespace pettime
