#include "UsagePage.h"

#include "Theme.h"
#include "core/Contacts.h"
#include "core/UsageEngine.h"
#include "core/UsageLog.h"

#include <QApplication>
#include <QComboBox>
#include <QDate>
#include <QEvent>
#include <QFrame>
#include <QPointer>
#include <QThreadPool>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QTimeZone>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <utility>

namespace {

using UsageEngine::Breakdown;

enum Column { Name, Net, Idle, Connected, Status, ColumnCount };
enum Range { Today, Yesterday, Week, Month, Everything };
constexpr int kKeyRole = Qt::UserRole + 1;

constexpr int kRefreshMs = 30000;

QString clock(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
}

QString duration(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    if (seconds <= 0)
        return QStringLiteral("—");
    if (seconds < 60)
        return UsagePage::tr("%1 sec").arg(seconds);
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return UsagePage::tr("%1 min").arg(minutes);
    return minutes % 60 == 0 ? UsagePage::tr("%1 h").arg(minutes / 60)
                             : UsagePage::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

// A time that is a minimum because a start or end record is missing.
QString minimum(qint64 ms, bool uncertain)
{
    return uncertain && ms > 0 ? UsagePage::tr("At least %1").arg(duration(ms)) : duration(ms);
}

QString dayText(const QDate &day)
{
    if (day == QDate::currentDate())
        return UsagePage::tr("Today");
    if (day == QDate::currentDate().addDays(-1))
        return UsagePage::tr("Yesterday");
    return day.toString(QStringLiteral("dd.MM.yyyy ddd"));
}

QString shortDay(const QDate &day)
{
    return day.toString(QStringLiteral("dd.MM ddd"));
}

// "14:20", "Yesterday 14:20" or "21.11.2026 14:20".
QString lastSeen(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("—");
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    if (at.date() == QDate::currentDate())
        return clock(ms);
    if (at.date() == QDate::currentDate().addDays(-1))
        return UsagePage::tr("Yesterday %1").arg(clock(ms));
    return at.toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

QList<QStandardItem *> rowOf(const QStringList &texts)
{
    QList<QStandardItem *> items;
    for (const QString &t : texts)
        items << new QStandardItem(t);
    return items;
}

// The cells for a stretch of time; Name and Status are filled by the caller.
QList<QStandardItem *> timeRow(const QString &name, const UsageEngine::Time &t, bool uncertain)
{
    const QList<QStandardItem *> row = rowOf({name, minimum(t.netMs(), uncertain), duration(t.afkMs),
                                              minimum(t.totalMs(), uncertain), {}});
    row.at(Idle)->setForeground(Theme::kLabelSecondary);
    row.at(Connected)->setForeground(Theme::kLabelSecondary);
    return row;
}

QString html(const QStringList &lines)
{
    QStringList escaped;
    for (const QString &line : lines)
        escaped << line.toHtmlEscaped();
    return QStringLiteral("<qt>%1</qt>").arg(escaped.join(QStringLiteral("<br>")));
}

// The person behind a visit: the normalized nickname from Contacts ("Emre", "Emre-2", "Emre (3)" all
// merge into "Emre"), else a computer whose name matches a Contact person, else the computer name, else IP.
UsageEngine::Who whoOf(const UsageEngine::Visit &v)
{
    const Contact *c = Contacts::instance().match(v.client, v.user);
    if (c && !c->name.trimmed().isEmpty()) {
        const QString person = UsageEngine::personNameOf(c->name);
        return {QStringLiteral("N|") + person.toLower(), person};
    }
    if (!v.client.isEmpty()) {
        const QString clientBase = UsageEngine::personNameOf(v.client);
        for (const QString &name : Contacts::instance().names()) {
            const QString person = UsageEngine::personNameOf(name);
            if (person.compare(v.client, Qt::CaseInsensitive) == 0
                || person.compare(clientBase, Qt::CaseInsensitive) == 0)
                return {QStringLiteral("N|") + person.toLower(), person};
        }
        return {QStringLiteral("C|") + v.client.toLower(), v.client};
    }
    if (!v.address.isEmpty())
        return {QStringLiteral("A|") + v.address, v.address};
    if (!v.user.isEmpty())
        return {QStringLiteral("U|") + v.user.toLower(), v.user};
    return {QStringLiteral("?"), UsagePage::tr("Unknown computer")};
}

// Shows the server-side computer name(s) next to a local nickname so different RConnect PCs with
// different nicknames still transparently show the same underlying server identity.
QString personDisplayName(const UsageEngine::Person &p)
{
    if (p.computers.isEmpty())
        return p.name;
    if (p.computers.size() == 1 && p.computers.first().compare(p.name, Qt::CaseInsensitive) == 0)
        return p.name;
    if (p.computers.size() <= 2)
        return QStringLiteral("%1 (%2)").arg(p.name, p.computers.join(QStringLiteral(", ")));
    return UsagePage::tr("%1 (%2 computers)").arg(p.name).arg(p.computers.size());
}

} // namespace

UsagePage::UsagePage(QWidget *parent)
    : QWidget(parent)
{
    m_model = new QStandardItemModel(0, ColumnCount, this);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    m_subtitle->setWordWrap(true);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);

    m_range = new QComboBox(this);
    m_range->setMinimumHeight(32);
    connect(m_range, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    m_breakdown = new QComboBox(this);
    m_breakdown->setMinimumHeight(32);
    connect(m_breakdown, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    m_refresh = new QPushButton(this);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        emit syncRequested({});
        reload();
    });
    m_configure = new QPushButton(this);
    connect(m_configure, &QPushButton::clicked, this, [this] { emit configureServersRequested(); });
    m_rebuild = new QPushButton(this);
    connect(m_rebuild, &QPushButton::clicked, this, [this] { rebuild(); });
    m_help = new QPushButton(this);
    connect(m_help, &QPushButton::clicked, this, [this] { explain(); });

    auto *head = new QHBoxLayout;
    head->addWidget(m_range, 0, Qt::AlignTop);
    head->addWidget(m_breakdown, 0, Qt::AlignTop);
    head->addWidget(m_refresh, 0, Qt::AlignTop);
    head->addStretch(1);
    head->addWidget(m_configure, 0, Qt::AlignTop);
    head->addWidget(m_rebuild, 0, Qt::AlignTop);
    head->addWidget(m_help, 0, Qt::AlignTop);

    auto *metrics = new QHBoxLayout;
    for (int i = 0; i < 4; ++i) {
        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("detailCard"));
        auto *metric = new QVBoxLayout(card);
        metric->setContentsMargins(12, 8, 12, 9);
        metric->setSpacing(1);
        m_metricLabels[i] = new QLabel(card);
        m_metricLabels[i]->setObjectName(QStringLiteral("fieldHint"));
        m_metricLabels[i]->setWordWrap(true);
        m_metricValues[i] = new QLabel(QStringLiteral("—"), card);
        m_metricValues[i]->setObjectName(QStringLiteral("brandName"));
        metric->addWidget(m_metricLabels[i]);
        metric->addWidget(m_metricValues[i]);
        metrics->addWidget(card, 1);
    }

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::PlainText);
    m_warning->setObjectName(QStringLiteral("warningText"));
    m_warning->hide();

    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setUniformRowHeights(true);
    m_view->setIndentation(22);
    m_view->setStyleSheet(QStringLiteral("QTreeView::item { padding: 7px 4px; }"));
    m_view->setAllColumnsShowFocus(true);
    m_view->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setHighlightSections(false);
    m_view->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int widths[ColumnCount] = {240, 130, 100, 130, 130};
    for (int c = 0; c < ColumnCount; ++c)
        m_view->setColumnWidth(c, widths[c]);
    m_view->header()->setSectionResizeMode(Name, QHeaderView::Stretch);

    m_total = new QLabel(this);
    m_total->setObjectName(QStringLiteral("pageSubtitle"));
    m_total->setWordWrap(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(12);
    lay->addLayout(titles);
    lay->addLayout(head);
    lay->addLayout(metrics);
    lay->addWidget(m_warning);
    lay->addWidget(m_view, 1);
    lay->addWidget(m_total);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &UsagePage::reload);
    connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] { if (isVisible()) reload(); });
    connect(&Contacts::instance(), &Contacts::changed, this, [this] { if (isVisible()) reload(); });
    retranslate();
}

