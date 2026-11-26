#pragma once
#include "ServerProfile.h"
#include "ServerStatus.h"

#include <QAbstractListModel>
#include <QHash>
#include <QTimer>

class ServerStore : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        HostRole,
        StatusRole, // ServerStatus::State
        StatusBadgeRole, // short text of the status badge
        UserRole_, // domain\user
        SearchRole,
        CountryRole, // ISO code, empty if unknown
        StatusColorRole,
        StatusToolTipRole,
        OccupancyRole, // ServerStatus::Occupancy
        LatencyRole,   // ms, -1 if unknown
        SignalRole,    // 0..4
        OwnerIdleRole, // minutes the connected user has been idle, -1 if nobody
        FavoriteRole,  // bool
        GroupRole,     // group name, empty if none
        OwnerConnectionMinutesRole, // minutes since the current connection started, -1 if unknown
        OwnSessionRole, // bool: this PC has a connected session
    };

    explicit ServerStore(QObject *parent = nullptr);
    ~ServerStore() override;

    struct Counts {
        int total = 0, online = 0, offline = 0, free = 0, busy = 0;
    };

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void load();
    bool save() const;

    QList<ServerProfile> all() const { return m_profiles; }
    ServerProfile profileAt(int row) const { return m_profiles.value(row); }
    ServerProfile profileById(const QString &id) const;
    bool hasNotes(const QString &id) const;
    int indexOfId(const QString &id) const;

    void add(ServerProfile p);
    void update(const ServerProfile &p);
    void importBatch(QList<ServerProfile> toAdd, const QList<ServerProfile> &toUpdate);
    void remove(const QString &id);

    ServerStatus status(const QString &id) const { return m_status.value(id); }
    void setStatus(const QString &id, const ServerStatus &s);
    bool setLocalConnection(const QString &id, bool connected, const QString &account = {});

    Counts counts() const;
    void setCountry(const QString &id, const QString &host, const QString &country);
    void setFavorite(const QString &id, bool favorite);
    int favoriteCount() const;
    void setGroup(const QString &id, const QString &group);
    // Adds a group name so it is available even before servers are placed in it.
    void addGroup(const QString &name);
    // Gives every server of a group another name, or removes the group when `to` is empty.
    void renameGroup(const QString &from, const QString &to);
    // All group names (saved and in use), sorted.
    QStringList groups() const;
    int groupServerCount(const QString &group) const;
    // The nicknames in the contacts changed: the cards show other text.
    void refreshDisplay();

signals:
    void statusChanged(const QString &id);
    // Rows or statuses changed; sent at most a few times a second however many servers there are.
    void summaryChanged();
    void groupsChanged();

private:
    struct LocalConnection {
        QString account;
        qint64 connectedAt;
    };

    static QString filePath();
    void rebuildIndex();
    void scheduleSave();
    void queueSummary();

    QList<ServerProfile> m_profiles;
    QStringList m_savedGroups;
    QHash<QString, ServerStatus> m_status;
    QHash<QString, LocalConnection> m_localConnections;
    QHash<QString, int> m_rows;
    QTimer m_saveTimer;
    QTimer m_summaryTimer;
};
