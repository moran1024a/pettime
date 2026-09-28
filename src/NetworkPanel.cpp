#include "NetworkPanel.h"
#include <QHeaderView>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QVBoxLayout>
namespace pettime {
NetworkPanel::NetworkPanel(NetworkService &service, QWidget *parent)
    : QDialog(parent), service_(service) {
    setObjectName("networkPanel");
    setWindowTitle("局域网网络面板");
    resize(820, 420);
    auto *layout = new QVBoxLayout(this);
    enabled_ = new QCheckBox("开启网络服务", this);
    enabled_->setObjectName("networkEnabled");
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    local_ = new QLabel(this);
    local_->setTextFormat(Qt::PlainText);
    local_->setWordWrap(true);
    table_ = new QTableWidget(0, 7, this);
    table_->setObjectName("networkDevices");
    table_->setHorizontalHeaderLabels(
        {"设备名", "IP", "端口", "数量", "上限", "连接状态", "最后通讯"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    refresh_ = new QPushButton("立即刷新", this);
    refresh_->setObjectName("networkRefresh");
    count_ = new QLabel(this);
    auto *note = new QLabel("仅用于可信局域网，资料明文传输。关闭此窗口后网络服务继续运行。\n"
                            "未发现设备时，请检查双方服务、系统防火墙及路由器客户端隔离设置。",
                            this);
    note->setWordWrap(true);
    layout->addWidget(enabled_);
    layout->addWidget(status_);
    layout->addWidget(local_);
    layout->addWidget(table_);
    layout->addWidget(count_);
    layout->addWidget(refresh_);
    layout->addWidget(note);
    connect(enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled)
            service_.start();
        else
            service_.stop();
    });
    connect(refresh_, &QPushButton::clicked, &service_, &NetworkService::refresh);
    connect(&service_, &NetworkService::changed, this, &NetworkPanel::updateView);
    updateView();
}
void NetworkPanel::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    updateView();
}
void NetworkPanel::updateView() {
    const QSignalBlocker blocker(enabled_);
    enabled_->setChecked(service_.running());
    refresh_->setEnabled(service_.running());
    status_->setText(service_.status());
    const auto info = service_.localInfo();
    local_->setText(
        QString("本机：%1  |  数量：%2 / %3\nIPv4：%4  |  TCP / UDP：%5")
            .arg(QSysInfo::machineHostName())
            .arg(info.count)
            .arg(info.limit)
            .arg(service_.addresses().isEmpty() ? "未开启" : service_.addresses().join(", "))
            .arg(service_.port()));
    QString selected;
    if (table_->currentRow() >= 0 && table_->item(table_->currentRow(), 0))
        selected = table_->item(table_->currentRow(), 0)->data(Qt::UserRole).toString();
    const auto devices = service_.devices();
    table_->setRowCount(devices.size());
    int online = 0;
    for (int row = 0; row < devices.size(); ++row) {
        const auto &d = devices[row];
        if (d.state.startsWith("已连接"))
            ++online;
        const QStringList values{d.name.isEmpty() ? "待握手" : d.name,
                                 d.address,
                                 QString::number(d.port),
                                 d.count ? QString::number(d.count) : "—",
                                 d.limit ? QString::number(d.limit) : "—",
                                 d.state,
                                 d.lastContact.isValid() ? d.lastContact.toString("HH:mm:ss")
                                                         : "—"};
        for (int col = 0; col < values.size(); ++col) {
            auto *item = table_->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                table_->setItem(row, col, item);
            }
            item->setText(values[col]);
            item->setData(Qt::UserRole, d.id);
            item->setForeground(d.state.startsWith("已连接")
                                    ? palette().brush(QPalette::Text)
                                    : palette().brush(QPalette::Disabled, QPalette::Text));
        }
        if (d.id == selected)
            table_->selectRow(row);
    }
    count_->setText(QString("已连接 %1 台，共发现 %2 台").arg(online).arg(devices.size()));
}
} // namespace pettime
