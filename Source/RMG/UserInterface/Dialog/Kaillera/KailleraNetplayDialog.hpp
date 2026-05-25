/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef KAILLERANETPLAYDIALOG_HPP
#define KAILLERANETPLAYDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QAction>
#include <QTimer>
#include <QTabWidget>
#include <QListWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QColor>
#include <QPushButton>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUdpSocket>

#include <future>

class KailleraP2PDialog;

struct ServerEntry {
    QString name;
    QString host; // "ip:port"
    QString country;
    QString players = "-";
    int playerCount = -1;
    QString ping;
    int pingValue = 999999;
};

class KailleraNetplayDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraNetplayDialog(QWidget* parent = nullptr);
    ~KailleraNetplayDialog() override;

signals:
    void rollbackSessionPreparing();
    void rollbackSessionRequested(QString gameName, QString remoteAddress, int localPort, int remotePort, int localPlayer, int frameDelay, int predictionWindow);

private slots:
    void onStateMachineTimer();

    // Server tab
    void onAddServer();
    void onEditServer();
    void onConnectServer();
    void onServerDoubleClicked(int row, int column);
    void onServerRightClicked(QPoint pos);
    void onWaitingGames();

    // P2P tab
    void onP2PHost();
    void onP2PHostPrivate();
    void onP2PJoin();
    void onP2PPasteAndGo();
    void onP2PStoredRightClicked(const QPoint& pos);
    void onCopyP2PCode();
    void onConfigureP2PCode();

    // Network replies
    void onWaitingGamesReply(QNetworkReply* reply);

    // Tab changed
    void onTabChanged(int index);

private:
    void setupUI();
    QWidget* createServerTab();
    QWidget* createP2PTab();

    void loadServerList();
    void saveServerList();
    void refreshServerListDisplay(bool forcePingResort = false);
    void fetchLiveServerList();
    void schedulePingAllServers();
    void pingAllServers();
    void startNextServerPing();
    void pollServerPing();
    void stopServerPingQueue();
    void waitForActiveServerPing(bool applyResult);
    QVector<ServerEntry> parseLiveServerList(const QByteArray& data) const;
    int favoriteServerIndexByHost(const QString& host) const;
    int cachedServerIndexByHost(const QString& host) const;
    void toggleFavoriteServer(const QString& host, const QString& name);
    void moveFavoriteServer(int favoriteIndex, int delta);
    void updateServerPing(const QString& host, int pingMs);
    void updateVisibleServerPing(const QString& host, const QString& pingText);
    void cacheVisibleLiveServerOrder();
    void updateServerButtons();
    void saveSettings();
    void loadSettings();

    // P2P recent/favorite peers
    void loadP2PStoredUsers();
    void saveP2PStoredUsers();
    void refreshP2PStoredDisplay();
    int p2pStoredIndexByHost(const QString& host) const;
    int p2pFavoriteCount() const;
    void toggleP2PStoredFavorite(int row);
    void editP2PStoredEntry(int row);
    void deleteP2PStoredEntry(int row);
    void rememberP2PStoredEntry(const QString& host, const QString& nickname = QString());
    void updateP2PStoredNickname(const QString& host, const QString& nickname);
    void showP2PCodeStatusMessage(const QString& message, const QColor& color);
    void refreshP2PStaticCodeDisplay();
    void maybeAutoClaimP2PStaticCode();
    void cancelPendingP2PAutoClaim();
    QString currentP2PStaticCode() const;
    QString currentP2PStaticCodeOwnerToken() const;
    void fetchP2PWaitingGames(bool manualRefresh = false);
    void handleP2PWaitingGamesReply(QNetworkReply* reply);
    void populateP2PWaitingGames(const QByteArray& data);
    void useP2PWaitingGameRow(int row, bool connectNow);
    void connectRollbackSessionLaunch(KailleraP2PDialog& p2pDialog, bool& rollbackSessionActive);
    void hostP2P(bool showOnPublicList);

    // State machine timer (replaces blocking KSSDFA loop)
    QTimer* m_stateMachineTimer = nullptr;

    // Mode tabs
    QTabWidget* m_tabWidget = nullptr;

    // Server list (Server tab)
    QTableWidget* m_serverTable = nullptr;
    QVector<ServerEntry> m_favoriteServers;
    QVector<ServerEntry> m_cachedLiveServers;
    QVector<ServerEntry> m_displayServers;
    int m_serverSortColumn = 4;
    Qt::SortOrder m_serverSortOrder = Qt::AscendingOrder;

    // Server tab buttons
    QPushButton* m_btnAdd = nullptr;
    QPushButton* m_btnConnect = nullptr;
    QPushButton* m_btnWaitingGames = nullptr;

    // P2P host controls
    QLineEdit* m_p2pCurrentCodeEdit = nullptr;
    QAction* m_p2pCopyAction = nullptr;
    QPushButton* m_btnP2PConfigureCode = nullptr;
    QLabel* m_p2pCodeStatusLabel = nullptr;
    QComboBox* m_p2pGameCombo = nullptr;
    QPushButton* m_btnP2PHost = nullptr;
    QPushButton* m_btnP2PHostPrivate = nullptr;

    // P2P connect controls
    QLineEdit* m_p2pHostEdit = nullptr;
    QPushButton* m_btnP2PJoin = nullptr;
    QListWidget* m_p2pStoredList = nullptr;
    QTableWidget* m_p2pWaitingGamesTable = nullptr;
    QPushButton* m_btnP2PWaitingGamesReload = nullptr;
    QTimer* m_p2pWaitingGamesRefreshTimer = nullptr;
    bool m_p2pWaitingGamesFetchInFlight = false;

    struct P2PStoredEntry {
        QString name;
        QString host;
        bool favorite = false;
    };
    QVector<P2PStoredEntry> m_p2pStoredUsers;

    // Shared settings (shown above tabs)
    QLineEdit* m_usernameEdit = nullptr;

    // Frame delay (Server tab only)
    QComboBox* m_frameDelayCombo = nullptr;

    // Network manager for master list fetching
    QNetworkAccessManager* m_netManager = nullptr;
    QTimer* m_serverPingPollTimer = nullptr;
    QStringList m_pendingPingHosts;
    QString m_activePingHost;
    std::future<int> m_activePingFuture;
    bool m_serverListNeedsRefresh = false;
    bool m_pingAllQueued = false;
    bool m_pingAllInProgress = false;
    bool m_serverPingsSuspended = false;
    QTimer* m_p2pCopyFeedbackTimer = nullptr;
    QTimer* m_p2pCodeStatusTimer = nullptr;
    QUdpSocket* m_p2pAutoClaimSocket = nullptr;
    QTimer* m_p2pAutoClaimTimeoutTimer = nullptr;
    bool m_p2pAutoClaimAttempted = false;
    bool m_p2pHostLaunchQueued = false;
    bool m_p2pHostLaunchQueuedPublic = true;
    bool m_p2pAutoClaimAwaitingAck = false;
    QString m_p2pAutoClaimPendingCode;
    QString m_p2pAutoClaimPendingToken;

};

#endif // NETPLAY
#endif // KAILLERANETPLAYDIALOG_HPP
