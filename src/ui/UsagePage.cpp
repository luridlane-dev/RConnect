#include "UsagePage.h"

#include "Theme.h"
#include "core/UsageEngine.h"
#include "core/UsageLog.h"

#include <QApplication>
#include <QComboBox>
#include <QDate>
#include <QEvent>
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

namespace {

enum Column { Name, Computer, Address, Entries, FirstIn, LastOut, Total, Active, Away, Unknown, Note, ColumnCount };
enum Range { Today, Yesterday, Week, Month, Everything };
constexpr int kKeyRole = Qt::UserRole + 1;

constexpr int kRefreshMs = 30000;

QString clock(qint64 ms, bool seconds = false)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(seconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
}

QString duration(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    if (seconds <= 0)
        return QStringLiteral("—");
    if (seconds < 60)
        return UsagePage::tr("%1 sec").arg(seconds);
    const qint64 minutes = (seconds + 30) / 60;
    if (minutes < 60)
        return UsagePage::tr("%1 min").arg(minutes);
    return UsagePage::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString dayText(const QDate &day)
{
    if (day == QDate::currentDate())
        return UsagePage::tr("Today");
    if (day == QDate::currentDate().addDays(-1))
        return UsagePage::tr("Yesterday");
    return day.toString(QStringLiteral("dd.MM.yyyy ddd"));
}

QList<QStandardItem *> rowOf(const QStringList &texts)
{
    QList<QStandardItem *> items;
    for (const QString &t : texts)
        items << new QStandardItem(t);
    return items;
}

void paint(const QList<QStandardItem *> &items, const QColor &color)
{
    for (QStandardItem *item : items)
        item->setForeground(color);
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
    m_refresh = new QPushButton(this);
    connect(m_refresh, &QPushButton::clicked, this, [this] { reload(); });
    m_rebuild = new QPushButton(this);
    connect(m_rebuild, &QPushButton::clicked, this, [this] { rebuild(); });
    m_help = new QPushButton(this);
    connect(m_help, &QPushButton::clicked, this, [this] { explain(); });

    auto *head = new QHBoxLayout;
    head->addLayout(titles, 1);
    head->addWidget(m_range, 0, Qt::AlignTop);
    head->addWidget(m_help, 0, Qt::AlignTop);
    head->addWidget(m_rebuild, 0, Qt::AlignTop);
    head->addWidget(m_refresh, 0, Qt::AlignTop);

    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::PlainText);
    m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::kOrange.name()));
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
    m_view->header()->setStretchLastSection(true);
    m_view->header()->setHighlightSections(false);
    m_view->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int widths[ColumnCount] = {230, 160, 140, 80, 150, 140, 140, 110, 120, 110, 300};
    for (int c = 0; c < ColumnCount; ++c)
        m_view->setColumnWidth(c, widths[c]);

    m_total = new QLabel(this);
    m_total->setObjectName(QStringLiteral("pageSubtitle"));
    m_total->setWordWrap(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(12);
    lay->addLayout(head);
    lay->addWidget(m_warning);
    lay->addWidget(m_view, 1);
    lay->addWidget(m_total);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &UsagePage::reload);
    retranslate();
}

void UsagePage::retranslate()
{
    m_title->setText(tr("Time on servers"));
    m_subtitle->setText(tr("Who came in from which computer, when they came in and left, and how long they really "
                           "worked. Times come from the servers' own logs and are shown in this PC's time zone (%1). "
                           "Time away from the computer (no mouse or keyboard for %2 minutes or more) is counted as "
                           "away, not as active.")
                            .arg(QString::fromLatin1(QTimeZone::systemTimeZoneId()))
                            .arg(UsageEngine::kAfkMinutes));
    m_refresh->setText(tr("Refresh"));
    m_rebuild->setText(tr("Recalculate from log"));
    m_rebuild->setToolTip(tr("Reads the sign-in and sign-out events from the access log again and recalculates every day."));
    m_help->setText(tr("How is this calculated?"));
    const int keep = m_range->currentIndex();
    m_range->blockSignals(true);
    m_range->clear();
    m_range->addItems({tr("Today"), tr("Yesterday"), tr("Last 7 days"), tr("Last 30 days"), tr("All time")});
    m_range->setCurrentIndex(keep < 0 ? Today : keep);
    m_range->blockSignals(false);
    m_model->setHorizontalHeaderLabels({tr("Server"), tr("Computer"), tr("IP address"), tr("Visits"),
                                        tr("First in"), tr("Last out"), tr("Time on server"), tr("Active time"),
                                        tr("Away time (AFK)"), tr("Unknown"), tr("Note")});
    reload();
}

