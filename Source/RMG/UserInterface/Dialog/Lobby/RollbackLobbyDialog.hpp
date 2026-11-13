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
#include "LobbyTheme.hpp"
#include "LagStats.hpp"
#include "NetplaySaveDialogs.hpp"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QByteArray>
#include <QMutex>
#include <QPoint>
#include <QVariant>

#include <RMG-Core/RomSettings.hpp>
#include <RMG-Core/rmgk_gekko.hpp> // LobbyRemotePeer (late join rosters)

#include <vector>

class QFrame;
class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QTextEdit;
class QLineEdit;
class QVBoxLayout;
class QGridLayout;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QComboBox;
class QButtonGroup;

namespace UserInterface
{
namespace Dialog
{

class CartridgeTile;
class ChallengeCard;
class ChatTabBar;
class ChatView;
class CountryFlag;
class DropPanel;
class ElidedLabel;
class FrameStepper;
class GameBadge;
class IconBadge;
class IconButton;
class IconLabel;
class LedDot;
class LiveGallery;
class LobbyBand;
class LobbyButton;
class LobbyCard;
class MentionPopup;
class ModerationDialog;
class PortCard;
class PrivateMessagesPanel;
class RoomsColumn;
class RoomWindow;
class Spinner;
class StageScrim;
class StatePill;
class StatusTag;
class ToggleSwitch;
class UndoToast;
class UserAvatar;

// Top-level rollback lobby UI. Owns a LobbyClient and renders presence /
// rooms / chat. Modeless — opened from the main window menu.
//
// Visual approach: its own charcoal or cream theme, picked from the application
// palette (see LobbyTheme.hpp). Running matches share the window with the open
// rooms, and a room opens as a window of its own over them.
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

    // Sends a chat line to a live replay's chat: its viewers and, on a server that
    // shares it, the match's players. No-op unless watching, or playing a match
    // being broadcast.
    void sendSpectatorChat(const QString& message);

    // MainWindow calls this when a spectate playback session ends (emulation
    // stopped) so the server can drop us from the broadcast. Idempotent.
    void stopSpectating();

    // MainWindow calls this once a spectate session is fully torn down, so a
    // switch to another match can start its new session.
    void notifySpectateSessionClosed();

    // Lag stats for the running rollback match (or the last one), built from
    // our own numbers and the other players' reports. /lagstat in the in-game
    // chat shows this on the overlay and adds it to the room chat.
    LagReport lagReport(bool wholeMatch) const;
    // Adds the report to a chat for this player only (the room chat by default).
    void appendLagReport(const LagReport& report, const QString& channel = QString());
    void showLagStats();

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

    // Non-modal in-game warning: lobby-backed services (including chat) are
    // offline, but the established peer-to-peer match transport is intact.
    void matchServerConnectionLost(QString message);

    // Fired for every *remote* room-channel chat message (own messages are
    // filtered out — the overlay echoes those locally). MainWindow routes this
    // to the in-game chat overlay.
    void roomChatReceived(QString nickname, QString message);

    // Chat from viewers of the live replay being watched (own lines included —
    // the server echoes them). MainWindow routes this to the in-game overlay.
    void spectatorChatReceived(QString nickname, QString message);

    // Spectating: ask MainWindow to launch a streaming-playback session for a
    // broadcast match, feed it krec bytes as they arrive, and tear it down.
    // romPath is the local ROM matching the match's MD5 (empty when unknown).
    void spectateLaunch(quint64 matchId, QString gameName, QString romPath);
    void spectateStreamData(QByteArray bytes, int liveFrame, qint64 offset);
    // A savestate keyframe (raw bytes) plus the first krec record index to consume
    // after restoring it, so catch-up is bounded regardless of match length.
    void spectateStreamKeyframe(int frame, QByteArray savestate);
    void spectateStreamClosed(QString reason);
    // Persistent OSD audience badge. isSpectator selects the role-specific text.
    void liveReplayViewerCountChanged(int viewerCount, bool isSpectator);
    void liveReplayViewerCountCleared();

    // Late join notices for the running game: a line in the OSD message
    // stack, the centered banner shown while the game holds for a joiner
    // (empty hides it), and a refresh of the port labels from
    // recording_player_names for a match emulating `seats` ports.
    void overlayMessage(QString message);
    void overlayCenterMessage(QString message);
    void portLabelsChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    // Escape minimizes an open room before it closes the lobby.
    void keyPressEvent(QKeyEvent* event) override;
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
    void onRoomJoinRequested(quint64 roomId);
    void onMatchWatchRequested(quint64 roomId); // spectate a broadcast match
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
    void onMatchAdmitBegin(quint64 matchId, quint64 admitId, int epoch, int seats, quint64 hostId,
                           const LobbyClient::LobbyMatchPeer& joiner,
                           const QList<LobbyClient::LobbyMatchPeer>& joiners,
                           const QList<LobbyClient::LobbyMatchPeer>& peers);
    void onMatchAdmitStep(quint64 matchId, quint64 admitId, quint64 fromUserId, const QString& step,
                          int frame, bool ok, const QString& reason);
    void onMatchAdmitBlob(quint64 matchId, quint64 admitId, const QString& step, const QByteArray& raw);
    void onMatchAdmitFailed(quint64 matchId, quint64 userId, const QString& reason);
    void onLateJoinTick();

