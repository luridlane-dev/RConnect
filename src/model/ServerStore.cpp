#include "ServerStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {
constexpr int kSaveDelayMs = 500;
constexpr int kSummaryDelayMs = 150;
}

ServerStore::ServerStore(QObject *parent)
    : QAbstractListModel(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { save(); });
    m_summaryTimer.setSingleShot(true);
    m_summaryTimer.setInterval(kSummaryDelayMs);
    connect(&m_summaryTimer, &QTimer::timeout, this, &ServerStore::summaryChanged);
}

ServerStore::~ServerStore()
{
    if (m_saveTimer.isActive())
        save();
}

void ServerStore::rebuildIndex()
{
    m_rows.clear();
    m_rows.reserve(m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i)
        m_rows.insert(m_profiles.at(i).id, i);
}

// Many writes in a row (country lookups) become one file write.
void ServerStore::scheduleSave()
{
    m_saveTimer.start();
}

void ServerStore::queueSummary()
{
    if (!m_summaryTimer.isActive())
        m_summaryTimer.start();
}

void ServerStore::refreshDisplay()
{
    if (!m_profiles.isEmpty())
        emit dataChanged(index(0), index(int(m_profiles.size()) - 1), {StatusBadgeRole, SearchRole});
    queueSummary();
}

QString ServerStore::filePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/servers.json");
}

int ServerStore::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_profiles.size());
}

QVariant ServerStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_profiles.size())
        return {};
    const ServerProfile &p = m_profiles.at(index.row());
    static const ServerStatus none;
    const auto found = m_status.constFind(p.id);
    const ServerStatus &st = found == m_status.constEnd() ? none : *found; // no copy: this runs for every painted card
    const auto local = m_localConnections.constFind(p.id);
    const bool localConnected = local != m_localConnections.constEnd();
    const bool own = localConnected || (st.state == ServerStatus::Online && st.mine);
    const bool freshForConnection = !localConnected || st.checkedAt > local->connectedAt;
    const auto ownLabel = [&] {
        static const QString pcName = qEnvironmentVariable("COMPUTERNAME");
        if (!pcName.isEmpty())
            return tr("%1 (you)").arg(pcName);
        return tr("This computer");
    };
    const auto ownDetailLabel = [&] {
        const QString base = ownLabel();
        return localConnected && !local->account.isEmpty()
            ? QStringLiteral("%1 · %2").arg(base, local->account) : base;
    };
    switch (role) {
    case Qt::DisplayRole:  return p.name;
    case IdRole:           return p.id;
    case HostRole:         return p.port == 3389 ? p.host : QStringLiteral("%1:%2").arg(p.host).arg(p.port);
    case StatusRole:       return int(localConnected ? ServerStatus::Online : st.state);
    case StatusBadgeRole: {
        if (!own)
            return statusBadge(st);
        const QString label = ownLabel();
        return freshForConnection && st.state == ServerStatus::Online && !st.others.isEmpty()
            ? label + QStringLiteral(" +%1").arg(st.others.size()) : label;
    }
    case UserRole_:        return p.displayUser();
    case SearchRole: {
        QString text = p.name + QLatin1Char(' ') + p.host + QLatin1Char(' ') + p.displayUser();
        for (const RemoteSession &s : st.all) // so searching a nickname finds the server that person is on
            text += QLatin1Char(' ') + sessionOwner(s) + QLatin1Char(' ') + s.client + QLatin1Char(' ') + s.user;
        if (own)
            text += QLatin1Char(' ') + ownLabel();
        return text;
    }
    case CountryRole:      return p.countryHost == p.host ? p.country : QString();
    case StatusColorRole:  return own ? Theme::kGreen : statusColor(st);
    case StatusToolTipRole: {
        const QString detail = freshForConnection || st.occupancy == ServerStatus::OccupancyError
            ? statusToolTip(st) : QString();
        if (!own)
            return detail;
        const QString label = ownDetailLabel();
        return detail.isEmpty() ? label : label + QStringLiteral("\n\n") + detail;
    }
    case OccupancyRole:    return int(st.occupancy);
    case LatencyRole:      return st.latencyMs;
    case SignalRole:       return signalBars(st);
    case OwnerIdleRole:    return st.others.isEmpty() ? -1 : st.others.first().idleMinutes;
    case FavoriteRole:     return p.favorite;
    case GroupRole:        return p.group;
    case OwnerConnectionMinutesRole: return st.others.isEmpty() ? -1 : connectionMinutes(st, st.others.first());
    case OwnSessionRole:   return own;
    default:               return {};
    }
}

