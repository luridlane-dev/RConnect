#pragma once
#include <QWidget>

class AccessLogPage;
class LiveSessionsPage;
class QButtonGroup;
class QFrame;
class QPropertyAnimation;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QStackedWidget;
class ServerPanel;
class ServerStore;
class UsagePage;

// The start page: a navigation bar on the left, and the server list, the live sessions or the
// access log on the right.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(ServerStore *store, QWidget *parent = nullptr);

    ServerPanel *panel() const { return m_panel; }
    LiveSessionsPage *liveSessions() const { return m_live; }
    AccessLogPage *accessLog() const { return m_log; }

signals:
    void updateCheckRequested();
    void importRequested();
    void exportRequested();

protected:
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void retranslate();
    void updateSummary();
    void showSettings();
    void showAbout();
    void movePill(bool animate);
    void applyPages();

    ServerStore *m_store;
    ServerPanel *m_panel;
    LiveSessionsPage *m_live;
    UsagePage *m_usage;
    AccessLogPage *m_log;
    QStackedWidget *m_pages;
    QButtonGroup *m_nav;
    QLabel *m_title;
    QPushButton *m_settings;
    QHBoxLayout *m_brandLayout = nullptr;
    QFrame *m_navPill = nullptr;       // the highlight behind the current page's button
    QPropertyAnimation *m_pillAnim = nullptr;
    bool m_shown = false;
};
