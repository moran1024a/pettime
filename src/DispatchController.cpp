#include "DispatchController.h"
#include <QJsonDocument>
#include <QSysInfo>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace pettime {
namespace {
constexpr qint64 ConfirmMs = 3000;
constexpr qint64 AnimationWatchdogMs = 10000;
constexpr qint64 InterpolationMs = 100;
QPointF safePosition(QPointF point, QRectF area) {
    const double mx = std::min(36.0, area.width() * .45);
    const double my = std::min(36.0, area.height() * .45);
    return {std::clamp(point.x(), area.left() + mx, area.right() - mx),
            std::clamp(point.y(), area.top() + my, area.bottom() - my)};
}
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
DispatchController::~DispatchController() {
    // The application explicitly shuts down while its windows are alive. During
    // member destruction their callbacks/receivers may already have been destroyed.
    disconnect(this, nullptr, nullptr, nullptr);
    hideLocal = {};
    hideVisitor = {};
    localSurface = {};
    shutdown();
}
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
bool DispatchController::isLocalVisible(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    return it == outgoing_.cend() || !it->localHidden;
}
bool DispatchController::isAway(quint64 id) const { return !isLocalVisible(id); }
int DispatchController::awayCount() const {
    return int(std::count_if(outgoing_.cbegin(), outgoing_.cend(),
                             [](const Outgoing &o) { return o.localHidden; }));
}
bool DispatchController::canControl(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    return it == outgoing_.cend() || (it->phase == Phase::Active && it->frozenAt < 0 &&
                                     !it->awaitingActive && !it->recallRequested);
}
bool DispatchController::canHighlight(quint64 id) const {
    const auto *p = swarm_.find(id);
    if (!p || p->dispatchPaused) return false;
    const auto it = outgoing_.constFind(id);
    return it == outgoing_.cend() || (!it->localHidden || it->phase == Phase::Active ||
                                    it->phase == Phase::LeavingRemote);
}
QString DispatchController::location(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    return it != outgoing_.cend() && it->localHidden ? peerName(it->peer)
                                                    : QStringLiteral("本机");
}
QString DispatchController::stateText(quint64 id) const {
    const auto it = outgoing_.constFind(id);
    if (it == outgoing_.cend()) {
        const auto *p = swarm_.find(id);
        return p && p->entering() ? QStringLiteral("返回入场中") : QStringLiteral("本地");
    }
    const auto &o = *it;
    if (o.frozenAt >= 0)
        return QString("等待重连（%1 秒）%2")
            .arg(std::max<qint64>(0, (GraceMs - now() + o.frozenAt + 999) / 1000))
            .arg(o.recallRequested ? " · 待召回" : "");
    switch (o.phase) {
    case Phase::Offering: return "请求中";
    case Phase::LeavingLocal: return "本机离场中";
    case Phase::Activating: return "交接确认中";
    case Phase::Active: {
        const auto *p = swarm_.find(id);
        return p && p->entering() ? "远端入场中" : "已派遣";
    }
    case Phase::LeavingRemote: return "远端离场中";
    case Phase::AwaitingHidden: return "等待离场确认";
    }
    return {};
}
QString DispatchController::visitorStateText(const Visitor &v) const {
    if (v.hidden) return "已隐藏";
    if (v.frozen) return "等待重连";
    if (!v.active) return "预留中";
    if (v.finishing) return "离场确认中";
    if (v.stage == 1) return "离场中";
    if (v.stage == 2) return "入场中";
    return v.returnRequested ? "等待返程" : "只读访客";
}
QPointF DispatchController::savedPosition(quint64 id) const {
    const auto *p = swarm_.find(id);
    if (!p) return {};
    const auto it = outgoing_.constFind(id);
    const auto point = it == outgoing_.cend() ? p->position : it->home;
    const auto area = localSurface ? localSurface(point)
                                  : it == outgoing_.cend() ? p->area : it->homeArea;
    return safePosition(point, area);
}
void DispatchController::updateLocalSurface(quint64 id, QRectF area) {
    auto it = outgoing_.find(id);
    if (it == outgoing_.end() || area.width() <= 0 || area.height() <= 0) return;
    it->homeArea = area;
    it->home = safePosition(it->home, area);
    if (!it->localHidden)
        if (auto *p = swarm_.find(id)) p->setArea(area);
}
QString DispatchController::dispatchPets(const QList<quint64> &ids, const QString &peer) {
    if (stopping_ || !network_.dispatchReady(peer))
        return "目标未连接或不支持派遣，请先开启双方网络服务。";
    int accepted = 0, skipped = 0;
    QSet<quint64> unique(ids.begin(), ids.end());
    for (auto id : unique) {
        auto *p = swarm_.find(id);
        if (!p || p->primary || p->petting || !p->active() || p->expired || p->splitReady || p->transitioning() ||
            outgoing_.contains(id)) {
            ++skipped;
            continue;
        }
        Outgoing o;
        o.entity = id;
        o.id = uuid();
        o.stream = uuid();
        o.peer = peer;
        o.session = network_.peerSession(peer);
        o.home = p->position;
        o.homeArea = p->area;
        o.started = now();
        outgoing_.insert(id, o);
        if (!send(peer, "offer",
                  {{"id", o.id}, {"stream", o.stream}, {"entity", QString::number(id)}, {"name", name(*p)}})) {
            outgoing_.remove(id);
            ++skipped;
            continue;
        }
        p->setManualControl(false);
        p->dispatchLocked = true;
        ++accepted;
    }
    emit changed();
    return QString("已申请 %1 只，跳过 %2 只；最终结果以目标确认和列表状态为准。")
        .arg(accepted)
        .arg(skipped);
}
void DispatchController::finish(quint64 id, bool restore) {
    if (!outgoing_.contains(id)) return;
    const auto o = outgoing_.take(id);
    if (auto *p = swarm_.find(id)) {
        const bool outside = !p->area.contains(p->position);
        const int edge = p->transitionEdge();
        const double fraction = p->transitionFraction();
        p->cancelTransition();
        p->setManualControl(false);
        p->motionGroup.clear();
        p->dispatchPaused = p->dispatchLocked = false;
        if (restore && p->active() && !p->expired) {
            const QRectF area = localSurface ? localSurface(o.home) : o.homeArea;
            if (o.localHidden)
                p->beginEntry(area, o.homeEdge, o.homeFraction);
            else if (outside)
                p->beginEntry(area, edge, fraction);
            else
                p->moveTo(p->position, area);
        }
    }
    emit changed();
}
void DispatchController::activate(quint64 id) {
    auto it = outgoing_.find(id);
    auto *p = swarm_.find(id);
    if (it == outgoing_.end() || !p || it->phase != Phase::LeavingLocal ||
        it->frozenAt >= 0 || !p->exitComplete()) return;
    it->homeEdge = p->transitionEdge();
    it->homeFraction = p->transitionFraction();
    // The renderer hides synchronously. Never rely on deferred changed() reconciliation.
    if (hideLocal) hideLocal(id);
    it->localHidden = true;
    p->motionGroup = it->peer;
    p->dispatchPaused = true;
    p->beginEntry(it->remoteArea, it->homeEdge ^ 1, it->homeFraction);
    it->phase = Phase::Activating;
    it->started = now();
    it->awaitingActive = true;
    send(it->peer, "activate", {{"id", it->id}, {"stream", it->stream},
        {"state", PetRenderState::from(*p).encode()}, {"name", name(*p)}, {"stage", 2}});
    emit changed();
}
void DispatchController::beginReturn(quint64 id) {
    auto it = outgoing_.find(id);
    auto *p = swarm_.find(id);
    if (it == outgoing_.end() || !p || it->phase != Phase::Active || it->frozenAt >= 0)
        return;
    it->recallRequested = true;
    p->dispatchLocked = true;
    if (!p->beginExit()) {
        removeEntity(id);
        return;
    }
    it->phase = Phase::LeavingRemote;
    it->started = now();
    emit changed();
}
void DispatchController::sendExit(quint64 id) {
    auto it = outgoing_.find(id);
    auto *p = swarm_.find(id);
    if (it == outgoing_.end() || !p) return;
    it->phase = Phase::AwaitingHidden;
    it->started = now();
    p->dispatchPaused = true;
    send(it->peer, "finishExit", {{"id", it->id}, {"stream", it->stream},
        {"sequence", QString::number(++it->sequence)},
        {"state", PetRenderState::from(*p).encode()}, {"name", name(*p)}});
    emit changed();
}
void DispatchController::recallPet(quint64 id) {
    auto it = outgoing_.find(id);
    if (it == outgoing_.end()) return;
    it->recallRequested = true;
    if (!it->localHidden) {
        const auto copy = *it;
        finish(id);
        send(copy.peer, "recall", {{"id", copy.id}});
    } else if (it->phase == Phase::Active && it->frozenAt < 0 &&
               network_.dispatchReady(it->peer)) {
        beginReturn(id);
    }
    // During activation/freeze retain the intent until the peer's state is known.
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
    if (!outgoing_.contains(id) || !isAway(id) || !canHighlight(id))
        return false;
    const auto o = outgoing_[id];
    return send(
        o.peer, "highlight",
        {{"id", o.id}, {"stream", o.stream}, {"event", QString::number(++highlightSerial_)}});
}
bool DispatchController::expelVisitor(const QString &id) {
    auto it = visitors_.find(id);
    if (it == visitors_.end()) return false;
    const auto peer = it->peer;
    if (!it->active || it->frozen || !network_.dispatchReady(peer)) {
        eraseVisitor(id);
        send(peer, "gone", {{"id", id}});
    } else {
        if (it->returnRequested) return true;
        it->returnRequested = true;
        it->returnRequestedAt = now();
        send(peer, "returnRequest", {{"id", id}, {"stream", it->stream}});
        emit changed();
    }
    return true;
}
void DispatchController::eraseVisitor(const QString &id) {
    auto it = visitors_.find(id);
    if (it == visitors_.end()) return;
    it->hidden = true;
    if (hideVisitor) hideVisitor(id);
    const auto v = visitors_.take(id);
    auto &closed = ended_[v.peer];
    if (closed.size() < 4096) closed.insert(id);
    emit changed();
}
void DispatchController::freeze(Outgoing &o) {
    if (o.phase == Phase::Offering) return;
    if (o.frozenAt < 0) o.frozenAt = now();
    // Preserve the exact transition phase, including a pending hide acknowledgement.
    o.awaitingActive = false;
    if (auto *p = swarm_.find(o.entity)) {
        p->setManualControl(false);
        p->dispatchPaused = p->dispatchLocked = true;
    }
}
void DispatchController::freeze(Visitor &v) {
    if (v.hidden) return;
    if (v.frozenAt < 0) v.frozenAt = now();
    v.frozen = true;
    v.awaitingState = false;
}
void DispatchController::requestStop() {
    if (stopping_ || !network_.running()) return;
    stopping_ = true;
    recallAll();
    for (const auto &id : visitors_.keys()) expelVisitor(id);
    emit changed();
    if (outgoing_.isEmpty() && visitors_.isEmpty()) network_.stop();
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
        if (o.phase == Phase::Offering || o.session != session) {
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
    stopping_ = false;
    emit changed();
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
        for (auto &o : outgoing_)
            if (o.peer == peer) {
                o.remoteArea = area;
                if (o.localHidden)
                    if (auto *p = swarm_.find(o.entity)) {
                        p->setArea(area);
                        if (o.phase == Phase::AwaitingHidden && !p->exitComplete()) {
                            o.phase = Phase::LeavingRemote;
                            // The old terminal snapshot may now be inside the new surface.
                            // A new stream cancels its pending hide and resumes the new path.
                            const bool wasFrozen = o.frozenAt >= 0;
                            freeze(o);
                            if (!wasFrozen) resume(o);
                        }
                    }
            }
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
        receiveStates(peer, session, m);
        return;
    }
    if (op == "renew") {
        if (!m["items"].isArray() || m["items"].toArray().size() > 71) { invalid(); return; }
        for (const auto &item : m["items"].toArray()) {
            const auto a = item.toArray();
            if (a.size() != 2 || !validUuid(a[0].toString()) || !validUuid(a[1].toString())) {
                invalid(); return;
            }
            auto it = visitors_.find(a[0].toString());
            if (it == visitors_.end()) { send(peer, "gone", {{"id", a[0]}}); continue; }
            if (it->peer != peer || it->session != session) { invalid(); return; }
            if (it->active || (it->frozen && !it->awaitingState) || it->stream != a[1].toString())
                continue;
            if (it->frozenAt >= 0 && now() - it->frozenAt >= GraceMs) continue;
            it->lastState = now();
            it->awaitingState = it->frozen = false;
            it->frozenAt = -1;
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
        if (stopping_ || ended_[peer].contains(id) || ended_[peer].size() >= 4096 ||
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
            v.stream = validUuid(m["stream"].toString()) ? m["stream"].toString() : QString{};
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
    if (op == "returnRequest") {
        if (out != outgoing_.end() && out->stream == m["stream"].toString()) {
            emit notice("对方请求驱赶，实体正在返回本机。");
            recallPet(out.key());
        }
        return;
    }
    if (op == "hidden") {
        quint64 sequence;
        if (!serial(m["sequence"], sequence) || !validUuid(m["stream"].toString())) {
            invalid(); return;
        }
        if (out != outgoing_.end() && out->phase == Phase::AwaitingHidden &&
            out->stream == m["stream"].toString() && out->sequence == sequence)
            finish(out.key());
        return;
    }
    if (op == "recalled" || op == "reject" || op == "gone") {
        if (out != outgoing_.end()) {
            // Recalled acknowledges forced cancellation, never a normal return animation.
            if (op == "recalled") return;
            if (op == "reject" && out->phase != Phase::Offering) return;
            if (op == "reject") emit notice("派遣未完成：" + m["reason"].toString().left(160));
            finish(out.key());
        }
        return;
    }
    if (op == "finishExit") {
        PetRenderState state;
        quint64 sequence;
        const QString stream = m["stream"].toString();
        if (!validUuid(stream) || !serial(m["sequence"], sequence) ||
            !PetRenderState::decode(m["state"], state)) { invalid(); return; }
        if (visitor == visitors_.end()) { send(peer, "gone", {{"id", id}}); return; }
        auto &v = *visitor;
        if (!v.active || v.hidden || v.stream != stream || (v.frozen && !v.awaitingState) ||
            (v.frozenAt >= 0 && now() - v.frozenAt >= GraceMs)) return;
        if (sequence <= v.sequence || v.finishing) return;
        v.previous = interpolated(v);
        v.current = state;
        v.sequence = sequence;
        v.lastState = now();
        v.stage = 1;
        v.finishing = true;
        v.awaitingState = v.frozen = false;
        v.frozenAt = -1;
        emit changed();
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
        out->remoteArea = area;
        out->home = p->position;
        out->homeArea = p->area;
        p->dispatchLocked = true;
        if (!p->beginExit()) { removeEntity(out.key()); return; }
        out->homeEdge = p->transitionEdge();
        out->homeFraction = p->transitionFraction();
        out->phase = Phase::LeavingLocal;
        out->started = now();
        emit changed();
        return;
    }
    if (op == "resume") {
        if (!validUuid(m["stream"].toString())) {
            invalid();
            return;
        }
        if (visitor == visitors_.end() || visitor->hidden ||
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
            out->remoteArea = area;
            if (out->localHidden) {
                p->setArea(area);
                if (out->phase == Phase::AwaitingHidden && !p->exitComplete())
                    out->phase = Phase::LeavingRemote;
            }
            out->awaitingActive = true;
            send(peer, "commit",
                 {{"id", id},
                  {"stream", out->stream},
                  {"phase", int(out->phase)},
                  {"stage", out->phase >= Phase::LeavingRemote ? 1 : p->entering() ? 2 : 0},
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
        if (op == "commit" && v.pendingStream != stream) return;
        const int phase = op == "commit" ? m["phase"].toInt(-1) : int(Phase::Active);
        if (phase < int(Phase::LeavingLocal) || phase > int(Phase::AwaitingHidden)) {
            invalid(); return;
        }
        if (op == "activate" && !v.active && now() - v.lastState >= ConfirmMs) {
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
        v.active = phase != int(Phase::LeavingLocal);
        v.stage = phase >= int(Phase::LeavingRemote) ? 1 : m["stage"].toInt(2);
        if (v.stage < 0 || v.stage > 2) { invalid(); return; }
        v.finishing = false;
        v.awaitingState = op == "commit";
        v.frozen = v.awaitingState;
        if (!v.awaitingState)
            v.frozenAt = -1;
        send(peer, "active", {{"id", id}, {"stream", stream}});
        if (v.returnRequested) {
            v.returnRequestedAt = now();
            send(peer, "returnRequest", {{"id", id}, {"stream", stream}});
        }
        emit changed();
        return;
    }
    if (op == "active") {
        if (out == outgoing_.end() || !out->awaitingActive ||
            out->stream != m["stream"].toString() || out->phase == Phase::Offering) return;
        if (out->frozenAt >= 0 && now() - out->frozenAt >= GraceMs) {
            const auto entity = out.key();
            const auto old = *out;
            finish(entity);
            send(peer, "recall", {{"id", old.id}});
            return;
        }
        if (out->phase == Phase::Activating) out->phase = Phase::Active;
        out->frozenAt = -1;
        out->awaitingActive = false;
        out->started = now();
        if (auto *p = swarm_.find(out.key())) {
            p->dispatchPaused = out->phase == Phase::AwaitingHidden;
            p->dispatchLocked = out->phase != Phase::Active;
        }
        const auto entity = out.key();
        if (out->phase == Phase::AwaitingHidden) sendExit(entity);
        else if (out->phase == Phase::Active && out->recallRequested) beginReturn(entity);
        emit changed();
        return;
    }
    if (op == "highlight") {
        quint64 event;
        if (!serial(m["event"], event)) {
            invalid();
            return;
        }
        if (visitor != visitors_.end() && visitor->active && !visitor->hidden && !visitor->frozen &&
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
        auto *p = swarm_.find(id);
        if (!p || !p->active() || p->expired || p->splitReady) {
            // Remote retirement is destructive cleanup, not a return. Do not
            // briefly materialize its fading model in a local window.
            if (p && o.localHidden) p->expired = true;
            removeEntity(id);
            continue;
        }
        if ((o.frozenAt >= 0 && time - o.frozenAt >= GraceMs) ||
            (o.phase == Phase::Offering && time - o.started >= ConfirmMs)) {
            finish(id);
            send(o.peer, "recall", {{"id", o.id}});
            if (o.phase == Phase::Offering) emit notice("派遣请求超时，实体仍保留在本机。");
            continue;
        }
        if (o.frozenAt >= 0) continue;
        if (o.phase == Phase::LeavingLocal && p->exitComplete()) activate(id);
        else if (o.phase == Phase::LeavingRemote && p->exitComplete()) sendExit(id);
        else if (((o.phase == Phase::Activating || o.phase == Phase::AwaitingHidden) &&
                   time - o.started >= ConfirmMs) ||
                 ((o.phase == Phase::LeavingLocal || o.phase == Phase::LeavingRemote) &&
                   time - o.started >= AnimationWatchdogMs)) {
            freeze(outgoing_[id]);
            network_.disconnectPeer(o.peer);
            emit changed();
        }
    }
    for (const auto &id : visitors_.keys()) {
        auto &v = visitors_[id];
        if (v.frozenAt >= 0 && time - v.frozenAt >= GraceMs) {
            const auto peer = v.peer;
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
        } else if (!v.frozen && v.finishing && time - v.lastState >= InterpolationMs) {
            const auto copy = v;
            eraseVisitor(id); // Synchronous window hide precedes the acknowledgement.
            send(copy.peer, "hidden", {{"id", id}, {"stream", copy.stream},
                                      {"sequence", QString::number(copy.sequence)}});
        } else if (!v.frozen && !v.active && time - v.lastState >= ConfirmMs) {
            const auto peer = v.peer;
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
        } else if (!v.frozen && ((v.active && time - v.lastState >= ConfirmMs) ||
                   (v.returnRequested && v.stage == 0 && v.returnRequestedAt >= 0 &&
                    time - v.returnRequestedAt >= AnimationWatchdogMs))) {
            const auto peer = v.peer;
            freeze(v);
            network_.disconnectPeer(peer);
        }
    }
    if (stopping_ && outgoing_.isEmpty() && visitors_.isEmpty()) {
        network_.stop();
        return;
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
    if (time >= nextRenew_) {
        nextRenew_ = time + 500;
        QHash<QString, QJsonArray> reservations;
        for (const auto &o : outgoing_)
            if (o.phase == Phase::LeavingLocal && o.frozenAt < 0 && !o.awaitingActive)
                reservations[o.peer].append(QJsonArray{o.id, o.stream});
        for (auto it = reservations.cbegin(); it != reservations.cend(); ++it)
            send(it.key(), "renew", {{"items", it.value()}}, "reservations");
    }
    if (time >= nextState_) {
        nextState_ = time + 100;
        publishStates();
    }
}
void DispatchController::publishStates() {
    QHash<QString, QJsonArray> batches;
    QHash<QString, int> parts, bytes;
    auto flush = [&](const QString &peer) {
        if (batches[peer].isEmpty()) return;
        send(peer, "states", {{"items", batches[peer]}},
             "states-" + QString::number(parts[peer]++));
        batches[peer] = {};
        bytes[peer] = 2;
    };
    for (auto &o : outgoing_) {
        if ((o.phase != Phase::Active && o.phase != Phase::LeavingRemote) ||
            o.frozenAt >= 0 || o.awaitingActive) continue;
        const auto *p = swarm_.find(o.entity);
        if (!p) continue;
        const QJsonArray item{o.id, o.stream, QString::number(++o.sequence),
            PetRenderState::from(*p).encode(), name(*p), o.phase == Phase::LeavingRemote ? 1 : p->entering() ? 2 : 0};
        // Measure each item once instead of repeatedly serializing the growing batch.
        const int size = QJsonDocument(item).toJson(QJsonDocument::Compact).size();
        if (bytes.value(o.peer, 2) + size + 1 > 7600) flush(o.peer);
        bytes[o.peer] = bytes.value(o.peer, 2) + size + (batches[o.peer].isEmpty() ? 0 : 1);
        batches[o.peer].append(item);
    }
    for (const auto &peer : batches.keys()) flush(peer);
}
void DispatchController::receiveStates(const QString &peer, const QString &session,
                                        const QJsonObject &m) {
    auto invalid = [&] { network_.disconnectPeer(peer); };
    if (!m["items"].isArray() || m["items"].toArray().size() > 71) { invalid(); return; }
    for (const auto &item : m["items"].toArray()) {
        const auto a = item.toArray();
        PetRenderState state;
        quint64 sequence;
        if (a.size() != 6 || !validUuid(a[0].toString()) || !validUuid(a[1].toString()) ||
            !serial(a[2], sequence) || !PetRenderState::decode(a[3], state) ||
            !a[4].isString() || a[4].toString().size() > 160 ||
            !a[5].isDouble() || (a[5].toDouble() != 0 && a[5].toDouble() != 1 && a[5].toDouble() != 2)) {
            invalid(); return;
        }
        auto it = visitors_.find(a[0].toString());
        if (it == visitors_.end()) { send(peer, "gone", {{"id", a[0]}}); continue; }
        auto &v = *it;
        if (v.peer != peer || v.session != session) { invalid(); return; }
        if (!v.active || v.hidden || v.finishing || (v.frozen && !v.awaitingState) ||
            v.stream != a[1].toString() || sequence <= v.sequence ||
            (v.stage == 1 && a[5].toInt() != 1)) continue;
        if (v.frozenAt >= 0 && now() - v.frozenAt >= GraceMs) {
            const auto id = v.id;
            eraseVisitor(id);
            send(peer, "gone", {{"id", id}});
            continue;
        }
        v.previous = interpolated(v);
        v.current = state;
        v.sequence = sequence;
        v.lastState = now();
        v.name = a[4].toString();
        v.stage = a[5].toInt();
        if (v.awaitingState) {
            v.awaitingState = v.frozen = false;
            v.frozenAt = -1;
            emit changed();
        }
    }
}
} // namespace pettime
