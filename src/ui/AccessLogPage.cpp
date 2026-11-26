#include "AccessLogPage.h"
#include "model/ServerStore.h"

#include "ContactsDialog.h"
#include "Theme.h"

#include "core/AccessLog.h"
#include "core/Contacts.h"
#include "core/GeoIp.h"
#include "core/HostNames.h"
#include "core/Threats.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace {

enum Column { Time, Server, Event, Who, Address, Details, ColumnCount };
enum Category { All, Logons, Leaves, Failed, Alerts, Changes };

enum class EventTone { SignIn, Reconnect, Disconnect, Logoff, Failed, Alert, Change, App, Neutral };

constexpr int kLineRole = Qt::UserRole + 2;
constexpr int kToneRole = Qt::UserRole + 4;

// Period filter in hours (0 = all).
constexpr int kPeriodHours[] = {24, 24 * 7, 24 * 30, 0};
constexpr int kRefreshMs = 15000;

EventTone toneOfEvent(const QString &event)
{
    if (event.startsWith(QLatin1String("Alert")) || Threats::isTampering(event)
        || event == QLatin1String("AccountLocked"))
        return EventTone::Alert;
    if (event == QLatin1String("FailedLogon"))
        return EventTone::Failed;
    if (event == QLatin1String("Logon") || event == QLatin1String("SecurityLogon")
        || event == QLatin1String("Authenticated"))
        return EventTone::SignIn;
    if (event == QLatin1String("Reconnect") || event == QLatin1String("SecurityReconnect"))
        return EventTone::Reconnect;
    if (event == QLatin1String("Disconnect") || event == QLatin1String("SecurityDisconnect"))
        return EventTone::Disconnect;
    if (event == QLatin1String("Logoff"))
        return EventTone::Logoff;
    if (event.startsWith(QLatin1String("User")) || event.startsWith(QLatin1String("GroupMember"))
        || event.startsWith(QLatin1String("Task")) || event.startsWith(QLatin1String("Password"))
        || event == QLatin1String("ServiceInstalled"))
        return EventTone::Change;
    if (event == QLatin1String("Connect") || event == QLatin1String("Monitor")
        || event == QLatin1String("AuditEnable") || event == QLatin1String("Message")
        || event == QLatin1String("DisplayTuned") || event == QLatin1String("Closed")
        || event.startsWith(QLatin1String("Server")) || event == QLatin1String("Import")
        || event == QLatin1String("Update"))
        return EventTone::App;
    return EventTone::Neutral;
}

QColor colorForTone(EventTone tone)
{
    switch (tone) {
    case EventTone::SignIn:     return Theme::kGreen;
    case EventTone::Reconnect:  return Theme::kTeal;
    case EventTone::Disconnect: return Theme::kOrange;
    case EventTone::Logoff:     return Theme::kLabelSecondary;
    case EventTone::Failed:     return Theme::kRed;
    case EventTone::Alert:      return Theme::kRed;
    case EventTone::Change:     return Theme::kPurple;
    case EventTone::App:        return Theme::kLabelSecondary;
    case EventTone::Neutral:    return Theme::kLabelSecondary;
    }
    return Theme::kLabelSecondary;
}

bool inCategory(const QString &event, int category)
{
    switch (category) {
    case Logons:
        return event == QLatin1String("Logon") || event == QLatin1String("Authenticated")
            || event == QLatin1String("SecurityLogon") || event == QLatin1String("Reconnect")
            || event == QLatin1String("SecurityReconnect");
    case Leaves:
        return event == QLatin1String("Logoff") || event == QLatin1String("Disconnect")
            || event == QLatin1String("SecurityDisconnect");
    case Failed:
        return event == QLatin1String("FailedLogon");
    case Alerts:
        return event.startsWith(QLatin1String("Alert")) || Threats::isTampering(event);
    case Changes:
        return event.startsWith(QLatin1String("User")) || event.startsWith(QLatin1String("GroupMember"))
            || event.startsWith(QLatin1String("Task")) || event.startsWith(QLatin1String("Password"))
            || event == QLatin1String("ServiceInstalled") || event == QLatin1String("AccountLocked")
            || event == QLatin1String("AuditPolicyChanged") || event == QLatin1String("LogCleared")
            || event == QLatin1String("ServerConfigured") || event == QLatin1String("ServerEdited")
            || event == QLatin1String("ServerAdded") || event == QLatin1String("ServerRemoved");
    default:
        return true;
    }
}

QString detailText(const QString &detail)
{
    static const QRegularExpression format(QStringLiteral("^logon type (\\d+)(?:: (.+))?$"));
    const QRegularExpressionMatch m = format.match(detail);
    if (!m.hasMatch())
        return detail;
    const auto t = [](const char *text) { return QCoreApplication::translate("AccessLogPage", text); };
    static const QHash<QString, const char *> reasons = {
        {QStringLiteral("unknown user name"), QT_TRANSLATE_NOOP("AccessLogPage", "Unknown user name")},
        {QStringLiteral("wrong password"), QT_TRANSLATE_NOOP("AccessLogPage", "Wrong password")},
        {QStringLiteral("logon failed"), QT_TRANSLATE_NOOP("AccessLogPage", "Sign-in failed")},
        {QStringLiteral("account restriction"), QT_TRANSLATE_NOOP("AccessLogPage", "Account restricted")},
        {QStringLiteral("outside allowed hours"), QT_TRANSLATE_NOOP("AccessLogPage", "Outside allowed hours")},
        {QStringLiteral("workstation restriction"), QT_TRANSLATE_NOOP("AccessLogPage", "Not allowed from this device")},
        {QStringLiteral("password expired"), QT_TRANSLATE_NOOP("AccessLogPage", "Password expired")},
        {QStringLiteral("account disabled"), QT_TRANSLATE_NOOP("AccessLogPage", "Account disabled")},
        {QStringLiteral("clocks out of sync"), QT_TRANSLATE_NOOP("AccessLogPage", "Clocks out of sync")},
        {QStringLiteral("logon type not allowed"), QT_TRANSLATE_NOOP("AccessLogPage", "Sign-in not allowed")},
        {QStringLiteral("account expired"), QT_TRANSLATE_NOOP("AccessLogPage", "Account expired")},
        {QStringLiteral("account locked out"), QT_TRANSLATE_NOOP("AccessLogPage", "Account locked")},
    };
    const QString reason = m.captured(2);
    if (reason.isEmpty())
        return {};
    return reasons.contains(reason) ? t(reasons.value(reason)) : reason;
}

