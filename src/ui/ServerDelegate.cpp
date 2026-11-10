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
#include <QPolygonF>
#include <QToolTip>

#include <cmath>

namespace {

constexpr int kPad = 12;
constexpr int kDot = 8;
constexpr int kFlagWidth = 16;
constexpr int kFlagHeight = 11;
constexpr int kButtonWidth = 76;
constexpr int kButtonHeight = 24;
constexpr int kSide = ServerPanel::kInset; // a row stays this far from the left and right edge of its column

// A row is drawn inside its item, between the side margins; a card fills its item.
QRect inner(const QRect &item, bool list)
{
    return list ? item.adjusted(kSide, 0, -kSide, 0) : item;
}

// Every rectangle of an item, so painting and tooltips agree.
struct Geometry {
    QRect dot, name, pill, star, latency, flag, host, status, button;
};

Geometry cardGeometry(const QRect &card, bool hasFlag)
{
    Geometry g;
    const int left = card.left() + kPad;
    const int right = card.right() + 1 - kPad;
    const int top = card.top() + 10;
    const int bottom = card.top() + 33;

    g.dot = QRect(left, top + 5, kDot, kDot);
    const int textLeft = left + kDot + 8;
    g.star = QRect(right - 16, top, 16, 18);
    g.latency = QRect(g.star.left() - 4 - 44, top, 44, 18);
    g.name = QRect(textLeft, top, g.latency.left() - 6 - textLeft, 18);
    g.pill = QRect(g.name.right() + 1 - 38, top, 38, 18);

    const int hostLeft = textLeft + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft, bottom + (16 - kFlagHeight) / 2, kFlagWidth, kFlagHeight);
    const int hostRight = hostLeft + qMin((right - hostLeft) * 46 / 100, 100);
    g.host = QRect(hostLeft, bottom, hostRight - hostLeft, 16);
    g.status = QRect(hostRight + 8, bottom, right - hostRight - 8, 16);
    return g;
}

Geometry rowGeometry(const QRect &row, bool hasFlag)
{
    Geometry g;
    const int left = row.left() + kPad;
    const int right = row.right() + 1 - kPad;
    const int mid = row.center().y();

    g.button = QRect(right - kButtonWidth, mid - kButtonHeight / 2, kButtonWidth, kButtonHeight);
    g.star = QRect(g.button.left() - 10 - 16, mid - 9, 16, 18);
    g.latency = QRect(g.star.left() - 8 - 52, mid - 9, 52, 18);
    g.dot = QRect(left, mid - kDot / 2, kDot, kDot);
    const int textLeft = left + kDot + 10;
    const int room = g.latency.left() - 12 - textLeft;
    const int nameWidth = room * 38 / 100;
    const int hostWidth = room * 30 / 100;
    g.name = QRect(textLeft, mid - 9, nameWidth - 8, 18);
    g.pill = QRect(g.name.right() - 38 + 1, mid - 9, 38, 18);
    const int hostLeft = textLeft + nameWidth + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft + nameWidth, mid - kFlagHeight / 2, kFlagWidth, kFlagHeight);
    g.host = QRect(hostLeft, mid - 8, textLeft + nameWidth + hostWidth - hostLeft, 16);
    g.status = QRect(textLeft + nameWidth + hostWidth + 8, mid - 8, g.latency.left() - 12 - (textLeft + nameWidth + hostWidth + 8), 16);
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

void drawStar(QPainter *p, const QRect &box, bool filled)
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
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setBrush(filled ? QBrush(Theme::kOrange) : Qt::NoBrush);
    p->setPen(QPen(filled ? Theme::kOrange : Theme::kLabelTertiary, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->drawPath(star);
    p->setRenderHint(QPainter::Antialiasing, false);
}

} // namespace

