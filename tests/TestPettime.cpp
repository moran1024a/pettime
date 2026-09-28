#include "AnimationLibrary.h"
#include "ApplicationController.h"
#include "PetWindow.h"
#include "SettingsStore.h"
#include "SwarmController.h"
#include <QFile>
#include <QInputDialog>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <memory>

using namespace pettime;
class TestPettime : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { animation_ = std::make_unique<AnimationLibrary>(); }
    void feedingSavesOnlyAfterEating() {
        PetModel p(5, {500, 500}, {0, 0, 1000, 1000});
        QVERIFY(p.feed());
        QVERIFY(!p.feed());
        QCOMPARE(p.affection, 0);
        for (int i = 0; i < 1500 && p.mealActive; ++i)
            p.advance(.02, {-1000, -1000});
        QVERIFY(!p.mealActive);
        QVERIFY(p.affection >= 3 && p.affection <= 5);
        QCOMPARE(p.state, State::Happy);
        p.affection = 99;
        QVERIFY(p.feed());
        for (int i = 0; i < 1500 && p.mealActive; ++i)
            p.advance(.02, {-1000, -1000});
        QCOMPARE(p.affection, 100);
    }
    void pausedFlightFreezesFrame() {
        PetModel p(7, {500, 500}, {0, 0, 1000, 1000});
        p.demoPounce();
        p.advance(.1, {});
        p.paused = true;
        const auto state = p.state;
        const auto pos = p.position;
        const auto age = p.actionAge;
        const QImage image = animation_->render(p);
        for (int i = 0; i < 500; ++i)
            p.advance(.02, {});
        QCOMPARE(p.state, state);
        QCOMPARE(p.actionAge, age);
        QCOMPARE(p.position, pos);
        QCOMPARE(animation_->render(p), image);
        p.paused = false;
        bool impact = false;
        for (int i = 0; i < 200; ++i) {
            p.advance(.02, {-1000, -1000});
            impact |= p.impact;
        }
        QVERIFY(impact);
    }
    void feedingAtEdgeAndMenuPause() {
        PetModel p(4, {20, 20}, {0, 0, 800, 600});
        QVERIFY(p.feed());
        p.menuOpen = true;
        for (int i = 0; i < 2000; ++i)
            p.advance(.02, {});
        QVERIFY(p.mealActive);
        QCOMPARE(p.affection, 0);
        p.menuOpen = false;
        for (int i = 0; i < 1500 && p.mealActive; ++i)
            p.advance(.02, {-1000, -1000});
        QVERIFY(p.affection >= 3);
    }
    void growthBoundaries() {
        QCOMPARE(PetModel::growthScale(-1), MinimumScale);
        QCOMPARE(PetModel::growthScale(59.99), MinimumScale);
        QVERIFY(PetModel::growthScale(60) > MinimumScale);
        QCOMPARE(PetModel::growthScale(360), AdultScale);
        QCOMPARE(PetModel::growthScale(100000), AdultScale);
    }
    void chaseProbability() {
        QCOMPARE(PetModel::chaseChance(2), 0.0);
        QCOMPARE(PetModel::chaseChance(3), .12);
        QCOMPARE(PetModel::chaseChance(100), .7);
        PetModel p(1, {500, 500}, {0, 0, 1000, 1000});
        p.advance(.01, p.position + QPointF(30, 0));
        p.advance(.01, p.position + QPointF(40, 0));
        QCOMPARE(p.evadeCount, 1);
        p.paused = true;
        p.advance(6, {});
        QCOMPARE(p.evadeCount, 0);
    }
    void crushAndSwarmLimit() {
        PetModel main(12, {500, 500}, {0, 0, 1000, 1000});
        main.crush();
        main.advance(1.06, {});
        QVERIFY(main.splitReady);
        main.becomeNymph();
        SwarmController swarm(12);
        swarm.spawn(main.position, main.area, main.generation, 9);
        QCOMPARE(swarm.totalCount(), 10);
        for (int round = 0; round < 12; ++round) {
            for (const auto &p : swarm.pets())
                p->crush();
            for (int i = 0; i < 400; ++i) {
                swarm.advance(.02);
                QVERIFY(swarm.totalCount() <= 72);
            }
            QCOMPARE(swarm.pendingCount(), std::size_t(0));
        }
        QVERIFY(swarm.totalCount() > 48);
    }
    void swarmOverflowDoesNotOverRetire() {
        SwarmController swarm(1);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 1, 71);
        swarm.spawn({500, 500}, {0, 0, 1000, 1000}, 2, 10);
        for (int i = 0; i < 10; ++i)
            swarm.advance(.02);
        int retiring = 0;
        for (const auto &p : swarm.pets())
            if (p->state == State::Fade)
                ++retiring;
        QCOMPARE(retiring, 10);
        for (int i = 0; i < 40; ++i)
            swarm.advance(.02);
        QCOMPARE(swarm.totalCount(), 72);
        QCOMPARE(swarm.pendingCount(), std::size_t(0));
    }
    void identityAndDynamicLimit() {
        SwarmController swarm(4);
        auto *primary = &swarm.primary();
        primary->affection = 45;
        QCOMPARE(primary->id, std::uint64_t(1));
        QCOMPARE(primary->displayName(), QSysInfo::machineHostName());
        QVERIFY(!swarm.rename(1, "other"));
        swarm.spawn(primary->position, primary->area, 1, 71);
        QVERIFY(swarm.rename(2, "  小强  "));
        QCOMPARE(swarm.find(2)->displayName(), QString("小强"));
        QVERIFY(swarm.rename(2, ""));
        QCOMPARE(swarm.find(2)->displayName(), QString("蟑螂 #2"));
        std::vector<std::uint64_t> removed;
        swarm.beforeRemove = [&](std::uint64_t id) {
            QVERIFY(swarm.find(id)); // Notification precedes model destruction.
            QVERIFY(id != 1);
            removed.push_back(id);
        };
        swarm.spawn(primary->position, primary->area, 2, 12);
        swarm.setLimit(3);
        QCOMPARE(swarm.totalCount(), 3);
        QCOMPARE(swarm.pendingCount(), std::size_t(0));
        QCOMPARE(removed.front(), std::uint64_t(2));
        swarm.setLimit(1);
        for (int i = 0; i < 20; ++i) {
            primary->crush();
            primary->advance(1.1, {});
            QVERIFY(primary->splitReady);
            primary->becomeNymph();
            swarm.spawn(primary->position, primary->area, primary->generation, 12);
            swarm.advance(2);
            QCOMPARE(swarm.totalCount(), 1);
            QCOMPARE(&swarm.primary(), primary);
            QCOMPARE(primary->affection, 45);
        }
        swarm.setLimit(2);
        swarm.spawn(primary->position, primary->area, 1, 12);
        QVERIFY(swarm.pets().back()->id > 72);
        for (int i = 0; i < 15; ++i) {
            swarm.pets().back()->crush();
            swarm.advance(1.1);
            QCOMPARE(swarm.totalCount(), 2);
            QCOMPARE(swarm.pendingCount(), std::size_t(0));
        }
    }
    void cosmeticFeedingDoesNotChangeSimulation() {
        for (bool isPrimary : {false, true}) {
            PetModel p(9, {500, 500}, {0, 0, 1000, 1000}, !isPrimary);
            p.primary = isPrimary;
            p.affection = 38;
            const PetModel before = p;
            QVERIFY(p.playFeeding());
            QVERIFY(!p.playFeeding());
            QVERIFY(!p.feed());
            for (int i = 0; i < 133; ++i)
                p.advance(.02, {});
            QVERIFY(!p.cosmeticFeeding);
            QCOMPARE(p.affection, 38);
            QCOMPARE(p.state, before.state);
            QCOMPARE(p.position, before.position);
            QCOMPARE(p.actionAge, before.actionAge);
            QCOMPARE(p.generation, before.generation);
            QCOMPARE(p.splitReady, false);
            QVERIFY(p.age > before.age); // Natural growth still uses wall time.
            PetModel copy = before;
            QCOMPARE(p.random(0, 1), copy.random(0, 1));
            p.crush();
            QVERIFY(!p.playFeeding());
        }
    }
    void entityListAndLimitMenu() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QVERIFY(store.saveLimit(3));
        RunOptions options;
        options.population = 3;
        ApplicationController controller(*animation_, store, options);
        controller.start();
        QAction *listAction = nullptr, *limitAction = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *menu = qobject_cast<QMenu *>(widget))
                for (auto *action : menu->actions()) {
                    if (action->text() == "蟑螂列表…")
                        listAction = action;
                    if (action->text().startsWith("数量上限…"))
                        limitAction = action;
                }
        QVERIFY(listAction && limitAction);
        listAction->trigger();
        QDialog *dialog = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->windowTitle() == "蟑螂列表")
                dialog = qobject_cast<QDialog *>(widget);
        QVERIFY(dialog);
        auto *table = dialog->findChild<QTableWidget *>();
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->currentRow(), 0);
        QCOMPARE(table->item(0, 1)->text(), QSysInfo::machineHostName());
        QPushButton *rename = nullptr, *feed = nullptr;
        for (auto *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() == "重命名")
                rename = button;
            if (button->text() == "播放投喂动画")
                feed = button;
        }
        QVERIFY(rename && feed);
        QVERIFY(!rename->isEnabled());
        table->selectRow(1);
        QVERIFY(rename->isEnabled());
        QTimer::singleShot(0, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *input = qobject_cast<QInputDialog *>(widget)) {
                    input->setTextValue("测试小强");
                    input->accept();
                }
        });
        rename->click();
        QCOMPARE(table->item(1, 1)->text(), QString("测试小强"));
        feed->click();
        QCOMPARE(table->item(1, 3)->text(), QString("投喂动画"));
        QVERIFY(!feed->isEnabled());
        QTimer::singleShot(0, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *input = qobject_cast<QInputDialog *>(widget)) {
                    input->setIntValue(1);
                    input->accept();
                }
        });
        limitAction->trigger();
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->currentRow(), -1);
        QVERIFY(!feed->isEnabled());
        QCOMPARE(store.loadLimit(), 1);
        QCOMPARE(store.loadAffinity(), 0);
        listAction->trigger();
        QCOMPARE(table->currentRow(), 0);
    }
    void screenRecoveryAndDeterminism() {
        PetModel a(42, {1000, 500}, {0, 0, 1920, 1080}), b = a;
        for (int i = 0; i < 1000; ++i) {
            a.advance(.016, {-1000, -1000});
            b.advance(.016, {-1000, -1000});
        }
        QCOMPARE(a.position, b.position);
        a.setArea({-640, -480, 640, 480});
        QVERIFY(a.area.contains(a.position));
        a.setArea({0, 0, 20, 20});
        a.advance(.02, {});
        QVERIFY(std::isfinite(a.position.x()));
        QVERIFY(a.area.contains(a.position));
    }
    void animationFrames() {
        QCOMPARE(animation_->walk.size(), std::size_t(72));
        QCOMPARE(animation_->idle.size(), std::size_t(48));
        QCOMPARE(animation_->nymph.size(), std::size_t(48));
        QCOMPARE(animation_->frontPounce.size(), std::size_t(72));
        QCOMPARE(animation_->crush.size(), std::size_t(16));
        for (const auto *group :
             {&animation_->walk, &animation_->idle, &animation_->nymph, &animation_->nymphIdle,
              &animation_->pounce, &animation_->frontPounce, &animation_->crush,
              &animation_->nymphCrush, &animation_->nymphPounce})
            for (const auto &frame : *group) {
                QVERIFY(!frame.isNull());
                QVERIFY(!AnimationLibrary::alphaBounds(frame).isEmpty());
                QCOMPARE(qAlpha(frame.pixel(0, 0)), 0);
                QCOMPARE(frame.format(), QImage::Format_ARGB32_Premultiplied);
            }
    }
    void renderAndInputMask() {
        PetModel p(1, {0, 0}, {-1000, -1000, 2000, 2000});
        for (double dpr : {1.0, 1.5, 2.0}) {
            const QImage image = animation_->render(p, 320, dpr);
            QCOMPARE(image.width(), qCeil(320 * dpr));
            const auto region = PetWindow::inputRegion(image, {320, 320});
            QVERIFY(!region.contains({0, 0}));
            QVERIFY(region.contains({160, 160}));
        }
        p.becomeNymph();
        p.age = 360;
        p.advance(.001, {});
        for (int i = 0; i < 36; ++i) {
            p.heading = i * Pi / 18;
            const auto image = animation_->render(p, 224);
            const auto bounds = AnimationLibrary::alphaBounds(image, 0, 0);
            QVERIFY(bounds.left() > 0 && bounds.top() > 0 && bounds.right() < 223 &&
                    bounds.bottom() < 223);
        }
    }
    void storageFailureAndRecovery() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("data");
        SettingsStore store(path);
        QString error;
        QCOMPARE(store.loadAffinity(), 0);
        QCOMPARE(store.loadLimit(), 72);
        QVERIFY(store.saveLimit(1, &error));
        QCOMPARE(store.loadLimit(), 1);
        QVERIFY(store.saveLimit(72, &error));
        QCOMPARE(store.loadLimit(), 72);
        QFile limitFile(path + "/population-limit.dat");
        QVERIFY(limitFile.open(QIODevice::WriteOnly));
        limitFile.write("0");
        limitFile.close();
        QCOMPARE(store.loadLimit(&error), 72);
        QVERIFY(!error.isEmpty());
        QVERIFY(store.saveAffinity(24, &error));
        QCOMPARE(store.loadAffinity(), 24);
        QVERIFY(store.saveAffinity(200, &error));
        QCOMPARE(store.loadAffinity(), 100);
        QFile blocker(dir.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("unchanged");
        blocker.close();
        SettingsStore invalid(blocker.fileName() + "/child");
        QVERIFY(!invalid.saveAffinity(42, &error));
        QVERIFY(!invalid.saveLimit(12, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), QByteArray("unchanged"));
        QFile corrupt(path + "/affinity.dat");
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("invalid");
        corrupt.close();
        error.clear();
        QCOMPARE(store.loadAffinity(&error), 0);
        QVERIFY(!error.isEmpty());
    }
    void singleInstanceRecallAndCrashRecovery() {
        QTemporaryDir dir;
        const QString executable = QCoreApplication::applicationDirPath() + "/instance_probe";
        QProcess first;
        first.start(executable, {dir.path()});
        QVERIFY(first.waitForStarted());
        QVERIFY(first.waitForReadyRead(5000));
        QCOMPARE(first.readLine(), QByteArray("READY\n"));
        QProcess second;
        second.start(executable, {dir.path()});
        QVERIFY(second.waitForFinished(7000));
        QCOMPARE(second.exitCode(), 0);
        QCOMPARE(second.readAllStandardOutput(), QByteArray("RECALLED\n"));
        first.kill();
        QVERIFY(first.waitForFinished());
        QProcess recovery;
        recovery.start(executable, {dir.path()});
        QVERIFY(recovery.waitForStarted());
        QVERIFY(recovery.waitForReadyRead(5000));
        QCOMPARE(recovery.readLine(), QByteArray("READY\n"));
        recovery.kill();
        QVERIFY(recovery.waitForFinished());
    }

  private:
    std::unique_ptr<AnimationLibrary> animation_;
};
QTEST_MAIN(TestPettime)
#include "TestPettime.moc"
