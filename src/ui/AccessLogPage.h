#pragma once
#include "core/AccessLog.h"

#include <QPair>
#include <QStringList>
#include <QWidget>

class AccessTableModel;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTimer;

// One row of the table: an entry of the file, or the failed attempts of one address in a row.
struct AccessLine {
    AccessRecord r;          // the newest entry
    QString firstUtc;        // the oldest, for a run of attempts
    int attempts = 1;
    QStringList users;       // accounts tried
};

// Table of access-log.csv: who logged on, failed to log on, left or was watched, newest first.
class AccessLogPage : public QWidget {
    Q_OBJECT
public:
    explicit AccessLogPage(QWidget *parent = nullptr);

    static QString eventText(const QString &event);
    static QString sourceText(const QString &source);
    // A sentence about a security alert or a change on the server; empty for other events.
    static QString alertSummary(const AccessRecord &r);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void reload();
    void apply(QList<AccessLine> lines);
    void showDetail();
    void updateCount();
    void fillServers();
    void applyPeriod();
    void exportCsv();

    AccessTableModel *m_model;
    QTableView *m_view;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLineEdit *m_filter;
    QComboBox *m_category;
    QComboBox *m_server;
    QComboBox *m_period;
    QLabel *m_count;
    QTimer *m_timer;
    QPushButton *m_refresh;
    QPushButton *m_export;
    QPushButton *m_open;
    QPair<qint64, qint64> m_stamp; // size and modification time of the file last shown
    bool m_loaded = false;
    bool m_busy = false;   // a read is under way
    bool m_again = false;  // the file changed meanwhile
    QList<AccessLine> m_lines;
    QFrame *m_detail = nullptr;
    QLabel *m_detailText = nullptr;
};
