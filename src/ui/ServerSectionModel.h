#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

// Flattens the sorted server list into "header, cards, header, cards": one header per group. Headers only appear
// once at least one server has a group, and a collapsed group keeps just its header.
class ServerSectionModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        HeaderRole = Qt::UserRole + 100, // bool: this row is a group header
        SectionGroupRole,                // QString: name of the header's group, empty for the servers without one
        SectionCountRole,                // servers in the group (before collapsing)
        SectionFreeRole,                 // of those, online and nobody on them
        SectionCollapsedRole,
    };

    // `source` must be sorted by group; it is not owned.
    explicit ServerSectionModel(QAbstractItemModel *source, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void toggleCollapsed(const QString &group);

private:
    struct Entry {
        int sourceRow = -1; // -1: a header
        QString id;         // of the server on a card
        QString group;
        int count = 0;
        int free = 0;
        bool collapsed = false;
    };

    // One pass over the source: the group of each row, and the per-group totals.
    struct Totals {
        int count = 0;
        int free = 0;
    };
    struct Snapshot {
        QVector<QString> group;
        QVector<QString> id;
        QHash<QString, Totals> totals;
    };
    Snapshot snapshot(bool withIds) const;

    void rebuild();
    void refreshHeaders();
    void queueHeaderRefresh();
    void onDataChanged(const QModelIndex &first, const QModelIndex &last);
    void saveCollapsed() const;

    QAbstractItemModel *m_source;
    QVector<Entry> m_entries;
    QVector<int> m_rowOfSource; // source row -> row here, -1 while collapsed
    QVector<QString> m_groupOfSource; // as of the last rebuild
    QSet<QString> m_collapsed;  // group names
    bool m_headersQueued = false;
};
