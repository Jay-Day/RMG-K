/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef LOBBYCLIENT_HPP
#define LOBBYCLIENT_HPP

#ifdef NETPLAY

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>

class QWebSocket;
class QUdpSocket;

namespace UserInterface
{
namespace Dialog
{

// LobbyClient is the Qt-side wrapper around the WebSocket protocol spoken by
// rmgk-lobby. It owns one QWebSocket plus a UDP socket that talks to the
// server's anchor service (port 6364 on the same host).
//
// Threading: this class lives on the UI thread; QWebSocket/QUdpSocket are
// driven by the Qt event loop. All signals are emitted on the UI thread.
class LobbyClient : public QObject
{
    Q_OBJECT

public:
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Authenticating, // WS open, HELLO sent, awaiting HELLO_OK
        Connected,
        Failed,
    };
    Q_ENUM(ConnectionState)

    struct LobbyUser
    {
        quint64 id = 0;
        QString username;
        QString state;           // matches protocol UserState strings
        QString region;
        quint16 pingToServer = 0;
        quint64 currentRoomId = 0;
        QString currentRoomName;
        QString searchingRom;    // ROM the user is queued for while state == searching
    };

    struct LobbyRoomSummary
    {
        quint64 id = 0;
        QString name;
        quint64 hostId = 0;
        QString hostName;
        QString romName;
        QString romMd5;
        int players = 0;
        int maxPlayers = 0;
        QString state;
        bool hasPassword = false;
        QStringList playerNames;   // seated players (for Ongoing Matches)
        qint64 startedAtMs = 0;    // match start, unix ms (0 until in-game)
        bool    broadcasting = false; // a player is streaming this match for spectators
        quint64 matchId = 0;          // current match id (set only while broadcasting)
    };

    struct LobbyMatchPeer
    {
        quint64 userId = 0;
        QString username;
        QString publicIp;
        quint16 publicPort = 0;
        QString localIp;
        int slot = 0;
    };

    struct ChatMessage
    {
        QString channel;
        quint64 fromUserId = 0;
        QString fromUsername;
        QString message;
        qint64 serverTimeMs = 0;
    };

    explicit LobbyClient(QObject* parent = nullptr);
    ~LobbyClient() override;

    // Connect to a server. wsUrl is like "ws://216.128.157.98:8080/ws".
    // udpAddr is the server's UDP anchor host:port (often the same hostname
    // as wsUrl, port 6364). If empty, derived from wsUrl host + default 6364.
    void connectToServer(const QString& wsUrl, const QString& username,
                         const QStringList& romHashes, const QString& udpAddr = QString());
    void disconnectFromServer();

    ConnectionState state() const { return m_state; }
    quint64 selfUserId() const { return m_selfUserId; }
    QString selfRegion() const { return m_region; }
    const QHash<quint64, LobbyUser>& users() const { return m_users; }
    const QHash<quint64, LobbyRoomSummary>& rooms() const { return m_rooms; }

    // Chat
    void sendChat(const QString& channel, const QString& message);
    void requestChatHistory(const QString& channel);

    // Rooms
    void createRoom(const QString& name, const QString& romName, const QString& romMd5,
                    const QString& romRegion, int maxPlayers, int delay, int prediction,
                    int pacing, const QString& password = QString());
    void joinRoom(quint64 roomId, const QString& password = QString());
    void leaveRoom();
    void startRoom();
    void kickFromRoom(quint64 userId);

    // Host-only: swap two seats (P1-P4) to re-order players before the match.
    // Server validates (host, waiting, valid slots) and rebroadcasts ROOM_STATE.
    void swapSeats(int slotA, int slotB);

    // Host-only: change the room's rollback delay / prediction / pacing. The
    // *Auto flags tell the server whether delay/prediction are host-Auto-driven
    // so non-hosts can mirror the host's label (pacing has no Auto). Server must
    // ack with a fresh ROOM_STATE so all seated peers stay in sync.
    void updateRoomSettings(int delay, int prediction, int pacing, bool delayAuto, bool predictionAuto);

    // Ping probe — server replies with target's UDP endpoint; client probes directly.
    void requestPingProbe(quint64 targetUserId);

