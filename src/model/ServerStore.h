#pragma once
#include "ServerProfile.h"
#include "ServerStatus.h"

#include <QAbstractListModel>
#include <QHash>

class ServerStore : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        HostRole,
        ColorRole,
        StatusRole, // ServerStatus::State
        StatusTextRole,
        UserRole_, // domain\user
        SearchRole,
        CountryRole, // ISO code, empty if unknown
    };

    explicit ServerStore(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void load();
    bool save() const;

    QList<ServerProfile> all() const { return m_profiles; }
    ServerProfile profileAt(int row) const { return m_profiles.value(row); }
    ServerProfile profileById(const QString &id) const;
    int indexOfId(const QString &id) const;

    void add(ServerProfile p);
    void update(const ServerProfile &p);
    void remove(const QString &id);

    ServerStatus status(const QString &id) const { return m_status.value(id); }
    void setStatus(const QString &id, const ServerStatus &s);

    int countWithState(ServerStatus::State s) const;
    QStringList usedColors() const;
    void setCountry(const QString &id, const QString &host, const QString &country);

signals:
    void statusChanged(const QString &id);

private:
    static QString filePath();

    QList<ServerProfile> m_profiles;
    QHash<QString, ServerStatus> m_status;
};
