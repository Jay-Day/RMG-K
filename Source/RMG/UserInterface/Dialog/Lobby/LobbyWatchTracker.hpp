#ifndef LOBBYWATCHTRACKER_HPP
#define LOBBYWATCHTRACKER_HPP

#ifdef NETPLAY

#include "UserInterface/StreamViewer/StreamViewerProtocol.hpp"

#include <QFrame>
#include <QHash>
#include <QList>

class QHBoxLayout;

namespace UserInterface
{
namespace Dialog
{

class LobbyButton;
class WatchEntry;

// One live replay this PC is watching, as the Watching strip shows it.
struct WatchEntryView
{
    quint64 matchId = 0;
    QString title;          // who's playing: "Alice vs Bob"
    QString game;
    StreamViewer::State state = StreamViewer::State::Connecting;
    bool muted = false;
    bool mainWindow = false; // watched in the main window, not a window of its own
    bool selected = false;   // the Spectators chat is this stream's
};

// Everything being watched, in one strip under the top bar: the main
// window's watch and each viewer window, with their state, sound and a way to
// close them. Hidden while nothing is watched.
class WatchTracker : public QFrame
{
    Q_OBJECT

public:
    explicit WatchTracker(QWidget* parent = nullptr);

    void setEntries(const QList<WatchEntryView>& entries);

signals:
    // A click on an entry: bring its window to the front, and chat with its viewers.
    void entryClicked(quint64 matchId);
    void muteToggled(quint64 matchId, bool muted);
    void closeClicked(quint64 matchId);
    void closeAllClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QHBoxLayout* m_entries = nullptr;
    QHash<quint64, WatchEntry*> m_entryWidgets;
    LobbyButton* m_closeAll = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYWATCHTRACKER_HPP
