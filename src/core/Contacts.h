#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// A person in the address book: a nickname for a computer, for example the computer "LENOVO" is "Elif".
// The computer name is what identifies a person, so the nickname follows them whatever IP address they
// connect from. The Windows account is optional, for people who sign in with their own account (or when
// the computer name is not known); when both are filled in, both must match. The entry with the most
// fields wins.
struct Contact {
    QString id;
    QString name;     // the nickname shown instead of the computer
    QString device;   // computer name the session connects from
    QString user;     // Windows account (without the domain), optional

    bool hasMatch() const { return !device.isEmpty() || !user.isEmpty(); }
};

// The address book, kept in contacts.json next to servers.json. The file is plain JSON
// ("rconnect-contacts") so the Android app can read and write the same list.
class Contacts : public QObject {
    Q_OBJECT
public:
    static Contacts &instance();

    QList<Contact> all() const { return m_contacts; }
    const Contact *find(const QString &id) const;
    // The best entry for a session or an event; null when nobody has a nickname.
    const Contact *match(const QString &device, const QString &user) const;
    QString nameFor(const QString &device, const QString &user) const;
    // "Elif (LENOVO)", or just the device when there is no nickname.
    QString label(const QString &device, const QString &user) const;
    // Every nickname once, for suggestions.
    QStringList names() const;

    void save(Contact c); // adds, or replaces the entry with the same id
    void remove(const QString &id);

    bool exportTo(const QString &path) const;
    // Returns how many entries were added or changed, or -1 when the file is not an address book.
    int importFrom(const QString &path);

    static QString filePath();

signals:
    void changed();

private:
    Contacts();
    void load();
    void write() const;

    QList<Contact> m_contacts;
};
