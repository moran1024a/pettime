#include "SettingsStore.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace pettime {
SettingsStore::SettingsStore(QString directory)
    : directory_(directory.isEmpty()
                     ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                     : std::move(directory)) {}
int SettingsStore::loadAffinity(QString *warning) const {
    QString path = QDir(directory_).filePath("affinity.dat");
    bool migrate = false;
#ifdef Q_OS_WIN
    if (!QFileInfo::exists(path)) {
        const QString legacy =
            QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("XiaoqiangPet/affinity.dat");
        if (QFileInfo::exists(legacy)) {
            path = legacy;
            migrate = true;
        }
    }
#endif
    QFile file(path);
    if (!file.exists())
        return 0;
    if (!file.open(QIODevice::ReadOnly)) {
        if (warning)
            *warning = file.errorString();
        return 0;
    }
    bool ok = false;
    const int value = file.read(128).trimmed().toInt(&ok);
    if (!ok) {
        if (warning)
            *warning = "好感度存档损坏，已使用初始值。";
        return 0;
    }
    const int bounded = std::clamp(value, 0, 100);
    if (migrate)
        saveAffinity(bounded, warning);
    return bounded;
}
bool SettingsStore::saveAffinity(int value, QString *error) const {
    if (!QDir().mkpath(directory_)) {
        if (error)
            *error = "无法创建存档目录：" + directory_;
        return false;
    }
    QSaveFile file(QDir(directory_).filePath("affinity.dat"));
    const QByteArray data = QByteArray::number(std::clamp(value, 0, 100)) + "\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
int SettingsStore::loadLimit(QString *warning) const {
    QFile file(QDir(directory_).filePath("population-limit.dat"));
    if (!file.exists())
        return 72;
    if (!file.open(QIODevice::ReadOnly)) {
        if (warning)
            *warning = file.errorString();
        return 72;
    }
    bool ok = false;
    const int value = file.read(128).trimmed().toInt(&ok);
    if (!ok || value < 1 || value > 72) {
        if (warning)
            *warning = "数量上限存档损坏，已使用默认值 72。";
        return 72;
    }
    return value;
}
bool SettingsStore::saveLimit(int value, QString *error) const {
    if (!QDir().mkpath(directory_)) {
        if (error)
            *error = "无法创建存档目录：" + directory_;
        return false;
    }
    QSaveFile file(QDir(directory_).filePath("population-limit.dat"));
    const QByteArray data = QByteArray::number(std::clamp(value, 1, 72)) + "\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
QString SettingsStore::networkDeviceId(QString *error) const {
    QFile existing(QDir(directory_).filePath("network-device-id.dat"));
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) {
            if (error) *error = existing.errorString();
            return {};
        }
        const QByteArray data = existing.read(128).trimmed();
        const QUuid id(QString::fromUtf8(data));
        if (existing.size() > 64 || id.isNull() || data != id.toString(QUuid::WithoutBraces).toUtf8()) {
            if (error) *error = "网络设备 ID 存档损坏，请检查 network-device-id.dat。";
            return {};
        }
        return QString::fromUtf8(data);
    }
    if (!QDir().mkpath(directory_)) {
        if (error) *error = "无法创建网络设备 ID 存档目录。";
        return {};
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSaveFile file(existing.fileName());
    const QByteArray data = id.toUtf8() + "\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return {};
    }
    return id;
}
void SettingsStore::log(const QString &message) const {
    if (!QDir().mkpath(directory_))
        return;
    const QString path = QDir(directory_).filePath("runtime.log");
    if (QFileInfo(path).size() > 2 * 1024 * 1024) {
        QFile::remove(path + ".1");
        QFile::rename(path, path + ".1");
    }
    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " " + message + "\n")
                       .toUtf8());
}
} // namespace pettime