bool looksLikeIp(const QString &text)
{
    static const QRegularExpression ip(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$|^[0-9a-fA-F:.]*:[0-9a-fA-F:.]*$"));
    return ip.match(text).hasMatch();
}

QString countryOfAddress(const QString &address)
{
    static QHash<QString, QString> known;
    const auto it = known.constFind(address);
    if (it != known.constEnd())
        return it.value();
    const QString code = GeoIp::countryOf(address);
    if (known.size() >= 8192)
        known.clear();
    known.insert(address, code);
    return code;
}

QString csvField(QString text)
{
    if (!text.isEmpty() && QStringLiteral("=+-@\t").contains(text.at(0)))
        text.prepend(QLatin1Char('\''));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + text + QLatin1Char('"');
}

qint64 parseUtcMs(QStringView s)
{
    if (s.size() < 19)
        return 0;
    const auto dig2 = [&](qsizetype i, int &out) {
        const ushort c0 = s.at(i).unicode() - '0';
        const ushort c1 = s.at(i + 1).unicode() - '0';
        if (c0 > 9 || c1 > 9)
            return false;
        out = c0 * 10 + c1;
        return true;
    };
    if (s.at(4) != QLatin1Char('-') || s.at(7) != QLatin1Char('-')
        || (s.at(10) != QLatin1Char('T') && s.at(10) != QLatin1Char(' '))
        || s.at(13) != QLatin1Char(':') || s.at(16) != QLatin1Char(':'))
        return 0;
    int yHi = 0, yLo = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!dig2(0, yHi) || !dig2(2, yLo) || !dig2(5, mo) || !dig2(8, d)
        || !dig2(11, h) || !dig2(14, mi) || !dig2(17, se))
        return 0;
    const int y = yHi * 100 + yLo;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59)
        return 0;
    int ms = 0;
    if (s.size() > 19 && s.at(19) == QLatin1Char('.')) {
        int mul = 100;
        for (qsizetype i = 20; i < s.size() && mul >= 1; ++i) {
            const ushort c = s.at(i).unicode() - '0';
            if (c > 9)
                break;
            ms += int(c) * mul;
            mul /= 10;
        }
    }
    const qint64 yr = y - (mo <= 2 ? 1 : 0);
    const qint64 era = (yr >= 0 ? yr : yr - 399) / 400;
    const qint64 yoe = yr - era * 400;
    const qint64 doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const qint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const qint64 days = era * 146097 + doe - 719468;
    return ((days * 86400 + qint64(h) * 3600 + qint64(mi) * 60 + se) * 1000) + ms;
}

QDateTime parseUtc(const QString &timeUtc)
{
    const qint64 ms = parseUtcMs(timeUtc);
    return ms != 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC) : QDateTime();
}

QString shortName(const QString &user)
{
    return user.mid(user.lastIndexOf(QLatin1Char('\\')) + 1);
}

