#pragma once
#include "SettingsStore.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <functional>
#include <memory>

namespace pettime {
class NetworkTest;
class NetworkService : public QObject {
    Q_OBJECT
  public:
    struct Info {
        int count = 1;
        int limit = 72;
    };
    struct Device {
        QString id, name, address, state;
        quint16 port = 0;
        int count = 0, limit = 0;
        QDateTime lastContact;
    };
    // Nondefault options are for isolated integration tests, not application settings.
    struct Options {
        quint16 port = 21012;
        bool loopbackOnly = false;
        QList<QPair<QHostAddress, quint16>> discoveryTargets;
        std::function<qint64()> now;
    };
    NetworkService(SettingsStore store, std::function<Info()> snapshot, QObject *parent = nullptr);
    NetworkService(SettingsStore store, std::function<Info()> snapshot, Options options,
                   QObject *parent = nullptr);
    ~NetworkService() override;
    bool start();
    void stop();
    void refresh();
    bool running() const { return running_; }
    QString status() const { return status_; }
    QStringList addresses() const;
    QList<Device> devices() const;
    Info localInfo() const { return snapshot_(); }
    quint16 port() const { return options_.port; }
  signals:
    void changed();

  private:
    friend class NetworkTest;
    struct Endpoint {
        QHostAddress address;
        quint16 port;
    };
    struct Peer {
        Device device;
        QString session;
        QList<Endpoint> endpoints;
        QTcpSocket *socket = nullptr;
        qint64 lastSeen = 0, retryAt = 0, manualAt = -1000;
        int failures = 0, addressIndex = 0;
        double revision = 0;
    };
    struct Connection {
        QTcpSocket *socket = nullptr;
        QByteArray input;
        QString peer, session, token, ping, query;
        bool outbound = false, ready = false, scheduled = false;
        qint64 created = 0, connected = 0, readyAt = 0, lastRx = 0;
        qint64 pingAt = 0, queryAt = 0, rateAt = 0;
        int frames = 0;
        QJsonObject pendingInfo;
        QHash<QString, qint64> lateQueries;
    };
    qint64 now() const;
    bool enumerate();
    bool allowed(const QHostAddress &address) const;
    void discover(bool manual);
    void receiveDatagrams();
    void candidate(const QJsonObject &object, const QHostAddress &address, bool manual);
    void tryConnect(const QString &id);
    void attach(QTcpSocket *socket, bool outbound, const QString &id = {});
    void read(QTcpSocket *socket);
    bool handle(Connection &c, const QJsonObject &object);
    void drop(QTcpSocket *socket, const QString &reason);
    bool send(QTcpSocket *socket, QJsonObject object);
    QJsonObject localData();
    bool applyInfo(Peer &peer, const QJsonObject &object);
    void markReady(Connection &c);
    void tick();
    SettingsStore store_;
    std::function<Info()> snapshot_;
    Options options_;
    QTcpServer server_;
    QUdpSocket udp_;
    QTimer timer_;
    QElapsedTimer clock_;
    QList<QPair<QHostAddress, int>> subnets_;
    QList<QPair<QHostAddress, quint16>> targets_;
    QHash<QString, Peer> peers_;
    QHash<QTcpSocket *, std::shared_ptr<Connection>> connections_;
    QHash<QString, qint64> requests_, udpRates_;
    QString id_, session_, status_ = "已关闭";
    Info previous_;
    double revision_ = 1, publishedRevision_ = 0;
    bool running_ = false;
    qint64 nextDiscovery_ = 0, nextSample_ = 0, lastRefresh_ = -1000;
};
} // namespace pettime
