#include "UsagePage.h"

#include "ContactsDialog.h"
#include "Theme.h"
#include "core/Contacts.h"
#include "core/UsageEngine.h"
#include "core/UsageLog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

#include <utility>

namespace {

enum Column { Name, Net, Idle, Total, Status, ColumnCount };
enum Range { Today, Yesterday, Week, Month, Everything };
constexpr int kKeyRole = Qt::UserRole + 1;
constexpr int kDeviceRole = Qt::UserRole + 2;
constexpr int kAddressRole = Qt::UserRole + 3;
constexpr int kUserRole = Qt::UserRole + 4;
constexpr int kAddressesRole = Qt::UserRole + 5;
constexpr int kRefreshMs = 30000;

struct ContactTarget {
    QString device;
    QString address;
    QString user;
    QStringList addresses;
    bool valid() const { return !device.isEmpty() || !address.isEmpty(); }
};

ContactTarget targetForIndex(const QModelIndex &index)
{
    ContactTarget t;
    if (!index.isValid())
        return t;
    QModelIndex cur = index.siblingAtColumn(Name);
    while (cur.isValid()) {
        const QString dev = cur.data(kDeviceRole).toString();
        const QString addr = cur.data(kAddressRole).toString();
        const QString usr = cur.data(kUserRole).toString();
        const QStringList addrs = cur.data(kAddressesRole).toStringList();
        if (!dev.isEmpty() || !addr.isEmpty() || !addrs.isEmpty()) {
            t.device = dev;
            t.address = addr;
            t.user = usr;
            t.addresses = addrs;
            if (t.address.isEmpty() && !t.addresses.isEmpty())
                t.address = t.addresses.first();
            return t;
        }
        cur = cur.parent();
    }
    return t;
}

// Paints a uniform full-row hover and selection background (including the branch gutter) so Fusion's
// expand triangles stay crisp and never suffer double-blended seams.
class UsageTreeView : public QTreeView {
public:
    explicit UsageTreeView(QWidget *parent = nullptr)
        : QTreeView(parent)
    {
        setMouseTracking(true);
    }

protected:
    void mouseMoveEvent(QMouseEvent *e) override
    {
        setHovered(indexAt(e->pos()));
        QTreeView::mouseMoveEvent(e);
    }

    void leaveEvent(QEvent *e) override
    {
        setHovered({});
        QTreeView::leaveEvent(e);
    }

    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool selected = selectionModel() && selectionModel()->isRowSelected(index.row(), index.parent());
        const bool hovered = m_hovered.isValid() && m_hovered.row() == index.row() && m_hovered.parent() == index.parent();
        static const QColor kSelHoverDark("#192c44"), kSelHoverLight("#d3e5fc");
        static const QColor kSelDark("#142336"), kSelLight("#e1edfd");
        static const QColor kHoverDark("#17171b"), kHoverLight("#eef0f5");
        const bool dark = Theme::mode() == Theme::Dark;
        if (selected) {
            painter->fillRect(option.rect, hovered ? (dark ? kSelHoverDark : kSelHoverLight)
                                                   : (dark ? kSelDark : kSelLight));
        } else if (hovered) {
            painter->fillRect(option.rect, dark ? kHoverDark : kHoverLight);
        }
        QTreeView::drawRow(painter, option, index);
    }

private:
    void setHovered(const QModelIndex &idx)
    {
        const QModelIndex rowIdx = idx.isValid() ? idx.siblingAtColumn(0) : QModelIndex();
        if (m_hovered == rowIdx)
            return;
        const QPersistentModelIndex prev = m_hovered;
        m_hovered = rowIdx;
        if (prev.isValid())
            viewport()->update(QRect(0, visualRect(prev).top(), viewport()->width(), visualRect(prev).height()));
        if (m_hovered.isValid())
            viewport()->update(QRect(0, visualRect(m_hovered).top(), viewport()->width(), visualRect(m_hovered).height()));
    }

    QPersistentModelIndex m_hovered;
};

QString clockText(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
}