void UsagePage::retranslate()
{
    m_title->setText(tr("Time on servers"));
    m_subtitle->setText(tr("Server connection time across all servers. Open a row to see each server or day and every connection."));
    m_metricLabels[0]->setText(tr("Users"));
    m_metricLabels[1]->setText(tr("Active"));
    m_metricLabels[2]->setText(tr("Idle"));
    m_metricLabels[3]->setText(tr("Connected"));
    m_refresh->setText(tr("Refresh"));
    m_configure->setText(tr("Configure servers"));
    m_configure->setToolTip(tr("Enables built-in Windows client-name logging (4778/4779) and optional RDP idle disconnect on remote servers."));
    m_rebuild->setText(tr("Rebuild totals"));
    m_rebuild->setToolTip(tr("Rebuilds the report from the Windows session events already saved on this PC."));
    m_help->setText(tr("How to read this"));

    const int keep = m_range->currentIndex();
    m_range->blockSignals(true);
    m_range->clear();
    m_range->addItems({tr("Today"), tr("Yesterday"), tr("Last 7 days"), tr("Last 30 days"), tr("All time")});
    m_range->setCurrentIndex(keep < 0 ? Today : keep);
    m_range->blockSignals(false);

    const int keepBreakdown = m_breakdown->currentIndex();
    m_breakdown->blockSignals(true);
    m_breakdown->clear();
    m_breakdown->addItems({tr("By server"), tr("By day")});
    m_breakdown->setCurrentIndex(keepBreakdown < 0 ? 0 : keepBreakdown);
    m_breakdown->setToolTip(tr("How each row is grouped when expanded."));
    m_breakdown->blockSignals(false);

    m_model->setHorizontalHeaderLabels({tr("User"), tr("Active"), tr("Idle"), tr("Connected"), tr("Last seen")});
    m_model->horizontalHeaderItem(Net)->setToolTip(tr("Server connected time minus detected idle periods. "
                                                      "Each moment counts once, even across several servers at the same time."));
    m_model->horizontalHeaderItem(Idle)->setToolTip(tr("Detected idle time (no keyboard or mouse input for %1 minutes or more).")
                                                        .arg(UsageEngine::kAfkMinutes));
    m_model->horizontalHeaderItem(Connected)->setToolTip(tr("Total connected time from the remote server logs."));
    reload();
}

