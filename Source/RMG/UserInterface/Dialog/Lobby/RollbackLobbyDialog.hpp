/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef ROLLBACKLOBBYDIALOG_HPP
#define ROLLBACKLOBBYDIALOG_HPP

#ifdef NETPLAY

#include "LobbyClient.hpp"

#include <QDialog>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QByteArray>
#include <QMutex>
#include <QPoint>
#include <QVariant>

#include <RMG-Core/RomSettings.hpp>

class QFrame;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QTextEdit;
class QLineEdit;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QComboBox;
class QCheckBox;

namespace UserInterface
{
namespace Dialog
{

// Top-level rollback lobby UI. Owns a LobbyClient and renders presence /
// rooms / chat. Modeless — opened from the main window menu.
//
// Visual approach matches the Kaillera launcher: palette-based colors,
// bold section-header labels with hairline dividers (no bordered group
// boxes), native widget styling. Sizes standardized across the dialog
// via the constants block in the .cpp.
class RollbackLobbyDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RollbackLobbyDialog(QWidget* parent = nullptr);
    ~RollbackLobbyDialog() override;

    // MainWindow refreshes this on every open so the Create Room dropdown
    // reflects the user's latest ROM library.
    void setRomLibrary(const QMap<QString, CoreRomSettings>& roms);

    // MainWindow calls these from its existing emulation-thread signal slots
    // so the lobby server stays in sync with the actual game lifecycle.
    void notifyEmulationStarted();
    void notifyEmulationFinished();

    // Sends a room-channel chat message — used by the in-game chat overlay so
    // typed messages reach the room while a match is running.
    void sendRoomChat(const QString& message);

    // MainWindow calls this when a spectate playback session ends (emulation
    // stopped) so the server can drop us from the broadcast. Idempotent.
    void stopSpectating();

signals:
    // Fired when the server has issued MATCH_BEGIN. Each entry in remotePeers
    // is pre-formatted as "<slot>,<ip>,<port>" — matches the LOBBY| address
    // peer-entry format consumed by CoreStartEmulation. romFile is the local
    // ROM path resolved by MD5 (not by name) so ROMs absent from the database
    // — where name matching fails — still launch; gameName is for display only.
    void matchReady(QString gameName, QString romFile, QStringList remotePeers,
                    int localPort, int localPlayer,
                    int frameDelay, int predictionWindow);

    // Fired when the user clicks "Close Game" mid-match or when a peer drops.
    void closeMatchRequested();

    // Fired for every *remote* room-channel chat message (own messages are
    // filtered out — the overlay echoes those locally). MainWindow routes this
    // to the in-game chat overlay.
    void roomChatReceived(QString nickname, QString message);

    // Spectating: ask MainWindow to launch a streaming-playback session for a
    // broadcast match, feed it krec bytes as they arrive, and tear it down.
    void spectateLaunch(quint64 matchId, QString gameName);
    void spectateStreamData(QByteArray bytes, int liveFrame, qint64 offset);
    // A savestate keyframe (raw bytes) plus the first krec record index to consume
    // after restoring it, so catch-up is bounded regardless of match length.
    void spectateStreamKeyframe(int frame, QByteArray savestate);
    void spectateStreamClosed(QString reason);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    // Clears the players-list selection when the user clicks empty space in it,
    // so a highlighted name can be dismissed by clicking off it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onClientStateChanged(LobbyClient::ConnectionState s);
    void onHelloFailed(const QString& reason);
    void onConnectError(const QString& msg);

    void onPresenceFull();
    void onUserAdded(quint64 userId);
    void onUserRemoved(quint64 userId);
    void onUserUpdated(quint64 userId);

    void onRoomListChanged();
    void onCreateRoomClicked();
    void onRoomDoubleClicked(QTreeWidgetItem* item, int column);
    void onMatchDoubleClicked(QTreeWidgetItem* item, int column); // spectate a broadcast match
    void onRoomCreateRequested();
    void onRoomCreated(quint64 roomId);
    void onRoomCreateFailed(const QString& reason);
    void onRoomJoinOk(quint64 roomId);
    void onRoomJoinFailed(const QString& reason);
    void onRoomLeft(const QString& reason);
    void onRoomStateChanged(const QJsonObject& roomState);

    void onQuickMatchClicked();
    void onQuickMatchStatusChanged(bool searching, int queueSize);

