#include "AccessLogPage.h"

#include "Theme.h"

#include "core/AccessLog.h"
#include "core/GeoIp.h"
#include "core/Threats.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFrame>
#include <QPointer>
#include <QThreadPool>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

enum Column { Time, Server, Event, User, Address, Computer, Details, By, ColumnCount };
enum Category { All, Logons, Failed, Leaves, Mine, Alerts, Changes };

constexpr int kEventRole = Qt::UserRole + 1;
constexpr int kRefreshMs = 15000;
constexpr int kMaxRows = 20000; // the newest are shown; the file has all

bool inCategory(const QString &event, int category)
{
    switch (category) {
    case Logons:
        return event == QLatin1String("Logon") || event == QLatin1String("Authenticated")
            || event == QLatin1String("SecurityLogon") || event == QLatin1String("Reconnect")
            || event == QLatin1String("SecurityReconnect");
    case Failed:
        return event == QLatin1String("FailedLogon");
    case Leaves:
        return event == QLatin1String("Logoff") || event == QLatin1String("Disconnect")
            || event == QLatin1String("SecurityDisconnect");
    case Mine:
        return event == QLatin1String("Connect") || event == QLatin1String("Monitor")
            || event == QLatin1String("AuditEnable") || event == QLatin1String("Message");
    case Alerts:
        return event.startsWith(QLatin1String("Alert")) || Threats::isTampering(event);
    case Changes:
        return event == QLatin1String("UserCreated") || event == QLatin1String("GroupMemberAdded")
            || event == QLatin1String("TaskCreated") || event == QLatin1String("ServiceInstalled")
            || event == QLatin1String("LogCleared");
    default:
        return true;
    }
}

// The details of a logon as the server's log gave them ("logon type 3: wrong password"), in plain words.
QString detailText(const QString &detail)
{
    static const QRegularExpression format(QStringLiteral("^logon type (\\d+)(?:: (.+))?$"));
    const QRegularExpressionMatch m = format.match(detail);
    if (!m.hasMatch())
        return detail;
    const auto t = [](const char *text) { return QCoreApplication::translate("AccessLogPage", text); };
    static const QHash<QString, const char *> reasons = {
        {QStringLiteral("unknown user name"), QT_TRANSLATE_NOOP("AccessLogPage", "Unknown user name")},
        {QStringLiteral("wrong password"), QT_TRANSLATE_NOOP("AccessLogPage", "Wrong password")},
        {QStringLiteral("logon failed"), QT_TRANSLATE_NOOP("AccessLogPage", "Sign-in failed")},
        {QStringLiteral("account restriction"), QT_TRANSLATE_NOOP("AccessLogPage", "Account restriction")},
        {QStringLiteral("outside allowed hours"), QT_TRANSLATE_NOOP("AccessLogPage", "Outside allowed hours")},
        {QStringLiteral("workstation restriction"), QT_TRANSLATE_NOOP("AccessLogPage", "Not allowed from this device")},
        {QStringLiteral("password expired"), QT_TRANSLATE_NOOP("AccessLogPage", "Password expired")},
        {QStringLiteral("account disabled"), QT_TRANSLATE_NOOP("AccessLogPage", "Account disabled")},
        {QStringLiteral("clocks out of sync"), QT_TRANSLATE_NOOP("AccessLogPage", "Clocks out of sync")},
        {QStringLiteral("logon type not allowed"), QT_TRANSLATE_NOOP("AccessLogPage", "Sign-in type not allowed")},
        {QStringLiteral("account expired"), QT_TRANSLATE_NOOP("AccessLogPage", "Account expired")},
        {QStringLiteral("account locked out"), QT_TRANSLATE_NOOP("AccessLogPage", "Account locked")},
    };
    const int type = m.captured(1).toInt();
    const QString how = type == 10 ? t("Remote Desktop") : type == 3 ? t("Network") : QString();
    const QString reason = m.captured(2);
    if (reason.isEmpty())
        return how;
    const QString why = reasons.contains(reason) ? t(reasons.value(reason)) : reason;
    return how.isEmpty() ? why : why + QStringLiteral(" (") + how + QLatin1Char(')');
}

bool looksLikeIp(const QString &text)
{
    static const QRegularExpression ip(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$|^[0-9a-fA-F:.]*:[0-9a-fA-F:.]*$"));
    return ip.match(text).hasMatch();
}

QString csvField(QString text)
{
    // Names come from the network: keep a spreadsheet from running them as a formula.
    if (!text.isEmpty() && QStringLiteral("=+-@\t").contains(text.at(0)))
        text.prepend(QLatin1Char('\''));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + text + QLatin1Char('"');
}

} // namespace

