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

using namespace UserInterface::Dialog;

CreateRoomDialog::CreateRoomDialog(const QString& defaultUsername,
                                   const QString& gameName, const QString& gameMd5,
                                   QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Create Netplay Room");
    setModal(true);
    setFixedWidth(420);
    setMinimumHeight(440);
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
        QDialog {
            background-color: #1a1c23;
            color: #e0e0e0;
            font-family: 'Segoe UI', system-ui, sans-serif;
            font-size: 13px;
        }
        QLabel {
            color: #a0a5b5;
            font-size: 12px;
            font-weight: 600;
        }
        QLabel#HeaderTitle {
            color: #ffffff;
            font-size: 18px;
            font-weight: 700;
        }
        QLineEdit, QSpinBox {
            background-color: #121318;
            color: #ffffff;
            border: 1px solid #2a2d3c;
            border-radius: 8px;
            padding: 10px 14px;
            font-size: 13px;
            selection-background-color: #0084ff;
        }
        QLineEdit:focus, QSpinBox:focus {
            border: 1.5px solid #0084ff;
            background-color: #14161f;
        }
        QLabel#GameCard {
            background-color: #121318;
            color: #38b6ff;
            border: 1px solid #2a2d3c;
            border-radius: 8px;
            padding: 10px 14px;
            font-size: 13px;
            font-weight: 600;
        }
        QCheckBox {
            color: #a0a5b5;
            font-size: 12.5px;
            spacing: 8px;
            font-weight: 500;
        }
        QCheckBox::indicator {
            width: 17px;
            height: 17px;
            border-radius: 4px;
            border: 1px solid #323544;
            background-color: #121318;
        }
        QCheckBox::indicator:checked {
            background-color: #0084ff;
            border-color: #0084ff;
        }
        QPushButton#CreateBtn {
            background-color: #0084ff;
            color: #ffffff;
            border: none;
            border-radius: 18px;
            padding: 10px 32px;
            font-size: 13px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QPushButton#CreateBtn:hover {
            background-color: #1a92ff;
        }
        QPushButton#CreateBtn:pressed {
            background-color: #006cd4;
        }
        QPushButton#CreateBtn:disabled {
            background-color: #262a36;
            color: #555b6e;
        }
        QPushButton#CancelBtn {
            background-color: transparent;
            color: #a0a5b5;
            border: none;
            padding: 8px 16px;
            font-size: 13px;
        }
        QPushButton#CancelBtn:hover {
            color: #ffffff;
            text-decoration: underline;
        }
    )");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 22);
    root->setSpacing(16);

    // Title Header with close button
    auto* headerRow = new QHBoxLayout;
    auto* headerTitle = new QLabel("Create Netplay Room", this);
    headerTitle->setObjectName("HeaderTitle");
    headerRow->addWidget(headerTitle);
    headerRow->addStretch();

    auto* closeBtn = new QPushButton("✕", this);
    closeBtn->setFixedSize(24, 24);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet("QPushButton { background: transparent; color: #8a8f9e; border: none; font-size: 15px; } QPushButton:hover { color: #ffffff; }");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerRow->addWidget(closeBtn);
    root->addLayout(headerRow);

    // Room Name Field (Label above input)
    auto* nameLay = new QVBoxLayout;
    nameLay->setSpacing(6);
    auto* nameLbl = new QLabel("Room Name", this);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMaxLength(48);
    if (!defaultUsername.isEmpty())
        m_nameEdit->setText(QString("%1's Room").arg(defaultUsername));
    else
        m_nameEdit->setPlaceholderText("My Room");
    nameLay->addWidget(nameLbl);
    nameLay->addWidget(m_nameEdit);
    root->addLayout(nameLay);

    // Optional Password
    auto* pwLay = new QVBoxLayout;
    pwLay->setSpacing(6);
    m_passwordCheck = new QCheckBox("Password-protect this room", this);
    pwLay->addWidget(m_passwordCheck);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Room password");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setVisible(false);
    pwLay->addWidget(m_passwordEdit);
    root->addLayout(pwLay);

    // Game Field (Card display)
    auto* gameLay = new QVBoxLayout;
    gameLay->setSpacing(6);
    auto* gameTitleLbl = new QLabel("Game", this);
    m_gameLabel = new QLabel(this);
    m_gameLabel->setObjectName("GameCard");
    m_gameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gameLay->addWidget(gameTitleLbl);
    gameLay->addWidget(m_gameLabel);
    root->addLayout(gameLay);

    // Max Players Field (2 to 5 players)
    auto* playersLay = new QVBoxLayout;
    playersLay->setSpacing(6);
    auto* playersLbl = new QLabel("Max Players", this);
    m_maxPlayersSpin = new QSpinBox(this);
    m_maxPlayersSpin->setRange(2, 5);
    m_maxPlayersSpin->setValue(4);
    m_maxPlayersSpin->setSuffix(" players");
    playersLay->addWidget(playersLbl);
    playersLay->addWidget(m_maxPlayersSpin);
    root->addLayout(playersLay);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px; font-weight: 500;");
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    root->addStretch(1);

    // Action Buttons
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setObjectName("CancelBtn");
    m_cancelButton->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(m_cancelButton);

    m_createButton = new QPushButton("CREATE", this);
    m_createButton->setObjectName("CreateBtn");
    m_createButton->setCursor(Qt::PointingHandCursor);
    m_createButton->setDefault(true);
    btnRow->addWidget(m_createButton);

    root->addLayout(btnRow);

    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &CreateRoomDialog::onCreateClicked);

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
    // Capture form values. Delay/prediction stay at their loadDefaults()
    // values (or the struct defaults 2/7) — host adjusts in-room.
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
