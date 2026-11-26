#pragma once
#include "core/AccessLog.h"

#include <QHash>
#include <QPair>
#include <QStringList>
#include <QWidget>

class AccessTableModel;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;
class ServerStore;

// One row of the table: an entry of the file, or grouped failed attempts from one address.
struct AccessLine {
    AccessRecord r;          // the newest entry
    QString firstUtc;        // the oldest, for a run of attempts
    int attempts = 1;
    QStringList users;       // accounts tried
    QStringList clients;     // computer names the attempts came from
    QStringList reasons;     // why they failed, as the server logged it
};

// Activity log table: sign-ins, disconnections, failed attempts, alerts, and server changes.
class AccessLogPage : public QWidget {
    Q_OBJECT
public:
    explicit AccessLogPage(ServerStore *store, QWidget *parent = nullptr);
    void setSyncState(const QString &id, const QString &name, bool running,
                      const QStringList &failedChannels, const QString &error);

    static QString eventText(const QString &event);
    static QString sourceText(const QString &source);
    static QString alertSummary(const AccessRecord &r);

signals:
    void syncRequested(const QString &serverId);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void reload();
    void apply(QList<AccessLine> lines);
    void showDetail();
    void updateCount();
    void fillServers();
    void applyPeriod();
    void exportCsv();
    void updateSyncStatus();

    struct SyncState {
        QString name;
        bool running = false;
        QStringList failedChannels;
        QString error;
    };
    ServerStore *m_store;
    QHash<QString, SyncState> m_syncStates;
    AccessTableModel *m_model;
    QTableView *m_view;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLineEdit *m_filter;
    QComboBox *m_category;
    QComboBox *m_server;
    QComboBox *m_period;
    QPushButton *m_showFailed;
    QLabel *m_count;
    QLabel *m_syncStatus;
    QTimer *m_timer;
    QTimer *m_syncReloadTimer = nullptr;
    QPushButton *m_refresh;
    QPushButton *m_export;
    QPair<qint64, qint64> m_stamp;
    bool m_loaded = false;
    bool m_busy = false;
    bool m_again = false;
    QList<AccessLine> m_lines;
    QFrame *m_detail = nullptr;
    QLabel *m_detailText = nullptr;
};