QString auditSubcategoryName(const QString &guid)
{
    const QString key = guid.trimmed().toUpper();
    if (key.isEmpty())
        return {};
    static const QHash<QString, const char *> map = {
        {QStringLiteral("{0CCE9210-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Security state change")},
        {QStringLiteral("{0CCE9211-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Security system extension")},
        {QStringLiteral("{0CCE9212-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "System integrity")},
        {QStringLiteral("{0CCE9215-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Logon")},
        {QStringLiteral("{0CCE9216-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Logoff")},
        {QStringLiteral("{0CCE9217-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Account lockout")},
        {QStringLiteral("{0CCE921B-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Special logon")},
        {QStringLiteral("{0CCE921C-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "RDP session tracking (Other Logon/Logoff)")},
        {QStringLiteral("{0CCE922B-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Process creation")},
        {QStringLiteral("{0CCE922F-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Audit policy change")},
        {QStringLiteral("{0CCE9230-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Authentication policy change")},
        {QStringLiteral("{0CCE9235-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "User account management")},
        {QStringLiteral("{0CCE9237-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Security group management")},
        {QStringLiteral("{0CCE923F-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Credential validation")},
        {QStringLiteral("{0CCE9240-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Kerberos service ticket")},
        {QStringLiteral("{0CCE9242-69AE-11D9-BED3-505054503030}"), QT_TRANSLATE_NOOP("AccessLogPage", "Kerberos authentication")},
    };
    const auto it = map.constFind(key);
    return it != map.cend() ? QCoreApplication::translate("AccessLogPage", it.value()) : guid.trimmed();
}

QString auditChangesText(const QString &raw)
{
    if (raw.trimmed().isEmpty())
        return {};
    QStringList out;
    const auto t = [](const char *s) { return QCoreApplication::translate("AccessLogPage", s); };
    for (const QString &tok : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString s = tok.trimmed();
        if (s == QLatin1String("%%8448"))
            out << t("Success removed");
        else if (s == QLatin1String("%%8449"))
            out << t("Success added");
        else if (s == QLatin1String("%%8450"))
            out << t("Failure removed");
        else if (s == QLatin1String("%%8451"))
            out << t("Failure added");
        else if (!s.isEmpty())
            out << s;
    }
    return out.join(QStringLiteral(", "));
}

QString changeActorOf(const AccessRecord &r)
{
    const QString &e = r.event;
    if (e == QLatin1String("UserCreated") || e == QLatin1String("UserEnabled")
        || e == QLatin1String("UserDisabled") || e == QLatin1String("UserDeleted")
        || e == QLatin1String("PasswordChangeAttempt") || e == QLatin1String("PasswordResetAttempt"))
        return shortName(r.detail);
    if (e == QLatin1String("GroupMemberAdded") || e == QLatin1String("GroupMemberRemoved"))
        return shortName(r.detail.section(QLatin1Char('|'), 1, 1));
    if (e.startsWith(QLatin1String("Task")) || e == QLatin1String("AuditPolicyChanged")
        || e == QLatin1String("LogCleared"))
        return shortName(r.user);
    if (r.source == QLatin1String("RCONNECT"))
        return shortName(r.operatorName.isEmpty() ? r.client : r.operatorName);
    return {};
}

QString serverEditFieldsText(const QString &raw)
{
    static const QHash<QString, const char *> fields = {
        {QStringLiteral("name"), QT_TRANSLATE_NOOP("AccessLogPage", "name")},
        {QStringLiteral("host"), QT_TRANSLATE_NOOP("AccessLogPage", "address")},
        {QStringLiteral("port"), QT_TRANSLATE_NOOP("AccessLogPage", "port")},
        {QStringLiteral("user"), QT_TRANSLATE_NOOP("AccessLogPage", "user")},
        {QStringLiteral("domain"), QT_TRANSLATE_NOOP("AccessLogPage", "domain")},
        {QStringLiteral("password"), QT_TRANSLATE_NOOP("AccessLogPage", "password")},
        {QStringLiteral("notes"), QT_TRANSLATE_NOOP("AccessLogPage", "notes")},
        {QStringLiteral("display"), QT_TRANSLATE_NOOP("AccessLogPage", "display")},
        {QStringLiteral("redirection"), QT_TRANSLATE_NOOP("AccessLogPage", "redirection")},
        {QStringLiteral("monitoring"), QT_TRANSLATE_NOOP("AccessLogPage", "monitoring")},
    };
    QStringList out;
    for (const QString &part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString key = part.trimmed();
        const auto it = fields.constFind(key);
        out << (it != fields.cend() ? QCoreApplication::translate("AccessLogPage", it.value()) : key);
    }
    return out.join(QStringLiteral(", "));
}

// Clean iOS-inspired log table delegate: soft translucent event pills, crisp mono timestamps/IPs.
class AccessLogDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, index);
        const bool selected = o.state & QStyle::State_Selected;
        const bool dark = Theme::mode() == Theme::Dark;
        const auto tone = static_cast<EventTone>(index.data(kToneRole).toInt());
        const QColor toneColor = colorForTone(tone);
        const int col = index.column();

        if (!selected && tone == EventTone::Alert) {
            QColor bg = Theme::kRed;
            bg.setAlpha(dark ? 22 : 14);
            p->fillRect(o.rect, bg);
        }

        const QWidget *widget = o.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();

        if (col == Event) {
            const QString text = o.text;
            o.text.clear();
            style->drawControl(QStyle::CE_ItemViewItem, &o, p, widget);

            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setRenderHint(QPainter::TextAntialiasing, true);

            const bool alert = tone == EventTone::Alert;
            const QFont badgeFont = Theme::font(11, alert ? QFont::DemiBold : QFont::Medium);
            static const QFontMetrics fmAlert(Theme::font(11, QFont::DemiBold));
            static const QFontMetrics fmNormal(Theme::font(11, QFont::Medium));
            const QFontMetrics &fm = alert ? fmAlert : fmNormal;
            const int maxW = qMax(36, o.rect.width() - 12);
            const int dotSpace = 12;
            const int availTextW = qMax(12, maxW - dotSpace - 16);
            const QString elided = fm.elidedText(text, Qt::ElideRight, availTextW);
            const int pillW = qMin(maxW, fm.horizontalAdvance(elided) + dotSpace + 16);
            const int pillH = 22;
            const QRectF pill(o.rect.left() + 6, o.rect.center().y() - pillH / 2.0, pillW, pillH);

            QColor bg = toneColor;
            bg.setAlpha(selected ? (dark ? 54 : 34) : (tone == EventTone::Alert ? (dark ? 44 : 26) : (dark ? 28 : 18)));

            p->setBrush(bg);
            p->setPen(Qt::NoPen);
            p->drawRect(pill);

            p->setBrush(toneColor);
            p->drawEllipse(QRectF(pill.left() + 8, pill.center().y() - 3.0, 6.0, 6.0));

            p->setFont(badgeFont);
            p->setPen(selected ? Theme::kLabel : toneColor);
            p->drawText(QRectF(pill.left() + 18, pill.top(), pill.width() - 24, pill.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, elided);
            p->restore();
            return;
        }

        if (col == Time || col == Address) {
            o.font = Theme::monoFont(12);
            if (!selected)
                o.palette.setColor(QPalette::Text, col == Time ? Theme::kLabelSecondary : Theme::kLabel);
        } else if (col == Server) {
            o.font = Theme::font(12, QFont::Medium);
        } else if (col == Details && !selected) {
            o.palette.setColor(QPalette::Text, tone == EventTone::Alert || tone == EventTone::Failed
                                   ? toneColor : Theme::kLabelSecondary);
        }

        style->drawControl(QStyle::CE_ItemViewItem, &o, p, widget);

        if (col == Time && (tone == EventTone::Alert || tone == EventTone::Failed)) {
            p->save();
            QColor bar = toneColor;
            bar.setAlpha(selected ? 235 : 190);
            p->fillRect(QRect(o.rect.left(), o.rect.top() + 3, 2, o.rect.height() - 6), bar);
            p->restore();
        }
    }
};

QString lineIdentity(const AccessLine &line)
{
    const AccessRecord &r = line.r;
    return r.serverId + QLatin1Char('|') + r.event + QLatin1Char('|') + r.address + QLatin1Char('|')
         + line.firstUtc + QLatin1Char('|') + QString::number(r.recordId);
}

} // namespace

class AccessTableModel : public QAbstractTableModel {
public:
    using Cells = std::array<QString, ColumnCount>;
    using Builder = std::function<Cells(const AccessLine &)>;

    AccessTableModel(Builder builder, QObject *parent)
        : QAbstractTableModel(parent)
        , m_builder(std::move(builder))
    {
        m_refilter.setSingleShot(true);
        m_refilter.setInterval(0);
        connect(&m_refilter, &QTimer::timeout, this, [this] { refilter(); });
    }

    void setHeaders(const QStringList &headers)
    {
        m_headers = headers;
        emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    }

    void setLines(const QList<AccessLine> *lines)
    {
        m_lines = lines;
        m_cells.clear();
        m_cells.resize(lines->size());
        m_built.assign(size_t(lines->size()), false);
        m_tones.resize(lines->size());
        for (qsizetype i = 0; i < lines->size(); ++i)
            m_tones[i] = quint8(toneOfEvent(lines->at(i).r.event));
        m_refilter.stop();
        refilter();
    }

    void invalidateCells()
    {
        if (!m_lines || m_rows.isEmpty())
            return;
        m_built.assign(m_built.size(), false);
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, ColumnCount - 1), {Qt::DisplayRole});
    }

    void setCategory(int category) { change(m_category, category, 0); }
    void setServer(const QString &serverId) { change(m_server, serverId, 0); }
    void setSince(const QString &sinceUtc) { change(m_since, sinceUtc, 0); }
    void setText(const QString &text) { change(m_text, text.trimmed(), 150); }
    void setHideFailed(bool hide) { change(m_hideFailed, hide, 0); }

    qint64 hiddenFailedAttempts() const { return m_hiddenFailedAttempts; }
    int hiddenFailedGroups() const { return m_hiddenFailedGroups; }

    int findRowByIdentity(const QString &identity) const
    {
        if (!m_lines || identity.isEmpty())
            return -1;
        for (int row = 0; row < m_rows.size(); ++row)
            if (lineIdentity(m_lines->at(m_rows.at(row))) == identity)
                return row;
        return -1;
    }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_rows.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(ColumnCount); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return {};
        const int line = m_rows.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            return cells(line)[size_t(index.column())];
        case kLineRole:
            return line;
        case kToneRole:
            return int(m_tones.at(line));
        default:
            return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
            return m_headers.value(section);
        return {};
    }

    void sort(int column, Qt::SortOrder order) override
    {
        m_sortColumn = column;
        m_order = order;
        emit layoutAboutToBeChanged();
        sortRows();
        emit layoutChanged();
    }

private:
    template <typename T>
    void change(T &field, const T &value, int delayMs)
    {
        if (field == value)
            return;
        field = value;
        if (m_lines)
            m_refilter.start(delayMs);
    }

    const Cells &cells(int line) const
    {
        if (!m_built[size_t(line)]) {
            m_cells[line] = m_builder(m_lines->at(line));
            m_built[size_t(line)] = true;
        }
        return m_cells[line];
    }

    void refilter()
    {
        beginResetModel();
        m_rows.clear();
        m_hiddenFailedAttempts = 0;
        m_hiddenFailedGroups = 0;
        if (m_lines) {
            for (int i = 0; i < m_lines->size(); ++i) {
                const AccessLine &line = m_lines->at(i);
                const AccessRecord &r = line.r;
                if (!inCategory(r.event, m_category))
                    continue;
                if (!m_server.isEmpty() && r.serverId != m_server)
                    continue;
                if (!m_since.isEmpty() && r.timeUtc < m_since)
                    continue;
                if (!m_text.isEmpty()) {
                    bool hit = false;
                    for (const QString &cell : cells(i))
                        if (cell.contains(m_text, Qt::CaseInsensitive)) {
                            hit = true;
                            break;
                        }
                    if (!hit)
                        continue;
                }
                if (m_category == All && m_hideFailed && r.event == QLatin1String("FailedLogon")) {
                    m_hiddenFailedAttempts += line.attempts;
                    ++m_hiddenFailedGroups;
                    continue;
                }
                m_rows.append(i);
            }
            sortRows();
        }
        endResetModel();
    }

    void sortRows()
    {
        if (!m_lines)
            return;
        const bool desc = m_order == Qt::DescendingOrder;
        const int column = m_sortColumn;
        std::stable_sort(m_rows.begin(), m_rows.end(), [&](int a, int b) {
            const int c = column == Time ? m_lines->at(a).r.timeUtc.compare(m_lines->at(b).r.timeUtc)
                                         : cells(a)[size_t(column)].compare(cells(b)[size_t(column)]);
            return desc ? c > 0 : c < 0;
        });
    }

    Builder m_builder;
    const QList<AccessLine> *m_lines = nullptr;
    mutable QVector<Cells> m_cells;
    mutable std::vector<bool> m_built;
    QVector<quint8> m_tones;
    QVector<int> m_rows;
    QStringList m_headers;
    QTimer m_refilter;
    int m_category = All;
    QString m_server;
    QString m_since;
    QString m_text;
    bool m_hideFailed = true;
    qint64 m_hiddenFailedAttempts = 0;
    int m_hiddenFailedGroups = 0;
    int m_sortColumn = Time;
    Qt::SortOrder m_order = Qt::DescendingOrder;
};

