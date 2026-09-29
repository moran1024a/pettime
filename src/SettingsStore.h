#pragma once
#include <QString>
#include <QJsonObject>
namespace pettime {
class SettingsStore {
  public:
    explicit SettingsStore(QString directory = {});
    QJsonObject loadJson(const QString &name, QString *warning = nullptr) const;
    bool saveJson(const QString &name, const QJsonObject &object, QString *error = nullptr) const;
    bool hasFile(const QString &name) const;
    QString legacyDirectory() const;
    int loadAffinity(QString *warning = nullptr) const;
    bool saveAffinity(int value, QString *error = nullptr) const;
    int loadLimit(QString *warning = nullptr) const;
    bool saveLimit(int value, QString *error = nullptr) const;
    QString networkDeviceId(QString *error = nullptr) const;
    void log(const QString &message) const;
    QString directory() const { return directory_; }

  private:
    QString directory_;
    bool portableDefault_ = false;
};
} // namespace pettime
