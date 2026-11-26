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
                            channel(from.blueF(), to.blueF()), channel(from.alphaF(), to.alphaF()));
}

QColor compositeOver(const QColor &fg, const QColor &bg)
{
    const qreal a = fg.alphaF();
    return QColor::fromRgbF(fg.redF() * a + bg.redF() * (1.0 - a),
                            fg.greenF() * a + bg.greenF() * (1.0 - a),
                            fg.blueF() * a + bg.blueF() * (1.0 - a));
}

// A row is drawn inside its item, between the side margins; a card fills its item.
QRect inner(const QRect &item, bool list)
{
    return list ? item.adjusted(kSide, 0, -kSide, 0) : item;
}

// Every rectangle of an item, so painting and tooltips agree.
struct Geometry {
    QRect icon, dot, name, pill, star, latency, flag, host, footer, status, activity, button;
};

Geometry cardGeometry(const QRect &card, bool hasFlag)
{
    Geometry g;
    const int left = card.left() + kPad;
    const int right = card.right() + 1 - kPad;
    const int top = card.top() + 10;
    const int addressTop = card.top() + 31;

    g.icon = QRect(left, card.top() + 11, 36, 36);
    g.dot = QRect(g.icon.right() - 5, g.icon.bottom() - 5, kDot, kDot);
    const int textLeft = g.icon.right() + 12;

    g.star = QRect(right - 15, top, 15, 18);
    g.latency = QRect(g.star.left() - 6 - 46, top, 46, 18);
    g.name = QRect(textLeft, top, g.latency.left() - 8 - textLeft, 18);
    g.pill = QRect(g.name.right() + 1 - 40, top, 40, 18);

    const int hostLeft = textLeft + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft, addressTop + (16 - kFlagHeight) / 2, kFlagWidth, kFlagHeight);
    g.host = QRect(hostLeft, addressTop, right - hostLeft, 16);

    g.footer = QRect(card.left(), card.bottom() - 28, card.width(), 29);
    g.status = QRect(left, card.bottom() - 26, right - left, 20);
    g.activity = g.status;
    return g;
}

Geometry rowGeometry(const QRect &row, bool hasFlag)
{
    Geometry g;
    const int left = row.left() + kPad;
    const int right = row.right() + 1 - kPad;
    const int mid = row.center().y();

    g.button = QRect(right - kButtonWidth, mid - kButtonHeight / 2, kButtonWidth, kButtonHeight);
    g.latency = QRect(g.button.left() - 10 - 52, mid - 9, 52, 18);
    g.star = QRect(g.latency.left() - 6 - 16, mid - 9, 16, 18);
    g.dot = QRect(left, mid - kDot / 2, kDot, kDot);
    const int textLeft = left + kDot + 10;
    const int statusRight = g.star.left() - 12;
    const int room = statusRight - textLeft;
    const int statusWidth = qMin(qMax(230, room / 2), room * 2 / 3);
    const int statusLeft = statusRight - statusWidth;
    const int nameRight = statusLeft - 12;
    g.name = QRect(textLeft, mid - 18, nameRight - textLeft, 18);
    g.pill = QRect(nameRight - 40, g.name.top(), 40, 18);
    const int hostLeft = textLeft + (hasFlag ? kFlagWidth + 6 : 0);
    g.flag = QRect(textLeft, mid + 4, kFlagWidth, kFlagHeight);
    g.host = QRect(hostLeft, mid + 2, nameRight - hostLeft, 16);
    g.status = QRect(statusLeft, mid - 20, statusWidth, 20);
    g.activity = QRect(statusLeft, mid + 3, statusWidth, 16);
    return g;
}

const QFontMetrics &cachedMetrics(int pixelSize, QFont::Weight weight = QFont::Normal)
{
    static QHash<int, QFontMetrics> cache;
    const int key = pixelSize * 1000 + int(weight);
    auto it = cache.constFind(key);
    if (it != cache.cend())
        return it.value();
    return cache.insert(key, QFontMetrics(Theme::font(pixelSize, weight))).value();
}

