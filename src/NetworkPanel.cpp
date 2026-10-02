#include "NetworkPanel.h"
#include <QHeaderView>
#include <QHash>
#include <QMetaMethod>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QVBoxLayout>
#include <algorithm>
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
    table_ = new QTableWidget(0, 13, this);
    table_->setObjectName("networkDevices");
    table_->setHorizontalHeaderLabels({"设备名", "IP", "端口", "数量", "上限", "连接状态",
                                       "最后通讯", "派出", "访客 / 144", "派遣能力", "版本", "协议", "源码指纹"});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAutoScroll(false);
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
        if (closing_)
            return;
        if (enabled)
            service_.start();
        else {
            const bool handled = isSignalConnected(QMetaMethod::fromSignal(&NetworkPanel::stopRequested));
            emit stopRequested();
            if (!handled)
                service_.stop();
        }
    });
    connect(refresh_, &QPushButton::clicked, &service_, &NetworkService::refresh);
    connect(&service_, &NetworkService::changed, this, [this] {
        if (isVisible())
            updateView();
    });
    updateView();
}
void NetworkPanel::setClosing(bool closing) {
    if (closing_ == closing)
        return;
    closing_ = closing;
    updateView();
}
void NetworkPanel::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    updateView();
}
void NetworkPanel::updateView() {
    const QSignalBlocker blocker(enabled_);
    enabled_->setChecked(service_.running() && !closing_);
    enabled_->setEnabled(!closing_);
    refresh_->setEnabled(service_.running() && !closing_);
    status_->setText(closing_ ? QStringLiteral("正在召回派出实体，等待访客清理后关闭网络服务…")
                             : service_.status());
    const auto info = service_.localInfo();
    local_->setText(
        QString(
            "本机：%1  |  数量：%2 / %3\nIPv4：%4  |  TCP / UDP：%5\n派出：%6  |  访客：%7 / 144")
            .arg(QSysInfo::machineHostName())
            .arg(info.count)
            .arg(info.limit)
            .arg(service_.addresses().isEmpty() ? "未开启" : service_.addresses().join(", "))
            .arg(service_.port())
            .arg(info.dispatched)
            .arg(info.visitors) +
        "\n本机版本：" + service_.localVersion() + " · 协议：" +
        QString::number(NetworkService::ProtocolVersion) + " · 源码指纹：" + service_.localFingerprint().left(12));
    const int scrollY = table_->verticalScrollBar()->value();
    const int scrollX = table_->horizontalScrollBar()->value();
    QString selected, current;
    const auto selection = table_->selectionModel()->selectedRows();
    if (!selection.isEmpty())
        selected = table_->item(selection.front().row(), 0)->data(Qt::UserRole).toString();
    if (table_->currentRow() >= 0 && table_->item(table_->currentRow(), 0))
        current = table_->item(table_->currentRow(), 0)->data(Qt::UserRole).toString();
    const QSignalBlocker tableBlocker(table_);
    const auto devices = service_.devices();
    QHash<QString, int> rows;
    for (int row = 0; row < table_->rowCount(); ++row)
        if (const auto *item = table_->item(row, 0))
            rows.insert(item->data(Qt::UserRole).toString(), row);
    const bool sameDevices = rows.size() == devices.size() &&
        std::all_of(devices.cbegin(), devices.cend(), [&](const auto &d) { return rows.contains(d.id); });
    if (!sameDevices) {
        table_->setRowCount(devices.size());
        table_->clearSelection();
        table_->setCurrentCell(-1, -1);
    }
    int online = 0;
    for (int index = 0; index < devices.size(); ++index) {
        const auto &d = devices[index];
        const int row = sameDevices ? rows.value(d.id) : index;
        if (d.state.startsWith("已连接"))
            ++online;
        const QStringList values{d.name.isEmpty() ? "待握手" : d.name,
                                 d.address,
                                 QString::number(d.port),
                                 d.count ? QString::number(d.count) : "—",
                                 d.limit ? QString::number(d.limit) : "—",
                                 d.state,
                                 d.lastContact.isValid() ? d.lastContact.toString("HH:mm:ss") : "—",
                                 QString::number(d.dispatched),
                                 QString::number(d.visitors),
                                 d.dispatch ? "支持" : "不支持或未握手",
                                 d.appVersion.isEmpty() ? "未知" : d.appVersion,
                                 QString::number(d.protocolVersion),
                                 d.fingerprint.isEmpty() ? "未知" : d.fingerprint.left(12)};
        for (int col = 0; col < values.size(); ++col) {
            auto *item = table_->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                table_->setItem(row, col, item);
            }
            if (item->text() != values[col])
                item->setText(values[col]);
            if (col == 12 && item->toolTip() != d.fingerprint)
                item->setToolTip(d.fingerprint);
            if (item->data(Qt::UserRole).toString() != d.id)
                item->setData(Qt::UserRole, d.id);
            const auto foreground = d.state.startsWith("已连接")
                ? palette().brush(QPalette::Text)
                : palette().brush(QPalette::Disabled, QPalette::Text);
            if (item->foreground() != foreground)
                item->setForeground(foreground);
        }
        if (!sameDevices && d.id == selected)
            table_->selectionModel()->select(table_->model()->index(row, 0),
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!sameDevices && d.id == current)
            table_->selectionModel()->setCurrentIndex(table_->model()->index(row, 0),
                QItemSelectionModel::NoUpdate);
    }
    table_->verticalScrollBar()->setValue(scrollY);
    table_->horizontalScrollBar()->setValue(scrollX);
    count_->setText(QString("已连接 %1 台，共发现 %2 台").arg(online).arg(devices.size()));
}
} // namespace pettime
