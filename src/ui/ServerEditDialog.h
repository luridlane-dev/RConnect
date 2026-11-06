#pragma once
#include "model/ServerProfile.h"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;
class QWidget;

class ServerEditDialog : public QDialog {
    Q_OBJECT
public:
    // `groups` are the names already in use, offered in the Group box.
    ServerEditDialog(const ServerProfile &initial, bool isNew, const QStringList &groups, QWidget *parent = nullptr);

    ServerProfile profile() const;

private:
    void accept() override;

    ServerProfile m_initial;
    QLineEdit *m_name;
    QComboBox *m_group;
    QLineEdit *m_host;
    QLineEdit *m_user;
    QLineEdit *m_password;
    QCheckBox *m_savePassword;
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
    QCheckBox *m_checkSessions;
};
