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
    return {{QStringLiteral("Id"), c.id}, {QStringLiteral("Name"), c.name}, {QStringLiteral("Device"), c.device},
            {QStringLiteral("User"), c.user}};
}

Contact fromJson(const QJsonObject &o)
{
    Contact c;
    c.id = o.value(QLatin1String("Id")).toString();
    c.name = o.value(QLatin1String("Name")).toString().trimmed();
    c.device = o.value(QLatin1String("Device")).toString().trimmed();
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

bool sameMatch(const Contact &a, const Contact &b)
{
    return norm(a.device) == norm(b.device) && shortUser(a.user) == shortUser(b.user);
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

const Contact *Contacts::match(const QString &device, const QString &user) const
{
    const QString d = norm(device), u = shortUser(user);
    const Contact *best = nullptr;
    int bestFields = 0;
    for (const Contact &c : m_contacts) {
        int fields = 0;
        if (!c.device.isEmpty()) {
            if (norm(c.device) != d)
                continue;
            ++fields;
        }
        if (!c.user.isEmpty()) {
            if (shortUser(c.user) != u)
                continue;
            ++fields;
        }
        if (fields > bestFields) {
            best = &c;
            bestFields = fields;
        }
    }
    return best;
}

QString Contacts::nameFor(const QString &device, const QString &user) const
{
    const Contact *c = match(device, user);
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

QString Contacts::label(const QString &device, const QString &user) const
{
    const QString name = nameFor(device, user);
    if (name.isEmpty())
        return device;
    return device.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, device);
}

void Contacts::save(Contact c)
{
    c.name = c.name.trimmed();
    if (c.name.isEmpty() || !c.hasMatch())
        return;
    if (c.id.isEmpty())
        c.id = newId();
    bool replaced = false;
    for (Contact &existing : m_contacts)
        if (existing.id == c.id) {
            existing = c;
            replaced = true;
        }
    if (!replaced)
        m_contacts << c;
    write();
    emit changed();
}

void Contacts::remove(const QString &id)
{
    for (qsizetype i = 0; i < m_contacts.size(); ++i)
        if (m_contacts.at(i).id == id) {
            m_contacts.removeAt(i);
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
    int count = 0;
    for (Contact &c : incoming) {
        // The same computer, address and account is the same entry, whatever its id was on the other device.
        c.id.clear();
        for (const Contact &mine : std::as_const(m_contacts))
            if (sameMatch(mine, c)) {
                if (mine.name == c.name)
                    c.id = QStringLiteral("-"); // nothing new
                else
                    c.id = mine.id;
                break;
            }
        if (c.id == QLatin1String("-"))
            continue;
        save(c);
        ++count;
    }
    return count;
}
