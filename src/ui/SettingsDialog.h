#pragma once
#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class ServerStore;

// The program's settings in a window of their own: a few plain sections, each a list of rows.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ServerStore *store, QWidget *parent = nullptr);

signals:
    void importRequested();
    void exportRequested();
    void updateCheckRequested();
    void aboutRequested();
    void pagesChanged();

protected:
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void retranslate();
    void rebuildGroups();

    ServerStore *m_store;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QStringList m_shownGroups; // what the group rows show; rebuilt only when this changes
    bool m_fitted = false;
    QVBoxLayout *m_groupRows = nullptr;
    QLabel *m_groupHint = nullptr;
    QLabel *m_headings[6] = {};
    QLabel *m_labels[4] = {};
    QLabel *m_hints[3] = {};
    QComboBox *m_language = nullptr;
    QComboBox *m_theme = nullptr;
    QComboBox *m_buttons = nullptr;
    QCheckBox *m_autostart = nullptr;
    QCheckBox *m_pages[3] = {};
    QCheckBox *m_detailed = nullptr;
    QCheckBox *m_autoUpdate = nullptr;
    QPushButton *m_logFile = nullptr;
    QPushButton *m_logFolder = nullptr;
    QPushButton *m_contacts = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_check = nullptr;
    QPushButton *m_about = nullptr;
    QPushButton *m_close = nullptr;
};
