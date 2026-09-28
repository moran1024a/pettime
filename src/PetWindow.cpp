#include "PetWindow.h"
#include <QCloseEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

namespace pettime {
PetWindow::PetWindow(PetModel &model, const AnimationLibrary &animation)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus),
      model_(&model), animation_(animation) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setWindowTitle(model.primary ? "Pettime" : "Pettime · 若虫");
    setFixedSize(model.primary ? 320 : 224, model.primary ? 320 : 224);
}
PetWindow::PetWindow(const AnimationLibrary &animation)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput),
      animation_(animation) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setWindowTitle("Pettime · 访客");
    setFixedSize(224, 224);
}
void PetWindow::presentRemote(const PetRenderState &state, const QString &name, bool frozen) {
    remote_ = state;
    remoteName_ = name;
    frozen_ = frozen;
    present();
}
QRegion PetWindow::inputRegion(const QImage &source, QSize logicalSize) {
    // A non-zero alpha mask passes only painted pixels to the pet. It also works on
    // Windows, unlike ignoring a mouse event (which does not forward it to other apps).
    const QImage image =
        source.size() == logicalSize
            ? source
            : source.scaled(logicalSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QRegion region;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        int start = -1;
        for (int x = 0; x <= image.width(); ++x) {
            const bool opaque = x < image.width() && qAlpha(row[x]) > 0;
            if (opaque && start < 0)
                start = x;
            else if (!opaque && start >= 0) {
                region += QRect(start, y, x - start, 1);
                start = -1;
            }
        }
    }
    // An empty QWidget mask means an unmasked rectangle; use an off-window region instead.
    return region.isEmpty() ? QRegion(-2, -2, 1, 1) : region;
}
void PetWindow::highlight() {
    if (!highlight_) {
        highlight_ = std::make_unique<QLabel>(
            nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                         Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        highlight_->setAttribute(Qt::WA_TranslucentBackground);
        highlight_->setAttribute(Qt::WA_ShowWithoutActivating);
        highlight_->setAttribute(Qt::WA_TransparentForMouseEvents);
        highlight_->setTextFormat(Qt::PlainText);
        highlight_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        highlight_->setStyleSheet(
            "QLabel { color: #fff176; border: 3px solid #ffd740; border-radius: 35px; padding: "
            "6px; background: rgba(35, 30, 0, 35); }");
    }
    highlightElapsed_ = 0;
    highlightClock_.restart();
    present();
    highlight_->show();
    highlight_->raise();
}
void PetWindow::present() {
    const auto state = model_ ? PetRenderState::from(*model_) : remote_;
    image_ = animation_.render(state, width(), devicePixelRatioF());
    const QRegion region = inputRegion(image_, size());
    if (region != mask_) {
        mask_ = region;
        setMask(mask_);
    }
    move(qRound(state.position.x() - width() * .5), qRound(state.position.y() - height() * .5));
    if (highlight_) {
        const auto elapsed = highlightClock_.restart();
        if (!frozen_)
            highlightElapsed_ += elapsed;
        if (highlightElapsed_ >= 3000)
            highlight_->hide();
        else {
            highlight_->setText(model_ ? model_->displayName() : remoteName_);
            highlight_->setGeometry(geometry());
        }
    }
    update();
}
void PetWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.drawImage(QPointF(0, 0), image_);
    ++painted_;
}
void PetWindow::mousePressEvent(QMouseEvent *e) {
    if (!model_)
        return;
    if (e->button() == Qt::RightButton) {
        emit contextRequested(e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton && model_->primary && model_->active()) {
        model_->dragging = true;
        model_->speed = 0;
        dragOffset_ = e->globalPosition() - model_->position;
    }
}
void PetWindow::mouseMoveEvent(QMouseEvent *e) {
    if (!model_)
        return;
    if (!model_->dragging)
        return;
    QScreen *screen = QGuiApplication::screenAt(e->globalPosition().toPoint());
    model_->moveTo(e->globalPosition() - dragOffset_,
                   screen ? QRectF(screen->availableGeometry()) : model_->area);
    present();
}
void PetWindow::mouseReleaseEvent(QMouseEvent *e) {
    if (!model_)
        return;
    if (e->button() == Qt::LeftButton)
        model_->dragging = false;
}
void PetWindow::mouseDoubleClickEvent(QMouseEvent *e) {
    if (!model_)
        return;
    if (e->button() == Qt::LeftButton && model_->active()) {
        model_->dragging = false;
        model_->crush();
        emit crushed();
        present();
    }
}
void PetWindow::closeEvent(QCloseEvent *e) {
    e->ignore();
    if (model_)
        emit exitRequested();
}
} // namespace pettime
