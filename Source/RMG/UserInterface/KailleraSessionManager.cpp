/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "KailleraSessionManager.hpp"
#include "KailleraUIBridge.hpp"
#ifdef NETPLAY
#include "Dialog/Kaillera/KailleraNetplayDialog.hpp"
#endif

#include <RMG-Core/Kaillera.hpp>

#include <QMetaObject>
#include <QWidget>
#include <QEventLoop>

KailleraSessionManager::KailleraSessionManager(QWidget* parent)
    : QObject(parent)
    , m_gameActive(false)
    , m_playerNumber(-1)
    , m_totalPlayers(0)
    , m_parentWidget(parent)
{
#ifdef NETPLAY
    // Register n02 UI callbacks (detailed server browser / P2P UI events)
    KailleraUIBridge::instance().registerCallbacks();
#endif

    // Register Kaillera callbacks
    // These callbacks are invoked from Kaillera's internal thread
    // We use QMetaObject::invokeMethod with Qt::QueuedConnection to safely
    // marshal calls to the Qt main thread

    CoreSetKailleraCallbacks(
        // Game start callback
        [this](std::string game, int player, int numPlayers) {
            QMetaObject::invokeMethod(this,
                [this, game, player, numPlayers]() {
                    this->handleGameStart(QString::fromStdString(game), player, numPlayers);
                },
                Qt::QueuedConnection);
        },
        // Chat received callback
        [this](std::string nick, std::string text) {
            QMetaObject::invokeMethod(this,
                [this, nick, text]() {
                    this->handleChatReceived(QString::fromStdString(nick),
                                            QString::fromStdString(text));
                },
                Qt::QueuedConnection);
        },
        // Client dropped callback
        [this](std::string nick, int playerNum) {
            QMetaObject::invokeMethod(this,
                [this, nick, playerNum]() {
                    this->handlePlayerDropped(QString::fromStdString(nick), playerNum);
                },
                Qt::QueuedConnection);
        },
        // More infos callback (optional, can be nullptr)
        nullptr
    );
}

KailleraSessionManager::~KailleraSessionManager()
{
    if (m_gameActive)
    {
        endGame();
    }

#ifdef NETPLAY
    // Unregister n02 UI callbacks
    KailleraUIBridge::instance().unregisterCallbacks();
#endif
}

bool KailleraSessionManager::showServerDialog(int initialTab)
{
#ifdef NETPLAY
    // Pass nullptr as parent so the dialog is a truly independent top-level
    // window. On Linux, child dialogs always stay above their parent in the
    // window manager stacking order, preventing the emulator from being
    // brought to the front or fullscreened.
    KailleraNetplayDialog dialog(nullptr);
    dialog.setWindowModality(Qt::NonModal);
    QObject::connect(&dialog, &KailleraNetplayDialog::rollbackSessionPreparing,
                     this, &KailleraSessionManager::rollbackSessionPreparing);
    QObject::connect(&dialog, &KailleraNetplayDialog::rollbackSessionRequested,
                     this, &KailleraSessionManager::rollbackSessionRequested);
    if (initialTab >= 0)
    {
        dialog.setActiveTab(initialTab);
    }
    dialog.show();

    QEventLoop loop;
    QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    loop.exec();

    return (dialog.result() == QDialog::Accepted);
#else
    // Kaillera is Windows-only
    (void)initialTab;
    return false;
#endif
}

bool KailleraSessionManager::isGameActive() const
{
    return m_gameActive;
}

QString KailleraSessionManager::getCurrentGame() const
{
    return m_currentGame;
}

int KailleraSessionManager::getPlayerNumber() const
{
    return m_playerNumber;
}

int KailleraSessionManager::getTotalPlayers() const
{
    return m_totalPlayers;
}

void KailleraSessionManager::sendChatMessage(QString message)
{
    if (!m_gameActive)
    {
        return;
    }

    CoreKailleraSendChat(message.toStdString());
}

void KailleraSessionManager::endGame()
{
    if (!m_gameActive)
    {
        return;
    }

    CoreEndKailleraGame();
    m_gameActive = false;
    m_currentGame.clear();
    m_playerNumber = -1;
    m_totalPlayers = 0;

    emit gameEnded();
}

//
// Private callback handlers (called on Qt main thread)
//

void KailleraSessionManager::handleGameStart(QString game, int player, int numPlayers)
{
    m_gameActive = true;
    m_currentGame = game;
    m_playerNumber = player;
    m_totalPlayers = numPlayers;

    emit gameStarted(game, player, numPlayers);
}

void KailleraSessionManager::handleChatReceived(QString nick, QString text)
{
    emit chatReceived(nick, text);
}

void KailleraSessionManager::handlePlayerDropped(QString nick, int playerNum)
{
    // Just forward the signal — all drop/stop logic is handled by
    // KailleraServerBrowserDialog::onPlayerDropped() via the UIBridge path.
    emit playerDropped(nick, playerNum);
    // Just update total players count
    if (m_totalPlayers > 1)
    {
        m_totalPlayers--;
    }
}
