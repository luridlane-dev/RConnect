#include "ServerPanel.h"

#include "ServerDelegate.h"
#include "Theme.h"
#include "model/ServerStore.h"

#include <QButtonGroup>
#include <QCollator>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QVBoxLayout>

// Filters by search text and availability, sorts by name with numbers in natural order
// ("srv2" before "srv10").
class ServerProxy : public QSortFilterProxyModel {
public:
    enum Mode { All, Available, InUse, Offline };

    explicit ServerProxy(QObject *parent)
        : QSortFilterProxyModel(parent)
    {
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
        setDynamicSortFilter(true);
    }

    void setMode(Mode mode)
    {
        if (mode == m_mode)
            return;
        beginFilterChange();
        m_mode = mode;
        endFilterChange();
    }

    void setText(const QString &text)
    {
        const QString t = text.trimmed();
        if (t == m_text)
            return;
        beginFilterChange();
        m_text = t;
        endFilterChange();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const QModelIndex i = sourceModel()->index(row, 0, parent);
        if (!m_text.isEmpty() && !i.data(ServerStore::SearchRole).toString().contains(m_text, Qt::CaseInsensitive))
            return false;
        const int state = i.data(ServerStore::StatusRole).toInt();
        const bool online = state == ServerStatus::Online;
        const bool busy = online && i.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Busy;
        switch (m_mode) {
        case Available: return online && !busy;
        case InUse:     return busy;
        case Offline:   return state == ServerStatus::Offline;
        case All:       break;
        }
        return true;
    }

    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override
    {
        return m_collator.compare(l.data(Qt::DisplayRole).toString(), r.data(Qt::DisplayRole).toString()) < 0;
    }

private:
    Mode m_mode = All;
    QString m_text;
    QCollator m_collator;
};

// Lets the panel ask for a fresh layout when the card width changes.
class CardView : public QListView {
public:
    using QListView::QListView;
    void relayout() { scheduleDelayedItemsLayout(); }
};

namespace {

QIcon magnifierIcon()
{
    constexpr int size = 18;
    constexpr qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::kLabelSecondary, 1.7, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(QRectF(2.5, 2.5, 9, 9));
    p.drawLine(QPointF(10.2, 10.2), QPointF(15, 15));
    return QIcon(pm);
}

constexpr int kColumnGap = ServerPanel::kInset; // the view adds this around every card

} // namespace

