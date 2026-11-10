#include "UsagePage.h"

#include "Theme.h"
#include "core/Contacts.h"
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

enum Column { Name, Server, FirstIn, LastOut, Total, Active, Away, Unknown, ColumnCount };
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
    const int widths[ColumnCount] = {300, 170, 150, 150, 130, 110, 110, 110};
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
    connect(&Contacts::instance(), &Contacts::changed, this, [this] { if (isVisible()) reload(); });
    retranslate();
}

void UsagePage::retranslate()
{
    m_title->setText(tr("Time on servers"));
    m_subtitle->setText(tr("Who was on which server, when they came in and left, and how long they really worked. "
                           "If nobody touches the mouse or keyboard for %1 minutes, that time counts as away. "
                           "Times are in this PC's time zone (%2).")
                            .arg(UsageEngine::kAfkMinutes)
                            .arg(QString::fromLatin1(QTimeZone::systemTimeZoneId())));
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
    m_model->setHorizontalHeaderLabels({tr("Who"), tr("Server"), tr("Came in"), tr("Left"), tr("Time connected"),
                                        tr("Working"), tr("Away"), tr("Unknown")});
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
        tr("<p><b>Came in and left.</b> The server writes down every time someone signs in, disconnects or signs "
           "out, so these times are exact. If the same computer comes back within %1 minutes it counts as one stay.</p>"
           "<p><b>Working and away.</b> RConnect asks each server about every 2 minutes how long the person has not "
           "touched the mouse or keyboard. After %2 minutes without any touch, that time counts as away.</p>"
           "<p><b>Unknown.</b> Working or away can only be told while RConnect is open and can reach the server. "
           "For the rest the total is still right, but the split is not guessed. Keep RConnect running (Settings > "
           "Start with Windows) to measure all day.</p>"
           "<p>Away time can be off by up to 2 minutes. Computer names come from the server at the time of the visit.</p>")
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
    auto finishDay = [&] {
        if (!dayItem)
            return;
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

        // The person first: the nickname or computer name, else the address.
        const QString who = !g.client.isEmpty() ? Contacts::instance().label(g.client, {})
                            : !g.address.isEmpty() ? g.address
                                                    : tr("Unknown computer");
        const QList<QStandardItem *> parent = rowOf(
            {who, g.server, firstIn, lastOut, duration(g.totalMs()), duration(g.activeMs),
             duration(g.afkMs), g.unknownMs > 0 ? duration(g.unknownMs) : dash});
        const QString key = QStringLiteral("G|") + g.day.toString(Qt::ISODate) + QLatin1Char('|') + g.serverId
                            + QLatin1Char('|') + g.client + QLatin1Char('|') + g.address;
        parent.at(Name)->setData(key, kKeyRole);
        parent.at(Name)->setToolTip(tr("Address: %1\nAccounts used: %2\nTimes in: %3")
                                        .arg(g.address.isEmpty() ? dash : g.address, g.users.join(QStringLiteral(", ")))
                                        .arg(g.entries));
        QFont bold = parent.at(Name)->font();
        bold.setBold(true);
        parent.at(Name)->setFont(bold);
        parent.at(Total)->setFont(bold);
        if (g.inside)
            parent.at(LastOut)->setForeground(Theme::kGreen);
        parent.at(Away)->setForeground(Theme::kLabelSecondary);
        if (g.unknownMs > 0)
            parent.at(Unknown)->setForeground(Theme::kOrange);

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

            const bool exact = v.startKnown() && v.endKnown() && !v.inferred();
            const QString prefix = exact || dv.totalMs() == 0 ? QString() : tr("at least ");
            const QList<QStandardItem *> visit = rowOf(
                {tr("Time in %1").arg(ordinal), {}, start, end, prefix + duration(dv.totalMs()),
                 duration(dv.activeMs), duration(dv.afkMs), dv.unknownMs > 0 ? duration(dv.unknownMs) : dash});
            visit.at(Name)->setToolTip(tr("Account: %1").arg(v.user));
            visit.at(Away)->setForeground(Theme::kLabelSecondary);
            if (dv.unknownMs > 0)
                visit.at(Unknown)->setForeground(Theme::kOrange);
            if (v.open() && !dv.intoNextDay)
                visit.at(LastOut)->setForeground(Theme::kGreen);

            for (const UsageEngine::Part &p : dv.parts) {
                if (p.kind == UsageEngine::Kind::Active)
                    continue;
                const bool away = p.kind == UsageEngine::Kind::Afk;
                const QList<QStandardItem *> part = rowOf(
                    {away ? tr("Away") : tr("Unknown"), {}, clock(p.start, true), clock(p.end, true),
                     duration(p.end - p.start), {}, away ? duration(p.end - p.start) : QString(),
                     away ? QString() : duration(p.end - p.start)});
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
    m_view->setColumnHidden(Unknown, unknownMs == 0); // only when something was not measured

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
                         : unknownMs > 0
                             ? tr("In total %1 on servers: %2 working, %3 away, %4 unknown.")
                                   .arg(duration(totalMs), duration(activeMs), duration(afkMs), duration(unknownMs))
                             : tr("In total %1 on servers: %2 working, %3 away.")
                                   .arg(duration(totalMs), duration(activeMs), duration(afkMs)));
}
