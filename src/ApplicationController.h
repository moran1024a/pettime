#pragma once
#include "AnimationLibrary.h"
#include "CrackWindow.h"
#include "DispatchController.h"
#include "FoodInventory.h"
#include "NetworkPanel.h"
#include "PetWindow.h"
#include "SettingsStore.h"
#include "SwarmController.h"
#include <QDialog>
#include <QComboBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QObject>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QSet>
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
    friend class DispatchTest;
    friend class FoodApplicationTest;
    friend class InteractionApplicationTest;
    enum class Interaction { Petting, Reproduce };
    QString interactionBlockReason(quint64 id, Interaction action) const;
    bool interact(quint64 id, Interaction action);
    bool settleInteractions(quint64 removing = 0);
    void showInteractions(quint64 id = 0);
    void refreshInteractions();
    bool eventFilter(QObject *object, QEvent *event) override;
    void stopControl();
    void clearControlKeys();
    void validateControl();
    bool canStartControl(quint64 id) const;
    bool canFeedPet(quint64 id) const;
    bool useFood(quint64 id);
    bool settleFood(quint64 removing = 0);
    void showFood(quint64 id = 0);
    void refreshFood();
    void setBottomMode(bool enabled);
    bool canDispatchPet(quint64 id) const;
    void tick();
    void makeMenu();
    void showPets();
    void selectPets(bool visitors);
    bool canClearPet(quint64 id) const;
    bool clearPet(quint64 id);
    void showNetwork();
    void refreshPets();
    void dispatchSelected();
    void reconcileVisitors();
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
    FoodInventory food_;
    QHash<quint64, quint64> feedingSerials_;
    QHash<quint64, quint64> pettingSerials_;
    std::mt19937 rewardRng_;
    double interactionCooldown_ = 0;
    bool progressChanged_ = false;
    PetModel &primary_;
    PetWindow mainWindow_;
    CrackWindow crack_;
    std::map<std::uint64_t, std::unique_ptr<PetWindow>> windows_;
    std::unique_ptr<NetworkService> network_;
    std::unique_ptr<DispatchController> dispatch_;
    std::map<QString, std::unique_ptr<PetWindow>> visitorWindows_;
    std::unique_ptr<NetworkPanel> networkPanel_;
    std::unique_ptr<QDialog> petDialog_;
    std::unique_ptr<QDialog> foodDialog_;
    std::unique_ptr<QDialog> interactionDialog_;
    QComboBox *interactionTarget_ = nullptr, *interactionAction_ = nullptr;
    QLabel *interactionDetails_ = nullptr, *interactionReason_ = nullptr,
           *interactionResult_ = nullptr;
    QPushButton *interactionUse_ = nullptr, *interactionPet_ = nullptr;
    QString lastInteractionResult_;
    QComboBox *foodTarget_ = nullptr;
    QLabel *foodStock_ = nullptr, *foodReason_ = nullptr;
    QPushButton *foodUse_ = nullptr;
    QTableWidget *petTable_ = nullptr;
    QLabel *petCount_ = nullptr, *dispatchStatus_ = nullptr;
    QPushButton *renamePet_ = nullptr, *highlightPet_ = nullptr, *foodPet_ = nullptr;
    QPushButton *dispatchPet_ = nullptr, *recallPet_ = nullptr, *recallAllPets_ = nullptr;
    QPushButton *clearPet_ = nullptr;
    QPushButton *expelPet_ = nullptr;
    QString selectedVisitor() const;
    QPushButton *controlPet_ = nullptr;
    QLabel *controlStatus_ = nullptr;
    quint64 controlledPet_ = 0;
    QString controlledGroup_;
    QSet<int> controlKeys_;
    std::uint64_t contextPet_ = 1;
    QMenu menu_;
    QSystemTrayIcon tray_;
    QAction *pause_ = nullptr, *affection_ = nullptr, *demo_ = nullptr,
            *retry_ = nullptr, *limitAction_ = nullptr;
    QAction *bottomAction_ = nullptr;
    QTimer timer_;
    QElapsedTimer clock_;
    double last_ = 0, lastSwarm_ = 0, nextSwarm_ = 0, nextVisitors_ = 0, retryAt_ = 0;
    double nextSave_ = 5;
    bool restoreNetwork_ = false;
    bool bottomMode_ = false;
    double totalTickMs_ = 0, maxTickMs_ = 0;
    int ticks_ = 0, swarmUpdates_ = 0, recalls_ = 0, impacts_ = 0;
    bool dirty_ = false, saveWarning_ = false, smokePounce_ = false;
    QString startupWarning_;
};
} // namespace pettime