    // Most recent measured round-trip to this peer in milliseconds, or -1 if
    // no PROBE_REPLY has been received from them. Updated whenever a probe
    // we sent comes back.
    int measuredPingMs(quint64 userId) const;

    // Quick match queue. The ROM (name + md5) scopes the search so the server
    // only pairs players queued for the same game; the name lets the matched
    // room resolve to a local ROM by name on both clients.
    void quickMatchJoin(const QString& romName, const QString& romMd5);
    void quickMatchCancel();

    // Match lifecycle reports back to server (called from emulation hookup).
    void reportMatchConnected(quint64 matchId, quint64 peerUserId);
    void reportMatchPunchFailed(quint64 matchId, quint64 peerUserId);
    void reportMatchFinished(quint64 matchId);

    // Broadcast (one player streams the live match's .krec up to the server).
    void sendBroadcastBegin(quint64 matchId);
    void sendBroadcastData(quint64 matchId, const QByteArray& chunk, int liveFrame); // raw krec bytes (base64'd) + broadcaster's live frame
    // Upload a savestate keyframe (already compressed) for frame F. Split into chunks
    // so each message stays under the server's per-message read limit.
    void sendBroadcastKeyframe(quint64 matchId, const QByteArray& savestate, int frame);
    void sendBroadcastEnd(quint64 matchId);

    // Spectate (pull a broadcast match's krec stream back down).
    void startSpectate(quint64 matchId);
    void stopSpectate(quint64 matchId);

    // Moderation. sendAdminAuth claims the moderator role with a password;
    // sendModAction issues a command once authenticated. action is one of
    // "kick"/"mute"/"timeout"/"ban"/"unban"/"unmute"/"list"; target is a
    // username (or an IP for unban/unmute); duration is "10m"/"1h"/"2d"/"".
    void sendAdminAuth(const QString& password);
    void sendModAction(const QString& action, const QString& target,
                       const QString& duration = QString(), const QString& reason = QString());
    bool isModerator() const { return m_isModerator; }

    // UDP anchor port management — exposed so the GekkoNet session can take
    // the same local port we registered with the server (matches NAT mapping).
    quint16 localUdpPort() const;
    void releaseUdpAnchor();
    void reopenUdpAnchor();

    // Fire a burst of UDP punch packets from the anchor socket to each peer's
    // public endpoint. Call this *before* releaseUdpAnchor() so the punch goes
    // out from the same NAT mapping GekkoNet will inherit when it re-binds.
    // Peers silently drop these on their still-open anchor socket.
    void punchPeerEndpoints(const QList<LobbyMatchPeer>& peers);
    bool syncPrematchManifest(const QList<LobbyMatchPeer>& peers, int localSlot, const QString& romFile, QString& error);

signals:
    void stateChanged(LobbyClient::ConnectionState newState);
    void helloFailed(const QString& reason);
    void connectError(const QString& humanMessage);

    void presenceFull();                       // full user list available via users()
    void userAdded(quint64 userId);
    void userRemoved(quint64 userId);
    void userUpdated(quint64 userId);

    void roomListChanged();
    void roomCreated(quint64 roomId);
    void roomCreateFailed(const QString& reason);
    void roomJoinOk(quint64 roomId);
    void roomJoinFailed(const QString& reason);
    void roomLeft(QString reason);
    void roomStateChanged(const QJsonObject& roomState);

    void chatMessageReceived(const LobbyClient::ChatMessage& msg);
    void chatHistoryReceived(const QString& channel, const QList<LobbyClient::ChatMessage>& msgs);

    void pingProbeReply(quint64 targetUserId, const QString& endpoint);
    void pingProbeMeasured(quint64 targetUserId, int rttMs);

    void matchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers);
    void matchPeerLeft(quint64 matchId, quint64 userId, int slot, const QString& reason);

    void quickMatchStatus(bool searching, int queueSize);

    // Spectate stream (server → spectator). data carries decoded krec bytes.
    void spectateBegan(quint64 matchId);
    void spectateData(quint64 matchId, const QByteArray& data, int liveFrame);
    // A reassembled savestate keyframe (still compressed) the spectator should restore
    // at frame, before replaying the krec tail that follows in spectateData.
    void spectateKeyframe(quint64 matchId, int frame, const QByteArray& savestate);
    void spectateEnded(quint64 matchId, const QString& reason);
    void spectateFailed(quint64 matchId, const QString& reason);

