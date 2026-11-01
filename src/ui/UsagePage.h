#pragma once
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QStandardItemModel;
class QTimer;
class QTreeView;

// Who was on which server, and for how long, day by day (see UsageLog and UsageEngine). A row per day,
// server and computer; open it to see each visit with its start and end, and under that the stretches
// that were away or not known.
class UsagePage : public QWidget {
    Q_OBJECT
public:
    explicit UsagePage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void reload();
    void rebuild();
    void explain();

    QStandardItemModel *m_model;
    QTreeView *m_view;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLabel *m_warning;
    QLabel *m_total;
    QComboBox *m_range;
    QPushButton *m_refresh;
    QPushButton *m_rebuild;
    QPushButton *m_help;
    QTimer *m_timer;
};
