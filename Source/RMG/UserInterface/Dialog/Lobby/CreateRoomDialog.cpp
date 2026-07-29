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
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QSettings>
#include <QFileInfo>

using namespace UserInterface::Dialog;

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername,
                                   const QString& gameName, const QString& gameMd5,
                                   QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Create Netplay Room");
    setModal(true);
    setFixedWidth(380);
    setMinimumHeight(220);
    // The game comes from the lobby's shared picker, not a picker of our own.
    m_romName = gameName;
    m_romMd5  = gameMd5;
    buildUi(defaultUsername);
    if (m_gameLabel)
        m_gameLabel->setText(gameName.isEmpty() ? QStringLiteral("—") : gameName);
    loadDefaults();
    validateInput();
}

void CreateRoomDialog::buildUi(const QString& defaultUsername)
{
    setStyleSheet(R"(
        QLineEdit, QComboBox {
            padding: 4px 6px;
        }
        QPushButton#CreateBtn {
            background-color: #0078D7;
            color: #ffffff;
            border: none;
            border-radius: 3px;
            padding: 6px 18px;
            font-weight: bold;
            min-height: 24px;
        }
        QPushButton#CreateBtn:hover {
            background-color: #1084e3;
        }
        QPushButton#CreateBtn:disabled {
            background-color: #505050;
            color: #888888;
        }
        QPushButton#CancelBtn {
            padding: 6px 16px;
            min-height: 24px;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* formLayout = new QFormLayout;
    formLayout->setLabelAlignment(Qt::AlignLeft);
    formLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    formLayout->setSpacing(10);

    // Room Name
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(48);
    if (!defaultUsername.isEmpty())
        m_nameEdit->setText(QString("%1's Room").arg(defaultUsername));
    else
        m_nameEdit->setPlaceholderText("My Room");
    formLayout->addRow("Room Name:", m_nameEdit);

    // Password
    m_passwordCheck = new QCheckBox("Password-protect this room", this);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Room password");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setVisible(false);

    auto* pwLay = new QVBoxLayout;
    pwLay->setSpacing(4);
    pwLay->setContentsMargins(0, 0, 0, 0);
    pwLay->addWidget(m_passwordCheck);
    pwLay->addWidget(m_passwordEdit);
    formLayout->addRow("Password:", pwLay);

    // Game
    m_gameLabel = new QLabel(this);
    m_gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    formLayout->addRow("Game:", m_gameLabel);

    // Max Players
    m_maxPlayersCombo = new QComboBox(this);
    m_maxPlayersCombo->addItem("2 players", 2);
    m_maxPlayersCombo->addItem("3 players", 3);
    m_maxPlayersCombo->addItem("4 players", 4);
    m_maxPlayersCombo->addItem("5 players", 5);
    m_maxPlayersCombo->setCurrentIndex(2); // default 4 players
    formLayout->addRow("Max Players:", m_maxPlayersCombo);

    root->addLayout(formLayout);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #e74c3c; font-weight: bold;");
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    root->addStretch(1);

    // Action Buttons
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setObjectName("CancelBtn");
    btnRow->addWidget(m_cancelButton);

    m_createButton = new QPushButton("Create", this);
    m_createButton->setObjectName("CreateBtn");
    m_createButton->setDefault(true);
    btnRow->addWidget(m_createButton);

    root->addLayout(btnRow);

    connect(m_createButton, &QPushButton::clicked, this, &CreateRoomDialog::onCreateClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_passwordCheck, &QCheckBox::toggled, this, &CreateRoomDialog::onPasswordToggled);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
    connect(m_passwordEdit, &QLineEdit::textChanged, this, &CreateRoomDialog::validateInput);
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
    // Capture form values. Delay/prediction stay at their loadDefaults()
    // values (or the struct defaults 2/7) — host adjusts in-room.
    m_name = m_nameEdit->text().trimmed();
    // m_romName / m_romMd5 were set from the lobby picker in the constructor.
    m_romRegion  = ""; // baked into the ROM; resolved later via md5 lookup
    m_maxPlayers = m_maxPlayersCombo ? m_maxPlayersCombo->currentData().toInt() : 4;
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
    if (m_maxPlayersCombo) m_maxPlayersCombo->setEnabled(enabled);
    m_passwordCheck->setEnabled(enabled);
    m_passwordEdit->setEnabled(enabled && m_passwordCheck->isChecked());
    m_createButton->setEnabled(enabled);
}

void CreateRoomDialog::loadDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    if (s.contains("MaxPlayers") && m_maxPlayersCombo)
    {
        const int val = s.value("MaxPlayers").toInt();
        const int idx = m_maxPlayersCombo->findData(val);
        if (idx >= 0) m_maxPlayersCombo->setCurrentIndex(idx);
    }
    // Seed the initial delay/prediction from the last in-room values the
    // host configured. The in-room view writes to the same keys.
    if (s.contains("Delay"))      m_delay = s.value("Delay").toInt();
    if (s.contains("Prediction")) m_prediction = s.value("Prediction").toInt();
    s.endGroup();
}

void CreateRoomDialog::saveDefaults()
{
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    // The game selection is persisted by the lobby's shared picker, not here.
    s.setValue("MaxPlayers", m_maxPlayers);
    // Delay/prediction persistence moves to the in-room view; CreateRoom
    // only consumes those defaults, doesn't write them.
    s.endGroup();
}

#endif // NETPLAY