void UsagePage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    reload();
    m_timer->start();
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
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int events = UsageLog::rebuildEvents();
    QApplication::restoreOverrideCursor();
    reload();
    QMessageBox::information(this, tr("Recalculate from log"),
                             tr("Done. %1 sign-in, sign-out, disconnect and reconnect events were read from the access "
                                "log and every day was recalculated.")
                                 .arg(events));
}

void UsagePage::explain()
{
    QMessageBox box(QMessageBox::Information, tr("How is this calculated?"), QString(), QMessageBox::Ok, this);
    box.setTextFormat(Qt::RichText);
    box.setText(
        tr("<p><b>Where the times come from.</b> From the server's own session log (Windows events: signed in, "
           "disconnected, reconnected, signed out). Those times are exact. All times are saved in UTC and shown in "
           "this PC's time zone; a day ends at this PC's midnight.</p>"
           "<p><b>Visit.</b> From connecting to disconnecting. Time while disconnected is never counted. If the "
           "same computer (same IP address) comes back within %1 minutes, it is one visit and the short break "
           "is not counted.</p>"
           "<p><b>Away (AFK).</b> RConnect asks every server about every 2 minutes how long the person has not "
           "touched the mouse or keyboard. If that is %2 minutes or more, the whole stretch is away, from the "
           "last touch on, and it is not active time.</p>"
           "<p><b>Unknown.</b> If RConnect was closed or could not reach the server, nobody measured. That time "
           "is shown as unknown and is never guessed.</p>"
           "<p><b>Accuracy.</b> Entering and leaving: exact to the second. Away time: the server reports whole "
           "minutes and RConnect asks about every 2 minutes, so each away stretch can be off by up to 2 minutes "
           "(usually too short rather than too long). Computer names come from the server at the time of the "
           "visit; if no measurement was made during a visit, only the IP address is known.</p>")
            .arg(UsageEngine::kMergeGapSec / 60)
            .arg(UsageEngine::kAfkMinutes));
    box.exec();
}

