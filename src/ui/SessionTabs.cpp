#include "SessionTabs.h"

#include "Theme.h"

#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QEasingCurve>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>

namespace {

constexpr int kDetachDistance = 28;

// Used instead of the "not allowed" cursor over empty space.
const QPixmap &arrowCursor()
{
    static const QPixmap pm = [] {
        QPixmap pmap(20, 22);
        pmap.fill(Qt::transparent);
        QPainter p(&pmap);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath arrow;
        arrow.moveTo(1, 1);
        arrow.lineTo(1, 17);
        arrow.lineTo(5, 13);
        arrow.lineTo(8, 20);
        arrow.lineTo(11, 19);
        arrow.lineTo(8, 12);
        arrow.lineTo(13, 12);
        arrow.closeSubpath();
        p.fillPath(arrow, Qt::white);
        p.setPen(QPen(Qt::black, 1.2));
        p.drawPath(arrow);
        return pmap;
    }();
    return pm;
}

} // namespace

const char *const AnimatedTabBar::kMimeType = "application/x-rconnect-session";

AnimatedTabBar::AnimatedTabBar(QWidget *parent)
    : QTabBar(parent)
{
    setTabsClosable(false); // the owner adds its own close buttons
    setMovable(true);
    setDocumentMode(true);
    setExpanding(false);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
    setDrawBase(false);
    setFixedHeight(kHeight);
    setAcceptDrops(true);

    setProperty("selectionVisible", true);

    m_add = new QToolButton(this);
    m_add->setObjectName(QStringLiteral("menuButton"));
    m_add->setText(QStringLiteral("+"));
    m_add->setToolTip(tr("Servers"));
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setFixedSize(kHeight, kHeight);
    connect(m_add, &QToolButton::clicked, this, &AnimatedTabBar::addRequested);

    m_anim.setDuration(175);
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    m_anim.setStartValue(0.0);
    m_anim.setEndValue(1.0);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this] {
        const int idx = currentIndex();
        if (idx >= 0) {
            const QRect dirty = m_current.toAlignedRect().united(tabRect(idx)).united(m_from.toAlignedRect()).adjusted(-2, 0, 2, 0);
            update(dirty);
        } else {
            update();
        }
    });
    connect(this, &QTabBar::currentChanged, this, &AnimatedTabBar::slideTo);
    connect(Theme::notifier(), &ThemeNotifier::changed, this, qOverload<>(&QWidget::update));
    placeAddButton();
}

QSize AnimatedTabBar::tabSizeHint(int index) const
{
    QSize s = QTabBar::tabSizeHint(index);
    s.setHeight(kHeight);
    return s;
}

void AnimatedTabBar::placeAddButton()
{
    if (!m_add) // called by QTabBar during construction
        return;
    int x = count() > 0 ? tabRect(count() - 1).right() + 1 : 0;
    x = qMax(0, qMin(x, width() - m_add->width()));
    if (m_add->x() != x || m_add->y() != 0)
        m_add->move(x, 0);
}

void AnimatedTabBar::tabInserted(int index)
{
    QTabBar::tabInserted(index);
    placeAddButton();
    if (m_add)
        m_add->raise();
}

void AnimatedTabBar::tabRemoved(int index)
{
    QTabBar::tabRemoved(index);
    placeAddButton();
}

void AnimatedTabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    placeAddButton();
}

void AnimatedTabBar::resizeEvent(QResizeEvent *e)
{
    QTabBar::resizeEvent(e);
    placeAddButton();
}

void AnimatedTabBar::changeEvent(QEvent *e)
{
    QTabBar::changeEvent(e);
    if (e->type() == QEvent::LanguageChange && m_add)
        m_add->setToolTip(tr("Servers"));
}

void AnimatedTabBar::setSelectionVisible(bool visible)
{
    if (m_selectionVisible == visible)
        return;
    m_selectionVisible = visible;
    setProperty("selectionVisible", visible);
    if (style()) {
        style()->unpolish(this);
        style()->polish(this);
    }
    if (!visible) {
        m_anim.stop();
        m_from = {};
        m_current = {};
    }
    update();
}

void AnimatedTabBar::slideTo(int index)
{
    if (index < 0)
        return;
    m_from = m_current.isEmpty() ? QRectF(tabRect(index)) : m_current;
    m_anim.stop();
    m_anim.start();
}

