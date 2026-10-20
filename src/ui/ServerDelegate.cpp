#include "ServerDelegate.h"
#include "core/GeoIp.h"
#include "model/ServerStore.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QPainter>
#include <QPixmapCache>
#include <QToolTip>

namespace {
constexpr int kCardWidth = 236;
constexpr int kCardHeight = 68;
constexpr int kFlagWidth = 16;
constexpr int kFlagHeight = 11;

QRect flagRect(const QRect &card)
{
    return QRect(card.left() + 29, card.top() + 51, kFlagWidth, kFlagHeight);
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

} // namespace

bool ServerDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    const QString code = index.data(ServerStore::CountryRole).toString();
    if (event->type() == QEvent::ToolTip && !code.isEmpty()
        && flagRect(option.rect).adjusted(-2, -2, 2, 2).contains(event->pos())) {
        QToolTip::showText(event->globalPos(), GeoIp::countryName(code), view);
        return true;
    }
    QToolTip::hideText();
    return true;
}

QSize ServerDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(kCardWidth, kCardHeight);
}

void ServerDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    p->save();

    const QRect card = opt.rect.adjusted(0, 0, -1, -1);
    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool open = m_open.contains(index.data(ServerStore::IdRole).toString());

    p->fillRect(card, hover ? QColor(0x30, 0x30, 0x30) : QColor(0x27, 0x27, 0x27));
    p->setPen(open ? QColor(0x3a, 0x96, 0xdd) : (hover ? QColor(0x4a, 0x4a, 0x4a) : QColor(0x38, 0x38, 0x38)));
    p->drawRect(card);

    p->fillRect(QRect(card.left() + 1, card.top() + 1, 3, card.height() - 1),
                QColor(index.data(ServerStore::ColorRole).toString()));

    const int left = card.left() + 16;
    const int right = card.right() - 12;

    QFont nameFont = opt.font;
    nameFont.setPixelSize(13);
    nameFont.setWeight(QFont::DemiBold);
    p->setFont(nameFont);
    p->setPen(QColor(0xf0, 0xf0, 0xf0));
    const int nameWidth = right - left - (open ? 44 : 0);
    p->drawText(QRect(left, card.top() + 10, nameWidth, 18), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameWidth));

    if (open) {
        QFont f = opt.font;
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        p->setFont(f);
        p->setPen(QColor(0x5c, 0xb0, 0xee));
        p->drawText(QRect(right - 44, card.top() + 10, 44, 18), Qt::AlignVCenter | Qt::AlignRight,
                    QCoreApplication::translate("ServerDelegate", "OPEN"));
    }

    const auto state = static_cast<ServerStatus::State>(index.data(ServerStore::StatusRole).toInt());
    const int lineY = card.top() + 34;
    p->fillRect(QRect(left, lineY + 5, 7, 7), statusColor(state));

    QFont small = opt.font;
    small.setPixelSize(11);
    p->setFont(small);
    p->setPen(QColor(0xa8, 0xa8, 0xa8));
    const int statusWidth = right - left - 13;
    p->drawText(QRect(left + 13, lineY, statusWidth, 17), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(small).elidedText(index.data(ServerStore::StatusTextRole).toString(), Qt::ElideRight, statusWidth));

    int hostLeft = left + 13;
    const QPixmap flag = flagPixmap(index.data(ServerStore::CountryRole).toString(), p->device()->devicePixelRatioF());
    if (!flag.isNull()) {
        p->drawPixmap(flagRect(card).topLeft(), flag);
        hostLeft += kFlagWidth + 6;
    }
    p->setPen(QColor(0x78, 0x78, 0x78));
    const int hostWidth = right - hostLeft;
    p->drawText(QRect(hostLeft, card.top() + 49, hostWidth, 15), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(small).elidedText(index.data(ServerStore::HostRole).toString(), Qt::ElideRight, hostWidth));

    p->restore();
}
