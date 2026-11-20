#include "ServerDelegate.h"
#include "Theme.h"
#include "core/GeoIp.h"
#include "ServerPanel.h"
#include "ServerSectionModel.h"
#include "model/ServerStore.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QToolTip>

#include <cmath>
#include <utility>

namespace {

constexpr int kPad = 14;
constexpr int kDot = 8;
constexpr int kFlagWidth = 16;
constexpr int kFlagHeight = 11;
constexpr int kButtonWidth = 76;
constexpr int kButtonHeight = 28;
constexpr int kSide = ServerPanel::kInset; // a row stays this far from the left and right edge of its column

QColor blend(const QColor &from, const QColor &to, qreal amount)
{
    const auto channel = [amount](qreal a, qreal b) { return a + (b - a) * amount; };
    return QColor::fromRgbF(channel(from.redF(), to.redF()), channel(from.greenF(), to.greenF()),
                           channel(from.blueF(), to.blueF()));
}

// A row is drawn inside its item, between the side margins; a card fills its item.
QRect inner(const QRect &item, bool list)
{
    return list ? item.adjusted(kSide, 0, -kSide, 0) : item;
}

// Every rectangle of an item, so painting and tooltips agree.
struct Geometry {
    QRect icon, dot, name, pill, star, latency, flag, host, status, activity, button;
};

Geometry cardGeometry(const QRect &card, bool hasFlag)
{
    Geometry g;
    const int left = card.left() + kPad;
    const int right = card.right() + 1 - kPad;
    const int top = card.top() + 9;
    const int addressTop = card.top() + 30;
    const int statusTop = card.top() + 50;

    g.star = QRect(card.left() + 11, card.top() + 4, 16, 18);
    g.icon = QRect(left, card.top() + 26, 38, 38);
    g.dot = QRect(g.icon.right() - 6, g.icon.bottom() - 6, kDot, kDot);
    const int textLeft = g.icon.right() + 13;
    g.latency = QRect(right - 49, top, 49, 18);
    g.name = QRect(textLeft, top, g.latency.left() - 8 - textLeft, 18);
    g.pill = QRect(g.name.right() + 1 - 38, top, 38, 18);

    const int hostLeft = textLeft + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft, addressTop + (16 - kFlagHeight) / 2, kFlagWidth, kFlagHeight);
    g.host = QRect(hostLeft, addressTop, right - hostLeft, 16);
    g.status = QRect(textLeft, statusTop, right - textLeft, 17);
    return g;
}

Geometry rowGeometry(const QRect &row, bool hasFlag)
{
    Geometry g;
    const int left = row.left() + kPad;
    const int right = row.right() + 1 - kPad;
    const int mid = row.center().y();

    g.button = QRect(right - kButtonWidth, mid - kButtonHeight / 2, kButtonWidth, kButtonHeight);
    // The time sits next to the button whether or not the star is shown; the star goes in front of it.
    g.latency = QRect(g.button.left() - 10 - 52, mid - 9, 52, 18);
    g.star = QRect(g.latency.left() - 6 - 16, mid - 9, 16, 18);
    g.dot = QRect(left, mid - kDot / 2, kDot, kDot);
    const int textLeft = left + kDot + 10;
    const int statusRight = g.star.left() - 12;
    const int room = statusRight - textLeft;
    const int statusWidth = qMin(qMax(208, room / 2), room * 2 / 3);
    const int statusLeft = statusRight - statusWidth;
    const int nameRight = statusLeft - 12;
    g.name = QRect(textLeft, mid - 18, nameRight - textLeft, 18);
    g.pill = QRect(nameRight - 38, g.name.top(), 38, 18);
    const int hostLeft = textLeft + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft, mid + 3, kFlagWidth, kFlagHeight);
    g.host = QRect(hostLeft, mid + 1, nameRight - hostLeft, 16);
    g.status = QRect(statusLeft, mid - 18, statusWidth, 18);
    g.activity = QRect(statusLeft, mid + 1, statusWidth, 16);
    return g;
}

