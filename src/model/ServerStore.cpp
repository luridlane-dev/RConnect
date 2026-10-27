#include "ServerStore.h"

#include <QDir>
#include <QFile>
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
    switch (role) {
    case Qt::DisplayRole:  return p.name;
    case IdRole:           return p.id;
    case HostRole:         return p.port == 3389 ? p.host : QStringLiteral("%1:%2").arg(p.host).arg(p.port);
    case ColorRole:        return p.color;
    case StatusRole:       return int(m_status.value(p.id).state);
    case StatusBadgeRole:  return statusBadge(m_status.value(p.id));
    case UserRole_:        return p.displayUser();
    case SearchRole:       return p.name + QLatin1Char(' ') + p.host + QLatin1Char(' ') + p.displayUser();
    case CountryRole:      return p.countryHost == p.host ? p.country : QString();
    case StatusColorRole:  return statusColor(m_status.value(p.id));
    case StatusToolTipRole: return statusToolTip(m_status.value(p.id));
    case OccupancyRole:    return int(m_status.value(p.id).occupancy);
    case LatencyRole:      return m_status.value(p.id).latencyMs;
    case SignalRole:       return signalBars(m_status.value(p.id));
    case OwnerIdleRole: {
        const ServerStatus st = m_status.value(p.id);
        return st.others.isEmpty() ? -1 : st.others.first().idleMinutes;
    }
    default:               return {};
    }
}

void ServerStore::load()
{
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
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("servers")).toArray();
    beginResetModel();
    m_profiles.clear();
    for (const QJsonValue &v : arr) {
        ServerProfile p = ServerProfile::fromJson(v.toObject());
        if (p.isValid())
            m_profiles.append(p);
    }
    rebuildIndex();
    endResetModel();
    queueSummary();
}

bool ServerStore::save() const
{
    QJsonArray arr;
    for (const ServerProfile &p : m_profiles)
        arr.append(p.toJson());
    QJsonObject root;
    root[QLatin1String("version")] = 1;
    root[QLatin1String("servers")] = arr;

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
    m_profiles[i] = p;
    emit dataChanged(index(i), index(i));
    save();
}

void ServerStore::remove(const QString &id)
{
    const int i = indexOfId(id);
    if (i < 0)
        return;
    beginRemoveRows({}, i, i);
    m_profiles.removeAt(i);
    m_status.remove(id);
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
    m_status.insert(id, s);
    emit dataChanged(index(i), index(i));
    emit statusChanged(id);
    queueSummary();
}

ServerStore::Counts ServerStore::counts() const
{
    Counts c;
    c.total = int(m_profiles.size());
    for (const ServerProfile &p : m_profiles) {
        const ServerStatus st = m_status.value(p.id);
        if (st.state == ServerStatus::Online) {
            ++c.online;
            if (st.occupancy == ServerStatus::Busy)
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

QStringList ServerStore::usedColors() const
{
    QStringList colors;
    for (const ServerProfile &p : m_profiles)
        colors << p.color.toLower();
    return colors;
}
