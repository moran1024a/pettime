#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <memory>
namespace pettime {
class SingleInstance : public QObject {
    Q_OBJECT
  public:
    enum class Result { Primary, Recalled, Error };
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;
    Result start(const QString &dataDirectory, const QString &scope, QString *error);
  signals:
    void recallRequested();

  private:
    QLocalServer server_;
    std::unique_ptr<QLockFile> lock_;
};
} // namespace pettime