AccessLogPage::AccessLogPage(ServerStore *store, QWidget *parent)
    : QWidget(parent), m_store(store)
{
    m_model = new AccessTableModel(
        [this](const AccessLine &line) {
            const AccessRecord &r = line.r;
            const QDateTime utc = parseUtc(r.timeUtc);
            const QString time = utc.isValid() ? utc.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))
                                               : r.timeUtc;

            QString where = r.address;
            const bool isIp = !r.address.isEmpty() && looksLikeIp(r.address);
            if (isIp) {
                const QString code = countryOfAddress(r.address);
                if (!code.isEmpty())
                    where += QStringLiteral(" · ") + GeoIp::countryName(code);
            }

            QString computer = r.client;
            if (computer.isEmpty() && isIp && HostNames::isLocalAddress(r.address))
                computer = HostNames::cachedOrRequest(r.address);
            if (line.clients.size() > 1)
                computer = tr("%1 (+%2)").arg(line.clients.first()).arg(line.clients.size() - 1);

            const QString user = line.users.size() > 1 ? tr("%1 accounts").arg(line.users.size()) : line.users.value(0);
            const QString changeActor = changeActorOf(r);
            const QString device = r.source == QLatin1String("RCONNECT")
                ? (r.operatorName.isEmpty() ? r.client : shortName(r.operatorName))
                : line.clients.size() > 1 ? computer
                                          : Contacts::instance().label(computer, r.user, r.address);
            const QString who = !changeActor.isEmpty() ? changeActor : (!device.isEmpty() ? device : user);

            QStringList details;
            if (line.attempts > 1)
                details << tr("%1 attempts").arg(line.attempts);
            const QString summary = alertSummary(r);
            if (!summary.isEmpty())
                details << summary;
            else if (!r.detail.isEmpty()) {
                const QString d = detailText(r.detail);
                if (!d.isEmpty())
                    details << d;
            }
            if (!user.isEmpty() && summary.isEmpty() && who.compare(user, Qt::CaseInsensitive) != 0)
                details << user;

            return AccessTableModel::Cells{time, r.server, eventText(r.event), who, where,
                                           details.join(QStringLiteral(" · "))};
        },
        this);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    m_subtitle->setWordWrap(true);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);

    m_refresh = new QPushButton(this);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        emit syncRequested(m_server->currentData().toString());
        reload();
    });
    m_export = new QPushButton(this);
    connect(m_export, &QPushButton::clicked, this, &AccessLogPage::exportCsv);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    head->addLayout(titles, 1);
    head->addWidget(m_refresh, 0, Qt::AlignTop);
    head->addWidget(m_export, 0, Qt::AlignTop);

    m_category = new QComboBox(this);
    m_category->setMinimumWidth(150);
    m_category->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(m_category, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_model->setCategory(i);
        m_showFailed->setVisible(i == All);
    });
    m_server = new QComboBox(this);
    m_server->setMinimumWidth(140);
    m_server->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(m_server, &QComboBox::currentIndexChanged, this, [this] {
        m_model->setServer(m_server->currentData().toString());
        updateSyncStatus();
    });
    m_period = new QComboBox(this);
    m_period->setMinimumWidth(120);
    m_period->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(m_period, &QComboBox::currentIndexChanged, this, [this] { applyPeriod(); });
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_model->setText(text);
    });
    m_showFailed = new QPushButton(this);
    m_showFailed->setObjectName(QStringLiteral("pillToggle"));
    m_showFailed->setCheckable(true);
    m_showFailed->setChecked(false);
    m_showFailed->setCursor(Qt::PointingHandCursor);
    connect(m_showFailed, &QPushButton::toggled, this, [this](bool show) {
        m_model->setHideFailed(!show);
    });

    auto *filters = new QHBoxLayout;
    filters->setSpacing(8);
    filters->addWidget(m_category);
    filters->addWidget(m_server);
    filters->addWidget(m_period);
    filters->addWidget(m_filter, 1);
    filters->addWidget(m_showFailed);

    auto *view = m_view = new QTableView(this);
    view->setModel(m_model);
    view->setItemDelegate(new AccessLogDelegate(view));
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSortingEnabled(true);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(32);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setSectionResizeMode(Details, QHeaderView::Stretch);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    view->setColumnWidth(Time, 138);
    view->setColumnWidth(Server, 145);
    view->setColumnWidth(Event, 195);
    view->setColumnWidth(Who, 165);
    view->setColumnWidth(Address, 155);
    view->sortByColumn(Time, Qt::DescendingOrder);

    m_syncStatus = new QLabel(this);
    m_syncStatus->setObjectName(QStringLiteral("fieldHint"));
    m_syncStatus->setTextFormat(Qt::PlainText);
    m_syncStatus->setWordWrap(true);
    m_syncStatus->hide();

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("fieldHint"));
    connect(m_model, &QAbstractItemModel::modelReset, this, &AccessLogPage::updateCount);

    m_detail = new QFrame(this);
    m_detail->setObjectName(QStringLiteral("detailCard"));
    m_detail->hide();
    auto *detailLay = new QVBoxLayout(m_detail);
    detailLay->setContentsMargins(14, 10, 14, 10);
    m_detailText = new QLabel(m_detail);
    m_detailText->setTextFormat(Qt::RichText);
    m_detailText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLay->addWidget(m_detailText);
    connect(view->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { showDetail(); });

    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex index = m_view->indexAt(pos);
        const int row = index.isValid() ? index.sibling(index.row(), Time).data(kLineRole).toInt() : -1;
        if (row < 0 || row >= m_lines.size())
            return;
        const AccessRecord &r = m_lines.at(row).r;
        QMenu menu(this);
        if (!r.address.isEmpty()) {
            menu.addAction(tr("Copy IP address"), this, [addr = r.address] {
                QApplication::clipboard()->setText(addr);
            });
        }
        menu.addAction(tr("Copy row"), this, [this, viewRow = index.row()] {
            QStringList cols;
            for (int c = 0; c < ColumnCount; ++c) {
                const QString v = m_model->index(viewRow, c).data().toString();
                if (!v.isEmpty())
                    cols << v;
            }
            QApplication::clipboard()->setText(cols.join(QStringLiteral(" | ")));
        });
        if (r.source != QLatin1String("RCONNECT")) {
            const bool known = Contacts::instance().match(r.client, r.user, r.address) != nullptr;
            if (known || !r.client.isEmpty() || !r.address.isEmpty()) {
                menu.addSeparator();
                menu.addAction(known ? tr("Edit contact…") : tr("Add to contacts…"), this,
                               [this, r] { ContactsDialog::editFor(r.client, r.user, this, r.address); });
            }
        }
        menu.exec(m_view->viewport()->mapToGlobal(pos));
    });

    connect(&Contacts::instance(), &Contacts::changed, this, [this] {
        if (m_loaded && !m_busy) {
            m_model->invalidateCells();
            showDetail();
        }
    });
    connect(Theme::notifier(), &ThemeNotifier::changed, this, [this] {
        m_view->viewport()->update();
        showDetail();
    });

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 0, 6, 0);
    lay->setSpacing(10);
    lay->addLayout(head);
    lay->addLayout(filters);
    lay->addWidget(m_syncStatus);
    lay->addWidget(view, 1);
    lay->addWidget(m_detail);
    lay->addWidget(m_count);

    auto *namesTimer = new QTimer(this);
    namesTimer->setSingleShot(true);
    namesTimer->setInterval(300);
    connect(namesTimer, &QTimer::timeout, this, [this] {
        m_model->invalidateCells();
        showDetail();
    });
    connect(HostNames::notifier(), &HostNames::Notifier::resolved, namesTimer, qOverload<>(&QTimer::start));

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &AccessLogPage::reload);

    m_syncReloadTimer = new QTimer(this);
    m_syncReloadTimer->setSingleShot(true);
    m_syncReloadTimer->setInterval(1000);
    connect(m_syncReloadTimer, &QTimer::timeout, this, &AccessLogPage::reload);

    connect(m_store, &QAbstractItemModel::rowsInserted, this, &AccessLogPage::fillServers);
    connect(m_store, &QAbstractItemModel::rowsRemoved, this, &AccessLogPage::fillServers);
    connect(m_store, &QAbstractItemModel::modelReset, this, &AccessLogPage::fillServers);
    retranslate();
}

