#pragma once
#include <QString>
namespace pettime {
class SettingsStore {
  public:
    explicit SettingsStore(QString directory = {});
    int loadAffinity(QString *warning = nullptr) const;
    bool saveAffinity(int value, QString *error = nullptr) const;
    int loadLimit(QString *warning = nullptr) const;
    bool saveLimit(int value, QString *error = nullptr) const;
    QString networkDeviceId(QString *error = nullptr) const;
    void log(const QString &message) const;
    QString directory() const { return directory_; }

  private:
    QString directory_;
};
} // namespace pettime
