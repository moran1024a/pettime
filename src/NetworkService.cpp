#include "NetworkService.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace pettime {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validId(const QJsonValue &value) {
    const QString s = value.toString();
    return !QUuid(s).isNull() && QUuid(s).toString(QUuid::WithoutBraces) == s;
}
bool integer(const QJsonValue &v, double low, double high) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low &&
           v.toDouble() <= high && std::floor(v.toDouble()) == v.toDouble();
}
bool envelope(const QJsonObject &o) {
    return o["protocol"] == "pettime" && integer(o["version"], 1, 1) && o["type"].isString();
}
bool validInfo(const QJsonObject &o) {
    return validId(o["deviceId"]) && validId(o["sessionId"]) && o["deviceName"].isString() &&
           o["deviceName"].toString().size() <= 128 && integer(o["tcpPort"], 1, 65535) &&
           integer(o["petLimit"], 1, 72) && integer(o["petCount"], 1, o["petLimit"].toInt()) &&
           integer(o["revision"], 1, 9007199254740991.0);
}
} // namespace
NetworkService::NetworkService(SettingsStore store, std::function<Info()> snapshot, QObject *parent)
    : NetworkService(std::move(store), std::move(snapshot), Options{}, parent) {}
NetworkService::NetworkService(SettingsStore store, std::function<Info()> snapshot, Options options,
                               QObject *parent)
    : QObject(parent), store_(std::move(store)), snapshot_(std::move(snapshot)),
      options_(std::move(options)) {
    server_.setProxy(QNetworkProxy::NoProxy);
    udp_.setProxy(QNetworkProxy::NoProxy);
    clock_.start();
    timer_.setInterval(100);
    server_.setMaxPendingConnections(16);
    connect(&timer_, &QTimer::timeout, this, &NetworkService::tick);
    connect(&udp_, &QUdpSocket::readyRead, this, &NetworkService::receiveDatagrams);
    connect(&server_, &QTcpServer::newConnection, this, [this] {
        while (server_.hasPendingConnections()) {
            auto *socket = server_.nextPendingConnection();
            int pending = 0;
            for (const auto &c : connections_)
                if (!c->ready)
                    ++pending;
            if (!running_ || !allowed(socket->peerAddress()) || pending >= 16 ||
                connections_.size() >= 80) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            attach(socket, false);
        }
    });
}
NetworkService::~NetworkService() { stop(); }
qint64 NetworkService::now() const { return options_.now ? options_.now() : clock_.elapsed(); }
QStringList NetworkService::addresses() const {
    QStringList result;
    for (const auto &net : subnets_)
        result.append(net.first.toString());
    result.removeDuplicates();
    return result;
}
QList<NetworkService::Device> NetworkService::devices() const {
    QList<Device> result;
    for (const auto &peer : peers_)
        result.append(peer.device);
    std::sort(result.begin(), result.end(),
              [](const Device &a, const Device &b) { return a.id < b.id; });
    return result;
}
bool NetworkService::enumerate() {
    subnets_.clear();
    targets_.clear();
    if (options_.loopbackOnly) {
        subnets_.append({QHostAddress::LocalHost, 8});
        targets_ = options_.discoveryTargets;
        return true;
    }
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanBroadcast) ||
            flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                entry.broadcast().isNull() || entry.prefixLength() < 1)
                continue;
            subnets_.append({entry.ip(), entry.prefixLength()});
            const auto target = qMakePair(entry.broadcast(), options_.port);
            if (!targets_.contains(target))
                targets_.append(target);
        }
    }
    return !subnets_.isEmpty();
}
bool NetworkService::allowed(const QHostAddress &address) const {
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    for (const auto &net : subnets_)
        if (address.isInSubnet(net.first, net.second))
            return true;
    return false;
}
bool NetworkService::start() {
    if (running_)
        return true;
    auto fail = [this](const QString &message) {
        stop();
        status_ = message;
        emit changed();
        return false;
    };
    if (!enumerate())
        return fail("无法开启：没有可用的 IPv4 局域网接口。");
    QString error;
    id_ = store_.networkDeviceId(&error);
    if (id_.isEmpty())
        return fail("无法开启：" + error);
    const QHostAddress bind =
        options_.loopbackOnly ? QHostAddress::LocalHost : QHostAddress::AnyIPv4;
    if (!server_.listen(bind, options_.port))
        return fail(QString("TCP %1 监听失败：%2").arg(options_.port).arg(server_.errorString()));
    if (!udp_.bind(bind, options_.port, QUdpSocket::DontShareAddress))
        return fail(QString("UDP %1 监听失败：%2").arg(options_.port).arg(udp_.errorString()));
    session_ = uuid();
    previous_ = snapshot_();
    revision_ = 1;
    publishedRevision_ = 0;
    running_ = true;
    status_ = "运行中";
    lastRefresh_ = now() - 1000;
    nextSample_ = now() + 1000;
    timer_.start();
    discover(false);
    emit changed();
    return true;
}
void NetworkService::stop() {
    if (running_) {
        emit stopping();
        // One bounded end-of-session message also covers recalls still queued behind traffic.
        for (auto *socket : connections_.keys()) {
            const auto c = connections_.value(socket);
            if (c && c->ready && c->dispatch &&
                send(socket, {{"type", "dispatch"}, {"op", "closing"}}))
                socket->flush();
        }
    }
    running_ = false;
    timer_.stop();
    server_.close();
    udp_.close();
    const auto sockets = connections_.keys();
    for (auto *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    connections_.clear();
    peers_.clear();
    requests_.clear();
    udpRates_.clear();
    targets_.clear();
    subnets_.clear();
    session_.clear();
    status_ = "已关闭";
    emit changed();
}
void NetworkService::discover(bool manual) {
    if (!running_)
        return;
    if (!enumerate()) {
        for (auto *socket : connections_.keys())
            drop(socket, "网络接口暂时不可用");
        nextDiscovery_ = now() + 1000;
        status_ = "网络暂时不可用，正在等待接口恢复。";
        emit changed();
        return;
    }
    for (auto *socket : connections_.keys())
        if (connections_.value(socket)->connected >= 0 && !allowed(socket->peerAddress()))
            drop(socket, "网络接口已变化");
    const QString request = uuid();
    requests_.insert(request, now());
    QJsonObject message{{"protocol", "pettime"},    {"version", 1},          {"type", "discover"},
                        {"deviceId", id_},          {"sessionId", session_}, {"requestId", request},
                        {"tcpPort", options_.port}, {"manual", manual}};
    const auto details = discoveryData();
    for (auto it = details.begin(); it != details.end(); ++it)
        message[it.key()] = it.value();
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bool failed = false;
    for (const auto &target : targets_)
        if (udp_.writeDatagram(bytes, target.first, target.second) != bytes.size())
            failed = true;
    status_ = failed ? "运行中；部分发现请求发送失败" : "运行中";
    nextDiscovery_ = now() + 10000;
    emit changed();
}
void NetworkService::refresh() {
    if (!running_ || now() - lastRefresh_ < 1000)
        return;
    lastRefresh_ = now();
    discover(true);
    if (!running_)
        return;
    for (const auto &id : peers_.keys()) {
        auto &p = peers_[id];
        auto c = connections_.value(p.socket);
        if (c && c->ready) {
            if (c->query.isEmpty()) {
                c->query = uuid();
                c->queryAt = now();
                send(c->socket, {{"type", "getInfo"}, {"requestId", c->query}});
            }
        } else if (!p.socket) {
            p.retryAt = now();
            tryConnect(id);
        }
    }
}
void NetworkService::receiveDatagrams() {
    int budget = 64;
    while (running_ && udp_.hasPendingDatagrams() && budget-- > 0) {
        const auto d = udp_.receiveDatagram(1025);
        if (d.data().size() > 1024 || !allowed(d.senderAddress()))
            continue;
        const auto doc = QJsonDocument::fromJson(d.data());
        const auto o = doc.object();
        if (o["protocol"] != "pettime" || !integer(o["version"], 1, 65535) ||
            !validId(o["deviceId"]) || !validId(o["sessionId"]) || !validId(o["requestId"]) ||
            !integer(o["tcpPort"], 1, 65535) ||
            (!options_.loopbackOnly && o["tcpPort"].toInt() != 21012))
            continue;
        if (o["deviceId"] == id_ && o["sessionId"] == session_)
            continue;
        const auto type = o["type"].toString();
        if (type == "discover") {
            if (o.contains("manual") && !o["manual"].isBool())
                continue;
            const QString source = d.senderAddress().toString();
            if (udpRates_.contains(source) && now() - udpRates_[source] < 1000)
                continue;
            if (udpRates_.size() >= 256 && !udpRates_.contains(source))
                continue;
            udpRates_[source] = now();
            QJsonObject response{{"protocol", "pettime"},   {"version", 1},
                                 {"type", "announce"},      {"deviceId", id_},
                                 {"sessionId", session_},   {"requestId", o["requestId"]},
                                 {"tcpPort", options_.port}};
            const auto details = discoveryData();
            for (auto it = details.begin(); it != details.end(); ++it)
                response[it.key()] = it.value();
            udp_.writeDatagram(QJsonDocument(response).toJson(QJsonDocument::Compact),
                               d.senderAddress(), d.senderPort());
            candidate(o, d.senderAddress(), o["manual"].toBool());
        } else if (type == "announce" && requests_.contains(o["requestId"].toString()) &&
                   now() - requests_[o["requestId"].toString()] <= 3000) {
            candidate(o, d.senderAddress(), false);
        }
    }
    if (running_ && udp_.hasPendingDatagrams())
        QTimer::singleShot(0, &udp_, [this] { receiveDatagrams(); });
}
void NetworkService::candidate(const QJsonObject &o, const QHostAddress &address, bool manual) {
    const QString id = o["deviceId"].toString(), session = o["sessionId"].toString();
    if (id == id_) {
        status_ = "运行中；发现重复设备 ID，请检查复制的存档";
        emit changed();
        return;
    }
    if (!peers_.contains(id) && peers_.size() >= 128)
        return;
    auto &p = peers_[id];
    if (p.device.id.isEmpty()) {
        p.device.id = id;
        p.lastSeen = now();
        p.device.state = "等待连接";
    }
    if (p.session != session) {
        if (p.socket) {
            const auto connection = connections_.value(p.socket);
            if (connection && connection->ready) {
                p.device.state = "身份冲突";
                emit changed();
                return;
            }
            drop(p.socket, "远端会话已更新");
        }
        p.session = session;
        p.revision = 0;
        p.endpoints.clear();
        p.retryAt = now();
    }
    const auto existing = connections_.value(p.socket);
    if (!existing || !existing->ready) {
        const bool wasCompatible = p.device.compatible;
        checkVersion(p, o);
        if (o["deviceName"].isString())
            p.device.name = o["deviceName"].toString().left(128);
        if (integer(o["petLimit"], 1, 72) && integer(o["petCount"], 1, o["petLimit"].toInt())) {
            p.device.count = o["petCount"].toInt();
            p.device.limit = o["petLimit"].toInt();
        }
        if (!p.device.compatible) {
            if (p.socket)
                drop(p.socket, "版本不匹配");
            p.lastSeen = now(); // Discovery keeps an incompatible device visible, not connected.
            p.device.lastContact = QDateTime::currentDateTime();
        } else if (!wasCompatible) {
            p.device.state = "等待连接";
            p.retryAt = now();
        }
    }
    const quint16 port = quint16(o["tcpPort"].toInt());
    bool found = false;
    for (const auto &e : p.endpoints)
        if (e.address == address && e.port == port)
            found = true;
    if (!found && p.endpoints.size() < 8)
        p.endpoints.append({address, port});
    if (p.device.address.isEmpty()) {
        p.device.address = address.toString();
        p.device.port = port;
    }
    if (manual && now() - p.manualAt >= 1000) {
        p.manualAt = now();
        p.retryAt = now();
    }
    tryConnect(id);
    emit changed();
}
void NetworkService::tryConnect(const QString &id) {
    if (!running_ || id_ >= id || !peers_.contains(id))
        return;
    auto &p = peers_[id];
    if (!p.device.compatible || p.socket || p.endpoints.isEmpty() || now() < p.retryAt)
        return;
    int pending = 0, ready = 0;
    for (const auto &c : connections_) {
        if (c->ready)
            ++ready;
        else if (c->outbound)
            ++pending;
    }
    if (pending >= 4 || ready >= 64 || connections_.size() >= 80)
        return;
    const auto e = p.endpoints[p.addressIndex++ % p.endpoints.size()];
    if (!allowed(e.address))
        return;
    auto *socket = new QTcpSocket(this);
    attach(socket, true, id);
    p.socket = socket;
    p.device.state = "连接中";
    socket->connectToHost(e.address, e.port);
}
void NetworkService::attach(QTcpSocket *socket, bool outbound, const QString &id) {
    socket->setParent(this);
    if (outbound)
        socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(65536);
    auto c = std::make_shared<Connection>();
    c->socket = socket;
    c->outbound = outbound;
    c->peer = id;
    c->created = now();
    c->connected = outbound ? -1 : now();
    c->lastRx = now();
    c->rateAt = now();
    if (outbound) {
        c->session = peers_[id].session;
        c->token = uuid();
    }
    connections_.insert(socket, c);
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        auto c = connections_.value(socket);
        if (!c)
            return;
        c->connected = now();
        peers_[c->peer].device.state = "握手中";
        auto o = localData();
        o["type"] = "hello";
        o["connectionId"] = c->token;
        send(socket, o);
        emit changed();
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket] { drop(socket, "连接已断开"); });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket] { drop(socket, socket->errorString()); });
    if (socket->bytesAvailable())
        read(socket);
}
QJsonObject NetworkService::discoveryData() const {
    const auto info = snapshot_();
    return {{"appVersion", options_.appVersion},
            {"deviceName", QSysInfo::machineHostName().left(128)},
            {"petCount", info.count},
            {"petLimit", info.limit}};
}
bool NetworkService::checkVersion(Peer &p, const QJsonObject &o) {
    p.device.appVersion = o["appVersion"].isString() && o["appVersion"].toString().size() <= 64
                              ? o["appVersion"].toString()
                              : QString{};
    p.device.protocolVersion = o["version"].toInt();
    p.device.compatible = !p.device.appVersion.isEmpty() &&
                          p.device.appVersion == options_.appVersion &&
                          p.device.protocolVersion == 1;
    if (!p.device.compatible) {
        p.device.dispatch = false;
        p.device.state = p.device.appVersion.isEmpty()   ? "版本未知，无法互联"
                         : p.device.protocolVersion != 1 ? "协议版本不匹配，无法互联"
                                                         : "版本不匹配，无法互联";
    }
    return p.device.compatible;
}
QJsonObject NetworkService::localData() {
    const auto info = snapshot_();
    if (info.count != previous_.count || info.limit != previous_.limit ||
        info.dispatched != previous_.dispatched || info.visitors != previous_.visitors) {
        previous_ = info;
        ++revision_;
    }
    return {{"deviceId", id_},
            {"sessionId", session_},
            {"deviceName", QSysInfo::machineHostName().left(128)},
            {"tcpPort", options_.port},
            {"petCount", info.count},
            {"petLimit", info.limit},
            {"revision", revision_},
            {"appVersion", options_.appVersion},
            {"capabilities", dispatchEnabled_ ? QJsonArray{"dispatch-v1"} : QJsonArray{}},
            {"dispatched", info.dispatched},
            {"visitors", info.visitors}};
}
QString NetworkService::peerSession(const QString &peer) const {
    return peers_.value(peer).session;
}
bool NetworkService::dispatchReady(const QString &peer) const {
    const auto c = connections_.value(peers_.value(peer).socket);
    return c && c->ready && c->dispatch;
}
void NetworkService::disconnectPeer(const QString &peer) {
    auto *socket = peers_.value(peer).socket;
    if (socket)
        drop(socket, "派遣同步中断");
}
bool NetworkService::sendDispatch(const QString &peer, QJsonObject o, const QString &key) {
    const auto c = connections_.value(peers_.value(peer).socket);
    if (!c || !c->ready || !c->dispatch)
        return false;
    o["type"] = "dispatch";
    if (QJsonDocument(o).toJson(QJsonDocument::Compact).size() > 8100)
        return false;
    if (key.isEmpty()) {
        if (c->controls.size() >= 256) {
            drop(c->socket, "派遣控制队列已满");
            return false;
        }
        c->controls.enqueue(o);
    } else {
        if (!c->updates.contains(key) && c->updates.size() >= 144)
            return false;
        if (!c->updates.contains(key))
            c->updateOrder.enqueue(key);
        c->updates[key] = o;
    }
    return true;
}
void NetworkService::flushDispatch(Connection &c) {
    if (!c.dispatch)
        return;
    if (now() - c.txAt >= 1000) {
        c.txAt = now();
        c.txFrames = c.txBytes = 0;
    }
    while (c.txFrames < 100 && c.socket->bytesToWrite() < 8192) {
        bool control = !c.controls.isEmpty();
        if (!control && c.updates.isEmpty())
            break;
        const QString key = control ? QString{} : c.updateOrder.head();
        const auto o = control ? c.controls.head() : c.updates.value(key);
        const int bytes = QJsonDocument(o).toJson(QJsonDocument::Compact).size() + 64;
        if (c.txBytes + bytes > 262144)
            break;
        if (control)
            c.controls.dequeue();
        else {
            c.updates.remove(key);
            c.updateOrder.dequeue();
        }
        ++c.txFrames;
        c.txBytes += bytes;
        if (!send(c.socket, o))
            return;
    }
}
bool NetworkService::send(QTcpSocket *socket, QJsonObject o) {
    if (!connections_.contains(socket))
        return false;
    o["protocol"] = "pettime";
    o["version"] = 1;
    const auto body = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (body.size() > 8192 || socket->bytesToWrite() + body.size() + 4 > 65536) {
        drop(socket, "发送缓冲超限");
        return false;
    }
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(quint32(body.size()), frame.data());
    frame += body;
    if (socket->write(frame) != frame.size()) {
        drop(socket, "发送失败");
        return false;
    }
    return true;
}
void NetworkService::read(QTcpSocket *socket) {
    auto c = connections_.value(socket);
    if (!c)
        return;
    c->scheduled = false;
    if (socket->bytesAvailable() + c->input.size() > 65536) {
        drop(socket, "接收缓冲超限");
        return;
    }
    c->input += socket->readAll();
    int budget = 16;
    while (c->input.size() >= 4 && budget-- > 0) {
        const auto size = qFromBigEndian<quint32>(c->input.constData());
        if (!size || size > 8192) {
            drop(socket, "无效消息长度");
            return;
        }
        if (c->input.size() < qsizetype(size + 4))
            break;
        const auto doc = QJsonDocument::fromJson(c->input.mid(4, size));
        c->input.remove(0, size + 4);
        if (now() - c->rateAt >= 1000) {
            c->rateAt = now();
            c->frames = 0;
        }
        const auto elapsed = std::max<qint64>(0, now() - c->creditAt);
        c->creditAt = now();
        c->frameCredit = std::min(256.0, c->frameCredit + elapsed * .128) - 1;
        c->byteCredit = std::min(524288.0, c->byteCredit + elapsed * 262.144) - size - 4;
        if ((c->dispatch ? c->frameCredit < 0 || c->byteCredit < 0 : ++c->frames > 32) ||
            !doc.isObject() || !envelope(doc.object()) || !handle(*c, doc.object())) {
            drop(socket, "协议校验失败");
            return;
        }
        if (!connections_.contains(socket))
            return;
        c->lastRx = now();
        if (c->ready) {
            auto &p = peers_[c->peer];
            p.lastSeen = now();
            p.device.lastContact = QDateTime::currentDateTime();
        }
    }
    if (c->input.size() >= 4 && !c->scheduled &&
        c->input.size() >= qsizetype(qFromBigEndian<quint32>(c->input.constData())) + 4) {
        c->scheduled = true;
        QTimer::singleShot(0, socket, [this, socket] { read(socket); });
    }
}
bool NetworkService::applyInfo(Peer &p, const QJsonObject &o) {
    if (!validInfo(o) || o["deviceId"] != p.device.id || o["sessionId"] != p.session ||
        (!options_.loopbackOnly && o["tcpPort"].toInt() != 21012))
        return false;
    if (!checkVersion(p, o))
        return false;
    if (o["revision"].toDouble() > p.revision) {
        p.revision = o["revision"].toDouble();
        p.device.name = o["deviceName"].toString();
        p.device.count = o["petCount"].toInt();
        p.device.limit = o["petLimit"].toInt();
        p.device.port = quint16(o["tcpPort"].toInt());
        p.device.dispatched = std::clamp(o["dispatched"].toInt(), 0, 71);
        p.device.visitors = std::clamp(o["visitors"].toInt(), 0, 144);
    }
    return true;
}
void NetworkService::markReady(Connection &c) {
    c.ready = true;
    peers_[c.peer].device.dispatch = c.dispatch;
    c.readyAt = now();
    c.pingAt = now();
    c.lastRx = now();
    auto &p = peers_[c.peer];
    p.socket = c.socket;
    p.lastSeen = now();
    p.device.address = c.socket->peerAddress().toString();
    p.device.state = "已连接";
    p.device.lastContact = QDateTime::currentDateTime();
    emit changed();
    emit peerReady(c.peer, c.session);
}
bool NetworkService::handle(Connection &c, const QJsonObject &o) {
    const auto type = o["type"].toString();
    if (!c.ready) {
        int ready = 0;
        for (const auto &other : connections_)
            if (other->ready)
                ++ready;
        if (ready >= 64)
            return false;
        if (!c.outbound && c.peer.isEmpty() && type == "hello") {
            if (!validInfo(o) || !validId(o["connectionId"]))
                return false;
            const auto id = o["deviceId"].toString(), session = o["sessionId"].toString();
            if (id >= id_ || (!peers_.contains(id) && peers_.size() >= 128))
                return false;
            auto &p = peers_[id];
            if (p.socket)
                return false;
            if (p.session != session) {
                p.revision = 0;
                p.endpoints.clear();
            }
            if (p.device.id.isEmpty())
                p.lastSeen = now();
            p.device.id = id;
            p.session = session;
            p.device.address = c.socket->peerAddress().toString();
            p.device.port = quint16(o["tcpPort"].toInt());
            p.device.name = o["deviceName"].toString();
            if (!applyInfo(p, o)) {
                emit changed();
                return false;
            }
            c.dispatch = dispatchEnabled_ && o["capabilities"].toArray().contains("dispatch-v1");
            c.peer = id;
            c.session = session;
            c.token = o["connectionId"].toString();
            p.socket = c.socket;
            p.device.state = "握手中";
            auto answer = localData();
            answer["type"] = "helloAck";
            answer["connectionId"] = c.token;
            return send(c.socket, answer);
        }
        if (c.outbound && type == "helloAck" && o["connectionId"] == c.token) {
            if (!peers_.contains(c.peer) || o["sessionId"] != c.session ||
                !applyInfo(peers_[c.peer], o))
                return false;
            c.dispatch = dispatchEnabled_ && o["capabilities"].toArray().contains("dispatch-v1");
            if (!send(c.socket, {{"type", "ready"}, {"connectionId", c.token}}))
                return false;
            markReady(c);
            return true;
        }
        if (!c.outbound && !c.peer.isEmpty() && type == "ready" && o["connectionId"] == c.token) {
            markReady(c);
            return true;
        }
        return false;
    }
    if (type == "dispatch") {
        if (!c.dispatch || !o["op"].isString())
            return false;
        emit dispatchMessage(c.peer, c.session, o);
        return true;
    }
    if (type == "getInfo" && validId(o["requestId"])) {
        auto answer = localData();
        answer["type"] = "info";
        answer["requestId"] = o["requestId"];
        return send(c.socket, answer);
    }
    if (type == "info" || type == "infoChanged") {
        if (type == "info" &&
            (!validId(o["requestId"]) ||
             (o["requestId"] != c.query && !c.lateQueries.remove(o["requestId"].toString()))))
            return false;
        if (!applyInfo(peers_[c.peer], o))
            return false;
        if (type == "info" && o["requestId"] == c.query)
            c.query.clear();
        peers_[c.peer].device.state = "已连接";
        emit changed();
        return true;
    }
    if (type == "ping" && validId(o["requestId"]))
        return send(c.socket, {{"type", "pong"}, {"requestId", o["requestId"]}});
    if (type == "pong" && !c.ping.isEmpty() && o["requestId"] == c.ping) {
        c.ping.clear();
        return true;
    }
    return false;
}
void NetworkService::drop(QTcpSocket *socket, const QString &reason) {
    auto c = connections_.take(socket);
    if (!c)
        return;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    if (peers_.contains(c->peer)) {
        auto &p = peers_[c->peer];
        if (p.socket == socket) {
            p.socket = nullptr;
            if (p.device.compatible)
                p.device.state = id_ < c->peer ? "重连等待：" + reason : "离线：" + reason;
            const int delay = std::min(30000, 1000 * (1 << std::min(p.failures++, 5)));
            p.retryAt = now() + delay + QRandomGenerator::global()->bounded(251);
        }
    }
    emit changed();
    if (!c->peer.isEmpty())
        emit peerLost(c->peer);
}
void NetworkService::tick() {
    if (!running_)
        return;
    const auto time = now();
    for (auto i = requests_.begin(); i != requests_.end();)
        if (time - i.value() > 3000)
            i = requests_.erase(i);
        else
            ++i;
    for (auto i = udpRates_.begin(); i != udpRates_.end();)
        if (time - i.value() > 10000)
            i = udpRates_.erase(i);
        else
            ++i;
    if (time >= nextDiscovery_)
        discover(false);
    if (!running_)
        return;
    const bool sampleDue = time >= nextSample_;
    if (sampleDue) {
        // Send the latest revision to every peer, even if a getInfo sampled the change first.
        auto o = localData();
        o["type"] = "infoChanged";
        if (revision_ != publishedRevision_) {
            for (const auto &c : connections_)
                if (c->ready)
                    c->pendingInfo = o;
            publishedRevision_ = revision_;
        }
        nextSample_ = time + 1000;
    }
    for (auto *socket : connections_.keys()) {
        auto c = connections_.value(socket);
        if (!c)
            continue;
        if (!c->ready) {
            if ((c->connected < 0 && time - c->created >= 3000) ||
                (c->connected >= 0 && time - c->connected >= 3000))
                drop(socket, "连接或握手超时");
            continue;
        }
        if (time - c->lastRx >= 30000 || (!c->ping.isEmpty() && time - c->pingAt >= 30000)) {
            drop(socket, "心跳超时");
            continue;
        }
        flushDispatch(*c);
        if (!connections_.contains(socket))
            continue;
        if (time - c->readyAt >= 30000)
            peers_[c->peer].failures = 0;
        if (!c->query.isEmpty() && time - c->queryAt >= 3000) {
            c->lateQueries.insert(c->query, time);
            c->query.clear();
            peers_[c->peer].device.state = "已连接（刷新响应延迟）";
        }
        for (auto i = c->lateQueries.begin(); i != c->lateQueries.end();)
            if (time - i.value() >= 30000)
                i = c->lateQueries.erase(i);
            else
                ++i;
        if (c->ping.isEmpty() && time - c->pingAt >= 10000) {
            c->ping = uuid();
            c->pingAt = time;
            if (!send(socket, {{"type", "ping"}, {"requestId", c->ping}}))
                continue;
        }
        if (!c->pendingInfo.isEmpty() && socket->bytesToWrite() == 0) {
            const auto o = c->pendingInfo;
            c->pendingInfo = {};
            send(socket, o);
        }
    }
    for (const auto &id : peers_.keys()) {
        auto &p = peers_[id];
        if (time - p.lastSeen >= 120000) {
            if (p.socket)
                drop(p.socket, "设备过期");
            peers_.remove(id);
        } else
            tryConnect(id);
    }
    if (sampleDue)
        emit changed();
}
} // namespace pettime