QPixmap flagPixmap(const QString &code, qreal dpr)
{
    if (code.isEmpty())
        return {};
    const QString key = QStringLiteral("flag:%1:%2").arg(code).arg(dpr);
    QPixmap pm;
    if (QPixmapCache::find(key, &pm))
        return pm;
    const QImage src(QStringLiteral(":/flags/%1.png").arg(code));
    if (src.isNull())
        return {};
    pm = QPixmap::fromImage(src.scaled(QSize(kFlagWidth, kFlagHeight) * dpr, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation));
    pm.setDevicePixelRatio(dpr);
    QPixmapCache::insert(key, pm);
    return pm;
}

void drawStar(QPainter *p, const QRect &box, bool filled, qreal opacity)
{
    const QPointF c = QRectF(box).center();
    QPainterPath star;
    for (int k = 0; k < 10; ++k) {
        const qreal r = k % 2 ? 3.2 : 7.2;
        const qreal a = -M_PI / 2 + k * M_PI / 5;
        const QPointF pt(c.x() + r * std::cos(a), c.y() + 0.6 + r * std::sin(a));
        if (k == 0)
            star.moveTo(pt);
        else
            star.lineTo(pt);
    }
    star.closeSubpath();
    p->save();
    p->setOpacity(opacity);
    p->setRenderHint(QPainter::Antialiasing);
    p->setBrush(filled ? QBrush(Theme::kFavorite) : Qt::NoBrush);
    const QColor outline = Theme::mode() == Theme::Dark ? Theme::kFavorite : Theme::kOrange;
    p->setPen(QPen(filled ? outline : Theme::kLabelTertiary, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->drawPath(star);
    p->restore();
}

} // namespace

ServerDelegate::ServerDelegate(QAbstractItemView *view)
    : QStyledItemDelegate(view), m_view(view)
{
    m_hover.setStartValue(0.0);
    m_hover.setEndValue(1.0);
    m_hover.setDuration(145);
    m_hover.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_hover, &QVariantAnimation::valueChanged, this, [this] {
        for (auto it = m_hoverFrom.cbegin(); it != m_hoverFrom.cend(); ++it) {
            if (it.key().isValid())
                m_view->update(it.key());
        }
    });
    connect(&m_hover, &QVariantAnimation::finished, this, [this] { m_hoverFrom.clear(); });
}

bool ServerDelegate::setOpenSessionIds(const QSet<QString> &open)
{
    if (m_open == open)
        return false;
    m_open = open;
    return true;
}

qreal ServerDelegate::hoverAmount(const QPersistentModelIndex &index) const
{
    if (!index.isValid())
        return 0;
    const qreal target = index == m_hovered ? 1 : 0;
    if (m_hover.state() != QAbstractAnimation::Running)
        return target;
    const qreal from = m_hoverFrom.value(index);
    return from + (target - from) * m_hover.currentValue().toReal();
}

void ServerDelegate::setHovered(const QModelIndex &index)
{
    if (m_hovered == index)
        return;
    QHash<QPersistentModelIndex, qreal> current;
    for (auto it = m_hoverFrom.cbegin(); it != m_hoverFrom.cend(); ++it) {
        const qreal amount = hoverAmount(it.key());
        if (amount > 0)
            current.insert(it.key(), amount);
    }
    if (m_hovered.isValid())
        current.insert(m_hovered, hoverAmount(m_hovered));
    if (index.isValid() && !current.contains(index))
        current.insert(index, 0);

    m_hover.stop();
    m_hoverFrom = std::move(current);
    m_hovered = index;
    if (!m_hoverFrom.isEmpty()) {
        m_hover.start();
        for (auto it = m_hoverFrom.cbegin(); it != m_hoverFrom.cend(); ++it)
            m_view->update(it.key());
    }
}

void ServerDelegate::setPressed(const QModelIndex &index)
{
    if (m_pressed == index)
        return;
    const QPersistentModelIndex previous = m_pressed;
    m_pressed = index;
    if (previous.isValid())
        m_view->update(previous);
    if (index.isValid())
        m_view->update(index);
}

