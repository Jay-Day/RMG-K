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

class QActionGroup;
class QLabel;
class QTimer;
class QToolButton;

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
    // A check box, or one of a group of choices when group is set. Ticking it
    // leaves the menu open, so several can be set in one go.
    QAction* addCheckItem(const QString& iconName, const QString& text, bool checked,
                          QActionGroup* group = nullptr);
    // The same, with the signal bars of a ping limit for its icon (0: none).
    QAction* addPingItem(int maxPingMs, const QString& text, bool checked, QActionGroup* group);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
};

// The chat tabs. An icon-only tab is only as wide as its icon and badge; left to
// itself, QTabBar reserves room for text it doesn't have. A tab can have a face
// instead of text: a widget it's sized by and that covers it. When the bar runs
// out of room, tabs with a face shrink to their face's minimum first, and then the
// bar scrolls.
class ChatTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit ChatTabBar(QWidget* parent = nullptr);

    // Use this, not setTabVisible: that, given a tab's current visibility,
    // also drops a relayout an earlier call still owes, so a hidden tab could
    // leave a gap where it was.
    void showTab(int index, bool visible);

    // The tab takes the face's size; give it no text. The bar owns the face
    // from here, and deletes it with its tab.
    void setTabFace(int index, QWidget* face);
    QWidget* tabFace(int index) const;
    // A face's tab has something unread. Its scroll button says so while it's out of view.
    void setTabAttention(int index, bool attention);
    // Lays the tabs out again after a tab's face changed size.
    void refreshTab(int index);

signals:
    // A right click on a tab. A middle click on one with a face asks to close
    // it (tabCloseRequested).
    void tabMenuRequested(int index, const QPoint& globalPos);

protected:
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;
    void tabLayoutChange() override;
    void childEvent(QChildEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void positionBadges();
    // Puts a tab's face and badge where they go, which QTabBar doesn't know.
    void placeTabButtons(int index);
    void paintScrollButton(QToolButton* button);
    // Where tabs show, between the scroll buttons.
    QRect tabArea() const;
    // What the tabs scrolled out of view to that side have unread. A lobby mention
    // alone is the quieter of the two, as its badge is.
    enum class HiddenUnread { None, Mention, Message };
    HiddenUnread unreadOutOfView(bool left) const;

    QList<QToolButton*> m_scrollButtons;
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

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYSOCIALWIDGETS_HPP