QString durationText(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    if (seconds <= 0)
        return QStringLiteral("—");
    if (seconds < 60)
        return UsagePage::tr("%1 sec").arg(seconds);
    const qint64 minutes = (seconds + 30) / 60;
    if (minutes < 60)
        return UsagePage::tr("%1 min").arg(minutes);
    return minutes % 60 == 0 ? UsagePage::tr("%1 h").arg(minutes / 60)
                             : UsagePage::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString dayText(const QDate &day, const QDate &today)
{
    if (day == today)
        return UsagePage::tr("Today");
    if (day == today.addDays(-1))
        return UsagePage::tr("Yesterday");
    return day.toString(QStringLiteral("dd.MM.yyyy ddd"));
}

QString lastSeen(qint64 ms, const QDate &today)
{
    if (ms <= 0)
        return QStringLiteral("—");
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    if (at.date() == today)
        return at.toString(QStringLiteral("HH:mm"));
    if (at.date() == today.addDays(-1))
        return UsagePage::tr("Yesterday %1").arg(at.toString(QStringLiteral("HH:mm")));
    return at.toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

QString html(const QStringList &lines)
{
    QStringList escaped;
    for (const QString &line : lines)
        escaped << line.toHtmlEscaped();
    return QStringLiteral("<qt>%1</qt>").arg(escaped.join(QStringLiteral("<br>")));
}

QList<QStandardItem *> timeRow(const QString &name, const UsageEngine::Time &t)
{
    QList<QStandardItem *> row;
    row << new QStandardItem(name)
        << new QStandardItem(durationText(t.netMs()))
        << new QStandardItem(durationText(t.afkMs))
        << new QStandardItem(durationText(t.totalMs()))
        << new QStandardItem();
    row.at(Net)->setForeground(t.netMs() > 0 ? Theme::kGreen : Theme::kLabelTertiary);
    row.at(Idle)->setForeground(t.afkMs >= 60000 ? Theme::kOrange : Theme::kLabelTertiary);
    row.at(Total)->setForeground(Theme::kLabelSecondary);
    return row;
}

// Builds an O(1) lookup table of normalized contact person names once per aggregation pass
// instead of calling Contacts::names() (O(C^2)) + regex per unmatched visit.
QHash<QString, QString> buildContactPersonMap()
{
    QHash<QString, QString> map;
    for (const Contact &c : Contacts::instance().all()) {
        if (c.name.isEmpty())
            continue;
        const QString person = UsageEngine::personNameOf(c.name);
        if (!person.isEmpty()) {
            const QString key = person.toLower();
            if (!map.contains(key))
                map.insert(key, person);
        }
    }
    return map;
}

UsageEngine::Who whoOfWithIndex(const UsageEngine::Visit &v, const QHash<QString, QString> &personByName)
{
    const Contact *c = Contacts::instance().match(v.client, v.user, v.address);
    if (c && !c->name.trimmed().isEmpty()) {
        const QString person = UsageEngine::personNameOf(c->name);
        return {QStringLiteral("N|") + person.toLower(), person};
    }
    if (!v.client.isEmpty()) {
        const QString clientLower = v.client.toLower();
        if (const auto it = personByName.constFind(clientLower); it != personByName.cend())
            return {QStringLiteral("N|") + it.key(), it.value()};
        const QString clientBaseLower = UsageEngine::personNameOf(v.client).toLower();
        if (const auto it = personByName.constFind(clientBaseLower); it != personByName.cend())
            return {QStringLiteral("N|") + it.key(), it.value()};
        return {QStringLiteral("C|") + clientLower, v.client};
    }
    if (!v.address.isEmpty())
        return {QStringLiteral("A|") + v.address, v.address};
    if (!v.user.isEmpty())
        return {QStringLiteral("U|") + v.user.toLower(), v.user};
    return {QStringLiteral("?"), UsagePage::tr("Unknown")};
}

QString personDisplayName(const UsageEngine::Person &p)
{
    if (p.computers.isEmpty())
        return p.name;
    if (p.computers.size() == 1 && p.computers.first().compare(p.name, Qt::CaseInsensitive) == 0)
        return p.name;
    if (p.computers.size() <= 2)
        return QStringLiteral("%1 (%2)").arg(p.name, p.computers.join(QStringLiteral(", ")));
    return UsagePage::tr("%1 (%2 devices)").arg(p.name).arg(p.computers.size());
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
    m_range->setMinimumWidth(135);
    connect(m_range, &QComboBox::currentIndexChanged, this, [this] { renderCurrent(); });

    m_search = new QLineEdit(this);
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(190);
    m_search->setMaximumWidth(280);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout, this, &UsagePage::renderCurrent);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty()) {
            m_searchTimer->stop();
            renderCurrent();
        } else {
            m_searchTimer->start();
        }
    });

    m_refresh = new QPushButton(this);
    m_refresh->setCursor(Qt::PointingHandCursor);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        emit syncRequested({});
        reload();
    });
    m_addContact = new QPushButton(this);
    m_addContact->setCursor(Qt::PointingHandCursor);
    m_addContact->setEnabled(false);
    connect(m_addContact, &QPushButton::clicked, this, &UsagePage::editSelectedContact);
    m_configure = new QPushButton(this);
    m_configure->setCursor(Qt::PointingHandCursor);
    connect(m_configure, &QPushButton::clicked, this, [this] { emit configureServersRequested(); });

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    head->addWidget(m_range);
    head->addWidget(m_search);
    head->addWidget(m_refresh);
    head->addWidget(m_addContact);
    head->addStretch(1);
    head->addWidget(m_configure);

    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(12);
    for (int i = 0; i < 4; ++i) {
        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("detailCard"));
        auto *metric = new QVBoxLayout(card);
        metric->setContentsMargins(16, 12, 16, 12);
        metric->setSpacing(4);
        m_metricLabels[i] = new QLabel(card);
        m_metricLabels[i]->setObjectName(QStringLiteral("fieldHint"));
        m_metricValues[i] = new QLabel(QStringLiteral("—"), card);
        m_metricValues[i]->setObjectName(QStringLiteral("metricValue"));
        metric->addWidget(m_metricLabels[i]);
        metric->addWidget(m_metricValues[i]);
        metrics->addWidget(card, 1);
    }

    m_view = new UsageTreeView(this);
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setUniformRowHeights(true);
    m_view->setIndentation(20);
    m_view->setAllColumnsShowFocus(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setHighlightSections(false);
    m_view->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int widths[ColumnCount] = {260, 125, 110, 125, 140};
    for (int c = 0; c < ColumnCount; ++c)
        m_view->setColumnWidth(c, widths[c]);
    m_view->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    connect(m_view, &QWidget::customContextMenuRequested, this, &UsagePage::showContextMenu);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &UsagePage::updateContactButton);

    m_total = new QLabel(this);
    m_total->setObjectName(QStringLiteral("fieldHint"));
    m_total->setWordWrap(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(12);
    lay->addLayout(titles);
    lay->addLayout(head);
    lay->addLayout(metrics);
    lay->addWidget(m_view, 1);
    lay->addWidget(m_total);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &UsagePage::reload);
    m_syncReloadTimer = new QTimer(this);
    m_syncReloadTimer->setSingleShot(true);
    m_syncReloadTimer->setInterval(1000);
    connect(m_syncReloadTimer, &QTimer::timeout, this, &UsagePage::reload);
    connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] {
        m_activeMetricStyle.clear();
        m_idleMetricStyle.clear();
        if (isVisible())
            renderCurrent();
    });
    connect(&Contacts::instance(), &Contacts::changed, this, [this] {
        m_peopleValid = false;
        if (isVisible())
            renderCurrent();
    });
    retranslate();
}

