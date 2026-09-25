/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef KAILLERASERVERBROWSERDIALOG_HPP
#define KAILLERASERVERBROWSERDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QTimer>
#include <QTableWidget>
#include <QListWidget>
#include <QTextBrowser>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QStackedWidget>
#include <QSplitter>
#include <QMenu>

#ifdef RMGK_GAME_STATS
#include <RMG-Core/Replay.hpp>
#endif

class KailleraServerBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KailleraServerBrowserDialog(const QString& serverName, QWidget* parent = nullptr);
    ~KailleraServerBrowserDialog() override;

protected:
    void reject() override;

private slots:
    // Lobby mode handlers
    void onUserAdded(QString name, int ping, int status, unsigned short id, char conn);
    void onUserJoined(QString name, int ping, unsigned short id, char conn);
    void onUserLeft(QString name, QString quitMsg, unsigned short id);
    void onGameAdded(QString gameName, unsigned int id, QString emulator, QString owner, QString users, char status);
    void onGameCreated(QString gameName, unsigned int id, QString emulator, QString owner);
    void onGameClosed(unsigned int id);
    void onGameStatusChanged(unsigned int id, char status, int players, int maxPlayers);
    void onChatReceived(QString name, QString message);
    void onMotdReceived(QString name, QString message);
    void onLoginStatus(QString message);
    void onError(QString message);

    // Game room handlers
    void onUserGameCreated();
    void onUserGameJoined();
    void onUserGameClosed();
    void onPlayerAdded(QString name, int ping, unsigned short id, char conn);
    void onPlayerJoined(QString name, int ping, unsigned short uid, char conn);
    void onPlayerLeft(QString name, unsigned short id);
    void onGameChatReceived(QString name, QString message);
    void onUserKicked();
    void onPlayerDropped(QString name, int player);
    void onGameStarted(QString game, int player, int numPlayers);
    void onGameEnded();
    void onNetsyncWait(int tx);

    // Button actions
    void onCreateOrSwap();
    void onJoinGame();
    void onStartGame();
    void onDropGame();
    void onLeaveGame();
    void onKickPlayer();
    void onLagStat();
    void onOptions();
    void onAdvertise();
    void onSendLobbyChat();
    void onSendGameChat();

    // Context menus
    void onUserListContextMenu(const QPoint& pos);
    void onGameListContextMenu(const QPoint& pos);
    void onPlayerListContextMenu(const QPoint& pos);

    // Stats timer
    void onStatsTimer();

private:
    void setupUI();
    QWidget* createGameListWidget();
    QWidget* createGameRoomWidget();
    void connectSignals();
    void switchToLobby();
    void switchToGameRoom();
    void showBottomGameList();
    void showBottomGameRoom();
    void setRoomChatSwapView(bool showLobbies);
    void refreshRoomLobbyTable();
    void buildGameListMenu();
    void requestCreateGame(const QString& gameName);
    void addCreateMenuEntries(QMenu* parentMenu);
    void populateGameSubmenus(QMenu* parentMenu);
    void executeOptions();
    void saveColumnWidths();
    void restoreColumnWidths();
    void updateTitle();
    void updateHeaderCounts();
    int detectCurrentRoomMaxPlayers();
    QString timestamp(const QString& baseColor = QString());
    QString linkify(const QString& text);
    QString connString(char conn);
    QString userStatusString(char status);
    QString gameStatusString(char status);
    bool hasOpenSlot(QTableWidget* table, int row) const;
    bool tryJoinGameFromTable(QTableWidget* table, int row, bool leaveCurrentGame);
    void syncCurrentGameUsersCount();
    void refreshPlayerCards();
    int findPlayerIndexById(unsigned short id) const;
    int findRowByText(QTableWidget* table, int column, const QString& text);
    void updateUserStatus(const QString& username, const QString& status, int sortKey);

    // Server info
    QString m_serverName;

    // Mode switching
    QStackedWidget* m_bottomStack = nullptr;
    bool m_inGameRoom = false;
    bool m_isHost = false;
    bool m_isClosing = false;
    QString m_currentGameName;
    unsigned int m_currentGameId = 0;
    QString m_pendingJoinGameName;
    unsigned int m_pendingJoinGameId = 0;

    // Top section (always visible)
    QSplitter* m_topSplitter = nullptr;
    QSplitter* m_roomSplitter = nullptr;
    QTextBrowser* m_lobbyChat = nullptr;
    QTableWidget* m_userTable = nullptr;
    QLineEdit* m_lobbyChatInput = nullptr;
    QPushButton* m_btnSendLobby = nullptr;
    QPushButton* m_btnCreateSwap = nullptr;
    QLabel* m_connectedPlayersCountLabel = nullptr;

    // Bottom: Game table (page 0)
    QTableWidget* m_gameTable = nullptr;

    // Bottom: Game room (page 1)
    QListWidget* m_playerList = nullptr;
    QStackedWidget* m_roomChatStack = nullptr;
    QTableWidget* m_roomLobbyTable = nullptr;
    bool m_roomShowingLobbies = false;
    QTextBrowser* m_gameChat = nullptr;
    QLineEdit* m_gameChatInput = nullptr;
    QPushButton* m_btnSendGame = nullptr;
    QPushButton* m_btnSwapChat = nullptr;
    QPushButton* m_btnStart = nullptr;
    QPushButton* m_btnDrop = nullptr;
    QPushButton* m_btnLeave = nullptr;
    QPushButton* m_btnLagStat = nullptr;
    QPushButton* m_btnOptions = nullptr;
    QPushButton* m_btnAdvertise = nullptr;
    QCheckBox* m_recordCheck = nullptr;
#ifdef RMGK_GAME_STATS
    // Independent .rmgr toggle, mirrors m_recordCheck's pattern - see
    // RollbackLobbyDialog.hpp for the full rationale. Only shown when
    // m_currentGameName is Smash Remix 2.0.1.
    QCheckBox* m_replayRecordCheck = nullptr;
#endif
    QLabel* m_fpsLabel = nullptr;
    QLabel* m_playersInGameCountLabel = nullptr;
    int m_roomMaxPlayers = 0;

    // Stats timer
    QTimer* m_statsTimer = nullptr;
    int m_lastFrameCount = 0;
    int m_lastPPS = 0;

    // Game list menu for Create Game
    QMenu* m_gameListMenu = nullptr;

    // Find a row by matching a numeric value in the given column
    int findRowByValue(QTableWidget* table, int column, unsigned int value);
};

#endif // NETPLAY
#endif // KAILLERASERVERBROWSERDIALOG_HPP
