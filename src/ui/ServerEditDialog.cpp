#include "ServerEditDialog.h"
#include "Theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QToolButton>
#include <QVBoxLayout>

#include <climits>

namespace {

const QStringList kPalette = {QStringLiteral("#3a96dd"), QStringLiteral("#3fa874"), QStringLiteral("#d19a2e"),
                              QStringLiteral("#c4504c"), QStringLiteral("#8a6fc4"), QStringLiteral("#3aa6b0"),
                              QStringLiteral("#8c8c8c"), QStringLiteral("#b07a4a")};

const char *const kResolutions[] = {"1920x1080", "2560x1440", "1600x900", "1366x768"};

QFormLayout *makeForm()
{
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setVerticalSpacing(10);
    form->setHorizontalSpacing(14);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return form;
}

QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("fieldHint"));
    l->setWordWrap(true);
    return l;
}

} // namespace

ServerEditDialog::ServerEditDialog(const ServerProfile &initial, bool isNew, const QStringList &usedColors,
                                   QWidget *parent)
    : QDialog(parent)
    , m_initial(initial)
    , m_usedColors(usedColors)
{
    setWindowTitle(isNew ? tr("Add Server") : tr("Edit Server"));

    m_name = new QLineEdit(initial.name);
    m_name->setPlaceholderText(tr("e.g. Office server"));
    // Custom ports are typed as "host:port", like in mstsc.
    m_host = new QLineEdit(initial.port == 3389 ? initial.host
                                                : QStringLiteral("%1:%2").arg(initial.host).arg(initial.port));
    m_host->setPlaceholderText(tr("IP address or host name"));
    m_user = new QLineEdit(initial.user);
    m_password = new QLineEdit(initial.password);
    m_password->setEchoMode(QLineEdit::Password);
    m_savePassword = new QCheckBox(tr("Save password (encrypted for this Windows account)"));
    m_savePassword->setChecked(initial.savePassword);
    connect(m_savePassword, &QCheckBox::toggled, m_password, &QWidget::setEnabled);
    m_password->setEnabled(initial.savePassword);

    m_colors = new QButtonGroup(this);
    auto *colorRow = new QHBoxLayout;
    colorRow->setSpacing(6);
    for (int i = 0; i < kPalette.size(); ++i) {
        auto *b = new QToolButton;
        b->setCheckable(true);
        b->setFixedSize(22, 22);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; border: 2px solid #1c1c1c; border-radius: 2px; padding: 0; }"
            "QToolButton:checked { border-color: #e8e8e8; }").arg(kPalette[i]));
        m_colors->addButton(b, i);
        colorRow->addWidget(b);
        if (!initial.color.isEmpty() && kPalette[i].compare(initial.color, Qt::CaseInsensitive) == 0)
            b->setChecked(true);
    }
    colorRow->addSpacing(6);
    if (!m_colors->checkedButton()) {
        QLabel *autoLabel = hint(tr("Automatic"));
        colorRow->addWidget(autoLabel);
        connect(m_colors, &QButtonGroup::idClicked, autoLabel, &QWidget::hide);
    }
    colorRow->addStretch();

    m_notes = new QPlainTextEdit(initial.notes);
    m_notes->setPlaceholderText(tr("e.g. Check the nightly backup every morning"));
    m_notes->setTabChangesFocus(true);
    m_notes->setFixedHeight(76);

    auto *basic = makeForm();
    basic->addRow(tr("Name"), m_name);
    basic->addRow(tr("Server"), m_host);
    basic->addRow(tr("User name"), m_user);
    basic->addRow(tr("Password"), m_password);
    basic->addRow(QString(), m_savePassword);
    basic->addRow(tr("Colour"), colorRow);
    basic->addRow(tr("Notes"), m_notes);
    basic->addRow(QString(), hint(tr("Notes are stored encrypted, like the password.")));

    m_domain = new QLineEdit(initial.domain);
    m_domain->setPlaceholderText(tr("Optional"));
    m_depth = new QComboBox;
    m_depth->addItem(tr("Best (32-bit)"), 32);
    m_depth->addItem(tr("Balanced (24-bit)"), 24);
    m_depth->addItem(tr("Fast (16-bit)"), 16);
    m_depth->setCurrentIndex(qMax(0, m_depth->findData(initial.colorDepth)));
    m_resolution = new QComboBox;
    m_resolution->addItem(tr("Fit to window"), QString());
    for (const char *r : kResolutions) {
        const QString value = QString::fromLatin1(r);
        m_resolution->addItem(tr("%1 (fixed)").arg(QString(value).replace(QLatin1Char('x'), QChar(0x00d7))), value);
    }
    m_resolution->setCurrentIndex(qMax(0, m_resolution->findData(initial.resolution)));
    m_clipboard = new QCheckBox(tr("Share clipboard"));
    m_clipboard->setChecked(initial.clipboard);
    m_drives = new QCheckBox(tr("Share local drives"));
    m_drives->setChecked(initial.drives);
    m_audio = new QCheckBox(tr("Play remote audio on this PC"));
    m_audio->setChecked(initial.audio);
    m_certs = new QCheckBox(tr("Trust the server certificate automatically"));
    m_certs->setChecked(initial.ignoreCertWarnings);
    m_monitor = new QCheckBox(tr("Check online status (port check once a minute)"));
    m_monitor->setChecked(initial.monitor);

    m_advanced = new QWidget;
    auto *adv = makeForm();
    adv->setContentsMargins(0, 4, 0, 0);
    adv->addRow(tr("Domain"), m_domain);
    adv->addRow(tr("Quality"), m_depth);
    adv->addRow(tr("Resolution"), m_resolution);
    adv->addRow(QString(), m_clipboard);
    adv->addRow(QString(), m_drives);
    adv->addRow(QString(), m_audio);
    adv->addRow(QString(), m_certs);
    adv->addRow(QString(), m_monitor);
    m_advanced->setLayout(adv);
    m_advanced->setVisible(false);

    m_advancedToggle = new QToolButton;
    m_advancedToggle->setObjectName(QStringLiteral("sectionToggle"));
    m_advancedToggle->setText(tr("Advanced settings"));
    m_advancedToggle->setCheckable(true);
    m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advancedToggle->setArrowType(Qt::RightArrow);
    m_advancedToggle->setCursor(Qt::PointingHandCursor);
    connect(m_advancedToggle, &QToolButton::toggled, this, [this](bool on) {
        m_advancedToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        m_advanced->setVisible(on);
    });

    auto *buttons = new QDialogButtonBox;
    auto *save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primaryButton"));
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(12);
    lay->setSizeConstraint(QLayout::SetFixedSize);
    lay->addLayout(basic);
    lay->addWidget(m_advancedToggle, 0, Qt::AlignLeft);
    lay->addWidget(m_advanced);
    lay->addSpacing(4);
    lay->addWidget(buttons);

    for (QLineEdit *e : {m_name, m_host, m_user, m_password, m_domain})
        e->setMinimumWidth(320);

    Theme::darkTitleBar(this);
}

