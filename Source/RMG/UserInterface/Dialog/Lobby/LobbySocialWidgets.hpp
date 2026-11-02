#ifndef LOBBYSOCIALWIDGETS_HPP
#define LOBBYSOCIALWIDGETS_HPP

#ifdef NETPLAY

#include "LobbyWidgets.hpp"

#include <QFrame>
#include <QHash>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QString>
#include <QTabBar>
#include <QTextEdit>

#include <functional>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

// A presence state's colour, for its dot.
QColor presenceColor(const QString& state);

// The lobby's menus: a real QMenu, so keyboard use and screen readers work, drawn
// by hand because a stylesheet can't colour single items or show a note beside one.
class LobbyMenu : public QMenu
{
    Q_OBJECT

public:
    explicit LobbyMenu(QWidget* parent = nullptr);

    QAction* addItem(const QString& iconName, const QString& text);
    // Red, for what can't be taken back.
    QAction* addDangerItem(const QString& iconName, const QString& text);
    // Greyed out, saying why.
    QAction* addDisabledItem(const QString& iconName, const QString& text, const QString& why);
    LobbyMenu* addSubmenu(const QString& iconName, const QString& text);
    // Small heading over the items that follow.
    void addHeading(const QString& iconName, const QString& text, bool moderator);
    // A widget shown at the top, or anywhere; it takes no clicks.
    void addHeaderWidget(QWidget* widget);
    // A widget that keeps the menu open while it's used.
    void addLiveWidget(QWidget* widget);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
};

// The chat tabs. An icon-only tab is only as wide as its icon and badge; left to
// itself, QTabBar reserves room for text it doesn't have.
class ChatTabBar : public QTabBar
{
    Q_OBJECT

public:
    using QTabBar::QTabBar;

protected:
    QSize tabSizeHint(int index) const override;
};

// A player's initial on a tile in one of the port colours, picked from the name so
// a person keeps their colour.
class PlayerTile : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerTile(int side, QWidget* parent = nullptr);

    void setPlayer(const QString& name);
    void setDimmed(bool dimmed);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_name;
    bool m_dimmed = false;
};

// One chat log. Player names and @mentions in it are links: left or right clicking
// one reports the player, and the dialog opens their menu.
class ChatView : public QTextEdit
{
    Q_OBJECT

public:
    explicit ChatView(QWidget* parent = nullptr);

    // Adds one finished line of chat.
    void addLine(const QString& html);
    // The href a player's name links to.
    static QString userHref(quint64 userId);

signals:
    void userActivated(quint64 userId, const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    quint64 userAt(const QPoint& pos) const;

    QPoint m_pressPos;
};

// What the floating card says about one challenge.
struct ChallengeCardView
{
    quint64 challengeId = 0;
    bool invite = false;
    QString fromName;
    QString fromCountry;
    int fromPingMs = -1;
    bool fromPingEstimate = true;
    QString fromConnection;
    QString toName;
    QString toCountry;
    QString toTag;       // "You", or how they're on this PC
    QString toDetail;    // under their name
    QString toConnection;
    int roomPlayers = 0;
    int roomMaxPlayers = 0;
    QString game;
    QString gameDetail;  // the sender's message, or which room
    QString coverPath;
    QString warning;     // what accepting costs
    bool canAccept = true;
    int waitingBehind = 0;
    qint64 expiresAtMs = 0;
    qint64 durationMs = 30'000;
};

// An incoming challenge. It floats over the stage in the same place in every view,
// and the lobby stays usable around it.
class ChallengeCard : public QFrame
{
    Q_OBJECT

public:
    explicit ChallengeCard(QWidget* parent = nullptr);

    void setView(const ChallengeCardView& view);

signals:
    void acceptClicked();
    void declineClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void tick();

    ChallengeCardView m_view;
    QTimer* m_timer;
    IconLabel* m_bandIcon;
    QLabel* m_bandTitle;
    QLabel* m_more;
    QLabel* m_seconds;
    PlayerTile* m_fromTile;
    ElidedLabel* m_fromName;
    CountryFlag* m_fromFlag;
    SignalBars* m_fromBars;
    QLabel* m_fromPing;
    ConnectionGlyph* m_fromConnection;
    QLabel* m_fromConnectionName;
    QLabel* m_vs;
    QWidget* m_toSide;
    PlayerTile* m_toTile;
    ElidedLabel* m_toName;
    StatusTag* m_toTag;
    CountryFlag* m_toFlag;
    ConnectionGlyph* m_toConnection;
    QLabel* m_toDetail;
    QWidget* m_seatsSide;
    QLabel* m_seatsLabel;
    SeatPips* m_seatsPips;
    CartridgeTile* m_tile;
    ElidedLabel* m_game;
    ElidedLabel* m_gameDetail;
    IconLabel* m_warnIcon;
    ElidedLabel* m_warn;
    LobbyButton* m_decline;
    LobbyButton* m_accept;
};

// A short confirmation over the stage's corner, for what can still be undone.
class UndoToast : public QFrame
{
    Q_OBJECT

public:
    explicit UndoToast(QWidget* parent = nullptr);

