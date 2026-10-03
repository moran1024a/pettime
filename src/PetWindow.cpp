#include "PetWindow.h"
#include <QCloseEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>

namespace pettime {
PetWindow::PetWindow(PetModel &model, const AnimationLibrary &animation)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus),
      model_(&model), animation_(animation) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setWindowTitle(model.primary ? "Pettime" : "Pettime · 若虫");
    const int extent = model.primary ? 320 : PetWindowExtent;
    setFixedSize(extent, extent);
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
    setFixedSize(PetWindowExtent, PetWindowExtent);
}
void PetWindow::presentRemote(const PetRenderState &state, const QString &name, bool frozen) {
    remote_ = state;
    remoteName_ = name;
    frozen_ = frozen;
    present();
}
void PetWindow::setBottomMode(bool enabled) {
    if (enabled && model_)
        model_->dragging = false;
    if (bottomMode_ == enabled)
        return;
    bottomMode_ = enabled;
    const bool visible = isVisible();
    const QRect bounds = geometry();
    const bool highlighted = highlight_ && highlight_->isVisible();
    const QRect highlightBounds = highlight_ ? highlight_->geometry() : QRect{};
    auto flags = windowFlags();
    const bool x11Bottom = enabled && QGuiApplication::platformName() == "xcb";
    // X11 tools are group transients and can remain above our management panel
    // even with BELOW set. An independent utility window can join the bottom layer.
    flags = (flags & ~Qt::WindowType_Mask) | (x11Bottom ? Qt::Window : Qt::Tool);
    flags.setFlag(Qt::WindowStaysOnTopHint, !enabled);
    flags.setFlag(Qt::WindowStaysOnBottomHint, enabled);
    flags.setFlag(Qt::WindowTransparentForInput, enabled || !model_);
    // Discard the platform object: Qt 6.4's xcb recreation otherwise retains its
    // transparency cache and can leave the new native window accepting input.
    if (windowHandle())
        windowHandle()->destroy();
    setAttribute(Qt::WA_X11NetWmWindowTypeUtility, x11Bottom);
    setAttribute(Qt::WA_TransparentForMouseEvents, enabled || !model_);
    setWindowFlags(flags);
    setGeometry(bounds);
    if (visible) {
        show();
        if (enabled)
            lower();
    }
    if (highlight_) {
        auto highlightFlags = highlight_->windowFlags();
        highlightFlags = (highlightFlags & ~Qt::WindowType_Mask) |
                         (x11Bottom ? Qt::Window : Qt::Tool);
        highlightFlags.setFlag(Qt::WindowStaysOnTopHint, !enabled);
        highlightFlags.setFlag(Qt::WindowStaysOnBottomHint, enabled);
        if (highlight_->windowHandle())
            highlight_->windowHandle()->destroy();
        highlight_->setAttribute(Qt::WA_X11NetWmWindowTypeUtility, x11Bottom);
        highlight_->setWindowFlags(highlightFlags);
        highlight_->setGeometry(highlightBounds);
        if (highlighted) {
            highlight_->show();
            if (enabled)
                highlight_->lower();
        }
    }
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
    if (!isVisible() || frozen_ || (model_ && model_->dispatchPaused))
        return;
    if (!highlight_) {
        const bool x11Bottom = bottomMode_ && QGuiApplication::platformName() == "xcb";
        highlight_ = std::make_unique<QLabel>(
            nullptr, (x11Bottom ? Qt::Window : Qt::Tool) | Qt::FramelessWindowHint |
                         (bottomMode_ ? Qt::WindowStaysOnBottomHint : Qt::WindowStaysOnTopHint) |
                         Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        highlight_->setAttribute(Qt::WA_TranslucentBackground);
        highlight_->setAttribute(Qt::WA_X11NetWmWindowTypeUtility, x11Bottom);
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
    if (highlight_->isVisible() && !bottomMode_)
        highlight_->raise();
}
void PetWindow::hidePet() {
    if (highlight_)
        highlight_->hide();
    highlightElapsed_ = 3000;
    hide();
}
void PetWindow::present() {
    const auto state = model_ ? PetRenderState::from(*model_) : remote_;
    image_ = animation_.render(state, width(), devicePixelRatioF());
    QRegion region = inputRegion(image_, size());
    const QPoint topLeft(qRound(state.position.x() - width() * .5),
                         qRound(state.position.y() - height() * .5));
    if (model_ && model_->transitioning())
        region &= QRegion(model_->area.toAlignedRect().translated(-topLeft));
    else if (!model_)
        if (const auto *screen = QGuiApplication::primaryScreen()) {
            region &= QRegion(screen->availableGeometry().translated(-topLeft));
        }
    if (region.isEmpty())
        region = QRegion(-2, -2, 1, 1);
    if (region != mask_) {
        mask_ = region;
        setMask(mask_);
    }
    move(topLeft);
    if (highlight_) {
        const auto elapsed = highlightClock_.restart();
        if (!frozen_)
            highlightElapsed_ += elapsed;
        if (highlightElapsed_ >= 3000)
            highlight_->hide();
        else {
            highlight_->setText(model_ ? model_->displayName() : remoteName_);
            highlight_->setGeometry(geometry());
            QRegion clip(QRect(QPoint{}, size()));
            if (model_ && model_->transitioning())
                clip &= QRegion(model_->area.toAlignedRect().translated(-topLeft));
            else if (!model_)
                if (const auto *screen = QGuiApplication::primaryScreen())
                    clip &= QRegion(screen->availableGeometry().translated(-topLeft));
            highlight_->setMask(clip.isEmpty() ? QRegion(-2, -2, 1, 1) : clip);
            highlight_->setVisible(isVisible() && !clip.isEmpty());
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
    if (bottomMode_ || !model_) {
        e->ignore();
        return;
    }
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
    if (bottomMode_ || !model_) {
        e->ignore();
        return;
    }
    if (!model_->dragging)
        return;
    QScreen *screen = QGuiApplication::screenAt(e->globalPosition().toPoint());
    model_->moveTo(e->globalPosition() - dragOffset_,
                   screen ? QRectF(screen->availableGeometry()) : model_->area);
    present();
}
void PetWindow::mouseReleaseEvent(QMouseEvent *e) {
    if (bottomMode_ || !model_) {
        e->ignore();
        return;
    }
    if (e->button() == Qt::LeftButton)
        model_->dragging = false;
}
void PetWindow::mouseDoubleClickEvent(QMouseEvent *e) {
    if (bottomMode_ || !model_) {
        e->ignore();
        return;
    }
    if (e->button() == Qt::LeftButton && model_->active() && !model_->transitioning() &&
        !model_->dispatchLocked) {
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