// Filters by category and by text in any column.
class AccessProxy : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setCategory(int category)
    {
        beginFilterChange();
        m_category = category;
        endFilterChange();
    }

    void setText(const QString &text)
    {
        beginFilterChange();
        m_text = text.trimmed();
        endFilterChange();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QAbstractItemModel *m = sourceModel();
        if (!inCategory(m->index(row, Event, parent).data(kEventRole).toString(), m_category))
            return false;
        if (m_text.isEmpty())
            return true;
        for (int c = 0; c < ColumnCount; ++c)
            if (m->index(row, c, parent).data().toString().contains(m_text, Qt::CaseInsensitive))
                return true;
        return false;
    }

private:
    int m_category = All;
    QString m_text;
};

AccessLogPage::AccessLogPage(QWidget *parent)
    : QWidget(parent)
{
    m_model = new QStandardItemModel(0, ColumnCount, this);
    m_proxy = new AccessProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);

    m_refresh = new QPushButton(this);
    connect(m_refresh, &QPushButton::clicked, this, &AccessLogPage::reload);
    m_export = new QPushButton(this);
    connect(m_export, &QPushButton::clicked, this, &AccessLogPage::exportCsv);
    m_open = new QPushButton(this);
    connect(m_open, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(AccessLog::path())); });

    auto *head = new QHBoxLayout;
    head->addLayout(titles, 1);
    head->addWidget(m_refresh, 0, Qt::AlignTop);
    head->addWidget(m_export, 0, Qt::AlignTop);
    head->addWidget(m_open, 0, Qt::AlignTop);

    m_category = new QComboBox(this);
    m_category->setMinimumWidth(210);
    connect(m_category, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_proxy->setCategory(i);
        updateCount();
    });
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_proxy->setText(text);
        updateCount();
    });
    auto *filters = new QHBoxLayout;
    filters->setSpacing(10);
    filters->addWidget(m_category);
    filters->addWidget(m_filter, 1);

    auto *view = m_view = new QTableView(this);
    view->setModel(m_proxy);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSortingEnabled(true);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(30);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setSectionResizeMode(Details, QHeaderView::Stretch);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    view->setColumnWidth(Time, 150);
    view->setColumnWidth(Server, 150);
    view->setColumnWidth(Event, 200);
    view->setColumnWidth(User, 170);
    view->setColumnWidth(Address, 120);
    view->setColumnWidth(Computer, 130);
    view->setColumnWidth(By, 160);
    view->sortByColumn(Time, Qt::DescendingOrder);

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("pageSubtitle"));

    m_detail = new QFrame(this);
    m_detail->setObjectName(QStringLiteral("detailCard"));
    m_detail->hide();
    auto *detailLay = new QVBoxLayout(m_detail);
    detailLay->setContentsMargins(16, 12, 16, 12);
    m_detailText = new QLabel(m_detail);
    m_detailText->setTextFormat(Qt::RichText);
    m_detailText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLay->addWidget(m_detailText);
    connect(view->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { showDetail(); });

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(14);
    lay->addLayout(head);
    lay->addLayout(filters);
    lay->addWidget(view, 1);
    lay->addWidget(m_detail);
    lay->addWidget(m_count);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &AccessLogPage::reload);

    retranslate();
}

QString AccessLogPage::sourceText(const QString &source)
{
    if (source == QLatin1String("LSM"))
        return tr("Remote Desktop session log");
    if (source == QLatin1String("RCM"))
        return tr("Remote Desktop connection log");
    if (source == QLatin1String("SEC"))
        return tr("Windows security log");
    if (source == QLatin1String("SYS"))
        return tr("Windows system log");
    return source == QLatin1String("RCONNECT") ? QStringLiteral("RConnect") : source;
}

