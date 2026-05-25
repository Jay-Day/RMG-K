/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef KAILLERAP2PDIALOG_HPP
#define KAILLERAP2PDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QTextBrowser>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QTimer>
#include <QGroupBox>
#include <QFrame>
#include <QString>
#include <QVector>

class QAction;
class QResizeEvent;

class KailleraP2PDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraP2PDialog(bool isHost, const QString& gameName,
                               const QString& username,
                               const QString& joinCode = QString(),
                               QWidget* parent = nullptr,
                               bool showOnPublicList = true);
    ~KailleraP2PDialog() override;

signals:
    void peerNicknameResolved(QString nickname);
    void rollbackSessionReady(QString gameName, QString remoteAddress, int localPort, int remotePort, int localPlayer, int frameDelay, int predictionWindow);
    void rollbackSessionEnded();

protected:
    void reject() override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onChatReceived(QString nick, QString message);
    void onGameStarted(QString game, int player, int maxPlayers);
    void onGameEnded();
    void onClientDropped(QString nick, int player);
    void onDebug(QString message);
    void onHostedGame(QString game);
    void onPingUpdated(int ping);
    void onPeerJoined();
    void onPeerLeft();
    void onPeerInfo(QString name, QString app);
    void onFodippResult(QString host);
    void onSsrvPacketReceived(QByteArray cmd, QByteArray saddr);

    void onSendChat();
    void onReady();
    void onDrop();
    void onKickPeer();
    void onCopyConnectCode();
    void onStepTimer();
    void onTravTimer();