    void onLeaveRoomClicked();
    void onDropGameClicked();
    void onStartGameClicked();

    void onMatchPeerLeft(quint64 matchId, quint64 userId, int slot, const QString& reason);

    // Broadcaster: drain staged krec bytes to the WebSocket.
    void onBroadcastDrainTick();

    // Spectator: server stream callbacks.
    void onSpectateBegan(quint64 matchId);
    void onSpectateData(quint64 matchId, const QByteArray& bytes, int liveFrame, qint64 offset);
    void onSpectateKeyframe(quint64 matchId, int frame, const QByteArray& savestate);
    void onSpectateEnded(quint64 matchId, const QString& reason);
    void onSpectateFailed(quint64 matchId, const QString& reason);

    void onChatSendClicked();
    void onRoomChatSendClicked();
    void onChatMessageReceived(const LobbyClient::ChatMessage& msg);
    void onMatchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers);

    // Moderation (server → client) responses.
    void onAdminAuthResult(bool ok, const QString& nameOrReason);
    void onModNotice(const QString& severity, const QString& text);
    void onModListReceived(const QJsonArray& bans, const QJsonArray& mutes);

    // Periodic probe driver: while in a room, requests a fresh ping
    // measurement from each seated peer (skipping self). Cadence is set
    // by m_pingProbeTimer's interval.
    void onPingProbeTick();

    // Measure one peer because the user selected a row showing their ping.
    // Lobby rows show the region estimate until this lands; room seats are
    // measured continuously by onPingProbeTick regardless.
    void probeOnDemand(quint64 userId);

    // Punch progress for a seated peer, so a slow or failing NAT punch is
    // visible in the room rather than looking like a stalled Start button.
    void onIcePeerConnectionChanged(quint64 userId, bool connected, bool failed);
    void onIcePeerConnectionAttemptChanged(quint64 userId, int attempt, int maxAttempts);
    void onRoomPingMeasurementsChanged();
    void onPingProbeRetrying(quint64 userId, int attempt, int maxAttempts);
    void onPingProbeFailed(quint64 userId);
    // First consecutive miss on a peer we've measured before — treat as
    // transient: drop any retry text and let the seat fall back to the last
    // measurement instead of flashing unreachable.
    void onPingProbeSoftFailed(quint64 userId);
    // Repaint one seat's right-hand meta cell (ping / retry / unreachable).
    void refreshSeatMeta(quint64 userId, const QString& statusHtml);
    void onPingMeasured(quint64 userId, int rttMs);

private:
    void buildUi();
    QWidget* buildLobbyView();
    QWidget* buildMarquee();
    QWidget* buildBrowseView();
    QWidget* buildInRoomView();
    QWidget* buildChatColumn();
    QWidget* buildPlayersColumn();
    void     applyStylesheet();

    // Repopulate the browse-view ROM picker from m_roms (RMG-K library).
    void     populateBrowseRoms();

    // Resolve the game selected in the (editable) browse picker to its item data
    // {name, md5, file}. Falls back to matching the typed text to an item, so a
    // typed-but-not-committed entry still resolves. Shared by Quick Match and
    // Create Room. Empty map when nothing valid is selected.
    QVariantMap selectedBrowseRom() const;
    void refreshSameGameFilter();

    // Show the disconnected lobby behind a compact modal username prompt.
    // No server connection is attempted until the prompt is accepted.
    void     promptForUsername(const QString& statusMessage = QString());
    QString  prefillUsername() const;

