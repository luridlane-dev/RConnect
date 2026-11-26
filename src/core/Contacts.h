#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// A person in the address book: a nickname for a computer, IP address, or account.
// The computer name is preferred because it follows a person across IP addresses, while an IP address
// can identify someone when the server only logged their IP. When multiple fields are filled in, all of
// them must match; the most specific match wins.
struct Contact {
    QString id;
    QString name;     // the nickname shown instead of the computer or IP
    QString device;   // computer name the session connects from
    QString address;  // IP address, optional (useful when computer name is unavailable)
    QString user;     // Windows account (without the domain), optional

    bool hasMatch() const { return !device.isEmpty() || !address.isEmpty() || !user.isEmpty(); }
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
    const Contact *match(const QString &device, const QString &user, const QString &address = {}) const;
    QString nameFor(const QString &device, const QString &user, const QString &address = {}) const;
    // "Elif (LENOVO)", or just the device/name when only one is available.
    QString label(const QString &device, const QString &user, const QString &address = {}) const;
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
    struct Normalized {
        QString device;
        QString address;
        QString user;
    };

    Contacts();
    void load();
    void write() const;
    void rebuildIndex();

    QList<Contact> m_contacts;
    QList<Normalized> m_norm;
};