void UsagePage::reload()
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

    const UsageEngine::Report report = UsageLog::report();

    // Keep what was open and where the list was scrolled to.
    QSet<QString> expanded, seen;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *day = m_model->item(r, 0);
        seen.insert(day->data(kKeyRole).toString());
        if (m_view->isExpanded(day->index()))
            expanded.insert(day->data(kKeyRole).toString());
        for (int c = 0; c < day->rowCount(); ++c)
            if (m_view->isExpanded(day->child(c, 0)->index()))
                expanded.insert(day->child(c, 0)->data(kKeyRole).toString());
    }
    const int scroll = m_view->verticalScrollBar()->value();
    const bool firstLoad = m_model->rowCount() == 0;

    m_model->removeRows(0, m_model->rowCount());
    qint64 totalMs = 0, activeMs = 0, afkMs = 0, unknownMs = 0;
    int groups = 0;
    const QString dash = QStringLiteral("—");
    QStandardItem *dayItem = nullptr;
    QList<QStandardItem *> dayRow;
    QDate currentDay;
    qint64 dayTotal = 0, dayActive = 0, dayAfk = 0, dayUnknown = 0;
    int dayEntries = 0;
    auto finishDay = [&] {
        if (!dayItem)
            return;
        dayRow.at(Entries)->setText(QString::number(dayEntries));
        dayRow.at(Total)->setText(duration(dayTotal));
        dayRow.at(Active)->setText(duration(dayActive));
        dayRow.at(Away)->setText(duration(dayAfk));
        dayRow.at(Unknown)->setText(dayUnknown > 0 ? duration(dayUnknown) : dash);
    };

    for (const UsageEngine::DayGroup &g : report.days) {
        if (g.day < from || g.day > to)
            continue;
        ++groups;
        if (!dayItem || g.day != currentDay) {
            finishDay();
            currentDay = g.day;
            dayTotal = dayActive = dayAfk = dayUnknown = 0;
            dayEntries = 0;
            dayRow = rowOf(QStringList(int(ColumnCount), QString()));
            dayRow.at(Name)->setText(dayText(g.day));
            dayRow.at(Name)->setData(QStringLiteral("D|") + g.day.toString(Qt::ISODate), kKeyRole);
            QFont bold = dayRow.at(Name)->font();
            bold.setBold(true);
            bold.setPointSizeF(bold.pointSizeF() + 1);
            for (QStandardItem *item : dayRow) {
                item->setFont(bold);
                item->setBackground(QColor(255, 255, 255, 22));
            }
            dayItem = dayRow.at(Name);
            m_model->appendRow(dayRow);
        }
        dayTotal += g.totalMs();
        dayActive += g.activeMs;
        dayAfk += g.afkMs;
        dayUnknown += g.unknownMs;
        dayEntries += g.entries;

        QString firstIn = clock(g.firstIn);
        QString lastOut = clock(g.lastOut);
        const UsageEngine::DayVisit *latest = nullptr;
        for (const UsageEngine::DayVisit &dv : g.visits) {
            if (dv.fromPreviousDay && dv.start == g.firstIn)
                firstIn = tr("%1 (from the day before)").arg(clock(g.firstIn));
            if (!latest || dv.end >= latest->end)
                latest = &dv;
        }
        if (g.inside)
            lastOut = tr("Still inside");
        else if (latest && latest->intoNextDay)
            lastOut = tr("Midnight (goes on)");

        QStringList note;
        if (g.uncertain)
            note << tr("Some times are not exact: open the row");
        if (g.unknownMs > 0)
            note << tr("Part of it was not measured");
        const QList<QStandardItem *> parent = rowOf(
            {g.server, g.client.isEmpty() ? dash : g.client, g.address.isEmpty() ? dash : g.address,
             QString::number(g.entries), firstIn, lastOut, duration(g.totalMs()), duration(g.activeMs),
             duration(g.afkMs), g.unknownMs > 0 ? duration(g.unknownMs) : dash, note.join(QStringLiteral("; "))});
        const QString key = QStringLiteral("G|") + g.day.toString(Qt::ISODate) + QLatin1Char('|') + g.serverId
                            + QLatin1Char('|') + g.client + QLatin1Char('|') + g.address;
        parent.at(Name)->setData(key, kKeyRole);
        parent.at(Name)->setToolTip(tr("Accounts used: %1").arg(g.users.join(QStringLiteral(", "))));
        QFont bold = parent.at(Name)->font();
        bold.setBold(true);
        parent.at(Name)->setFont(bold);
        parent.at(Total)->setFont(bold);
        if (g.client.isEmpty())
            parent.at(Computer)->setForeground(Theme::kLabelSecondary);
        if (g.inside)
            parent.at(LastOut)->setForeground(Theme::kGreen);
        parent.at(Away)->setForeground(Theme::kLabelSecondary);
        if (g.unknownMs > 0)
            parent.at(Unknown)->setForeground(Theme::kOrange);
        if (g.uncertain || g.unknownMs > 0)
            parent.at(Note)->setForeground(Theme::kOrange);

        int ordinal = 0;
        for (const UsageEngine::DayVisit &dv : g.visits) {
            const UsageEngine::Visit &v = report.visits.at(dv.visit);
            ++ordinal;
            QString start = dv.fromPreviousDay ? QStringLiteral("00:00:00") : clock(dv.start, true);
            QString end = clock(dv.end, true);
            if (!v.startKnown() && !dv.fromPreviousDay)
                start = tr("not known");
            if (v.open() && !dv.intoNextDay)
                end = tr("Still inside");
            else if (dv.intoNextDay)
                end = tr("Midnight (goes on)");
            else if (!v.endKnown())
                end = tr("not known");

            QStringList notes;
            if (v.inferred())
                notes << tr("Not in the event log: only the server's list of connected people shows it");
            else {
                if (!v.startKnown() && !dv.fromPreviousDay)
                    notes << tr("No sign-in event in the log");
                if (!v.endKnown() && !dv.intoNextDay && !v.open())
                    notes << tr("No sign-out event in the log");
            }
            if (v.joined() > 0)
                notes << tr("%1 short break(s) of up to %2 minutes counted as one visit")
                             .arg(v.joined()).arg(UsageEngine::kMergeGapSec / 60);
            if (v.open() && !dv.intoNextDay)
                notes << tr("Still going on");
            if (dv.unknownMs > 0)
                notes << tr("Part of it was not measured");

            const bool exact = v.startKnown() && v.endKnown() && !v.inferred();
            const QString prefix = exact || dv.totalMs() == 0 ? QString() : tr("at least ");
            const QList<QStandardItem *> visit = rowOf(
                {tr("Visit %1").arg(ordinal), {}, {}, {}, start, end, prefix + duration(dv.totalMs()),
                 duration(dv.activeMs), duration(dv.afkMs), dv.unknownMs > 0 ? duration(dv.unknownMs) : dash,
                 notes.join(QStringLiteral("; "))});
            visit.at(Name)->setToolTip(tr("Account: %1").arg(v.user));
            visit.at(Away)->setForeground(Theme::kLabelSecondary);
            if (dv.unknownMs > 0)
                visit.at(Unknown)->setForeground(Theme::kOrange);
            if (!exact || dv.unknownMs > 0)
                visit.at(Note)->setForeground(Theme::kOrange);
            if (v.open() && !dv.intoNextDay)
                visit.at(LastOut)->setForeground(Theme::kGreen);

            for (const UsageEngine::Part &p : dv.parts) {
                if (p.kind == UsageEngine::Kind::Active)
                    continue;
                const bool away = p.kind == UsageEngine::Kind::Afk;
                const QList<QStandardItem *> part = rowOf(
                    {away ? tr("Away (AFK)") : tr("Unknown"), {}, {}, {}, clock(p.start, true), clock(p.end, true),
                     duration(p.end - p.start), {}, away ? duration(p.end - p.start) : QString(),
                     away ? QString() : duration(p.end - p.start),
                     away ? tr("No mouse or keyboard for %1 minutes or more").arg(UsageEngine::kAfkMinutes)
                          : tr("Nobody measured this time")});
                paint(part, away ? Theme::kLabelSecondary : Theme::kOrange);
                visit.at(Name)->appendRow(part);
            }
            parent.at(Name)->appendRow(visit);
        }
        dayItem->appendRow(parent);

        totalMs += g.totalMs();
        activeMs += g.activeMs;
        afkMs += g.afkMs;
        unknownMs += g.unknownMs;
    }
    finishDay();

    // Days open, so the servers of each day show; what the user opened or closed stays that way.
    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *day = m_model->item(r, 0);
        const QString dayKey = day->data(kKeyRole).toString();
        m_view->setExpanded(day->index(), firstLoad || expanded.contains(dayKey) || !seen.contains(dayKey));
        for (int c = 0; c < day->rowCount(); ++c) {
            const QString key = day->child(c, 0)->data(kKeyRole).toString();
            m_view->setExpanded(day->child(c, 0)->index(), expanded.contains(key));
        }
    }
    m_view->verticalScrollBar()->setValue(scroll);

    // Where the server and its log disagree.
    QStringList warnings;
    QSet<QString> told;
    for (const UsageEngine::Mismatch &m : report.mismatches) {
        const QString text = m.serverSaysConnected
            ? tr("%1: the server says someone is connected, but its event log has no sign-in for that. This time is "
                 "shown as not in the log.")
                  .arg(m.server)
            : tr("%1: the server says nobody is connected, but its event log has no sign-out. The visit ends at the "
                 "last check that saw it.")
                  .arg(m.server);
        if (!told.contains(text)) {
            told.insert(text);
            warnings << text;
        }
    }
    m_warning->setText(warnings.join(QLatin1Char('\n')));
    m_warning->setVisible(!warnings.isEmpty());

    m_total->setText(groups == 0
                         ? tr("No visits in this period. Times are recorded while RConnect is open; sign-ins and "
                              "sign-outs are read from the servers' logs.")
                         : tr("Total: %1 on servers: %2 active, %3 away, %4 unknown.")
                               .arg(duration(totalMs), duration(activeMs), duration(afkMs), duration(unknownMs)));
}
