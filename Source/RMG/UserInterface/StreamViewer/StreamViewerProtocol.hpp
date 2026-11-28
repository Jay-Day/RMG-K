/*
 * The pipe between the main window and a live replay viewer window: another
 * copy of RMG-K started with --stream-viewer, which plays one stream the main
 * window's lobby connection receives for it. mupen64plus-core runs one game
 * per process, so each extra stream needs a process of its own.
 */
#ifndef STREAMVIEWERPROTOCOL_HPP
#define STREAMVIEWERPROTOCOL_HPP

#ifdef NETPLAY

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace StreamViewer
{

// Command-line options of the viewer process. Hidden from --help.
inline constexpr char kServerOption[] = "stream-viewer";
inline constexpr char kTokenOption[]  = "stream-token";

// Windows a user can have open besides the main window.
inline constexpr int kMaxViewerWindows = 4;

// Each message is a little-endian u32 length, then that many bytes: a u8
// type and its fields, written with QDataStream (Qt_6_0).
enum class Message : quint8
{
    // viewer → main window
    Hello    = 1,  // QByteArray token: which stream this connection is
    Ready    = 2,  // the window is up; stream messages may follow
    State    = 3,  // qint32 State
    Muted    = 4,  // bool: the window's sound was switched off or on
    ChatSend = 5,  // QString: a line typed into the game's chat
    Failed   = 6,  // QString: why watching stopped, for the lobby to show once

    // main window → viewer
    Init     = 32, // quint64 matchId, QString title, QString game, QString romPath, bool muted
    Manifest = 33, // QByteArray: SPECTATE_BEGIN's pre-match settings
    Keyframe = 34, // qint32 frame, QByteArray savestate
    Data     = 35, // qint32 liveFrame, qint64 offset, QByteArray krec bytes
    Viewers  = 36, // qint32 audience
    Chat     = 37, // QString name, QString message
    Ended    = 38, // QString reason: the stream ended; the window says so, then closes
    Raise    = 39, // bring the window to the front
    SetMuted = 40, // bool
    Close    = 41, // close now (the main window quits, or a match of its own starts)
};

// A stream's progress, as the lobby's Watching strip shows it.
enum class State : qint32
{
    Connecting = 0, // waiting for the match's start (and the window's game)
    CatchingUp = 1, // fast-forwarding to the live edge, or buffering
    Live       = 2,
    Ended      = 3,
};

// A keyframe is up to 24 MiB; nothing else comes close.
inline constexpr quint32 kMaxMessageBytes = 40u * 1024u * 1024u;

// One message, ready to write.
QByteArray encode(Message type, const QByteArray& fields = QByteArray());

// Builds the fields of a message, and reads them back (false when they're
// short or malformed).
template <typename... Args>
QByteArray fields(const Args&... args);
template <typename... Args>
bool read(const QByteArray& fields, Args&... args);

// Collects bytes off the pipe and hands back whole messages.
class Reader
{
public:
    void append(const QByteArray& bytes) { m_buffer.append(bytes); }
    // False once nothing whole is left. A message over the size limit makes
    // the stream unusable: failed() turns true and nothing more comes out.
    bool next(Message& type, QByteArray& fields);
    bool failed() const { return m_failed; }

private:
    QByteArray m_buffer;
    bool m_failed = false;
};

} // namespace StreamViewer

#include <QDataStream>
#include <QIODevice>

template <typename... Args>
QByteArray StreamViewer::fields(const Args&... args)
{
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    (stream << ... << args);
    return out;
}

template <typename... Args>
bool StreamViewer::read(const QByteArray& fields, Args&... args)
{
    QDataStream stream(fields);
    stream.setVersion(QDataStream::Qt_6_0);
    (stream >> ... >> args);
    return stream.status() == QDataStream::Ok;
}

#endif // NETPLAY
#endif // STREAMVIEWERPROTOCOL_HPP