void ServerStore::load()
{
    // A missing list comes back from the backup before anything older is looked at.
    if (!QFile::exists(filePath()) && QFile::exists(filePath() + QStringLiteral(".bak")))
        QFile::copy(filePath() + QStringLiteral(".bak"), filePath());
    // One-time copy from the app's old names.
    if (!QFile::exists(filePath())) {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        for (const char *legacy : {"/DeskLink/DeskLink/servers.json", "/RemoteConnect/RemoteConnect/servers.json"}) {
            const QString path = base + QLatin1String(legacy);
            if (QFile::exists(path)) {
                QFile::copy(path, filePath());
                break;
            }
        }
    }

    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray arr = root.value(QLatin1String("servers")).toArray();
    const QJsonArray grpArr = root.value(QLatin1String("groups")).toArray();
    beginResetModel();
    m_profiles.clear();
    m_savedGroups.clear();
    for (const QJsonValue &v : grpArr) {
        const QString g = v.toString().trimmed();
        if (!g.isEmpty() && !m_savedGroups.contains(g, Qt::CaseInsensitive))
            m_savedGroups << g;
    }
    for (const QJsonValue &v : arr) {
        ServerProfile p = ServerProfile::fromJson(v.toObject());
        if (p.isValid())
            m_profiles.append(p);
    }
    rebuildIndex();
    endResetModel();
    queueSummary();
    emit groupsChanged();
}

bool ServerStore::save() const
{
    QJsonArray arr;
    for (const ServerProfile &p : m_profiles)
        arr.append(p.toJson());
    QJsonArray grpArr;
    for (const QString &g : m_savedGroups)
        grpArr.append(g);
    QJsonObject root;
    root[QLatin1String("version")] = 1;
    root[QLatin1String("servers")] = arr;
    if (!grpArr.isEmpty())
        root[QLatin1String("groups")] = grpArr;

    // The last good list stays next to the file, so a lost or damaged servers.json can be recovered.
    const QString backup = filePath() + QStringLiteral(".bak");
    if (QFile::exists(filePath()) && QFileInfo(filePath()).size() > 0) {
        QFile::remove(backup);
        QFile::copy(filePath(), backup);
    }
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

ServerProfile ServerStore::profileById(const QString &id) const
{
    const int i = indexOfId(id);
    return i < 0 ? ServerProfile{} : m_profiles.at(i);
}

bool ServerStore::hasNotes(const QString &id) const
{
    const int i = indexOfId(id);
    return i >= 0 && !m_profiles.at(i).notes.isEmpty();
}

int ServerStore::indexOfId(const QString &id) const
{
    return m_rows.value(id, -1);
}

void ServerStore::add(ServerProfile p)
{
    if (p.id.isEmpty())
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    beginInsertRows({}, static_cast<int>(m_profiles.size()), static_cast<int>(m_profiles.size()));
    m_profiles.append(p);
    m_rows.insert(p.id, int(m_profiles.size()) - 1);
    endInsertRows();
    save();
    queueSummary();
}

void ServerStore::update(const ServerProfile &p)
{
    const int i = indexOfId(p.id);
    if (i < 0)
        return;
    const auto &previous = m_profiles.at(i);
    if (!previous.sameConnection(p) || previous.checkSessions != p.checkSessions || previous.monitor != p.monitor) {
        m_status.remove(p.id);
        queueSummary();
    }
    m_profiles[i] = p;
    emit dataChanged(index(i), index(i));
    save();
}

void ServerStore::importBatch(QList<ServerProfile> toAdd, const QList<ServerProfile> &toUpdate)
{
    if (toAdd.isEmpty() && toUpdate.isEmpty())
        return;
    bool statusCleared = false;
    bool groupAdded = false;
    for (const ServerProfile &p : toUpdate) {
        const int i = indexOfId(p.id);
        if (i < 0)
            continue;
        const auto &previous = m_profiles.at(i);
        if (!previous.sameConnection(p) || previous.checkSessions != p.checkSessions || previous.monitor != p.monitor) {
            m_status.remove(p.id);
            statusCleared = true;
        }
        if (previous.group != p.group)
            groupAdded = true;
        m_profiles[i] = p;
        emit dataChanged(index(i), index(i));
    }
    if (!toAdd.isEmpty()) {
        const int first = int(m_profiles.size());
        const int last = first + int(toAdd.size()) - 1;
        beginInsertRows({}, first, last);
        m_profiles.reserve(m_profiles.size() + toAdd.size());
        for (ServerProfile &p : toAdd) {
            if (p.id.isEmpty())
                p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!p.group.isEmpty())
                groupAdded = true;
            m_rows.insert(p.id, int(m_profiles.size()));
            m_profiles.append(std::move(p));
        }
        endInsertRows();
    }
    save();
    if (!toAdd.isEmpty() || statusCleared)
        queueSummary();
    if (groupAdded)
        emit groupsChanged();
}

