#pragma once
#include <QTabBar>
#include <QVariantAnimation>

class QToolButton;

// Tab bar with a sliding accent line, a "+" button and tabs that can be dragged to other windows.
class AnimatedTabBar : public QTabBar {
    Q_OBJECT
public:
    explicit AnimatedTabBar(QWidget *parent = nullptr);

    static constexpr int kHeight = 26;
    static const char *const kMimeType;

    QToolButton *addButton() const { return m_add; }

signals:
    void addRequested();
    // Dropped outside every tab bar.
    void detachRequested(const QString &serverId, const QPoint &globalPos);
    void tabDropped(const QString &serverId, int index);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    QSize tabSizeHint(int index) const override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;
    void tabLayoutChange() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void slideTo(int index);
    void placeAddButton();
    void startDrag(const QString &serverId, const QPoint &hotSpot);
    int insertIndexAt(const QPoint &pos) const;

    QVariantAnimation m_anim;
    QRectF m_from;
    QRectF m_current;
    QToolButton *m_add = nullptr;
    QString m_pressId;
    QPoint m_pressPos;
    int m_dropIndex = -1;
};