    void refreshPlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u);
    void refreshRoomRow(QTreeWidgetItem* item, const LobbyClient::LobbyRoomSummary& r);
    // Right-click menu on the rooms / matches lists; moderator-only "closeroom".
    void showAdminRoomMenu(QTreeWidget* tree, const QPoint& pos);
    // Ticks the Ongoing Matches "Duration" cells once a second.
    void updateMatchDurations();

    QString stateGlyph(const QString& state) const;
    void    appendChatLine(const QString& channel, const QString& text);
    void    appendChatSystemLine(const QString& channel, const QString& text);

    // Parse and execute a chat slash command (/login, /kick, /mute, /timeout,
    // /ban, /unban, /unmute, /modlist, /modhelp). Returns true if text was a
    // recognized command and was handled (so it must NOT be sent as chat).
    bool     handleSlashCommand(const QString& channel, const QString& text);

    // Flash the taskbar entry + play the system notification sound — called when
    // a new player takes a seat while we're in the room. No-ops the flash if the
    // lobby is already the active window (Qt handles that).
    void    notifyPlayerJoined();
    void    switchToRoomsView();
    void    switchToInRoomView();
    void    enterRoom(quint64 roomId, const QString& greetingChatLine);
    void    updateStatusIndicator(LobbyClient::ConnectionState s);
    // Render the in-room state label as a colored pill (Waiting / Connecting /
    // In Game / failure). colorHex drives both the text and the soft fill.
    void    applyRoomStateBadge(const QString& text, const QString& colorHex);
    void    updateServerMeta();
    void    updateInRoomBanner();   // refresh "you're in: X" banner in browse view

    // Delay is local to this player. Auto resolves from this client's direct
    // RTTs. Prediction is room-wide and may be changed only by the host.
    int     worstLocalPeerPingMs() const;
    void    applyLocalFrameDelay(bool force);
    void    applyRoomPrediction(bool force);

    // Broadcaster lifecycle. startBroadcast arms the n02 recording sink + drain
    // timer and sends BROADCAST_BEGIN; stopBroadcast flushes and sends
    // BROADCAST_END. feedBroadcastBytes is the sink target (emulation thread).
    void startBroadcast(quint64 matchId);
    void stopBroadcast();
    void feedBroadcastBytes(const void* data, int len);

    // Begin spectating a broadcast match: tell the server and ask MainWindow to
    // launch a streaming-playback session.
    void beginSpectate(quint64 matchId, const QString& gameName);

    // Seat row API — 4 fixed slots rendered as a vertical player list.
    // Empty rows show a ○ dot + "Waiting…"; filled rows show ● + name + meta.
    // Only rows 1..maxPlayers are visible (others hidden via setVisible).
    struct SeatRow
    {
        QWidget* row       = nullptr;
        QLabel*  dragHandle = nullptr;    // ⋮⋮ — host-only drag grip (reorder)
        QLabel*  dotLabel  = nullptr;     // ● filled, ○ empty
        QLabel*  slotLabel = nullptr;     // "P1"
        QLabel*  nameLabel = nullptr;     // username or "Waiting…"
        QLabel*  metaLabel = nullptr;     // "host · Frame delay: 2f · 12ms"
        QPushButton* kickButton = nullptr; // ✕ — host-only, removes the seated player
        bool     isHost    = false;
        quint64  userId    = 0;           // seated user, 0 when empty
        int      slot      = 0;           // 1-4, drives the per-player accent color
        int      frameDelay = -1;          // published local input delay
        int      iceAttempt = 1;            // current ICE generation, one-based
        int      iceMaxAttempts = 20;
    };
    void buildSeatRow(SeatRow& row, int slotIdx, QWidget* parent);
    void renderSeatEmpty(SeatRow& row);
    void renderSeatFilled(SeatRow& row, const QString& username, bool isHost,
                          bool isSelf, int pingMs, int frameDelay, bool canKick);

    // Seat reorder (host, waiting): a seat's drag handle starts a QDrag carrying
    // its slot; the seats container handles the drop and asks the server to swap.
    // Seats may be left sparse on purpose (P1 + P3 with P2 empty) — the core
    // sizes the session by the highest occupied seat, so a swap into an empty
    // seat is a supported move, not something to guard against.
    void startSeatDrag(int slot, QWidget* card);
    int  seatSlotAtPos(const QPoint& pos) const;

    // Hand-rolled column fill shared by the players, rooms, and matches
    // trees: the column right of a dragged divider absorbs the change (so
    // every column is resizable, the last one via the divider on its left),
    // column 0 absorbs viewport resizes, minimum widths keep squeezed
    // dividers grabbable, and the header always spans the viewport exactly
    // so a horizontal scrollbar is never needed. Called on sectionResized
    // (pass the index) and viewport resizes (pass -1). Re-entrant-safe via
    // m_clampingTreeColumns.
    void clampTreeColumns(QTreeWidget* tree, int resizedIndex);

    // Returns the local ROM path whose MD5 matches `md5` (case-insensitive), or
    // empty if the user doesn't have that ROM. Gates joining a room and resolves
    // the ROM at match start so both use identical matching.
    QString localRomPathForMd5(const QString& md5) const;

    // Cleanly abort a match that failed before emulation started (ROM missing or
    // pre-match sync failed/timed out): reset await state, tell the server the
    // match is over so the room returns to "waiting", reopen the ping anchor,
    // and surface `reason` in room chat. Prevents the dialog from getting stuck
    // on "Connecting…" with a half-started match.
    void abortMatchStart(const QString& reason);

    // Enable the host's Start button only once the room is startable: waiting,
    // 2+ players seated, AND a ping measured for every seated peer. Re-run from
    // both onRoomStateChanged (seats change) and onPingMeasured (ping lands),
    // since pings arrive asynchronously after seats populate.
    void refreshStartButton();

    LobbyClient* m_client = nullptr;

    bool    m_connectPromptOpen = false;
    QString m_connectPromptMessage;

    // ── Marquee bar ──
    QFrame*  m_marquee     = nullptr;
    QLabel*  m_brandLabel  = nullptr;
    QFrame*  m_statusPill  = nullptr;   // rounded pill holding the LED + text
    QLabel*  m_statusLed   = nullptr;
    QLabel*  m_statusText  = nullptr;
    QLabel*  m_serverMeta  = nullptr;
    QLabel*  m_userLabel   = nullptr;

    // ── Main panels ──
    QSplitter*   m_splitter      = nullptr;
    QSplitter*   m_browseSplitter = nullptr; // Active Rooms / Ongoing Matches divider
    QTreeWidget* m_playersTree   = nullptr;
    // Last on-demand probe per user id, for probeOnDemand's throttle.
    QHash<quint64, qint64> m_lastOnDemandProbe;
    // Peers we've already posted an "couldn't reach" notice about, so the 3s
    // seat refresh doesn't repeat it every tick. Cleared on a measurement.
    QSet<quint64> m_probeFailureAnnounced;
    QTreeWidget* m_roomsTree     = nullptr;
    QTreeWidget* m_matchesTree   = nullptr;
    QTimer*      m_matchDurationTimer = nullptr;
    QTextEdit*   m_chatViewLobby = nullptr;
    QTextEdit*   m_chatViewRoom  = nullptr; // room chat, shown in the in-room view
    QLineEdit*   m_chatInput     = nullptr; // lobby chat input (middle column)
    QLineEdit*   m_roomChatInput = nullptr; // room chat input (below the seats)

    // ── Stacked browse / in-room view ──
    QStackedWidget* m_roomsStack = nullptr;

    // ── Persistent "you're in a room" banner shown in browse view when
    //    the user is in a room (so they can flip back without losing their seat). ──
    QFrame*    m_inRoomBanner    = nullptr;
    QLabel*    m_bannerText      = nullptr;
    QPushButton* m_bannerReturn  = nullptr;

    // In-room header card
    QLabel*    m_roomTitle      = nullptr;   // ROM title (large)
    QLabel*    m_roomSubtitle   = nullptr;   // host · region · max
    QLabel*    m_roomStateLabel = nullptr;   // "Waiting" / "In Game"
    QLabel*    m_roomMetaLabel  = nullptr;   // Seats 2/4 · Region NTSC

    // Delay is locally editable for every player; prediction is room-wide and
    // host-editable. Both lock once the match starts. Delay uses data -1 for
    // Auto; prediction uses data 0 for Default.
    QComboBox* m_delayCombo      = nullptr;
    QComboBox* m_predictionCombo = nullptr;
    bool       m_suppressSettingsSignal = false;  // guard against ROOM_STATE → setCurrentIndex echo

    // Per-player local toggle: when checked, this client writes a .krec of the
    // match (the player who checks it records, same as the p2p/kaillera lobbies).
    // Drives the shared n02_kaillera_recording_enabled flag; not synced to the room.
    QCheckBox* m_recordCheck     = nullptr;

    // When checked, this client also streams the match's .krec up to the server
    // so others can spectate. Broadcasting implies recording (the stream is the
    // krec bytes). Only one player per match becomes the broadcaster (server
    // picks the first). The krec is written on the emulation thread, so bytes
    // are staged into m_broadcastBuf (under m_broadcastMutex) by the n02 sink
    // and drained to the WebSocket by m_broadcastDrainTimer on the UI thread.
    QCheckBox* m_broadcastCheck       = nullptr;
    QTimer*    m_broadcastDrainTimer  = nullptr;
    QMutex     m_broadcastMutex;
    QByteArray m_broadcastBuf;
    bool       m_broadcasting   = false;
    quint64    m_broadcastMatchId = 0;
    // Wall-clock of the last savestate-keyframe request (0 = request one now). The
    // drain tick requests a keyframe at a fixed interval and polls for the result.
    qint64     m_lastKeyframeRequestMs = 0;

    // Non-zero while this client is spectating a broadcast match (the match id
    // we asked the server to stream). Cleared when the spectate session ends.
    quint64    m_spectatingMatchId = 0;
    // Session boundary guard. The persistent lobby socket can still deliver krec
    // chunks from a PREVIOUS watch of the same match after we re-subscribe (same
    // matchId, so they'd otherwise pass the filter and corrupt the new replay).
    // The server always sends SPECTATE_BEGIN first on a fresh subscribe, after all
    // the stale data on the wire — so we drop every keyframe/data message until we
    // see the BEGIN for the current subscribe. Reset false in beginSpectate.
    bool       m_spectateStreamArmed = false;

    // Guards clampTreeColumns against re-entering itself: the setColumnWidth
    // calls it makes emit sectionResized, which is what invokes it. One flag
    // covers all three trees: the corrections are synchronous, so they never
    // interleave.
    bool       m_clampingTreeColumns = false;

    // Seat rows (always 4 — slots beyond maxPlayers are hidden)
    SeatRow    m_seats[4];
    QWidget*   m_seatsBox        = nullptr; // container that accepts seat drops
    bool       m_canReorderSeats = false;   // host && room waiting
    QPoint     m_seatDragStartPos;          // press point, to clear the drag threshold
    int        m_seatDragSlot    = 0;       // slot being dragged (0 = none)

    // Drives PING_PROBE_REQUEST → UDP PROBE → PROBE_REPLY refresh cycle for
    // each peer seated in the current room. Started when a room is entered,
    // stopped when it's left. Inactive (and unused) outside rooms.
    QTimer*    m_pingProbeTimer = nullptr;

    // In-room action bar
    QPushButton* m_startBtn      = nullptr;
    QPushButton* m_dropBtn       = nullptr;
    QPushButton* m_leaveBtn      = nullptr;

    // Hero / browse-view action buttons
    QPushButton* m_quickMatchBtn = nullptr;   // primary CTA (blue)
    QPushButton* m_createRoomBtn = nullptr;
    QComboBox*   m_browseRomCombo = nullptr;   // library game picker (feeds Create Room)
    QCheckBox*   m_sameGameFilterCheck = nullptr;

    QHash<quint64, QTreeWidgetItem*> m_userItems;
    QHash<quint64, QTreeWidgetItem*> m_roomItems;

    class CreateRoomDialog* m_createRoomDialog = nullptr;

    QString  m_username;
    QString  m_lastRoomName;
    QString  m_serverUrl;
    quint64  m_currentRoomId = 0;

    QString m_currentRoomGame;
    QString m_currentRoomMd5;
    QString m_currentRoomRegion;
    QString m_currentRoomState;
    int     m_currentRoomDelay      = 2;   // this client's local input delay
    int     m_currentRoomPrediction = 7;
    int     m_currentRoomPacing     = 0;   // 0 = aggressive, 1 = smooth
    quint64 m_currentRoomHostId     = 0;   // seated host's user id (0 when none)
    quint64 m_currentMatchId        = 0;
    quint64 m_iceWaitingMatchId     = 0;
    qint64  m_iceMatchDeadlineMs    = 0;

    // Seated user ids as of the last ROOM_STATE, used to detect when a *new*
    // player joins (so we can flash/chime). m_roomSeatsSeen suppresses the chime
    // on the first ROOM_STATE after entering a room (those seats aren't "joins").
    QSet<quint64> m_knownSeatedUsers;
    bool          m_roomSeatsSeen = false;

    // Delay Auto is local. Prediction Auto is the host-owned room selection;
    // its Default entry resolves to 7.
    bool    m_delayAuto = true;
    bool    m_predictionAuto = true;
    bool    m_localDelayPublished = false;

    bool m_awaitingEmulationStart = false;
    bool m_emulationActive        = false;
    bool m_quickMatchActive       = false;

    QMap<QString, CoreRomSettings> m_roms;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // ROLLBACKLOBBYDIALOG_HPP
