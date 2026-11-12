#ifndef LOBBYBROWSEPANELS_HPP
#define LOBBYBROWSEPANELS_HPP

#ifdef NETPLAY

#include "LobbyWidgets.hpp"

#include <QHash>
#include <QList>
#include <QPoint>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

// What one running match looks like to this client. The dialog fills these in
// from the room list; the gallery only draws them and reports clicks.
struct LiveMatchView
{
    struct Player
    {
        QString name;
        QString country;    // empty when anonymous or unknown
        QString connection; // "wifi", "lan", ... or empty when unknown
    };

    quint64 roomId = 0;
    quint64 matchId = 0;      // 0 unless a live replay is streaming
    QString game;
    QString roomName;
    QString host;
    QString coverPath;
    qint64 startedAtMs = 0;
    int viewers = 0;
    int openSeats = 0;        // seats a late joiner may take
    int seatsNeeded = 1;      // seats this client's players take together
    bool hasRom = true;
    bool sameGame = false;    // matches the game selected in the picker
    bool watching = false;    // this client is watching this match
    bool watchingOther = false; // ...or a different one
    bool inRoom = false;      // this client is seated in a room
    QList<Player> players;
    // The host's game options, named for this client, and what they do.
    QStringList gameOptions;
    QString gameOptionsTip;
};

// What one joinable room looks like to this client.
struct RoomRowView
{
    quint64 roomId = 0;
    quint64 hostId = 0;
    QString name;
    QString host;
    QString hostCountry;
    QString game;
    int players = 0;
    int maxPlayers = 0;
    int pingMs = -1;
    bool pingMeasured = false;
    bool hasPassword = false;
    bool inGame = false;
    bool mine = false;
    bool hasRom = true;
    bool sameGame = false;
    int seatsNeeded = 1;      // seats this client's players take together
    bool anyWifi = false;     // someone seated is on Wi-Fi
    // Who the room takes (0 / false: anyone), and whether this PC is one of them.
    int maxPing = 0;
    bool wiredOnly = false;
    enum class Refusal { None, WiredOnly, PingTooHigh };
    Refusal refusal = Refusal::None;
    // The host's game options, named for this client, and what they do.
    QStringList gameOptions;
    QString gameOptionsTip;
};

class LiveMatchBase;
class LiveMatchRow;
class FeaturedMatch;

// The Live list: every running match as a compact row, biggest audience first,
// with the most watched one featured above it when there's room. A row opens
// for the details. Rows are updated in place, so the list keeps its scroll
// position as matches come and go.
class LiveGallery : public LobbyCard
{
    Q_OBJECT

public:
    explicit LiveGallery(QWidget* parent = nullptr);

    void setMatches(const QList<LiveMatchView>& matches);
    void setThisGameAvailable(bool available);
    void setOffline(bool offline);
    // Ticks the durations. The room list only refreshes on events.
    void updateDurations();

signals:
    void watchRequested(quint64 roomId);
    void stopWatchingRequested();
    void joinRequested(quint64 roomId);
    void contextMenuRequested(quint64 roomId, const QPoint& globalPos);
    // A right-click on a player's name.
    void playerMenuRequested(const QString& name, const QPoint& globalPos);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class Sort { MostWatched, Newest, Longest };

    bool wideEnoughToFeature() const;
    void fitHeader();
    void rebuild();
    void setExpandedRoom(quint64 roomId);
    // Shows the note under a list short enough to leave room for it.
    void updateNote();
    QList<LiveMatchView> visibleMatches() const;
    void connectMatch(LiveMatchBase* match);

    QList<LiveMatchView> m_matches;
    QList<quint64> m_order;
    QHash<quint64, LiveMatchRow*> m_rows;
    FeaturedMatch* m_featured = nullptr;
    QWidget* m_featuredHolder = nullptr;
    quint64 m_featuredRoomId = 0;
    quint64 m_expandedRoomId = 0;
    bool m_builtWide = false;
    bool m_thisGameOnly = false;
    bool m_offline = false;

    QLabel* m_countLabel = nullptr;
    QPushButton* m_allChip = nullptr;
    QPushButton* m_thisGameChip = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_list = nullptr;
    QVBoxLayout* m_listLayout = nullptr;
    QWidget* m_note = nullptr;
    QLabel* m_noteTitle = nullptr;
    QLabel* m_noteBody = nullptr;
};

class RoomRow;

// The open rooms, one three-line row each.
class RoomsColumn : public LobbyCard
{
    Q_OBJECT

public:
    explicit RoomsColumn(QWidget* parent = nullptr);

    void setRooms(const QList<RoomRowView>& rooms);
    void setThisGameAvailable(bool available);
    void setOffline(bool offline);

signals:
    void joinRequested(quint64 roomId);
    void returnRequested();
    // A row was clicked; the dialog measures the ping to its host.
    void hostProbeRequested(quint64 hostId);
    void contextMenuRequested(quint64 roomId, const QPoint& globalPos);
    // A right-click on a host's name in a row.
    void playerMenuRequested(const QString& name, const QPoint& globalPos);

private:
    enum class Sort { Default, Ping, Players, Name };

    void rebuild();
    void showFilterMenu();
    void selectRow(quint64 roomId);
    bool filtering() const;
    void saveFilters() const;

    QList<RoomRowView> m_rooms;
    QHash<quint64, RoomRow*> m_rows;
    quint64 m_selectedRoomId = 0;
    bool m_thisGameOnly = false;
    bool m_thisGameAvailable = false;
    bool m_offline = false;
    Sort m_sort = Sort::Default;
    // The filters, kept between sessions. m_maxPing 0 shows every ping.
    bool m_inLibraryOnly = false;
    bool m_hideFull = false;
    bool m_hidePassword = false;
    bool m_hideWifi = false;
    int m_maxPing = 0;

    QLabel* m_countLabel = nullptr;
    IconButton* m_filterButton = nullptr;
    IconButton* m_searchButton = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_list = nullptr;
    QVBoxLayout* m_listLayout = nullptr;
    QWidget* m_note = nullptr;
    QLabel* m_noteTitle = nullptr;
    QLabel* m_noteBody = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYBROWSEPANELS_HPP
