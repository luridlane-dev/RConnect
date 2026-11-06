#include "ServerSectionModel.h"
#include "model/ServerStatus.h"
#include "model/ServerStore.h"

#include <QHash>
#include <QSettings>
#include <QTimer>

namespace {

const QString kSettingsKey = QStringLiteral("ui/collapsedGroupNames");

bool isFree(const QModelIndex &i)
{
    return i.data(ServerStore::StatusRole).toInt() == ServerStatus::Online
        && i.data(ServerStore::OccupancyRole).toInt() != ServerStatus::Busy;
}

} // namespace

ServerSectionModel::ServerSectionModel(QAbstractItemModel *source, QObject *parent)
    : QAbstractListModel(parent)
    , m_source(source)
{
    const QStringList saved = QSettings().value(kSettingsKey).toStringList();
    m_collapsed = QSet<QString>(saved.cbegin(), saved.cend());

    connect(source, &QAbstractItemModel::rowsInserted, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::rowsRemoved, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::rowsMoved, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::modelReset, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::layoutChanged, this, [this] { rebuild(); });
    connect(source, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &last) { onDataChanged(first, last); });
    rebuild();
}

int ServerSectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant ServerSectionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    if (e.sourceRow < 0) {
        switch (role) {
        case HeaderRole:           return true;
        case SectionGroupRole:     return e.group;
        case SectionCountRole:     return e.count;
        case SectionFreeRole:      return e.free;
        case SectionCollapsedRole: return e.collapsed;
        default:                   return {};
        }
    }
    if (role == HeaderRole)
        return false;
    return m_source->index(e.sourceRow, 0).data(role);
}

void ServerSectionModel::toggleCollapsed(const QString &group)
{
    if (!m_collapsed.remove(group))
        m_collapsed.insert(group);
    saveCollapsed();
    rebuild();
}

void ServerSectionModel::saveCollapsed() const
{
    QSettings().setValue(kSettingsKey, QStringList(m_collapsed.cbegin(), m_collapsed.cend()));
}

ServerSectionModel::Snapshot ServerSectionModel::snapshot(bool withIds) const
{
    Snapshot s;
    const int rows = m_source->rowCount();
    s.group.resize(rows);
    if (withIds)
        s.id.resize(rows);
    for (int r = 0; r < rows; ++r) {
        const QModelIndex i = m_source->index(r, 0);
        s.group[r] = i.data(ServerStore::GroupRole).toString();
        if (withIds)
            s.id[r] = i.data(ServerStore::IdRole).toString();
        Totals &t = s.totals[s.group.at(r)];
        ++t.count;
        t.free += isFree(i);
    }
    return s;
}

// Brings the rows in line with the source by removing and inserting only what differs, so the view keeps its
// scroll position and hover while statuses change.
void ServerSectionModel::rebuild()
{
    const Snapshot s = snapshot(true);
    const int rows = int(s.group.size());
    // Headers are shown as soon as any server has a group; before that the list is plain cards.
    const bool headers = s.totals.size() > 1 || (s.totals.size() == 1 && !s.totals.contains(QString()));

    QVector<Entry> fresh;
    fresh.reserve(rows + int(s.totals.size()));
    QString last;
    bool first = true;
    for (int r = 0; r < rows; ++r) {
        const QString &group = s.group.at(r);
        if (headers && (first || group != last)) {
            Entry h;
            h.group = group;
            h.collapsed = m_collapsed.contains(group);
            h.count = s.totals.value(group).count;
            h.free = s.totals.value(group).free;
            fresh.append(h);
            last = group;
            first = false;
        }
        if (headers && m_collapsed.contains(group))
            continue;
        Entry e;
        e.sourceRow = r;
        e.id = s.id.at(r);
        e.group = group;
        fresh.append(e);
    }

    // Rows that stay at the start and at the end are kept; whatever lies between is replaced.
    const auto sameRow = [](const Entry &a, const Entry &b) {
        return (a.sourceRow < 0) == (b.sourceRow < 0) && (a.sourceRow < 0 ? a.group == b.group : a.id == b.id);
    };
    const int oldSize = int(m_entries.size()), newSize = int(fresh.size());
    int head = 0;
    while (head < oldSize && head < newSize && sameRow(m_entries.at(head), fresh.at(head)))
        ++head;
    int tail = 0;
    while (tail < oldSize - head && tail < newSize - head
           && sameRow(m_entries.at(oldSize - 1 - tail), fresh.at(newSize - 1 - tail)))
        ++tail;

    // Kept cards may sit on other source rows now; fix that before anything is announced.
    for (int i = 0; i < head; ++i)
        m_entries[i].sourceRow = fresh.at(i).sourceRow;
    for (int t = 0; t < tail; ++t)
        m_entries[oldSize - 1 - t].sourceRow = fresh.at(newSize - 1 - t).sourceRow;

    const int oldMid = oldSize - head - tail, newMid = newSize - head - tail;
    if (oldMid > 0) {
        beginRemoveRows({}, head, head + oldMid - 1);
        m_entries.remove(head, oldMid);
        endRemoveRows();
    }
    if (newMid > 0) {
        beginInsertRows({}, head, head + newMid - 1);
        for (int i = 0; i < newMid; ++i)
            m_entries.insert(head + i, fresh.at(head + i));
        endInsertRows();
    }

    m_groupOfSource = s.group;
    m_rowOfSource.fill(-1, rows);
    for (int row = 0; row < m_entries.size(); ++row) {
        const Entry &e = m_entries.at(row);
        if (e.sourceRow >= 0)
            m_rowOfSource[e.sourceRow] = row;
    }
    // Headers that stayed may show other totals or folded state.
    for (int row = 0; row < m_entries.size(); ++row) {
        Entry &e = m_entries[row];
        const Entry &f = fresh.at(row);
        if (e.sourceRow >= 0 || (e.count == f.count && e.free == f.free && e.collapsed == f.collapsed))
            continue;
        e.count = f.count;
        e.free = f.free;
        e.collapsed = f.collapsed;
        emit dataChanged(index(row), index(row));
    }
}

// Counts of the headers follow the statuses without rebuilding the list.
void ServerSectionModel::refreshHeaders()
{
    const Snapshot s = snapshot(false);
    for (int row = 0; row < m_entries.size(); ++row) {
        Entry &e = m_entries[row];
        if (e.sourceRow >= 0)
            continue;
        const Totals t = s.totals.value(e.group);
        if (e.count == t.count && e.free == t.free)
            continue;
        e.count = t.count;
        e.free = t.free;
        emit dataChanged(index(row), index(row));
    }
}

// A sweep of status updates changes many rows; the headers are counted once afterwards.
void ServerSectionModel::queueHeaderRefresh()
{
    if (m_headersQueued)
        return;
    m_headersQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_headersQueued = false;
        refreshHeaders();
    });
}

void ServerSectionModel::onDataChanged(const QModelIndex &first, const QModelIndex &last)
{
    // A server moved to another group without changing place in the order: the headers need redoing.
    for (int r = first.row(); r <= last.row() && r < m_groupOfSource.size(); ++r)
        if (m_source->index(r, 0).data(ServerStore::GroupRole).toString() != m_groupOfSource.at(r)) {
            rebuild();
            return;
        }
    for (int r = first.row(); r <= last.row() && r < m_rowOfSource.size(); ++r) {
        const int row = m_rowOfSource.at(r);
        if (row >= 0)
            emit dataChanged(index(row), index(row));
    }
    queueHeaderRefresh();
}