QRect ServerDelegate::starRect(const QRect &itemRect) const
{
    const QRect r = inner(itemRect, m_list).adjusted(0, 0, -1, -1);
    return m_list ? rowGeometry(r, false).star.adjusted(-6, -6, 6, 6)
                  : cardGeometry(r, false).star.adjusted(-5, -3, 5, 2);
}

bool ServerDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    if (event->type() != QEvent::ToolTip || index.data(ServerSectionModel::HeaderRole).toBool()) {
        QToolTip::hideText();
        return true;
    }
    const QString code = index.data(ServerStore::CountryRole).toString();
    const QRect r = inner(option.rect, m_list).adjusted(0, 0, -1, -1);
    const Geometry g = m_list ? rowGeometry(r, !code.isEmpty()) : cardGeometry(r, !code.isEmpty());
    const QPoint pos = event->pos();
    QString text;
    if (!code.isEmpty() && g.flag.adjusted(-3, -3, 3, 3).contains(pos))
        text = GeoIp::countryName(code);
    else if (g.status.contains(pos) || g.activity.contains(pos) || g.latency.contains(pos)
             || g.dot.adjusted(-3, -3, 3, 3).contains(pos))
        text = index.data(ServerStore::StatusToolTipRole).toString();
    if (text.isEmpty() && starRect(option.rect).contains(pos))
        text = QCoreApplication::translate("ServerDelegate", "Favourite");
    if (text.isEmpty()) {
        QToolTip::hideText();
        return true;
    }
    QToolTip::showText(event->globalPos(), QStringLiteral("<qt>%1</qt>")
        .arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))), view);
    return true;
}

QSize ServerDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    if (index.data(ServerSectionModel::HeaderRole).toBool())
        return QSize(m_columns * m_cardWidth + (m_columns - 1) * m_gap, kHeaderHeight);
    return QSize(m_cardWidth, m_list ? kRowHeight : kCardHeight);
}

