#pragma once
#include "NetworkService.h"
#include "PetRenderState.h"
#include "SwarmController.h"
#include <QHash>
#include <QSet>
#include <functional>

namespace pettime {
class DispatchTest;
class DispatchController : public QObject {
    Q_OBJECT
  public:
    static constexpr int VisitorLimit = 144;
    static constexpr qint64 GraceMs = 30000;
    enum class Phase { Offering, LeavingLocal, Activating, Active, LeavingRemote, AwaitingHidden };
    struct Outgoing {
        quint64 entity = 0;
        QString id, peer, session, stream;
        Phase phase = Phase::Offering;
        QPointF home;
        QRectF homeArea, remoteArea;
        qint64 started = 0, frozenAt = -1;
        quint64 sequence = 0;
        bool awaitingActive = false, localHidden = false, recallRequested = false;
        int homeEdge = 0;
        double homeFraction = .5;
    };
    struct Visitor {
        QString id, peer, session, entity, name, sourceName, stream, pendingStream;
        PetRenderState previous, current;
        bool active = false, frozen = false, awaitingState = false;
        bool hidden = false, finishing = false, returnRequested = false;
        int stage = 0;
        qint64 returnRequestedAt = -1;
        qint64 created = 0, lastState = 0, frozenAt = -1;
        quint64 sequence = 0, highlight = 0;
    };
    DispatchController(NetworkService &network, SwarmController &swarm,
                       std::function<QRectF()> surface, std::function<qint64()> now = {},
                       QObject *parent = nullptr);
    ~DispatchController() override;
    QString dispatchPets(const QList<quint64> &ids, const QString &peer);
    void recallPet(quint64 id);
    void recallAll();
    bool expelVisitor(const QString &id);
    void removeEntity(quint64 id);
    bool highlightPet(quint64 id);
    bool isAway(quint64 id) const;
    bool isLocalVisible(quint64 id) const;
    int awayCount() const;
    bool canControl(quint64 id) const;
    bool canHighlight(quint64 id) const;
    QString stateText(quint64 id) const;
    QString visitorStateText(const Visitor &visitor) const;
    QString location(quint64 id) const;
    QPointF savedPosition(quint64 id) const;
    void updateLocalSurface(quint64 id, QRectF area);
    void requestStop();
    bool stoppingService() const { return stopping_; }
    // Installed by the window owner. Callbacks synchronously hide the window AND
    // its highlight; deletion may be deferred. With no callbacks this is headless.
    std::function<void(quint64)> hideLocal;
    std::function<void(const QString &)> hideVisitor;
    std::function<QRectF(QPointF)> localSurface;
    const QHash<quint64, Outgoing> &outgoing() const { return outgoing_; }
    const QHash<QString, Visitor> &visitors() const { return visitors_; }
    PetRenderState interpolated(const Visitor &v) const;
    void tick();
    void shutdown();
  signals:
    void changed();
    void notice(QString text);
    void visitorHighlight(QString id);

  private:
    friend class DispatchTest;
    struct Health {
        QString session, probe;
        bool monitoring = false;
        qint64 lastAck = 0, lastRx = 0, probeAt = 0, nextProbe = 0;
    };
    qint64 now() const;
    bool send(const QString &peer, const QString &op, QJsonObject data = {},
              const QString &key = {});
    void receive(const QString &peer, const QString &session, const QJsonObject &message);
    void ready(const QString &peer, const QString &session);
    void lost(const QString &peer);
    void freeze(Outgoing &out);
    void freeze(Visitor &visitor);
    void finish(quint64 id, bool restore = true);
    void eraseVisitor(const QString &id);
    void resume(Outgoing &out);
    void activate(quint64 id);
    void beginReturn(quint64 id);
    void sendExit(quint64 id);
    void receiveStates(const QString &peer, const QString &session, const QJsonObject &message);
    void publishStates();
    QString peerName(const QString &peer) const;
    NetworkService &network_;
    SwarmController &swarm_;
    std::function<QRectF()> surface_;
    std::function<qint64()> now_;
    QElapsedTimer clock_;
    QHash<quint64, Outgoing> outgoing_;
    QHash<QString, Visitor> visitors_;
    QHash<QString, Health> health_;
    QHash<QString, QSet<QString>> ended_;
    QRectF lastSurface_;
    qint64 nextState_ = 0, nextRenew_ = 0;
    quint64 highlightSerial_ = 0;
    bool stopping_ = false;
};
} // namespace pettime
