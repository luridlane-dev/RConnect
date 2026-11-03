#include "ContactsDialog.h"

#include "Theme.h"

#include <QCompleter>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column { Name, Device, User, ColumnCount };

} // namespace

ContactsDialog::ContactsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Contacts"));
    resize(640, 440);

    auto *intro = new QLabel(tr("Give a computer a nickname, for example LENOVO is “Elif”. The nickname is shown in the "
                                "server list, in who is connected and in the history, whatever IP address the person "
                                "connects from. To add someone quickly, right-click a server with a person on it."),
                             this);
    intro->setObjectName(QStringLiteral("pageSubtitle"));
    intro->setWordWrap(true);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({tr("Nickname"), tr("Computer"), tr("Account")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this] { editSelected(); });

    m_empty = new QLabel(tr("No contacts yet. Right-click a server with a person on it, or press Add."), this);
    m_empty->setObjectName(QStringLiteral("pageSubtitle"));
    m_empty->setAlignment(Qt::AlignCenter);

    auto *add = new QPushButton(tr("Add…"), this);
    auto *edit = new QPushButton(tr("Edit…"), this);
    auto *remove = new QPushButton(tr("Delete"), this);
    auto *importButton = new QPushButton(tr("Import…"), this);
    auto *exportButton = new QPushButton(tr("Export…"), this);
    auto *close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("primaryButton"));
    connect(add, &QPushButton::clicked, this, [this] { ContactsDialog::edit(Contact{}, this); });
    connect(edit, &QPushButton::clicked, this, &ContactsDialog::editSelected);
    connect(remove, &QPushButton::clicked, this, &ContactsDialog::removeSelected);
    connect(importButton, &QPushButton::clicked, this, &ContactsDialog::importFile);
    connect(exportButton, &QPushButton::clicked, this, &ContactsDialog::exportFile);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(edit);
    buttons->addWidget(remove);
    buttons->addSpacing(16);
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);
    buttons->addStretch();
    buttons->addWidget(close);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(12);
    lay->addWidget(intro);
    lay->addWidget(m_table, 1);
    lay->addWidget(m_empty, 1);
    lay->addLayout(buttons);

    connect(&Contacts::instance(), &Contacts::changed, this, &ContactsDialog::reload);
    reload();
    Theme::darkTitleBar(this);
}

void ContactsDialog::reload()
{
    const QString keep = selectedId();
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    const QList<Contact> contacts = Contacts::instance().all();
    for (const Contact &c : contacts) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString texts[ColumnCount] = {c.name, c.device, c.user};
        for (int col = 0; col < ColumnCount; ++col) {
            auto *item = new QTableWidgetItem(texts[col]);
            item->setData(Qt::UserRole, c.id);
            m_table->setItem(row, col, item);
        }
        if (c.id == keep)
            m_table->selectRow(row);
    }
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(Name, Qt::AscendingOrder);
    m_table->setVisible(!contacts.isEmpty());
    m_empty->setVisible(contacts.isEmpty());
}

QString ContactsDialog::selectedId() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString();
}

void ContactsDialog::editSelected()
{
    if (const Contact *c = Contacts::instance().find(selectedId()))
        edit(*c, this);
}

void ContactsDialog::removeSelected()
{
    const Contact *c = Contacts::instance().find(selectedId());
    if (!c)
        return;
    const QString id = c->id;
    QMessageBox box(QMessageBox::Warning, tr("Delete contact"), tr("Remove “%1” from the contacts?").arg(c->name),
                    QMessageBox::NoButton, this);
    QPushButton *yes = box.addButton(tr("Yes"), QMessageBox::YesRole);
    box.addButton(tr("No"), QMessageBox::NoRole);
    box.exec();
    if (box.clickedButton() == yes)
        Contacts::instance().remove(id);
}

void ContactsDialog::importFile()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import contacts"), QString(), tr("Contacts (*.json)"));
    if (path.isEmpty())
        return;
    const int count = Contacts::instance().importFrom(path);
    if (count < 0)
        QMessageBox::warning(this, tr("Import contacts"), tr("This file is not a contacts list."));
    else
        QMessageBox::information(this, tr("Import contacts"), tr("%1 contacts added or updated.").arg(count));
}

void ContactsDialog::exportFile()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export contacts"), QStringLiteral("rconnect-contacts.json"),
                                                      tr("Contacts (*.json)"));
    if (!path.isEmpty() && !Contacts::instance().exportTo(path))
        QMessageBox::warning(this, tr("Export contacts"), tr("Could not write the file."));
}

bool ContactsDialog::edit(Contact contact, QWidget *parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(contact.id.isEmpty() ? tr("Add to contacts") : tr("Edit contact"));

    auto *name = new QLineEdit(contact.name);
    name->setPlaceholderText(tr("e.g. Elif"));
    // The same person on a second computer: pick the nickname they already have.
    auto *suggestions = new QCompleter(Contacts::instance().names(), name);
    suggestions->setCaseSensitivity(Qt::CaseInsensitive);
    name->setCompleter(suggestions);
    auto *device = new QLineEdit(contact.device);
    device->setPlaceholderText(tr("e.g. LENOVO"));
    auto *user = new QLineEdit(contact.user);
    user->setPlaceholderText(tr("Optional"));
    for (QLineEdit *e : {name, device, user})
        e->setMinimumWidth(300);

    auto *hint = new QLabel(tr("The computer name is enough: the nickname follows it from any IP address. Fill in the "
                               "account only if this person signs in with their own account."));
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    hint->setMaximumWidth(400);

    auto *form = new QFormLayout;
    form->addRow(tr("Nickname"), name);
    form->addRow(tr("Computer"), device);
    form->addRow(tr("Account"), user);

    auto *buttons = new QDialogButtonBox;
    auto *save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primaryButton"));
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        if (name->text().trimmed().isEmpty() || (device->text().trimmed().isEmpty() && user->text().trimmed().isEmpty())) {
            QMessageBox::information(&dlg, dlg.windowTitle(), tr("Enter a nickname and a computer name or an account."));
            return;
        }
        dlg.accept();
    });

    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(12);
    lay->setSizeConstraint(QLayout::SetFixedSize);
    lay->addLayout(form);
    lay->addWidget(hint);
    lay->addWidget(buttons);
    Theme::darkTitleBar(&dlg);
    name->setFocus();

    if (dlg.exec() != QDialog::Accepted)
        return false;
    contact.name = name->text().trimmed();
    contact.device = device->text().trimmed();
    contact.user = user->text().trimmed();
    Contacts::instance().save(contact);
    return true;
}

void ContactsDialog::editFor(const QString &device, const QString &user, QWidget *parent)
{
    if (const Contact *existing = Contacts::instance().match(device, user)) {
        edit(*existing, parent);
        return;
    }
    Contact c;
    c.device = device;
    edit(c, parent);
}