    // Moderation (server → client).
    void adminAuthResult(bool ok, const QString& nameOrReason); // ok => moderator granted, name; else reason
    void modNotice(const QString& severity, const QString& text); // system notice ("info"/"warn"/"error")
    void modListReceived(const QJsonArray& bans, const QJsonArray& mutes); // reply to "list"

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessageReceived(const QString& msg);
    void onWsErrorOccurred();

    void onUdpReadyRead();
    void onUdpKeepaliveTimer();
    void onHeartbeatTimer();

private:
    void setState(ConnectionState s);
    void sendEnvelope(const QString& type, const QJsonObject& data = {}, const QString& id = {});
    void handleEnvelope(const QJsonObject& env);

    // Specific message handlers
    void handleHelloOk(const QJsonObject& data);
    void handleHelloFail(const QJsonObject& data);
    void handleHeartbeatAck(const QJsonObject& data);
    void handlePresenceFull(const QJsonObject& data);
    void handlePresenceDelta(const QJsonObject& data);
    void handleRoomList(const QJsonObject& data);
    void handleRoomCreated(const QJsonObject& data);
    void handleRoomCreateFail(const QJsonObject& data);
    void handleRoomState(const QJsonObject& data);
    void handleRoomJoinOk(const QJsonObject& data);
    void handleRoomJoinFail(const QJsonObject& data);
    void handleRoomLeft(const QJsonObject& data);
    void handleChatMsg(const QJsonObject& data);
    void handleChatHistoryReply(const QJsonObject& data);
    void handlePingProbeReply(const QJsonObject& data);
    void handleMatchBegin(const QJsonObject& data);
    void handleMatchPeerLeft(const QJsonObject& data);
    void handleQuickMatchStatus(const QJsonObject& data);
    void handleSpectateBegin(const QJsonObject& data);
    void handleSpectateData(const QJsonObject& data);
    void handleSpectateKeyframe(const QJsonObject& data);
    void handleSpectateEnd(const QJsonObject& data);
    void handleSpectateFail(const QJsonObject& data);
    void handleAdminAuthOk(const QJsonObject& data);
    void handleAdminAuthFail(const QJsonObject& data);
    void handleModNotice(const QJsonObject& data);
    void handleModList(const QJsonObject& data);

    void initiateUdpAnchor();
    void sendUdpRegister();
    void sendUdpKeepalive();

    static LobbyUser parsePresenceUser(const QJsonObject& obj);

    ConnectionState m_state = ConnectionState::Disconnected;

    QWebSocket* m_ws = nullptr;
    QUdpSocket* m_udp = nullptr;

    QTimer* m_heartbeatTimer = nullptr;
    QTimer* m_udpKeepaliveTimer = nullptr;
    bool m_inPrematchSync = false;

    // Incoming spectate keyframe reassembly (chunked SPECTATE_KEYFRAME messages).
    int        m_kfRecvFrame = -1;
    int        m_kfRecvCount = 0;
    int        m_kfRecvGot = 0;
    QByteArray m_kfRecvBuf;
    QList<bool> m_kfRecvChunkSeen;

    // Pending HELLO context
    QString m_pendingUsername;
    QStringList m_pendingRomHashes;
    QString m_pendingLocalIp;
    QString m_udpAnchorHost;
    quint16 m_udpAnchorPort = 6364;

    // Session state
    quint64 m_selfUserId = 0;
    QString m_observedIp;
    QString m_region;
    bool    m_isModerator = false; // set once ADMIN_AUTH_OK is received

    QHash<quint64, LobbyUser> m_users;
    QHash<quint64, LobbyRoomSummary> m_rooms;

    // In-flight UDP PROBE state, keyed by per-request nonce. Each entry maps
    // a sent probe back to the target user and the wall-clock send time so
    // we can compute RTT when PROBE_REPLY comes back.
    struct ProbeInFlight
    {
        quint64 targetUserId = 0;
        qint64  sendMs       = 0;
    };
    QHash<quint64, ProbeInFlight> m_pendingProbes;

    // Last measured round-trip per peer (userId → ms). Survives between
    // measurements so the UI has a value to render even when the next probe
    // is still in flight.
    QHash<quint64, int> m_measuredPing;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYCLIENT_HPP
