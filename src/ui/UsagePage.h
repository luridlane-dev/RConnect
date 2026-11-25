#pragma once
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QTimer;
class QTreeView;
namespace UsageEngine { struct Report; }

// Server connection and active time per user (see UsageLog and UsageEngine). One row per user with
// their deduplicated active time over all servers; open it for each server (or each day), and under
// that every connection with its start and end.
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
    void showReport(const UsageEngine::Report &report);
    void rebuild();
    void explain();

    QStandardItemModel *m_model;
    QTreeView *m_view;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLabel *m_warning;
    QLabel *m_total;
    QLabel *m_metricLabels[4] = {};
    QLabel *m_metricValues[4] = {};
    QComboBox *m_range;
    QComboBox *m_breakdown;
    QPushButton *m_refresh;
    QPushButton *m_configure;
    QPushButton *m_rebuild;
    QPushButton *m_help;
    QTimer *m_timer;
    bool m_reading = false;
    bool m_readAgain = false;
};
