#pragma once
#include <QSet>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class ServerDelegate;
class ServerProxy;
class ServerSectionModel;
class ServerStore;

// Search, filter and the grid of server cards. Built for lists of a hundred servers.
class ServerPanel : public QWidget {
    Q_OBJECT
public:
    // Left and right inset, also used by the page title so everything lines up with the cards.
    static constexpr int kInset = 6;
    // Space right of the last card column, which the search row and the page title also leave free.
    int rightInset() const { return m_rightInset; }

    explicit ServerPanel(ServerStore *store, QWidget *parent = nullptr);

    void setOpenIds(const QSet<QString> &ids);
    void focusSearch();
    // The popup next to the session tabs has no import button.
    void setImportVisible(bool visible);
    void clearSearch();

    // "3 servers · 2 available · 1 in use · 0 offline"
    static QString summary(const ServerStore *store);

signals:
    void favoriteToggled(const QString &id);
    void rightInsetChanged(int inset);
    void openRequested(const QString &id);
    void addRequested();
    void importRequested();
    void editRequested(const QString &id);
    void removeRequested(const QString &id);
    void refreshRequested(const QString &id);
    void auditRequested(const QString &id);

protected:
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private:
    void retranslate();
    void showContextMenu(const QPoint &pos);
    void setListMode(bool list);
    void updateCounts();
    void updateEmptyState();
    void fitCards();
    void openFirstMatch();

    ServerStore *m_store;
    ServerProxy *m_proxy;
    ServerSectionModel *m_sections;
    QButtonGroup *m_viewGroup = nullptr;
    QLineEdit *m_search;
    QPushButton *m_add;
    QPushButton *m_import;
    QButtonGroup *m_filters;
    QListView *m_list;
    ServerDelegate *m_delegate;
    QLabel *m_empty;
    QHBoxLayout *m_headLayout = nullptr;
    QHBoxLayout *m_filterLayout = nullptr;
    int m_rightInset = kInset;
    int m_columns = 0;
    bool m_fitted = false;
};
