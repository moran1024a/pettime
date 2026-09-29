#include "SettingsStore.h"
#include <QDateTime>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace pettime {
SettingsStore::SettingsStore(QString directory)
    : directory_(directory.isEmpty() ? QCoreApplication::applicationDirPath() : directory),
      portableDefault_(directory.isEmpty()) {}
QString SettingsStore::legacyDirectory() const {
    return portableDefault_
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : directory_;
}
bool SettingsStore::hasFile(const QString &name) const {
    return QFileInfo::exists(QDir(directory_).filePath(name));
}
QJsonObject SettingsStore::loadJson(const QString &name, QString *warning) const {
    QFile file(QDir(directory_).filePath(name));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (warning) *warning = file.fileName() + ": " + file.errorString();
        return {};
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.read(2 * 1024 * 1024 + 1), &parse);
    if (file.size() > 2 * 1024 * 1024 || parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (warning) *warning = file.fileName() + " 格式无效，已使用默认值；原文件会保留为 .invalid 备份。";
        return {};
    }
    return document.object();
}
bool SettingsStore::saveJson(const QString &name, const QJsonObject &object, QString *error) const {
    if (!QDir().mkpath(directory_)) {
        if (error) *error = "无法创建存档目录：" + directory_;
        return false;
    }
    const QString path = QDir(directory_).filePath(name);
    QString warning;
    loadJson(name, &warning);
    if (!warning.isEmpty()) {
        QString backup = path + ".invalid";
        for (int i = 1; QFileInfo::exists(backup); ++i)
            backup = path + ".invalid." + QString::number(i);
        if (!QFile::copy(path, backup)) {
            if (error) *error = "无法备份损坏存档，未覆盖原文件：" + path;
            return false;
        }
    }
    QSaveFile file(path);
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = path + ": " + file.errorString();
        return false;
    }
    return true;
}
int SettingsStore::loadAffinity(QString *warning) const {
    if (hasFile("progress.json")) {
        const auto pets = loadJson("progress.json", warning).value("pets").toArray();
        for (const auto &value : pets) {
            const auto p = value.toObject();
            if (p.value("id").toString() == "1")
                return std::clamp(p.value("affection").toInt(), 0, 100);
        }
        return 0;
    }
    QString path = QDir(directory_).filePath("affinity.dat");
#ifdef Q_OS_WIN
    if (!QFileInfo::exists(path)) {
        const QString legacy =
            QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("XiaoqiangPet/affinity.dat");
        if (QFileInfo::exists(legacy)) {
            path = legacy;
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
    if (hasFile("config.json"))
        return std::clamp(loadJson("config.json", warning).value("populationLimit").toInt(72), 1, 72);
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
