#pragma once
#include "AnimationLibrary.h"
#include "CrackWindow.h"
#include "PetWindow.h"
#include "NetworkPanel.h"
#include "SettingsStore.h"
#include "SwarmController.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QMenu>
#include <QObject>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <map>

namespace pettime {
struct RunOptions {
    bool smoke = false;
    int population = 1;
    double duration = 6;
    QString output;
    std::uint32_t seed = 1;
};
class ApplicationController : public QObject {
    Q_OBJECT
  public:
    ApplicationController(const AnimationLibrary &animation, SettingsStore store,
                          RunOptions options, QObject *parent = nullptr);
    ~ApplicationController() override;
    void start();
    void recall();

  private:
    void tick();
    void makeMenu();
    void showPets();
    void showNetwork();
    void refreshPets();
    void changeLimit();
    std::uint64_t selectedPet() const;
    void refreshMenu();
    void persist();
    void updateScreens();
    void reconcileWindows();
    void finishSmoke();
    void warn(const QString &message);
    static QRectF areaAt(QPointF point);
    const AnimationLibrary &animation_;
    SettingsStore store_;
    RunOptions options_;
    SwarmController swarm_;
    PetModel &primary_;
    PetWindow mainWindow_;
    CrackWindow crack_;
    std::map<std::uint64_t, std::unique_ptr<PetWindow>> windows_;
    std::unique_ptr<NetworkService> network_;
    std::unique_ptr<NetworkPanel> networkPanel_;
    std::unique_ptr<QDialog> petDialog_;
    QTableWidget *petTable_ = nullptr;
    QLabel *petCount_ = nullptr;
    QPushButton *renamePet_ = nullptr, *highlightPet_ = nullptr, *feedPet_ = nullptr;
    std::uint64_t contextPet_ = 1;
    QMenu menu_;
    QSystemTrayIcon tray_;
    QAction *pause_ = nullptr, *affection_ = nullptr, *feed_ = nullptr, *demo_ = nullptr,
            *retry_ = nullptr, *limitAction_ = nullptr;
    QTimer timer_;
    QElapsedTimer clock_;
    double last_ = 0, lastSwarm_ = 0, nextSwarm_ = 0, retryAt_ = 0;
    double totalTickMs_ = 0, maxTickMs_ = 0;
    int ticks_ = 0, swarmUpdates_ = 0, recalls_ = 0, impacts_ = 0;
    bool dirty_ = false, saveWarning_ = false, smokePounce_ = false;
    QString startupWarning_;
};
} // namespace pettime
