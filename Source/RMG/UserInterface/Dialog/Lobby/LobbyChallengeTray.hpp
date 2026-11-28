#ifndef LOBBYCHALLENGETRAY_HPP
#define LOBBYCHALLENGETRAY_HPP

#ifdef NETPLAY

#include "LobbyTheme.hpp"

#include <QFrame>
#include <QList>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

class CartridgeTile;
class ConnectionGlyph;
class CountryFlag;
class ElidedLabel;
class IconLabel;
class LobbyButton;
class PlayerTile;
class SeatPips;
class SignalBars;

// How one challenge to or from this PC went, as the challenge log says it.
enum class ChallengeLogOutcome
{
    // To this PC
    Missed,             // it ran out of time unanswered
    WhilePlaying,       // the lobby turned it away because this PC was in a match
    WhileNotificationsOff,
    Withdrawn,          // the sender called it off first
    YouDeclined,
    YouAccepted,
    CouldNotGoAhead,    // accepted too late, or it ended some other way
    // From this PC
    Accepted,
    Declined,
    DeclinedRecently,
    NoAnswer,
    InAMatch,
    NotTakingChallenges,
    NoGame,
    NoteBlocked,
    NeedsUpdate,
    TooFast,
    Unavailable,
};

struct ChallengeLogEntry
{
    quint64 key = 0;
    QString name;       // the other player
    QString forName;    // to this PC, for another of its players: who
    bool incoming = true;
    bool invite = false;
    QString game;
    QString md5;
    QString roomName;   // an invite's room
    ChallengeLogOutcome outcome = ChallengeLogOutcome::Missed;
    qint64 timeMs = 0;  // the last time it happened
    int count = 1;
    int unseen = 0;     // how many of those times nobody here has seen yet
};

// The challenges to and from this PC, newest first, for as long as the app
// runs: through reconnects and the lobby window closing. Never on disk.
class ChallengeLog
{
public:
    static ChallengeLog& session();

    // The kinds the unseen count is made of: the ones nobody answered live.
    static bool countsUnseen(ChallengeLogOutcome outcome);

    // Adds an entry. When the player's newest entry is the same thing again
    // (direction, outcome and game), that one counts it and moves to the top.
    // seen: someone was looking at the log when it happened.
    void add(ChallengeLogEntry entry, bool seen);
    void markSeen();
    void clear();
    const QList<ChallengeLogEntry>& entries() const { return m_entries; }

private:
    QList<ChallengeLogEntry> m_entries;
    quint64 m_nextKey = 1;
};

// The header's way into the challenges tray: crossed swords, with a count of
// the challenges this PC missed that nobody has looked at yet.
class ChallengesButton : public QPushButton
{
    Q_OBJECT

public:
    explicit ChallengesButton(QWidget* parent = nullptr);

    void setUnseen(int count);
    // The tray's log is showing.
    void setOpen(bool open);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void refreshTip();

    int m_unseen = 0;
    bool m_open = false;
};

// What the card for one open challenge to this PC says.
struct ChallengeCardView
{
    quint64 challengeId = 0;
    bool invite = false;
    QString fromName;
    QString fromCountry;
    int fromPingMs = -1;
    bool fromPingEstimate = true;
    QString fromConnection;
    // Who on this PC it's for, and who else plays here: only when this PC
    // has other players.
    QString forDetail;
    int roomPlayers = 0;
    int roomMaxPlayers = 0;
    QString game;
    QString gameDetail;  // the sender's message, or which room
    QString coverPath;
    QString warning;     // what accepting costs
    bool canAccept = true;
    qint64 expiresAtMs = 0;
    qint64 durationMs = 30'000;
};

// One open challenge to this PC, in the tray. It answers only its own.
class ChallengeCard : public QFrame
{
    Q_OBJECT

public:
    explicit ChallengeCard(QWidget* parent = nullptr);

    void setView(const ChallengeCardView& view);
    quint64 challengeId() const { return m_view.challengeId; }