QRect ServerDelegate::starRect(const QRect &itemRect) const
{
    const QRect r = inner(itemRect, m_list).adjusted(0, 0, -1, -1);
    return (m_list ? rowGeometry(r, false) : cardGeometry(r, false)).star.adjusted(-6, -6, 6, 6);
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
    else if (g.status.united(g.latency).united(g.dot).adjusted(-3, -3, 3, 3).contains(pos))
        text = index.data(ServerStore::StatusToolTipRole).toString();
    if (text.isEmpty() && g.star.adjusted(-6, -6, 6, 6).contains(pos))
        text = QCoreApplication::translate("ServerDelegate", "Favourite");
    if (text.isEmpty()) {
        QToolTip::hideText();
        return true;
    }
    QToolTip::showText(event->globalPos(), text, view);
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
    const bool hover = opt.state & QStyle::State_MouseOver;

    // The bar that heads a group: square, tinted, boxed on all sides. Rows hang under it as one table.
    const QRect item = inner(opt.rect, m_list).adjusted(0, 0, -1, -1);
    const QRect bar(item.left(), item.bottom() - 28, item.width(), 28);
    const int mid = bar.center().y();
    p->fillRect(bar, Theme::kHeaderFill);
    if (hover)
        p->fillRect(bar, Theme::kSeparator);
    p->setPen(Theme::kSeparatorStrong);
    p->setBrush(Qt::NoBrush);
    p->drawRect(bar);

    // Arrow: points right while the group is folded
    const qreal ax = bar.left() + 14;
    QPolygonF arrow;
    if (collapsed)
        arrow << QPointF(ax - 2, mid - 4) << QPointF(ax + 2.5, mid) << QPointF(ax - 2, mid + 4);
    else
        arrow << QPointF(ax - 4, mid - 2) << QPointF(ax + 4, mid - 2) << QPointF(ax, mid + 2.5);
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    p->setBrush(Theme::kLabelSecondary);
    p->drawPolygon(arrow);
    p->setRenderHint(QPainter::Antialiasing, false);

    const QFont titleFont = Theme::font(12, QFont::Bold);
    p->setFont(titleFont);
    p->setPen(Theme::kLabel);
    const QString title = (group.isEmpty() ? QCoreApplication::translate("ServerDelegate", "Other") : group).toUpper();
    const int titleWidth = qMin(QFontMetrics(titleFont).horizontalAdvance(title) + 6, bar.width() / 3);
    int x = bar.left() + 28;
    p->drawText(QRect(x, mid - 10, titleWidth + 2, 20), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, titleWidth));
    x += titleWidth + 12;

    p->setFont(Theme::font(11));
    p->setPen(Theme::kLabelSecondary);
    p->drawText(QRect(x, mid - 10, 60, 20), Qt::AlignVCenter | Qt::AlignLeft, QString::number(count));

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

    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool open = m_open.contains(index.data(ServerStore::IdRole).toString());
    const QString code = index.data(ServerStore::CountryRole).toString();
    const QRect rect = inner(opt.rect, m_list).adjusted(0, 0, -1, -1);
    const Geometry g = m_list ? rowGeometry(rect, !code.isEmpty()) : cardGeometry(rect, !code.isEmpty());

    const int state = index.data(ServerStore::StatusRole).toInt();
    const bool offline = state == ServerStatus::Offline;
    const bool online = state == ServerStatus::Online;
    const bool busy = (online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Busy);

    // Surface. A row is a line of the group's table: boxed at the sides, divided from the next one, with the
    // last one closing the box. A card is a flat tile with a thin outline.
    const QColor fill = hover ? Theme::kCardHover : Theme::kCard;
    // A server somebody is on gets a flat, faint tint of the busy colour: it reads on both themes, where a
    // fading wash turned muddy against the dark card and against the white one.
    auto wash = [&](const QRect &area) {
        QColor tint = Theme::kBusy;
        tint.setAlpha(Theme::mode() == Theme::Dark ? 26 : 20);
        p->fillRect(area, tint);
    };
    if (m_list) {
        const QModelIndex next = index.sibling(index.row() + 1, 0);
        const bool last = !next.isValid() || next.data(ServerSectionModel::HeaderRole).toBool();
        p->fillRect(rect, fill);
        if (busy)
            wash(rect.adjusted(1, 0, 0, 0));
        p->setPen(Theme::kSeparatorStrong);
        p->drawLine(rect.left(), rect.top(), rect.left(), rect.bottom());
        p->drawLine(rect.right(), rect.top(), rect.right(), rect.bottom());
        p->setPen(last ? Theme::kSeparatorStrong : Theme::kSeparator);
        p->drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());
        if (open) {
            QColor tint = Theme::kBlue;
            tint.setAlpha(24);
            p->fillRect(rect.adjusted(1, 0, 0, -1), tint);
            p->fillRect(QRect(rect.left(), rect.top(), 3, rect.height()), Theme::kBlue);
        }
    } else {
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setBrush(fill);
        p->setPen(QPen(open ? Theme::kBlue : (hover ? Theme::kLabelTertiary : Theme::kSeparatorStrong), 1));
        p->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (busy) {
            QPainterPath clip;
            clip.addRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), 2.5, 2.5);
            p->save();
            p->setClipPath(clip);
            wash(rect);
            p->restore();
        }
        p->setRenderHint(QPainter::Antialiasing, false);
        if (open)
            p->fillRect(QRect(rect.left() + 1, rect.top() + 3, 3, rect.height() - 5), Theme::kBlue);
    }
    if (offline)
        p->setOpacity(0.6);

    // Status marker
    const QColor statusColor = index.data(ServerStore::StatusColorRole).value<QColor>();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    p->setBrush(statusColor);
    p->drawEllipse(g.dot);
    p->setRenderHint(QPainter::Antialiasing, false);

    // Name, and the mark of a server that is open in a tab
    const QFont nameFont = Theme::font(13, QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(Theme::kLabel);
    const int nameWidth = g.name.width() - (open ? g.pill.width() + 4 : 0);
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

    // Status in words. For a server in use the person comes first, in bold and the plain text colour, and then
    // whether they are at the computer.
    const QString badge = index.data(ServerStore::StatusBadgeRole).toString();
    if (busy) {
        QString detail;
        const int idle = index.data(ServerStore::OwnerIdleRole).toInt();
        if (idle >= 0)
            detail = QStringLiteral("  ·  ") + idleShort(idle);
        const QFont whoFont = Theme::font(13, QFont::Bold);
        const QFontMetrics whoMetrics(whoFont);
        const int whoWidth = qMin(whoMetrics.horizontalAdvance(badge) + 6, g.status.width());
        const QString who = whoMetrics.elidedText(badge, Qt::ElideRight, whoWidth);
        const int detailWidth = qMax(0, g.status.width() - whoWidth);
        const QString rest = textMetrics.elidedText(detail, Qt::ElideRight, detailWidth);
        const int total = whoWidth + textMetrics.horizontalAdvance(rest);
        const int x = m_list ? g.status.left() : g.status.right() + 1 - total;
        p->setFont(whoFont);
        p->setPen(Theme::kBusyText); // the person on the server: bold and red, readable on either theme
        p->drawText(QRect(x, g.status.top(), whoWidth, g.status.height()), Qt::AlignVCenter | Qt::AlignLeft, who);
        p->setFont(textFont);
        p->setPen(Theme::kLabelSecondary);
        p->drawText(QRect(x + whoWidth, g.status.top(), g.status.width() - whoWidth, g.status.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, rest);
    } else {
        const bool blind = online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::OccupancyError;
        p->setPen(blind ? Theme::kOrange : offline ? Theme::kLabelTertiary : Theme::kLabelSecondary);
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
    if (favorite || hover)
        drawStar(p, g.star, favorite);

    // Connect button of a row
    if (m_list) {
        p->setOpacity(1.0);
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setBrush(hover ? Theme::kBlue : Theme::kBackground);
        p->setPen(QPen(hover ? Theme::kBlue : Theme::kLabelTertiary, 1));
        p->drawRoundedRect(QRectF(g.button).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p->setRenderHint(QPainter::Antialiasing, false);
        p->setFont(Theme::font(12, QFont::DemiBold));
        p->setPen(hover ? QColor(Qt::white) : Theme::kLabel);
        p->drawText(g.button, Qt::AlignCenter, QCoreApplication::translate("ServerDelegate", "Connect"));
    }

    p->restore();
}
