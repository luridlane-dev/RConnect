#include "HomePage.h"

#include "ServerPanel.h"
#include "core/Language.h"
#include "model/ServerStore.h"

#include <QActionGroup>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QVBoxLayout>

HomePage::HomePage(ServerStore *store, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("emptyPage"));

    auto *inner = new QWidget(this);
    inner->setMaximumWidth(1100);
    auto *innerLay = new QVBoxLayout(inner);
    innerLay->setContentsMargins(0, 0, 0, 0);
    innerLay->setSpacing(18);

    m_title = new QLabel(inner);
    m_title->setObjectName(QStringLiteral("brandName"));
    m_summary = new QLabel(inner);
    m_summary->setObjectName(QStringLiteral("brandSummary"));
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    titles->addWidget(m_title);
    titles->addWidget(m_summary);

    m_settings = new QToolButton(inner);
    m_settings->setObjectName(QStringLiteral("settingsButton"));
    m_settings->setText(QStringLiteral("")); // Settings glyph (Segoe MDL2 Assets)
    m_settings->setFixedSize(36, 36);
    m_settings->setCursor(Qt::PointingHandCursor);
    connect(m_settings, &QToolButton::clicked, this, &HomePage::showSettingsMenu);

    auto *brand = new QHBoxLayout;
    brand->setContentsMargins(ServerPanel::kInset, 0, ServerPanel::kInset, 0);
    brand->addLayout(titles);
    brand->addStretch(1);
    brand->addWidget(m_settings, 0, Qt::AlignTop);
    innerLay->addLayout(brand);
    m_brandLayout = brand;

    m_panel = new ServerPanel(store, inner);
    innerLay->addWidget(m_panel, 1);

    auto *lay = new QHBoxLayout(this);
    lay->addStretch(1);
    lay->addWidget(inner, 100);
    lay->addStretch(1);
    lay->setContentsMargins(24, 44, 24, 20);

    connect(m_panel, &ServerPanel::rightInsetChanged, this, [this](int inset) {
        m_brandLayout->setContentsMargins(ServerPanel::kInset, 0, inset, 0);
    });
    connect(store, &ServerStore::summaryChanged, this, &HomePage::updateSummary);
    m_settings->setToolTip(tr("Settings"));
    updateSummary();
}

void HomePage::updateSummary()
{
    m_title->setText(tr("Servers"));
    m_summary->setText(ServerPanel::summary(m_store));
}

void HomePage::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange) {
        m_settings->setToolTip(tr("Settings"));
        updateSummary();
    }
}

void HomePage::showSettingsMenu()
{
    QMenu menu(this);
    QMenu *language = menu.addMenu(tr("Language"));
    auto *group = new QActionGroup(language);
    for (const QString &code : Language::available()) {
        QAction *a = language->addAction(Language::nativeName(code));
        a->setCheckable(true);
        a->setChecked(code == Language::current());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [code] { Language::set(code); });
    }
    menu.addSeparator();
    menu.addAction(tr("About RConnect"), this, &HomePage::showAbout);
    menu.exec(m_settings->mapToGlobal(QPoint(m_settings->width(), m_settings->height() + 4))
              - QPoint(menu.sizeHint().width(), 0));
}

void HomePage::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("About RConnect"));
    box.setIconPixmap(QIcon(QStringLiteral(":/icon.png")).pixmap(56, 56));
    box.setText(QStringLiteral("<b>RConnect %1</b>").arg(QApplication::applicationVersion()));
    box.setInformativeText(tr("Saves your Remote Desktop servers and connects to them in one click.")
                           + QStringLiteral("<br><br>MIT License<br>"
                                            "<a href=\"https://db-ip.com\" style=\"color:#5cb0ee\">IP Geolocation by DB-IP</a>"));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.exec();
}