void UsagePage::retranslate()
{
    m_title->setText(tr("Time on servers"));
    m_subtitle->setText(tr("Daily active and idle time pulled from the servers."));
    m_metricLabels[0]->setText(tr("People"));
    m_metricLabels[1]->setText(tr("Active"));
    m_metricLabels[2]->setText(tr("Idle"));
    m_metricLabels[3]->setText(tr("Total"));
    m_search->setPlaceholderText(tr("Search name or server"));
    m_refresh->setText(tr("Refresh"));
    m_configure->setText(tr("Server settings"));
    m_configure->setToolTip(tr("Enable computer name logging and automatic idle disconnect on servers."));
    updateContactButton();

    const int keep = m_range->currentIndex();
    m_range->blockSignals(true);
    m_range->clear();
    m_range->addItems({tr("Today"), tr("Yesterday"), tr("Last 7 days"), tr("Last 30 days"), tr("All time")});
    m_range->setCurrentIndex(keep < 0 ? Today : keep);
    m_range->blockSignals(false);

    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Active"), tr("Idle"), tr("Total"), tr("Status")});
    if (isVisible())
        renderCurrent();
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
        m_syncReloadTimer->start();
}

void UsagePage::setConfiguringServers(bool busy)
{
    m_configure->setEnabled(!busy);
    m_configure->setText(busy ? tr("Applying…") : tr("Server settings"));
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
            page->m_lastReport = report;
            page->m_hasReport = true;
            page->m_peopleValid = false;
            if (page->isVisible())
                page->showReport(page->m_lastReport);
            if (std::exchange(page->m_readAgain, false))
                page->reload();
        }, Qt::QueuedConnection);
    });
}

