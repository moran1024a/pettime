// Explicitly invoked two-machine test harness, not part of the application or CTest.
#include "DispatchController.h"
#include "PetWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>
#include <map>

using namespace pettime;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addOptions({{"data-dir", "Isolated data directory", "path"},
                       {"control", "JSON command file", "path"},
                       {"report", "JSON report file", "path"},
                       {"duration", "Safety exit seconds", "seconds", "300"}});
    parser.process(app);
    for (const auto &key : {"data-dir", "control", "report"})
        if (!parser.isSet(key))
            return 2;
    const auto area = [] { return QRectF(QGuiApplication::primaryScreen()->availableGeometry()); };
    AnimationLibrary animation;
    SwarmController swarm(42, area().center(), area());
    swarm.spawn(area().center(), area(), 1, 2);
    std::unique_ptr<DispatchController> dispatch;
    NetworkService network(SettingsStore(parser.value("data-dir")), [&] {
        return NetworkService::Info{swarm.totalCount(), swarm.limit(),
                                    dispatch ? int(dispatch->outgoing().size()) : 0,
                                    dispatch ? int(dispatch->visitors().size()) : 0};
    });
    dispatch = std::make_unique<DispatchController>(network, swarm, area);
    swarm.beforeRemove = [&](quint64 id) { dispatch->removeEntity(id); };
    std::map<QString, std::unique_ptr<PetWindow>> windows;
    quint64 command = 0;
    int highlights = 0;
    QString result;
    QJsonArray events;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&network, &NetworkService::peerLost, [&](const QString &) {
        events.append(QJsonObject{{"ms", elapsed.elapsed()}, {"event", "disconnected"}});
    });
    QObject::connect(&network, &NetworkService::peerReady, [&](const QString &, const QString &) {
        events.append(QJsonObject{{"ms", elapsed.elapsed()}, {"event", "connected"}});
    });
    QObject::connect(dispatch.get(), &DispatchController::notice,
                     [&](const QString &s) { result = s; });
    QObject::connect(dispatch.get(), &DispatchController::visitorHighlight, [&](const QString &id) {
        ++highlights;
        auto it = windows.find(id);
        if (it != windows.end())
            it->second->highlight();
    });
    if (!network.start()) {
        qCritical().noquote() << network.status();
        return 3;
    }
    QTimer timer;
    qint64 last = elapsed.elapsed(), reportAt = 0;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        QFile file(parser.value("control"));
        if (file.open(QIODevice::ReadOnly)) {
            const auto o = QJsonDocument::fromJson(file.readAll()).object();
            const auto serial = o["serial"].toVariant().toULongLong();
            if (serial > command) {
                command = serial;
                const auto op = o["op"].toString();
                if (op == "dispatch") {
                    QList<quint64> ids;
                    if (o["all"].toBool()) {
                        for (const auto &p : swarm.pets())
                            if (!p->primary)
                                ids.append(p->id);
                    } else
                        for (const auto &v : o["ids"].toArray())
                            ids.append(v.toVariant().toULongLong());
                    result = dispatch->dispatchPets(ids, o["peer"].toString());
                } else if (op == "recall")
                    dispatch->recallPet(o["id"].toVariant().toULongLong());
                else if (op == "recallAll")
                    dispatch->recallAll();
                else if (op == "highlight")
                    dispatch->highlightPet(o["id"].toVariant().toULongLong());
                else if (op == "rename")
                    swarm.rename(o["id"].toVariant().toULongLong(), o["name"].toString());
                else if (op == "feed") {
                    if (auto *p = swarm.find(o["id"].toVariant().toULongLong()))
                        p->playFeeding();
                } else if (op == "spawn")
                    swarm.spawn(area().center(), area(), 1, o["count"].toInt());
                else if (op == "limit")
                    swarm.setLimit(o["count"].toInt());
                else if (op == "disconnect")
                    network.disconnectPeer(o["peer"].toString());
                else if (op == "stop")
                    network.stop();
                else if (op == "start")
                    network.start();
                else if (op == "quit")
                    app.quit();
            }
        }
        dispatch->tick();
        const auto now = elapsed.elapsed();
        swarm.advance(std::min(.1, (now - last) / 1000.0));
        last = now;
        for (auto it = windows.begin(); it != windows.end();) {
            if (!dispatch->visitors().contains(it->first))
                it = windows.erase(it);
            else
                ++it;
        }
        for (auto it = dispatch->visitors().cbegin(); it != dispatch->visitors().cend(); ++it) {
            if (!it->active)
                continue;
            auto &w = windows[it.key()];
            if (!w)
                w = std::make_unique<PetWindow>(animation);
            w->presentRemote(dispatch->interpolated(*it), it->name, it->frozen);
            if (!w->isVisible())
                w->show();
        }
        if (now < reportAt)
            return;
        reportAt = now + 200;
        QJsonArray devices, own, visitors;
        for (const auto &d : network.devices())
            devices.append(QJsonObject{{"id", d.id},
                                       {"name", d.name},
                                       {"ip", d.address},
                                       {"state", d.state},
                                       {"dispatch", d.dispatch}});
        for (const auto &p : swarm.pets()) {
            const auto it = dispatch->outgoing().constFind(p->id);
            own.append(QJsonObject{
                {"id", QString::number(p->id)},
                {"name", p->displayName()},
                {"away", dispatch->isAway(p->id)},
                {"paused", p->dispatchPaused},
                {"age", p->age},
                {"feeding", p->cosmeticFeeding},
                {"x", p->position.x()},
                {"y", p->position.y()},
                {"state", dispatch->stateText(p->id)},
                {"dispatchId", it == dispatch->outgoing().cend() ? QString{} : it->id}});
        }
        for (const auto &v : dispatch->visitors())
            visitors.append(
                QJsonObject{{"id", v.id},
                            {"entity", v.entity},
                            {"name", v.name},
                            {"active", v.active},
                            {"frozen", v.frozen},
                            {"x", v.current.position.x()},
                            {"y", v.current.position.y()},
                            {"action", int(v.current.state)},
                            {"sequence", double(v.sequence)},
                            {"frames", windows.count(v.id) ? windows[v.id]->paintedFrames() : 0}});
        QSaveFile out(parser.value("report"));
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(QJsonObject{{"pid", double(QCoreApplication::applicationPid())},
                                                {"deviceId", network.deviceId()},
                                                {"session", network.sessionId()},
                                                {"ms", now},
                                                {"command", double(command)},
                                                {"result", result},
                                                {"devices", devices},
                                                {"own", own},
                                                {"visitors", visitors},
                                                {"events", events},
                                                {"highlights", highlights},
                                                {"limit", swarm.limit()},
                                                {"platform", QGuiApplication::platformName()}})
                          .toJson());
            out.commit();
        }
    });
    timer.start(33);
    QTimer::singleShot(parser.value("duration").toInt() * 1000, &app, &QCoreApplication::quit);
    const int code = app.exec();
    dispatch->shutdown();
    network.stop();
    return code;
}