QString AccessLogPage::sourceText(const QString &source)
{
    if (source == QLatin1String("LSM") || source == QLatin1String("RCM"))
        return tr("Remote Desktop");
    if (source == QLatin1String("SEC") || source == QLatin1String("AUD"))
        return tr("Security");
    if (source == QLatin1String("SYS"))
        return tr("System");
    return source == QLatin1String("RCONNECT") ? QStringLiteral("RConnect") : source;
}

QString AccessLogPage::eventText(const QString &event)
{
    const auto is = [&event](const char *name) { return event == QLatin1String(name); };
    if (is("Logon") || is("SecurityLogon"))             return tr("Signed in");
    if (is("Logoff"))                                   return tr("Signed out");
    if (is("Disconnect") || is("SecurityDisconnect"))   return tr("Disconnected");
    if (is("Reconnect") || is("SecurityReconnect"))     return tr("Reconnected");
    if (is("Authenticated"))                            return tr("Connection accepted");
    if (is("FailedLogon"))                              return tr("Failed sign-in");
    if (is("Connect"))            return tr("Connected");
    if (is("Monitor"))            return tr("Screen viewed");
    if (is("AuditEnable"))        return tr("Tracking enabled");
    if (is("DisplayTuned"))       return tr("Display updated");
    if (is("Message"))            return tr("Message sent");
    if (is("Closed"))             return tr("Connection closed");
    if (is("ServerAdded"))        return tr("Server added");
    if (is("ServerEdited"))       return tr("Server updated");
    if (is("ServerRemoved"))      return tr("Server removed");
    if (is("ServerConfigured"))   return tr("Server configured");
    if (is("Import"))             return tr("Servers imported");
    if (is("Update"))             return tr("App updated");
    if (is("UserCreated"))        return tr("Account created");
    if (is("GroupMemberAdded"))   return tr("Admin rights granted");
    if (is("TaskCreated"))        return tr("Scheduled task added");
    if (is("ServiceInstalled"))   return tr("Service installed");
    if (is("UserEnabled"))        return tr("Account enabled");
    if (is("UserDisabled"))       return tr("Account disabled");
    if (is("UserDeleted"))        return tr("Account deleted");
    if (is("PasswordChangeAttempt")) return tr("Password change tried");
    if (is("PasswordResetAttempt"))  return tr("Password reset tried");
    if (is("AccountLocked"))      return tr("Account locked");
    if (is("GroupMemberRemoved")) return tr("Admin rights removed");
    if (is("TaskRemoved"))        return tr("Scheduled task removed");
    if (is("TaskEnabled"))        return tr("Scheduled task enabled");
    if (is("TaskDisabled"))       return tr("Scheduled task disabled");
    if (is("TaskUpdated"))        return tr("Scheduled task updated");
    if (is("AuditPolicyChanged")) return tr("Audit setting changed");
    if (is("LogCleared"))         return tr("Log cleared");
    if (is("AlertBruteForce"))    return tr("Alert: password guessing");
    if (is("AlertBreakIn"))       return tr("Alert: sign-in after failures");
    if (is("AlertNewCountry"))    return tr("Alert: new country");
    if (is("AlertSuspicious"))    return tr("Alert: new background program");
    return event;
}