    // Broadcaster: drain staged krec bytes to the WebSocket.
    void onBroadcastDrainTick();

    // Spectator: server stream callbacks.
    void onSpectateBegan(quint64 matchId, const QByteArray& manifest);
    void onSpectateData(quint64 matchId, const QByteArray& bytes, int liveFrame, qint64 offset);
    void onSpectateKeyframe(quint64 matchId, int frame, const QByteArray& savestate);
    void onSpectateEnded(quint64 matchId, const QString& reason);
    void onSpectateFailed(quint64 matchId, const QString& reason);
    void onBroadcastViewerCount(quint64 matchId, int viewerCount);

    void onChatSendClicked();
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
    // Put one seat's Link stat back to its ping or connection attempt, or show
    // a status in it (retrying, unreachable).
    void refreshSeatLink(quint64 userId);
    void setSeatLinkStatus(quint64 userId, const QString& text, LobbyTheme::Role role);
    void onPingMeasured(quint64 userId, int rttMs);

private:
    void buildUi();
    QWidget* buildLobbyView();
    QWidget* buildMarquee();
    QWidget* buildPlayBar();
    QWidget* buildBrowseView();
    QWidget* buildInRoomView();
    QWidget* buildSidebar();
    QWidget* buildChatColumn();
    QWidget* buildPlayersColumn();
    void     applyStylesheet();
    // After the lobby's colours change: restyles and repaints what holds them.
    void     refreshTheme();
    void     setLobbyTheme(LobbyTheme::ThemeChoice choice);
    void     setBrowseStacked(bool stacked);
    void     restoreBrowseSplit();
    QString  browseSplitterKey() const;

    void     showUserMenu();
    void     updateUserChip();
    // Asks for a new name while staying connected. Only an accepted name
    // reconnects; if the lobby then refuses it, cancelling the prompt that
    // follows goes back to the old one instead of closing the lobby.
    void     changeUsername();
    void     connectAs(const QString& username, const QString& serverUrl);

    // Repopulate the browse-view ROM picker from m_roms (RMG-K library).
    void     populateBrowseRoms();

    // Resolve the game selected in the (editable) browse picker to its item data
    // {name, md5, file}. Falls back to matching the typed text to an item, so a
    // typed-but-not-committed entry still resolves. Shared by Quick Match and
    // Create Room. Empty map when nothing valid is selected.
    QVariantMap selectedBrowseRom() const;
    void refreshSameGameFilter();
    void refreshPickerInfo();
    // The local cover art for a game in the library, or empty. Cached per MD5.
    QString coverPathForMd5(const QString& md5);

    // Show the disconnected lobby behind a compact modal username prompt.
    // No server connection is attempted until the prompt is accepted.
    void     promptForUsername(const QString& statusMessage = QString());
    QString  prefillUsername() const;
    bool     matchTransportInProgress() const;
    void     showMatchConnectionLostNotice();

