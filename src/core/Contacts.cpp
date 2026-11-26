#include "Contacts.h"

#include "Log.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {

constexpr auto kFormat = "rconnect-contacts";

QString norm(const QString &s) { return s.trimmed().toLower(); }

// "DOMAIN\elif" and "elif" are the same account.
QString shortUser(const QString &user) { return norm(user.mid(user.lastIndexOf(QLatin1Char('\\')) + 1)); }

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QJsonObject toJson(const Contact &c)
{
    QJsonObject o{{QStringLiteral("Id"), c.id}, {QStringLiteral("Name"), c.name}, {QStringLiteral("Device"), c.device},
                  {QStringLiteral("User"), c.user}};
    if (!c.address.isEmpty())
        o.insert(QStringLiteral("Address"), c.address);
    return o;
}

Contact fromJson(const QJsonObject &o)
{
    Contact c;
    c.id = o.value(QLatin1String("Id")).toString();
    c.name = o.value(QLatin1String("Name")).toString().trimmed();
    c.device = o.value(QLatin1String("Device")).toString().trimmed();
    c.address = o.value(QLatin1String("Address")).toString().trimmed();
    c.user = o.value(QLatin1String("User")).toString().trimmed();
    return c;
}

QByteArray serialize(const QList<Contact> &list)
{
    QJsonArray a;
    for (const Contact &c : list)
        a.append(toJson(c));
    return QJsonDocument(QJsonObject{{QStringLiteral("Format"), QLatin1String(kFormat)}, {QStringLiteral("Version"), 1},
                                     {QStringLiteral("Contacts"), a}})
        .toJson(QJsonDocument::Indented);
}

// Reads a file written by serialize(); false when it is something else.
bool parse(const QByteArray &bytes, QList<Contact> &out)
{
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    if (root.value(QLatin1String("Format")).toString() != QLatin1String(kFormat))
        return false;
    const QJsonArray list = root.value(QLatin1String("Contacts")).toArray();
    for (const QJsonValue &v : list) {
        Contact c = fromJson(v.toObject());
        if (!c.name.isEmpty() && c.hasMatch())
            out << c;
    }
    return true;
}

} // namespace

Contacts &Contacts::instance()
{
    static Contacts contacts;
    return contacts;
}

Contacts::Contacts() { load(); }

QString Contacts::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/contacts.json");
}

void Contacts::rebuildIndex()
{
    m_norm.clear();
    m_norm.reserve(m_contacts.size());
    for (const Contact &c : std::as_const(m_contacts))
        m_norm.append({norm(c.device), norm(c.address), shortUser(c.user)});
}

void Contacts::load()
{
    QFile f(filePath());
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly) || !parse(f.readAll(), m_contacts))
        Log::write(QStringLiteral("[contacts] contacts.json could not be read"));
    for (Contact &c : m_contacts)
        if (c.id.isEmpty())
            c.id = newId();
    rebuildIndex();
}

void Contacts::write() const
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    QSaveFile f(filePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(serialize(m_contacts));
        f.commit();
    }
}

const Contact *Contacts::find(const QString &id) const
{
    for (const Contact &c : m_contacts)
        if (c.id == id)
            return &c;
    return nullptr;
}

const Contact *Contacts::match(const QString &device, const QString &user, const QString &address) const
{
    if (m_contacts.isEmpty())
        return nullptr;
    const QString d = norm(device), a = norm(address), u = shortUser(user);
    const Contact *best = nullptr;
    int bestScore = 0;
    for (qsizetype i = 0; i < m_contacts.size(); ++i) {
        const Normalized &n = m_norm.at(i);
        int score = 0;
        if (!n.device.isEmpty()) {
            if (!d.isEmpty() && n.device == d)
                score += 4;
            else if (d.isEmpty() && n.address.isEmpty() && !a.isEmpty() && n.device == a)
                score += 2; // legacy entries where an IP was typed into the Computer field
            else
                continue;
        }
        if (!n.address.isEmpty()) {
            if (a.isEmpty() || n.address != a)
                continue;
            score += 2;
        }
        if (!n.user.isEmpty()) {
            if (u.isEmpty() || n.user != u)
                continue;
            score += 1;
        }
        if (score > bestScore) {
            best = &m_contacts.at(i);
            bestScore = score;
            if (bestScore == 7)
                break; // cannot beat device + address + user
        }
    }
    return best;
}

