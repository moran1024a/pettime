// Opt-in X11 integration test. Uses XTEST to exercise the window system, rather
// than sending Qt events directly (which cannot prove cross-application pass-through).
#include "AnimationLibrary.h"
#include "ApplicationController.h"
#include "CrackWindow.h"
#include "PetWindow.h"
#include <QApplication>
#include <QCursor>
#include <QLabel>
#include <QLibrary>
#include <QMouseEvent>
#include <QScreen>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest>
#include <cmath>

class Underlay : public QLabel {
  public:
    int presses = 0;
    Underlay() : QLabel("Pettime X11 integration test\nThis window closes automatically.") {
        setAlignment(Qt::AlignCenter);
        setStyleSheet("background: #e4e8ec; color: #20252a;");
        setWindowTitle("Pettime X11 test underlay");
        resize(900, 700);
    }
    void mousePressEvent(QMouseEvent *e) override {
        ++presses;
        QLabel::mousePressEvent(e);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QPoint initialCursor = QCursor::pos();
    if (app.platformName() != "xcb") {
        qCritical() << "This probe requires QT_QPA_PLATFORM=xcb and an X11 desktop.";
        return 2;
    }
    QLibrary x11("libX11.so.6"), xtst("libXtst.so.6"), xext("libXext.so.6");
    auto open = reinterpret_cast<void *(*)(const char *)>(x11.resolve("XOpenDisplay"));
    auto close = reinterpret_cast<int (*)(void *)>(x11.resolve("XCloseDisplay"));
    auto flush = reinterpret_cast<int (*)(void *)>(x11.resolve("XFlush"));
    auto focus =
        reinterpret_cast<int (*)(void *, unsigned long *, int *)>(x11.resolve("XGetInputFocus"));
    auto rootWindow =
        reinterpret_cast<unsigned long (*)(void *)>(x11.resolve("XDefaultRootWindow"));
    auto queryTree = reinterpret_cast<int (*)(void *, unsigned long, unsigned long *,
                                              unsigned long *, unsigned long **, unsigned int *)>(
        x11.resolve("XQueryTree"));
    auto internAtom = reinterpret_cast<unsigned long (*)(void *, const char *, int)>(
        x11.resolve("XInternAtom"));
    auto getProperty = reinterpret_cast<int (*)(void *, unsigned long, unsigned long, long, long,
                                                int, unsigned long, unsigned long *, int *,
                                                unsigned long *, unsigned long *, unsigned char **)>(
        x11.resolve("XGetWindowProperty"));
    auto freeData = reinterpret_cast<int (*)(void *)>(x11.resolve("XFree"));
    auto shapeRectangles = reinterpret_cast<void *(*)(void *, unsigned long, int, int *, int *)>(
        xext.resolve("XShapeGetRectangles"));
    auto motion = reinterpret_cast<int (*)(void *, int, int, int, unsigned long)>(
        xtst.resolve("XTestFakeMotionEvent"));
    auto button = reinterpret_cast<int (*)(void *, unsigned int, int, unsigned long)>(
        xtst.resolve("XTestFakeButtonEvent"));
    if (!open || !close || !flush || !focus || !rootWindow || !queryTree || !internAtom ||
        !getProperty || !freeData || !shapeRectangles || !motion || !button) {
        qCritical() << "X11/XTEST/XShape libraries unavailable";
        return 2;
    }
    void *display = open(nullptr);
    if (!display)
        return 2;
    const unsigned long root = rootWindow(display);
    const unsigned long below = internAtom(display, "_NET_WM_STATE_BELOW", 1);
    unsigned long type = 0, count = 0, remaining = 0;
    int format = 0;
    unsigned char *supported = nullptr;
    const unsigned long supportedAtom = internAtom(display, "_NET_SUPPORTED", 1);
    if (supportedAtom)
        getProperty(display, root, supportedAtom, 0, 4096, 0, 0,
                    &type, &format, &count, &remaining, &supported);
    bool supportsBelow = false;
    if (supported && format == 32) {
        const auto *atoms = reinterpret_cast<const unsigned long *>(supported);
        for (unsigned long i = 0; i < count; ++i)
            supportsBelow |= below && atoms[i] == below;
    }
    if (supported)
        freeData(supported);
    if (!supportsBelow) {
        qCritical() << "This probe requires an X11 window manager supporting _NET_WM_STATE_BELOW.";
        close(display);
        return 2;
    }
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok)
            ++failures;
        qInfo() << (ok ? "PASS" : "FAIL") << message;
    };
    auto move = [&](QPoint p) {
        motion(display, -1, p.x(), p.y(), 0);
        flush(display);
        QTest::qWait(60);
    };
    auto click = [&](QPoint p) {
        motion(display, -1, p.x(), p.y(), 0);
        button(display, 1, 1, 0);
        button(display, 1, 0, 30);
        flush(display);
        QTest::qWait(100);
    };
    auto rootChild = [&](unsigned long window) {
        for (int depth = 0; depth < 16; ++depth) {
            unsigned long queryRoot = 0, parent = 0, *children = nullptr;
            unsigned int childrenCount = 0;
            const bool ok = queryTree(display, window, &queryRoot, &parent, &children,
                                      &childrenCount);
            if (children)
                freeData(children);
            if (!ok || !parent)
                return 0UL;
            if (parent == root)
                return window;
            window = parent;
        }
        return 0UL;
    };
    auto above = [&](QWidget &upper, QWidget &lower) {
        // Flag changes recreate the QWindow. Qt 6.4 can leave QWidget::winId()
        // referring to the destroyed native window, so query the current handle.
        if (!upper.windowHandle() || !lower.windowHandle())
            return false;
        const auto upperFrame = rootChild(upper.windowHandle()->winId()),
                   lowerFrame = rootChild(lower.windowHandle()->winId());
        unsigned long queryRoot = 0, parent = 0, *children = nullptr;
        unsigned int childrenCount = 0;
        const bool ok = queryTree(display, root, &queryRoot, &parent, &children, &childrenCount);
        int upperIndex = -1, lowerIndex = -1;
        if (ok)
            for (unsigned int i = 0; i < childrenCount; ++i) {
                if (children[i] == upperFrame)
                    upperIndex = int(i);
                if (children[i] == lowerFrame)
                    lowerIndex = int(i);
            }
        if (children)
            freeData(children);
        return upperIndex >= 0 && lowerIndex >= 0 && upperIndex > lowerIndex;
    };
    auto inputTransparent = [&](QWidget &window) {
        int count = -1, ordering = 0;
        // XShapeInput is 2; no X11 development headers are needed by this probe.
        void *rectangles = shapeRectangles(display, window.windowHandle()->winId(), 2,
                                          &count, &ordering);
        if (rectangles)
            freeData(rectangles);
        return count == 0;
    };
    Underlay underlay;
    underlay.move(app.primaryScreen()->availableGeometry().center() - QPoint(450, 350));
    underlay.show();
    underlay.activateWindow();
    check(QTest::qWaitForWindowActive(&underlay), "test underlay active");
    unsigned long originalFocus = 0;
    int revert = 0;
    focus(display, &originalFocus, &revert);
    pettime::AnimationLibrary animation;
    pettime::PetModel model(1, underlay.mapToGlobal(underlay.rect().center()),
                            app.primaryScreen()->availableGeometry());
    model.paused = true;
    pettime::PetWindow pet(model, animation);
    pet.present();
    pet.show();
    check(QTest::qWaitForWindowExposed(&pet), "pet window exposed");
    QTest::qWait(100);
    unsigned long afterFocus = 0;
    focus(display, &afterFocus, &revert);
    check(originalFocus == afterFocus, "pet does not steal keyboard focus");
    int beforeClick = underlay.presses;
    click(pet.mapToGlobal(QPoint(5, 5)));
    check(underlay.presses == beforeClick + 1,
          "transparent pet pixels pass native clicks to underlay");
    QPoint body(160, 160);
    check(pet.mask().contains(body), "body belongs to input region");
    beforeClick = underlay.presses;
    click(pet.mapToGlobal(body));
    check(underlay.presses == beforeClick, "body intercepts native clicks");
    const auto originalMask = pet.mask();
    pet.highlight();
    QTest::qWait(120);
    check(pet.mask() == originalMask, "highlight leaves pet input mask unchanged");
    beforeClick = underlay.presses;
    click(pet.mapToGlobal(QPoint(10, 10)));
    check(underlay.presses == beforeClick + 1, "highlight overlay passes native clicks");

    unsigned long beforeModeFocus = 0;
    focus(display, &beforeModeFocus, &revert);
    const QRect beforeModeGeometry = pet.geometry();
    const QPointF beforeModePosition = model.position;
    const auto beforeModeState = model.state;
    QSignalSpy bottomCrushed(&pet, &pettime::PetWindow::crushed);
    pet.setBottomMode(true);
    underlay.lower();
    QTest::qWait(150);
    check(pet.isVisible() && pet.geometry() == beforeModeGeometry,
          "bottom mode keeps pet visible at its original geometry");
    check(above(underlay, pet), "native stack places lowered ordinary window above bottom pet");
    check(inputTransparent(pet), "bottom pet has an empty native input region");
    QLabel *highlight = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *label = qobject_cast<QLabel *>(widget); label && label != &underlay &&
                                                       label->text() == model.displayName())
            highlight = label;
    check(highlight && highlight->isVisible() && above(underlay, *highlight),
          "native stack places highlight below ordinary window");
    check(highlight && inputTransparent(*highlight), "bottom highlight has an empty input region");
    pet.highlight();
    QTest::qWait(80);
    check(highlight && above(underlay, *highlight), "repeated highlight respects bottom mode");
    focus(display, &afterFocus, &revert);
    check(beforeModeFocus == afterFocus, "bottom mode switch does not steal keyboard focus");
    // Lowering a normal window can expose unrelated applications. Restore the
    // test background before sending input so native clicks stay in this fixture.
    underlay.raise();
    underlay.activateWindow();
    check(QTest::qWaitForWindowActive(&underlay), "underlay active before bottom input checks");
    QTest::qWait(80);
    beforeClick = underlay.presses;
    click(pet.mapToGlobal(body));
    check(underlay.presses == beforeClick + 1 && model.position == beforeModePosition &&
              !model.dragging,
          "opaque bottom pet body passes native clicks to underlay");
    click(pet.mapToGlobal(body));
    click(pet.mapToGlobal(body));
    check(model.position == beforeModePosition && model.state == beforeModeState &&
              !model.dragging && bottomCrushed.count() == 0,
          "bottom native clicks cannot drag or crush pet");
    pet.setBottomMode(false);
    QTest::qWait(150);
    check(above(pet, underlay), "native stack restores pet above ordinary window");
    check(highlight && inputTransparent(*highlight), "restored highlight remains input transparent");
    focus(display, &afterFocus, &revert);
    check(beforeModeFocus == afterFocus, "normal mode restoration does not steal keyboard focus");
    QTest::qWait(app.doubleClickInterval() + 80);
    beforeClick = underlay.presses;
    click(pet.mapToGlobal(body));
    check(underlay.presses == beforeClick, "normal pet body intercepts native clicks again");

    {
        pettime::PetWindow visitor(animation);
        auto remote = pettime::PetRenderState::from(model);
        remote.position += QPointF(220, 0);
        visitor.presentRemote(remote, "X11 mode visitor", false);
        visitor.show();
        visitor.setBottomMode(true);
        visitor.setBottomMode(false);
        QTest::qWait(150);
        check(above(visitor, underlay), "restored visitor remains above ordinary window");
        check(inputTransparent(visitor), "restored visitor has an empty native input region");
        check(visitor.mask().contains(visitor.rect().center()),
              "visitor body belongs to painted region");
        beforeClick = underlay.presses;
        click(visitor.mapToGlobal(visitor.rect().center()));
        check(underlay.presses == beforeClick + 1,
              "restored visitor still passes native body clicks");
        visitor.hidePet();
        visitor.setBottomMode(true);
        visitor.setBottomMode(false);
        check(!visitor.isVisible(), "mode switches do not revive a hidden visitor");
    }

    QTest::qWait(app.doubleClickInterval() + 80);
    const QPointF oldPosition = model.position;
    const QPoint start = pet.mapToGlobal(body), delta(90, 30);
    move(start);
    button(display, 1, 1, 0);
    flush(display);
    QTest::qWait(80);
    move(start + delta);
    button(display, 1, 0, 0);
    flush(display);
    QTest::qWait(100);
    check(std::hypot(model.position.x() - oldPosition.x() - delta.x(),
                     model.position.y() - oldPosition.y() - delta.y()) < 3,
          "native drag moves pet");
    check(!model.dragging, "mouse release ends drag");
    QTest::qWait(app.doubleClickInterval() + 80);
    click(pet.mapToGlobal(body));
    click(pet.mapToGlobal(body));
    check(model.state == pettime::State::Crush, "native double click triggers crush");
    pettime::CrackWindow crack;
    crack.trigger(underlay.mapToGlobal(underlay.rect().center()));
    check(QTest::qWaitForWindowExposed(&crack), "crack overlay exposed");
    const int before = underlay.presses;
    click(crack.mapToGlobal(QPoint(80, 420)));
    check(underlay.presses == before + 1, "crack overlay passes native clicks");
    focus(display, &beforeModeFocus, &revert);
    const QRect crackGeometry = crack.geometry();
    crack.setBottomMode(true);
    QTest::qWait(100);
    check(crack.isVisible() && crack.geometry() == crackGeometry && above(underlay, crack),
          "native stack places visible crack below ordinary window");
    check(inputTransparent(crack), "bottom crack has an empty native input region");
    crack.trigger(underlay.mapToGlobal(underlay.rect().center()));
    QTest::qWait(100);
    check(above(underlay, crack), "retriggered crack stays below ordinary window");
    focus(display, &afterFocus, &revert);
    check(beforeModeFocus == afterFocus, "bottom crack switch and retrigger preserve focus");
    crack.setBottomMode(false);
    QTest::qWait(100);
    check(above(crack, underlay), "native stack restores crack above ordinary window");
    check(inputTransparent(crack), "restored crack has an empty native input region");
    focus(display, &afterFocus, &revert);
    check(beforeModeFocus == afterFocus, "normal crack restoration preserves focus");
    // Exercise the real controller's model/window ownership through mass splits
    // and shutdown, in addition to the simulation-only unit tests.
    auto applicationPets = [&] {
        std::vector<pettime::PetWindow *> result;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<pettime::PetWindow *>(widget); window && window != &pet)
                result.push_back(window);
        return result;
    };
    pet.hide();
    crack.hide();
    auto crushWindow = [](pettime::PetWindow *window) {
        // Native double-click delivery is tested above. Send this controller
        // stress event directly, without mixing QTest's synthetic button state
        // and the preceding XTEST/native event timestamps.
        const QPointF local = window->rect().center();
        QMouseEvent event(QEvent::MouseButtonDblClick, local, window->mapToGlobal(local.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    {
        QTemporaryDir data;
        pettime::RunOptions options;
        options.seed = 42;
        pettime::ApplicationController controller(animation, pettime::SettingsStore(data.path()),
                                                  options);
        controller.start();
        auto windows = applicationPets();
        check(windows.size() == 1, "controller starts one pet");
        if (!windows.empty()) {
            check(QTest::qWaitForWindowExposed(windows.front()), "controller window exposed");
            QSignalSpy crushed(windows.front(), &pettime::PetWindow::crushed);
            crushWindow(windows.front());
            check(crushed.count() == 1, "controller receives crush input");
            QElapsedTimer wait;
            wait.start();
            while (applicationPets().size() != 10 && wait.elapsed() < 3000)
                QTest::qWait(20);
            check(applicationPets().size() == 10,
                  "controller creates exactly ten pets after adult crush");
            for (auto *window : applicationPets())
                crushWindow(window);
            wait.restart();
            bool bounded = true;
            while (wait.elapsed() < 3000) {
                QTest::qWait(20);
                bounded = bounded && applicationPets().size() <= 72;
            }
            check(bounded && applicationPets().size() > 10,
                  "controller mass split respects total window limit");
        }
    }
    check(applicationPets().empty(), "controller destruction releases every pet window");
    motion(display, -1, initialCursor.x(), initialCursor.y(), 0);
    flush(display);
    close(display);
    qInfo() << "X11 probe failures:" << failures;
    return failures ? 1 : 0;
}
