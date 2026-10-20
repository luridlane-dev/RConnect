#include "ServerPanel.h"

#include "ServerDelegate.h"
#include "model/ServerStore.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>

ServerPanel::ServerPanel(ServerStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("serverPanel"));

    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(store);
    m_proxy->setFilterRole(ServerStore::SearchRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(true);

    m_search = new QLineEdit(this);
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_proxy->setFilterFixedString(t.trimmed());
        updateFooter();
    });
    connect(m_search, &QLineEdit::returnPressed, this, &ServerPanel::openFirstMatch);

    m_add = new QPushButton(this);
    m_add->setCursor(Qt::PointingHandCursor);
    connect(m_add, &QPushButton::clicked, this, &ServerPanel::addRequested);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(16, 0, 16, 0);
    head->setSpacing(10);
    head->addWidget(m_search, 1);
    head->addWidget(m_add);

    m_list = new QListView(this);
    m_list->setModel(m_proxy);
    m_delegate = new ServerDelegate(m_list);
    m_list->setItemDelegate(m_delegate);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSpacing(6);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setCursor(Qt::PointingHandCursor);
    m_list->setFrameShape(QFrame::NoFrame);
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &i) {
        emit openRequested(i.data(ServerStore::IdRole).toString());
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, &ServerPanel::showContextMenu);

    m_footer = new QLabel(this);
    m_footer->setObjectName(QStringLiteral("panelFooter"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 14, 0, 8);
    lay->setSpacing(10);
    lay->addLayout(head);
    auto *listRow = new QHBoxLayout;
    listRow->setContentsMargins(10, 0, 10, 0);
    listRow->addWidget(m_list);
    lay->addLayout(listRow, 1);
    lay->addWidget(m_footer);

    connect(store, &ServerStore::statusChanged, this, &ServerPanel::updateFooter);
    connect(store, &QAbstractItemModel::rowsInserted, this, &ServerPanel::updateFooter);
    connect(store, &QAbstractItemModel::rowsRemoved, this, &ServerPanel::updateFooter);
    connect(store, &QAbstractItemModel::modelReset, this, &ServerPanel::updateFooter);
    retranslate();
}

QString ServerPanel::summary(const ServerStore *store)
{
    const int total = store->rowCount();
    if (total == 0)
        return tr("No servers yet. Use “+ Add server” to get started.");
    return tr("%1 · %2 online · %3 offline")
        .arg(total == 1 ? tr("1 server") : tr("%1 servers").arg(total))
        .arg(store->countWithState(ServerStatus::Online))
        .arg(store->countWithState(ServerStatus::Offline));
}

void ServerPanel::retranslate()
{
    m_search->setPlaceholderText(tr("Search servers..."));
    m_add->setText(tr("+ Add server"));
    updateFooter();
    m_list->viewport()->update();
}

void ServerPanel::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

void ServerPanel::setOpenIds(const QSet<QString> &ids)
{
    m_delegate->setOpenIds(ids);
    m_list->viewport()->update();
}

void ServerPanel::setFooterVisible(bool visible)
{
    m_footer->setVisible(visible);
    updateFooter();
}

void ServerPanel::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

void ServerPanel::clearSearch()
{
    m_search->clear();
}

void ServerPanel::openFirstMatch()
{
    if (m_proxy->rowCount() > 0)
        emit openRequested(m_proxy->index(0, 0).data(ServerStore::IdRole).toString());
}

void ServerPanel::showContextMenu(const QPoint &pos)
{
    const QModelIndex i = m_list->indexAt(pos);
    if (!i.isValid())
        return;
    const QString id = i.data(ServerStore::IdRole).toString();

    QMenu menu(this);
    menu.addAction(tr("Connect"), this, [this, id] { emit openRequested(id); });
    menu.addAction(tr("Check status now"), this, [this, id] { emit refreshRequested(id); });
    menu.addSeparator();
    menu.addAction(tr("Edit…"), this, [this, id] { emit editRequested(id); });
    menu.addAction(tr("Delete…"), this, [this, id] { emit removeRequested(id); });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void ServerPanel::updateFooter()
{
    if (m_footer->isHidden())
        return;
    QString text = summary(m_store);
    const int shown = m_proxy->rowCount();
    if (m_store->rowCount() > 0 && shown != m_store->rowCount())
        text += QStringLiteral("  ·  ") + (shown == 1 ? tr("1 result") : tr("%1 results").arg(shown));
    m_footer->setText(text);
}