void UsagePage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    emit syncRequested({});
    reload();
    m_timer->start();
}

void UsagePage::onServerSynced(const QString &, const QString &, bool busy, const QStringList &, const QString &)
{
    if (!busy && isVisible())
        reload();
}

void UsagePage::setConfiguringServers(bool busy)
{
    m_configure->setEnabled(!busy);
    m_configure->setText(busy ? tr("Configuring…") : tr("Configure servers"));
}

void UsagePage::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

void UsagePage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

void UsagePage::rebuild()
{
    m_rebuild->setEnabled(false);
    const QPointer<UsagePage> page(this);
    QThreadPool::globalInstance()->start([page] {
        const int events = UsageLog::rebuildEvents();
        QMetaObject::invokeMethod(qApp, [page, events] {
            if (!page)
                return;
            page->m_rebuild->setEnabled(true);
            page->reload();
            QMessageBox::information(page, tr("Rebuild totals"), events >= 0
                ? tr("%1 saved Windows session events were used.").arg(events)
                : tr("Could not rebuild totals from the saved records."));
        }, Qt::QueuedConnection);
    });
}

void UsagePage::explain()
{
    QMessageBox::information(this, tr("How to read this"),
        tr("<p><b>Server-based calculation.</b> Sign-in, reconnect, disconnect and sign-out times are pulled directly "
           "from the remote Windows servers' session logs. Connections are counted even when RConnect was not running.</p>"
           "<p><b>Active.</b> Server connected time minus detected idle stretches. When no keyboard or mouse input is "
           "detected for %1 minutes or more, that stretch counts as idle.</p>"
           "<p><b>Several servers at once.</b> Overlapping connections are merged so each minute counts once. If one "
           "server is active while another is idle, that minute counts as active. As a result, a row's total can be "
           "less than the sum of its individual servers.</p>"
           "<p><b>Connected.</b> Total connected time recorded by the remote servers. Disconnected time is never counted. "
           "Reconnections within %2 minutes are grouped as one connection, without the minutes in between.</p>"
           "<p><b>Incomplete records.</b> When the server log has no sign-in or sign-out for a connection, it is "
           "counted from or until the moment it was seen, and shown as a minimum (\"At least\").</p>"
           "<p><b>Grouping.</b> Uses the label from Contacts (numbered names like \"Ali\" and \"Ali-2\" are combined), "
           "else the computer name, else the IP address.</p>")
            .arg(UsageEngine::kAfkMinutes).arg(UsageEngine::kMergeGapSec / 60));
}