QString AccessLogPage::alertSummary(const AccessRecord &r)
{
    const QString &e = r.event;
    // Fast short-circuit for the ~99% of rows (sign-ins, disconnects, failed logons) before splitting/allocating.
    if (e == QLatin1String("Logon") || e == QLatin1String("Logoff") || e == QLatin1String("Disconnect")
        || e == QLatin1String("Reconnect") || e == QLatin1String("Authenticated")
        || e == QLatin1String("SecurityLogon") || e == QLatin1String("FailedLogon")
        || e == QLatin1String("SecurityReconnect") || e == QLatin1String("SecurityDisconnect")
        || e == QLatin1String("Connect") || e == QLatin1String("Closed") || e == QLatin1String("Monitor")
        || e == QLatin1String("Message") || e == QLatin1String("DisplayTuned") || e == QLatin1String("AuditEnable")
        || e == QLatin1String("Import") || e == QLatin1String("Update"))
        return {};
    const QStringList parts = r.detail.split(QLatin1Char('|'));
    const QString first = parts.value(0), second = parts.value(1);
    const QString target = shortName(r.user);
    const QString actor = shortName(r.detail);
    const QString op = shortName(r.operatorName.isEmpty() ? r.client : r.operatorName);
    if (e == QLatin1String("AlertBruteForce"))
        return tr("%1 failed attempts from %2 (%3 accounts)").arg(first, r.address, second);
    if (e == QLatin1String("AlertBreakIn"))
        return tr("%1 signed in from %2 after %3 failed tries").arg(r.user, r.address, first);
    if (e == QLatin1String("AlertNewCountry"))
        return tr("%1 signed in from %2 (%3) for the first time").arg(r.user, r.address, GeoIp::countryName(first));
    if (e == QLatin1String("AlertSuspicious"))
        return (first == QLatin1String("TaskCreated") ? tr("New task: %1") : tr("New service: %1"))
            .arg(r.detail.mid(first.size() + 1));
    if (e == QLatin1String("UserCreated"))
        return tr("Account %1 created by %2").arg(target, actor);
    if (e == QLatin1String("UserEnabled"))
        return tr("Account %1 enabled by %2").arg(target, actor);
    if (e == QLatin1String("UserDisabled"))
        return tr("Account %1 disabled by %2").arg(target, actor);
    if (e == QLatin1String("UserDeleted"))
        return tr("Account %1 deleted by %2").arg(target, actor);
    if (e == QLatin1String("PasswordChangeAttempt"))
        return tr("Password change for %1 by %2").arg(target, actor);
    if (e == QLatin1String("PasswordResetAttempt"))
        return tr("Password reset for %1 by %2").arg(target, actor);
    if (e == QLatin1String("AccountLocked"))
        return r.client.isEmpty() ? tr("Account %1 locked").arg(target)
                                  : tr("Account %1 locked (from %2)").arg(target, r.client);
    if (e == QLatin1String("GroupMemberAdded"))
        return tr("%1 added to %2 by %3").arg(target, first, shortName(second));
    if (e == QLatin1String("GroupMemberRemoved"))
        return tr("%1 removed from %2 by %3").arg(target, first, shortName(second));
    if (e == QLatin1String("LogCleared"))
        return tr("%1 log cleared by %2").arg(r.detail, target);
    if (e == QLatin1String("TaskCreated"))
        return tr("Task %1 added by %2").arg(r.detail, target);
    if (e == QLatin1String("TaskRemoved"))
        return tr("Task %1 removed by %2").arg(r.detail, target);
    if (e == QLatin1String("TaskEnabled"))
        return tr("Task %1 enabled by %2").arg(r.detail, target);
    if (e == QLatin1String("TaskDisabled"))
        return tr("Task %1 disabled by %2").arg(r.detail, target);
    if (e == QLatin1String("TaskUpdated"))
        return tr("Task %1 updated by %2").arg(r.detail, target);
    if (e == QLatin1String("AuditPolicyChanged")) {
        const QString policy = auditSubcategoryName(first);
        const QString what = auditChangesText(second);
        if (!policy.isEmpty() && !what.isEmpty())
            return tr("%1 (%2) — by %3").arg(policy, what, target);
        if (!policy.isEmpty())
            return tr("%1 — changed by %2").arg(policy, target);
        return tr("Changed by %1").arg(target);
    }
    if (e == QLatin1String("ServiceInstalled"))
        return r.user.isEmpty() ? tr("Service: %1").arg(r.detail)
                                : tr("Service: %1 (runs as %2)").arg(r.detail, r.user);
    if (e == QLatin1String("ServerEdited")) {
        const QString fields = serverEditFieldsText(r.detail);
        if (!fields.isEmpty() && !op.isEmpty())
            return tr("Changed: %1 — by %2").arg(fields, op);
        return fields.isEmpty() ? tr("Edited by %1").arg(op) : tr("Changed: %1").arg(fields);
    }
    if (e == QLatin1String("ServerConfigured"))
        return op.isEmpty() ? tr("Session logging and idle limit applied")
                            : tr("Session logging and idle limit applied by %1").arg(op);
    if (e == QLatin1String("ServerAdded"))
        return op.isEmpty() ? r.detail : tr("%1 — added by %2").arg(r.detail, op);
    if (e == QLatin1String("ServerRemoved"))
        return op.isEmpty() ? r.detail : tr("%1 — removed by %2").arg(r.detail, op);
    return {};
}

void AccessLogPage::retranslate()
{
    m_title->setText(tr("Activity history"));
    m_subtitle->setText(tr("Who signed in, disconnected, or failed to sign in."));
    m_refresh->setText(tr("Refresh"));
    m_export->setText(tr("Export…"));
    m_filter->setPlaceholderText(tr("Search name, server or IP"));

    const int current = m_category->currentIndex();
    m_category->blockSignals(true);
    m_category->clear();
    m_category->addItems({tr("All events"), tr("Sign-ins"), tr("Sign-outs"),
                          tr("Failed sign-ins"), tr("Security alerts"), tr("Server changes")});
    m_category->setCurrentIndex(qMax(0, current));
    m_category->blockSignals(false);

    const int period = m_period->currentIndex();
    m_period->blockSignals(true);
    m_period->clear();
    m_period->addItems({tr("Last 24 hours"), tr("Last 7 days"), tr("Last 30 days"), tr("All time")});
    m_period->setCurrentIndex(period < 0 ? 1 : period);
    m_period->blockSignals(false);
    applyPeriod();

    fillServers();
    updateSyncStatus();
    m_model->setHeaders({tr("Date"), tr("Server"), tr("Event"), tr("Person / Device"), tr("IP address"), tr("Details")});
    updateCount();
    if (isVisible())
        reload();
}

void AccessLogPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    reload();
    m_timer->start();
    applyPeriod();
}

void AccessLogPage::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

void AccessLogPage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}

