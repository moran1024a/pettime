#include "AnimationLibrary.h"
#include "CrackWindow.h"
#include "PetWindow.h"
#include <QMouseEvent>
#include <QScreen>
#include <QtTest>
#include <memory>

using namespace pettime;
class WindowModeTest : public QObject {
    Q_OBJECT
    std::unique_ptr<AnimationLibrary> animation_;
    static QLabel *highlightLabel(const QString &name) {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *label = qobject_cast<QLabel *>(widget); label && label->text() == name)
                return label;
        return nullptr;
    }
    static void checkMode(QWidget &window, bool bottom, bool transparent) {
        QCOMPARE(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint), !bottom);
        QCOMPARE(window.windowFlags().testFlag(Qt::WindowStaysOnBottomHint), bottom);
        QCOMPARE(window.windowFlags().testFlag(Qt::WindowTransparentForInput), transparent);
        QCOMPARE(window.testAttribute(Qt::WA_TransparentForMouseEvents), transparent);
        QVERIFY(window.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(window.testAttribute(Qt::WA_ShowWithoutActivating));
        QCOMPARE(window.focusPolicy(), Qt::NoFocus);
    }
    static bool sendMouse(PetWindow &window, QEvent::Type type, Qt::MouseButton button,
                          QPointF local = {160, 160}) {
        QMouseEvent event(type, local, window.mapToGlobal(local.toPoint()), button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &event);
        return event.isAccepted();
    }

  private slots:
    void initTestCase() { animation_ = std::make_unique<AnimationLibrary>(); }
    void localRoundTripPreservesVisibleGeometryAndRendering() {
        PetModel model(1, {400, 300}, {0, 0, 800, 600});
        PetWindow window(model, *animation_);
        window.present();
        window.show();
        QTest::qWait(20);
        const QRect bounds = window.geometry();
        const QRegion mask = window.mask();
        const QImage image = window.image();
        QVERIFY(!window.bottomMode());
        checkMode(window, false, false);
        window.setBottomMode(true);
        QVERIFY(window.bottomMode());
        checkMode(window, true, true);
        QVERIFY(window.isVisible());
        QCOMPARE(window.geometry(), bounds);
        QCOMPARE(window.mask(), mask);
        QCOMPARE(window.image(), image);
        const int painted = window.paintedFrames();
        window.present();
        QTRY_VERIFY(window.paintedFrames() > painted);
        window.setBottomMode(false);
        QVERIFY(!window.bottomMode());
        checkMode(window, false, false);
        QVERIFY(window.isVisible());
        QCOMPARE(window.geometry(), bounds);
        QCOMPARE(window.mask(), mask);
        QSignalSpy context(&window, &PetWindow::contextRequested);
        sendMouse(window, QEvent::MouseButtonPress, Qt::RightButton);
        QCOMPARE(context.count(), 1);
        sendMouse(window, QEvent::MouseButtonPress, Qt::LeftButton);
        QVERIFY(model.dragging);
        sendMouse(window, QEvent::MouseButtonRelease, Qt::LeftButton);
        QVERIFY(!model.dragging);
    }
    void bottomRejectsMouseEventsAndCancelsDrag() {
        PetModel model(2, {400, 300}, {0, 0, 800, 600});
        PetWindow window(model, *animation_);
        window.present();
        sendMouse(window, QEvent::MouseButtonPress, Qt::LeftButton);
        QVERIFY(model.dragging);
        const QPointF position = model.position;
        const State state = model.state;
        QSignalSpy context(&window, &PetWindow::contextRequested),
            crushed(&window, &PetWindow::crushed);
        window.setBottomMode(true);
        QVERIFY(!model.dragging);
        QVERIFY(!sendMouse(window, QEvent::MouseButtonPress, Qt::LeftButton));
        QVERIFY(!sendMouse(window, QEvent::MouseButtonPress, Qt::RightButton));
        QVERIFY(!sendMouse(window, QEvent::MouseMove, Qt::LeftButton, {210, 190}));
        QVERIFY(!sendMouse(window, QEvent::MouseButtonRelease, Qt::LeftButton));
        QVERIFY(!sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton));
        QVERIFY(!model.dragging);
        QCOMPARE(model.position, position);
        QCOMPARE(model.state, state);
        QCOMPARE(context.count(), 0);
        QCOMPARE(crushed.count(), 0);
        window.setBottomMode(false);
        sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton);
        QCOMPARE(model.state, State::Crush);
        QCOMPARE(crushed.count(), 1);
    }
    void juvenilesAndPettingNeverEmitCrushed() {
        PetModel model(2, {400, 300}, {0, 0, 800, 600}, true);
        PetWindow window(model, *animation_);
        window.present();
        QSignalSpy crushed(&window, &PetWindow::crushed);
        sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton);
        QCOMPARE(crushed.count(), 0);
        QVERIFY(model.state != State::Crush);
        model.advance(model.growthDuration(), {});
        QVERIFY(!model.juvenile);
        QVERIFY(model.startPetting());
        sendMouse(window, QEvent::MouseButtonPress, Qt::LeftButton);
        QVERIFY(!model.dragging);
        sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton);
        QCOMPARE(crushed.count(), 0);
        QVERIFY(model.petting);
        window.setBottomMode(true);
        model.advance(2, {});
        window.present();
        QVERIFY(!model.petting);
        checkMode(window, true, true);
        sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton);
        QCOMPARE(crushed.count(), 0);
        window.setBottomMode(false);
        sendMouse(window, QEvent::MouseButtonDblClick, Qt::LeftButton);
        QCOMPARE(crushed.count(), 1);
    }
    void visitorRoundTripStaysReadOnly() {
        PetWindow visitor(*animation_);
        PetRenderState state;
        state.position = QGuiApplication::primaryScreen()->availableGeometry().center();
        visitor.presentRemote(state, "mode-visitor", false);
        visitor.show();
        const QRect bounds = visitor.geometry();
        QSignalSpy context(&visitor, &PetWindow::contextRequested),
            crushed(&visitor, &PetWindow::crushed);
        for (bool bottom : {true, false}) {
            visitor.setBottomMode(bottom);
            QCOMPARE(visitor.bottomMode(), bottom);
            checkMode(visitor, bottom, true);
            QVERIFY(visitor.isVisible());
            QCOMPARE(visitor.geometry(), bounds);
            QVERIFY(!sendMouse(visitor, QEvent::MouseButtonPress, Qt::RightButton));
            QVERIFY(!sendMouse(visitor, QEvent::MouseButtonDblClick, Qt::LeftButton));
        }
        QCOMPARE(context.count(), 0);
        QCOMPARE(crushed.count(), 0);
        visitor.hidePet();
        for (bool bottom : {true, false}) {
            visitor.setBottomMode(bottom);
            QVERIFY(!visitor.isVisible());
            checkMode(visitor, bottom, true);
        }
    }
    void hiddenPetsAndHighlightsStayHidden() {
        PetModel model(3, {400, 300}, {0, 0, 800, 600});
        model.customName = "mode-hidden-highlight";
        PetWindow window(model, *animation_);
        window.present();
        const QRect bounds = window.geometry();
        for (bool bottom : {true, false}) {
            window.setBottomMode(bottom);
            QVERIFY(!window.isVisible());
            QCOMPARE(window.geometry(), bounds);
        }
        window.show();
        window.highlight();
        auto *label = highlightLabel(model.displayName());
        QVERIFY(label);
        QVERIFY(label->isVisible());
        model.dispatchPaused = true;
        window.hidePet();
        for (bool bottom : {true, false}) {
            window.setBottomMode(bottom);
            window.highlight();
            window.present();
            QVERIFY(!window.isVisible());
            QVERIFY(!label->isVisible());
        }
    }
    void highlightInheritsModeAndKeepsDeadline() {
        PetModel model(4, {400, 300}, {0, 0, 800, 600});
        model.customName = "mode-timed-highlight";
        PetWindow window(model, *animation_);
        window.present();
        window.setBottomMode(true);
        window.show();
        window.highlight();
        auto *label = highlightLabel(model.displayName());
        QVERIFY(label);
        QVERIFY(label->isVisible());
        checkMode(*label, true, true);
        const QRect bounds = label->geometry();
        QTest::qWait(1700);
        window.present();
        window.setBottomMode(false);
        checkMode(*label, false, true);
        QVERIFY(label->isVisible());
        QCOMPARE(label->geometry(), bounds);
        window.setBottomMode(true);
        checkMode(*label, true, true);
        QVERIFY(label->isVisible());
        QTest::qWait(1500);
        window.present();
        QVERIFY(!label->isVisible());
        window.setBottomMode(false);
        QVERIFY(!label->isVisible());
    }
    void frozenVisitorHighlightDoesNotExpireOnModeChange() {
        PetWindow visitor(*animation_);
        PetRenderState state;
        state.position = QGuiApplication::primaryScreen()->availableGeometry().center();
        visitor.presentRemote(state, "mode-frozen-highlight", false);
        visitor.show();
        visitor.highlight();
        auto *label = highlightLabel("mode-frozen-highlight");
        QVERIFY(label);
        visitor.presentRemote(state, "mode-frozen-highlight", true);
        QTest::qWait(3100);
        visitor.setBottomMode(true);
        visitor.presentRemote(state, "mode-frozen-highlight", true);
        QVERIFY(label->isVisible());
        visitor.setBottomMode(false);
        checkMode(*label, false, true);
        visitor.presentRemote(state, "mode-frozen-highlight", false);
        QVERIFY(label->isVisible());
    }
    void crackRoundTripPreservesEffectAgeAndRetriggerMode() {
        CrackWindow crack;
        checkMode(crack, false, true);
        crack.setBottomMode(true);
        QVERIFY(!crack.isVisible());
        crack.trigger({400, 300});
        QVERIFY(crack.bottomMode());
        checkMode(crack, true, true);
        QVERIFY(crack.isVisible());
        const QRect bounds = crack.geometry();
        const QImage initial = crack.grab().toImage();
        crack.advance(.4);
        QVERIFY(crack.grab().toImage() != initial);
        crack.setBottomMode(false);
        checkMode(crack, false, true);
        QVERIFY(crack.isVisible());
        QCOMPARE(crack.geometry(), bounds);
        crack.advance(.94);
        QVERIFY(crack.isVisible());
        crack.setBottomMode(true);
        crack.advance(.02);
        QVERIFY(!crack.isVisible());
        crack.setBottomMode(false);
        QVERIFY(!crack.isVisible());
        crack.setBottomMode(true);
        crack.trigger({430, 320});
        crack.trigger({420, 310});
        checkMode(crack, true, true);
        QCOMPARE(crack.pos(), QPoint(100, -10));
        QVERIFY(crack.isVisible());
        crack.advance(1.34);
        QVERIFY(crack.isVisible());
        crack.advance(.02);
        QVERIFY(!crack.isVisible());
    }
};
QTEST_MAIN(WindowModeTest)
#include "TestWindowMode.moc"
