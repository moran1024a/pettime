#include "DispatchController.h"
#include <QJsonDocument>
#include <QSysInfo>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace pettime {
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validUuid(const QString &s) {
    return !QUuid(s).isNull() && QUuid(s).toString(QUuid::WithoutBraces) == s;
}
bool serial(const QJsonValue &v, quint64 &result) {
    bool ok = false;
    result = v.toString().toULongLong(&ok);
    return v.isString() && ok && result > 0 && QString::number(result) == v.toString();
}
QJsonArray rect(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
bool rect(const QJsonValue &v, QRectF &r) {
    if (!v.isArray() || v.toArray().size() != 4)
        return false;
    const auto a = v.toArray();
    for (const auto &n : a)
        if (!n.isDouble() || !std::isfinite(n.toDouble()) || std::abs(n.toDouble()) > 100000)
            return false;
    r = {a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()};
    return r.width() >= 100 && r.height() >= 100 && r.width() <= 32768 && r.height() <= 32768;
}
QString name(const PetModel &p) {
    return p.customName.isEmpty()
               ? QSysInfo::machineHostName().left(128) + " #" + QString::number(p.id)
               : p.customName;
}
} // namespace
DispatchController::DispatchController(NetworkService &network, SwarmController &swarm,
                                       std::function<QRectF()> surface,
                                       std::function<qint64()> time, QObject *parent)
    : QObject(parent), network_(network), swarm_(swarm), surface_(std::move(surface)),
      now_(std::move(time)) {
    clock_.start();
    network_.enableDispatch();
    lastSurface_ = surface_();
    connect(&network_, &NetworkService::peerReady, this, &DispatchController::ready);
    connect(&network_, &NetworkService::peerLost, this, &DispatchController::lost);
    connect(&network_, &NetworkService::dispatchMessage, this, &DispatchController::receive);
    connect(&network_, &NetworkService::stopping, this, &DispatchController::shutdown);
}
DispatchController::~DispatchController() { shutdown(); }
qint64 DispatchController::now() const { return now_ ? now_() : clock_.elapsed(); }
bool DispatchController::send(const QString &peer, const QString &op, QJsonObject data,
                              const QString &key) {
    data["op"] = op;
    return network_.sendDispatch(peer, data, key);
}
QString DispatchController::peerName(const QString &peer) const {
    for (const auto &d : network_.devices())
        if (d.id == peer)
            return d.name;
    return peer.left(8);
}
bool DispatchController::isAway(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    return it != outgoing_.cend() && it->phase != Phase::Offering;
}
bool DispatchController::canControl(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    return it == outgoing_.cend() || it->phase == Phase::Active;
}
QString DispatchController::location(quint64 id) const {
    return outgoing_.contains(id) ? peerName(outgoing_[id].peer) : QStringLiteral("本机");
}
QString DispatchController::stateText(quint64 id) const {
    if (!outgoing_.contains(id))
        return "本地";
    const auto &o = outgoing_[id];
    if (o.frozenAt >= 0)
        return QString("等待重连（%1 秒）")
            .arg(std::max<qint64>(0, (GraceMs - now() + o.frozenAt + 999) / 1000));
    switch (o.phase) {
    case Phase::Offering:
        return "请求中";
    case Phase::Activating:
        return "确认中";
    case Phase::Active:
        return "已派遣";
    case Phase::Frozen:
        return "等待重连";
    case Phase::Recalling:
        return "召回中";
    }
    return {};
}
QString DispatchController::dispatchPets(const QList<quint64> &ids, const QString &peer) {
    if (!network_.dispatchReady(peer))
        return "目标未连接或不支持派遣，请先开启双方网络服务。";
    int accepted = 0, skipped = 0;
    QSet<quint64> unique(ids.begin(), ids.end());
    for (auto id : unique) {
        auto *p = swarm_.find(id);
        if (!p || p->primary || !p->active() || p->expired || p->splitReady ||
            outgoing_.contains(id)) {
            ++skipped;
            continue;
        }
        Outgoing o;
        o.entity = id;
        o.id = uuid();
        o.peer = peer;
        o.session = network_.peerSession(peer);
        o.home = p->position;
        o.homeArea = p->area;
        o.started = now();
        outgoing_.insert(id, o);
        if (!send(peer, "offer",
                  {{"id", o.id}, {"entity", QString::number(id)}, {"name", name(*p)}})) {
            outgoing_.remove(id);
            ++skipped;
            continue;
        }
        ++accepted;
    }
    emit changed();
    return QString("已申请 %1 只，跳过 %2 只；最终结果以目标确认和列表状态为准。")
        .arg(accepted)
        .arg(skipped);
}
void DispatchController::finish(quint64 id, bool restore) {
    if (!outgoing_.contains(id))
        return;
    const auto o = outgoing_.take(id);
    if (auto *p = swarm_.find(id)) {
        p->cancelEntry();
        p->setManualControl(false);
        p->motionGroup.clear();
        p->dispatchPaused = false;
        if (restore && o.phase != Phase::Offering)
            p->moveTo(o.home, o.homeArea);
    }
    emit changed();
}
void DispatchController::recallPet(quint64 id) {
    if (!outgoing_.contains(id))
        return;
    auto &o = outgoing_[id];
    if (o.phase == Phase::Recalling)
        return;
    const bool local =
        o.phase == Phase::Offering || o.frozenAt >= 0 || !network_.dispatchReady(o.peer);
    const auto copy = o;
    if (local)
        finish(id);
    else {
        o.phase = Phase::Recalling;
        o.started = now();
        if (auto *p = swarm_.find(id))
            p->dispatchPaused = true;
    }
    if (!send(copy.peer, "recall", {{"id", copy.id}}))
        finish(id);
    emit changed();
}
void DispatchController::recallAll() {
    for (auto id : outgoing_.keys())
        recallPet(id);
}
void DispatchController::removeEntity(quint64 id) {
    if (!outgoing_.contains(id))
        return;
    const auto o = outgoing_[id];
    finish(id, false);
    send(o.peer, "recall", {{"id", o.id}});
}
bool DispatchController::highlightPet(quint64 id) {
    if (!outgoing_.contains(id) || !canControl(id))
        return false;
    const auto o = outgoing_[id];
    return send(
        o.peer, "highlight",
        {{"id", o.id}, {"stream", o.stream}, {"event", QString::number(++highlightSerial_)}});
}
bool DispatchController::expelVisitor(const QString &id) {
    const auto it = visitors_.constFind(id);
    if (it == visitors_.cend())
        return false;
    const auto v = *it;
    eraseVisitor(id);
    send(v.peer, "expelled", {{"id", id}});
    return true;
}
void DispatchController::eraseVisitor(const QString &id) {
    if (!visitors_.contains(id))
        return;
    const auto v = visitors_.take(id);
    auto &closed = ended_[v.peer];
    if (closed.size() < 4096)
        closed.insert(id);
    emit changed();
}
void DispatchController::freeze(Outgoing &o) {
    if (o.phase == Phase::Offering || o.phase == Phase::Recalling)
        return;
    if (o.frozenAt < 0)
        o.frozenAt = now();
    o.phase = Phase::Frozen;
    o.awaitingActive = false;
    if (auto *p = swarm_.find(o.entity))
        p->dispatchPaused = true;
}
void DispatchController::freeze(Visitor &v) {
    if (!v.active)
        return;
    if (v.frozenAt < 0)
        v.frozenAt = now();
    v.frozen = true;
    v.awaitingState = false;
}
void DispatchController::lost(const QString &peer) {
    for (auto &o : outgoing_)
        if (o.peer == peer)
            freeze(o);
    for (auto &v : visitors_)
        if (v.peer == peer)
            freeze(v);
    emit changed();
}
void DispatchController::resume(Outgoing &o) {
    o.stream = uuid();
    o.sequence = 0;
    send(o.peer, "resume", {{"id", o.id}, {"stream", o.stream}});
}
void DispatchController::ready(const QString &peer, const QString &session) {
    if (health_.contains(peer) && health_[peer].session != session) {
        for (auto id : outgoing_.keys())
            if (outgoing_[id].peer == peer)
                finish(id);
        for (const auto &id : visitors_.keys())
            if (visitors_[id].peer == peer)
                eraseVisitor(id);
        ended_.remove(peer);
    }
    health_[peer] = {session, {}, false, now(), now(), 0, now()};
    QJsonArray inventory;
    for (const auto &o : outgoing_)
        if (o.peer == peer)
            inventory.append(o.id);
    send(peer, "inventory", {{"ids", inventory}});
    for (auto id : outgoing_.keys()) {
        auto &o = outgoing_[id];
        if (o.peer != peer)
            continue;
        if (o.phase == Phase::Offering || o.phase == Phase::Recalling || o.session != session) {
            finish(id);
            continue;
        }
        freeze(o);
        if (now() - o.frozenAt < GraceMs)
            resume(o);
    }
}
PetRenderState DispatchController::interpolated(const Visitor &v) const {
    if (v.frozen)
        return v.current;
    auto s = v.current;
    const double t = std::clamp((now() - v.lastState) / 100.0, 0.0, 1.0);
    s.position = v.previous.position * (1 - t) + v.current.position * t;
    s.heading =
        v.previous.heading + std::remainder(v.current.heading - v.previous.heading, 2 * Pi) * t;
    return s;
}
void DispatchController::shutdown() {
    for (auto id : outgoing_.keys()) {
        const auto o = outgoing_[id];
        send(o.peer, "recall", {{"id", o.id}});
        finish(id);
    }
    for (const auto &id : visitors_.keys()) {
        send(visitors_[id].peer, "gone", {{"id", id}});
        eraseVisitor(id);
    }
    health_.clear();
    ended_.clear();
}
void DispatchController::receive(const QString &peer, const QString &session,
                                 const QJsonObject &m) {
    if (!health_.contains(peer) || health_[peer].session != session)
        return;
    auto invalid = [this, &peer] { network_.disconnectPeer(peer); };
    const QString op = m["op"].toString();
    auto &h = health_[peer];
    h.lastRx = now();
    if (op == "closing") {
        for (auto id : outgoing_.keys())
            if (outgoing_[id].peer == peer)
                finish(id);
        for (const auto &id : visitors_.keys())
            if (visitors_[id].peer == peer)
                eraseVisitor(id);
        return;
    }
    if (op == "lease") {
        if (!validUuid(m["probe"].toString())) {
            invalid();
            return;
        }
        send(peer, "leaseAck", {{"probe", m["probe"]}});
        return;
    }
    if (op == "leaseAck") {
        if (!h.probe.isEmpty() && m["probe"] == h.probe) {
            h.lastAck = now();
            h.probe.clear();
        }
        return;
    }
    if (op == "surface") {
        QRectF area;
        if (!rect(m["area"], area)) {
            invalid();
            return;
        }
        for (const auto &o : outgoing_)
            if (o.peer == peer && o.phase != Phase::Offering)
                if (auto *p = swarm_.find(o.entity))
                    p->setArea(area);
        return;
    }
    if (op == "inventory") {
        if (!m["ids"].isArray() || m["ids"].toArray().size() > 71) {
            invalid();
            return;
        }
        QSet<QString> ids;
        for (const auto &v : m["ids"].toArray()) {
            if (!validUuid(v.toString())) {
                invalid();
                return;
            }
            ids.insert(v.toString());
        }
        for (const auto &id : visitors_.keys())
            if (visitors_[id].peer == peer && !ids.contains(id))
                eraseVisitor(id);
        return;
    }
    if (op == "states") {
        if (!m["items"].isArray() || m["items"].toArray().size() > 71) {
            invalid();
            return;
        }
        for (const auto &item : m["items"].toArray()) {
            const auto a = item.toArray();
            PetRenderState state;
            quint64 sequence;
            if (a.size() != 5 || !validUuid(a[0].toString()) || !validUuid(a[1].toString()) ||
                !serial(a[2], sequence) || !PetRenderState::decode(a[3], state) ||
                !a[4].isString() || a[4].toString().size() > 160) {
                invalid();
                return;
            }
            auto it = visitors_.find(a[0].toString());
            if (it == visitors_.end()) {
                send(peer, "gone", {{"id", a[0].toString()}});
                continue;
            }
            auto &v = it.value();
            if (v.peer != peer || v.session != session) {
                invalid();
                return;
            }
            if (!v.active || (v.frozen && !v.awaitingState) || v.stream != a[1].toString() ||
                sequence <= v.sequence)
                continue;
            if (v.frozenAt >= 0 && now() - v.frozenAt >= GraceMs) {
                const auto id = v.id;
                eraseVisitor(id);
                send(peer, "gone", {{"id", id}});
                continue;
            }
            if (v.awaitingState) {
                v.awaitingState = false;
                v.frozen = false;
                v.frozenAt = -1;
                emit changed();
            }
            v.previous = interpolated(v);
            v.current = state;
            v.sequence = sequence;
            v.lastState = now();
            v.name = a[4].toString();
        }
        return;
    }
    const QString id = m["id"].toString();
    if (!validUuid(id)) {
        invalid();
        return;
    }
    auto visitor = visitors_.find(id);
    if (visitor != visitors_.end() && (visitor->peer != peer || visitor->session != session)) {
        invalid();
        return;
    }
    auto out = outgoing_.end();
    for (auto it = outgoing_.begin(); it != outgoing_.end(); ++it)
        if (it->id == id && it->peer == peer && it->session == session) {
            out = it;
            break;
        }
    if (op == "offer") {
        quint64 entity;
        if (!serial(m["entity"], entity) || entity == 1 || !m["name"].isString() ||
            m["name"].toString().size() > 160) {
            invalid();
            return;
        }
        QRectF area;
        if (ended_[peer].contains(id) || ended_[peer].size() >= 4096 ||
            !rect(rect(surface_()), area)) {
            send(peer, "reject", {{"id", id}, {"reason", "派遣已结束或接收屏幕不可用"}});
            return;
        }
        if (visitor == visitors_.end()) {
            bool duplicate = false;
            for (const auto &v : visitors_)
                if (v.peer == peer && v.entity == m["entity"].toString())
                    duplicate = true;
            if (visitors_.size() >= VisitorLimit || duplicate) {
                send(peer, "reject", {{"id", id}, {"reason", "访客容量已满或实体已存在"}});
                return;
            }
            Visitor v;
            v.id = id;
            v.peer = peer;
            v.session = session;
            v.entity = m["entity"].toString();
            v.name = m["name"].toString();
            v.sourceName = peerName(peer);
            v.created = v.lastState = now();
            visitors_.insert(id, v);
        } else if (visitor->entity != m["entity"].toString()) {
            invalid();
            return;
        }
        send(peer, "accept", {{"id", id}, {"area", rect(area)}});
        emit changed();
        return;
    }
    if (op == "recall") {
        if (visitor != visitors_.end())
            eraseVisitor(id);
        else if (ended_[peer].size() < 4096)
            ended_[peer].insert(id);
        send(peer, "recalled", {{"id", id}});
        return;
    }
    if (op == "recalled" || op == "reject" || op == "gone" || op == "expelled") {
        if (out != outgoing_.end()) {
            if (op == "recalled" && out->phase != Phase::Recalling)
                return;
            if (op == "expelled")
                emit notice("派遣实体已被对方驱赶，返回本机。");
            if (op == "reject")
                emit notice("派遣未完成：" + m["reason"].toString().left(160));
            finish(out.key());
        }
        return;
    }
    if (op == "accept") {
        if (out == outgoing_.end() || out->phase != Phase::Offering)
            return;
        QRectF area;
        if (!rect(m["area"], area)) {
            invalid();
            return;
        }
        auto *p = swarm_.find(out.key());
        if (!p || !p->active() || p->expired || p->splitReady || now() - out->started >= 3000) {
            removeEntity(out.key());
            return;
        }
        p->motionGroup = peer;
        p->dispatchPaused = true;
        p->beginEntry(area, int(p->random(0, 4)), p->random(.2, .8));
        out->phase = Phase::Activating;
        out->started = now();
        out->stream = uuid();
        out->awaitingActive = true;
        send(peer, "activate",
             {{"id", id},
              {"stream", out->stream},
              {"state", PetRenderState::from(*p).encode()},
              {"name", name(*p)}});
        emit changed();
        return;
    }
    if (op == "resume") {
        if (!validUuid(m["stream"].toString())) {
            invalid();
            return;
        }
        if (visitor == visitors_.end() || !visitor->active ||
            (visitor->frozenAt >= 0 && now() - visitor->frozenAt >= GraceMs)) {
            if (visitor != visitors_.end())
                eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
            return;
        }
        freeze(*visitor);
        visitor->pendingStream = m["stream"].toString();
        send(peer, "resumeAck",
             {{"id", id}, {"stream", visitor->pendingStream}, {"area", rect(surface_())}});
        return;
    }
    if (op == "resumeAck") {
        if (out == outgoing_.end() || out->frozenAt < 0 || out->stream != m["stream"].toString() ||
            now() - out->frozenAt >= GraceMs)
            return;
        QRectF area;
        if (!rect(m["area"], area)) {
            invalid();
            return;
        }
        if (auto *p = swarm_.find(out.key())) {
            p->setArea(area);
            out->awaitingActive = true;
            send(peer, "commit",
                 {{"id", id},
                  {"stream", out->stream},
                  {"state", PetRenderState::from(*p).encode()},
                  {"name", name(*p)}});
        }
        return;
    }
    if (op == "activate" || op == "commit") {
        PetRenderState state;
        const auto stream = m["stream"].toString();
        if (!validUuid(stream) || !PetRenderState::decode(m["state"], state) ||
            !m["name"].isString() || m["name"].toString().size() > 160) {
            invalid();
            return;
        }
        if (visitor == visitors_.end()) {
            send(peer, "gone", {{"id", id}});
            return;
        }
        auto &v = *visitor;
        if (v.frozenAt >= 0 && now() - v.frozenAt >= GraceMs) {
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
            return;
        }
        if (op == "commit" && v.pendingStream != stream)
            return;
        if (op == "activate" && !v.active && now() - v.created >= 3000) {
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
            return;
        }
        if (op == "activate" && v.active) {
            if (!v.frozen && v.stream == stream)
                send(peer, "active", {{"id", id}, {"stream", stream}});
            return;
        }
        v.previous = v.current = state;
        v.name = m["name"].toString();
        v.stream = stream;
        v.pendingStream.clear();
        v.sequence = 0;
        v.lastState = now();
        v.active = true;
        v.awaitingState = op == "commit";
        v.frozen = v.awaitingState;
        if (!v.awaitingState)
            v.frozenAt = -1;
        send(peer, "active", {{"id", id}, {"stream", stream}});
        emit changed();
        return;
    }
    if (op == "active") {
        if (out == outgoing_.end() || !out->awaitingActive ||
            out->stream != m["stream"].toString() || out->phase == Phase::Recalling ||
            out->phase == Phase::Offering)
            return;
        if (out->frozenAt >= 0 && now() - out->frozenAt >= GraceMs) {
            recallPet(out.key());
            return;
        }
        out->phase = Phase::Active;
        out->frozenAt = -1;
        out->awaitingActive = false;
        if (auto *p = swarm_.find(out.key()))
            p->dispatchPaused = false;
        emit changed();
        return;
    }
    if (op == "highlight") {
        quint64 event;
        if (!serial(m["event"], event)) {
            invalid();
            return;
        }
        if (visitor != visitors_.end() && visitor->active && !visitor->frozen &&
            visitor->stream == m["stream"].toString() && event > visitor->highlight) {
            visitor->highlight = event;
            emit visitorHighlight(id);
        }
        return;
    }
    invalid();
}
void DispatchController::tick() {
    const auto time = now();
    for (auto id : outgoing_.keys()) {
        const auto o = outgoing_[id];
        if (!swarm_.find(id)) {
            removeEntity(id);
            continue;
        }
        if ((o.frozenAt >= 0 && time - o.frozenAt >= GraceMs) ||
            ((o.phase == Phase::Offering || o.phase == Phase::Recalling) &&
             time - o.started >= 3000)) {
            finish(id);
            send(o.peer, "recall", {{"id", o.id}});
            if (o.phase == Phase::Offering)
                emit notice("派遣请求超时，实体仍保留在本机。");
        } else if (o.phase == Phase::Activating && time - o.started >= 3000) {
            freeze(outgoing_[id]);
            network_.disconnectPeer(o.peer);
        }
    }
    for (const auto &id : visitors_.keys()) {
        auto &v = visitors_[id];
        if ((!v.active && time - v.created >= 3000) ||
            (v.frozenAt >= 0 && time - v.frozenAt >= GraceMs)) {
            const auto peer = v.peer;
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
        } else if (v.active && !v.frozen && time - v.lastState >= 3000) {
            const auto peer = v.peer;
            freeze(v);
            network_.disconnectPeer(peer);
        }
    }
    for (const auto &peer : health_.keys()) {
        if (time - health_[peer].lastRx < 120000 || network_.dispatchReady(peer))
            continue;
        bool used = false;
        for (const auto &o : outgoing_)
            if (o.peer == peer)
                used = true;
        for (const auto &v : visitors_)
            if (v.peer == peer)
                used = true;
        if (!used) {
            health_.remove(peer);
            ended_.remove(peer);
        }
    }
    QSet<QString> needed;
    for (const auto &o : outgoing_)
        needed.insert(o.peer);
    for (const auto &v : visitors_)
        needed.insert(v.peer);
    for (auto it = health_.begin(); it != health_.end(); ++it)
        if (!needed.contains(it.key())) {
            it->monitoring = false;
            it->probe.clear();
        }
    for (const auto &peer : needed) {
        if (!network_.dispatchReady(peer) || !health_.contains(peer))
            continue;
        auto &h = health_[peer];
        if (!h.monitoring) {
            h.monitoring = true;
            h.lastAck = h.lastRx = time;
        }
        if (time - h.lastAck >= 3000 || time - h.lastRx >= 3000) {
            lost(peer);
            network_.disconnectPeer(peer);
            continue;
        }
        if (time >= h.nextProbe && h.probe.isEmpty()) {
            h.probe = uuid();
            h.probeAt = time;
            h.nextProbe = time + 1000;
            send(peer, "lease", {{"probe", h.probe}});
        }
    }
    const auto area = surface_();
    if (area != lastSurface_) {
        lastSurface_ = area;
        QRectF valid;
        if (!rect(rect(area), valid)) {
            for (const auto &id : visitors_.keys()) {
                const auto peer = visitors_[id].peer;
                eraseVisitor(id);
                send(peer, "gone", {{"id", id}});
            }
        } else {
            QSet<QString> peers;
            for (const auto &v : visitors_)
                peers.insert(v.peer);
            for (const auto &peer : peers)
                send(peer, "surface", {{"area", rect(area)}}, "surface");
        }
    }
    if (time < nextState_)
        return;
    nextState_ = time + 100;
    QHash<QString, QJsonArray> batches;
    QHash<QString, int> parts;
    auto flush = [&](const QString &peer) {
        if (batches[peer].isEmpty())
            return;
        send(peer, "states", {{"items", batches[peer]}},
             "states-" + QString::number(parts[peer]++));
        batches[peer] = {};
    };
    for (auto &o : outgoing_) {
        if (o.phase != Phase::Active)
            continue;
        const auto *p = swarm_.find(o.entity);
        if (!p)
            continue;
        const QJsonArray item{o.id, o.stream, QString::number(++o.sequence),
                              PetRenderState::from(*p).encode(), name(*p)};
        auto trial = batches[o.peer];
        trial.append(item);
        if (QJsonDocument(trial).toJson(QJsonDocument::Compact).size() > 7600)
            flush(o.peer);
        batches[o.peer].append(item);
    }
    for (const auto &peer : batches.keys())
        flush(peer);
}
} // namespace pettime
