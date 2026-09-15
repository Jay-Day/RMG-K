/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "KailleraUIBridge.hpp"

#ifdef NETPLAY

#include "n02_client.h"
#include "common/k_socket.h"  // for sockaddr_in
#include "common/kaillera_encoding.h"

static inline QString safeUtf8Str(const char* s)
{
    return QString::fromUtf8(s ? s : "");
}

static inline QString safeKailleraStr(const char* s)
{
    const std::string utf8 = n02::encoding::decodeKailleraText(s ? s : "");
    return QString::fromUtf8(utf8.c_str());
}

KailleraUIBridge::KailleraUIBridge()
    : QObject(nullptr)
{
}

KailleraUIBridge& KailleraUIBridge::instance()
{
    static KailleraUIBridge s_instance;
    return s_instance;
}

void KailleraUIBridge::setSelectedDelay(int delay)
{
    m_selectedDelay = delay;
}

int KailleraUIBridge::getSelectedDelay() const
{
    return m_selectedDelay;
}

void KailleraUIBridge::registerCallbacks()
{
    n02::UICallbacks cb;

    // In our Qt6 architecture, kaillera_step() and p2p_step() are called from a
    // QTimer on the main thread. All callbacks fire on the main thread, so we
    // emit signals directly (synchronous) instead of using QueuedConnection.
    // This matches the original Win32 behavior where callbacks directly update
    // dialog controls from within kaillera_step().

    // ---- P2P callbacks ----

    cb.p2pChatCallback = [this](char* nick, char* msg) {
        emit p2pChatReceived(safeUtf8Str(nick), safeUtf8Str(msg));
    };

    cb.p2pGameCallback = [this](char* game, int player, int maxplayers) {
        emit p2pGameStarted(safeUtf8Str(game), player, maxplayers);
    };

    cb.p2pEndGameCallback = [this]() {
        emit p2pGameEnded();
    };

    cb.p2pClientDroppedCallback = [this](char* nick, int player) {
        emit p2pClientDropped(safeUtf8Str(nick), player);
    };

    cb.p2pDebugCallback = [this](char* msg) {
        emit p2pDebugMessage(safeUtf8Str(msg));
    };

    cb.p2pHostedGameCallback = [this](char* game) {
        emit p2pHostedGame(safeUtf8Str(game));
    };

    cb.p2pPingCallback = [this](int ping) {
        emit p2pPingUpdated(ping);
    };

    cb.p2pPeerLeftCallback = [this]() {
        emit p2pPeerLeft();
    };

    cb.p2pPeerJoinedCallback = [this]() {
        emit p2pPeerJoined();
    };

    cb.p2pPeerInfoCallback = [this](char* name, char* app) {
        emit p2pPeerInfo(safeUtf8Str(name), safeUtf8Str(app));
    };

    cb.p2pGetSelectedDelayCallback = [this]() -> int {
        return m_selectedDelay;
    };

    cb.p2pSsrvPacketRecvCallback = [this](char* cmd, int len, void* sadr) {
        if (cmd == nullptr || len <= 0) return;
        QByteArray cmdData(cmd, len);
        QByteArray saddrData;
        if (sadr) {
            saddrData = QByteArray(reinterpret_cast<const char*>(sadr),
                                   static_cast<int>(sizeof(sockaddr_in)));
        }
        emit p2pSsrvPacketReceived(cmdData, saddrData);
    };

    cb.p2pFodippCallback = [this](char* host) {
        emit p2pFodippResult(safeUtf8Str(host));
    };

    // ---- Kaillera server callbacks ----

    cb.kailleraUserAddCallback = [this](char* name, int ping, int status, unsigned short id, char conn) {
        emit kailleraUserAdded(safeKailleraStr(name), ping, status, id, conn);
    };

    cb.kailleraGameAddCallback = [this](char* gname, unsigned int id, char* emulator, char* owner, char* users, char status) {
        emit kailleraGameAdded(safeKailleraStr(gname), id, safeKailleraStr(emulator), safeKailleraStr(owner), safeKailleraStr(users), status);
    };

    cb.kailleraChatCallback = [this](char* name, char* msg) {
        emit kailleraChatReceived(safeKailleraStr(name), safeKailleraStr(msg));
    };

    cb.kailleraGameChatCallback = [this](char* name, char* msg) {
        emit kailleraGameChatReceived(safeKailleraStr(name), safeKailleraStr(msg));
    };

    cb.kailleraMotdCallback = [this](char* name, char* msg) {
        emit kailleraMotdReceived(safeKailleraStr(name), safeKailleraStr(msg));
    };

    cb.kailleraUserJoinCallback = [this](char* name, int ping, unsigned short id, char conn) {
        emit kailleraUserJoined(safeKailleraStr(name), ping, id, conn);
    };

    cb.kailleraUserLeaveCallback = [this](char* name, char* quitmsg, unsigned short id) {
        emit kailleraUserLeft(safeKailleraStr(name), safeKailleraStr(quitmsg), id);
    };

    cb.kailleraGameCreateCallback = [this](char* gname, unsigned int id, char* emulator, char* owner) {
        emit kailleraGameCreated(safeKailleraStr(gname), id, safeKailleraStr(emulator), safeKailleraStr(owner));
    };

    cb.kailleraGameCloseCallback = [this](unsigned int id) {
        emit kailleraGameClosed(id);
    };

    cb.kailleraGameStatusChangeCallback = [this](unsigned int id, char status, int players, int maxplayers) {
        emit kailleraGameStatusChanged(id, status, players, maxplayers);
    };

    cb.kailleraUserGameCreateCallback = [this]() {
        emit kailleraUserGameCreated();
    };

    cb.kailleraUserGameJoinedCallback = [this]() {
        emit kailleraUserGameJoined();
    };

    cb.kailleraUserGameClosedCallback = [this]() {
        emit kailleraUserGameClosed();
    };

    cb.kailleraPlayerAddCallback = [this](char* name, int ping, unsigned short id, char conn) {
        emit kailleraPlayerAdded(safeKailleraStr(name), ping, id, conn);
    };

    cb.kailleraPlayerJoinedCallback = [this](char* username, int ping, unsigned short uid, char connset) {
        emit kailleraPlayerJoined(safeKailleraStr(username), ping, uid, connset);
    };

    cb.kailleraPlayerLeftCallback = [this](char* user, unsigned short id) {
        emit kailleraPlayerLeft(safeKailleraStr(user), id);
    };

    cb.kailleraUserKickedCallback = [this]() {
        emit kailleraUserKicked();
    };

    cb.kailleraLoginStatCallback = [this](char* lsmsg) {
        emit kailleraLoginStatus(safeKailleraStr(lsmsg));
    };

    cb.kailleraPlayerDroppedCallback = [this](char* user, int gdpl) {
        emit kailleraPlayerDropped(safeKailleraStr(user), gdpl);
    };

    cb.kailleraGameStartCallback = [this](char* game, char player, char players) {
        emit kailleraGameStarted(safeKailleraStr(game), static_cast<int>(player), static_cast<int>(players));
    };

    cb.kailleraGameNetsyncWaitCallback = [this](int tx) {
        emit kailleraNetsyncWait(tx);
    };

    cb.kailleraEndGameCallback = [this]() {
        emit kailleraGameEnded();
    };

    cb.kailleraDebugCallback = [this](char* msg) {
        emit kailleraDebugMessage(safeKailleraStr(msg));
    };

    cb.kailleraErrorCallback = [this](char* msg) {
        emit kailleraErrorMessage(safeKailleraStr(msg));
    };

    cb.recordingFileClosedCallback = [this]() {
        emit recordingFileClosed();
    };

    n02::setUICallbacks(cb);
}

void KailleraUIBridge::unregisterCallbacks()
{
    n02::UICallbacks empty;
    n02::setUICallbacks(empty);
}

#endif // NETPLAY
