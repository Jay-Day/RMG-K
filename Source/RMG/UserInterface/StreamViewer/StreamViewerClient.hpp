/*
 * A live replay viewer window's side of the pipe to the main window (see
 * StreamViewerProtocol.hpp). The main window owns the lobby connection; this
 * hears the one stream it forwards here, and reports back.
 */
#ifndef STREAMVIEWERCLIENT_HPP
#define STREAMVIEWERCLIENT_HPP

#ifdef NETPLAY

#include "StreamViewerProtocol.hpp"

#include <QObject>
#include <QString>

class QLocalSocket;
class QTimer;

namespace UserInterface
{

class StreamViewerClient : public QObject
{
    Q_OBJECT

public:
    StreamViewerClient(const QString& serverName, const QByteArray& token, QObject* parent = nullptr);

    // Connects and says hello; lost() if the main window can't be reached.
    void start();

    void sendReady();
    void sendState(StreamViewer::State state);
    void sendMuted(bool muted);
    void sendChat(const QString& text);
    // Why watching stopped, written out before the window closes.
    void sendFailed(const QString& reason);

signals:
    void init(quint64 matchId, const QString& title, const QString& game, const QString& romPath, bool startMuted);
    void manifest(const QByteArray& manifest);
    void keyframe(int frame, const QByteArray& savestate);
    void data(const QByteArray& bytes, int liveFrame, qint64 offset);
    void viewers(int viewerCount);
    void chat(const QString& name, const QString& message);
    void ended(const QString& reason);
    void raiseRequested();
    void muteRequested(bool muted);
    void closeRequested();
    // The main window is gone (it quit or crashed), or never answered.
    void lost();

private:
    void send(StreamViewer::Message type, const QByteArray& fields = QByteArray());
    void onReadyRead();
    void handle(StreamViewer::Message type, const QByteArray& fields);

    QString m_serverName;
    QByteArray m_token;
    QLocalSocket* m_socket = nullptr;
    QTimer* m_connectTimeout = nullptr;
    StreamViewer::Reader m_reader;
    bool m_initialized = false;
    bool m_lost = false;
};

} // namespace UserInterface

#endif // NETPLAY
#endif // STREAMVIEWERCLIENT_HPP
