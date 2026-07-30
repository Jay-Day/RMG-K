/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyConnectDialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QDialogButtonBox>
#include <QFont>

using namespace UserInterface::Dialog;

namespace
{
    // Production lobby server hosted on Vultr Chicago. Update here when DNS
    // is set up (e.g. ws://lobby.rmgk.net:8080/ws). The dialog no longer
    // exposes server choice — every client connects here.
    constexpr const char* kDefaultLobbyUrl = "ws://216.128.157.98:8080/ws";
} // namespace

QString LobbyConnectDialog::defaultServerUrl()
{
    // Allow overriding the lobby server without a rebuild — handy for local
    // testing. e.g. set RMGK_LOBBY_URL=ws://127.0.0.1:8080/ws to point at a
    // server running on this machine. Falls back to the production server.
    const QString override = qEnvironmentVariable("RMGK_LOBBY_URL");
    if (!override.trimmed().isEmpty())
        return override.trimmed();
    return QString::fromUtf8(kDefaultLobbyUrl);
}

LobbyConnectDialog::LobbyConnectDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Join Rollback Netplay");
    setModal(true);
    setMinimumWidth(380);
    buildUi();
    loadSettings();
    validateInput();
}

void LobbyConnectDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);

    auto* heading = new QLabel("Welcome to Netplay!", this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* form = new QFormLayout;

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setMaxLength(16);
    m_usernameEdit->setPlaceholderText("3-16 characters: letters, numbers, _ - .");
    auto* validator = new QRegularExpressionValidator(
        QRegularExpression(R"([A-Za-z0-9_\-\.]{1,16})"), this);
    m_usernameEdit->setValidator(validator);
    form->addRow("Enter a username:", m_usernameEdit);

    root->addLayout(form);

    m_validationLbl = new QLabel(this);
    m_validationLbl->setStyleSheet("color: gray;");
    root->addWidget(m_validationLbl);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_connectButton = btnBox->addButton("Connect", QDialogButtonBox::AcceptRole);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_connectButton, &QPushButton::clicked, this, &LobbyConnectDialog::onConnect);

    root->addWidget(btnBox);

    connect(m_usernameEdit, &QLineEdit::textChanged, this, [this]() {
        m_statusMessage.clear();
        validateInput();
    });
}

void LobbyConnectDialog::validateInput()
{
    const QString user = m_usernameEdit->text().trimmed();

    QString reason;
    if (user.length() < 3)
        reason = "Username must be at least 3 characters.";

    const QString message = reason.isEmpty() ? m_statusMessage : reason;
    m_validationLbl->setStyleSheet(
        message.isEmpty() ? "color: gray;" : "color: #c0392b;");
    m_validationLbl->setText(message);
    m_connectButton->setEnabled(reason.isEmpty());
}

void LobbyConnectDialog::onConnect()
{
    m_serverUrl = defaultServerUrl();
    m_username  = m_usernameEdit->text().trimmed();
    saveSettings();
    accept();
}

void LobbyConnectDialog::setUsername(const QString& username)
{
    m_usernameEdit->setText(username);
    m_usernameEdit->setFocus();
    m_usernameEdit->selectAll();
}

void LobbyConnectDialog::setStatusMessage(const QString& message)
{
    m_statusMessage = message;
    validateInput();
}

void LobbyConnectDialog::loadSettings()
{
    QSettings s("RMG-K", "n02");
    m_usernameEdit->setText(s.value("Lobby/Username").toString());
}

void LobbyConnectDialog::saveSettings()
{
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/Username", m_username);
}

#endif // NETPLAY
