#ifdef NETPLAY

#include "StreamViewerClient.hpp"

#include <QLocalSocket>
#include <QTimer>

using namespace UserInterface;
using namespace StreamViewer;

namespace
{
    // The main window starts the viewer and listens before it does, so this
    // is only ever reached when something went wrong.
    constexpr int kConnectTimeoutMs = 10'000;
} // namespace

StreamViewerClient::StreamViewerClient(const QString& serverName, const QByteArray& token, QObject* parent)
    : QObject(parent), m_serverName(serverName), m_token(token)
{
}

void StreamViewerClient::start()
{
    m_socket = new QLocalSocket(this);
    m_connectTimeout = new QTimer(this);
    m_connectTimeout->setSingleShot(true);
    connect(m_connectTimeout, &QTimer::timeout, this, [this]() {
        if (!m_initialized && !m_lost)
        {
            m_lost = true;
            emit lost();
        }
    });
    connect(m_socket, &QLocalSocket::connected, this, [this]() { send(Message::Hello, fields(m_token)); });
    connect(m_socket, &QLocalSocket::readyRead, this, &StreamViewerClient::onReadyRead);
    const auto gone = [this]() {
        if (m_lost)
            return;
        m_lost = true;
        emit lost();
    };
    connect(m_socket, &QLocalSocket::disconnected, this, gone);
    connect(m_socket, &QLocalSocket::errorOccurred, this, gone);
    m_connectTimeout->start(kConnectTimeoutMs);
    m_socket->connectToServer(m_serverName);
}

void StreamViewerClient::send(Message type, const QByteArray& body)
{
    if (m_socket != nullptr && m_socket->state() == QLocalSocket::ConnectedState)
        m_socket->write(encode(type, body));
}

void StreamViewerClient::sendReady()
{
    send(Message::Ready);
}

void StreamViewerClient::sendState(State state)
{
    send(Message::State, fields(qint32(state)));
}

void StreamViewerClient::sendMuted(bool muted)
{
    send(Message::Muted, fields(muted));
}

void StreamViewerClient::sendChat(const QString& text)
{
    send(Message::ChatSend, fields(text));
}

void StreamViewerClient::sendFailed(const QString& reason)
{
    send(Message::Failed, fields(reason));
    if (m_socket != nullptr && m_socket->state() == QLocalSocket::ConnectedState)
        m_socket->waitForBytesWritten(1000);
}

void StreamViewerClient::onReadyRead()
{
    m_reader.append(m_socket->readAll());
    Message type;
    QByteArray body;
    while (!m_lost && m_reader.next(type, body))
        handle(type, body);
    if (m_reader.failed() && !m_lost)
    {
        m_lost = true;
        emit lost();
    }
}

void StreamViewerClient::handle(Message type, const QByteArray& body)
{
    // Nothing means anything before the stream is named.
    if (!m_initialized && type != Message::Init && type != Message::Close)
        return;

    switch (type)
    {
    case Message::Init:
    {
        quint64 matchId = 0;
        QString title;
        QString game;
        QString romPath;
        bool startMuted = false;
        if (m_initialized || !read(body, matchId, title, game, romPath, startMuted) || matchId == 0)
            return;
        m_initialized = true;
        m_connectTimeout->stop();
        emit init(matchId, title, game, romPath, startMuted);
        break;
    }
    case Message::Manifest:
    {
        QByteArray manifestBytes;
        if (read(body, manifestBytes))
            emit manifest(manifestBytes);
        break;
    }
    case Message::Keyframe:
    {
        qint32 frame = -1;
        QByteArray savestate;
        if (read(body, frame, savestate))
            emit keyframe(frame, savestate);
        break;
    }
    case Message::Data:
    {
        qint32 liveFrame = 0;
        qint64 offset = -1;
        QByteArray bytes;
        if (read(body, liveFrame, offset, bytes))
            emit data(bytes, liveFrame, offset);
        break;
    }
    case Message::Viewers:
    {
        qint32 count = 0;
        if (read(body, count))
            emit viewers(count);
        break;
    }
    case Message::Chat:
    {
        QString name;
        QString message;
        if (read(body, name, message))
            emit chat(name, message);
        break;
    }
    case Message::Ended:
    {
        QString reason;
        if (read(body, reason))
            emit ended(reason);
        break;
    }
    case Message::Raise:
        emit raiseRequested();
        break;
    case Message::SetMuted:
    {
        bool muted = false;
        if (read(body, muted))
            emit muteRequested(muted);
        break;
    }
    case Message::Close:
        emit closeRequested();
        break;
    default:
        break;
    }
}

#endif // NETPLAY
