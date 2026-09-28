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
    QLibrary x11("libX11.so.6"), xtst("libXtst.so.6");
    auto open = reinterpret_cast<void *(*)(const char *)>(x11.resolve("XOpenDisplay"));
    auto close = reinterpret_cast<int (*)(void *)>(x11.resolve("XCloseDisplay"));
    auto flush = reinterpret_cast<int (*)(void *)>(x11.resolve("XFlush"));
    auto focus =
        reinterpret_cast<int (*)(void *, unsigned long *, int *)>(x11.resolve("XGetInputFocus"));
    auto motion = reinterpret_cast<int (*)(void *, int, int, int, unsigned long)>(
        xtst.resolve("XTestFakeMotionEvent"));
    auto button = reinterpret_cast<int (*)(void *, unsigned int, int, unsigned long)>(
        xtst.resolve("XTestFakeButtonEvent"));
    if (!open || !close || !flush || !focus || !motion || !button) {
        qCritical() << "X11/XTEST libraries unavailable";
        return 2;
    }
    void *display = open(nullptr);
    if (!display)
        return 2;
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
