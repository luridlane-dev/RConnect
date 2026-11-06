#include "SettingsDialog.h"

#include "ContactsDialog.h"
#include "SessionWindow.h"
#include "Theme.h"
#include "core/Autostart.h"
#include "core/Language.h"
#include "core/Log.h"
#include "core/UpdateChecker.h"
#include "model/ServerStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr const char *kPageKeys[3] = {"pages/live", "pages/usage", "pages/log"};

// One section: a small heading over a bordered block of rows.
QFrame *block(QVBoxLayout *page, QLabel *&heading, QWidget *parent)
{
    heading = new QLabel(parent);
    heading->setObjectName(QStringLiteral("settingsHeading"));
    heading->setContentsMargins(2, 8, 0, 0);
    page->addWidget(heading);
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("detailCard"));
    auto *lay = new QVBoxLayout(frame);
    lay->setContentsMargins(14, 10, 14, 10);
    lay->setSpacing(10);
    page->addWidget(frame);
    return frame;
}

// A caption on the left, a control on the right.
QHBoxLayout *row(QFrame *frame, QLabel *&caption, QWidget *control)
{
    auto *h = new QHBoxLayout;
    caption = new QLabel(frame);
    h->addWidget(caption, 1);
    h->addWidget(control);
    static_cast<QVBoxLayout *>(frame->layout())->addLayout(h);
    return h;
}

} // namespace

SettingsDialog::SettingsDialog(ServerStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setMinimumWidth(480);
    auto *page = new QVBoxLayout(this);
    page->setContentsMargins(20, 14, 20, 16);
    page->setSpacing(6);

    // General
    QFrame *general = block(page, m_headings[0], this);
    m_language = new QComboBox(general);
    m_language->setMinimumWidth(150);
    for (const QString &code : Language::available())
        m_language->addItem(Language::nativeName(code), code);
    m_language->setCurrentIndex(qMax(0, m_language->findData(Language::current())));
    connect(m_language, &QComboBox::activated, this, [this](int i) { Language::set(m_language->itemData(i).toString()); });
    row(general, m_labels[0], m_language);

    m_theme = new QComboBox(general);
    m_theme->setMinimumWidth(150);
    m_theme->addItems({QString(), QString()});
    m_theme->setCurrentIndex(Theme::mode() == Theme::Dark ? 0 : 1);
    connect(m_theme, &QComboBox::activated, this, [](int i) { Theme::setMode(i == 0 ? Theme::Dark : Theme::Light); });
    row(general, m_labels[1], m_theme);

    m_buttons = new QComboBox(general);
    m_buttons->setMinimumWidth(150);
    m_buttons->addItems({QString(), QString()});
    m_buttons->setCurrentIndex(SessionWindow::macButtons() ? 1 : 0);
    connect(m_buttons, &QComboBox::activated, this, [](int i) { SessionWindow::setMacButtons(i == 1); });
    row(general, m_labels[2], m_buttons);

    m_autostart = new QCheckBox(general);
    m_autostart->setChecked(Autostart::isEnabled());
    connect(m_autostart, &QCheckBox::toggled, this, [](bool on) { Autostart::setEnabled(on); });
    general->layout()->addWidget(m_autostart);

    // Groups
    QFrame *groups = block(page, m_headings[5], this);
    m_groupRows = new QVBoxLayout;
    m_groupRows->setSpacing(6);
    static_cast<QVBoxLayout *>(groups->layout())->addLayout(m_groupRows);
    m_groupHint = new QLabel(groups);
    m_groupHint->setObjectName(QStringLiteral("fieldHint"));
    m_groupHint->setWordWrap(true);
    groups->layout()->addWidget(m_groupHint);
    connect(m_store, &QAbstractItemModel::dataChanged, this, &SettingsDialog::rebuildGroups);

    // Pages
    QFrame *pages = block(page, m_headings[1], this);
    for (int i = 0; i < 3; ++i) {
        m_pages[i] = new QCheckBox(pages);
        m_pages[i]->setChecked(QSettings().value(QLatin1String(kPageKeys[i]), false).toBool());
        connect(m_pages[i], &QCheckBox::toggled, this, [this, i](bool on) {
            QSettings().setValue(QLatin1String(kPageKeys[i]), on);
            emit pagesChanged();
        });
        pages->layout()->addWidget(m_pages[i]);
    }

    // Logging
    QFrame *logging = block(page, m_headings[2], this);
    m_detailed = new QCheckBox(logging);
    m_detailed->setChecked(Log::detailed());
    connect(m_detailed, &QCheckBox::toggled, this, [](bool on) { Log::setDetailed(on); });
    logging->layout()->addWidget(m_detailed);
    m_hints[1] = new QLabel(logging);
    m_hints[1]->setObjectName(QStringLiteral("fieldHint"));
    m_hints[1]->setWordWrap(true);
    logging->layout()->addWidget(m_hints[1]);
    // Who signs in to and out of a server is always recorded; there is nothing to switch.
    m_hints[0] = new QLabel(logging);
    m_hints[0]->setObjectName(QStringLiteral("fieldHint"));
    m_hints[0]->setWordWrap(true);
    logging->layout()->addWidget(m_hints[0]);
    auto *logButtons = new QHBoxLayout;
    m_logFile = new QPushButton(logging);
    m_logFolder = new QPushButton(logging);
    connect(m_logFile, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Log::path())); });
    connect(m_logFolder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(Log::path()).absolutePath()));
    });
    logButtons->addWidget(m_logFile);
    logButtons->addWidget(m_logFolder);
    logButtons->addStretch(1);
    static_cast<QVBoxLayout *>(logging->layout())->addLayout(logButtons);

    // Servers and contacts
    QFrame *data = block(page, m_headings[3], this);
    auto *dataButtons = new QHBoxLayout;
    m_contacts = new QPushButton(data);
    m_import = new QPushButton(data);
    m_export = new QPushButton(data);
    connect(m_contacts, &QPushButton::clicked, this, [this] { ContactsDialog(this).exec(); });
    connect(m_import, &QPushButton::clicked, this, [this] {
        accept();
        emit importRequested();
    });
    connect(m_export, &QPushButton::clicked, this, [this] {
        accept();
        emit exportRequested();
    });
    dataButtons->addWidget(m_contacts);
    dataButtons->addWidget(m_import);
    dataButtons->addWidget(m_export);
    dataButtons->addStretch(1);
    static_cast<QVBoxLayout *>(data->layout())->addLayout(dataButtons);

    // Updates
    QFrame *updates = block(page, m_headings[4], this);
    m_autoUpdate = new QCheckBox(updates);
    m_autoUpdate->setChecked(UpdateChecker::autoCheckEnabled());
    connect(m_autoUpdate, &QCheckBox::toggled, this, [](bool on) { UpdateChecker::setAutoCheckEnabled(on); });
    updates->layout()->addWidget(m_autoUpdate);
    auto *updateButtons = new QHBoxLayout;
    m_check = new QPushButton(updates);
    m_about = new QPushButton(updates);
    connect(m_check, &QPushButton::clicked, this, [this] {
        accept();
        emit updateCheckRequested();
    });
    connect(m_about, &QPushButton::clicked, this, &SettingsDialog::aboutRequested);
    updateButtons->addWidget(m_check);
    updateButtons->addWidget(m_about);
    updateButtons->addStretch(1);
    static_cast<QVBoxLayout *>(updates->layout())->addLayout(updateButtons);

    page->addSpacing(6);
    auto *footer = new QHBoxLayout;
    footer->addStretch(1);
    m_close = new QPushButton(this);
    m_close->setObjectName(QStringLiteral("primaryButton"));
    m_close->setDefault(true);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(m_close);
    page->addLayout(footer);

    retranslate();
}