QString AccessLogPage::eventText(const QString &event)
{
    const auto is = [&event](const char *name) { return event == QLatin1String(name); };
    // The security log repeats what the Remote Desktop logs say, so both read the same.
    if (is("Logon") || is("SecurityLogon"))             return tr("Signed in");
    if (is("Logoff"))                                   return tr("Signed out");
    if (is("Disconnect") || is("SecurityDisconnect"))   return tr("Disconnected");
    if (is("Reconnect") || is("SecurityReconnect"))     return tr("Reconnected");
    if (is("Authenticated"))                            return tr("Connection accepted");
    if (is("FailedLogon"))                              return tr("Failed sign-in");
    if (is("Connect"))            return tr("Connected with RConnect");
    if (is("Monitor"))            return tr("Screen viewed in RConnect");
    if (is("AuditEnable"))        return tr("Sign-in tracking turned on");
    if (is("Message"))            return tr("Message sent in RConnect");
    if (is("UserCreated"))        return tr("User account created");
    if (is("GroupMemberAdded"))   return tr("Added to a privileged group");
    if (is("TaskCreated"))        return tr("Scheduled task created");
    if (is("ServiceInstalled"))   return tr("Service installed");
    if (is("LogCleared"))         return tr("Event log cleared");
    if (is("AlertBruteForce"))    return tr("Alert: password guessing");
    if (is("AlertBreakIn"))       return tr("Alert: sign-in after failed attempts");
    if (is("AlertNewCountry"))    return tr("Alert: sign-in from a new country");
    if (is("AlertSuspicious"))    return tr("Alert: suspicious new program");
    return event;
}

// What a warning or a tampering event says, in a sentence. Empty for the other events.
QString AccessLogPage::alertSummary(const AccessRecord &r)
{
    const QString &e = r.event;
    const QStringList parts = r.detail.split(QLatin1Char('|'));
    const QString first = parts.value(0), second = parts.value(1);
    if (e == QLatin1String("AlertBruteForce"))
        return tr("%1 failed sign-ins from %2, %3 accounts tried").arg(first, r.address, second);
    if (e == QLatin1String("AlertBreakIn"))
        return tr("%1 signed in from %2 right after %3 failed attempts").arg(r.user, r.address, first);
    if (e == QLatin1String("AlertNewCountry"))
        return tr("%1 signed in from %2 (%3) for the first time").arg(r.user, r.address, GeoIp::countryName(first));
    if (e == QLatin1String("AlertSuspicious"))
        return (first == QLatin1String("TaskCreated") ? tr("New scheduled task: %1") : tr("New service: %1"))
            .arg(r.detail.mid(first.size() + 1));
    if (e == QLatin1String("UserCreated"))
        return tr("User account %1 created by %2").arg(r.user, r.detail);
    if (e == QLatin1String("GroupMemberAdded"))
        return tr("%1 added to %2 by %3").arg(r.user, first, second);
    if (e == QLatin1String("LogCleared"))
        return tr("%1 log cleared by %2").arg(r.detail, r.user);
    if (e == QLatin1String("TaskCreated"))
        return tr("New scheduled task %1 by %2").arg(r.detail, r.user);
    if (e == QLatin1String("ServiceInstalled"))
        return tr("New service %1").arg(r.detail);
    return {};
}

void AccessLogPage::retranslate()
{
    m_title->setText(tr("Activity history"));
    m_subtitle->setText(tr("Sign-ins, failed attempts, disconnections, security alerts and what was done in RConnect."));
    m_refresh->setText(tr("Refresh"));
    m_export->setText(tr("Export…"));
    m_open->setText(tr("Open file"));
    m_filter->setPlaceholderText(tr("Search by server, user, IP address or device"));
    const int current = m_category->currentIndex();
    m_category->blockSignals(true);
    m_category->clear();
    m_category->addItems({tr("All activity"), tr("Sign-ins"), tr("Failed sign-ins"), tr("Sign-outs and disconnections"),
                          tr("Done in RConnect"), tr("Security alerts"), tr("Changes on the server")});
    m_category->setCurrentIndex(qMax(0, current));
    m_category->blockSignals(false);
    m_model->setHorizontalHeaderLabels({tr("Time"), tr("Server"), tr("Activity"), tr("User"), tr("IP address"),
                                        tr("Device"), tr("Details"), tr("By")});
    if (isVisible())
        reload();
}

void AccessLogPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    reload();
    m_timer->start();
}

void AccessLogPage::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

void AccessLogPage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