void ServerStore::remove(const QString &id)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    beginRemoveRows({}, i, i);
    m_profiles.removeAt(i);
    m_status.remove(id);
    m_localConnections.remove(id);
    rebuildIndex();
    endRemoveRows();
    save();
    queueSummary();
}

void ServerStore::setStatus(const QString &id, const ServerStatus &s)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    auto it = m_status.constFind(id);
    if (it != m_status.constEnd() && *it == s)
        return;
    bool visibleChange = true;
    if (it != m_status.constEnd()) {
        ServerStatus previous = *it;
        const qint64 previousAge = previous.others.isEmpty() ? -1 : connectionMinutes(previous, previous.others.first());
        const qint64 age = s.others.isEmpty() ? -1 : connectionMinutes(s, s.others.first());
        previous.checkedAt = s.checkedAt;
        previous.serverNowUtcMs = s.serverNowUtcMs;
        // Repaint for a changed minute, rather than every new timestamp behind the same text.
        const auto local = m_localConnections.constFind(id);
        const bool firstFreshSample = local != m_localConnections.constEnd()
            && it->checkedAt <= local->connectedAt && s.checkedAt > local->connectedAt;
        visibleChange = firstFreshSample || previousAge != age || previous != s;
    }
    m_status.insert(id, s);
    if (!visibleChange)
        return;
    static const QList<int> kStatusRoles = {
        StatusRole, StatusBadgeRole, StatusColorRole, StatusToolTipRole,
        OccupancyRole, LatencyRole, SignalRole, OwnerIdleRole,
        OwnerConnectionMinutesRole, OwnSessionRole, SearchRole,
    };
    emit dataChanged(index(i), index(i), kStatusRoles);
    emit statusChanged(id);
    queueSummary();
}

bool ServerStore::setLocalConnection(const QString &id, bool connected, const QString &account)
{
    const int row = indexOfId(id);
    if (row < 0)
        return false;
    if (connected) {
        const auto previous = m_localConnections.constFind(id);
        if (previous != m_localConnections.constEnd() && previous->account == account)
            return false;
        m_localConnections.insert(id, {account, QDateTime::currentMSecsSinceEpoch()});
    } else {
        if (!m_localConnections.remove(id))
            return false;
        const auto status = m_status.find(id);
        if (status != m_status.end())
            status->mine = false; // A fresh server query can still find a session opened in another client.
    }
    emit dataChanged(index(row), index(row), {StatusRole, StatusBadgeRole, StatusColorRole, StatusToolTipRole,
                                            OwnSessionRole, SearchRole});
    emit statusChanged(id);
    queueSummary();
    return true;
}

