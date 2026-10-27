#pragma once
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QToolButton;
class ServerPanel;
class ServerStore;

class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(ServerStore *store, QWidget *parent = nullptr);

    ServerPanel *panel() const { return m_panel; }

protected:
    void changeEvent(QEvent *e) override;

private:
    void updateSummary();
    void showSettingsMenu();
    void showAbout();

    ServerStore *m_store;
    ServerPanel *m_panel;
    QLabel *m_title;
    QLabel *m_summary;
    QToolButton *m_settings;
    QHBoxLayout *m_brandLayout = nullptr;
};