void UsagePage::renderCurrent()
{
    if (m_hasReport)
        showReport(m_lastReport);
    else
        reload();
}

void UsagePage::showReport(const UsageEngine::Report &report)
{
    const QDate today = QDate::currentDate();
    const int rangeIdx = m_range->currentIndex();
    QDate from = QDate(1970, 1, 1), to = today;
    switch (rangeIdx) {
    case Today:      from = today; break;
    case Yesterday:  from = to = today.addDays(-1); break;
    case Week:       from = today.addDays(-6); break;
    case Month:      from = today.addDays(-29); break;
    default:         break;
    }
    const bool oneDay = from == to;
    if (!m_peopleValid || m_cachedRange != rangeIdx || m_cachedToday != today) {
        const QHash<QString, QString> personMap = buildContactPersonMap();
        m_cachedPeople = UsageEngine::people(
            report, from, to,
            [&personMap](const UsageEngine::Visit &v) { return whoOfWithIndex(v, personMap); },
            UsageEngine::Breakdown::ByDay);
        m_cachedRange = rangeIdx;
        m_cachedToday = today;
        m_peopleValid = true;
    }

    const QString query = m_search->text().trimmed();
    QList<const UsageEngine::Person *> visiblePeople;
    visiblePeople.reserve(m_cachedPeople.size());
    for (const UsageEngine::Person &p : std::as_const(m_cachedPeople)) {
        if (!query.isEmpty()) {
            bool hit = p.name.contains(query, Qt::CaseInsensitive);
            for (const QString &c : p.computers)
                if (!hit && c.contains(query, Qt::CaseInsensitive))
                    hit = true;
            for (const QString &a : p.addresses)
                if (!hit && a.contains(query, Qt::CaseInsensitive))
                    hit = true;
            for (const QString &s : p.servers)
                if (!hit && s.contains(query, Qt::CaseInsensitive))
                    hit = true;
            if (!hit)
                continue;
        }
        visiblePeople << &p;
    }

    QString selectedKey;
    const QModelIndexList selectedRows = m_view->selectionModel()->selectedRows(Name);
    const QModelIndex selectedIdx = !selectedRows.isEmpty() ? selectedRows.first() : m_view->currentIndex().siblingAtColumn(Name);
    if (selectedIdx.isValid())
        selectedKey = selectedIdx.data(kKeyRole).toString();

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

    m_view->setUpdatesEnabled(false);
    m_model->removeRows(0, m_model->rowCount());
    UsageEngine::Time sum;
    int connectedNow = 0;
    const QString dash = QStringLiteral("—");
    const QFont mediumFont = Theme::font(13, QFont::Medium);
    const QFont demiFont = Theme::font(13, QFont::DemiBold);
    QStandardItem *restoreItem = nullptr;

    for (const UsageEngine::Person *pp : std::as_const(visiblePeople)) {
        const UsageEngine::Person &p = *pp;
        sum.activeMs += p.time.activeMs;
        sum.afkMs += p.time.afkMs;
        sum.unknownMs += p.time.unknownMs;
        connectedNow += p.open ? 1 : 0;

        const QList<QStandardItem *> person = timeRow(personDisplayName(p), p.time);
        const QString personKey = QStringLiteral("P|") + p.key;
        if (!selectedKey.isEmpty() && personKey == selectedKey)
            restoreItem = person.at(Name);
        person.at(Name)->setData(personKey, kKeyRole);
        person.at(Name)->setData(p.computers.isEmpty() ? QString() : p.computers.first(), kDeviceRole);
        person.at(Name)->setData(p.addresses.isEmpty() ? QString() : p.addresses.first(), kAddressRole);
        person.at(Name)->setData(p.accounts.isEmpty() ? QString() : p.accounts.first(), kUserRole);
        person.at(Name)->setData(p.addresses, kAddressesRole);
        person.at(Name)->setFont(mediumFont);
        person.at(Net)->setFont(demiFont);
        person.at(Status)->setText(p.open ? tr("Online") : lastSeen(p.lastEnd, today));
        person.at(Status)->setForeground(p.open ? Theme::kGreen : Theme::kLabelSecondary);

        QStringList about{p.name};
        if (!p.addresses.isEmpty())
            about << tr("IP: %1").arg(p.addresses.join(QStringLiteral(", ")));
        if (!p.servers.isEmpty())
            about << tr("Servers: %1").arg(p.servers.join(QStringLiteral(", ")));
        if (!oneDay)
            about << tr("Days: %1").arg(p.days);
        person.at(Name)->setToolTip(html(about));

        for (const UsageEngine::Share &s : p.shares) {
            const QList<QStandardItem *> share = timeRow(dayText(s.day, today), s.time);
            const QString shareKey = personKey + QStringLiteral("|D|") + s.day.toString(Qt::ISODate);
            if (!selectedKey.isEmpty() && shareKey == selectedKey)
                restoreItem = share.at(Name);
            share.at(Name)->setData(shareKey, kKeyRole);
            share.at(Status)->setText(s.open ? tr("Online") : lastSeen(s.lastEnd, today));
            share.at(Status)->setForeground(s.open ? Theme::kGreen : Theme::kLabelSecondary);

            for (const UsageEngine::Entry &e : s.entries) {
                const UsageEngine::Visit &v = report.visits.at(e.visit);
                const UsageEngine::DayVisit &dv = e.slice;
                const QString start = dv.fromPreviousDay ? QStringLiteral("00:00") : clockText(dv.start);
                const QString end = dv.open ? tr("now") : dv.intoNextDay ? QStringLiteral("24:00") : clockText(dv.end);
                const QString span = QStringLiteral("%1 – %2").arg(start, end);
                const QString label = v.server.isEmpty() ? span : v.server + QStringLiteral("   ") + span;
                const UsageEngine::Time t{dv.activeMs, dv.afkMs, dv.unknownMs};
                const QList<QStandardItem *> entry = timeRow(label, t);
                const QString entryKey = shareKey + QStringLiteral("|V|") + v.serverId + QLatin1Char('|')
                    + QString::number(dv.start);
                if (!selectedKey.isEmpty() && entryKey == selectedKey)
                    restoreItem = entry.at(Name);
                entry.at(Name)->setData(entryKey, kKeyRole);
                entry.at(Name)->setData(v.client, kDeviceRole);
                entry.at(Name)->setData(v.address, kAddressRole);
                entry.at(Name)->setData(v.user, kUserRole);
                if (!v.address.isEmpty())
                    entry.at(Name)->setData(QStringList{v.address}, kAddressesRole);
                if (dv.open) {
                    entry.at(Status)->setText(tr("Online"));
                    entry.at(Status)->setForeground(Theme::kGreen);
                }

                QStringList detail{tr("Server: %1").arg(v.server)};
                if (!v.client.isEmpty())
                    detail << tr("Computer: %1").arg(v.client);
                if (!v.address.isEmpty())
                    detail << tr("IP: %1").arg(v.address);
                for (const UsageEngine::Part &part : dv.parts) {
                    if (part.kind != UsageEngine::Kind::Afk)
                        continue;
                    detail << tr("Idle %1 – %2 (%3)")
                                  .arg(clockText(part.start), clockText(part.end), durationText(part.end - part.start));
                }
                entry.at(Name)->setToolTip(html(detail));
                share.at(Name)->appendRow(entry);
            }
            person.at(Name)->appendRow(share);
        }
        m_model->appendRow(person);
    }

    for (int r = 0; r < m_model->rowCount(); ++r) {
        QStandardItem *person = m_model->item(r, 0);
        const QString key = person->data(kKeyRole).toString();
        m_view->setExpanded(person->index(), expanded.contains(key) || (firstLoad && m_model->rowCount() == 1));
        for (int c = 0; c < person->rowCount(); ++c) {
            QStandardItem *share = person->child(c, 0);
            m_view->setExpanded(share->index(), expanded.contains(share->data(kKeyRole).toString()));
        }
    }
    if (restoreItem) {
        m_view->selectionModel()->setCurrentIndex(
            restoreItem->index(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_view->verticalScrollBar()->setValue(scroll);
    m_view->setUpdatesEnabled(true);
    updateContactButton();

    const bool empty = visiblePeople.isEmpty();
    m_metricValues[0]->setText(empty ? dash : connectedNow > 0 ? tr("%1 (%2 online)").arg(visiblePeople.size()).arg(connectedNow)
                                                               : QString::number(visiblePeople.size()));
    m_metricValues[1]->setText(empty ? dash : durationText(sum.netMs()));
    const QString activeStyle = QStringLiteral("color: %1;").arg(Theme::kGreen.name());
    if (activeStyle != m_activeMetricStyle) {
        m_activeMetricStyle = activeStyle;
        m_metricValues[1]->setStyleSheet(activeStyle);
    }
    m_metricValues[2]->setText(empty ? dash : durationText(sum.afkMs));
    const QString idleStyle = sum.afkMs >= 60000 ? QStringLiteral("color: %1;").arg(Theme::kOrange.name()) : QString();
    if (idleStyle != m_idleMetricStyle) {
        m_idleMetricStyle = idleStyle;
        m_metricValues[2]->setStyleSheet(idleStyle);
    }
    m_metricValues[3]->setText(empty ? dash : durationText(sum.totalMs()));

    m_total->setText(empty ? tr("No records for this period.")
                           : tr("Calculated directly from server login, disconnect and idle data."));
}

void UsagePage::updateContactButton()
{
    const QModelIndexList selected = m_view->selectionModel()->selectedRows(Name);
    const QModelIndex idx = !selected.isEmpty() ? selected.first() : m_view->currentIndex();
    const ContactTarget target = targetForIndex(idx);
    m_addContact->setEnabled(target.valid());
    const bool known = target.valid() && Contacts::instance().match(target.device, target.user, target.address) != nullptr;
    m_addContact->setText(known ? tr("Edit contact…") : tr("Add to contacts…"));
}

void UsagePage::editSelectedContact()
{
    const QModelIndexList selected = m_view->selectionModel()->selectedRows(Name);
    const QModelIndex idx = !selected.isEmpty() ? selected.first() : m_view->currentIndex();
    const ContactTarget target = targetForIndex(idx);
    if (!target.valid())
        return;
    ContactsDialog::editFor(target.device, target.user, this, target.address);
}

void UsagePage::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    const ContactTarget target = targetForIndex(index);
    if (!target.valid())
        return;

    QMenu menu(this);
    const bool known = Contacts::instance().match(target.device, target.user, target.address) != nullptr;
    const QString primaryLabel = !target.device.isEmpty() ? target.device : target.address;
    menu.addAction(known ? tr("Edit contact…") : tr("Add “%1” to contacts…").arg(primaryLabel), this,
                   [this, target] { ContactsDialog::editFor(target.device, target.user, this, target.address); });

    // When a computer row also has IP address(es), offer adding a specific IP directly to contacts too.
    if (!target.device.isEmpty() && !target.addresses.isEmpty()) {
        for (const QString &addr : target.addresses.mid(0, 4)) {
            if (addr.isEmpty() || addr.compare(QLatin1String("LOCAL"), Qt::CaseInsensitive) == 0)
                continue;
            const bool addrKnown = Contacts::instance().match({}, {}, addr) != nullptr;
            menu.addAction(addrKnown ? tr("Edit IP “%1” contact…").arg(addr)
                                     : tr("Add IP “%1” to contacts…").arg(addr),
                           this, [this, addr] { ContactsDialog::editFor({}, {}, this, addr); });
        }
    }

    if (!target.addresses.isEmpty()) {
        menu.addSeparator();
        const QString clip = target.addresses.join(QStringLiteral(", "));
        menu.addAction(target.addresses.size() == 1 ? tr("Copy IP address") : tr("Copy IP addresses"), this,
                       [clip] { QApplication::clipboard()->setText(clip); });
    }
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
