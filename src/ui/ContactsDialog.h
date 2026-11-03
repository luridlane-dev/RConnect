#pragma once
#include "core/Contacts.h"

#include <QDialog>

class QLabel;
class QTableWidget;

// The address book: nicknames for computers and accounts.
class ContactsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ContactsDialog(QWidget *parent = nullptr);

    // Edits `contact` in a small dialog and saves it to the address book. Returns false if cancelled.
    static bool edit(Contact contact, QWidget *parent);
    // For the person behind a session or an activity line: edits the entry that matches, or starts a new
    // one filled in with what is known.
    static void editFor(const QString &device, const QString &user, QWidget *parent);

private:
    void reload();
    void editSelected();
    void removeSelected();
    void importFile();
    void exportFile();
    QString selectedId() const;

    QTableWidget *m_table;
    QLabel *m_empty;
};
