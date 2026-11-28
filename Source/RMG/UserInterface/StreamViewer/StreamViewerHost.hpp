/*
 * The main window's side of the live replay viewer windows: it starts one
 * process per stream, forwards what the lobby connection receives for that
 * match down a local pipe, and hears back how the stream is doing.
 */
#ifndef STREAMVIEWERHOST_HPP
#define STREAMVIEWERHOST_HPP

#ifdef NETPLAY

#include "StreamViewerProtocol.hpp"

#include <QObject>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>

class QLocalServer;
class QLocalSocket;
class QProcess;
class QTimer;

namespace UserInterface
{

class StreamViewerHost : public QObject
{
    Q_OBJECT

public:
    struct StreamInfo
    {
        quint64 matchId = 0;
        QString title;   // "Alice vs Bob"
        QString game;
        StreamViewer::State state = StreamViewer::State::Connecting;
        bool muted = false;
    };

    explicit StreamViewerHost(QObject* parent = nullptr);
    // Closes every window still open; ones that don't go in time are killed.
    ~StreamViewerHost() override;

    bool has(quint64 matchId) const;
    int count() const { return int(m_streams.size()); }
    // In the order they were opened.
    QList<StreamInfo> streams() const;
    // A window playing sound, as far as it has said.
    bool anyAudible() const;

    // Starts a window for a match. romPath is the local ROM matching the
    // match's. False, with why, when it can't.
    bool open(quint64 matchId, const QString& title, const QString& game, const QString& romPath,
              bool startMuted, QString* error);

    // What the lobby connection receives for a match with a window. Stream
    // messages from before its SPECTATE_BEGIN are an earlier watch's: dropped.
    void began(quint64 matchId, const QByteArray& manifest);
    void data(quint64 matchId, const QByteArray& bytes, int liveFrame, qint64 offset);
    void keyframe(quint64 matchId, int frame, const QByteArray& savestate);
    void viewers(quint64 matchId, int viewerCount);
    void chat(quint64 matchId, const QString& name, const QString& message);
    // The stream is over: the window says so briefly, then closes itself.
    void ended(quint64 matchId, const QString& reason);

    // Closes a window now.
    void close(quint64 matchId);
    void closeAll();
    // Brings a window to the front.
    void raise(quint64 matchId);
    void setMuted(quint64 matchId, bool muted);

signals:
    // The list, a stream's state or its sound changed.
    void streamsChanged();
    // A window is gone. failure is why it stopped, when there's something to
    // tell the user (it's shown once, in the lobby); empty when it just closed.
    void streamClosed(quint64 matchId, const QString& failure);
    // A line typed into a window's in-game chat.
    void chatSendRequested(quint64 matchId, const QString& text);

private:
    struct Stream
    {
        StreamInfo info;
        QByteArray token;
        QString romPath;
        bool startMuted = false;
        QProcess* process = nullptr;
        QPointer<QLocalSocket> socket;
        StreamViewer::Reader reader;
        bool ready = false;
        bool armed = false;   // its SPECTATE_BEGIN arrived
        bool closing = false; // asked to close; killed if it doesn't
        QList<QByteArray> pending; // stream messages for before it's ready
        QString failure;
        QTimer* deadline = nullptr; // start, or close, taking too long
        QString deadlineFailure;    // what to say if it runs out
    };

    Stream* find(quint64 matchId);
    const Stream* find(quint64 matchId) const;
    Stream* findBySocket(QLocalSocket* socket);
    bool listen(QString* error);
    void send(Stream& stream, StreamViewer::Message type, const QByteArray& fields = QByteArray());
    void sendStream(Stream& stream, StreamViewer::Message type, const QByteArray& fields);
    void onNewConnection();
    void onSocketReadyRead(QLocalSocket* socket);
    void handle(Stream& stream, StreamViewer::Message type, const QByteArray& fields);
    void onProcessFinished(QProcess* process);
    void startDeadline(Stream& stream, int ms, const QString& failure);
    void remove(quint64 matchId);

    QLocalServer* m_server = nullptr;
    QList<Stream*> m_streams;
    QHash<QLocalSocket*, StreamViewer::Reader> m_unbound; // connected, no Hello yet
#ifdef _WIN32
    void* m_job = nullptr; // closes every window if this process dies
#endif
};

} // namespace UserInterface

#endif // NETPLAY
#endif // STREAMVIEWERHOST_HPP
