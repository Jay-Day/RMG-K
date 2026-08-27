/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "CreateRoomDialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QSettings>
#include <QFileInfo>
#include <QIcon>

using namespace UserInterface::Dialog;

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                                   const QString& gameName, const QString& gameMd5,
                                   QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Create Room");
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    // The game comes from the lobby's shared picker, not a picker of our own.
    m_romName = gameName;
    m_romMd5  = gameMd5;
    buildUi(defaultUsername, defaultRoomName);
    if (m_gameLabel)
        m_gameLabel->setText(gameName.isEmpty() ? QStringLiteral("—") : gameName);
    loadDefaults();
    validateInput();
}

void CreateRoomDialog::buildUi(const QString& defaultUsername, const QString& defaultRoomName)
{
    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout;

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(48);
    if (!defaultRoomName.isEmpty())
        m_nameEdit->setText(defaultRoomName);
    else if (!defaultUsername.isEmpty())
        m_nameEdit->setText(QString("%1's Room").arg(defaultUsername));
    else
        m_nameEdit->setPlaceholderText("My Room");
    form->addRow("Room name:", m_nameEdit);

    m_gameLabel = new QLabel(this);
    m_gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("Game:", m_gameLabel);

    m_maxPlayersSpin = new QSpinBox(this);
    m_maxPlayersSpin->setRange(2, 4);
    m_maxPlayersSpin->setValue(2);
    m_maxPlayersSpin->setSuffix(" players");
    form->addRow("Max players:", m_maxPlayersSpin);

    root->addLayout(form);

    // Rollback settings are not surfaced here. Each player configures local
    // delay in the room, and prediction is fixed at 9 frames.

    // Optional password (collapsed by default)
    auto* pwRow = new QHBoxLayout;
    m_passwordCheck = new QCheckBox("Password-protect this room", this);
    pwRow->addWidget(m_passwordCheck);
    pwRow->addStretch();
    root->addLayout(pwRow);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Room password");
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setVisible(false);
    root->addWidget(m_passwordEdit);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: gray;");
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    auto* btns = new QDialogButtonBox(this);
    m_createButton = btns->addButton("Create", QDialogButtonBox::AcceptRole);
    m_cancelButton = btns->addButton(QDialogButtonBox::Cancel);
    m_createButton->setDefault(true); // explicitly: Enter = Create when focus is on Create
    m_cancelButton->setAutoDefault(false);
    m_cancelButton->setDefault(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &CreateRoomDialog::onCreateClicked);
    root->addWidget(btns);

    // Validation hooks
    connect(m_nameEdit,      &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
    connect(m_passwordCheck, &QCheckBox::toggled,     this, &CreateRoomDialog::onPasswordToggled);
    connect(m_passwordEdit,  &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
}

QString CreateRoomDialog::displayGameName(const QString& goodName, const QString& filePath)
{
    QString name = goodName.trimmed();
    if (name.isEmpty() || name.endsWith("(unknown rom)") || name.endsWith("(unknown disk)"))
    {
        name = QFileInfo(filePath).fileName();
    }
    // Strip "(unknown rom)" suffix even from otherwise-named entries, matches
    // what the Kaillera dialog does.
    const QString suffix = " (unknown rom)";
    if (name.endsWith(suffix))
        name.chop(suffix.length());
    return name;
}

void CreateRoomDialog::onPasswordToggled(bool enabled)
{
    m_passwordEdit->setEnabled(enabled);
    m_passwordEdit->setVisible(enabled);
    if (!enabled)
        m_passwordEdit->clear();
    adjustSize();
    validateInput();
}

void CreateRoomDialog::validateInput()
{
    const QString name = m_nameEdit->text().trimmed();
    const bool   hasRom = !m_romMd5.isEmpty();
    const bool passwordRequired = m_passwordCheck->isChecked();
    const QString pwd  = m_passwordEdit->text();

    QString reason;
    if (name.isEmpty())
        reason = "Room name is required.";
    else if (!hasRom)
        reason = "Add a ROM to your library before creating a room.";
    else if (passwordRequired && pwd.isEmpty())
        reason = "Password cannot be empty when enabled.";

    m_statusLabel->setText(reason);
    m_createButton->setEnabled(reason.isEmpty());
}

void CreateRoomDialog::onCreateClicked()
{
    // The legacy delay and fixed prediction stay at their loadDefaults() values.
    m_name = m_nameEdit->text().trimmed();
    // m_romName / m_romMd5 were set from the lobby picker in the constructor.
    m_romRegion  = ""; // baked into the ROM; resolved later via md5 lookup
    m_maxPlayers = m_maxPlayersSpin->value();
    m_password   = m_passwordCheck->isChecked() ? m_passwordEdit->text() : QString();

    saveDefaults();
    setFormEnabled(false);
    m_statusLabel->setText("Creating room...");
    emit createRequested();
}

void CreateRoomDialog::showCreateFailure(const QString& reason)
{
    setFormEnabled(true);
    QString human = reason;
    if (reason == "already_in_room") human = "You're already in a room. Leave it first.";
    else if (reason == "invalid_payload") human = "Server rejected the room settings.";
    m_statusLabel->setText(QString("Couldn't create room: %1").arg(human));
}

void CreateRoomDialog::setFormEnabled(bool enabled)
{
    m_nameEdit->setEnabled(enabled);
    m_maxPlayersSpin->setEnabled(enabled);
    m_passwordCheck->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled && m_passwordCheck->isChecked());
    m_createButton->setEnabled(enabled);
}

void CreateRoomDialog::loadDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    if (s.contains("MaxPlayers")) m_maxPlayersSpin->setValue(s.value("MaxPlayers").toInt());
    // Seed the legacy room delay from the last local value. Prediction is a
    // fixed lobby rule and is never loaded from older user settings.
    if (s.contains("Delay")) m_delay = s.value("Delay").toInt();
    m_prediction = 9;
    s.endGroup();
}

void CreateRoomDialog::saveDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    // The game selection is persisted by the lobby's shared picker, not here.
    s.setValue("MaxPlayers", m_maxPlayers);
    // Delay persistence lives in the room view; CreateRoom only consumes it.
    s.endGroup();
}

#endif // NETPLAY