// High-contrast person badge (dot + bold name) so whoever is connected stands out at a single glance.
int drawPersonBadge(QPainter *p, const QRect &bounds, const QString &name,
                    const QColor &dotColor, const QColor &textColor, const QColor &bgColor)
{
    if (name.isEmpty() || bounds.width() < 28)
        return 0;
    const QFont f = Theme::font(12, QFont::Bold);
    const QFontMetrics &fm = cachedMetrics(12, QFont::Bold);
    constexpr int padX = 8;
    constexpr int dotW = 11;
    const int maxTextW = qMax(10, bounds.width() - padX * 2 - dotW);
    const QString elided = fm.elidedText(name, Qt::ElideRight, maxTextW);
    const int w = qMin(bounds.width(), fm.horizontalAdvance(elided) + padX * 2 + dotW);
    const int h = 21;
    const QRect r(bounds.left(), bounds.center().y() - h / 2, w, h);

    p->fillRect(r, bgColor);
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(Qt::NoPen);
    p->setBrush(dotColor);
    p->drawEllipse(QRectF(r.left() + padX, r.center().y() - 3.0, 6.0, 6.0));
    p->restore();

    p->setFont(f);
    p->setPen(textColor);
    p->drawText(QRect(r.left() + padX + dotW, r.top(), r.width() - padX * 2 - dotW, r.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided);
    return w;
}

// Draws clean unboxed activity + duration text ("Aktif · 12d" or "2s 47d boşta · 2s 49d").
void drawActivityMeta(QPainter *p, const QRect &bounds, bool alignRight,
                      const QString &primary, const QColor &primaryColor,
                      const QString &secondary, const QColor &secondaryColor)
{
    if (bounds.width() < 20 || (primary.isEmpty() && secondary.isEmpty()))
        return;
    const QFont primaryFont = Theme::font(11, QFont::DemiBold);
    const QFont secondaryFont = Theme::font(11, QFont::Normal);
    const QFontMetrics &pfm = cachedMetrics(11, QFont::DemiBold);
    const QFontMetrics &sfm = cachedMetrics(11, QFont::Normal);
    static const QString sep = QStringLiteral("  ·  ");
    const int sepW = (!primary.isEmpty() && !secondary.isEmpty()) ? sfm.horizontalAdvance(sep) : 0;
    const int pW = primary.isEmpty() ? 0 : pfm.horizontalAdvance(primary);
    const int sW = secondary.isEmpty() ? 0 : sfm.horizontalAdvance(secondary);

    const bool showBoth = !primary.isEmpty() && !secondary.isEmpty() && (pW + sepW + sW <= bounds.width());
    if (!showBoth) {
        const bool usePrimary = !primary.isEmpty();
        const QString &only = usePrimary ? primary : secondary;
        const QFont &f = usePrimary ? primaryFont : secondaryFont;
        const QFontMetrics &fm = usePrimary ? pfm : sfm;
        const QColor &c = usePrimary ? primaryColor : secondaryColor;
        p->setFont(f);
        p->setPen(c);
        p->drawText(bounds, Qt::AlignVCenter | (alignRight ? Qt::AlignRight : Qt::AlignLeft),
                    fm.elidedText(only, Qt::ElideRight, bounds.width()));
        return;
    }

    const int totalW = pW + sepW + sW;
    int x = alignRight ? (bounds.right() + 1 - totalW) : bounds.left();
    p->setFont(primaryFont);
    p->setPen(primaryColor);
    p->drawText(QRect(x, bounds.top(), pW + 2, bounds.height()), Qt::AlignVCenter | Qt::AlignLeft, primary);
    x += pW;
    p->setFont(secondaryFont);
    p->setPen(Theme::kLabelTertiary);
    p->drawText(QRect(x, bounds.top(), sepW + 2, bounds.height()), Qt::AlignVCenter | Qt::AlignLeft, sep);
    x += sepW;
    p->setPen(secondaryColor);
    p->drawText(QRect(x, bounds.top(), sW + 2, bounds.height()), Qt::AlignVCenter | Qt::AlignLeft, secondary);
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
    static const QPainterPath unitStar = [] {
        QPainterPath path;
        for (int k = 0; k < 10; ++k) {
            const qreal r = k % 2 ? 2.5 : 5.6;
            const qreal a = -M_PI / 2 + k * M_PI / 5;
            const QPointF pt(r * std::cos(a), 0.5 + r * std::sin(a));
            if (k == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        path.closeSubpath();
        return path;
    }();
    const QPointF c = QRectF(box).center();
    p->save();
    p->translate(c);
    p->setOpacity(opacity);
    p->setRenderHint(QPainter::Antialiasing);
    p->setBrush(filled ? QBrush(Theme::kFavorite) : Qt::NoBrush);
    const QColor outline = Theme::mode() == Theme::Dark ? Theme::kFavorite : Theme::kOrange;
    p->setPen(QPen(filled ? outline : Theme::kLabelTertiary, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->drawPath(unitStar);
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
    p->setPen(QPen(blend(Theme::kLabelSecondary, Theme::kLabel, hover), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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
    const QFontMetrics &metrics = cachedMetrics(13, QFont::DemiBold);
    p->setFont(titleFont);
    p->setPen(blend(Theme::kLabelSecondary, Theme::kLabel, 0.75 + 0.25 * hover));
    const QString title = group == ServerSectionModel::favoritesSectionKey()
        ? QCoreApplication::translate("ServerDelegate", "Favourites")
        : group.isEmpty() ? QCoreApplication::translate("ServerDelegate", "Other") : group;
    const int titleWidth = qMin(metrics.horizontalAdvance(title) + 2, item.width() / 2);
    const int x = item.left() + 25;
    p->drawText(QRect(x, mid - 10, titleWidth, 20), Qt::AlignVCenter | Qt::AlignLeft,
                metrics.elidedText(title, Qt::ElideRight, titleWidth));

    const QString countText = QString::number(count);
    const QFont countFont = Theme::font(11, QFont::DemiBold);
    const int countWidth = qMax(24, cachedMetrics(11, QFont::DemiBold).horizontalAdvance(countText) + 12);
    const QRect countRect(x + titleWidth + 7, mid - 9, countWidth, 18);
    p->fillRect(countRect, Theme::kCard);
    p->setFont(countFont);
    p->setPen(Theme::kLabelSecondary);
    p->drawText(countRect, Qt::AlignCenter, countText);

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
    const bool dark = Theme::mode() == Theme::Dark;
    const QColor base = offline ? (dark ? QColor("#0b0b0d") : QColor("#f5f5f7"))
        : connected ? Theme::kConnectedSurface
        : busy ? Theme::kBusySurface
        : Theme::kCard;
    const qreal interaction = index == m_pressed ? 0.14 : 0.065 * hover;
    const QColor fill = blend(base, dark ? QColor("#ffffff") : QColor("#000000"), interaction * (dark ? 0.45 : 0.12));
    const QColor dotHalo = compositeOver(fill, Theme::kBackground);
    const QColor accentBar = connected ? Theme::kGreen : open ? Theme::kTabActiveAccent : busy ? Theme::kBusy : QColor();
    if (m_list) {
        const QModelIndex next = index.sibling(index.row() + 1, 0);
        const bool last = !next.isValid() || next.data(ServerSectionModel::HeaderRole).toBool();
        p->fillRect(rect, fill);
        if (accentBar.isValid())
            p->fillRect(QRect(rect.left(), rect.top(), 3, rect.height()), accentBar);
        if (!last) {
            p->setPen(Theme::kSeparator);
            p->drawLine(QPoint(rect.left() + kPad + kDot + 10, rect.bottom()), QPoint(rect.right() - kPad, rect.bottom()));
        }
    } else {
        p->fillRect(rect, fill);
        if (accentBar.isValid())
            p->fillRect(QRect(rect.left(), rect.top(), 3, rect.height()), accentBar);

        // Neutral monochrome icon tile by default; semantic tint ONLY for active occupancy/connection.
        QColor tileBg = dark ? QColor("#18181b") : QColor("#efeff3");
        QColor tileStroke = dark ? QColor("#7c7c86") : QColor("#6b7280");
        if (connected) {
            tileBg = Theme::kGreen;
            tileBg.setAlpha(dark ? 34 : 26);
            tileStroke = Theme::kGreen;
        } else if (open) {
            tileBg = Theme::kAccentSoft;
            tileStroke = Theme::kBlueText;
        } else if (busy) {
            tileBg = Theme::kBusy;
            tileBg.setAlpha(dark ? 34 : 24);
            tileStroke = Theme::kBusyText;
        }
        p->fillRect(g.icon, tileBg);
        const QPointF centre = QRectF(g.icon).center();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(QPen(tileStroke, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawRect(QRectF(centre.x() - 7.5, centre.y() - 6.5, 15, 10));
        p->drawLine(centre + QPointF(0, 3.5), centre + QPointF(0, 6.5));
        p->drawLine(centre + QPointF(-3, 6.5), centre + QPointF(3, 6.5));
        p->setRenderHint(QPainter::Antialiasing, false);
    }

    // Status marker
    const QColor statusColor = index.data(ServerStore::StatusColorRole).value<QColor>();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(m_list ? QPen(Qt::NoPen) : QPen(dotHalo, 2));
    p->setBrush(statusColor);
    p->drawEllipse(g.dot);
    p->setRenderHint(QPainter::Antialiasing, false);

    // Name, and the mark of a server that is open in a tab
    const QFont nameFont = Theme::font(14, QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(offline ? Theme::kLabelSecondary : Theme::kLabel);
    const int nameWidth = qMax(0, g.name.width() - (open ? g.pill.width() + 6 : 0));
    p->drawText(QRect(g.name.left(), g.name.top(), nameWidth, g.name.height()), Qt::AlignVCenter | Qt::AlignLeft,
                cachedMetrics(14, QFont::DemiBold).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameWidth));
    if (open) {
        QColor pillBg = connected ? Theme::kGreen : Theme::kBlue;
        pillBg.setAlpha(dark ? 40 : 26);
        p->fillRect(g.pill.adjusted(0, 1, 0, -1), pillBg);
        p->setFont(Theme::font(10, QFont::DemiBold));
        p->setPen(connected ? Theme::kGreen : Theme::kBlueText);
        p->drawText(g.pill, Qt::AlignCenter,
                    QCoreApplication::translate("ServerDelegate", "OPEN"));
    }

    // Address with the country flag
    const QPixmap flag = flagPixmap(code, p->device()->devicePixelRatioF());
    if (!flag.isNull())
        p->drawPixmap(g.flag.topLeft(), flag);
    const QFont textFont = Theme::font(12);
    p->setFont(textFont);
    p->setPen(Theme::kLabelTertiary);
    p->drawText(g.host, Qt::AlignVCenter | Qt::AlignLeft,
                cachedMetrics(12).elidedText(index.data(ServerStore::HostRole).toString(), Qt::ElideRight, g.host.width()));

    // Status row: high-contrast person badge for Busy / Connected, clean unboxed metadata on the right.
    const QString badge = index.data(ServerStore::StatusBadgeRole).toString();
    if (connected && !badge.isEmpty()) {
        drawPersonBadge(p, g.status, badge, Theme::kGreen,
                        dark ? QColor("#ffffff") : QColor("#115e32"),
                        dark ? QColor("#132b1e") : QColor("#dcf5e6"));
    } else if (busy) {
        const qint64 connMin = index.data(ServerStore::OwnerConnectionMinutesRole).toLongLong();
        const int idle = index.data(ServerStore::OwnerIdleRole).toInt();
        const QString idleTextStr = idle >= 0 ? idleShort(idle) : QString();
        const QString durTextStr = connMin >= 0 ? compactDuration(connMin) : QString();
        const bool isActive = idle == 0;

        const QColor personBg = dark ? QColor("#4d3fc2") : QColor("#5b50cc");
        const QColor personFg = QColor("#ffffff");
        const QColor personDot = QColor("#e3dfff");
        const QColor metaPrimary = isActive ? Theme::kGreen : Theme::kBlueText;
        const QColor metaSecondary = Theme::kLabelSecondary;

        if (m_list) {
            drawPersonBadge(p, g.status, badge, personDot, personFg, personBg);
            drawActivityMeta(p, g.activity, false, idleTextStr, metaPrimary, durTextStr, metaSecondary);
        } else {
            const int maxBadgeW = qMax(80, g.status.width() * 55 / 100);
            const int badgeW = drawPersonBadge(
                p, QRect(g.status.left(), g.status.top(), maxBadgeW, g.status.height()),
                badge, personDot, personFg, personBg);
            const QRect metaRect(g.status.left() + badgeW + 10, g.status.top(),
                                 qMax(0, g.status.width() - badgeW - 10), g.status.height());
            drawActivityMeta(p, metaRect, true, idleTextStr, metaPrimary, durTextStr, metaSecondary);
        }
    } else if (!badge.isEmpty()) {
        const bool queryFailed = online && index.data(ServerStore::OccupancyRole).toInt() == ServerStatus::OccupancyError;
        p->setFont(Theme::font(11, QFont::Medium));
        p->setPen(queryFailed ? Theme::kOrange : Theme::kLabelTertiary);
        p->drawText(g.status, Qt::AlignVCenter | Qt::AlignLeft,
                    cachedMetrics(11, QFont::Medium).elidedText(badge, Qt::ElideRight, g.status.width()));
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
        p->fillRect(g.button, blend(Theme::kAccentSoft, dark ? QColor("#ffffff") : QColor("#000000"),
                                    interaction * (dark ? 0.5 : 0.12)));
        p->setFont(Theme::font(12, QFont::DemiBold));
        p->setPen(Theme::kBlueText);
        p->drawText(g.button, Qt::AlignCenter, QCoreApplication::translate("ServerDelegate", "Connect"));
    }

    p->restore();
}