    // "18 SEC": what's left of a challenge's time, as its card shows it.
    static QString secondsLeft(qint64 msLeft);

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
    QLabel* m_seconds;
    PlayerTile* m_fromTile;
    ElidedLabel* m_fromName;
    CountryFlag* m_fromFlag;
    SignalBars* m_fromBars;
    QLabel* m_fromPing;
    ConnectionGlyph* m_fromConnection;
    QLabel* m_fromConnectionName;
    CartridgeTile* m_tile;
    ElidedLabel* m_game;
    ElidedLabel* m_gameDetail;
    QWidget* m_seatsBox;
    QLabel* m_seatsLabel;
    SeatPips* m_seatsPips;
    QWidget* m_forRow;
    ElidedLabel* m_forText;
    QWidget* m_warnRow;
    ElidedLabel* m_warn;
    LobbyButton* m_decline;
    LobbyButton* m_accept;
};

// The challenge this PC has out, as its card shows it.
struct OutgoingChallengeView
{
    quint64 challengeId = 0; // 0: none out
    QString title;           // "Challenge sent"
    QString waitingFor;      // "Waiting for Mira"
    QString toName;
    QString toCountry;
    int pingMs = -1;
    bool pingEstimate = true;
    QString game;
    QString coverPath;
    qint64 expiresAtMs = 0;
    qint64 durationMs = 30'000;
};

// One row of the challenge log.
struct ChallengeLogRowView
{
    quint64 key = 0;
    QString name;
    QString country;        // only for a player online now who shows one
    bool incoming = true;
    QString game;
    QString what;           // what happened
    LobbyTheme::Role whatRole = LobbyTheme::Role::Muted;
    qint64 timeMs = 0;
    int count = 1;
    bool fresh = false;     // unseen until this look at the log
    QString action;         // the button's text
    QString unavailable;    // why there's no button, instead of it
    bool canMessage = false;
    QString tip;
};

// The challenges corner, in the lobby window's top right under the header's
// Challenges button. Live cards (after a match, what was missed; the
// challenges waiting for an answer; the one this PC sent, then how it went)
// show on their own, and the log under them when the button opens it.
class ChallengeTray : public QWidget
{
    Q_OBJECT

public:
    explicit ChallengeTray(QWidget* parent = nullptr);

    // Clicks on it don't count as clicks outside the tray.
    void setAnchor(QWidget* anchor);
    // Off: the tray stays hidden whatever it holds.
    void setAvailable(bool available);

    void setIncoming(const QList<ChallengeCardView>& views);
    void setOutgoing(const OutgoingChallengeView& view);
    // How the sent challenge went, for a few seconds. icon is the badge's.
    void showOutcome(const QString& title, const QString& detail, LobbyTheme::Role role, const QString& icon);
    void clearOutcome();
    // After a match, who challenged this PC while it played. An empty title hides it.
    void setSummary(const QString& title, const QString& names);
    void setLogRows(const QList<ChallengeLogRowView>& rows);

    void setLogOpen(bool open);
    bool isLogOpen() const { return m_logOpen; }
    bool hasLiveCards() const;

    // The tray's top right corner goes at topRight, no wider or taller than given.
    void place(const QPoint& topRight, int maxWidth, int maxHeight);

    // "just now", "4 min ago", then the time of day.
    static QString relativeTime(qint64 timeMs);

signals:
    void acceptClicked(quint64 challengeId);
    void declineClicked(quint64 challengeId);
    void cancelClicked();
    void summaryViewClicked();
    void summaryDismissed();
    void actionClicked(quint64 key);
    void messageClicked(quint64 key);
    void clearClicked();
    // The log closed by itself: a click outside the tray.
    void logClosed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void relayout();
    void refreshVisibility();

    QWidget* m_anchor = nullptr;
    bool m_available = false;
    bool m_logOpen = false;
    QPoint m_topRight;
    int m_maxWidth = 0;
    int m_maxHeight = 0;

    QScrollArea* m_cardsArea = nullptr;
    QWidget* m_cardsBody = nullptr;
    QWidget* m_summaryCard = nullptr;
    QList<ChallengeCard*> m_incoming;
    QWidget* m_outgoingCard = nullptr;
    bool m_outgoingShown = false;
    QWidget* m_outcomeCard = nullptr;
    int m_outcomeGeneration = 0;
    bool m_outcomeShown = false;
    bool m_summaryShown = false;

    QFrame* m_logPanel = nullptr;
    QScrollArea* m_rowsArea = nullptr;
    QWidget* m_rowsBody = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QList<QWidget*> m_rows;
    QLabel* m_empty = nullptr;
    QWidget* m_footer = nullptr;
    QTimer* m_clock = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYCHALLENGETRAY_HPP