ServerStore::Counts ServerStore::counts() const
{
    Counts c;
    c.total = int(m_profiles.size());
    static const ServerStatus none;
    for (const ServerProfile &p : m_profiles) {
        const auto found = m_status.constFind(p.id);
        const ServerStatus &st = found == m_status.constEnd() ? none : *found;
        const bool local = m_localConnections.contains(p.id);
        if (local || st.state == ServerStatus::Online) {
            ++c.online;
            if (local || st.mine || st.occupancy == ServerStatus::Busy)
                ++c.busy;
            else if (st.occupancy == ServerStatus::Free)
                ++c.free;
        } else if (st.state == ServerStatus::Offline) {
            ++c.offline;
        }
    }
    return c;
}

void ServerStore::setCountry(const QString &id, const QString &host, const QString &country)
{
    const int i = indexOfId(id);
    if (i < 0 || m_profiles[i].host != host)
        return; // edited meanwhile
    ServerProfile &p = m_profiles[i];
    if (p.countryHost == host && p.country == country)
        return;
    p.country = country;
    p.countryHost = host;
    emit dataChanged(index(i), index(i));
    scheduleSave();
}

void ServerStore::setFavorite(const QString &id, bool favorite)
{
    const int i = indexOfId(id);
    if (i < 0 || m_profiles[i].favorite == favorite)
        return;
    m_profiles[i].favorite = favorite;
    emit dataChanged(index(i), index(i));
    scheduleSave();
    queueSummary();
}

int ServerStore::favoriteCount() const
{
    int n = 0;
    for (const ServerProfile &p : m_profiles)
        n += p.favorite;
    return n;
}

void ServerStore::setGroup(const QString &id, const QString &group)
{
    const int i = indexOfId(id);
    const QString name = group.trimmed();
    if (i < 0 || m_profiles[i].group == name)
        return;
    m_profiles[i].group = name;
    if (!name.isEmpty() && !m_savedGroups.contains(name, Qt::CaseInsensitive))
        m_savedGroups << name;
    emit dataChanged(index(i), index(i));
    emit groupsChanged();
    scheduleSave();
}

void ServerStore::addGroup(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty())
        return;
    for (const QString &existing : groups())
        if (existing.compare(clean, Qt::CaseInsensitive) == 0)
            return;
    m_savedGroups << clean;
    emit groupsChanged();
    scheduleSave();
}

void ServerStore::renameGroup(const QString &from, const QString &to)
{
    const QString name = to.trimmed();
    if (from.isEmpty() || name == from)
        return;
    bool changed = false;
    if (m_savedGroups.removeAll(from) > 0)
        changed = true;
    if (!name.isEmpty() && !m_savedGroups.contains(name, Qt::CaseInsensitive)) {
        m_savedGroups << name;
        changed = true;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].group != from)
            continue;
        m_profiles[i].group = name;
        emit dataChanged(index(i), index(i));
        changed = true;
    }
    if (changed) {
        emit groupsChanged();
        scheduleSave();
    }
}

QStringList ServerStore::groups() const
{
    QStringList names;
    for (const QString &g : m_savedGroups)
        if (!g.isEmpty() && !names.contains(g, Qt::CaseInsensitive))
            names << g;
    for (const ServerProfile &p : m_profiles)
        if (!p.group.isEmpty() && !names.contains(p.group, Qt::CaseInsensitive))
            names << p.group;
    names.sort(Qt::CaseInsensitive);
    return names;
}

int ServerStore::groupServerCount(const QString &group) const
{
    int count = 0;
    for (const ServerProfile &p : m_profiles)
        if (p.group.compare(group, Qt::CaseInsensitive) == 0)
            ++count;
    return count;
}
