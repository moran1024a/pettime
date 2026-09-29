#include "SingleInstance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>
#include <QVariant>

namespace pettime {
SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (auto *socket = server_.nextPendingConnection()) {
            socket->setReadBufferSize(64);
            auto read = [this, socket] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (request == "recall\n") {
                    emit recallRequested();
                    socket->write("ok\n");
                    socket->disconnectFromServer();
                } else if (request.size() > 32 || request.contains('\n'))
                    socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, [socket] {
                socket->abort();
                socket->deleteLater();
            });
            if (socket->bytesAvailable())
                read();
        }
    });
}
SingleInstance::~SingleInstance() {
    // Close/unlink the server before releasing the lock. A new process must not
    // have its freshly-created socket removed by this instance's destructor.
    server_.close();
    if (lock_)
        lock_->unlock();
}
SingleInstance::Result SingleInstance::start(const QString &directory, const QString &scope,
                                             QString *error) {
    if (!QDir().mkpath(directory)) {
        *error = "无法创建用户数据目录：" + directory;
        return Result::Error;
    }
    const QByteArray identity = (QDir(directory).absolutePath() + "|" + scope).toUtf8();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(24));
    const QString name = "pettime-" + key;
    lock_ = std::make_unique<QLockFile>(QDir(directory).filePath(name + ".lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        if (lock_->error() != QLockFile::LockFailedError) {
            *error = "无法写入程序数据目录，请确认目录可写：" + directory;
            return Result::Error;
        }
        QLocalSocket socket;
        // The first process may still be setting up its listener immediately after locking.
        for (int attempt = 0; attempt < 10; ++attempt) {
            socket.connectToServer(name);
            if (socket.waitForConnected(200))
                break;
            socket.abort();
            QThread::msleep(50);
        }
        if (socket.state() != QLocalSocket::ConnectedState) {
            *error = "已有实例占用锁，但无法连接召回服务：" + socket.errorString();
            return Result::Error;
        }
        socket.write("recall\n");
        socket.flush();
        QByteArray reply;
        QElapsedTimer deadline;
        deadline.start();
        while (!reply.contains('\n') && deadline.elapsed() < 5000) {
            if (!socket.bytesAvailable() && !socket.waitForReadyRead(5000 - deadline.elapsed()))
                break;
            reply += socket.readAll();
        }
        if (reply != "ok\n") {
            *error = "已有实例没有确认召回请求。";
            return Result::Error;
        }
        return Result::Recalled;
    }
    // Only the lock owner may clean up a socket left behind by a crashed process.
    QLocalServer::removeServer(name);
    if (!server_.listen(name)) {
        *error = server_.errorString();
        lock_->unlock();
        return Result::Error;
    }
    return Result::Primary;
}
} // namespace pettime
