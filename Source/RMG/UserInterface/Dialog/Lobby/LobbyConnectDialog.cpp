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
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QIcon>
#include <QPixmap>

using namespace UserInterface::Dialog;

namespace
{
    constexpr const char* kDefaultLobbyUrl = "ws://168.119.143.149:8088";
} // namespace

QString LobbyConnectDialog::defaultServerUrl()
{
    const QString override = qEnvironmentVariable("RMGK_LOBBY_URL");
    if (!override.trimmed().isEmpty())
        return override.trimmed();
    return QString::fromUtf8(kDefaultLobbyUrl);
}

LobbyConnectDialog::LobbyConnectDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Connect to RMG-K");
    setModal(true);
    setFixedSize(340, 280);
    setObjectName("LobbyConnectDialog");

    buildUi();
    loadSettings();
    validateInput();
}

void LobbyConnectDialog::buildUi()
{
    setStyleSheet(R"(
        QDialog#LobbyConnectDialog {
            background-color: #14161f;
            color: #e0e0e0;
            font-family: 'Segoe UI', system-ui, sans-serif;
        }
        QLabel {
            color: #a0a5b5;
        }
        QLineEdit {
            background-color: #1a1c24;
            color: #ffffff;
            border: 1px solid #2e3244;
            border-radius: 6px;
            padding: 7px 10px;
            font-size: 12.5px;
            selection-background-color: #0084ff;
        }
        QLineEdit:focus {
            border: 1.5px solid #0084ff;
            background-color: #1c1e28;
        }
    )");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(22, 16, 22, 16);
    lay->setSpacing(8);

    // Centered RMG-K Emblem Vector Logo
    auto* logoLabel = new QLabel(this);
    QIcon svgIcon(":/Resource/RMGK.svg");
    QPixmap logoPix;
    if (!svgIcon.isNull())
    {
        logoPix = svgIcon.pixmap(QSize(130, 52));
    }
    if (logoPix.isNull())
    {
        logoPix.load(":/Resource/RMGK.png");
    }
    if (!logoPix.isNull())
    {
        logoLabel->setPixmap(logoPix.scaled(130, 52, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logoLabel->setAlignment(Qt::AlignHCenter);
        lay->addWidget(logoLabel);
    }

    // Title & Subtitle
    auto* title = new QLabel("Connect to RMG-K", this);
    title->setAlignment(Qt::AlignHCenter);
    title->setStyleSheet("color: #ffffff; font-size: 16px; font-weight: 700;");
    lay->addWidget(title);

    auto* intro = new QLabel("Enter your details to join the network.", this);
    intro->setWordWrap(true);
    intro->setAlignment(Qt::AlignHCenter);
    intro->setStyleSheet("color: #8a8f9e; font-size: 11px;");
    lay->addWidget(intro);

    lay->addSpacing(2);

    // Username Field with Label above
    auto* userFieldLay = new QVBoxLayout;
    userFieldLay->setSpacing(3);

    auto* userLbl = new QLabel("Username", this);
    userLbl->setStyleSheet("color: #a0a5b5; font-size: 11px; font-weight: 600;");
    userFieldLay->addWidget(userLbl);

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setMaxLength(16);
    m_usernameEdit->setPlaceholderText("yourusername");
    auto* validator = new QRegularExpressionValidator(
        QRegularExpression(R"([A-Za-z0-9_\-\.]{1,16})"), this);
    m_usernameEdit->setValidator(validator);
    userFieldLay->addWidget(m_usernameEdit);

    lay->addLayout(userFieldLay);

    m_validationLbl = new QLabel(this);
    m_validationLbl->setWordWrap(true);
    m_validationLbl->setAlignment(Qt::AlignHCenter);
    m_validationLbl->setStyleSheet("color: #e74c3c; font-size: 11px;");
    lay->addWidget(m_validationLbl);

    // Pill Connect Button
    m_connectButton = new QPushButton("Connect", this);
    m_connectButton->setDefault(true);
    m_connectButton->setMinimumHeight(34);
    m_connectButton->setCursor(Qt::PointingHandCursor);
    m_connectButton->setStyleSheet(R"(
        QPushButton {
            background-color: #0084ff;
            color: #ffffff;
            border: none;
            border-radius: 17px;
            padding: 6px 28px;
            font-size: 13px;
            font-weight: 700;
        }
        QPushButton:hover {
            background-color: #1a92ff;
        }
        QPushButton:pressed {
            background-color: #006cd4;
        }
        QPushButton:disabled {
            background-color: #262a36;
            color: #555b6e;
        }
    )");

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 2, 0, 0);
    btnRow->addStretch(1);
    btnRow->addWidget(m_connectButton, 2);
    btnRow->addStretch(1);
    lay->addLayout(btnRow);

    // Subtle Cancel button
    auto* cancelBtn = new QPushButton("Cancel", this);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setStyleSheet(R"(
        QPushButton {
            background: transparent;
            color: #8a8f9e;
            border: none;
            font-size: 11px;
            text-decoration: underline;
        }
        QPushButton:hover {
            color: #ffffff;
        }
    )");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto* cancelRow = new QHBoxLayout();
    cancelRow->setContentsMargins(0, 0, 0, 0);
    cancelRow->addStretch(1);
    cancelRow->addWidget(cancelBtn);
    cancelRow->addStretch(1);
    lay->addLayout(cancelRow);

    connect(m_connectButton, &QPushButton::clicked, this, &LobbyConnectDialog::onConnect);
    connect(m_usernameEdit, &QLineEdit::returnPressed, this, &LobbyConnectDialog::onConnect);
    connect(m_usernameEdit, &QLineEdit::textChanged, this, &LobbyConnectDialog::validateInput);
}

void LobbyConnectDialog::validateInput()
{
    const QString user = m_usernameEdit->text().trimmed();

    QString reason;
    if (user.length() < 3)
        reason = "Username must be at least 3 characters.";

    m_validationLbl->setText(reason);
    m_connectButton->setEnabled(reason.isEmpty());
}

void LobbyConnectDialog::onConnect()
{
    if (!m_connectButton->isEnabled()) return;
    m_serverUrl = defaultServerUrl();
    m_username  = m_usernameEdit->text().trimmed();
    saveSettings();
    accept();
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
