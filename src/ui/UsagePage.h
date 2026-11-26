#pragma once
#include "core/UsageEngine.h"

#include <QDate>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class QTimer;
class QTreeView;

// Daily server connection, active, and idle time per user.
class UsagePage : public QWidget {
    Q_OBJECT
public:
    explicit UsagePage(QWidget *parent = nullptr);

public slots:
    void onServerSynced(const QString &serverId, const QString &serverName, bool busy,
                        const QStringList &failedSources, const QString &error);
    void setConfiguringServers(bool busy);

signals:
    void syncRequested(const QString &serverId);
    void configureServersRequested();

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void reload();
    void renderCurrent();
    void showReport(const UsageEngine::Report &report);
    void showContextMenu(const QPoint &pos);
    void updateContactButton();
    void editSelectedContact();

    QStandardItemModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_total = nullptr;
    QLabel *m_metricLabels[4] = {};
    QLabel *m_metricValues[4] = {};
    QComboBox *m_range = nullptr;
    QLineEdit *m_search = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_addContact = nullptr;
    QPushButton *m_configure = nullptr;
    QTimer *m_timer = nullptr;
    QTimer *m_searchTimer = nullptr;
    QTimer *m_syncReloadTimer = nullptr;
    UsageEngine::Report m_lastReport;
    QList<UsageEngine::Person> m_cachedPeople;
    int m_cachedRange = -1;
    QDate m_cachedToday;
    bool m_peopleValid = false;
    QString m_activeMetricStyle;
    QString m_idleMetricStyle;
    bool m_hasReport = false;
    bool m_reading = false;
    bool m_readAgain = false;
};