QString Contacts::nameFor(const QString &device, const QString &user, const QString &address) const
{
    const Contact *c = match(device, user, address);
    return c ? c->name : QString();
}

QStringList Contacts::names() const
{
    QStringList names;
    for (const Contact &c : m_contacts)
        if (!names.contains(c.name, Qt::CaseInsensitive))
            names << c.name;
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString Contacts::label(const QString &device, const QString &user, const QString &address) const
{
    const QString name = nameFor(device, user, address);
    if (name.isEmpty())
        return device;
    return device.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, device);
}

void Contacts::save(Contact c)
{
    c.name = c.name.trimmed();
    c.device = c.device.trimmed();
    c.address = c.address.trimmed();
    c.user = c.user.trimmed();
    if (c.name.isEmpty() || !c.hasMatch())
        return;
    if (c.id.isEmpty())
        c.id = newId();
    bool replaced = false;
    for (qsizetype i = 0; i < m_contacts.size(); ++i)
        if (m_contacts.at(i).id == c.id) {
            m_contacts[i] = c;
            m_norm[i] = {norm(c.device), norm(c.address), shortUser(c.user)};
            replaced = true;
            break;
        }
    if (!replaced) {
        m_contacts << c;
        m_norm.append({norm(c.device), norm(c.address), shortUser(c.user)});
    }
    write();
    emit changed();
}

void Contacts::remove(const QString &id)
{
    for (qsizetype i = 0; i < m_contacts.size(); ++i)
        if (m_contacts.at(i).id == id) {
            m_contacts.removeAt(i);
            m_norm.removeAt(i);
            write();
            emit changed();
            return;
        }
}

bool Contacts::exportTo(const QString &path) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(serialize(m_contacts));
    return f.commit();
}

int Contacts::importFrom(const QString &path)
{
    QFile f(path);
    QList<Contact> incoming;
    if (!f.open(QIODevice::ReadOnly) || !parse(f.readAll(), incoming))
        return -1;
    QHash<QString, qsizetype> byMatch;
    byMatch.reserve(m_contacts.size());
    const auto matchKey = [](const QString &d, const QString &a, const QString &u) {
        return d + QLatin1Char('\0') + a + QLatin1Char('\0') + u;
    };
    for (qsizetype i = 0; i < m_norm.size(); ++i)
        byMatch.insert(matchKey(m_norm.at(i).device, m_norm.at(i).address, m_norm.at(i).user), i);

    int count = 0;
    for (Contact &c : incoming) {
        c.name = c.name.trimmed();
        c.device = c.device.trimmed();
        c.address = c.address.trimmed();
        c.user = c.user.trimmed();
        if (c.name.isEmpty() || !c.hasMatch())
            continue;
        const Normalized n{norm(c.device), norm(c.address), shortUser(c.user)};
        const QString mk = matchKey(n.device, n.address, n.user);
        const auto it = byMatch.constFind(mk);
        if (it != byMatch.cend()) {
            Contact &mine = m_contacts[it.value()];
            if (mine.name == c.name)
                continue;
            mine.name = c.name;
            mine.device = c.device;
            mine.address = c.address;
            mine.user = c.user;
            m_norm[it.value()] = n;
        } else {
            c.id = newId();
            byMatch.insert(mk, m_contacts.size());
            m_contacts.append(c);
            m_norm.append(n);
        }
        ++count;
    }
    if (count > 0) {
        write();
        emit changed();
    }
    return count;
}