void SettingsDialog::retranslate()
{
    setWindowTitle(tr("Settings"));
    m_headings[0]->setText(tr("GENERAL"));
    m_headings[1]->setText(tr("PAGES"));
    m_headings[2]->setText(tr("LOGGING"));
    m_headings[3]->setText(tr("SERVERS AND CONTACTS"));
    m_headings[4]->setText(tr("UPDATES"));
    m_headings[5]->setText(tr("GROUPS"));
    m_groupHint->setText(tr("A group appears when a server is put in it: right-click a server, then Group."));
    rebuildGroups();
    m_labels[0]->setText(tr("Language"));
    m_labels[1]->setText(tr("Appearance"));
    m_labels[2]->setText(tr("Window buttons in full screen"));
    m_theme->setItemText(0, tr("Dark"));
    m_theme->setItemText(1, tr("Light"));
    m_buttons->setItemText(0, tr("Windows style"));
    m_buttons->setItemText(1, tr("macOS style"));
    m_autostart->setText(tr("Start with Windows"));
    m_pages[0]->setText(tr("Who's connected"));
    m_pages[1]->setText(tr("Time on servers"));
    m_pages[2]->setText(tr("Activity history"));
    m_hints[0]->setText(tr("Everyone who signs in to or out of a server is always recorded."));
    m_detailed->setText(tr("Detailed logging"));
    m_hints[1]->setText(tr("Also records connection details in the log file, and changing, removing and "
                           "importing servers and closing connections in Activity history."));
    m_logFile->setText(tr("Open log file"));
    m_logFolder->setText(tr("Open log folder"));
    m_contacts->setText(tr("Contacts…"));
    m_import->setText(tr("Import servers…"));
    m_export->setText(tr("Export servers…"));
    m_autoUpdate->setText(tr("Check for updates automatically"));
    m_check->setText(tr("Check for updates…"));
    m_about->setText(tr("About RConnect"));
    m_close->setText(tr("Close"));
}

// One row per group: its name, and buttons to rename it or to remove it (its servers stay, without a group).
void SettingsDialog::rebuildGroups()
{
    while (QLayoutItem *item = m_groupRows->takeAt(0)) {
        if (QLayout *row = item->layout()) {
            while (QLayoutItem *inner = row->takeAt(0)) {
                delete inner->widget();
                delete inner;
            }
        }
        delete item->widget();
        delete item;
    }
    const QStringList names = m_store->groups();
    if (names.isEmpty()) {
        auto *none = new QLabel(tr("No groups yet."));
        none->setObjectName(QStringLiteral("fieldHint"));
        m_groupRows->addWidget(none);
        return;
    }
    for (const QString &name : names) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(name);
        auto *rename = new QPushButton(tr("Rename…"));
        auto *remove = new QPushButton(tr("Remove"));
        row->addWidget(label, 1);
        row->addWidget(rename);
        row->addWidget(remove);
        connect(rename, &QPushButton::clicked, this, [this, name] {
            bool ok = false;
            const QString to = QInputDialog::getText(this, tr("Rename group"), tr("Group name"), QLineEdit::Normal, name, &ok);
            if (ok && !to.trimmed().isEmpty())
                m_store->renameGroup(name, to);
        });
        connect(remove, &QPushButton::clicked, this, [this, name] { m_store->renameGroup(name, {}); });
        m_groupRows->addLayout(row);
    }
}

void SettingsDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}
