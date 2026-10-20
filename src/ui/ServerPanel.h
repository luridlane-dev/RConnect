#pragma once
#include <QSet>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSortFilterProxyModel;
class ServerDelegate;
class ServerStore;

class ServerPanel : public QWidget {
    Q_OBJECT
public:
    explicit ServerPanel(ServerStore *store, QWidget *parent = nullptr);

    void setOpenIds(const QSet<QString> &ids);
    void setFooterVisible(bool visible);
    void focusSearch();
    void clearSearch();

    // "3 servers · 2 online · 1 offline"
    static QString summary(const ServerStore *store);

signals:
    void openRequested(const QString &id);
    void addRequested();
    void editRequested(const QString &id);
    void removeRequested(const QString &id);
    void refreshRequested(const QString &id);

protected:
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void showContextMenu(const QPoint &pos);
    void updateFooter();
    void openFirstMatch();

    ServerStore *m_store;
    QSortFilterProxyModel *m_proxy;
    QLineEdit *m_search;
    QPushButton *m_add;
    QListView *m_list;
    ServerDelegate *m_delegate;
    QLabel *m_footer;
};