QString ServerEditDialog::pickColor() const
{
    // Random pick among the least used colours.
    int fewest = INT_MAX;
    QStringList candidates;
    for (const QString &c : kPalette) {
        const int uses = int(m_usedColors.count(c));
        if (uses < fewest) {
            fewest = uses;
            candidates.clear();
        }
        if (uses == fewest)
            candidates << c;
    }
    return candidates.at(int(QRandomGenerator::global()->bounded(qsizetype(candidates.size()))));
}

ServerProfile ServerEditDialog::profile() const
{
    ServerProfile p = m_initial;
    p.host = m_host->text().trimmed();
    p.port = 3389;
    if (p.host.count(QLatin1Char(':')) == 1) { // not IPv6
        const int colon = p.host.indexOf(QLatin1Char(':'));
        bool ok = false;
        const int port = p.host.mid(colon + 1).toInt(&ok);
        if (ok && port > 0 && port < 65536) {
            p.port = port;
            p.host = p.host.left(colon).trimmed();
        }
    }
    p.name = m_name->text().trimmed().isEmpty() ? p.host : m_name->text().trimmed();
    p.user = m_user->text().trimmed();
    p.savePassword = m_savePassword->isChecked();
    p.password = p.savePassword ? m_password->text() : QString();
    if (m_colors->checkedId() >= 0)
        p.color = kPalette.at(m_colors->checkedId());
    else if (p.color.isEmpty())
        p.color = pickColor();
    p.notes = m_notes->toPlainText().trimmed();
    p.domain = m_domain->text().trimmed();
    p.colorDepth = m_depth->currentData().toInt();
    p.resolution = m_resolution->currentData().toString();
    p.clipboard = m_clipboard->isChecked();
    p.drives = m_drives->isChecked();
    p.audio = m_audio->isChecked();
    p.ignoreCertWarnings = m_certs->isChecked();
    p.monitor = m_monitor->isChecked();
    return p;
}

void ServerEditDialog::accept()
{
    if (m_host->text().trimmed().isEmpty()) {
        m_host->setFocus();
        m_host->setStyleSheet(QStringLiteral("border-color: #e5534b;"));
        return;
    }
    QDialog::accept();
}