void AnimatedTabBar::paintEvent(QPaintEvent *e)
{
    const int idx = currentIndex();
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Subtle dividers between inactive tabs so tabs stay visually distinct.
        for (int i = 0; i < count() - 1; ++i) {
            if (m_selectionVisible && (i == idx || i + 1 == idx))
                continue;
            const int x = tabRect(i).right();
            p.fillRect(QRect(x, 10, 1, height() - 20), Theme::kSeparatorStrong);
        }

        // The active tab stands out with a lifted surface, crisp frame and bright sliding accent bar.
        if (idx >= 0 && m_selectionVisible) {
            const QRectF target = tabRect(idx);
            const qreal t = m_anim.state() == QAbstractAnimation::Running ? m_anim.currentValue().toReal() : 1.0;
            const QRectF from = m_from.isEmpty() ? target : m_from;
            m_current = QRectF(from.x() + (target.x() - from.x()) * t,
                               from.y(),
                               from.width() + (target.width() - from.width()) * t,
                               target.height());

            const QRectF box = m_current.adjusted(2.0, 3.0, -2.0, 0.0);
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::kTabActive);
            p.drawRect(box);

            // Crisp 1px top/left/right border framing the active tab.
            p.setPen(QPen(Theme::kTabActiveBorder, 1.0));
            const qreal left = box.left() + 0.5;
            const qreal right = box.right() - 0.5;
            const qreal top = box.top() + 0.5;
            const qreal bottom = box.bottom();
            p.drawLine(QPointF(left, bottom), QPointF(left, top));
            p.drawLine(QPointF(left, top), QPointF(right, top));
            p.drawLine(QPointF(right, top), QPointF(right, bottom));

            // Bright bottom accent bar connecting the active tab to the session below it.
            constexpr qreal kAccentHeight = 3.0;
            p.fillRect(QRectF(box.left(), box.bottom() - kAccentHeight, box.width(), kAccentHeight),
                       Theme::kTabActiveAccent);
        }
    }

    QTabBar::paintEvent(e);

    if (m_dropIndex >= 0) {
        QPainter p(this);
        const int x = m_dropIndex < count() ? tabRect(m_dropIndex).left()
                                            : (count() > 0 ? tabRect(count() - 1).right() + 1 : 0);
        p.fillRect(QRect(qMax(0, x - 1), 4, 2, height() - 8), Theme::kBlue);
    }
}

void AnimatedTabBar::mousePressEvent(QMouseEvent *e)
{
    m_pressId.clear();
    if (e->button() == Qt::LeftButton) {
        const int i = tabAt(e->position().toPoint());
        if (i >= 0) {
            m_pressId = tabData(i).toString();
            m_pressPos = e->position().toPoint() - tabRect(i).topLeft();
        }
    }
    QTabBar::mousePressEvent(e);
}

void AnimatedTabBar::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!m_pressId.isEmpty() && (e->buttons() & Qt::LeftButton)
        && (pos.y() < -kDetachDistance || pos.y() > height() + kDetachDistance)) {
        const QString id = m_pressId;
        m_pressId.clear();
        // Finish QTabBar's own move first.
        QMouseEvent release(QEvent::MouseButtonRelease, e->position(), e->globalPosition(), Qt::LeftButton,
                            Qt::NoButton, e->modifiers());
        QTabBar::mouseReleaseEvent(&release);
        startDrag(id, m_pressPos);
        return;
    }
    QTabBar::mouseMoveEvent(e);
}

void AnimatedTabBar::mouseReleaseEvent(QMouseEvent *e)
{
    m_pressId.clear();
    QTabBar::mouseReleaseEvent(e);
}

void AnimatedTabBar::startDrag(const QString &serverId, const QPoint &hotSpot)
{
    int index = -1;
    for (int i = 0; i < count(); ++i)
        if (tabData(i).toString() == serverId)
            index = i;
    if (index < 0)
        return;

    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMimeType), serverId.toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(grab(tabRect(index)));
    drag->setHotSpot(hotSpot);
    drag->setDragCursor(arrowCursor(), Qt::IgnoreAction);
    if (drag->exec(Qt::MoveAction) == Qt::IgnoreAction)
        emit detachRequested(serverId, QCursor::pos());
}


int AnimatedTabBar::insertIndexAt(const QPoint &pos) const
{
    for (int i = 0; i < count(); ++i)
        if (pos.x() < tabRect(i).center().x())
            return i;
    return count();
}

void AnimatedTabBar::dragEnterEvent(QDragEnterEvent *e)
{
    if (!e->mimeData()->hasFormat(QString::fromLatin1(kMimeType)))
        return;
    e->setDropAction(Qt::MoveAction);
    e->accept();
    m_dropIndex = insertIndexAt(e->position().toPoint());
    update();
}

void AnimatedTabBar::dragMoveEvent(QDragMoveEvent *e)
{
    if (!e->mimeData()->hasFormat(QString::fromLatin1(kMimeType)))
        return;
    e->setDropAction(Qt::MoveAction);
    e->accept();
    const int idx = insertIndexAt(e->position().toPoint());
    if (idx != m_dropIndex) {
        m_dropIndex = idx;
        update();
    }
}

void AnimatedTabBar::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dropIndex = -1;
    update();
}

void AnimatedTabBar::dropEvent(QDropEvent *e)
{
    const QByteArray id = e->mimeData()->data(QString::fromLatin1(kMimeType));
    const int index = insertIndexAt(e->position().toPoint());
    m_dropIndex = -1;
    update();
    if (id.isEmpty())
        return;
    e->setDropAction(Qt::MoveAction);
    e->accept();
    emit tabDropped(QString::fromUtf8(id), index);
}
