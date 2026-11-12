/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifdef NETPLAY

#include "AccountEmailDialog.hpp"
#include "LobbyClient.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

using namespace UserInterface::Dialog;

namespace
{
constexpr const char* kErrorStyle = "color: #c0392b;";

// a quick check before asking the lobby, which checks properly
bool looksLikeEmail(const QString& text)
{
    const int at = text.lastIndexOf('@');
    return at > 0 && text.indexOf('.', at) > at + 1 && !text.endsWith('.') && !text.contains(' ');
}
} // namespace

AccountEmailDialog::AccountEmailDialog(LobbyClient* client, QWidget* parent) : QDialog(parent), m_client(client)
{
    setWindowTitle(tr("Email Address"));
    setMinimumWidth(460);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto* intro = new QLabel(tr("If you forget your password, the lobby can email a reset code to this "
                                "address. Other players don't see it."), this);
    intro->setWordWrap(true);
    intro->setForegroundRole(QPalette::PlaceholderText);
    root->addWidget(intro);

    auto* currentRow = new QHBoxLayout;
    m_currentLabel = new QLabel(this);
    m_removeButton = new QPushButton(tr("Remove"), this);
    m_removeButton->setAutoDefault(false);
    currentRow->addWidget(m_currentLabel, 1);
    currentRow->addWidget(m_removeButton);
    root->addLayout(currentRow);

    auto* form = new QFormLayout;
    form->setVerticalSpacing(8);
    auto* addressRow = new QWidget(this);
    auto* addressLay = new QHBoxLayout(addressRow);
    addressLay->setContentsMargins(0, 0, 0, 0);
    m_addressEdit = new QLineEdit(addressRow);
    m_addressEdit->setMaxLength(254);
    m_addressEdit->setPlaceholderText(tr("name@example.com"));
    m_sendButton = new QPushButton(tr("Send code"), addressRow);
    m_sendButton->setAutoDefault(false);
    addressLay->addWidget(m_addressEdit, 1);
    addressLay->addWidget(m_sendButton);
    m_addressLabel = new QLabel(this);
    form->addRow(m_addressLabel, addressRow);

    m_codeRow = new QWidget(this);
    auto* codeLay = new QHBoxLayout(m_codeRow);
    codeLay->setContentsMargins(0, 0, 0, 0);
    m_codeEdit = new QLineEdit(m_codeRow);
    m_codeEdit->setMaxLength(32);
    m_codeEdit->setPlaceholderText("XXXX-XXXX-XXXX");
    m_confirmButton = new QPushButton(tr("Confirm"), m_codeRow);
    m_confirmButton->setAutoDefault(false);
    codeLay->addWidget(m_codeEdit, 1);
    codeLay->addWidget(m_confirmButton);
    form->addRow(tr("Code:"), m_codeRow);
    form->setRowVisible(m_codeRow, false);
    root->addLayout(form);

    m_sentLabel = new QLabel(this);
    m_sentLabel->setWordWrap(true);
    m_sentLabel->setVisible(false);
    root->addWidget(m_sentLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setVisible(false);
    root->addWidget(m_statusLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_sendButton, &QPushButton::clicked, this, &AccountEmailDialog::sendCode);
    connect(m_addressEdit, &QLineEdit::returnPressed, this, &AccountEmailDialog::sendCode);
    connect(m_confirmButton, &QPushButton::clicked, this, &AccountEmailDialog::confirmCode);
    connect(m_codeEdit, &QLineEdit::returnPressed, this, &AccountEmailDialog::confirmCode);
    connect(m_removeButton, &QPushButton::clicked, this, &AccountEmailDialog::removeEmail);

    connect(m_client, &LobbyClient::emailCodeSent, this, [this, form](const QString& address) {
        m_pendingAddress = address;
        m_sendButton->setEnabled(true);
        m_sendButton->setText(tr("Send again"));
        m_sentLabel->setText(tr("We sent a code to <b>%1</b>. Enter it here to use that address. "
                                "Check your spam folder if it doesn't arrive.").arg(address.toHtmlEscaped()));
        m_sentLabel->setVisible(true);
        form->setRowVisible(m_codeRow, true);
        setStatus(QString(), false);
        m_codeEdit->clear();
        m_codeEdit->setFocus();
        adjustSize();
    });
    // confirmed or removed: the lobby window says so in the chat
    connect(m_client, &LobbyClient::emailUpdated, this, [this]() { accept(); });
    connect(m_client, &LobbyClient::emailFailed, this, [this](const QString& message) {
        m_sendButton->setEnabled(true);
        m_sendButton->setText(m_pendingAddress.isEmpty() ? tr("Send code") : tr("Send again"));
        m_confirmButton->setEnabled(true);
        m_removeButton->setEnabled(true);
        setStatus(message.isEmpty() ? tr("That didn't work. Try again.") : message, true);
    });

    showCurrent();
    m_addressEdit->setFocus();
}

void AccountEmailDialog::showCurrent(void)
{
    const QString current = m_client->accountEmail();
    if (current.isEmpty())
    {
        m_currentLabel->setText(tr("No email address yet."));
        m_addressLabel->setText(tr("Email address:"));
    }
    else
    {
        m_currentLabel->setText(tr("Current address: <b>%1</b>").arg(current.toHtmlEscaped()));
        m_addressLabel->setText(tr("New address:"));
    }
    m_removeButton->setVisible(!current.isEmpty());
}

void AccountEmailDialog::setStatus(const QString& text, bool error)
{
    m_statusLabel->setStyleSheet(error ? kErrorStyle : QString());
    m_statusLabel->setText(text);
    m_statusLabel->setVisible(!text.isEmpty());
}

void AccountEmailDialog::sendCode(void)
{
    const QString address = m_addressEdit->text().trimmed();
    if (!looksLikeEmail(address))
    {
        setStatus(tr("Enter an email address like name@example.com."), true);
        m_addressEdit->setFocus();
        return;
    }
    setStatus(QString(), false);
    m_sendButton->setEnabled(false);
    m_sendButton->setText(tr("Sending..."));
    m_client->setAccountEmail(address);
}

void AccountEmailDialog::confirmCode(void)
{
    const QString code = m_codeEdit->text().trimmed();
    if (code.isEmpty())
    {
        setStatus(tr("Enter the code from the email."), true);
        m_codeEdit->setFocus();
        return;
    }
    setStatus(QString(), false);
    m_confirmButton->setEnabled(false);
    m_client->confirmAccountEmail(code);
}

void AccountEmailDialog::removeEmail(void)
{
    const QString current = m_client->accountEmail();
    if (QMessageBox::question(this, tr("Remove Email Address"),
                              tr("Remove %1? You won't be able to email yourself a reset code until you "
                                 "add an address again.").arg(current)) != QMessageBox::Yes)
    {
        return;
    }
    setStatus(QString(), false);
    m_removeButton->setEnabled(false);
    m_client->removeAccountEmail();
}

#endif // NETPLAY
