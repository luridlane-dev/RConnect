#include "Language.h"

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace {

QString g_current = QStringLiteral("en");
QTranslator *g_app = nullptr;
QTranslator *g_qt = nullptr;

QString settingsKey() { return QStringLiteral("language"); }

void install(const QString &code)
{
    if (!g_app) {
        g_app = new QTranslator(QCoreApplication::instance());
        g_qt = new QTranslator(QCoreApplication::instance());
    }
    QCoreApplication::removeTranslator(g_app);
    QCoreApplication::removeTranslator(g_qt);
    g_current = code;
    if (code == QLatin1String("en"))
        return;
    if (g_qt->load(QStringLiteral(":/i18n/qtbase_%1.qm").arg(code)))
        QCoreApplication::installTranslator(g_qt);
    if (g_app->load(QStringLiteral(":/i18n/rconnect_%1.qm").arg(code)))
        QCoreApplication::installTranslator(g_app);
}

} // namespace

namespace Language {

QStringList available()
{
    return {QStringLiteral("en"), QStringLiteral("tr")};
}

QString nativeName(const QString &code)
{
    QString name = QLocale(code).nativeLanguageName();
    if (!name.isEmpty())
        name[0] = name[0].toUpper();
    return name;
}

void init()
{
    QString code = QSettings().value(settingsKey()).toString();
    if (!available().contains(code))
        code = QLocale::system().language() == QLocale::Turkish ? QStringLiteral("tr") : QStringLiteral("en");
    install(code);
}

QString current()
{
    return g_current;
}

void set(const QString &code)
{
    if (!available().contains(code) || code == g_current)
        return;
    QSettings().setValue(settingsKey(), code);
    install(code);
}

} // namespace Language