namespace {

constexpr qint64 kTailBytes = 8 * 1024 * 1024;
constexpr int kMaxRaw = 60000;
constexpr int kRunSeconds = 3600;

QList<AccessLine> buildLines(QList<AccessRecord> records)
{
    if (records.size() > kMaxRaw) {
        // Never let failed sign-in spam evict legitimate sign-in/sign-out/alert records.
        QList<AccessRecord> nonFailed;
        QList<AccessRecord> failed;
        nonFailed.reserve(records.size());
        for (AccessRecord &r : records) {
            if (r.event == QLatin1String("FailedLogon"))
                failed << std::move(r);
            else
                nonFailed << std::move(r);
        }
        const qsizetype keepFailed = qMax<qsizetype>(0, kMaxRaw - nonFailed.size());
        if (failed.size() > keepFailed)
            failed = failed.mid(failed.size() - keepFailed);
        records.clear();
        records.reserve(nonFailed.size() + failed.size());
        for (AccessRecord &r : nonFailed)
            records << std::move(r);
        for (AccessRecord &r : failed)
            records << std::move(r);
        std::stable_sort(records.begin(), records.end(),
                         [](const AccessRecord &a, const AccessRecord &b) { return a.timeUtc < b.timeUtc; });
    }
    QHash<QString, QString> clientByAddress;
    for (const AccessRecord &r : std::as_const(records))
        if (!r.client.isEmpty() && !r.address.isEmpty() && r.source != QLatin1String("RCONNECT"))
            clientByAddress.insert(r.address, r.client);
    for (AccessRecord &r : records)
        if (r.client.isEmpty() && !r.address.isEmpty())
            r.client = clientByAddress.value(r.address);

    QList<AccessLine> lines;
    lines.reserve(records.size());
    struct ActiveRun {
        int index = 0;
        qint64 lastMs = 0;
        QSet<QString> users;
        QSet<QString> clients;
        QSet<QString> reasons;
    };
    QHash<QString, ActiveRun> growing;
    constexpr qint64 kRunMs = qint64(kRunSeconds) * 1000;
    for (AccessRecord &r : records) {
        int standsFor = 1;
        if (const qsizetype marker = r.detail.indexOf(QLatin1String("|more=")); marker >= 0) {
            standsFor += qMax(0, QStringView(r.detail).mid(marker + 6).toInt());
            r.detail.truncate(marker);
        }
        const QString userShort = shortName(r.user);
        if (r.event == QLatin1String("FailedLogon")) {
            const QString addrKey = !r.address.isEmpty() ? r.address : (QStringLiteral("C:") + r.client);
            const QString key = r.serverId + QLatin1Char('|') + addrKey;
            const qint64 nowMs = parseUtcMs(r.timeUtc);
            const auto it = growing.find(key);
            if (it != growing.end()) {
                ActiveRun &run = it.value();
                if (run.lastMs != 0 && nowMs != 0 && qAbs(run.lastMs - nowMs) <= kRunMs) {
                    AccessLine &line = lines[run.index];
                    line.attempts += standsFor;
                    if (r.timeUtc > line.r.timeUtc) {
                        line.r.timeUtc = r.timeUtc;
                        run.lastMs = nowMs;
                    }
                    if (r.timeUtc < line.firstUtc)
                        line.firstUtc = r.timeUtc;
                    if (!run.users.contains(userShort)) {
                        run.users.insert(userShort);
                        line.users << userShort;
                    }
                    if (!r.client.isEmpty() && !run.clients.contains(r.client)) {
                        run.clients.insert(r.client);
                        line.clients << r.client;
                    }
                    if (!r.detail.isEmpty() && !run.reasons.contains(r.detail)) {
                        run.reasons.insert(r.detail);
                        line.reasons << r.detail;
                    }
                    continue;
                }
            }
            ActiveRun run;
            run.index = int(lines.size());
            run.lastMs = nowMs;
            run.users.insert(userShort);
            if (!r.client.isEmpty())
                run.clients.insert(r.client);
            if (!r.detail.isEmpty())
                run.reasons.insert(r.detail);
            growing.insert(key, std::move(run));
        }
        QString firstUtc = r.timeUtc;
        QStringList clients = r.client.isEmpty() ? QStringList() : QStringList{r.client};
        QStringList reasons = r.detail.isEmpty() ? QStringList() : QStringList{r.detail};
        lines.append({std::move(r), std::move(firstUtc), standsFor, {userShort}, std::move(clients), std::move(reasons)});
    }
    return lines;
}

} // namespace

void AccessLogPage::reload()
{
    const QFileInfo info(AccessLog::path());
    const QPair<qint64, qint64> stamp(info.size(), info.lastModified().toMSecsSinceEpoch());
    if (m_loaded && stamp == m_stamp)
        return;
    if (m_busy) {
        m_again = true;
        return;
    }
    m_stamp = stamp;
    m_loaded = true;
    m_busy = true;

    QPointer<AccessLogPage> self(this);
    QThreadPool::globalInstance()->start([self] {
        QList<AccessLine> lines = buildLines(AccessLog::readRecent(kTailBytes));
        QMetaObject::invokeMethod(qApp, [self, lines = std::move(lines)]() mutable {
            if (self)
                self->apply(std::move(lines));
        }, Qt::QueuedConnection);
    });
}