ServerPanel::ServerPanel(ServerStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("serverPanel"));

    m_proxy = new ServerProxy(this);
    m_proxy->setSourceModel(store);
    m_proxy->sort(0);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("searchField"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(magnifierIcon(), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_proxy->setText(t);
        updateEmptyState();
    });
    connect(m_search, &QLineEdit::returnPressed, this, &ServerPanel::openFirstMatch);

    m_add = new QPushButton(this);
    m_add->setObjectName(QStringLiteral("addButton"));
    m_add->setCursor(Qt::PointingHandCursor);
    connect(m_add, &QPushButton::clicked, this, &ServerPanel::addRequested);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(kInset, 0, m_rightInset, 0);
    m_headLayout = head;
    head->setSpacing(10);
    head->addWidget(m_search, 1);
    head->addWidget(m_add);

    // Segmented filter
    auto *segmented = new QFrame(this);
    segmented->setObjectName(QStringLiteral("segmented"));
    auto *segLay = new QHBoxLayout(segmented);
    segLay->setContentsMargins(2, 2, 2, 2);
    segLay->setSpacing(2);
    m_filters = new QButtonGroup(this);
    m_filters->setExclusive(true);
    for (int mode : {ServerProxy::All, ServerProxy::Available, ServerProxy::InUse, ServerProxy::Offline}) {
        auto *b = new QPushButton(segmented);
        b->setObjectName(QStringLiteral("segment"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setChecked(mode == ServerProxy::All);
        m_filters->addButton(b, mode);
        segLay->addWidget(b);
    }
    connect(m_filters, &QButtonGroup::idClicked, this, [this](int id) {
        m_proxy->setMode(ServerProxy::Mode(id));
        updateEmptyState();
    });
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(kInset, 0, m_rightInset, 0);
    m_filterLayout = filterRow;
    filterRow->addWidget(segmented);
    filterRow->addStretch();

    m_list = new CardView(this);
    m_list->setModel(m_proxy);
    m_delegate = new ServerDelegate(m_list);
    m_list->setItemDelegate(m_delegate);
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::LeftToRight);
    m_list->setWrapping(true);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setSpacing(kColumnGap);
    m_list->setUniformItemSizes(false); // the card width follows the window
    m_list->setMouseTracking(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->verticalScrollBar()->setSingleStep(24);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setCursor(Qt::PointingHandCursor);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->viewport()->installEventFilter(this);
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &i) {
        emit openRequested(i.data(ServerStore::IdRole).toString());
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, &ServerPanel::showContextMenu);

    m_empty = new QLabel(m_list->viewport());
    m_empty->setObjectName(QStringLiteral("emptyState"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    lay->addLayout(head);
    lay->addLayout(filterRow);
    lay->addWidget(m_list, 1);

    connect(store, &ServerStore::summaryChanged, this, &ServerPanel::updateCounts);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &ServerPanel::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &ServerPanel::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &ServerPanel::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &ServerPanel::updateEmptyState);
    retranslate();
}

QString ServerPanel::summary(const ServerStore *store)
{
    const ServerStore::Counts c = store->counts();
    if (c.total == 0)
        return tr("No servers yet");
    return tr("%1 · %2 available · %3 in use · %4 offline")
        .arg(c.total == 1 ? tr("1 server") : tr("%1 servers").arg(c.total))
        .arg(c.online - c.busy)
        .arg(c.busy)
        .arg(c.offline);
}

void ServerPanel::retranslate()
{
    m_search->setPlaceholderText(tr("Search servers"));
    m_add->setText(tr("+ Add server"));
    updateCounts();
    updateEmptyState();
    m_list->viewport()->update();
}

void ServerPanel::updateCounts()
{
    const ServerStore::Counts c = m_store->counts();
    const struct { int id; QString name; int count; } items[] = {
        {ServerProxy::All, tr("All"), c.total},
        {ServerProxy::Available, tr("Available"), c.online - c.busy},
        {ServerProxy::InUse, tr("In use"), c.busy},
        {ServerProxy::Offline, tr("Offline"), c.offline},
    };
    for (const auto &it : items)
        if (QAbstractButton *b = m_filters->button(it.id))
            b->setText(QStringLiteral("%1 %2").arg(it.name).arg(it.count));
}

void ServerPanel::updateEmptyState()
{
    const bool empty = m_proxy->rowCount() == 0;
    m_empty->setVisible(empty);
    if (!empty)
        return;
    m_empty->setText(m_store->rowCount() == 0 ? tr("No servers yet.\nUse “+ Add server” to get started.")
                                              : tr("No matching servers"));
    m_empty->setGeometry(m_list->viewport()->rect().adjusted(24, 0, -24, 0));
}

// Columns fill the row: as many as fit at the minimum width, then widened to the edge. The view
// puts one gap before every card and keeps the scroll bar's width free, so the room is measured
// from that. The search row is then given the same right edge as the last column.
void ServerPanel::fitCards()
{
    const int room = m_list->viewport()->width() - m_list->verticalScrollBar()->width() - 2;
    const int columns = qMax(1, (room - kColumnGap) / (ServerDelegate::kMinCardWidth + kColumnGap));
    const int card = qMax(ServerDelegate::kMinCardWidth, (room - (columns + 1) * kColumnGap) / columns);

    const int inset = qMax<int>(kInset, m_list->width() - columns * (card + kColumnGap));
    if (inset != m_rightInset) {
        m_rightInset = inset;
        m_headLayout->setContentsMargins(kInset, 0, inset, 0);
        m_filterLayout->setContentsMargins(kInset, 0, inset, 0);
        emit rightInsetChanged(inset);
    }
    if (card == m_delegate->cardWidth())
        return;
    m_delegate->setCardWidth(card);
    static_cast<CardView *>(m_list)->relayout();
    if (!m_fitted) {
        // The first layout is delayed; start at the top once it is done.
        m_fitted = true;
        QTimer::singleShot(250, this, [this] { m_list->verticalScrollBar()->setValue(0); });
    }
}

bool ServerPanel::eventFilter(QObject *watched, QEvent *e)
{
    if (watched == m_list->viewport() && e->type() == QEvent::Resize) {
        fitCards();
        if (m_empty->isVisible())
            m_empty->setGeometry(m_list->viewport()->rect().adjusted(24, 0, -24, 0));
    }
    return QWidget::eventFilter(watched, e);
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
