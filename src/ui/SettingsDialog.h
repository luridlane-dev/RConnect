#pragma once
#include "core/LocalCache.h"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class ServerStore;

// Settings split into categories with a shared close button.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ServerStore *store, QWidget *parent = nullptr);

signals:
    void importRequested();
    void exportRequested();
    void configureServersRequested();
    void updateCheckRequested();
    void aboutRequested();
    void pagesChanged();
    void serverLayoutChanged();

protected:
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void retranslate();
    void rebuildGroups();
    void scanCache(bool clear);
    void updateCacheStatus();

    ServerStore *m_store;
    QListWidget *m_navigation = nullptr;
    QStringList m_shownGroups; // what the group rows show; rebuilt only when this changes
    bool m_fitted = false;
    QVBoxLayout *m_groupRows = nullptr;
    QLineEdit *m_newGroup = nullptr;
    QPushButton *m_addGroup = nullptr;
    QLabel *m_groupHint = nullptr;
    QLabel *m_titles[4] = {};
    QLabel *m_pagesHeading = nullptr;
    QLabel *m_groupHeading = nullptr;
    QLabel *m_adminHeading = nullptr;
    QLabel *m_adminHint = nullptr;
    QLabel *m_sortingLabel = nullptr;
    QLabel *m_logHeading = nullptr;
    QLabel *m_cacheHeading = nullptr;
    QLabel *m_cacheHint = nullptr;
    QLabel *m_cacheStatus = nullptr;
    QLabel *m_labels[3] = {};
    QLabel *m_hints[2] = {};
    QComboBox *m_language = nullptr;
    QComboBox *m_theme = nullptr;
    QComboBox *m_buttons = nullptr;
    QComboBox *m_sorting = nullptr;
    QCheckBox *m_autostart = nullptr;
    QCheckBox *m_pages[2] = {};
    QCheckBox *m_detailed = nullptr;
    QCheckBox *m_autoUpdate = nullptr;
    QPushButton *m_logFile = nullptr;
    QPushButton *m_logFolder = nullptr;
    QPushButton *m_clearCache = nullptr;
    QPushButton *m_contacts = nullptr;
    QPushButton *m_import = nullptr;
    QPushButton *m_export = nullptr;
    QPushButton *m_configureServers = nullptr;
    QPushButton *m_check = nullptr;
    QPushButton *m_about = nullptr;
    QPushButton *m_close = nullptr;
    LocalCache::Result m_cache;
    bool m_cacheBusy = false;
    bool m_cacheClearing = false;
};
