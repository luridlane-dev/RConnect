#include "ServerDelegate.h"
#include "Theme.h"
#include "core/GeoIp.h"
#include "model/ServerStore.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QToolTip>

#include <cmath>

namespace {

constexpr int kPad = 14;
constexpr int kIconSize = 48;
constexpr int kFlagWidth = 16;
constexpr int kFlagHeight = 11;
constexpr int kBarWidth = 3;
constexpr int kBarGap = 2;
constexpr int kBarCount = 4;
constexpr int kBarsWidth = kBarCount * kBarWidth + (kBarCount - 1) * kBarGap;
constexpr int kBarsHeight = 13;
constexpr int kBadgeHeight = 20;

// Every rectangle of a card, so painting and tooltips agree.
struct Geometry {
    QRect card, icon, name, pill, star, status, flag, host, latency, bars;
};

Geometry geometry(const QRect &card, bool hasFlag)
{
    Geometry g;
    g.card = card;
    g.icon = QRect(card.left() + kPad, card.center().y() - kIconSize / 2 + 1, kIconSize, kIconSize);
    const int left = g.icon.right() + 1 + 12;
    const int right = card.right() + 1 - kPad;
    const int top = card.top() + 13;

    g.star = QRect(right - 16, top, 16, 18);
    g.name = QRect(left, top, right - 22 - left, 18);
    g.pill = QRect(g.star.left() - 6 - 42, top, 42, 18);
    g.status = QRect(left, top + 21, right - left, kBadgeHeight);
    const int hostLeft = left + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(left, top + 46 + (14 - kFlagHeight) / 2, kFlagWidth, kFlagHeight);
    g.bars = QRect(right - kBarsWidth, top + 46 + (14 - kBarsHeight) / 2 + 1, kBarsWidth, kBarsHeight);
    g.latency = QRect(g.bars.left() - 6 - 52, top + 46, 52, 14);
    g.host = QRect(hostLeft, top + 46, g.latency.left() - 6 - hostLeft, 14);
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

QColor barColor(int bars)
{
    return bars >= 3 ? Theme::kGreen : (bars == 2 ? Theme::kOrange : Theme::kRed);
}

// A flat status label in `color`: optional square marker, then text. Returns the width used.
int drawBadge(QPainter *p, const QRect &avail, const QColor &color, const QString &text, bool dot)
{
    const QFont font = Theme::font(11, QFont::DemiBold);
    const QFontMetrics fm(font);
    constexpr int padLeft = 0, padRight = 0, dotSize = 6, dotGap = 6;
    const bool dotOnly = text.isEmpty();
    const int reserved = padLeft + padRight + (dot ? dotSize + dotGap : 0);
    const QString shown = fm.elidedText(text, Qt::ElideRight, qMax(0, avail.width() - reserved));
    const int width = dotOnly ? 24 : reserved + fm.horizontalAdvance(shown);
    const QRect r(avail.left(), avail.top(), qMin(width, avail.width()), kBadgeHeight);

    p->setPen(Qt::NoPen);
    if (dot) {
        const int dotX = dotOnly ? r.center().x() - dotSize / 2 : r.left() + padLeft;
        p->setBrush(color);
        p->drawRect(QRect(dotX, r.center().y() - dotSize / 2 + 1, dotSize, dotSize));
    }
    if (!dotOnly) {
        p->setFont(font);
        p->setPen(color);
        p->drawText(QRect(r.left() + padLeft + (dot ? dotSize + dotGap : 0), r.top(), r.width(), r.height()),
                    Qt::AlignVCenter | Qt::AlignLeft, shown);
    }
    return r.width();
}

} // namespace

QRect ServerDelegate::starRect(const QRect &itemRect)
{
    return geometry(itemRect.adjusted(0, 0, -1, -1), false).star.adjusted(-6, -6, 6, 6);
}

bool ServerDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    if (event->type() != QEvent::ToolTip) {
        QToolTip::hideText();
        return true;
    }
    const QString code = index.data(ServerStore::CountryRole).toString();
    const Geometry g = geometry(option.rect.adjusted(0, 0, -1, -1), !code.isEmpty());
    const QPoint pos = event->pos();
    QString text;
    if (!code.isEmpty() && g.flag.adjusted(-3, -3, 3, 3).contains(pos))
        text = GeoIp::countryName(code);
    else if (g.status.united(g.latency).united(g.bars).adjusted(-3, -3, 3, 3).contains(pos))
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

QSize ServerDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(m_cardWidth, kCardHeight);
}

void ServerDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    p->save();
    p->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool open = m_open.contains(index.data(ServerStore::IdRole).toString());
    const QString code = index.data(ServerStore::CountryRole).toString();
    const QRect cardRect = opt.rect.adjusted(0, 0, -1, -1);
    const Geometry g = geometry(cardRect, !code.isEmpty());

    // Card
    // The card hints at what the server is doing without shouting: a faint warm wash when someone is on it, a
    // fainter green when it is free, dimmed when it does not answer.
    const int stateNow = index.data(ServerStore::StatusRole).toInt();
    const bool busyCard = stateNow == ServerStatus::Online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Busy;
    const bool freeCard = stateNow == ServerStatus::Online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Free;
    const bool offlineCard = stateNow == ServerStatus::Offline;
    const QRectF flat = QRectF(cardRect).adjusted(0.5, 0.5, -0.5, -0.5);
    p->fillRect(flat, hover ? Theme::kCardHover : Theme::kCard);
    if (busyCard || freeCard) {
        QColor wash = busyCard ? Theme::kOrange : Theme::kGreen;
        wash.setAlpha((busyCard ? 20 : 9) + (hover ? 6 : 0));
        p->fillRect(flat, wash);
    }
    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(open ? Theme::kBlue : (hover ? Theme::kSeparatorStrong : Theme::kSeparator), 1.0));
    p->drawRect(flat);
    if (offlineCard)
        p->setOpacity(0.55);

    // Icon: the server colour, faded, as a flat square with a monitor on it
    const QColor chosen = QColor(index.data(ServerStore::ColorRole).toString()).toHsl();
    const QColor base = QColor::fromHsl(qMax(0, chosen.hue()), chosen.saturation() * 40 / 100,
                                        qBound(70, chosen.lightness() * 75 / 100, 140));
    p->setPen(Qt::NoPen);
    p->setBrush(base);
    p->drawRect(g.icon);
    const QPointF c = g.icon.center() + QPointF(0.5, 0.5);
    const QRectF screen(c.x() - 13, c.y() - 13, 26, 18);
    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(QColor(255, 255, 255, 225), 1.8, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    p->drawRect(screen);
    p->drawLine(QPointF(c.x(), screen.bottom() + 1), QPointF(c.x(), screen.bottom() + 5));
    p->drawLine(QPointF(c.x() - 6, screen.bottom() + 6), QPointF(c.x() + 6, screen.bottom() + 6));

    // Favourite star: always there when set, otherwise only while the pointer is on the card
    const bool favorite = index.data(ServerStore::FavoriteRole).toBool();
    if (favorite || hover) {
        const QPointF c = QRectF(g.star).center();
        QPainterPath star;
        for (int k = 0; k < 10; ++k) {
            const qreal r = k % 2 ? 3.4 : 7.6;
            const qreal a = -M_PI / 2 + k * M_PI / 5;
            const QPointF pt(c.x() + r * std::cos(a), c.y() + 0.6 + r * std::sin(a));
            if (k == 0)
                star.moveTo(pt);
            else
                star.lineTo(pt);
        }
        star.closeSubpath();
        p->setBrush(favorite ? QBrush(Theme::kOrange) : Qt::NoBrush);
        p->setPen(QPen(favorite ? Theme::kOrange : Theme::kLabelTertiary, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPath(star);
    }

    // Name, and a pill while the server is open in a tab
    const QFont nameFont = Theme::font(14, QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(Theme::kLabel);
    const int nameWidth = g.name.width() - (open ? g.pill.width() + 8 : 0);
    p->drawText(QRect(g.name.left(), g.name.top(), nameWidth, g.name.height()), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameWidth));
    if (open) {
        QColor fill = Theme::kBlue;
        fill.setAlpha(44);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRect(g.pill);
        QFont f = Theme::font(10, QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        p->setFont(f);
        p->setPen(Theme::kBlueText);
        p->drawText(g.pill, Qt::AlignCenter, QCoreApplication::translate("ServerDelegate", "OPEN"));
    }

    // Status badge; when someone is connected: their computer, and whether they are active or idle
    const QColor statusColor = index.data(ServerStore::StatusColorRole).value<QColor>();
    const bool busy = index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::Busy;
    const bool online = index.data(ServerStore::StatusRole).toInt() == ServerStatus::Online;
    int x = g.status.left();
    const int badgeLimit = g.status.right() + 1;
    if (busy) {
        // The person's name in a badge, and how long they have been idle as plain text. Both are measured and
        // elided, so a long computer name never runs out of the card.
        const int idle = index.data(ServerStore::OwnerIdleRole).toInt();
        const QFont idleFont = Theme::font(11, idle < 1 ? QFont::DemiBold : QFont::Normal);
        const QFontMetrics idleMetrics(idleFont);
        const QString idleLabel = idleShort(idle);
        const int idleWidth = qMin(idleMetrics.horizontalAdvance(idleLabel), (badgeLimit - x) * 2 / 5);
        const int badgeWidth = qMax(60, badgeLimit - x - idleWidth - 8);
        x += drawBadge(p, QRect(x, g.status.top(), badgeWidth, kBadgeHeight), statusColor,
                       index.data(ServerStore::StatusBadgeRole).toString(), true) + 8;
        if (badgeLimit - x > 20) {
            p->setFont(idleFont);
            p->setPen(idle < 1 ? Theme::kGreen : Theme::kLabelSecondary);
            p->drawText(QRect(x, g.status.top(), badgeLimit - x, kBadgeHeight), Qt::AlignVCenter | Qt::AlignLeft,
                        idleMetrics.elidedText(idleLabel, Qt::ElideRight, badgeLimit - x));
        }
    } else {
        drawBadge(p, QRect(x, g.status.top(), badgeLimit - x, kBadgeHeight), statusColor,
                  index.data(ServerStore::StatusBadgeRole).toString(), online);
    }

    // Address with the country flag
    const QPixmap flag = flagPixmap(code, p->device()->devicePixelRatioF());
    if (!flag.isNull())
        p->drawPixmap(g.flag.topLeft(), flag);
    const QFont hostFont = Theme::font(11);
    p->setFont(hostFont);
    p->setPen(Theme::kLabelTertiary);
    p->drawText(g.host, Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(hostFont).elidedText(index.data(ServerStore::HostRole).toString(), Qt::ElideRight,
                                                  g.host.width()));

    // Connection quality: connect time and signal bars
    const int bars = index.data(ServerStore::SignalRole).toInt();
    if (bars > 0) {
        const int latency = index.data(ServerStore::LatencyRole).toInt();
        if (latency >= 0) {
            p->setPen(Theme::kLabelSecondary);
            p->drawText(g.latency, Qt::AlignVCenter | Qt::AlignRight,
                        QCoreApplication::translate("ServerDelegate", "%1 ms").arg(latency));
        }
        p->setPen(Qt::NoPen);
        for (int i = 0; i < kBarCount; ++i) {
            const int h = 4 + i * 3;
            const QRect bar(g.bars.left() + i * (kBarWidth + kBarGap), g.bars.bottom() + 1 - h, kBarWidth, h);
            p->setBrush(i < bars ? barColor(bars) : Theme::kBarOff);
            p->drawRect(bar);
        }
    }

    p->restore();
}
