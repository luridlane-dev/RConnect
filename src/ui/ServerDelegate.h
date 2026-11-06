#pragma once
#include <QSet>
#include <QStyledItemDelegate>

// Paints one server as a card (grid) or as a single row (list): status, name, address, connection quality.
class ServerDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int kCardHeight = 60;
    static constexpr int kRowHeight = 40;
    static constexpr int kHeaderHeight = 60;
    static constexpr int kMinCardWidth = 330;

    void setOpenIds(const QSet<QString> &ids) { m_open = ids; }
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

    QSet<QString> m_open;
    int m_cardWidth = kMinCardWidth;
    int m_columns = 1;
    int m_gap = 6;
    bool m_list = false;
};
