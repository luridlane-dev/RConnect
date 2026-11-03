#include "LiveSessionsPage.h"

#include "ContactsDialog.h"
#include "Theme.h"

#include "core/Contacts.h"

#include "model/ServerStore.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QTableView>
#include <QVBoxLayout>

namespace {

enum Column { Server, User, From, State, Idle, ColumnCount };

constexpr int kServerIdRole = Qt::UserRole + 1;
constexpr int kSessionIdRole = Qt::UserRole + 2;
constexpr int kConnectedRole = Qt::UserRole + 3;
constexpr int kClientRole = Qt::UserRole + 4;
constexpr int kUserRole = Qt::UserRole + 5;

} // namespace

LiveSessionsPage::LiveSessionsPage(ServerStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    m_model = new QStandardItemModel(0, ColumnCount, this);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);

    m_refresh = new QPushButton(this);
    connect(m_refresh, &QPushButton::clicked, this, &LiveSessionsPage::refreshRequested);

    m_message = new QPushButton(this);
    m_message->setEnabled(false);
    connect(m_message, &QPushButton::clicked, this, &LiveSessionsPage::requestMessage);

    auto *head = new QHBoxLayout;
    head->addLayout(titles, 1);
    head->addWidget(m_message, 0, Qt::AlignTop);
    head->addWidget(m_refresh, 0, Qt::AlignTop);

    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setDefaultSectionSize(34);
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setHighlightSections(false);
    m_view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_view->setColumnWidth(Server, 200);
    m_view->setColumnWidth(User, 200);
    m_view->setColumnWidth(From, 220);
    m_view->setColumnWidth(State, 140);
    m_view->setColumnWidth(Idle, 200);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(14);
    lay->addLayout(head);
    lay->addWidget(m_view, 1);

    // Only a signed-in session can be sent a message; double-clicking a row does it too.
    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex &row) {
        m_message->setEnabled(row.isValid() && row.sibling(row.row(), State).data(kConnectedRole).toBool());
    });
    connect(m_view, &QTableView::doubleClicked, this, [this] { requestMessage(); });
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_view->indexAt(pos);
        if (!index.isValid())
            return;
        const QString client = index.sibling(index.row(), Server).data(kClientRole).toString();
        const QString user = index.sibling(index.row(), Server).data(kUserRole).toString();
        QMenu menu(this);
        const bool known = Contacts::instance().match(client, user) != nullptr;
        if (!known && client.isEmpty())
            return; // nothing to tell this person by
        menu.addAction(known ? tr("Edit contact…") : tr("Add to contacts…"), this,
                       [this, client, user] { ContactsDialog::editFor(client, user, this); });
        menu.exec(m_view->viewport()->mapToGlobal(pos));
    });
    connect(&Contacts::instance(), &Contacts::changed, this, &LiveSessionsPage::reload);

    // Statuses of many servers arrive one by one: the table is rebuilt once a second at most.
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(1000);
    connect(m_reloadTimer, &QTimer::timeout, this, &LiveSessionsPage::reload);
    connect(store, &ServerStore::summaryChanged, this, [this] {
        if (isVisible()) {
            if (!m_reloadTimer->isActive())
                m_reloadTimer->start();
        } else {
            m_dirty = true;
        }
    });
    retranslate();
    reload();
}

void LiveSessionsPage::retranslate()
{
    m_title->setText(tr("Who's connected"));
    m_refresh->setText(tr("Refresh"));
    m_message->setText(tr("Send message…"));
    m_message->setToolTip(tr("Show a message on the screen of the selected person"));
    m_model->setHorizontalHeaderLabels({tr("Server"), tr("User"), tr("Connected from"), tr("Status"), tr("Activity")});
    reload();
}

void LiveSessionsPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_dirty)
        reload();
    // The shown sessions can be a minute old; but not on every tab switch.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastRefresh >= 15000) {
        m_lastRefresh = now;
        emit refreshRequested();
    }
}

void LiveSessionsPage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

void LiveSessionsPage::requestMessage()
{
    const QModelIndex row = m_view->currentIndex();
    if (!row.isValid() || !row.sibling(row.row(), State).data(kConnectedRole).toBool())
        return;
    const QModelIndex first = row.sibling(row.row(), Server);
    emit messageRequested(first.data(kServerIdRole).toString(), first.data(kSessionIdRole).toInt(),
                          row.sibling(row.row(), User).data().toString());
}

void LiveSessionsPage::reload()
{
    m_dirty = false;
    // Keep the selection across refreshes.
    QString keepServer;
    int keepSession = -1;
    const QModelIndex current = m_view->currentIndex();
    if (current.isValid()) {
        keepServer = current.sibling(current.row(), Server).data(kServerIdRole).toString();
        keepSession = current.sibling(current.row(), Server).data(kSessionIdRole).toInt();
    }

    m_model->removeRows(0, m_model->rowCount());
    int servers = 0, sessions = 0, row = 0, keepRow = -1;
    const QList<ServerProfile> profiles = m_store->all(); // implicitly shared: no copy of the data
    for (const ServerProfile &p : profiles) {
        const ServerStatus status = m_store->status(p.id);
        if (status.all.isEmpty())
            continue;
        ++servers;
        for (const RemoteSession &s : status.all) {
            ++sessions;
            const QString texts[ColumnCount] = {
                p.name, s.user,
                s.client.isEmpty() && Contacts::instance().nameFor({}, s.user).isEmpty()
                    ? QStringLiteral("—")
                    : Contacts::instance().label(s.client, s.user),
                s.connected ? tr("Active") : tr("Disconnected"), s.connected ? idleText(s.idleMinutes) : QString()};
            QList<QStandardItem *> items;
            for (int c = 0; c < ColumnCount; ++c) {
                auto *item = new QStandardItem(texts[c]);
                item->setData(p.id, kServerIdRole);
                item->setData(s.id, kSessionIdRole);
                item->setData(s.connected, kConnectedRole);
                item->setData(s.client, kClientRole);
                item->setData(s.user, kUserRole);
                if (c == State)
                    item->setForeground(s.connected ? Theme::kGreen : Theme::kLabelSecondary);
                items << item;
            }
            m_model->appendRow(items);
            if (p.id == keepServer && s.id == keepSession)
                keepRow = row;
            ++row;
        }
    }
    if (keepRow >= 0)
        m_view->selectRow(keepRow);
    else
        m_message->setEnabled(false);
    m_subtitle->setText(sessions == 0 ? tr("No one is connected right now.")
                                      : tr("%1 sessions on %2 servers").arg(sessions).arg(servers));
}