void UsagePage::reload()
{
    if (!isVisible())
        return;
    if (m_reading) {
        m_readAgain = true;
        return;
    }
    m_reading = true;
    const QPointer<UsagePage> page(this);
    QThreadPool::globalInstance()->start([page] {
        const UsageEngine::Report report = UsageLog::report();
        QMetaObject::invokeMethod(qApp, [page, report] {
            if (!page)
                return;
            page->m_reading = false;
            if (page->isVisible())
                page->showReport(report);
            if (std::exchange(page->m_readAgain, false))
                page->reload();
        }, Qt::QueuedConnection);
    });
}

void UsagePage::showReport(const UsageEngine::Report &report)
{
    const QDate today = QDate::currentDate();
    QDate from = QDate(1970, 1, 1), to = today;
    switch (m_range->currentIndex()) {
    case Today:      from = today; break;
    case Yesterday:  from = to = today.addDays(-1); break;
    case Week:       from = today.addDays(-6); break;
    case Month:      from = today.addDays(-29); break;
    default:         break;
    }
    const bool oneDay = from == to;
    const Breakdown by = m_breakdown->currentIndex() == 1 ? Breakdown::ByDay : Breakdown::ByServer;
    const QList<UsageEngine::Person> people = UsageEngine::people(report, from, to, whoOf, by);

    // Keep what was open and where the list was scrolled to.
    QSet<QString> expanded;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *person = m_model->item(r, 0);
        if (m_view->isExpanded(person->index()))
            expanded.insert(person->data(kKeyRole).toString());
        for (int c = 0; c < person->rowCount(); ++c)
            if (m_view->isExpanded(person->child(c, 0)->index()))
                expanded.insert(person->child(c, 0)->data(kKeyRole).toString());
    }
    const int scroll = m_view->verticalScrollBar()->value();
    const bool firstLoad = m_model->rowCount() == 0;

    m_model->removeRows(0, m_model->rowCount());
    UsageEngine::Time sum;
    bool incomplete = false;
    int connectedNow = 0;
    const QString dash = QStringLiteral("—");

    for (const UsageEngine::Person &p : people) {
        sum.activeMs += p.time.activeMs;
        sum.afkMs += p.time.afkMs;
        sum.unknownMs += p.time.unknownMs;
        incomplete |= p.uncertain;
        connectedNow += p.open ? 1 : 0;

        const QList<QStandardItem *> person = timeRow(personDisplayName(p), p.time, p.uncertain);
        const QString personKey = QStringLiteral("P|") + p.key;
        person.at(Name)->setData(personKey, kKeyRole);
        QFont bold = person.at(Name)->font();
        bold.setBold(true);
        person.at(Name)->setFont(bold);
        person.at(Net)->setFont(bold);
        person.at(Status)->setText(p.open ? tr("Connected now") : lastSeen(p.lastEnd));
        if (p.open)
            person.at(Status)->setForeground(Theme::kGreen);

        QStringList about{p.name};
        if (!p.computers.isEmpty())
            about << tr("Computers: %1").arg(p.computers.join(QStringLiteral(", ")));
        if (!p.addresses.isEmpty())
            about << tr("Addresses: %1").arg(p.addresses.join(QStringLiteral(", ")));
        if (!p.accounts.isEmpty())
            about << tr("Accounts: %1").arg(p.accounts.join(QStringLiteral(", ")));
        about << tr("Servers: %1").arg(p.servers.join(QStringLiteral(", ")));
        if (!oneDay)
            about << tr("Days with time: %1").arg(p.days);
        person.at(Name)->setToolTip(html(about));
        QStringList netAbout{tr("Active: %1").arg(duration(p.time.netMs()))};
        if (p.overlapMs >= 60000)
            netAbout << tr("%1 of overlapping time across simultaneous connections was counted once.")
                            .arg(duration(p.overlapMs));
        person.at(Net)->setToolTip(html(netAbout));

        for (const UsageEngine::Share &s : p.shares) {
            const QString shareName = by == Breakdown::ByServer ? (s.server.isEmpty() ? dash : s.server) : dayText(s.day);
            const QList<QStandardItem *> share = timeRow(shareName, s.time, s.uncertain);
            share.at(Name)->setData(personKey + QStringLiteral("|S|") + (by == Breakdown::ByServer
                                        ? s.serverId : s.day.toString(Qt::ISODate)), kKeyRole);
            share.at(Status)->setText(s.open ? tr("Connected now") : lastSeen(s.lastEnd));
            if (s.open)
                share.at(Status)->setForeground(Theme::kGreen);

            for (const UsageEngine::Entry &e : s.entries) {
                const UsageEngine::Visit &v = report.visits.at(e.visit);
                const UsageEngine::DayVisit &dv = e.slice;
                const QString start = dv.fromPreviousDay ? QStringLiteral("00:00") : clock(dv.start);
                const QString end = dv.open ? tr("now") : dv.intoNextDay ? QStringLiteral("24:00") : clock(dv.end);
                const QString span = QStringLiteral("%1 – %2").arg(start, end);
                const QString where = by == Breakdown::ByServer ? (oneDay ? QString() : shortDay(e.day)) : v.server;
                const UsageEngine::Time t{dv.activeMs, dv.afkMs, dv.unknownMs};
                const QList<QStandardItem *> entry = timeRow(
                    where.isEmpty() ? span : where + QStringLiteral("   ") + span, t, e.uncertain);
                if (dv.open) {
                    entry.at(Status)->setText(tr("Connected now"));
                    entry.at(Status)->setForeground(Theme::kGreen);
                } else if (e.uncertain) {
                    entry.at(Status)->setText(tr("Incomplete record"));
                    entry.at(Status)->setForeground(Theme::kOrange);
                }

                QStringList detail{tr("Server: %1").arg(v.server),
                                   tr("Computer: %1").arg(v.client.isEmpty() ? dash : v.client),
                                   tr("Address: %1").arg(v.address.isEmpty() ? dash : v.address),
                                   tr("Account: %1").arg(v.user.isEmpty() ? dash : v.user)};
                if (!v.startKnown() && !dv.fromPreviousDay)
                    detail << tr("No sign-in record: counted from %1, when it was first seen.").arg(clock(dv.start));
                if (!v.endKnown() && !dv.intoNextDay && !dv.open)
                    detail << tr("No sign-out record: counted until %1, when it was last seen.").arg(clock(dv.end));
                for (const UsageEngine::Part &part : dv.parts) {
                    if (part.kind != UsageEngine::Kind::Afk)
                        continue;
                    detail << tr("Idle %1 – %2 (%3)")
                                  .arg(clock(part.start), clock(part.end), duration(part.end - part.start));
                }
                entry.at(Name)->setToolTip(html(detail));
                share.at(Name)->appendRow(entry);
            }
            person.at(Name)->appendRow(share);
        }
        m_model->appendRow(person);
    }

    // People stay closed unless the user opened them (or there is only one); servers likewise.
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *person = m_model->item(r, 0);
        const QString key = person->data(kKeyRole).toString();
        m_view->setExpanded(person->index(), expanded.contains(key) || (firstLoad && m_model->rowCount() == 1));
        for (int c = 0; c < person->rowCount(); ++c) {
            QStandardItem *share = person->child(c, 0);
            m_view->setExpanded(share->index(), expanded.contains(share->data(kKeyRole).toString()));
        }
    }
    m_view->verticalScrollBar()->setValue(scroll);

    const bool empty = people.isEmpty();
    m_metricValues[0]->setText(empty ? dash : connectedNow > 0 ? tr("%1 (%2 connected)").arg(people.size()).arg(connectedNow)
                                                                : QString::number(people.size()));
    m_metricValues[1]->setText(empty ? dash : minimum(sum.netMs(), incomplete));
    m_metricValues[2]->setText(empty ? dash : duration(sum.afkMs));
    m_metricValues[3]->setText(empty ? dash : minimum(sum.totalMs(), incomplete));

    QStringList warnings;
    if (incomplete)
        warnings << tr("Some connections are missing a start or end record. Their totals are minimums.");
    m_warning->setText(warnings.join(QLatin1Char(' ')));
    m_warning->setVisible(!warnings.isEmpty());
    m_total->setText(empty ? tr("No connections recorded for this period.")
        : tr("Calculated from remote server logs. Overlapping connections are counted once. Times use this PC’s clock."));
}