    void refreshPlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u);
    void clearPlayerRows();
    QTreeWidgetItem* playerGroupForState(const QString& state) const;
    void placePlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u);
    void refreshPlayerGroups();
    // The players list keeps its state headings; sorting orders the players
    // under each one, and the filters hide rows like the name search does.
    enum class PlayerSort { Name, Ping, Region };
    bool playerRowBefore(const QTreeWidgetItem* a, const QTreeWidgetItem* b) const;
    void resortPlayers();
    void showPlayersFilterMenu();
    bool playersFiltered() const;
    // Right-click menu on rooms and matches; moderator-only "closeroom".
    void showAdminRoomMenu(quint64 roomId, const QPoint& globalPos);

    // ── Social (RollbackLobbySocial.cpp) ──
    // The menu on any player's name, everywhere a name appears.
    void showPlayerMenu(quint64 userId, const QPoint& globalPos);
    void showPlayerMenuForName(const QString& name, const QPoint& globalPos);
    quint64 userIdForName(const QString& name) const;
    void connectChatView(ChatView* view);
    void connectSocialSignals();
    void resetSocialState();
    bool socialEventFilter(QObject* watched, QEvent* event);
    void refreshChatTabIcons();
    // Anything that plays a sound or flashes the taskbar goes through this, so
    // Do not disturb and the per-sender limit apply to all of it.
    void alertFrom(quint64 userId);

    // Ignoring is per name and per PC; the server isn't involved.
    bool isIgnored(const QString& name) const;
    void setIgnored(const QString& name, bool ignored);
    void showIgnoredPlayers();

    // Challenges
    void startChallenge(quint64 userId);
    void onChallengeSent(const LobbyClient::Challenge& challenge);
    void onChallengeIncoming(const LobbyClient::Challenge& challenge);
    void onChallengeEnded(quint64 challengeId, quint64 toUserId, const QString& result);
    void showNextChallenge();
    void answerChallenge(bool accept);
    void expireChallengeLater(quint64 challengeId, qint64 expiresAtMs);
    void setChallengeOutcome(const QString& title, const QString& detail, const QColor& color, const QString& icon);
    void stopWatchingForRoom();
    void positionStageOverlays();

    // Private messages
    void openPrivateMessage(quint64 userId);
    void onPrivateMessage(const LobbyClient::ChatMessage& msg);
    void onPrivateMessageFailed(quint64 toUserId, const QString& reason);
    void refreshPrivateMessageStatus(quint64 userId);
    void sendCurrentPrivateMessage(const QString& text);

    // Mentions
    // A message as chat HTML: escaped, with @names as links. mentionsMe says
    // whether it names someone on this PC.
    QString chatBodyHtml(const QString& message, bool* mentionsMe) const;
    QString chatMentionLineHtml(const QString& line) const;
    void    noteMention(const QString& channel, const LobbyClient::ChatMessage& msg);
    void    startMention(const QString& name);
    void    refreshMentionPopup();
    void    insertMention(const QString& name);
    void    refreshMentionJump();
    void    positionChatOverlays();
    // An icon inside a chat line; registers it with every chat view's document.
    QString chatIconHtml(const QString& icon, const QColor& color, int px) const;

    // Moderation
    void openSanctionDialog(bool ban, const QString& name);
    void showModerationDialog();
    void onModResult(const QString& action, const QString& target, const QString& ip, qint64 untilMs);
    // Ticks the running durations once a second.
    void updateMatchDurations();

    QString stateGlyph(const QString& state) const;
    void    appendChatLine(const QString& channel, const QString& text);
    void    appendChatSystemLine(const QString& channel, const QString& text,
                                 const QString& icon = QString(),
                                 LobbyTheme::Role role = LobbyTheme::Role::Muted);
    // One line of chat. bodyHtml is from chatBodyHtml. mine marks this PC's own
    // lines in a private conversation.
    QString chatMessageHtml(const QString& time, quint64 userId, const QString& who, const QString& bodyHtml,
                            bool fromAdmin, bool mine = false, bool fromPlayer = false) const;
    void    refreshChatInput();
    void    setChatUnread(int tab, int count);
    void    setRoomChatVisible(bool visible);

    // Parse and execute a chat slash command (/login, /kick, /mute, /timeout,
    // /ban, /unban, /unmute, /modlist, /modhelp). Returns true if text was a
    // recognized command and was handled (so it must NOT be sent as chat).
    bool     handleSlashCommand(const QString& channel, const QString& text);

    // Flash the taskbar entry + play the system notification sound — called when
    // a new player takes a seat while we're in the room. No-ops the flash if the
    // lobby is already the active window (Qt handles that).
    void    notifyPlayerJoined();
    // Minimizes the room window (you keep your seat), or opens it.
    void    switchToRoomsView();
    void    switchToInRoomView();
    bool    roomWindowOpen() const;
    // Lays out the room window, its scrim and the overlays over the stage.
    void    layoutStage();
    void    refreshRoomTitleTip();
    // The room roster and metadata are server-owned and become invalid as soon
    // as the WebSocket drops. Clear them without touching an active match's
    // ICE ownership or lifecycle bookkeeping.
    void    clearServerRoomSnapshot();
    void    enterRoom(quint64 roomId, const QString& greetingChatLine);
    void    updateStatusIndicator(LobbyClient::ConnectionState s);
    // Render the in-room state as a colored pill (Waiting / Connecting /
    // In Game / failure). A running match's pill also shows its duration.
    void    applyRoomStateBadge(const QString& text, const QString& colorHex);
    void    refreshRoomBadge();
    void    updateServerMeta();
    // The play bar's state: choosing a game, searching, watching, or in a room.
    void    refreshPlayBar();
    void    onPlayBarActionClicked();

    // Delay is local to this player. Auto resolves from this client's direct
    // RTTs. Prediction is room-wide and may be changed only by the host.
    // Both stay editable while this client plays the match; the running
    // session follows the live values handed to rmgk_gekko.
    int     worstLocalPeerPingMs() const;
    void    applyLocalFrameDelay(bool force);
    void    applyRoomPrediction(bool force);
    bool    inLiveMatch() const;
    void    updateRollbackSettingControls();
    void    refreshSettingHints();
    void    endLiveRollbackSettings();

    // Broadcaster lifecycle. startBroadcast arms the n02 recording sink + drain
    // timer and sends BROADCAST_BEGIN; stopBroadcast flushes and sends
    // BROADCAST_END. feedBroadcastBytes is the sink target (emulation thread).
    void startBroadcast(quint64 matchId);
    void stopBroadcast();
    void feedBroadcastBytes(const void* data, int len);

    // Begin spectating a broadcast match: tell the server and ask MainWindow to
    // launch a streaming-playback session with the ROM matching romMd5.
    void beginSpectate(quint64 matchId, const QString& gameName, const QString& romMd5);

    // Shows the Spectators chat tab (and selects it) while watching a live
    // replay; hiding it returns the column to lobby chat.
    void setSpectatorChatVisible(bool visible);
    // Opens and closes the spectators' chat for this client as a player.
    void refreshPlayerSpectatorChat();
    void refreshSpectatorTab();
    void showSpectatorCommentOverGame(const LobbyClient::ChatMessage& msg);

    // Seat API — 4 fixed slots rendered as port cards. Only cards 1..maxPlayers
    // are visible (others hidden via setVisible).
    struct SeatRow
    {
        PortCard* row      = nullptr;
        bool     isHost    = false;
        bool     isSelf    = false;
        bool     canKick   = false;
        QString  username;
        quint64  userId    = 0;           // seated user, 0 when empty
        quint64  peerId    = 0;           // the connection they play through, 0 when empty
        int      slot      = 0;           // 1-4, drives the per-player accent color
        int      frameDelay = -1;          // published local input delay
        int      iceAttempt = 1;            // current ICE generation, one-based
        int      iceMaxAttempts = 20;
    };
    void buildSeatRow(SeatRow& row, int slotIdx, QWidget* parent);
    void renderSeatEmpty(SeatRow& row);
    void renderSeatFilled(SeatRow& row, const QString& username, bool isHost,
                          bool isSelf, int pingMs, int frameDelay, bool canKick,
                          const QString& connectionType);

    // Seat reorder (host, waiting): dragging a seat's handle lifts a picture of
    // its card that follows the pointer from where it was grabbed. Letting go
    // over another seat asks the server to swap the two; Escape cancels.
    // It's drawn inside the room window rather than as a system drag, whose
    // image rides the cursor and lands wherever the platform puts it.
    // Seats may be left sparse on purpose (P1 + P3 with P2 empty) — the core
    // sizes the session by the highest occupied seat, so a swap into an empty
    // seat is a supported move, not something to guard against.
    void beginSeatDrag(int slot, PortCard* card, const QPoint& globalPos);
    void moveSeatDrag(const QPoint& globalPos);
    // drop: swap with the seat under globalPos; otherwise just put it back.
    void endSeatDrag(const QPoint& globalPos, bool drop);
    int  seatSlotAtPos(const QPoint& pos) const;

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
    void updateRecordingControls();

    // Join limits (host-only room option): the highest ping to the host a
    // joiner may have, and whether a PC on Wi-Fi may join. Hidden on servers
    // without it.
    void updateJoinLimitControls();
    void updateRoomSizeControls();

    // Save data (host-only room option): the match starts from a save the
    // host picked, the game saves to memory, and any player can keep a copy.
    void updateSaveDataControls();
    // Host: pick the save the next match starts from; false when cancelled.
    bool pickStartingSave();
    // Host: tell the room which save that is.
    void publishHostSaveChoice();
    // The match's save is the starting save until the game changes it.
    void beginMatchSave();
    void onSaveDataTick();
    // Match over: take the last agreed save and offer to keep it.
    void finishMatchSave();
    void saveMatchSaveAs();
    bool writeMatchSave(const QString& file, QString* error = nullptr);

    // Game options (host-only room option): ROM patches from the packs in
    // Data/GameOptions, one switch each in the Session column. Hidden on
    // servers without them and in quick match.
    void updateGameOptionControls();
    void onGameOptionToggled(const QString& id, bool checked);
    // An option's name in this client's language when its own pack for the
    // game knows the id, else the host's label. Tips add the pack's description.
    QStringList gameOptionLabels(const QString& romMd5, const QList<LobbyClient::LobbyGameOption>& options) const;
    QString gameOptionsTip(const QString& romMd5, const QList<LobbyClient::LobbyGameOption>& options) const;
    // The chat line naming the game options the match just synced plays with.
    void announceMatchGameOptions(const QString& channel, const QString& romMd5);

    LobbyClient* m_client = nullptr;

    bool    m_connectPromptOpen = false;
    QString m_connectPromptMessage;
    bool    m_matchConnectionLostNoticeShown = false;

    // ── Top bar ──
    LobbyBand* m_marquee   = nullptr;
    QLabel*  m_brandLabel  = nullptr;
    LedDot*  m_statusLed   = nullptr;
    QLabel*  m_statusText  = nullptr;
    QLabel*  m_serverMeta  = nullptr;
    QPushButton* m_userChip = nullptr;
    UserAvatar*  m_userAvatar = nullptr;
    QLabel*      m_userName = nullptr;
    CountryFlag* m_userFlag = nullptr;
    IconLabel*   m_userRoleIcon = nullptr;
    QLabel*      m_userRole = nullptr;

    // ── Main panels ──
    QSplitter*   m_splitter      = nullptr;
    QSplitter*   m_browseSplitter = nullptr; // Live gallery / open rooms divider
    QSplitter*   m_sideSplitter  = nullptr;  // players / chat divider
    QTreeWidget* m_playersTree   = nullptr;
    QLabel*      m_playersCount  = nullptr;
    QLineEdit*   m_playersSearch = nullptr;
    IconButton*  m_playersFilterButton = nullptr;
    // Kept between sessions. An empty region shows every region; 0 every ping.
    PlayerSort   m_playerSort = PlayerSort::Name;
    QString      m_playerRegionFilter;
    bool         m_playerHideWifi = false;
    int          m_playerMaxPing = 0;
    // This client's own row, fixed above the scrolling list, then the other
    // players on this PC. Each plays on their own controller; see
    // LobbyClient::LocalPlayer.
    QFrame* m_selfRowBox = nullptr;
    QTreeWidget* m_selfRow = nullptr;
    void buildLocalRoster();
    void refreshLocalRoster();
    void openRosterEditor();
    void closeRosterEditor();
    void submitRosterEditor();
    void onLocalPlayerFailed(const QString& name, const QString& reason);
    void onQuickMatchRefused(const QString& reason);
    void fillControllerCombo(QComboBox* combo, int current, quint64 exceptUserId) const;
    int nextFreeController() const;
    QString nextRememberedLocalName() const;
    void warnAboutLocalControllers();
    QWidget*     m_rosterBox      = nullptr;
    QVBoxLayout* m_rosterRows     = nullptr;
    QWidget*     m_rosterEditor   = nullptr;
    QLineEdit*   m_rosterName     = nullptr;
    QComboBox*   m_rosterController = nullptr;
    QLabel*      m_rosterError    = nullptr;
    LobbyButton* m_rosterAdd      = nullptr;
    QString      m_rosterSubmittedName; // sent to the server, not answered yet
    int          m_rosterSeatsShown = 1; // the room list was last drawn for this many
    QList<QTreeWidgetItem*> m_playerGroups; // one heading per state
    // Last on-demand probe per user id, for probeOnDemand's throttle.
    QHash<quint64, qint64> m_lastOnDemandProbe;
    // Peers we've already posted an "couldn't reach" notice about, so the 3s
    // seat refresh doesn't repeat it every tick. Cleared on a measurement.
    QSet<quint64> m_probeFailureAnnounced;
    LiveGallery* m_liveGallery   = nullptr;
    RoomsColumn* m_roomsColumn   = nullptr;
    QTimer*      m_matchDurationTimer = nullptr;
    ChatView*    m_chatViewLobby = nullptr;
    ChatView*    m_chatViewRoom  = nullptr; // room chat, the Room tab
    ChatView*    m_chatViewSpectate = nullptr; // spectators' chat, watching or playing
    // Above that chat for a player: how many are watching, and the overlay switch.
    QWidget*     m_spectatorStrip = nullptr;
    IconLabel*   m_spectatorEye   = nullptr;
    QLabel*      m_spectatorCount = nullptr;
    ToggleSwitch* m_spectatorOverlayToggle = nullptr;
    ChatTabBar*  m_chatTabs      = nullptr; // Lobby | Room | Spectators | mail
    QStackedWidget* m_chatStack  = nullptr; // one view per tab
    QLineEdit*   m_chatInput     = nullptr; // sends to the selected tab
    QWidget*     m_chatCard      = nullptr;
    StatusTag*   m_composeAs     = nullptr; // who on this PC a private reply is from
    // Room only shows while seated, Spectators only while watching, mail only on
    // a server that relays private messages.
    enum ChatTab { kChatTabLobby, kChatTabRoom, kChatTabSpectators, kChatTabMail, kChatTabCount };
    int          m_chatUnread[kChatTabCount] = {};
    bool         m_chatMentioned[kChatTabCount] = {}; // an unseen line in that tab names this PC
    // Lines naming this PC below where the reader has scrolled, as block numbers.
    QList<int>   m_pendingMentions[kChatTabCount];
    MentionPopup* m_mentionPopup = nullptr;
    QPushButton* m_mentionJump   = nullptr;
    PrivateMessagesPanel* m_pmPanel = nullptr;
    quint64      m_lastPmLocal   = 0; // who /r answers: this PC's side...
    quint64      m_lastPmRemote  = 0; // ...and theirs

    QStringList  m_ignored;
    bool         m_doNotDisturb = false;
    QHash<quint64, qint64> m_lastAlertMs; // per sender

    // ── Challenges ──
    ChallengeCard* m_challengeCard = nullptr;
    QList<LobbyClient::Challenge> m_incomingChallenges; // the first is on the card
    LobbyClient::Challenge m_outgoingChallenge;         // id 0 when there's none
    QPointer<QDialog> m_challengeDialog;
    QPointer<QDialog> m_sanctionDialog;
    QString      m_challengeTargetName;
    // What this PC last said yes to, for the line in the room chat once it's in.
    quint64      m_acceptedChallengeId = 0;
    QString      m_acceptedChallengeFrom;
    bool         m_acceptedChallengeInvite = false;
    struct ChallengeOutcome
    {
        QString title;
        QString detail;
        QColor color;
        QString icon;
        int generation = 0;
    };
    ChallengeOutcome m_challengeOutcome; // shown on the play bar until dismissed

    // ── Moderation ──
    UndoToast*   m_toast = nullptr;
    ModerationDialog* m_moderationDialog = nullptr;
    QString      m_moderatorName;
    QString      m_undoAction;
    QString      m_undoIp;
    bool         m_appliedDarkTheme = false;

    // ── The stage: the browse view, and the room window over it ──
    QWidget*     m_stage      = nullptr;
    StageScrim*  m_scrim      = nullptr;
    RoomWindow*  m_roomWindow = nullptr;
    bool         m_roomMinimized = false;
    QString      m_renameUsername;          // reconnect under this once disconnected
    QString      m_renameFallbackUsername;  // ...and back to this if it's refused

    // ── Play bar: one fixed height whatever it's showing ──
    LobbyCard*      m_playBar      = nullptr;
    QStackedWidget* m_playBarStack = nullptr; // game picker / ticket
    CartridgeTile*  m_pickerTile   = nullptr;
    ElidedLabel*    m_pickerName   = nullptr;
    QLabel*         m_pickerInfo   = nullptr;
    IconBadge*      m_ticketBadge  = nullptr;
    Spinner*        m_ticketSpinner = nullptr;
    QLabel*         m_ticketTitle  = nullptr;
    ElidedLabel*    m_ticketSub    = nullptr;
    QWidget*        m_ticketValueBoxes[2] = {};
    QLabel*         m_ticketValues[2] = {};
    QLabel*         m_ticketCaptions[2] = {};
    LobbyButton*    m_ticketAction = nullptr;
    LobbyButton*    m_ticketLeave  = nullptr;

    // The room window's title bar
    GameBadge*   m_roomBadge     = nullptr;   // the room's game
    ElidedLabel* m_roomTitle     = nullptr;   // room and game names, with room details in the tooltip
    StatePill*   m_roomStatePill = nullptr;   // "Waiting" / "In Game"
    IconButton*  m_roomSettingsButton = nullptr;
    DropPanel*   m_roomSettingsPanel  = nullptr; // prediction and join limits
    QString      m_roomBadgeText;
    QString      m_roomBadgeColor;

    // Delay is locally editable for every player; prediction is room-wide and
    // host-editable. Both stay editable during this client's own match.
    FrameStepper* m_delayStepper      = nullptr;
    ToggleSwitch* m_delayAutoCheck    = nullptr;
    FrameStepper* m_predictionStepper = nullptr;
    QLabel*       m_predictionHint    = nullptr;
    IconLabel*    m_predictionLock    = nullptr;
    bool       m_suppressSettingsSignal = false;  // guard against ROOM_STATE → setChecked echo

    // Per-player local toggle: when checked, this client writes a .krec of the
    // match (the player who checks it records, same as the p2p/kaillera lobbies).
    // Drives the shared n02_kaillera_recording_enabled flag; not synced to the room.
    ToggleSwitch* m_recordCheck  = nullptr;
    // The player's own choice. While they wait to join a running game, the
    // switch shows off and this keeps what to restore.
    bool          m_recordLocallyChoice = false;

    // Host-only room option: the next match can take players mid-game. Those
    // who join it mid-game don't record it (see updateRecordingControls).
    // Hidden on servers without the option.
    ToggleSwitch* m_lateJoinCheck = nullptr;

    QWidget*      m_joinLimitsBox   = nullptr;
    QComboBox*    m_joinPingCombo   = nullptr;
    ToggleSwitch* m_wiredOnlyCheck  = nullptr;
    QLabel*       m_joinLimitsHint  = nullptr;
    bool          m_roomHasJoinLimits = false;
    int           m_roomMaxPing = 0;
    bool          m_roomWiredOnly = false;

    // Host-only, while the room waits: how many seats it has. Hidden on
    // servers that can't change it, and in quick match and challenge rooms.
    QWidget*      m_roomSizeBox     = nullptr;
    QButtonGroup* m_roomSizeGroup   = nullptr;
    QLabel*       m_roomSizeHint    = nullptr;
    bool          m_roomResizable   = false;
    int           m_roomMaxPlayers  = 0;
    int           m_roomSeatedCount = 0;

    // Host-only room option: the next match starts from the host's pick of
    // save data instead of a blank save. Hidden on servers without it.
    ToggleSwitch* m_saveDataCheck     = nullptr;
    QWidget*      m_saveDataRow       = nullptr;
    QLabel*       m_saveDataNote      = nullptr;   // which save, and how the match's stands
    LobbyButton*  m_saveDataChangeBtn = nullptr;   // host, waiting room
    LobbyButton*  m_saveDataSaveAsBtn = nullptr;   // everyone, from match start
    ToggleSwitch* m_saveDataKeepCheck = nullptr;   // keep the Save As file updated

    // Host-only room option: game options, one switch per option shown
    // (property "gameOptionId"), after the session switches in this grid.
    QGridLayout*         m_sessionSwitches = nullptr;
    QList<ToggleSwitch*> m_gameOptionChecks;
    // Puts the switches back to what the room has when the server ignores a change.
    QTimer*              m_gameOptionsResync = nullptr;

    // When checked, the room host streams replay bytes so others can spectate,
    // independently of whether a local .krec file is saved. On the emulation thread,
    // bytes are staged into m_broadcastBuf (under m_broadcastMutex) by the n02 sink
    // and drained to the WebSocket by m_broadcastDrainTimer on the UI thread.
    ToggleSwitch* m_broadcastCheck    = nullptr;
    QTimer*    m_broadcastDrainTimer  = nullptr;
    QMutex     m_broadcastMutex;
    QByteArray m_broadcastBuf;
    bool       m_broadcasting   = false;
    quint64    m_broadcastMatchId = 0;
    // Wall-clock of the last savestate-keyframe request (0 = request one now). The
    // drain tick requests a keyframe at the lobby's interval and polls for the result.
    qint64     m_lastKeyframeRequestMs = 0;

    // Non-zero while this client is spectating a broadcast match (the match id
    // we asked the server to stream). Cleared when the spectate session ends.
    quint64    m_spectatingMatchId = 0;
    // The match whose spectators this client, as a player, last chatted with. It
    // stays set after that chat closes, so the lines stay readable until the next
    // match or leaving the room. Live means it can still be posted to.
    quint64    m_playerChatMatchId = 0;
    bool       m_playerChatLive = false;
    QString    m_roomSubText; // room details for the title tooltip, before the viewer count
    // Session boundary guard. The persistent lobby socket can still deliver krec
    // chunks from a PREVIOUS watch of the same match after we re-subscribe (same
    // matchId, so they'd otherwise pass the filter and corrupt the new replay).
    // The server always sends SPECTATE_BEGIN first on a fresh subscribe, after all
    // the stale data on the wire — so we drop every keyframe/data message until we
    // see the BEGIN for the current subscribe. Reset false in beginSpectate.
    bool       m_spectateStreamArmed = false;

    // A switch to another match, held until the current session has shut down.
    struct PendingSpectate
    {
        quint64 matchId = 0;
        QString gameName;
        QString romMd5;
    };
    PendingSpectate m_pendingSpectate;

    // Seat rows (always 4 — slots beyond maxPlayers are hidden)
    SeatRow    m_seats[4];
    QWidget*   m_seatsBox        = nullptr; // holds the port cards
    bool       m_canReorderSeats = false;   // host && room waiting
    QPoint     m_seatDragStartPos;          // press point, to clear the drag threshold
    int        m_seatDragSlot    = 0;       // slot pressed, not dragged yet (0 = none)
    int        m_seatDraggingSlot = 0;      // slot being dragged (0 = none)
    int        m_seatDropSlot    = 0;       // seat under the pointer, ringed (0 = none)
    QLabel*    m_seatGhost       = nullptr; // the lifted card, over the room window
    QPoint     m_seatGhostGrab;             // the grab point on the ghost
    QObject*   m_seatDragTracker = nullptr; // app-wide filter while dragging

    // Drives PING_PROBE_REQUEST → UDP PROBE → PROBE_REPLY refresh cycle for
    // each peer seated in the current room. Started when a room is entered,
    // stopped when it's left. Inactive (and unused) outside rooms.
    QTimer*    m_pingProbeTimer = nullptr;

    // In-room action bar
    LobbyButton* m_startBtn      = nullptr;
    LobbyButton* m_dropBtn       = nullptr;
    LobbyButton* m_leaveBtn      = nullptr;
    Spinner*     m_whySpinner    = nullptr;
    IconBadge*   m_whyBadge      = nullptr;
    ElidedLabel* m_whyTitle      = nullptr;
    LobbyButton* m_lagStatsBtn   = nullptr;

    // Trades lag numbers with the other players during a match, for /lagstat
    // and the Lag Stats window.
    LagStatsExchange* m_lagStats = nullptr;
    class LagStatsDialog* m_lagStatsDialog = nullptr;
    quint64 m_lagStatsRoomId = 0; // the room the last match with lag stats was played in

    // Browse-view action buttons
    LobbyButton* m_quickMatchBtn = nullptr;   // primary CTA
    LobbyButton* m_createRoomBtn = nullptr;
    QComboBox*   m_browseRomCombo = nullptr;   // library game picker (feeds Create Room)
    qint64       m_searchStartedMs = 0;
    int          m_searchQueueSize = 0;
    QString      m_searchingGame;

    QHash<quint64, QTreeWidgetItem*> m_userItems;

    class CreateRoomDialog* m_createRoomDialog = nullptr;

    QString  m_username;
    QString  m_lastRoomName;
    QString  m_serverUrl;
    quint64  m_currentRoomId = 0;

    QString m_currentRoomGame;
    QString m_currentRoomMd5;
    QString m_currentRoomRegion;
    QString m_currentRoomState;
    QString m_currentRoomHostName;
    int     m_currentRoomDelay      = 2;   // this client's local input delay
    int     m_currentRoomPrediction = 7;
    int     m_livePrediction        = 0;   // window handed to this match's engine
    // The server relays delay and prediction changes while the room is in game
    // (ROOM_STATE "inGameSettings"). Older servers drop them, so the host can't
    // change prediction mid-match there.
    bool    m_serverAcceptsInGameSettings = false;
    int     m_currentRoomPacing     = 0;   // 0 = aggressive, 1 = smooth
    quint64 m_currentRoomHostId     = 0;   // seated host's user id (0 when none)
    quint64 m_currentMatchId        = 0;
    quint64 m_iceWaitingMatchId     = 0;
    qint64  m_iceMatchDeadlineMs    = 0;

    // Seated user ids as of the last ROOM_STATE, used to detect when a *new*
    // player joins (so we can flash/chime). m_roomSeatsSeen suppresses the chime
    // on the first ROOM_STATE after entering a room (those seats aren't "joins").
    QSet<quint64> m_knownSeatedUsers;

    // ── Late join (see the section in the .cpp) ──
    enum class LateJoinRole { None, Host, Player, Joiner };
    struct LateJoin
    {
        LateJoinRole role = LateJoinRole::None;
        quint64 matchId = 0;
        quint64 admitId = 0;
        int epoch = 0;
        int seats = 0;
        quint64 hostId = 0;
        LobbyClient::LobbyMatchPeer joiner;       // whose client speaks for the PC joining
        QList<LobbyClient::LobbyMatchPeer> joiners; // every player it adds, joiner first
        QList<LobbyClient::LobbyMatchPeer> peers; // roster after the join
        QSet<quint64> left;                       // players who left mid hand-off
        qint64 startedMs = 0;
        int cutFrame = -1;
        qint64 cutSentMs = 0;
        QSet<quint64> awaitingCut;                // host: players yet to stop
        bool stateSent = false;                   // host
        bool cutReported = false;                 // player
        QByteArray manifest;                      // joiner: held until the open game closes
        bool launched = false;                    // joiner
        bool readySent = false;                   // joiner
        qint64 readyMs = 0;                       // joiner, host: when it was ready
        bool stateReceived = false;               // joiner
        bool released = false;                    // left the cut for the new session
        qint64 releasedMs = 0;
    };
    LateJoin m_lateJoin;
    QTimer* m_lateJoinTimer = nullptr;
    // From ROOM_STATE: the running match can take late joiners, and which
    // seated members are outside it and able to join.
    bool m_roomLateJoin = false;
    // From ROOM_STATE: the host may start with nobody else seated.
    bool m_canStartAlone = false;
    // From ROOM_STATE "lateJoinEnabled": the host turned late join on. Older
    // servers don't send it; their matches take late joiners whenever every
    // player's client can.
    bool m_roomHasLateJoinOption = false;
    bool m_roomLateJoinEnabled = false;
    bool m_lateJoinEnabledSeen = false;   // this room's first ROOM_STATE arrived
    // In a match with nobody else in it yet.
    bool m_playingAlone = false;
    int autoFrameDelay() const;
    QSet<quint64> m_outOfMatchUsers;
    QSet<quint64> m_lateJoinCapableUsers;

    // From ROOM_STATE "saveData": present when the room has the option.
    bool m_roomHasSaveDataOption = false;
    bool m_roomSaveDataEnabled = false;
    bool m_saveDataSeen = false;
    QString m_roomSaveDataLabel;
    QStringList m_roomSaveDataKinds;
    int m_roomSaveDataBytes = 0;
    QString m_roomSaveDataHash;
    // Seated members whose client can play with save data.
    QSet<quint64> m_saveDataCapableUsers;
    // Host: what the next match starts from.
    std::optional<NetplaySaveChoice> m_hostSaveChoice;
    // Host: the game options the room's next match is played with, by id
    // (CoreGetGameOptions), as the room shows them; empty for none. The match
    // start resolves them against the ROM and puts them in the manifest.
    std::vector<std::string> m_hostGameOptionIds;
    // From ROOM_STATE: the room takes game options ("gameOptionsOffered"),
    // the ones on for the next match or the running one, and which seated
    // members' clients can play with them.
    bool m_roomGameOptionsOffered = false;
    bool m_gameOptionsSeen = false;
    QList<LobbyClient::LobbyGameOption> m_roomGameOptions;
    QSet<quint64> m_gameOptionsCapableUsers;
    // Host: the room hears about a continued save once it's waiting again
    // (the server takes no save data changes while the match is in game).
    bool m_publishSaveChoiceWhenWaiting = false;
    // The match's save as every player's game agrees on it. Kept after the
    // match, for Save As, until the next one starts.
    std::optional<CoreNetplaySaveData> m_matchSave;
    bool m_matchSaveActive = false;    // a match with save data is running
    bool m_matchSaveChanged = false;   // the game wrote it this match
    qint64 m_matchSaveChangedMs = 0;
    QString m_keepCopyFile;            // the Save As file kept updated
    bool m_keepCopyEnabled = false;
    qint64 m_keepCopyWrittenMs = 0;
    QTimer* m_saveDataTimer = nullptr;
    quint64 m_admitRequestedUserId = 0;   // host clicked Add; no answer yet
    bool m_announcedWaitingSelf = false;
    // When each member's "waiting to join" last went to the overlay, so one
    // who keeps leaving and rejoining the room shows there once in a while.
    QHash<quint64, qint64> m_waitingNoticeMs;
    // Ends our game once we're no longer part of the match (see
    // onMatchWatchTick), so we can be added back instead of playing on alone.
    QTimer* m_matchWatchTimer = nullptr;
    int m_matchWatchEpoch = -1;
    QSet<int> m_matchWatchSeenSlots;   // remotes connected in that session
    void onMatchWatchTick();
    QString lobbyUserName(quint64 userId) const;
    // Who a late join adds: "Mira", or "Mira and Sam".
    QString lateJoinNames() const;
    // The banner above a running match while they're added.
    QString lateJoinBanner() const;
    void onAdmitClicked(quint64 userId);
    void launchAsLateJoiner();
    std::vector<LobbyRemotePeer> lateJoinRemotes() const;
    void applyLateJoinRoster();
    void armLateJoinCut();
    bool connectedToLateJoiner() const;
    void finishLateJoin(bool resumed);
    void abortLateJoin(const QString& reason, bool tellOthers);
    void refreshAdmitButtons();
    bool          m_roomSeatsSeen = false;

    // Delay Auto is local. Prediction Auto is the host-owned room selection;
    // its Default entry resolves to 7.
    bool    m_delayAuto = true;
    bool    m_predictionAuto = true;
    bool    m_localDelayPublished = false;
    bool    m_liveReplayPreferencePublished = false;

    bool m_awaitingEmulationStart = false;
    bool m_emulationActive        = false;
    bool m_quickMatchActive       = false;
    // Any game open on this PC, a live replay or an offline game included.
    bool m_localGameOpen          = false;

    QMap<QString, CoreRomSettings> m_roms;
    QHash<QString, QString> m_coverPaths; // by MD5; empty when there's no cover
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // ROLLBACKLOBBYDIALOG_HPP