private:
    enum class GameLayer
    {
        Standard,
        Rollback
    };

    void setupUI();
    void connectSignals();
    void cleanupSessionForClose();
    void loadLocalRollbackSettings();
    void setGameLayer(GameLayer layer, bool announceToPeer, bool resetReady);
    void applyGameLayerUI();
    void sendGameLayer();
    void sendRollbackDelaySettings(bool force);
    void sendRollbackPredictionSettings(bool force);
    void setRollbackDelayMode(int mode, bool announceToPeer, bool resetReady);
    void setRollbackCustomFrameDelay(int delay, bool announceToPeer, bool resetReady);
    void setRollbackPredictionWindow(int predictionWindow, bool announceToPeer, bool resetReady);
    void updateRollbackDelayControls();
    void updateRollbackPredictionControls();
    void updatePeerConnectionUI();
    void updateNetcodeModeStatus();
    void setPingLabelText(const QString& text);
    void setPingLabelFromValue(int ping);
    bool parseRollbackDelayMessage(const QString& message, int& mode, int& delay) const;
    bool parseRollbackPredictionMessage(const QString& message, int& predictionWindow) const;
    int effectiveRollbackFrameDelay() const;
    int effectiveRollbackPredictionWindow() const;
    int calculatedRollbackFrameDelay() const;
    int automaticRollbackFrameDelay() const;
    void resetAutomaticRollbackDelay();
    void resetAutomaticRollbackDelaySamples();
    void recordRollbackDelayPingSample(int ping);
    bool maybeUpdateAutomaticRollbackDelay(bool force = false);
    bool canEditRollbackDelaySettings() const;
    bool canEditRollbackPredictionSettings() const;
    void resetReadyState();
    bool parseGameLayerMessage(const QString& message, GameLayer& layer) const;
    bool isRollbackMode() const;
    void appendChatHtml(const QString& html, bool debug);
    void appendChatStatus(const QString& message, const QString& color, bool debug);
    void appendChatError(const QString& message);
    void rebuildChat();
    void setShowDebugMessages(bool show);
    void updateLobbyStatusLabel();
    void positionCornerButtons();
    void appendPeerJoinedNotice();
    void appendPeerLeftNotice(const QString& name);

    struct ChatEntry
    {
        QString html;
        bool debug = false;
    };

    // NAT traversal helpers
    void travSendToServer(const QByteArray& msg);
    void travSendClaimAuto();
    void travSendClaimAck();
    void travSendHostOpen();
    void travSendHostKeep();
    void travSendHostClose();
    void travSendJoin();
    void travPunchEndpoint(const QString& hostIp, int hostPort, const QString& token);
    void travResetState();
    bool travTryFallbackConnect(const QString& reason);
    void updateHostCodeUI();
    void travLoadIdentity();
    void travSaveIdentity() const;
    void travClearIdentity();
    void ssrvSend(const QByteArray& cmd);
    void ssrvWhatIsMyIp();
    void enlistGame();
    void unenlistGame();
    QString buildEnlistAppName();

    bool m_isHost;
    GameLayer m_gameLayer = GameLayer::Standard;
    bool m_rollbackGameActive = false;
    bool m_closeCleanupDone = false;
    bool m_ready = false;
    bool m_peerReady = false;
    bool m_peerConnected = false;
    bool m_peerKickPending = false;
    bool m_peerJoinNoticeShown = false;
    bool m_peerLeaveNoticeShown = false;
    bool m_initialShowOnPublicList = true;
    bool m_lobbyOpening = false;
    bool m_gameActive = false;
    int m_standardFrameDelay = 0;
    QString m_gameName;
    QString m_username;
    QString m_peerName;

    // Top
    QLabel* m_lobbyStatusLabel = nullptr;
    QLabel* m_gameLabel = nullptr;
    QPushButton* m_btnKickPeer = nullptr;

    // Players
    QFrame* m_hostPlayerCard = nullptr;
    QLabel* m_hostPlayerNameLabel = nullptr;
    QFrame* m_hostConnectCodeBadge = nullptr;
    QLabel* m_hostConnectCodeLabel = nullptr;
    QPushButton* m_btnCopyConnectCode = nullptr;
    QLabel* m_hostReadyLabel = nullptr;
    QLabel* m_playersEmptyLabel = nullptr;
    QFrame* m_playerCard = nullptr;
    QLabel* m_playerNameLabel = nullptr;
    QLabel* m_playerReadyLabel = nullptr;

    // Chat area
    QGroupBox* m_chatGroup = nullptr;
    QTextBrowser* m_chat = nullptr;
    QLineEdit* m_chatInput = nullptr;
    QPushButton* m_btnChat = nullptr;
    QPushButton* m_chatSettingsButton = nullptr;
    QAction* m_showDebugMessagesAction = nullptr;
    QVector<ChatEntry> m_chatHistory;
    bool m_showDebugMessages = false;

    // Control buttons
    QPushButton* m_btnReady = nullptr;
    QPushButton* m_btnDrop = nullptr;
    QPushButton* m_btnLeave = nullptr;
    QCheckBox* m_recordCheck = nullptr;
    QCheckBox* m_enlistCheck = nullptr;
    QLabel* m_netcodeModeLabel = nullptr;
    QLabel* m_pingLabel = nullptr;
    QLabel* m_delayLabel = nullptr;
    QPushButton* m_netcodeSettingsButton = nullptr;
    QPushButton* m_standardLayerButton = nullptr;
    QPushButton* m_rollbackLayerButton = nullptr;

    // Host group
    QGroupBox* m_hostGroup = nullptr;
    QLabel* m_frameDelayLabel = nullptr;
    QWidget* m_frameDelayRow = nullptr;
    QComboBox* m_frameDelayCombo = nullptr;
    QComboBox* m_customFrameDelayCombo = nullptr;
    QLabel* m_frameDelayHelpLabel = nullptr;
    QComboBox* m_predictionWindowCombo = nullptr;

    int m_lastPing = -1;
    int m_rollbackDelayMode = 0;
    int m_customRollbackFrameDelay = 2;
    int m_rollbackFrameDelay = 2;
    int m_autoRollbackFrameDelay = 2;
    int m_rollbackPredictionWindow = 0;
    struct RollbackDelayPingSample
    {
        unsigned long timestampMs = 0;
        int ping = -1;
    };
    QVector<RollbackDelayPingSample> m_rollbackDelayPingSamples;
    unsigned long m_rollbackDelayFirstSampleMs = 0;
    unsigned long m_rollbackDelayLastUpdateMs = 0;
    bool m_hasRemoteRollbackDelaySettings = false;
    bool m_hasRemoteRollbackPredictionSettings = false;
    bool m_hasSentRollbackDelaySettings = false;
    bool m_hasSentRollbackPredictionSettings = false;
    int m_lastSentRollbackDelayMode = -1;
    int m_lastSentRollbackFrameDelay = -1;
    int m_lastSentRollbackPredictionWindow = -1;

    // Timers
    QTimer* m_stepTimer = nullptr;
    QTimer* m_travTimer = nullptr;
    QTimer* m_copyFeedbackTimer = nullptr;

    // ---- NAT traversal state ----
    bool m_travHostEnabled = false;
    bool m_travJoinEnabled = false;
    QString m_travCode;
    QString m_travToken;
    QString m_travLiveToken;
    int m_travRegAttempts = 0;
    bool m_travHostSessionSuspended = false;
    bool m_travHostFallbackActive = false;
    bool m_travHostIpPending = false;
    QString m_travHostIpPort;

    qint64 m_travNextRegMs = 0;
    qint64 m_travNextKeepMs = 0;

    // Join-by-code state
    qint64 m_travNextJoinMs = 0;
    qint64 m_travJoinDeadlineMs = 0;
    QString m_travJoinCode;
    bool m_travJoinGotHost = false;
    QString m_travJoinToken;
    QString m_travJoinHostIp;
    int m_travJoinHostPort = 0;
    qint64 m_travNextConnectMs = 0;
    QString m_travJoinFallbackIpPort;
    bool m_travJoinFallbackTried = false;
    bool m_travJoinBusy = false;
    int m_travJoinPunchAttempts = 0;

    // Host peer-punching state
    QString m_travHostPeerIp;
    int m_travHostPeerPort = 0;
    qint64 m_travHostPeerDeadlineMs = 0;
    qint64 m_travNextHostPunchMs = 0;

    bool m_ssrvCopyMyIpPending = false;
    int m_travTimerStep = 0;
};

#endif // NETPLAY
#endif // KAILLERAP2PDIALOG_HPP
