#pragma once
#include <QHash>
#include <QPersistentModelIndex>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVariantAnimation>

class QAbstractItemView;

// Paints one server as a card (grid) or as a single row (list): status, name, address, connection quality.
class ServerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ServerDelegate(QAbstractItemView *view);

    static constexpr int kCardHeight = 84;
    static constexpr int kRowHeight = 52;
    static constexpr int kHeaderHeight = 34;
    static constexpr int kMinCardWidth = 340;

    bool setOpenSessionIds(const QSet<QString> &open);
    void setHovered(const QModelIndex &index);
    void setPressed(const QModelIndex &index);
    // Where the favourite star is, for the item rectangle of the view.
    QRect starRect(const QRect &itemRect) const;
    void setCardWidth(int width) { m_cardWidth = width; }
    int cardWidth() const { return m_cardWidth; }
    // Group headers span the whole row of cards. `gap` is the view's spacing between cards.
    void setColumns(int columns, int gap)
    {
        m_columns = columns;
        m_gap = gap;
    }
    // One server per row, with a Connect button, instead of cards.
    void setListMode(bool list) { m_list = list; }
    bool listMode() const { return m_list; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

private:
    void paintHeader(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    qreal hoverAmount(const QPersistentModelIndex &index) const;

    QAbstractItemView *m_view;
    QSet<QString> m_open;
    QPersistentModelIndex m_hovered;
    QPersistentModelIndex m_pressed;
    QHash<QPersistentModelIndex, qreal> m_hoverFrom;
    QVariantAnimation m_hover;
    int m_cardWidth = kMinCardWidth;
    int m_columns = 1;
    int m_gap = 6;
    bool m_list = false;
};
