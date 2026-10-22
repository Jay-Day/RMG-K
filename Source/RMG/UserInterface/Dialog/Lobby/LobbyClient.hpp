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
#include <QSet>
#include <QFutureWatcher>

class QWebSocket;
class QUdpSocket;
class QFile;
// Only used by reference in sendProbeBurst's signature; the .cpp includes it.
class QHostAddress;

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
        QString country;         // ISO 3166-1 alpha-2 from the server; "" on old servers
        QString clientVersion;   // peer's self-reported RMG-K build
        QString connection;      // peer's self-reported transport ("wifi"/"lan"/...)
        // User asked the server to withhold their location: country comes back
        // empty and the UI must not fall back to the region badge either. The
        // region string itself still flows for ping estimation.
        bool anonymous = false;
        quint16 pingToServer = 0;
        quint64 currentRoomId = 0;
        QString currentRoomName;
        QString searchingRom;    // ROM the user is queued for while state == searching
        quint64 watchingMatchId = 0; // live replay the user watches while state == spectating
        bool verified = false;   // logged into a registered account
    };

    // Optional account authentication sent with HELLO. An empty mode
    // connects as a guest.
    struct LobbyAuth
    {
        QString mode;      // "login", "register", "token", "reset", "discord" or "discord_link"
        QString password;  // login/register/discord_link, reset: the new password
        QString token;     // token: remember-me token from a previous HELLO_OK
        QString resetCode; // reset: code from a moderator
        // discord: the authorization code from Discord's redirect (PKCE)
        QString code;
        QString codeVerifier;
        QString redirectUri;
        QString ticket;    // discord_link: from a discord_unlinked HELLO_FAIL
    };

    // The account's linked Discord account (sign-in only, never shown to
    // other players).
    struct DiscordLink
    {
        bool linked = false;
        QString username;
        QString name;
    };

    // An account's standing on one roster (ruleset) of a ranked ladder: each
    // roster is rated separately. ruleset is empty from servers that rate the
    // whole ladder.
    struct LadderStanding
    {
        QString ladder;
        QString ruleset;
        int rating = 0;
        int deviation = 0;
        QString tier;
        int tierIndex = 0;
        bool provisional = true;
        int setsPlayed = 0;
    };

    // A ranked ladder the server runs (from HELLO_OK).
    struct LadderInfo
    {
        QString id;
        QString name;
        QString moduleId;
        QStringList rulesets;
        QHash<QString, QString> rulesetNames; // display names, may be empty
        QList<int> bestOf;
        QStringList romMd5s;
        QStringList tiers;
        int teamSize = 1; // 2 = 2v2 team battles
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
        int viewerCount = 0;
        bool lateJoin = false;        // in game with a free seat a late joiner may take
        bool    ranked = false;
        QString ladder;               // ladder id while ranked
    };

    struct LobbyMatchPeer
    {
        quint64 userId = 0;
        QString username;
        QString publicIp;
        quint16 publicPort = 0;
        QString localIp;
        int slot = 0;
        bool iceSupported = false;
    };

    struct ChatMessage
    {
        QString channel;
        quint64 fromUserId = 0;
        QString fromUsername;
        QString message;
        qint64 serverTimeMs = 0;
        // Sender was a server-authenticated moderator when this was sent.
        // Old servers never send the field, so it stays false.
        bool fromAdmin = false;
    };

    explicit LobbyClient(QObject* parent = nullptr);
    ~LobbyClient() override;

    // Connect to a server. wsUrl is like "ws://216.128.157.98:8080/ws".
    // udpAddr is the server's UDP anchor host:port (often the same hostname
    // as wsUrl, port 6364). If empty, derived from wsUrl host + default 6364.
    void connectToServer(const QString& wsUrl, const QString& username,
                         const QStringList& romHashes, const QString& udpAddr = QString(),
                         const LobbyAuth& auth = LobbyAuth());
    void disconnectFromServer();

    // App-wide input watcher feeding the away detection: any key/mouse/touch
    // event counts as activity. Installed on the QApplication in the ctor.
    bool eventFilter(QObject* watched, QEvent* event) override;

    ConnectionState state() const { return m_state; }
    quint64 selfUserId() const { return m_selfUserId; }
    QString selfRegion() const { return m_region; }
    const QHash<quint64, LobbyUser>& users() const { return m_users; }
    const QHash<quint64, LobbyRoomSummary>& rooms() const { return m_rooms; }

    // Account (valid after HELLO_OK). selfUsername is the server's canonical
    // spelling; isVerified is false for guests.
    QString selfUsername() const { return m_selfUsername; }
    bool isVerified() const { return m_accountId != 0; }
    QString sessionToken() const { return m_sessionToken; }
    // Reason code of the last HELLO_FAIL (e.g. "auth_failed").
    QString lastHelloFailReason() const { return m_lastHelloFailReason; }
    // discord_unlinked: the ticket for a mode "discord_link" login and the
    // Discord account it links.
    QString discordLinkTicket() const { return m_discordLinkTicket; }
    QString discordLinkName() const { return m_discordLinkName; }
    // too_many_attempts: seconds until the lobby accepts a login again (0 if unknown)
    int lastHelloRetryAfter() const { return m_lastHelloRetryAfter; }
    // The account's Discord link (HELLO_OK, ACCOUNT_DISCORD).
    const DiscordLink& discordLink() const { return m_discordLink; }
    // Whether the lobby can email the account (HELLO_OK), and the account's
    // confirmed email address ("" when none).
    bool emailAvailable() const { return m_emailAvailable; }
    QString accountEmail() const { return m_accountEmail; }
    const QList<LadderInfo>& ladders() const { return m_ladders; }
    // The ladder a ROM can be played ranked on, or nullptr.
    const LadderInfo* ladderForRomMd5(const QString& md5) const;
    // every ladder a ROM is on (one per format), 1v1 first
    QList<const LadderInfo*> laddersForRomMd5(const QString& md5) const;
    // Our standing on one roster of a ladder, or nullptr (guest / unknown).
    const LadderStanding* standing(const QString& ladderId, const QString& rulesetId) const;
    static LadderStanding parseStanding(const QJsonObject& obj);
    // The "ranked" block of the last MATCH_BEGIN if it was for matchId,
    // otherwise empty (unranked match).
    QJsonObject matchRankedInfo(quint64 matchId) const
    {
        return matchId == m_lastMatchRankedId ? m_lastMatchRanked : QJsonObject();
    }

    // Chat
    void sendChat(const QString& channel, const QString& message);
    void requestChatHistory(const QString& channel);

    // Rooms
    void createRoom(const QString& name, const QString& romName, const QString& romMd5,
                    const QString& romRegion, int maxPlayers, int delay, int prediction,
                    int pacing, const QString& password = QString(),
                    const QJsonObject& ranked = QJsonObject());
    void joinRoom(quint64 roomId, const QString& password = QString());
    void leaveRoom();
    void startRoom();
    void updateRoomLiveReplay(bool enabled);
    // Host-only, waiting room: turn ranked on/off ({enabled, bestOf, ...}).
    void updateRoomRanked(const QJsonObject& ranked);
    void kickFromRoom(quint64 userId);

    // Host-only: swap two seats (P1-P4) to re-order players before the match.
    // Server validates (host, waiting, valid slots) and rebroadcasts ROOM_STATE.
    void swapSeats(int slotA, int slotB);

    // Publish this player's resolved local input delay for the room seat display.
    void updateLocalFrameDelay(int delay, bool delayAuto);

    // Publish the host-controlled prediction window for the whole room.
    void updateRoomPrediction(int prediction, bool predictionAuto);

    // Ping probe over the already-selected ICE candidate pair. This measures
    // the same direct path that the rollback session will use.
    void requestPingProbe(quint64 targetUserId);

    // Most recent measured round-trip to this peer in milliseconds, or -1 if
    // no PROBE_REPLY has been received from them. Updated whenever a probe
    // we sent comes back.
    int measuredPingMs(quint64 userId) const;

    // Worst measured RTT across every unordered pair in `roomUserIds`.
    // Host-local paths come from our probes; non-host paths are reported to
    // the host over ICE. `complete` is true only when every pair is known.
    int worstRoomPingMs(const QList<quint64>& roomUserIds, bool* complete = nullptr) const;

    // True when every remote in this match has negotiated a direct ICE path.
    // error describes unsupported clients or the first incomplete peer.
    bool allIcePeersConnected(const QList<LobbyMatchPeer>& peers, QString* error = nullptr) const;

    // The running GekkoNet match consumes the room's established libjuice
    // agents directly. While active, lobby/room cleanup must not destroy those
    // agents, and reconnect must be deferred because HELLO_OK reconfigures the
    // process-wide ICE context. Cleanup requested during the match is applied
    // when ownership is released after emulation stops.
    void setMatchTransportActive(bool active);
    bool matchTransportActive() const { return m_matchTransportActive; }

    // Quick match queue. The ROM (name + md5) scopes the search so the server
    // only pairs players queued for the same game; the name lets the matched
    // room resolve to a local ROM by name on both clients.
    // ranked: a RankedRoomRequest object (enabled, ladderId, bestOf, ...) to
    // search for a ranked set; empty for a casual match.
    // continuous (ranked only): search again after each set.
    // keepTeammate (2v2 sessions with a teammate): search with the set's
    // teammate again when they keep the team too.
    // teammate (2v2): the player wanted as teammate; the search waits until
    // they search with this player as theirs. Empty = a random teammate.
    void quickMatchJoin(const QString& romName, const QString& romMd5, const QJsonObject& ranked = QJsonObject(),
                        bool continuous = false, bool keepTeammate = false, const QString& teammate = QString());
    void quickMatchCancel();
    // Answer a rematch offer: play the previous opponent, or keep searching.
    void quickMatchRematch(bool accept);
    // Turn a ranked session's "keep searching" on or off (off = stop after this set).
    void quickMatchSetContinuous(bool continuous);
    // Turn a 2v2 session's "keep my teammate" on or off.
    void quickMatchSetKeepTeammate(bool keep);
    // Tell the lobby our measured ping to another player in the room (ranked
    // rooms check it against their max ping).
    void reportRoomPeerPing(quint64 userId, int rttMs);

    // Match lifecycle reports back to server (called from emulation hookup).
    void reportMatchConnected(quint64 matchId, quint64 peerUserId);
    void reportMatchPunchFailed(quint64 matchId, quint64 peerUserId);
    void reportMatchFinished(quint64 matchId);
    // Ranked: this player's report of the finished set (MATCH_RESULT payload).
    void reportMatchResult(const QJsonObject& result);
    // Changes the account's password; every saved login is revoked and this
    // connection gets a new session token (passwordChangeResult follows).
    void changePassword(const QString& currentPassword, const QString& newPassword);
    // Links or unlinks Discord sign-in for the account (discordLinkResult follows).
    void linkDiscord(const QString& code, const QString& codeVerifier, const QString& redirectUri);
    void unlinkDiscord();
    // Account email: emails a confirmation code to a new address
    // (emailCodeSent), makes it the account's address with that code, or
    // removes it (emailUpdated). emailFailed answers any of them.
    void setAccountEmail(const QString& email);
    void confirmAccountEmail(const QString& code);
    void removeAccountEmail();
    // Ranked replays: offer the set's .krec (size + sha256 + bookmarks). The
    // server accepts only the first offer per set; its bytes then follow as
    // REPLAY_CHUNKs of the accepted chunk size.
    void offerReplay(qint64 setId, int size, const QString& sha256, const QJsonArray& bookmarks);
    void sendReplayChunk(qint64 setId, int index, int count, const QByteArray& data);

    // Broadcast (one player streams the live match's .krec up to the server).
    bool sendBroadcastBegin(quint64 matchId);
    void sendBroadcastData(quint64 matchId, const QByteArray& chunk, int liveFrame); // raw krec bytes (base64'd) + broadcaster's live frame
    // Compress and upload a savestate keyframe plus the first krec record
    // index to consume after restore. Split it so each message stays under the limit.
    // Starts background compression and a paced low-priority upload. Returns false
    // if another keyframe is already compressing/uploading.
    bool sendBroadcastKeyframe(quint64 matchId, const QByteArray& savestate, int frame);
    bool broadcastKeyframeUploadBusy() const;
    // How often to upload a keyframe, from the lobby's last MATCH_BEGIN.
    qint64 broadcastKeyframeIntervalMs() const { return m_broadcastKeyframeIntervalMs; }
    void sendBroadcastEnd(quint64 matchId);
    // Ranked matches: the next chunk of the game module's write journal.
    void sendBroadcastJournal(quint64 matchId, const QByteArray& journal);

    // Spectate (pull a broadcast match's krec stream back down).
    void startSpectate(quint64 matchId);
    void stopSpectate(quint64 matchId);
    bool applySpectateManifest(const QByteArray& manifest);

    // --- Late join (the lobby's MATCH_ADMIT_* hand-off) ---
    // Controller ports the match MATCH_BEGIN started emulates; 0 when it
    // can't take late joiners (an older server or client in the room).
    int matchSeats() const { return m_matchSeats; }
    // The pre-match settings this match was synced with.
    const QByteArray& prematchManifest() const { return m_prematchManifest; }
    // Host: add a waiting room member to the running match.
    void sendMatchAdmit(quint64 matchId, quint64 userId);
    // One step of the hand-off; frame, ok and reason ride on the steps that
    // carry them.
    void sendAdmitStep(quint64 matchId, quint64 admitId, const QString& step,
                       int frame = 0, bool ok = false, const QString& reason = QString());
    // Host → joiner: a compressed blob step ("manifest" or "state"), split
    // to fit the server's message size limit.
    void sendAdmitBlob(quint64 matchId, quint64 admitId, const QString& step, const QByteArray& raw);
    // Connect to a room member mid-match. Only adds an agent; the match's
    // established ones are left alone.
    void ensureIcePeer(quint64 userId);

    // Moderation. sendAdminAuth claims the moderator role with a password;
    // sendModAction issues a command once authenticated. action is one of
    // "kick"/"mute"/"timeout"/"ban"/"unban"/"unmute"/"list"; target is a
    // username (or an IP for unban/unmute); duration is "10m"/"1h"/"2d"/"".
    void sendAdminAuth(const QString& password);
    void sendModAction(const QString& action, const QString& target,
                       const QString& duration = QString(), const QString& reason = QString());
    bool isModerator() const { return m_isModerator; }

    // Legacy UDP-anchor compatibility API. ICE-capable rollback-lobby matches
    // do not call these; they remain for compatibility with older servers.
    quint16 localUdpPort() const;
    void releaseUdpAnchor();
    void reopenUdpAnchor();

    // n02-style shared transport: instead of releasing the anchor socket for
    // GekkoNet to rebind (a handoff with a whole family of races), lend the
    // live socket itself. Returns the native descriptor, or -1 if there is no
    // bound socket. While lent, this side stops READING (GekkoNet's thread
    // owns recvfrom — same reader discipline n02 uses between p2p_step and
    // p2p_modify_play_values); the port, and every peer's NAT mapping to it,
    // never change. reclaim is idempotent and safe to call whether or not a
    // lend happened.
    qintptr lendAnchorToMatch();
    void reclaimAnchorFromMatch();

    // Fire a burst of UDP punch packets from the anchor socket to each peer's
    // public endpoint. Call this *before* releaseUdpAnchor() so the punch goes
    // out from the same NAT mapping GekkoNet will inherit when it re-binds.
    // Peers silently drop these on their still-open anchor socket.
    void punchPeerEndpoints(const QList<LobbyMatchPeer>& peers);
    // `hostUserId` — not "whoever sits in seat 1" — decides who builds and
    // broadcasts the manifest. Seat order is cosmetic and reorderable; the
    // authority for ROM identity and cheat/gameshark sync metadata is the room's
    // actual host. Keying this off the seat let a reordered host hand sync
    // authority to another player, who would then sync everyone against *their*
    // cheat set (a desync/crash, not a clean error).
    bool syncPrematchManifest(const QList<LobbyMatchPeer>& peers, int localSlot,
                              quint64 hostUserId, const QString& romFile, QString& error);