void ServerDelegate::paintHeader(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    p->save();
    p->setRenderHint(QPainter::TextAntialiasing);

    const QString group = index.data(ServerSectionModel::SectionGroupRole).toString();
    const bool collapsed = index.data(ServerSectionModel::SectionCollapsedRole).toBool();
    const int count = index.data(ServerSectionModel::SectionCountRole).toInt();
    const qreal hover = hoverAmount(index);

    const QRect item = inner(opt.rect, m_list).adjusted(0, 0, -1, -1);
    const int mid = item.center().y();
    const int ax = item.left() + 11;
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(blend(Theme::kLabelSecondary, Theme::kBlueText, hover), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath arrow;
    if (collapsed) {
        arrow.moveTo(ax - 2, mid - 4);
        arrow.lineTo(ax + 2, mid);
        arrow.lineTo(ax - 2, mid + 4);
    } else {
        arrow.moveTo(ax - 4, mid - 2);
        arrow.lineTo(ax, mid + 2);
        arrow.lineTo(ax + 4, mid - 2);
    }
    p->drawPath(arrow);

    const QFont titleFont = Theme::font(13, QFont::DemiBold);
    const QFontMetrics metrics(titleFont);
    p->setFont(titleFont);
    p->setPen(blend(Theme::kLabel, Theme::kBlueText, hover));
    const QString title = group.isEmpty() ? QCoreApplication::translate("ServerDelegate", "Other") : group;
    const int titleWidth = qMin(metrics.horizontalAdvance(title) + 2, item.width() / 2);
    const int x = item.left() + 25;
    p->drawText(QRect(x, mid - 10, titleWidth, 20), Qt::AlignVCenter | Qt::AlignLeft,
                metrics.elidedText(title, Qt::ElideRight, titleWidth));

    const QString countText = QString::number(count);
    const QFont countFont = Theme::font(11, QFont::DemiBold);
    const int countWidth = qMax(24, QFontMetrics(countFont).horizontalAdvance(countText) + 12);
    const QRect countRect(x + titleWidth + 7, mid - 9, countWidth, 18);
    p->setPen(Qt::NoPen);
    p->setBrush(Theme::kAccentSoft);
    p->drawRoundedRect(countRect, 3, 3);
    p->setFont(countFont);
    p->setPen(Theme::kLabelSecondary);
    p->drawText(countRect, Qt::AlignCenter, countText);
    p->setPen(Theme::kSeparator);
    p->drawLine(countRect.right() + 12, mid, item.right(), mid);

    p->restore();
}

void ServerDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    if (index.data(ServerSectionModel::HeaderRole).toBool()) {
        paintHeader(p, opt, index);
        return;
    }
    p->save();
    p->setRenderHint(QPainter::TextAntialiasing);

    const qreal hover = hoverAmount(index);
    const QString id = index.data(ServerStore::IdRole).toString();
    const bool open = m_open.contains(id);
    const QString code = index.data(ServerStore::CountryRole).toString();
    const QRect rect = inner(opt.rect, m_list).adjusted(0, 0, -1, -1);
    const Geometry g = m_list ? rowGeometry(rect, !code.isEmpty()) : cardGeometry(rect, !code.isEmpty());

    const int state = index.data(ServerStore::StatusRole).toInt();
    const bool offline = state == ServerStatus::Offline;
    const bool online = state == ServerStatus::Online;
    const bool busy = (online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Busy);

    const bool connected = index.data(ServerStore::OwnSessionRole).toBool();
    const QColor base = connected ? Theme::kConnectedSurface : busy ? Theme::kBusySurface : Theme::kCard;
    const bool dark = Theme::mode() == Theme::Dark;
    const QColor veil = dark ? Qt::white : Qt::black;
    const qreal interaction = index == m_pressed ? 0.09 : 0.045 * hover;
    const QColor fill = blend(base, veil, interaction);
    if (m_list) {
        const QModelIndex previous = index.sibling(index.row() - 1, 0);
        const bool first = !previous.isValid() || previous.data(ServerSectionModel::HeaderRole).toBool();
        const QModelIndex next = index.sibling(index.row() + 1, 0);
        const bool last = !next.isValid() || next.data(ServerSectionModel::HeaderRole).toBool();
        p->fillRect(rect, fill);
        p->setPen(Theme::kSeparatorStrong);
        p->drawLine(rect.left(), rect.top(), rect.left(), rect.bottom());
        p->drawLine(rect.right(), rect.top(), rect.right(), rect.bottom());
        if (first)
            p->drawLine(rect.topLeft(), rect.topRight());
        p->setPen(last ? Theme::kSeparatorStrong : Theme::kSeparator);
        p->drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());
    } else {
        p->setRenderHint(QPainter::Antialiasing);
        p->setBrush(fill);
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(QRectF(rect), 4, 4);

        p->setPen(Qt::NoPen);
        p->setBrush(open ? Theme::kCard : Theme::kAccentSoft);
        p->drawRoundedRect(g.icon, 3, 3);
        const QPointF centre = QRectF(g.icon).center();
        p->setPen(QPen(Theme::kBlueText, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(centre.x() - 8, centre.y() - 7, 16, 11), 2, 2);
        p->drawLine(centre + QPointF(0, 4), centre + QPointF(0, 7));
        p->drawLine(centre + QPointF(-3, 7), centre + QPointF(3, 7));
    }

    // Status marker
    const QColor statusColor = index.data(ServerStore::StatusColorRole).value<QColor>();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(m_list ? QPen(Qt::NoPen) : QPen(fill, 2));
    p->setBrush(statusColor);
    p->drawEllipse(g.dot);
    p->setRenderHint(QPainter::Antialiasing, false);

    // Name, and the mark of a server that is open in a tab
    const QFont nameFont = Theme::font(14, QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(Theme::kLabel);
    const int nameWidth = qMax(0, g.name.width() - (open ? g.pill.width() + 4 : 0));
    p->drawText(QRect(g.name.left(), g.name.top(), nameWidth, g.name.height()), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameWidth));
    if (open) {
        p->setFont(Theme::font(10, QFont::DemiBold));
        p->setPen(Theme::kBlueText);
        p->drawText(g.pill, Qt::AlignCenter,
                    QCoreApplication::translate("ServerDelegate", "OPEN"));
    }

    // Address with the country flag
    const QPixmap flag = flagPixmap(code, p->device()->devicePixelRatioF());
    if (!flag.isNull())
        p->drawPixmap(g.flag.topLeft(), flag);
    const QFont textFont = Theme::font(12);
    p->setFont(textFont);
    p->setPen(Theme::kLabelSecondary);
    const QFontMetrics textMetrics(textFont);
    p->drawText(g.host, Qt::AlignVCenter | Qt::AlignLeft,
                textMetrics.elidedText(index.data(ServerStore::HostRole).toString(), Qt::ElideRight, g.host.width()));

    // Reserve space for the activity first so long computer names cannot hide it.
    const QString badge = index.data(ServerStore::StatusBadgeRole).toString();
    if (connected) {
        const QFont whoFont = Theme::font(13, QFont::DemiBold);
        p->setFont(whoFont);
        p->setPen(Theme::kGreen);
        p->drawText(g.status, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(whoFont).elidedText(badge, Qt::ElideRight, g.status.width()));
    } else if (busy) {
        QStringList activityParts;
        const QString age = connectionDurationText(index.data(ServerStore::OwnerConnectionMinutesRole).toLongLong());
        if (!age.isEmpty())
            activityParts << age;
        const int idle = index.data(ServerStore::OwnerIdleRole).toInt();
        if (idle >= 0)
            activityParts << idleShort(idle);
        const QString activityText = activityParts.join(QStringLiteral(" · "));

        QRect whoRect = g.status;
        QRect activityRect = g.activity;
        if (!m_list && !activityText.isEmpty()) {
            const int width = qMin(g.status.width(), textMetrics.horizontalAdvance(activityText) + 2);
            activityRect = QRect(g.status.right() + 1 - width, g.status.top(), width, g.status.height());
            whoRect.setWidth(qMax(0, activityRect.left() - 8 - whoRect.left()));
        }

        const QFont whoFont = Theme::font(13, QFont::Bold);
        p->setFont(whoFont);
        p->setPen(Theme::kBusyText);
        p->drawText(whoRect, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(whoFont).elidedText(badge, Qt::ElideRight, whoRect.width()));
        if (!activityText.isEmpty()) {
            p->setFont(textFont);
            p->setPen(Theme::kLabelSecondary);
            p->drawText(activityRect, Qt::AlignVCenter | (m_list ? Qt::AlignLeft : Qt::AlignRight),
                        textMetrics.elidedText(activityText, Qt::ElideRight, activityRect.width()));
        }
    } else {
        const bool queryFailed = online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::OccupancyError;
        p->setPen(queryFailed ? Theme::kRed : offline ? Theme::kLabelTertiary : Theme::kLabelSecondary);
        p->drawText(g.status, Qt::AlignVCenter | (m_list ? Qt::AlignLeft : Qt::AlignRight),
                    textMetrics.elidedText(badge, Qt::ElideRight, g.status.width()));
    }

    // Connect time
    const int latency = index.data(ServerStore::LatencyRole).toInt();
    if (online && latency >= 0) {
        p->setFont(Theme::font(11));
        p->setPen(Theme::kLabelTertiary);
        p->drawText(g.latency, Qt::AlignVCenter | Qt::AlignRight,
                    QCoreApplication::translate("ServerDelegate", "%1 ms").arg(latency));
    }

    // Favourite star: always there when set, otherwise only while the pointer is on the item
    const bool favorite = index.data(ServerStore::FavoriteRole).toBool();
    if (favorite || hover > 0)
        drawStar(p, g.star, favorite, favorite ? 1.0 : hover);

    // Connect button of a row
    if (m_list) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setBrush(blend(Theme::kAccentSoft, veil, interaction));
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(g.button, 3, 3);
        p->setFont(Theme::font(12, QFont::DemiBold));
        p->setPen(Theme::kBlueText);
        p->drawText(g.button, Qt::AlignCenter, QCoreApplication::translate("ServerDelegate", "Connect"));
    }

    p->restore();
}
