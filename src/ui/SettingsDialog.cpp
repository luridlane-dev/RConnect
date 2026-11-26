#include "SettingsDialog.h"

#include "ContactsDialog.h"
#include "SessionWindow.h"
#include "Theme.h"
#include "core/Autostart.h"
#include "core/Language.h"
#include "core/Log.h"
#include "core/UpdateChecker.h"
#include "model/ServerStore.h"

#include <QApplication>
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
#include <QListWidget>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr const char *kPageKeys[] = {"pages/usage", "pages/log"};

QVBoxLayout *sectionPage(QStackedWidget *stack, QLabel *&title)
{
    auto *scroll = new QScrollArea(stack);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("settingsContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(26, 23, 26, 20);
    layout->setSpacing(16);
    title = new QLabel(content);
    title->setObjectName(QStringLiteral("settingsTitle"));
    layout->addWidget(title);
    scroll->setWidget(content);
    stack->addWidget(scroll);
    return layout;
}

QFrame *block(QVBoxLayout *page)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("settingsSection"));
    auto *lay = new QVBoxLayout(frame);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(10);
    page->addWidget(frame);
    return frame;
}

void sectionLabel(QVBoxLayout *page, QLabel *&label)
{
    label = new QLabel;
    label->setObjectName(QStringLiteral("settingsHeading"));
    page->addWidget(label);
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
    setMinimumWidth(620);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    outer->addLayout(body, 1);

    auto *rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("settingsRail"));
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(10, 19, 10, 12);
    m_navigation = new QListWidget(rail);
    m_navigation->setObjectName(QStringLiteral("settingsNavigation"));
    m_navigation->setFixedWidth(155);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setMouseTracking(true);
    m_navigation->setSelectionMode(QAbstractItemView::SingleSelection);
    for (int i = 0; i < 4; ++i)
        m_navigation->addItem(QString());
    railLayout->addWidget(m_navigation);
    body->addWidget(rail);

    auto *stack = new QStackedWidget(this);
    body->addWidget(stack, 1);
    connect(m_navigation, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);

    auto *generalPage = sectionPage(stack, m_titles[0]);
    auto *serversPage = sectionPage(stack, m_titles[1]);
    auto *monitoringPage = sectionPage(stack, m_titles[2]);
    auto *updatesPage = sectionPage(stack, m_titles[3]);
    monitoringPage->setContentsMargins(26, 20, 26, 14);
    monitoringPage->setSpacing(12);

    QFrame *general = block(generalPage);
    m_language = new QComboBox(general);
    m_language->setFixedWidth(165);
    for (const QString &code : Language::available())
        m_language->addItem(Language::nativeName(code), code);
    m_language->setCurrentIndex(qMax(0, m_language->findData(Language::current())));
    connect(m_language, &QComboBox::activated, this, [this](int i) { Language::set(m_language->itemData(i).toString()); });
    row(general, m_labels[0], m_language);

    m_theme = new QComboBox(general);
    m_theme->setFixedWidth(165);
    m_theme->addItems({QString(), QString()});
    m_theme->setCurrentIndex(Theme::mode() == Theme::Dark ? 0 : 1);
    connect(m_theme, &QComboBox::activated, this, [](int i) { Theme::setMode(i == 0 ? Theme::Dark : Theme::Light); });
    row(general, m_labels[1], m_theme);

    m_buttons = new QComboBox(general);
    m_buttons->setFixedWidth(165);
    m_buttons->addItems({QString(), QString()});
    m_buttons->setCurrentIndex(SessionWindow::macButtons() ? 1 : 0);
    connect(m_buttons, &QComboBox::activated, this, [](int i) { SessionWindow::setMacButtons(i == 1); });
    row(general, m_labels[2], m_buttons);

    m_autostart = new QCheckBox(general);
    m_autostart->setChecked(Autostart::isEnabled());
    connect(m_autostart, &QCheckBox::toggled, this, [](bool on) { Autostart::setEnabled(on); });
    general->layout()->addWidget(m_autostart);

    sectionLabel(generalPage, m_cacheHeading);
    QFrame *storage = block(generalPage);
    m_cacheHint = new QLabel(storage);
    m_cacheHint->setObjectName(QStringLiteral("fieldHint"));
    m_cacheHint->setWordWrap(true);
    storage->layout()->addWidget(m_cacheHint);
    m_clearCache = new QPushButton(storage);
    m_clearCache->setAutoDefault(false);
    auto *cacheRow = row(storage, m_cacheStatus, m_clearCache);
    m_cacheStatus->setWordWrap(true);
    cacheRow->setAlignment(m_clearCache, Qt::AlignTop);
    connect(m_clearCache, &QPushButton::clicked, this, [this] { scanCache(true); });

    QFrame *data = block(serversPage);
    m_sorting = new QComboBox(data);
    m_sorting->setFixedWidth(210);
    m_sorting->addItems({QString(), QString()});
    m_sorting->setCurrentIndex(QSettings().value(QStringLiteral("ui/favoritesFirst"), true).toBool() ? 0 : 1);
    connect(m_sorting, &QComboBox::activated, this, [this](int i) {
        QSettings().setValue(QStringLiteral("ui/favoritesFirst"), i == 0);
        emit serverLayoutChanged();
    });
    row(data, m_sortingLabel, m_sorting);

    sectionLabel(serversPage, m_groupHeading);
    QFrame *groups = block(serversPage);
    auto *addGroupRow = new QHBoxLayout;
    addGroupRow->setSpacing(8);
    m_newGroup = new QLineEdit(groups);
    m_newGroup->setClearButtonEnabled(true);
    m_addGroup = new QPushButton(groups);
    m_addGroup->setAutoDefault(false);
    auto createGroup = [this] {
        const QString name = m_newGroup->text().trimmed();
        if (name.isEmpty())
            return;
        m_store->addGroup(name);
        m_newGroup->clear();
    };
    connect(m_addGroup, &QPushButton::clicked, this, createGroup);
    connect(m_newGroup, &QLineEdit::returnPressed, this, createGroup);
    addGroupRow->addWidget(m_newGroup, 1);
    addGroupRow->addWidget(m_addGroup);
    static_cast<QVBoxLayout *>(groups->layout())->addLayout(addGroupRow);
    m_groupRows = new QVBoxLayout;
    m_groupRows->setSpacing(6);
    static_cast<QVBoxLayout *>(groups->layout())->addLayout(m_groupRows);
    m_groupHint = new QLabel(groups);
    m_groupHint->setObjectName(QStringLiteral("fieldHint"));
    m_groupHint->setWordWrap(true);
    groups->layout()->addWidget(m_groupHint);
    connect(m_store, &ServerStore::groupsChanged, this, &SettingsDialog::rebuildGroups);
    // dataChanged also comes with every status check: only a change of the group names rebuilds the rows.
    connect(m_store, &QAbstractItemModel::dataChanged, this, [this] {
        if (m_store->groups() != m_shownGroups)
            rebuildGroups();
    });

    // Pages
    sectionLabel(monitoringPage, m_pagesHeading);
    QFrame *pages = block(monitoringPage);
    for (int i = 0; i < int(std::size(kPageKeys)); ++i) {
        m_pages[i] = new QCheckBox(pages);
        m_pages[i]->setChecked(QSettings().value(QLatin1String(kPageKeys[i]), false).toBool());
        connect(m_pages[i], &QCheckBox::toggled, this, [this, i](bool on) {
            QSettings().setValue(QLatin1String(kPageKeys[i]), on);
            emit pagesChanged();
        });
        pages->layout()->addWidget(m_pages[i]);
    }

    // Logging
    sectionLabel(monitoringPage, m_logHeading);
    QFrame *logging = block(monitoringPage);
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
    QFrame *updates = block(updatesPage);
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

    for (auto *page : {generalPage, serversPage, monitoringPage, updatesPage})
        page->addStretch(1);
    m_navigation->setCurrentRow(0);

    auto *footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(20, 9, 20, 10);
    footerLayout->addStretch(1);
    m_close = new QPushButton(footer);
    m_close->setAutoDefault(false);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(m_close);
    outer->addWidget(footer);

    retranslate();
    scanCache(false);
}

