#pragma once
#include "NetworkService.h"
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
namespace pettime {
class NetworkPanel : public QDialog {
    Q_OBJECT
  public:
    explicit NetworkPanel(NetworkService &service, QWidget *parent = nullptr);
    void setClosing(bool closing);

  signals:
    void stopRequested();

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void updateView();
    NetworkService &service_;
    QCheckBox *enabled_;
    QLabel *status_, *local_, *count_;
    QPushButton *refresh_;
    QTableWidget *table_;
    bool closing_ = false;
};
} // namespace pettime