signals:
    void stateChanged(LobbyClient::ConnectionState newState);
    void helloFailed(const QString& reason);
    // Account or standings changed (HELLO_OK, after a rated set).
    void accountUpdated();
    // RANKED_RESULT: how a ranked set was recorded.
    void rankedResult(const QJsonObject& result);
    void passwordChangeResult(bool ok, const QString& message);
    void discordLinkResult(bool ok, const QString& message);
    void emailCodeSent(const QString& address);
    void emailUpdated(const QString& address);
    void emailFailed(const QString& message);
    void replayAccepted(qint64 setId, int chunkSize);
    void replayDeclined(qint64 setId, const QString& reason);
    void replayStored(qint64 setId);
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
    // Emitted whenever libjuice advances a peer's ICE state. `connected` is
    // true only while a validated direct candidate pair is usable; `failed`
    // distinguishes an exhausted ICE attempt from one still in progress.
    void icePeerConnectionChanged(quint64 targetUserId, bool connected, bool failed);
    void icePeerConnectionAttemptChanged(quint64 targetUserId, int attempt, int maxAttempts);
    // The host received a fresh RTT for a path between two other players.
    void roomPingMeasurementsChanged();
    // A burst went unanswered and we're sending another. `attempt` is 1-based
    // and counts the one now in flight. Surfaced in the room so a slow punch
    // reads as progress rather than a hang.
    void pingProbeRetrying(quint64 targetUserId, int attempt, int maxAttempts);
    // Every attempt went unanswered — that peer is unreachable for now.
    void pingProbeFailed(quint64 targetUserId);
    // A series died, but the peer has answered before and this is the first
    // consecutive miss — almost always transient (their socket is lent to a
    // match, or they're mid-rebind after one). The UI should fall back to the
    // last measurement rather than declare unreachable; a second consecutive
    // miss emits pingProbeFailed instead.
    void pingProbeSoftFailed(quint64 targetUserId);

    void matchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers);
    // Late join: a hand-off started (to the match's players and the joiner).
    // peers is the roster after the join, the joiner included.
    void matchAdmitBegin(quint64 matchId, quint64 admitId, int epoch, int seats, quint64 hostId,
                         const LobbyClient::LobbyMatchPeer& joiner,
                         const QList<LobbyClient::LobbyMatchPeer>& peers);
    void matchAdmitStep(quint64 matchId, quint64 admitId, quint64 fromUserId, const QString& step,
                        int frame, bool ok, const QString& reason);
    // A reassembled, decompressed blob step.
    void matchAdmitBlob(quint64 matchId, quint64 admitId, const QString& step, const QByteArray& raw);
    void matchAdmitFailed(quint64 matchId, quint64 userId, const QString& reason);
    void matchPeerLeft(quint64 matchId, quint64 userId, int slot, const QString& reason);

    // continuous: a ranked session is on (it stays on in the match's room).
    // keepTeammate: the 2v2 session keeps its teammate; teammate: the
    // teammate this search goes with (empty = on your own); waitingFor: the
    // wanted teammate who hasn't searched with this player as theirs yet.
    void quickMatchStatus(bool searching, int queueSize, bool ranked, bool continuous,
                          bool keepTeammate, const QString& teammate, const QString& waitingFor);
    // A ranked session waited 2 minutes and only previous opponents are left.
    void quickMatchRematchOffered(const QStringList& opponents);

    // Spectate stream (server → spectator). data carries decoded krec bytes.
    void spectateBegan(quint64 matchId, const QByteArray& manifest);
    // offset is the logical byte position in this spectator's krec stream, or
    // -1 when talking to a legacy server that does not provide offsets.
    void spectateData(quint64 matchId, const QByteArray& data, int liveFrame, qint64 offset);
    // A reassembled savestate keyframe (still compressed) plus the first krec record
    // index to consume after restoring it.
    void spectateKeyframe(quint64 matchId, int frame, const QByteArray& savestate);
    // Write journal chunk seq (contiguous from 0) of a ranked match.
    void spectateJournal(quint64 matchId, int seq, const QByteArray& journal);
    void spectateEnded(quint64 matchId, const QString& reason);
    void spectateFailed(quint64 matchId, const QString& reason);
    // Authoritative audience size, sent to the broadcaster and active spectators.
    void broadcastViewerCount(quint64 matchId, int viewerCount);

    // Moderation (server → client).
    void adminAuthResult(bool ok, const QString& nameOrReason); // ok => moderator granted, name; else reason
    void modNotice(const QString& severity, const QString& text); // system notice ("info"/"warn"/"error")
    void modListReceived(const QJsonArray& bans, const QJsonArray& mutes); // reply to "list"

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessageReceived(const QString& msg);
    void onWsErrorOccurred();
    void onWsBytesWritten(qint64 bytes);
    void onKeyframeCompressionFinished();

    void onUdpReadyRead();
    void onUdpKeepaliveTimer();
    void onHeartbeatTimer();

