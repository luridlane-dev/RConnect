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
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return UsagePage::tr("%1 min").arg(minutes);
    return minutes % 60 == 0 ? UsagePage::tr("%1 h").arg(minutes / 60)
                             : UsagePage::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
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
    head->addWidget(m_range, 0, Qt::AlignTop);
    head->addWidget(m_refresh, 0, Qt::AlignTop);
    head->addStretch(1);
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
    const int widths[ColumnCount] = {113, 79, 65, 70, 75, 59, 53, 94};
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
    m_subtitle->setText(tr("Daily connection totals by computer and server. Expand a row for individual connections."));
    m_metricLabels[0]->setText(tr("Total connected"));
    m_metricLabels[1]->setText(tr("Input estimated"));
    m_metricLabels[2]->setText(tr("Idle estimated"));
    m_metricLabels[3]->setText(tr("Unmeasured"));
    m_refresh->setText(tr("Refresh"));
    m_rebuild->setText(tr("Rebuild totals"));
    m_rebuild->setToolTip(tr("Rebuilds the report from the Windows session events already saved on this PC."));
    m_help->setText(tr("How to read this"));
    const int keep = m_range->currentIndex();
    m_range->blockSignals(true);
    m_range->clear();
    m_range->addItems({tr("Today"), tr("Yesterday"), tr("Last 7 days"), tr("Last 30 days"), tr("All time")});
    m_range->setCurrentIndex(keep < 0 ? Today : keep);
    m_range->blockSignals(false);
    m_model->setHorizontalHeaderLabels({tr("Computer"), tr("Server"), tr("First seen"), tr("Last seen"),
                                        tr("Connected"), tr("Input"), tr("Idle"), tr("Unmeasured")});
    m_view->header()->setToolTip(tr("Input and idle are estimates. Unmeasured time is part of the connected total."));
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
        tr("<p><b>Connected.</b> Time between connection and disconnection. Short gaps of up to %1 minutes can appear "
           "in the same row, while the minutes offline stay out of the total.</p>"
           "<p><b>Input and idle.</b> Estimates from keyboard and mouse checks. A period with at least %2 minutes without "
           "input counts as idle. Gaps without enough measurements show as unmeasured.</p>"
           "<p><b>Incomplete records.</b> A connection without an end event is counted through the last time it was seen. "
           "When it was first seen late, the report shows a minimum.</p>"
           "<p>Each connection adds to the total. Two simultaneous one-hour connections add two hours.</p>")
            .arg(UsageEngine::kMergeGapSec / 60).arg(UsageEngine::kAfkMinutes));
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
    bool incomplete = false;
    const QString dash = QStringLiteral("—");
    QStandardItem *dayItem = nullptr;
    QList<QStandardItem *> dayRow;
    QDate currentDay;
    qint64 dayTotal = 0, dayActive = 0, dayAfk = 0, dayUnknown = 0;
    bool dayIncomplete = false;
    auto finishDay = [&] {
        if (!dayItem)
            return;
        dayRow.at(Total)->setText(dayIncomplete && dayTotal > 0 ? tr("At least %1").arg(duration(dayTotal))
                                                               : duration(dayTotal));
        dayRow.at(Active)->setText(duration(dayActive));
        dayRow.at(Away)->setText(duration(dayAfk));
        dayRow.at(Unknown)->setText(dayUnknown > 0 ? duration(dayUnknown) : dash);
    };

    for (const UsageEngine::DayGroup &g : report.days) {
        if (g.day < from || g.day > to)
            continue;
        ++groups;
        incomplete |= g.uncertain;
        if (!dayItem || g.day != currentDay) {
            finishDay();
            currentDay = g.day;
            dayTotal = dayActive = dayAfk = dayUnknown = 0;
            dayIncomplete = false;
            dayRow = rowOf(QStringList(int(ColumnCount), QString()));
            dayRow.at(Name)->setText(dayText(g.day));
            dayRow.at(Name)->setData(QStringLiteral("D|") + g.day.toString(Qt::ISODate), kKeyRole);
            QFont bold = dayRow.at(Name)->font();
            bold.setBold(true);
            bold.setPointSizeF(bold.pointSizeF() + 1);
            for (QStandardItem *item : dayRow) {
                item->setFont(bold);
                item->setBackground(palette().alternateBase());
            }
            dayItem = dayRow.at(Name);
            m_model->appendRow(dayRow);
        }
        dayTotal += g.totalMs();
        dayIncomplete |= g.uncertain;
        dayActive += g.activeMs;
        dayAfk += g.afkMs;
        dayUnknown += g.unknownMs;

        QString firstIn = clock(g.firstIn);
        QString lastOut = clock(g.lastOut);
        const UsageEngine::DayVisit *latest = nullptr;
        for (const UsageEngine::DayVisit &dv : g.visits) {
            if (dv.fromPreviousDay && dv.start == g.firstIn)
                firstIn = tr("%1 (carried over)").arg(clock(g.firstIn));
            if (!latest || dv.end >= latest->end)
                latest = &dv;
        }
        if (g.inside)
            lastOut = tr("Seen connected");
        else if (latest && latest->intoNextDay)
            lastOut = tr("Continues next day");

        // The person first: the nickname or computer name, else the address.
        const QString who = !g.client.isEmpty() ? Contacts::instance().label(g.client, {})
                            : !g.address.isEmpty() ? g.address
                                                    : tr("Unknown computer");
        const QList<QStandardItem *> parent = rowOf(
            {who, g.server, firstIn, lastOut, g.uncertain && g.totalMs() > 0 ? tr("At least %1").arg(duration(g.totalMs()))
                                                                                 : duration(g.totalMs()), duration(g.activeMs),
             duration(g.afkMs), g.unknownMs > 0 ? duration(g.unknownMs) : dash});
        const QString key = QStringLiteral("G|") + g.day.toString(Qt::ISODate) + QLatin1Char('|') + g.serverId
                            + QLatin1Char('|') + g.client + QLatin1Char('|') + g.address;
        parent.at(Name)->setData(key, kKeyRole);
        const QString detail = who + QLatin1Char('\n') + tr("Address: %1\nAccounts used: %2\nTimes in: %3")
            .arg(g.address.isEmpty() ? dash : g.address, g.users.join(QStringLiteral(", "))).arg(g.entries);
        parent.at(Name)->setToolTip(QStringLiteral("<qt>%1</qt>").arg(
            detail.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
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
                start = tr("No record");
            if (dv.open)
                end = tr("Seen connected");
            else if (dv.intoNextDay)
                end = tr("Continues next day");
            else if (!v.endKnown())
                end = tr("No record");

            const bool exact = v.startKnown() && v.endKnown() && !v.inferred();
            const QString connected = exact || dv.totalMs() == 0 ? duration(dv.totalMs())
                                                                  : tr("At least %1").arg(duration(dv.totalMs()));
            const QList<QStandardItem *> visit = rowOf(
                {tr("Connection %1").arg(ordinal), {}, start, end, connected,
                 duration(dv.activeMs), duration(dv.afkMs), dv.unknownMs > 0 ? duration(dv.unknownMs) : dash});
            visit.at(Name)->setToolTip(tr("Account: %1").arg(v.user));
            visit.at(Away)->setForeground(Theme::kLabelSecondary);
            if (dv.unknownMs > 0)
                visit.at(Unknown)->setForeground(Theme::kOrange);
            if (dv.open)
                visit.at(LastOut)->setForeground(Theme::kGreen);

            for (const UsageEngine::Part &p : dv.parts) {
                if (p.kind == UsageEngine::Kind::Active)
                    continue;
                const bool away = p.kind == UsageEngine::Kind::Afk;
                const QList<QStandardItem *> part = rowOf(
                    {away ? tr("Idle") : tr("Unmeasured"), {}, clock(p.start, true), clock(p.end, true),
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
    const qint64 totals[] = {totalMs, activeMs, afkMs, unknownMs};
    for (int i = 0; i < 4; ++i)
        m_metricValues[i]->setText(groups == 0 ? dash : duration(totals[i]));
    if (incomplete && totalMs > 0)
        m_metricValues[0]->setText(tr("At least %1").arg(duration(totalMs)));

    m_warning->setText(tr("Some connections are missing a start or end record. Their totals are minimums."));
    m_warning->setVisible(incomplete);
    m_total->setText(groups == 0 ? tr("No connections recorded for this period.")
        : tr("Times use this PC’s clock. Simultaneous connections count separately."));
}
