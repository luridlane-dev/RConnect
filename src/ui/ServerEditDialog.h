#pragma once
#include "model/ServerProfile.h"

#include <QDialog>
#include <QStringList>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;
class QWidget;

class ServerEditDialog : public QDialog {
    Q_OBJECT
public:
    ServerEditDialog(const ServerProfile &initial, bool isNew, const QStringList &usedColors,
                     QWidget *parent = nullptr);

    ServerProfile profile() const;

private:
    void accept() override;
    QString pickColor() const;

    ServerProfile m_initial;
    QStringList m_usedColors;
    QLineEdit *m_name;
    QLineEdit *m_host;
    QLineEdit *m_user;
    QLineEdit *m_password;
    QCheckBox *m_savePassword;
    QButtonGroup *m_colors;
    QPlainTextEdit *m_notes;
    QToolButton *m_advancedToggle;
    QWidget *m_advanced;
    QLineEdit *m_domain;
    QComboBox *m_depth;
    QComboBox *m_resolution;
    QCheckBox *m_clipboard;
    QCheckBox *m_drives;
    QCheckBox *m_audio;
    QCheckBox *m_certs;
    QCheckBox *m_monitor;
};