namespace {

constexpr qint64 kTailBytes = 24 * 1024 * 1024; // how much of the end of the file is read
constexpr int kMaxRaw = 60000;                   // entries kept before grouping
constexpr int kRunSeconds = 600;                  // failed attempts this close together are one run

QDateTime parseUtc(const QString &timeUtc)
{
    return QDateTime::fromString(timeUtc.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
}

QString shortName(const QString &user)
{
    return user.mid(user.lastIndexOf(QLatin1Char('\\')) + 1);
}

// "Connection accepted" only repeats the sign-in line that follows it. Failed attempts from one
// address close to each other (a password guessing run) are one line.
QList<AccessLine> buildLines(QList<AccessRecord> records)
{
    if (records.size() > kMaxRaw)
        records = records.mid(records.size() - kMaxRaw);
    // The same address is the same computer: a line that has the name tells it to the others.
    QHash<QString, QString> clientByAddress;
    for (const AccessRecord &r : std::as_const(records))
        if (!r.client.isEmpty() && !r.address.isEmpty() && r.source != QLatin1String("RCONNECT"))
            clientByAddress.insert(r.address, r.client);
    for (AccessRecord &r : records)
        if (r.client.isEmpty() && !r.address.isEmpty())
            r.client = clientByAddress.value(r.address);

    QList<AccessLine> lines;
    QHash<QString, int> growing; // server and address -> the line still taking attempts
    for (const AccessRecord &r : std::as_const(records)) {
        if (r.event == QLatin1String("Authenticated"))
            continue;
        if (r.event == QLatin1String("FailedLogon") && !r.address.isEmpty()) {
            const QString key = r.serverId + QLatin1Char('|') + r.address;
            const auto it = growing.constFind(key);
            if (it != growing.cend()) {
                AccessLine &line = lines[it.value()];
                const QDateTime last = parseUtc(line.r.timeUtc), now = parseUtc(r.timeUtc);
                if (last.isValid() && now.isValid() && qAbs(last.secsTo(now)) <= kRunSeconds) {
                    ++line.attempts;
                    if (r.timeUtc > line.r.timeUtc)
                        line.r.timeUtc = r.timeUtc;
                    if (r.timeUtc < line.firstUtc)
                        line.firstUtc = r.timeUtc;
                    if (!line.users.contains(shortName(r.user)))
                        line.users << shortName(r.user);
                    continue;
                }
            }
            growing.insert(key, int(lines.size()));
        }
        lines.append({r, r.timeUtc, 1, {shortName(r.user)}});
    }
    return lines;
}

constexpr int kLineRole = Qt::UserRole + 2;

} // namespace

void AccessLogPage::reload()
{
    // Only when the file changed: the page also refreshes on a timer.
    const QFileInfo info(AccessLog::path());
    const QPair<qint64, qint64> stamp(info.size(), info.lastModified().toMSecsSinceEpoch());
    if (m_loaded && stamp == m_stamp)
        return;
    if (m_busy) {
        m_again = true;
        return;
    }
    m_stamp = stamp;
    m_loaded = true;
    m_busy = true;

    // The file can be large with many servers: it is read and grouped off the GUI thread.
    QPointer<AccessLogPage> self(this);
    QThreadPool::globalInstance()->start([self] {
        QList<AccessLine> lines = buildLines(AccessLog::readRecent(kTailBytes));
        QMetaObject::invokeMethod(qApp, [self, lines = std::move(lines)]() mutable {
            if (self)
                self->apply(std::move(lines));
        }, Qt::QueuedConnection);
    });
}

void AccessLogPage::apply(QList<AccessLine> lines)
{
    m_busy = false;
    m_lines = std::move(lines);

    // Filling a sorted, filtered model cell by cell would sort again after every cell, so it is
    // detached meanwhile. That resets the header, so its sizes and sort order are put back.
    QHeaderView *header = m_view->horizontalHeader();
    const QByteArray headerState = header->saveState();
    m_view->setUpdatesEnabled(false);
    m_proxy->setSourceModel(nullptr);
    m_model->removeRows(0, m_model->rowCount());
    m_model->setRowCount(int(m_lines.size()));
    for (int row = 0; row < m_lines.size(); ++row) {
        const AccessLine &line = m_lines.at(row);
        const AccessRecord &r = line.r;
        const QDateTime utc = parseUtc(r.timeUtc);
        const QString time = utc.isValid() ? utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                           : r.timeUtc;
        QStringList details;
        if (line.attempts > 1)
            details << tr("%1 attempts").arg(line.attempts);
        const QString summary = alertSummary(r);
        if (!summary.isEmpty())
            details << summary;
        else if (!r.detail.isEmpty())
            details << detailText(r.detail);
        // Without the computer name in front: the full account is in the details below.
        const QString user = line.users.size() > 1 ? tr("%1 accounts").arg(line.users.size()) : line.users.value(0);
        const QString texts[ColumnCount] = {time, r.server, eventText(r.event), user, r.address, r.client,
                                            details.join(QStringLiteral(", ")), r.operatorName};
        for (int c = 0; c < ColumnCount; ++c) {
            auto *item = new QStandardItem(texts[c]);
            item->setData(c == Time ? r.timeUtc : texts[c], Qt::UserRole); // the time sorts as text
            item->setData(r.event, kEventRole);
            item->setData(row, kLineRole);
            if (c == Event && (r.event == QLatin1String("FailedLogon") || r.event.startsWith(QLatin1String("Alert"))
                               || Threats::isTampering(r.event)))
                item->setForeground(Theme::kRed);
            m_model->setItem(row, c, item);
        }
    }
    m_proxy->setSourceModel(m_model);
    header->restoreState(headerState);
    m_view->setUpdatesEnabled(true);
    updateCount();
    showDetail();
    if (m_again) {
        m_again = false;
        reload();
    }
}

// Everything known about the selected row.
void AccessLogPage::showDetail()
{
    const QModelIndex current = m_view->currentIndex();
    const int row = current.isValid() ? current.sibling(current.row(), Time).data(kLineRole).toInt() : -1;
    if (row < 0 || row >= m_lines.size()) {
        m_detail->hide();
        return;
    }
    const AccessLine &line = m_lines.at(row);
    const AccessRecord &r = line.r;
    const auto local = [](const QString &timeUtc) {
        const QDateTime utc = parseUtc(timeUtc);
        return utc.isValid() ? utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : timeUtc;
    };
    QStringList cells;
    const auto add = [&cells](const QString &label, const QString &value) {
        if (!value.isEmpty())
            cells << QStringLiteral("<td style='color:#98989d;padding:2px 18px 2px 0'>%1</td><td style='padding:2px 28px 2px 0'>%2</td>")
                         .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    add(tr("Time"), line.attempts > 1 ? tr("%1 to %2").arg(local(line.firstUtc), local(r.timeUtc)) : local(r.timeUtc));
    add(tr("Server"), r.host.isEmpty() ? r.server : QStringLiteral("%1 (%2)").arg(r.server, r.host));
    add(tr("Activity"), eventText(r.event));
    add(line.users.size() > 1 ? tr("Accounts tried") : tr("Account"),
        line.users.size() > 1 ? line.users.mid(0, 12).join(QStringLiteral(", ")) + (line.users.size() > 12 ? QStringLiteral(" …") : QString())
                              : r.user);
    QString where = r.address;
    if (looksLikeIp(r.address)) {
        const QString code = GeoIp::countryOf(r.address);
        if (!code.isEmpty())
            where += QStringLiteral(" · ") + GeoIp::countryName(code);
    }
    add(tr("IP address"), where);
    add(tr("Device"), r.client);
    add(tr("Windows session no."), r.sessionId > 0 ? QString::number(r.sessionId) : QString());
    add(tr("Attempts"), line.attempts > 1 ? QString::number(line.attempts) : QString());
    add(tr("How"), alertSummary(r).isEmpty() ? detailText(r.detail) : alertSummary(r));
    add(tr("Done by"), r.operatorName);
    add(tr("Seen in"), sourceText(r.source));
    add(tr("Record no."), r.recordId > 0 ? QString::number(r.recordId) : QString());

    // Two entries per row of the table.
    QString html = QStringLiteral("<table cellspacing='0'>");
    for (int k = 0; k < cells.size(); k += 2)
        html += QStringLiteral("<tr>") + cells.at(k) + (k + 1 < cells.size() ? cells.at(k + 1) : QString()) + QStringLiteral("</tr>");
    m_detailText->setText(html + QStringLiteral("</table>"));
    m_detail->show();
}

void AccessLogPage::updateCount()
{
    m_count->setText(m_model->rowCount() == 0
                         ? tr("Nothing recorded yet. Activity appears here after your servers have been checked.")
                         : tr("Showing %1 of %2").arg(m_proxy->rowCount()).arg(m_model->rowCount()));
}

// The rows shown now, in the order shown, for a report.
void AccessLogPage::exportCsv()
{
    const QString name = QStringLiteral("rconnect-access-log-%1.csv")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), name, tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export"), tr("Could not write the file."));
        return;
    }
    f.write("\xEF\xBB\xBF"); // so Excel reads it as UTF-8
    QStringList header;
    for (int c = 0; c < ColumnCount; ++c)
        header << csvField(m_model->headerData(c, Qt::Horizontal).toString());
    f.write(header.join(QLatin1Char(',')).toUtf8() + "\r\n");
    for (int row = 0; row < m_proxy->rowCount(); ++row) {
        QStringList fields;
        for (int c = 0; c < ColumnCount; ++c)
            fields << csvField(m_proxy->index(row, c).data().toString());
        f.write(fields.join(QLatin1Char(',')).toUtf8() + "\r\n");
    }
}