private:
    void setState(ConnectionState s);
    qint64 sendEnvelope(const QString& type, const QJsonObject& data = {}, const QString& id = {});
    void pumpBroadcastKeyframeUpload();
    void resetBroadcastKeyframeUpload(quint64 matchId = 0);
    void handleEnvelope(const QJsonObject& env);

    // Specific message handlers
    void handleHelloOk(const QJsonObject& data);
    void handleHelloFail(const QJsonObject& data);
    void handleRankedResult(const QJsonObject& data);
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
    void handlePingProbeIncoming(const QJsonObject& data);
    void handleIceSignal(const QJsonObject& data);
    void reconcileIcePeers(const QJsonObject& roomState);
    void removeIcePeer(quint64 userId);
    bool iceAgentInUseByMatch(quint64 userId) const;
    void resetIceMesh();
    void resetIceTransport();
    bool restartIcePeer(quint64 userId, quint32 generation,
                        const QString& reason, bool notifyPeer);
    void flushIceEvents();
    void sendIcePing(quint64 userId, quint64 nonce);
    void sendRoomPingReport(quint64 targetUserId, int rttMs);
    // Start a probe series to `endpoint`. Shared by the reply and incoming
    // paths. No-op if a probe to that peer is already in flight.
    void sendProbeTo(quint64 userId, const QString& endpoint);
    // Emit one burst of identical PROBE packets for an existing series.
    void sendProbeBurst(const QHostAddress& addr, quint16 port, quint64 nonce);
    // A probe series ran out of attempts. Routes to pingProbeFailed or
    // pingProbeSoftFailed based on the peer's consecutive-miss streak.
    void noteProbeSeriesFailed(quint64 userId);
    // Drop every in-flight series without emitting failure signals — for the
    // moments we stop probing for our own reasons (socket lent to a match),
    // where an aged-out series says nothing about the peer.
    void cancelPendingProbes(const QString& reason);

    // Opt-in on-disk trace of the probe and ICE pipelines, for diagnosing peer
    // connections that never resolve. Off unless PingDiagnostics is set;
    // read once at connect time so toggling it mid-session does nothing until
    // the next lobby connect. One file per connection, in RMG-K's Logs folder.
    void startPingDiagnosticLog();
    void stopPingDiagnosticLog(const QString& reason);
    void writePingDiagnostic(const QString& event, const QString& details = QString());
    QString pingUserLabel(quint64 userId) const;
    void handleMatchPeerLeft(const QJsonObject& data);
    void handleMatchAdmitBegin(const QJsonObject& data);
    void handleMatchAdmitSignal(const QJsonObject& data);
    void handleMatchAdmitFail(const QJsonObject& data);
    void handleQuickMatchStatus(const QJsonObject& data);
    void handleSpectateBegin(const QJsonObject& data);
    void handleSpectateData(const QJsonObject& data);
    void handleSpectateKeyframe(const QJsonObject& data);
    void handleSpectateEnd(const QJsonObject& data);
    void handleSpectateFail(const QJsonObject& data);
    void handleBroadcastViewerCount(const QJsonObject& data);
    void handleAdminAuthOk(const QJsonObject& data);
    void handleAdminAuthFail(const QJsonObject& data);
    void handleModNotice(const QJsonObject& data);
    void handleModList(const QJsonObject& data);

    void initiateUdpAnchor();
    // Stop Windows from failing receives with WSAECONNRESET when a peer's port
    // becomes unreachable (see the definition). No-op elsewhere.
    void disableUdpConnReset();
    void sendUdpRegister();
    void sendUdpKeepalive();
    // Walks m_pendingProbes and re-bursts anything that's due. Runs on a short
    // fixed tick rather than a timer per probe.
    void onProbeRetryTimer();

    static LobbyUser parsePresenceUser(const QJsonObject& obj);

    ConnectionState m_state = ConnectionState::Disconnected;

    QWebSocket* m_ws = nullptr;
    QUdpSocket* m_udp = nullptr;

    QTimer* m_heartbeatTimer = nullptr;
    QTimer* m_udpKeepaliveTimer = nullptr;
    QTimer* m_probeRetryTimer = nullptr;
    QTimer* m_iceTimer = nullptr;
    // Retries the pinned anchor port while GekkoNet's teardown still holds it
    // (see initiateUdpAnchor). 0 deadline = no retry window active.
    QTimer* m_anchorRetryTimer = nullptr;
    qint64  m_anchorRetryDeadlineMs = 0;
    bool m_inPrematchSync = false;

    // Exact host manifest from the successful player sync, reused by live replay.
    QByteArray m_prematchManifest;
    int m_matchSeats = 0;
    // Chunks of the blob step being received.
    quint64 m_admitBlobId = 0;
    QString m_admitBlobStep;
    QList<QByteArray> m_admitBlobParts;

    // Incoming spectate keyframe reassembly (chunked SPECTATE_KEYFRAME messages).
    int        m_kfRecvFrame = -1;
    int        m_kfRecvCount = 0;
    int        m_kfRecvGot = 0;
    QByteArray m_kfRecvBuf;
    QList<bool> m_kfRecvChunkSeen;

    // Outbound priority/backpressure state. Ordinary control/chat/replay messages
    // are sent immediately. A keyframe contributes at most one small WebSocket
    // message at a time and only while the socket has no earlier bytes pending.
    QFutureWatcher<QByteArray>* m_kfCompressWatcher = nullptr;
    bool       m_kfCompressing = false;
    quint64    m_kfCompressMatchId = 0;
    int        m_kfCompressFrame = -1;
    int        m_kfCompressRawBytes = 0;
    bool       m_kfUploadActive = false;
    quint64    m_kfUploadMatchId = 0;
    int        m_kfUploadFrame = -1;
    int        m_kfUploadChunk = 0;
    int        m_kfUploadChunkCount = 0;
    QByteArray m_kfUploadData;

    // Pending HELLO context
    QString m_pendingUsername;
    LobbyAuth m_pendingAuth;
    QStringList m_pendingRomHashes;
    QString m_pendingLocalIp;
    QString m_udpAnchorHost;
    quint16 m_udpAnchorPort = 6364;

    // Our own UDP port, pinned on the first successful bind and reused for
    // every rebind afterwards. Peers and the server cache us as ip:port, so a
    // port that changes when a match starts and stops silently invalidates
    // every cached endpoint and every NAT mapping we've opened. n02's p2p core
    // does the same thing (p2p_core.cpp captures get_port() once, then always
    // re-initializes on that port), and releaseUdpAnchor already aborts rather
    // than closes specifically so GekkoNet can retake this exact port.
    // 0 means "not yet bound".
    quint16 m_anchorLocalPort = 0;

    // Session state
    quint64 m_selfUserId = 0;
    QString m_observedIp;
    QString m_region;
    bool    m_isModerator = false; // set once ADMIN_AUTH_OK is received
    qint64  m_broadcastKeyframeIntervalMs = 30'000;
    QString m_selfUsername;
    qint64  m_accountId = 0;
    QString m_sessionToken;
    QString m_lastHelloFailReason;
    QString m_discordLinkTicket;
    QString m_discordLinkName;
    int     m_lastHelloRetryAfter = 0;
    DiscordLink m_discordLink;
    bool    m_emailAvailable = false;
    QString m_accountEmail;
    QList<LadderStanding> m_standings;
    QList<LadderInfo> m_ladders;
    quint64 m_lastMatchRankedId = 0;
    QJsonObject m_lastMatchRanked;
    quint64 m_currentIceRoomId = 0;
    quint64 m_currentIceHostUserId = 0;
    QSet<quint64> m_icePeerIds;
    QHash<quint64, int> m_lastIceStates;
    // Every restart replaces both libjuice agents for this pair. Generations
    // scope trickled signals so candidates delayed from an old WebSocket
    // session cannot be applied to the fresh credentials and UDP socket.
    QHash<quint64, quint32> m_iceGenerations;
    QHash<quint64, int> m_iceRestartAttempts;
    bool m_matchTransportActive = false;
    // Room members waiting beside the running match.
    QSet<quint64> m_iceOutsideMatch;
    bool m_deferredIceMeshReset = false;
    bool m_deferredIceTransportReset = false;
    // Host-only matrix for non-host paths. To avoid duplicate reports, only
    // the lower user id measures/reports each unordered pair.
    QHash<quint64, QHash<quint64, int>> m_roomPeerPings;

    // Ping diagnostics (see startPingDiagnosticLog).
    QFile*  m_pingDiagnosticFile    = nullptr;
    qint64  m_pingDiagnosticStartMs = 0;

    // Datagrams read since the counter was last reset. Only consulted by the
    // match-end drain, to record how much had queued up while nobody read.
    int m_drainedDatagrams = 0;

    // True while GekkoNet borrows the anchor socket (see lendAnchorToMatch):
    // suppresses our reads and probe sends without touching the socket.
    bool m_anchorLent = false;

    // Away detection: last app-wide input, and whether the last heartbeat
    // reported us away (so the first input after can snap us back immediately
    // instead of waiting out the heartbeat interval).
    qint64  m_lastActivityMs = 0;
    bool    m_reportedAway   = false;

    QHash<quint64, LobbyUser> m_users;
    QHash<quint64, LobbyRoomSummary> m_rooms;

    // In-flight direct ICE ping state, keyed by per-request nonce. Each entry maps
    // a sent probe back to the target user and the wall-clock send time so
    // we can compute RTT when PROBE_REPLY comes back.
    struct ProbeInFlight
    {
        quint64 targetUserId = 0;
        qint64  sendMs       = 0;  // first burst — for age/timeout accounting only
        // Second target for the same series: the peer's learned route when it
        // differs from the advertised endpoint. Both are burst each attempt;
        // whichever one works produces the reply.
        QString altEndpoint;
        // Start of the most recent burst, and what RTT is measured from. Using
        // the first burst instead would add the retry delay to the reading and
        // report ~690 ms for a 90 ms peer answered on attempt 3, which then
        // feeds the Auto frame delay. Retries only fire when a burst went
        // unanswered, so a reply belongs to the latest burst unless the true
        // RTT exceeds PROBE_RETRY_INTERVAL_MS — in which case this
        // under-reports, bounded by that interval.
        qint64  attemptSendMs = 0;
        QString endpoint;          // kept so a retry can resend without re-asking the server
        int     attempt       = 1; // 1-based; attempt PROBE_ATTEMPTS is the last
        qint64  nextAttemptMs = 0;
    };
    QHash<quint64, ProbeInFlight> m_pendingProbes;

    // Consecutive exhausted probe series per peer, reset by any successful
    // reply and when a match borrows the socket. A single miss on a peer
    // we've measured before shows as a soft failure (the seat keeps its
    // number); the streak has to reach 2 before the UI says unreachable.
    QHash<quint64, int> m_probeFailStreak;

    // The address a peer's packets *actually* arrive from, learned from inbound
    // PROBE/PROBE_REPLY traffic — n02-style reply-to-observed-source, kept.
    // For a peer whose NAT assigns a different outgoing port per destination
    // (CGNAT/symmetric), the server-advertised endpoint only works for talking
    // to the server; the source their packets reach us from is the only route
    // our own packets can take back. Refreshed on every inbound probe packet;
    // consulted by probes and spliced over the advertised endpoint at match
    // start when fresh.
    struct LearnedRoute
    {
        QString endpoint;
        qint64  lastSeenMs = 0;
    };
    QHash<quint64, LearnedRoute> m_learnedRoutes;
    // Record `sender:port` as userId's proven route (no-op on self/zero id).
    void learnRoute(quint64 userId, const QHostAddress& sender, quint16 senderPort);
    // The learned endpoint if seen recently enough to trust, else empty.
    QString freshLearnedRoute(quint64 userId) const;
    // Nonces we've already measured. A burst draws one echo per packet that
    // arrives, so 9 of 10 replies are duplicates of a nonce we just erased —
    // without this they'd all log as REPLY_UNMATCHED and bury the real ones.
    QSet<quint64> m_recentlyMatchedNonces;

    // Last measured round-trip per peer (userId → ms). Survives between
    // measurements so the UI has a value to render even when the next probe
    // is still in flight.
    QHash<quint64, int> m_measuredPing;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYCLIENT_HPP