void AccessLogPage::apply(QList<AccessLine> lines)
{
    m_busy = false;

    QString selectedId;
    const QModelIndex current = m_view->currentIndex();
    const int curLine = current.isValid() ? current.sibling(current.row(), Time).data(kLineRole).toInt() : -1;
    if (curLine >= 0 && curLine < m_lines.size())
        selectedId = lineIdentity(m_lines.at(curLine));
    const int scroll = m_view->verticalScrollBar()->value();

    m_lines = std::move(lines);

    QHeaderView *header = m_view->horizontalHeader();
    const QByteArray headerState = header->saveState();
    m_view->setUpdatesEnabled(false);
    fillServers();
    applyPeriod();
    m_model->setLines(&m_lines);
    header->restoreState(headerState);
    if (const int row = m_model->findRowByIdentity(selectedId); row >= 0) {
        m_view->selectionModel()->setCurrentIndex(
            m_model->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    m_view->verticalScrollBar()->setValue(scroll);
    m_view->setUpdatesEnabled(true);
    showDetail();
    if (m_again) {
        m_again = false;
        reload();
    }
}

void AccessLogPage::showDetail()
{
    const QModelIndex current = m_view->currentIndex();
    const int row = current.isValid() ? current.sibling(current.row(), Time).data(kLineRole).toInt() : -1;
    if (row < 0 || row >= m_lines.size()) {
        m_detail->hide();
        return;
    }
    const AccessLine &line = m_lines.at(row);
    const AccessRecord &r = line.r;
    const auto local = [](const QString &timeUtc) {
        const QDateTime utc = parseUtc(timeUtc);
        return utc.isValid() ? utc.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")) : timeUtc;
    };
    const QString labelColor = Theme::kLabelSecondary.name();
    const QString toneHex = colorForTone(toneOfEvent(r.event)).name();
    QStringList cells;
    const auto add = [&cells, &labelColor](const QString &label, const QString &value, bool mono = false, const QString &color = {}) {
        if (value.isEmpty())
            return;
        QString valStyle = QStringLiteral("padding:2px 26px 2px 0;");
        if (mono)
            valStyle += QStringLiteral("font-family:Consolas,monospace;");
        if (!color.isEmpty())
            valStyle += QStringLiteral("color:%1;font-weight:600;").arg(color);
        cells << QStringLiteral("<td style='color:%1;padding:2px 12px 2px 0'>%2</td><td style='%3'>%4</td>")
                     .arg(labelColor, label.toHtmlEscaped(), valStyle, value.toHtmlEscaped());
    };
    add(tr("Date"), line.attempts > 1 ? tr("%1 – %2").arg(local(line.firstUtc), local(r.timeUtc)) : local(r.timeUtc), true);
    add(tr("Server"), r.host.isEmpty() ? r.server : QStringLiteral("%1 (%2)").arg(r.server, r.host));
    add(tr("Event"), eventText(r.event), false, toneHex);
    const QString actor = changeActorOf(r);
    if (!actor.isEmpty())
        add(tr("Changed by"), actor);
    else if (r.source != QLatin1String("RCONNECT"))
        add(tr("Person"), Contacts::instance().nameFor(r.client, r.user, r.address));
    add(line.clients.size() > 1 ? tr("Devices") : tr("Device"),
        line.clients.size() > 1 ? line.clients.mid(0, 8).join(QStringLiteral(", ")) + (line.clients.size() > 8 ? QStringLiteral(" …") : QString())
                                : r.client);
    const QString targetUser = shortName(r.user);
    if (actor.isEmpty() || targetUser.compare(actor, Qt::CaseInsensitive) != 0) {
        add(line.users.size() > 1 ? tr("Accounts tried") : (!actor.isEmpty() ? tr("Target account") : tr("Account")),
            line.users.size() > 1 ? line.users.mid(0, 12).join(QStringLiteral(", ")) + (line.users.size() > 12 ? QStringLiteral(" …") : QString())
                                  : targetUser);
    }
    QString where = r.address;
    if (looksLikeIp(r.address)) {
        const QString code = countryOfAddress(r.address);
        if (!code.isEmpty())
            where += QStringLiteral(" · ") + GeoIp::countryName(code);
    }
    add(tr("IP address"), where, true);
    if (line.attempts > 1)
        add(tr("Attempts"), QString::number(line.attempts), true);
    if (line.reasons.size() > 1) {
        QStringList why;
        for (const QString &reason : line.reasons.mid(0, 6)) {
            const QString d = detailText(reason);
            if (!d.isEmpty() && !why.contains(d))
                why << d;
        }
        add(tr("Reason"), why.join(QStringLiteral(", ")));
    } else {
        const QString summary = alertSummary(r);
        add(tr("Details"), summary.isEmpty() ? detailText(r.detail) : summary);
    }

    QString html = QStringLiteral("<table cellspacing='0'>");
    for (int k = 0; k < cells.size(); k += 2)
        html += QStringLiteral("<tr>") + cells.at(k) + (k + 1 < cells.size() ? cells.at(k + 1) : QString()) + QStringLiteral("</tr>");
    m_detailText->setText(html + QStringLiteral("</table>"));
    m_detail->show();
}

void AccessLogPage::fillServers()
{
    const QString chosen = m_server->currentData().toString();
    QHash<QString, QString> servers;
    for (const AccessLine &line : std::as_const(m_lines))
        if (!line.r.serverId.isEmpty())
            servers.insert(line.r.serverId, line.r.server.isEmpty() ? line.r.serverId : line.r.server);
    for (const ServerProfile &profile : m_store->all())
        servers.insert(profile.id, profile.name);
    QStringList ids = servers.keys();
    std::sort(ids.begin(), ids.end(), [&servers](const QString &a, const QString &b) {
        const int name = servers.value(a).localeAwareCompare(servers.value(b));
        return name == 0 ? a < b : name < 0;
    });
    m_server->blockSignals(true);
    m_server->clear();
    m_server->addItem(tr("All servers"), QString());
    for (const QString &id : ids)
        m_server->addItem(servers.value(id), id);
    const int index = m_server->findData(chosen);
    m_server->setCurrentIndex(index < 0 ? 0 : index);
    m_server->blockSignals(false);
    m_model->setServer(m_server->currentData().toString());
    updateSyncStatus();
}

void AccessLogPage::setSyncState(const QString &id, const QString &name, bool running,
                                 const QStringList &failedChannels, const QString &error)
{
    m_syncStates.insert(id, {name, running, failedChannels, error});
    updateSyncStatus();
    if (!running && isVisible())
        m_syncReloadTimer->start();
}

void AccessLogPage::updateSyncStatus()
{
    const QString selected = m_server->currentData().toString();
    QStringList details;
    int running = 0, incomplete = 0;
    for (auto it = m_syncStates.cbegin(); it != m_syncStates.cend(); ++it) {
        if (!selected.isEmpty() && selected != it.key())
            continue;
        const SyncState &state = it.value();
        if (!state.error.isEmpty()) {
            ++incomplete;
            details << state.name + QStringLiteral(" · ") + state.error;
        }
        if (state.running)
            ++running;
    }
    QStringList states;
    if (running > 0)
        states << tr("Updating %1 servers…").arg(running);
    if (incomplete > 0)
        states << tr("Could not reach %1 servers").arg(incomplete);
    if (states.isEmpty()) {
        m_syncStatus->hide();
        return;
    }
    m_syncStatus->setText(!selected.isEmpty() && !details.isEmpty() ? details.first() : states.join(QStringLiteral(" · ")));
    m_syncStatus->setToolTip(details.isEmpty() ? QString() : QStringLiteral("<qt>")
                             + details.join(QLatin1Char('\n')).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))
                             + QStringLiteral("</qt>"));
    m_syncStatus->show();
}

void AccessLogPage::applyPeriod()
{
    const int hours = kPeriodHours[qBound(0, m_period->currentIndex(), 3)];
    m_model->setSince(hours == 0 ? QString()
                                 : QDateTime::currentDateTimeUtc().addSecs(-qint64(hours) * 3600)
                                       .toString(QStringLiteral("yyyy-MM-ddTHH:mm:00")));
}

void AccessLogPage::updateCount()
{
    const qint64 hiddenAttempts = m_model->hiddenFailedAttempts();
    const bool showFailed = m_showFailed->isChecked();
    m_showFailed->setVisible(m_category->currentIndex() == All);
    if (showFailed)
        m_showFailed->setText(tr("Hide failed sign-ins"));
    else if (hiddenAttempts > 0)
        m_showFailed->setText(tr("Show failed (%1)").arg(hiddenAttempts));
    else
        m_showFailed->setText(tr("Show failed"));

    if (m_lines.isEmpty()) {
        m_count->setText(tr("No activity recorded yet."));
    } else if (hiddenAttempts > 0) {
        m_count->setText(tr("Showing %1 records · %2 failed attempts hidden")
                             .arg(m_model->rowCount())
                             .arg(hiddenAttempts));
    } else {
        m_count->setText(tr("Showing %1 records").arg(m_model->rowCount()));
    }
}

void AccessLogPage::exportCsv()
{
    const QString name = QStringLiteral("rconnect-activity-%1.csv")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export"), name, tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export"), tr("Could not write the file."));
        return;
    }
    QByteArray buf;
    buf.reserve(65536);
    buf.append("\xEF\xBB\xBF");
    QStringList header;
    header.reserve(ColumnCount);
    for (int c = 0; c < ColumnCount; ++c)
        header << csvField(m_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
    buf.append(header.join(QLatin1Char(',')).toUtf8());
    buf.append("\r\n");
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QStringList fields;
        fields.reserve(ColumnCount);
        for (int c = 0; c < ColumnCount; ++c)
            fields << csvField(m_model->index(row, c).data().toString());
        buf.append(fields.join(QLatin1Char(',')).toUtf8());
        buf.append("\r\n");
        if (buf.size() >= 65536) {
            f.write(buf);
            buf.clear();
        }
    }
    if (!buf.isEmpty())
        f.write(buf);
}
