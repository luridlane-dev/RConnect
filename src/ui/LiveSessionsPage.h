#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QTimer;
class ServerStore;

// Everyone who is logged on to the servers right now.
class LiveSessionsPage : public QWidget {
    Q_OBJECT
public:
    explicit LiveSessionsPage(ServerStore *store, QWidget *parent = nullptr);

signals:
    void refreshRequested();
    // Show a message on the screen of the selected session.
    void messageRequested(const QString &serverId, int sessionId, const QString &user);

protected:
    void showEvent(QShowEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void reload();
    void requestMessage();

    ServerStore *m_store;
    QStandardItemModel *m_model;
    QTableView *m_view;
    QLabel *m_title;
    QLabel *m_subtitle;
    QPushButton *m_refresh;
    QPushButton *m_message;
    bool m_dirty = false;
    QTimer *m_reloadTimer = nullptr;
    qint64 m_lastRefresh = 0;
};