void SettingsDialog::retranslate()
{
    setWindowTitle(tr("Settings"));
    const QString categories[] = {tr("General"), tr("Servers and contacts"), tr("Monitoring"), tr("Updates")};
    for (int i = 0; i < 4; ++i) {
        m_navigation->item(i)->setText(categories[i]);
        m_titles[i]->setText(categories[i]);
    }
    m_pagesHeading->setText(tr("Pages"));
    m_logHeading->setText(tr("Logging"));
    m_groupHeading->setText(tr("Groups"));
    m_newGroup->setPlaceholderText(tr("New group name"));
    m_addGroup->setText(tr("Add group"));
    m_groupHint->setText(tr("Create groups here, or right-click a server to assign one."));
    m_sortingLabel->setText(tr("Server sorting"));
    m_sorting->setItemText(0, tr("All favourites first"));
    m_sorting->setItemText(1, tr("Group by category"));
    rebuildGroups();
    m_labels[0]->setText(tr("Language"));
    m_labels[1]->setText(tr("Appearance"));
    m_labels[2]->setText(tr("Window buttons in full screen"));
    m_theme->setItemText(0, tr("Dark"));
    m_theme->setItemText(1, tr("Light"));
    m_buttons->setItemText(0, tr("Windows style"));
    m_buttons->setItemText(1, tr("macOS style"));
    m_autostart->setText(tr("Start with Windows"));
    m_pages[0]->setText(tr("Time on servers"));
    m_pages[1]->setText(tr("Activity history"));
    m_hints[0]->setText(tr("Everyone who signs in to or out of a server is always recorded."));
    m_detailed->setText(tr("Detailed logging"));
    m_hints[1]->setText(tr("Also records connection details in the log file, and changing, removing and "
                           "importing servers and closing connections in Activity history."));
    m_logFile->setText(tr("Open log file"));
    m_logFolder->setText(tr("Open log folder"));
    m_cacheHeading->setText(tr("Storage"));
    m_cacheHint->setText(tr("Shared Windows Remote Desktop image cache and old RConnect installers."));
    m_clearCache->setText(tr("Clear cache"));
    updateCacheStatus();
    m_contacts->setText(tr("Contacts…"));
    m_import->setText(tr("Import servers…"));
    m_export->setText(tr("Export servers…"));
    m_autoUpdate->setText(tr("Check for updates automatically"));
    m_check->setText(tr("Check for updates…"));
    m_about->setText(tr("About RConnect"));
    m_close->setText(tr("Close"));
}