    // The Undo button shows only when canUndo is set.
    void showMessage(const QString& text, bool canUndo);

signals:
    void undoClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void tick();

    QLabel* m_text;
    QWidget* m_meter;
    LobbyButton* m_undo;
    QTimer* m_timer;
    qint64 m_shownAtMs = 0;
};

// One name @ can complete to.
struct MentionCandidate
{
    QString name;
    QString state;      // presence state, for the dot
    QString country;
    QString note;       // right-hand word: "P3", "Online"
};

// The list above the message box while typing a name. It never takes keyboard
// focus; the box keeps typing and forwards the keys that drive this.
class MentionPopup : public QFrame
{
    Q_OBJECT

public:
    explicit MentionPopup(QWidget* parent = nullptr);

    // typed is how much of each name is highlighted. heading goes over the list.
    void setCandidates(const QList<MentionCandidate>& candidates, int typed, const QString& heading);
    void moveSelection(int delta);
    QString selectedName() const;
    int rowCount() const { return int(m_rows.size()); }

    QSize sizeHint() const override;

signals:
    void picked(const QString& name);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    int rowAt(const QPoint& pos) const;
    int rowsTop() const;

    QList<MentionCandidate> m_rows;
    int m_typed = 0;
    int m_selected = 0;
    QString m_heading;
};

// The mail tab: an inbox, and one thread per person. Threads belong to one player
// on this PC and one user id, never a name, because anyone can pick up a guest's
// name after they leave. They last as long as the connection; the server keeps
// nothing.
class PrivateMessagesPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PrivateMessagesPanel(QWidget* parent = nullptr);

    // Finds or starts the thread between this PC's localId and remoteId.
    ChatView* threadView(quint64 localId, const QString& localName,
                         quint64 remoteId, const QString& remoteName, bool localIsOwner);
    // Records a line for the inbox's preview. Unread unless the thread is open.
    void noteMessage(quint64 localId, quint64 remoteId, const QString& preview, qint64 timeMs, bool incoming);
    void openThread(quint64 localId, quint64 remoteId);
    void showInbox();
    void clear();

    // Which of this PC's players already talks to remoteId, or 0.
    quint64 threadLocalFor(quint64 remoteId) const;
    ChatView* existingView(quint64 remoteId) const;

    // Whether the panel is what the person is looking at.
    void setActive(bool active);

    // How the remote player's row and header read right now. A player who left,
    // from either end of a thread, stays listed, but it can't be written to any more.
    void setRemoteStatus(quint64 remoteId, const QString& state, const QString& statusText,
                         const QString& country, bool canChallenge);
    void markGone(quint64 userId);

    bool threadOpen() const { return m_openLocal != 0; }
    quint64 openLocalId() const { return m_openLocal; }
    quint64 openRemoteId() const { return m_openRemote; }
    QString openRemoteName() const;
    QString openLocalName() const;
    bool openLocalIsOwner() const;
    bool openRemoteGone() const;
    bool openLocalGone() const;
    int unreadTotal() const;
    QList<ChatView*> views() const;

signals:
    // The open thread changed, so the message box must say something else.
    void currentChanged();
    void unreadChanged();
    void userActivated(quint64 userId, const QPoint& globalPos);
    void challengeRequested(quint64 remoteId);
    void menuRequested(quint64 remoteId, const QPoint& globalPos);
    void newMessageRequested();

private:
    struct Thread
    {
        quint64 localId = 0;
        QString localName;
        bool localIsOwner = true;
        quint64 remoteId = 0;
        QString remoteName;
        QString remoteCountry;
        QString state;
        QString statusText;
        bool canChallenge = true;
        bool gone = false;
        bool localGone = false; // gone because it was this PC's end that left
        int unread = 0;
        QString preview;
        qint64 lastMs = 0;
        ChatView* view = nullptr;
    };

    static quint64 keyOf(quint64 localId, quint64 remoteId) { return (localId << 32) ^ remoteId; }
    Thread* openThreadData();
    const Thread* openThreadData() const;
    void rebuildInbox();
    void refreshHeader();

    QHash<quint64, Thread> m_threads;
    quint64 m_openLocal = 0;
    quint64 m_openRemote = 0;
    bool m_active = false;

    QStackedWidget* m_pages;
    QWidget* m_inboxRows;
    QVBoxLayout* m_inboxLayout;
    QLabel* m_inboxEmpty;
    QStackedWidget* m_threadStack;
    PlayerTile* m_headerTile;
    ElidedLabel* m_headerName;
    CountryFlag* m_headerFlag;
    StatusTag* m_headerTo;
    LedDot* m_headerDot;
    ElidedLabel* m_headerStatus;
    IconButton* m_headerChallenge;
    IconButton* m_headerMore;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYSOCIALWIDGETS_HPP
