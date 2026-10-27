#pragma once
#include <QSet>
#include <QStyledItemDelegate>

// Paints one server as a rounded card: icon, name, status, address, connection quality.
class ServerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int kCardHeight = 88;
    static constexpr int kMinCardWidth = 270;

    void setOpenIds(const QSet<QString> &ids) { m_open = ids; }
    void setCardWidth(int width) { m_cardWidth = width; }
    int cardWidth() const { return m_cardWidth; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

private:
    QSet<QString> m_open;
    int m_cardWidth = kMinCardWidth;
};