void SettingsDialog::scanCache(bool clear)
{
    if (m_cacheBusy)
        return;
    m_cacheBusy = true;
    m_cacheClearing = clear;
    updateCacheStatus();
    const QPointer<SettingsDialog> dialog(this);
    QThreadPool::globalInstance()->start([dialog, clear] {
        const auto result = LocalCache::scan(clear);
        QMetaObject::invokeMethod(qApp, [dialog, result] {
            if (!dialog)
                return;
            dialog->m_cache = result;
            dialog->m_cacheBusy = false;
            dialog->updateCacheStatus();
        }, Qt::QueuedConnection);
    });
}

void SettingsDialog::updateCacheStatus()
{
    m_clearCache->setEnabled(!m_cacheBusy && m_cache.remainingFiles > 0);
    if (m_cacheBusy) {
        m_cacheStatus->setText(m_cacheClearing ? tr("Clearing cache…") : tr("Checking cache…"));
        return;
    }

    const QLocale locale(Language::current());
    QStringList status;
    if (m_cache.removedFiles > 0)
        status << tr("Cleared %1.").arg(locale.formattedDataSize(m_cache.removedBytes));
    if (m_cache.remainingFiles > 0)
        status << tr("%1 on disk").arg(locale.formattedDataSize(m_cache.remainingBytes));
    else if (m_cache.removedFiles == 0 && !m_cache.locationUnavailable)
        status << tr("Cache is empty.");
    if (m_cache.failedFiles > 0)
        status << tr("Some files could not be removed.");
    if (m_cache.locationUnavailable)
        status << tr("A cache folder could not be opened.");
    m_cacheStatus->setText(status.join(QLatin1Char('\n')));
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
    m_shownGroups = names;
    if (names.isEmpty()) {
        auto *none = new QLabel(tr("No groups yet."));
        none->setObjectName(QStringLiteral("fieldHint"));
        m_groupRows->addWidget(none);
        return;
    }
    for (const QString &name : names) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(name);
        label->setTextFormat(Qt::PlainText);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setToolTip(name.toHtmlEscaped());
        const int serverCount = m_store->groupServerCount(name);
        auto *countLabel = new QLabel(serverCount == 0 ? tr("empty")
                                                       : (serverCount == 1 ? tr("1 server") : tr("%1 servers").arg(serverCount)));
        countLabel->setObjectName(QStringLiteral("fieldHint"));
        auto *rename = new QPushButton(tr("Rename…"));
        rename->setAutoDefault(false);
        auto *remove = new QPushButton(tr("Remove"));
        remove->setAutoDefault(false);
        row->addWidget(label, 1);
        row->addWidget(countLabel);
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

void SettingsDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (m_fitted)
        return;
    m_fitted = true;
    const QRect available = screen()->availableGeometry();
    const int frameHeight = frameGeometry().height() - height();
    setMinimumWidth(qMin(620, available.width() - 26));
    resize(qMin(720, available.width() - 26), qMin(455, available.height() - frameHeight - 24));
    QRect frame = frameGeometry();
    frame.moveCenter(available.center());
    move(frame.topLeft());
    m_navigation->setFocus();
}

void SettingsDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::LanguageChange)
        retranslate();
}
