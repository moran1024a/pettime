#include "NetworkPanel.h"
#include "DispatchController.h"
#include <QFile>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

namespace pettime {
namespace {
const QString firstId = "10000000-0000-4000-8000-000000000001";
const QString secondId = "20000000-0000-4000-8000-000000000002";
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
quint16 freePort() {
    QTcpServer tcp;
    if (!tcp.listen(QHostAddress::LocalHost, 0))
        return 0;
    return tcp.serverPort();
}
void identity(const QString &dir, const QString &id) {
    QDir().mkpath(dir);
    QFile f(dir + "/network-device-id.dat");
    if (f.open(QIODevice::WriteOnly))
        f.write(id.toUtf8() + "\n");
}
QByteArray frame(QJsonObject o) {
    o["protocol"] = "pettime";
    if (!o.contains("version"))
        o["version"] = 1;
    const auto body = QJsonDocument(o).toJson(QJsonDocument::Compact);
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), bytes.data());
    return bytes + body;
}
QJsonObject hello(const QString &id = firstId) {
    return {{"type", "hello"},
            {"appVersion", NetworkService::applicationVersion()},
            {"deviceId", id},
            {"sessionId", uuid()},
            {"deviceName", "测试设备 <b>文本</b>"},
            {"tcpPort", 21012},
            {"petCount", 2},
            {"petLimit", 12},
            {"revision", 1},
            {"connectionId", uuid()}};
}
struct Pair {
    QTemporaryDir dir;
    qint64 time = 100000;
    NetworkService::Info ia{2, 12}, ib{3, 24};
    std::unique_ptr<NetworkService> a, b;
    Pair() {
        identity(dir.path() + "/a", firstId);
        identity(dir.path() + "/b", secondId);
        NetworkService::Options oa, ob;
        oa.loopbackOnly = ob.loopbackOnly = true;
        oa.port = freePort();
        do {
            ob.port = freePort();
        } while (ob.port == oa.port && ob.port);
        oa.discoveryTargets = {{QHostAddress::LocalHost, ob.port}};
        ob.discoveryTargets = {{QHostAddress::LocalHost, oa.port}};
        oa.now = ob.now = [this] { return time; };
        a = std::make_unique<NetworkService>(
            SettingsStore(dir.path() + "/a"), [this] { return ia; }, oa);
        b = std::make_unique<NetworkService>(
            SettingsStore(dir.path() + "/b"), [this] { return ib; }, ob);
    }
    bool start() { return a->start() && b->start(); }
    bool connected() const {
        return a->devices().size() == 1 && b->devices().size() == 1 &&
               a->devices()[0].state == "已连接" && b->devices()[0].state == "已连接";
    }
};
} // namespace
class NetworkTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }
    void manualMovementOverRealConnection() {
        Pair pair;
        SwarmController source(1, {400, 400}, {0, 0, 1280, 720});
        SwarmController target(2, {400, 400}, {0, 0, 1280, 720});
        source.spawn({400, 400}, {0, 0, 1280, 720}, 1, 1);
        DispatchController owner(*pair.a, source, [] { return QRectF(0, 0, 1280, 720); });
        DispatchController guest(*pair.b, target, [] { return QRectF(0, 0, 1280, 720); });
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] {
            owner.tick();
            guest.tick();
            source.advance(.02);
        });
        timer.start(20);
        QVERIFY(pair.start());
        QTRY_VERIFY(pair.connected());
        owner.dispatchPets({2}, pair.b->deviceId());
        QTRY_VERIFY(owner.outgoing().contains(2) &&
                    owner.outgoing()[2].phase == DispatchController::Phase::Active);
        const QString id = owner.outgoing()[2].id;
        QTRY_VERIFY(guest.visitors().contains(id) && guest.visitors()[id].sequence > 0);
        auto *p = source.find(2);
        QVERIFY(p->setManualControl(true));
        p->moveTo({400, 400}, p->area);
        p->setManualDirection({1, 0});
        QTRY_VERIFY(guest.visitors()[id].current.position.x() > 420);
        p->setManualDirection({});
        const auto stop = p->position;
        QTRY_VERIFY(QLineF(guest.visitors()[id].current.position, stop).length() < .01);
        QSignalSpy highlights(&guest, &DispatchController::visitorHighlight);
        QVERIFY(owner.highlightPet(2));
        QTRY_COMPARE(highlights.count(), 1);
        QVERIFY(p->playFeeding());
        QTRY_COMPARE(guest.visitors()[id].current.state, State::Food);
        QCOMPARE(target.totalCount(), 1);
        owner.recallPet(2);
        QTRY_VERIFY(owner.outgoing().isEmpty() && guest.visitors().isEmpty());
        QVERIFY(!owner.isAway(2));
    }
    void identityPersistenceAndErrors() {
        QTemporaryDir dir;
        SettingsStore store(dir.path());
        QString error;
        const auto id = store.networkDeviceId(&error);
        QVERIFY(!QUuid(id).isNull());
        QVERIFY(error.isEmpty());
        QCOMPARE(store.networkDeviceId(), id);
        identity(dir.path(), "broken");
        QVERIFY(store.networkDeviceId(&error).isEmpty());
        QVERIFY(!error.isEmpty());
        QFile blocker(dir.path() + "/file");
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY(SettingsStore(blocker.fileName()).networkDeviceId(&error).isEmpty());
    }
    void defaultOffAndPanelLifecycle() {
        Pair p;
        NetworkPanel panel(*p.a);
        panel.show();
        QVERIFY(!p.a->running());
        QVERIFY(!p.a->server_.isListening());
        auto *enabled = panel.findChild<QCheckBox *>("networkEnabled");
        auto *refresh = panel.findChild<QPushButton *>("networkRefresh");
        QVERIFY(enabled);
        QVERIFY(refresh);
        QVERIFY(!refresh->isEnabled());
        enabled->setChecked(true);
        QVERIFY(p.a->running());
        QVERIFY(refresh->isEnabled());
        panel.close();
        QVERIFY(p.a->running());
        panel.show();
        enabled->setChecked(false);
        QVERIFY(!p.a->running());
        QVERIFY(!p.a->server_.isListening());
        QVERIFY(!p.a->timer_.isActive());
        QVERIFY(p.a->connections_.isEmpty());
        QVERIFY(p.a->devices().isEmpty());
        QTcpServer tcp;
        QUdpSocket udp;
        QVERIFY(tcp.listen(QHostAddress::LocalHost, p.a->port()));
        QVERIFY(udp.bind(QHostAddress::LocalHost, p.a->port(), QUdpSocket::DontShareAddress));
    }
    void bothPortFailuresRollBack() {
        Pair p;
        QTcpServer tcp;
        QVERIFY(tcp.listen(QHostAddress::LocalHost, p.a->port()));
        QVERIFY(!p.a->start());
        QVERIFY(p.a->status().contains("TCP"));
        QVERIFY(!p.a->running());
        tcp.close();
        QUdpSocket udp;
        QVERIFY(udp.bind(QHostAddress::LocalHost, p.a->port(), QUdpSocket::DontShareAddress));
        QVERIFY(!p.a->start());
        QVERIFY(p.a->status().contains("UDP"));
        QVERIFY(!p.a->server_.isListening());
        QVERIFY(!p.a->timer_.isActive());
        udp.close();
        QVERIFY(p.a->start());
    }
    void differentVersionsRemainDiscoverableAndNeverConnect() {
        Pair p;
        p.b->options_.appVersion = "99.0.0";
        QVERIFY(p.start());
        QTRY_COMPARE(p.a->devices().size(), 1);
        QTRY_COMPARE(p.b->devices().size(), 1);
        QCOMPARE(p.a->devices()[0].appVersion, QString("99.0.0"));
        QCOMPARE(p.a->devices()[0].state, QString("版本不匹配，无法互联"));
        QCOMPARE(p.a->devices()[0].count, p.ib.count);
        QVERIFY(!p.a->devices()[0].name.isEmpty());
        QVERIFY(!p.a->dispatchReady(secondId));
        for (int i = 0; i < 15; ++i) {
            p.time += 10000;
            p.a->refresh();
            p.b->refresh();
            QTest::qWait(5);
            QVERIFY(p.a->connections_.isEmpty());
            QVERIFY(p.b->connections_.isEmpty());
        }
        QCOMPARE(p.a->devices().size(), 1);
        NetworkPanel panel(*p.a);
        const auto *table = panel.findChild<QTableWidget *>("networkDevices");
        QCOMPARE(table->item(0, 10)->text(), QString("99.0.0"));
        QCOMPARE(table->item(0, 5)->text(), QString("版本不匹配，无法互联"));
        p.b->stop();
        p.b->options_.appVersion = NetworkService::applicationVersion();
        QVERIFY(p.b->start());
        p.time += 1000;
        p.a->refresh();
        p.b->refresh();
        QTRY_VERIFY(p.connected());
    }
    void unknownAndFutureVersionsAreVisible() {
        Pair p;
        QVERIFY(p.b->start());
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QJsonObject o{{"protocol", "pettime"}, {"version", 1},        {"type", "discover"},
                      {"deviceId", firstId},   {"sessionId", uuid()}, {"requestId", uuid()},
                      {"tcpPort", p.a->port()}};
        auto publish = [&] {
            sender.writeDatagram(QJsonDocument(o).toJson(QJsonDocument::Compact),
                                 QHostAddress::LocalHost, p.b->port());
        };
        publish();
        QTRY_COMPARE(p.b->devices().size(), 1);
        QCOMPARE(p.b->devices()[0].state, QString("版本未知，无法互联"));
        QVERIFY(p.b->connections_.isEmpty());
        p.time += 1000;
        o["version"] = 2;
        o["appVersion"] = NetworkService::applicationVersion();
        publish();
        QTRY_COMPARE(p.b->devices()[0].state, QString("协议版本不匹配，无法互联"));
        QVERIFY(p.b->connections_.isEmpty());
    }
    void inboundHandshakeCannotBypassVersionGate_data() {
        QTest::addColumn<QString>("version");
        QTest::newRow("different") << QString("99.0.0");
        QTest::newRow("missing") << QString{};
    }
    void inboundHandshakeCannotBypassVersionGate() {
        QFETCH(QString, version);
        Pair p;
        QVERIFY(p.b->start());
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, p.b->port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        auto h = hello();
        if (version.isEmpty())
            h.remove("appVersion");
        else
            h["appVersion"] = version;
        client.write(frame(h));
        QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(p.b->connections_.isEmpty());
        QCOMPARE(p.b->devices().size(), 1);
        QVERIFY(!p.b->devices()[0].compatible);
    }
    void outboundHandshakeRechecksAdvertisedVersion() {
        Pair p;
        QTcpServer remote;
        QVERIFY(remote.listen(QHostAddress::LocalHost, p.b->port()));
        QVERIFY(p.a->start());
        const auto session = uuid();
        p.a->candidate({{"version", 1},
                        {"appVersion", NetworkService::applicationVersion()},
                        {"deviceId", secondId},
                        {"sessionId", session},
                        {"tcpPort", p.b->port()}},
                       QHostAddress::LocalHost, false);
        QTRY_VERIFY(remote.hasPendingConnections());
        std::unique_ptr<QTcpSocket> socket(remote.nextPendingConnection());
        QTRY_VERIFY(socket->bytesAvailable() > 4);
        const auto bytes = socket->readAll();
        const auto request = QJsonDocument::fromJson(bytes.mid(4)).object();
        auto reply = hello(secondId);
        reply["type"] = "helloAck";
        reply["sessionId"] = session;
        reply["connectionId"] = request["connectionId"];
        reply["appVersion"] = "99.0.0";
        socket->write(frame(reply));
        QTRY_VERIFY(p.a->connections_.isEmpty());
        QCOMPARE(p.a->devices()[0].state, QString("版本不匹配，无法互联"));
        QVERIFY(!p.a->devices()[0].compatible);
        p.a->tryConnect(secondId);
        QVERIFY(p.a->connections_.isEmpty());
    }
    void discoveryHandshakeRefreshAndPush() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        QCOMPARE(p.a->connections_.size(), 1);
        QCOMPARE(p.b->connections_.size(), 1);
        QCOMPARE(p.a->devices()[0].count, 3);
        QCOMPARE(p.b->devices()[0].limit, 12);
        auto *socket = p.a->peers_[secondId].socket;
        NetworkPanel panel(*p.a);
        panel.show();
        auto *button = panel.findChild<QPushButton *>("networkRefresh");
        QVERIFY(button);
        p.ib = {5, 15};
        p.time += 1000;
        button->click();
        QTRY_COMPARE(p.a->devices()[0].count, 5);
        QCOMPARE(p.a->peers_[secondId].socket, socket);
        p.ia = {7, 8};
        p.time += 1000;
        p.a->tick();
        p.b->tick();
        QTRY_COMPARE(p.b->devices()[0].count, 7);
        QCOMPARE(p.b->devices()[0].limit, 8);
        auto *table = panel.findChild<QTableWidget *>("networkDevices");
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 3)->text(), QString("5"));
        for (int i = 0; i < 5; ++i) {
            p.time += 1000;
            p.a->refresh();
            p.b->refresh();
            QTest::qWait(10);
            QCOMPARE(p.a->connections_.size(), 1);
            QCOMPARE(p.b->connections_.size(), 1);
            QCOMPARE(p.a->peers_[secondId].socket, socket);
        }
    }
    void tenMinutesOfHeartbeatsAndRefresh() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        auto *socket = p.a->peers_[secondId].socket;
        for (int i = 0; i < 600; ++i) {
            p.time += 1000;
            p.a->tick();
            p.b->tick();
            if (i % 17 == 0) {
                p.a->refresh();
                p.b->refresh();
            }
            QTest::qWait(2);
            QCOMPARE(p.a->peers_[secondId].socket, socket);
            QCOMPARE(p.a->connections_.size(), 1);
            QCOMPARE(p.b->connections_.size(), 1);
        }
        QVERIFY(p.connected());
    }
    void realClockConnectionStability() {
        const int seconds = qEnvironmentVariableIntValue("PETTIME_NETWORK_SOAK_SECONDS");
        if (seconds <= 0)
            return; // Explicit long-running validation; fast CTest uses the simulated clock.
        Pair p;
        p.a->options_.now = {};
        p.b->options_.now = {};
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        auto *socket = p.a->peers_[secondId].socket;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 refreshAt = 0;
        while (elapsed.elapsed() < qint64(seconds) * 1000) {
            if (elapsed.elapsed() >= refreshAt) {
                p.a->refresh();
                p.b->refresh();
                ++p.ib.count;
                if (p.ib.count > p.ib.limit)
                    p.ib.count = 1;
                refreshAt += 17000;
            }
            QTest::qWait(100);
            QCOMPARE(p.a->peers_[secondId].socket, socket);
            QCOMPARE(p.a->connections_.size(), 1);
            QCOMPARE(p.b->connections_.size(), 1);
        }
        QVERIFY(p.connected());
    }
    void stopRestartAndSessionChange() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        const auto oldSession = p.b->session_;
        p.b->stop();
        QTRY_VERIFY(!p.a->peers_[secondId].socket);
        QVERIFY(p.b->connections_.isEmpty());
        QVERIFY(!p.b->timer_.isActive());
        p.time += 2000;
        QVERIFY(p.b->start());
        QVERIFY(oldSession != p.b->session_);
        p.a->refresh();
        QTRY_VERIFY(p.connected());
        QCOMPARE(p.a->peers_[secondId].session, p.b->session_);
        p.a->stop();
        p.a->stop();
        p.time += 60000;
        p.a->tick();
        QVERIFY(p.a->connections_.isEmpty());
        QVERIFY(p.a->peers_.isEmpty());
        QVERIFY(!p.a->running());
    }
    void silentPeerTimeoutBackoffAndExpiry() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        auto *socket = p.a->peers_[secondId].socket;
        // Simulate loss of traffic without requiring OS network configuration changes.
        auto c = p.a->connections_.value(socket);
        c->lastRx = p.time - 30000;
        p.a->tick();
        QVERIFY(!p.a->peers_[secondId].socket);
        const auto retry = p.a->peers_[secondId].retryAt;
        QVERIFY(retry >= p.time + 1000 && retry <= p.time + 1250);
        p.a->tick();
        QVERIFY(!p.a->peers_[secondId].socket);
        p.b->stop();
        p.time = retry;
        p.a->tick();
        QTRY_VERIFY(!p.a->peers_[secondId].socket);
        QVERIFY(p.a->peers_[secondId].retryAt >= p.time + 2000);
        p.time += 120000;
        p.a->tick();
        QVERIFY(p.a->peers_.isEmpty());
    }
    void partialAndCoalescedFrames() {
        Pair p;
        p.time = 1000000; // A first inbound peer must also work after long service uptime.
        QVERIFY(p.b->start());
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, p.b->port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        const auto h = hello();
        const auto bytes = frame(h);
        client.write(bytes.left(2));
        QTest::qWait(10);
        QVERIFY(p.b->peers_.isEmpty());
        client.write(bytes.mid(2, 9));
        QTest::qWait(10);
        QVERIFY(p.b->peers_.isEmpty());
        client.write(bytes.mid(11));
        QTRY_VERIFY(client.bytesAvailable() > 4);
        client.readAll();
        p.b->tick();
        QVERIFY(!p.b->connections_.isEmpty());
        client.write(frame({{"type", "ready"}, {"connectionId", h["connectionId"]}}) +
                     frame({{"type", "getInfo"}, {"requestId", uuid()}}) +
                     frame({{"type", "ping"}, {"requestId", uuid()}}));
        QTRY_COMPARE(p.b->devices()[0].state, QString("已连接"));
        QTRY_VERIFY(client.bytesAvailable() > 4);
        QVERIFY(p.b->connections_.size() == 1);
        auto *incoming = p.b->peers_[firstId].socket;
        QVERIFY(p.b->connections_[incoming]->ready);
    }
    void invalidFrames_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("zero-length") << QByteArray(4, '\0');
        QByteArray large(4, '\0');
        qToBigEndian<quint32>(8193, large.data());
        QTest::newRow("oversize") << large;
        auto h = hello();
        h["version"] = 2;
        QTest::newRow("version") << frame(h);
        h = hello();
        h["petCount"] = 73;
        QTest::newRow("count") << frame(h);
        h = hello();
        h["revision"] = 1.5;
        QTest::newRow("fractional-revision") << frame(h);
        h = hello();
        h["sessionId"] = "bad";
        QTest::newRow("identity") << frame(h);
        QTest::newRow("pre-handshake-ping") << frame({{"type", "ping"}, {"requestId", uuid()}});
        QTest::newRow("wrong-direction") << frame(hello("30000000-0000-4000-8000-000000000003"));
    }
    void invalidFrames() {
        QFETCH(QByteArray, bytes);
        Pair p;
        QVERIFY(p.b->start());
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, p.b->port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write(bytes);
        QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(p.b->connections_.isEmpty());
        // Invalid traffic must not prevent subsequent legitimate discovery and handshake.
        QVERIFY(p.a->start());
        QTRY_VERIFY(p.connected());
    }
    void handshakeDeadlineAndConnectionLimit() {
        Pair p;
        QVERIFY(p.b->start());
        QList<std::shared_ptr<QTcpSocket>> clients;
        for (int i = 0; i < 17; ++i) {
            auto c = std::make_shared<QTcpSocket>();
            c->connectToHost(QHostAddress::LocalHost, p.b->port());
            clients.append(c);
            QTest::qWait(5);
        }
        QTRY_COMPARE(p.b->connections_.size(), 16);
        p.time += 3000;
        p.b->tick();
        QTRY_VERIFY(p.b->connections_.isEmpty());
    }
    void duplicateConnectionAndIdentityConflict() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        auto *existing = p.b->peers_[firstId].socket;
        QTcpSocket duplicate;
        duplicate.connectToHost(QHostAddress::LocalHost, p.b->port());
        QTRY_COMPARE(duplicate.state(), QAbstractSocket::ConnectedState);
        auto h = hello();
        h["sessionId"] = p.a->session_;
        duplicate.write(frame(h));
        QTRY_COMPARE(duplicate.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(p.b->peers_[firstId].socket, existing);
        p.b->candidate({{"version", 1},
                        {"appVersion", NetworkService::applicationVersion()},
                        {"deviceId", firstId},
                        {"sessionId", uuid()},
                        {"tcpPort", p.a->port()}},
                       QHostAddress::LocalHost, false);
        QCOMPARE(p.b->peers_[firstId].socket, existing);
        QCOMPARE(p.b->peers_[firstId].device.state, QString("身份冲突"));
        p.b->candidate({{"version", 1},
                        {"appVersion", NetworkService::applicationVersion()},
                        {"deviceId", secondId},
                        {"sessionId", uuid()},
                        {"tcpPort", p.a->port()}},
                       QHostAddress::LocalHost, false);
        QVERIFY(p.b->status().contains("重复设备 ID"));
    }
    void ignoresApplicationProxyForLocalTraffic() {
        Pair p;
        struct RestoreProxy {
            ~RestoreProxy() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }
        } restore;
        QNetworkProxy::setApplicationProxy(
            QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", 1));
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        QCOMPARE(p.a->udp_.localPort(), p.a->port());
    }
    void unsolicitedDiscoveryAndOversizedDatagrams() {
        Pair p;
        QVERIFY(p.b->start());
        QUdpSocket sender;
        sender.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        QJsonObject o{{"appVersion", NetworkService::applicationVersion()},
                      {"protocol", "pettime"},
                      {"version", 1},
                      {"type", "announce"},
                      {"deviceId", firstId},
                      {"sessionId", uuid()},
                      {"requestId", uuid()},
                      {"tcpPort", p.a->port()}};
        sender.writeDatagram(QJsonDocument(o).toJson(QJsonDocument::Compact),
                             QHostAddress::LocalHost, p.b->port());
        sender.writeDatagram(QByteArray(1100, 'x'), QHostAddress::LocalHost, p.b->port());
        QTest::qWait(20);
        QVERIFY(p.b->peers_.isEmpty());
        o["type"] = "discover";
        sender.writeDatagram(QJsonDocument(o).toJson(QJsonDocument::Compact),
                             QHostAddress::LocalHost, p.b->port());
        QTRY_COMPARE(p.b->peers_.size(), 1);
        QVERIFY(p.b->connections_.isEmpty());
        const auto originalSeen = p.b->peers_[firstId].lastSeen;
        p.time += 10000;
        sender.writeDatagram(QJsonDocument(o).toJson(QJsonDocument::Compact),
                             QHostAddress::LocalHost, p.b->port());
        QTest::qWait(20);
        QCOMPARE(p.b->peers_[firstId].lastSeen, originalSeen);
    }
    void revisionsAndMessageRateLimit() {
        Pair p;
        QVERIFY(p.start());
        QTRY_VERIFY(p.connected());
        auto *socket = p.a->peers_[secondId].socket;
        auto info = p.a->localData();
        info["type"] = "infoChanged";
        info["revision"] = 5;
        info["petCount"] = 9;
        QVERIFY(p.a->send(socket, info));
        QTRY_COMPARE(p.b->devices()[0].count, 9);
        info["revision"] = 4;
        info["petCount"] = 4;
        QVERIFY(p.a->send(socket, info));
        QTest::qWait(10);
        QCOMPARE(p.b->devices()[0].count, 9);
        QByteArray flood;
        for (int i = 0; i < 40; ++i)
            flood += frame({{"type", "ping"}, {"requestId", uuid()}});
        socket->write(flood);
        QTRY_VERIFY(p.b->connections_.isEmpty());
    }
    void pendingHandshakeSurvivesManualRefresh() {
        Pair p;
        QTcpServer remote;
        QVERIFY(remote.listen(QHostAddress::LocalHost, p.b->port()));
        QVERIFY(p.a->start());
        p.a->candidate({{"version", 1},
                        {"appVersion", NetworkService::applicationVersion()},
                        {"deviceId", secondId},
                        {"sessionId", uuid()},
                        {"tcpPort", p.b->port()}},
                       QHostAddress::LocalHost, false);
        auto *socket = p.a->peers_[secondId].socket;
        QVERIFY(socket);
        QVERIFY(!p.a->connections_[socket]->ready);
        p.a->refresh();
        QCOMPARE(p.a->peers_[secondId].socket, socket);
        QCOMPARE(p.a->connections_.size(), 1);
    }
    void nativeInterfaceBroadcastAndHandshake() {
        if (!qEnvironmentVariableIsSet("PETTIME_NETWORK_LAN_PROBE"))
            return;
        QTemporaryDir dir;
        identity(dir.path(), secondId);
        NetworkService service(SettingsStore(dir.path()),
                               [] { return NetworkService::Info{4, 24}; });
        QVERIFY2(service.start(), qPrintable(service.status()));
        QVERIFY(!service.addresses().isEmpty());
        const QHostAddress address(service.addresses().first());
        QUdpSocket sender;
        sender.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(sender.bind(address, 0));
        auto h = hello();
        const QString request = uuid();
        const QJsonObject discovery{{"appVersion", NetworkService::applicationVersion()},
                                    {"protocol", "pettime"},
                                    {"version", 1},
                                    {"type", "discover"},
                                    {"deviceId", firstId},
                                    {"sessionId", h["sessionId"]},
                                    {"requestId", request},
                                    {"tcpPort", 21012}};
        for (const auto &target : service.targets_)
            sender.writeDatagram(QJsonDocument(discovery).toJson(QJsonDocument::Compact),
                                 target.first, target.second);
        QTRY_VERIFY(sender.hasPendingDatagrams());
        const auto response = sender.receiveDatagram();
        const auto announce = QJsonDocument::fromJson(response.data()).object();
        QCOMPARE(announce["type"].toString(), QString("announce"));
        QCOMPARE(announce["requestId"].toString(), request);
        QTcpSocket client;
        client.setProxy(QNetworkProxy::NoProxy);
        client.connectToHost(address, 21012);
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.write(frame(h));
        QTRY_VERIFY(client.bytesAvailable() > 4);
        client.readAll();
        client.write(frame({{"type", "ready"}, {"connectionId", h["connectionId"]}}));
        QTRY_COMPARE(service.devices().size(), 1);
        QTRY_COMPARE(service.devices()[0].state, QString("已连接"));
        QCOMPARE(service.devices()[0].address, address.toString());
        qInfo() << "Native IPv4 broadcast and TCP handshake passed on" << address.toString();
        service.stop();
        QVERIFY(!service.server_.isListening());
    }
    void subnetFiltering() {
        Pair p;
        QVERIFY(p.a->start());
        QVERIFY(p.a->allowed(QHostAddress::LocalHost));
        QVERIFY(!p.a->allowed(QHostAddress("192.0.2.12")));
        QVERIFY(!p.a->allowed(QHostAddress::LocalHostIPv6));
    }
};
} // namespace pettime
QTEST_MAIN(pettime::NetworkTest)
#include "TestNetwork.moc"
