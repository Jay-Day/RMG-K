#ifdef NETPLAY

#include "StreamViewerHost.hpp"

#include <RMG-Core/Core.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace UserInterface;
using namespace StreamViewer;

namespace
{
    // A window loads the core and its plugins before it says it's ready.
    constexpr int kStartTimeoutMs = 30'000;
    // Long enough for a window to stop its game and say why it stopped.
    constexpr int kCloseTimeoutMs = 5'000;
    // An ended stream's window shows that for a few seconds before closing.
    constexpr int kEndedTimeoutMs = 15'000;

    QByteArray randomHex(int bytes)
    {
        QByteArray raw(bytes, Qt::Uninitialized);
        for (int i = 0; i < bytes; ++i)
            raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
        return raw.toHex();
    }
} // namespace

StreamViewerHost::StreamViewerHost(QObject* parent)
    : QObject(parent)
{
}

StreamViewerHost::~StreamViewerHost()
{
    // Ask every window at once, then wait for them together, so quitting
    // with several open takes as long as the slowest one, not their sum.
    for (Stream* stream : m_streams)
    {
        if (stream->socket && stream->socket->state() == QLocalSocket::ConnectedState)
        {
            stream->socket->write(encode(Message::Close));
            stream->socket->waitForBytesWritten(500);
        }
    }
    QElapsedTimer waited;
    waited.start();
    for (Stream* stream : m_streams)
    {
        if (stream->process == nullptr)
            continue;
        disconnect(stream->process, nullptr, this, nullptr);
        const int left = std::max(0, 3000 - int(waited.elapsed()));
        if (stream->process->state() != QProcess::NotRunning && !stream->process->waitForFinished(left))
        {
            stream->process->kill();
            stream->process->waitForFinished(1000);
        }
    }
    for (Stream* stream : m_streams)
    {
        if (stream->socket)
            disconnect(stream->socket, nullptr, this, nullptr);
        delete stream;
    }
    m_streams.clear();
#ifdef _WIN32
    if (m_job != nullptr)
        CloseHandle(static_cast<HANDLE>(m_job));
#endif
}

bool StreamViewerHost::has(quint64 matchId) const
{
    return find(matchId) != nullptr;
}

QList<StreamViewerHost::StreamInfo> StreamViewerHost::streams() const
{
    QList<StreamInfo> out;
    for (const Stream* stream : m_streams)
        out.append(stream->info);
    return out;
}

bool StreamViewerHost::anyAudible() const
{
    for (const Stream* stream : m_streams)
    {
        if (!stream->info.muted && stream->info.state != State::Ended)
            return true;
    }
    return false;
}

StreamViewerHost::Stream* StreamViewerHost::find(quint64 matchId)
{
    for (Stream* stream : m_streams)
    {
        if (stream->info.matchId == matchId)
            return stream;
    }
    return nullptr;
}

const StreamViewerHost::Stream* StreamViewerHost::find(quint64 matchId) const
{
    for (const Stream* stream : m_streams)
    {
        if (stream->info.matchId == matchId)
            return stream;
    }
    return nullptr;
}

StreamViewerHost::Stream* StreamViewerHost::findBySocket(QLocalSocket* socket)
{
    for (Stream* stream : m_streams)
    {
        if (stream->socket == socket)
            return stream;
    }
    return nullptr;
}

bool StreamViewerHost::listen(QString* error)
{
    if (m_server != nullptr && m_server->isListening())
        return true;
    if (m_server == nullptr)
    {
        m_server = new QLocalServer(this);
        // Only this user's processes may connect.
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        connect(m_server, &QLocalServer::newConnection, this, &StreamViewerHost::onNewConnection);
    }
    const QString name = QStringLiteral("rmgk-streams-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QString::fromLatin1(randomHex(8)));
    if (!m_server->listen(name))
    {
        if (error != nullptr)
            *error = tr("Couldn't open a window for the live replay: %1").arg(m_server->errorString());
        return false;
    }
    return true;
}

bool StreamViewerHost::open(quint64 matchId, const QString& title, const QString& game, const QString& romPath,
                            bool startMuted, QString* error)
{
    if (has(matchId))
        return true;
    if (count() >= kMaxViewerWindows)
    {
        if (error != nullptr)
            *error = tr("You can watch up to %1 live replays in windows of their own at once.").arg(kMaxViewerWindows);
        return false;
    }
    if (!listen(error))
        return false;

    auto* stream = new Stream;
    stream->info.matchId = matchId;
    stream->info.title = title;
    stream->info.game = game;
    stream->info.muted = startMuted;
    stream->token = randomHex(16);
    stream->romPath = romPath;
    stream->startMuted = startMuted;

    auto* process = new QProcess(this);
    process->setProgram(QCoreApplication::applicationFilePath());
    QStringList args = {
        QStringLiteral("--") + QLatin1String(kServerOption), m_server->serverName(),
        QStringLiteral("--") + QLatin1String(kTokenOption), QString::fromLatin1(stream->token),
    };
    // A test run with debug messages gets them from the windows too.
    const QStringList ownArgs = QCoreApplication::arguments();
    if (ownArgs.contains(QStringLiteral("-d")) || ownArgs.contains(QStringLiteral("--debug-messages")))
        args << QStringLiteral("-d");
    process->setArguments(args);
    // Set before the window's first line runs: it never writes the user's files.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(CoreReadOnlyUserFilesVariable), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    // Its output goes where ours does; a pipe nobody reads would fill up and
    // stall it.
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(process, &QProcess::finished, this, [this, process]() { onProcessFinished(process); });

    process->start();
    if (!process->waitForStarted(10'000))
    {
        if (error != nullptr)
            *error = tr("Couldn't open a window for the live replay: %1").arg(process->errorString());
        delete process;
        delete stream;
        return false;
    }
    stream->process = process;

#ifdef _WIN32
    // The pipe closing ends a window when this process quits or crashes; the
    // job also ends one that hangs.
    if (m_job == nullptr)
    {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                m_job = job;
            else
                CloseHandle(job);
        }
    }
    if (m_job != nullptr)
    {
        HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process->processId()));
        if (handle != nullptr)
        {
            AssignProcessToJobObject(static_cast<HANDLE>(m_job), handle);
            CloseHandle(handle);
        }
    }
#endif

    m_streams.append(stream);
    startDeadline(*stream, kStartTimeoutMs, tr("Its window didn't start."));
    emit streamsChanged();
    return true;
}

void StreamViewerHost::startDeadline(Stream& stream, int ms, const QString& failure)
{
    if (stream.deadline == nullptr)
    {
        stream.deadline = new QTimer(this);
        stream.deadline->setSingleShot(true);
        const quint64 matchId = stream.info.matchId;
        connect(stream.deadline, &QTimer::timeout, this, [this, matchId]() {
            Stream* s = find(matchId);
            if (s == nullptr)
                return;
            if (s->failure.isEmpty())
                s->failure = s->deadlineFailure;
            if (s->process != nullptr && s->process->state() != QProcess::NotRunning)
                s->process->kill(); // finished cleans up
            else
                onProcessFinished(s->process);
        });
    }
    stream.deadlineFailure = failure;
    stream.deadline->start(ms);
}

void StreamViewerHost::send(Stream& stream, Message type, const QByteArray& fields)
{
    if (stream.socket && stream.socket->state() == QLocalSocket::ConnectedState)
        stream.socket->write(encode(type, fields));
}

void StreamViewerHost::sendStream(Stream& stream, Message type, const QByteArray& fields)
{
    if (stream.ready)
        send(stream, type, fields);
    else
        stream.pending.append(encode(type, fields));
}

void StreamViewerHost::onNewConnection()
{
    while (m_server->hasPendingConnections())
    {
        QLocalSocket* socket = m_server->nextPendingConnection();
        m_unbound.insert(socket, Reader());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onSocketReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_unbound.remove(socket);
            if (Stream* stream = findBySocket(socket))
            {
                stream->socket = nullptr;
                // The window is on its way out; make sure it goes.
                if (stream->process != nullptr && stream->process->state() != QProcess::NotRunning)
                    startDeadline(*stream, kCloseTimeoutMs, QString());
            }
            socket->deleteLater();
        });
    }
}

void StreamViewerHost::onSocketReadyRead(QLocalSocket* socket)
{
    const QByteArray bytes = socket->readAll();
    Stream* stream = findBySocket(socket);
    if (stream == nullptr)
    {
        auto it = m_unbound.find(socket);
        if (it == m_unbound.end())
            return;
        it->append(bytes);
        Message type;
        QByteArray hello;
        if (!it->next(type, hello))
        {
            if (it->failed())
                socket->abort();
            return;
        }
        QByteArray token;
        if (type == Message::Hello && read(hello, token))
        {
            for (Stream* candidate : m_streams)
            {
                if (!candidate->socket && candidate->token == token)
                {
                    stream = candidate;
                    break;
                }
            }
        }
        if (stream == nullptr)
        {
            // Not one of ours.
            m_unbound.erase(it);
            socket->abort();
            return;
        }
        stream->reader = *it;
        m_unbound.erase(it);
        stream->socket = socket;
        send(*stream, Message::Init, fields(stream->info.matchId, stream->info.title, stream->info.game,
                                            stream->romPath, stream->startMuted));
    }
    else
    {
        stream->reader.append(bytes);
    }

    const quint64 matchId = stream->info.matchId;
    Message type;
    QByteArray body;
    while (find(matchId) == stream && stream->reader.next(type, body))
        handle(*stream, type, body);
    if (find(matchId) == stream && stream->reader.failed())
        close(matchId);
}

void StreamViewerHost::handle(Stream& stream, Message type, const QByteArray& body)
{
    switch (type)
    {
    case Message::Ready:
    {
        stream.ready = true;
        if (stream.deadline != nullptr && !stream.closing)
            stream.deadline->stop();
        for (const QByteArray& message : stream.pending)
            stream.socket->write(message);
        stream.pending.clear();
        break;
    }
    case Message::State:
    {
        qint32 state = 0;
        if (read(body, state) && state >= qint32(State::Connecting) && state <= qint32(State::Ended))
        {
            stream.info.state = State(state);
            emit streamsChanged();
        }
        break;
    }
    case Message::Muted:
    {
        bool muted = false;
        if (read(body, muted) && muted != stream.info.muted)
        {
            stream.info.muted = muted;
            emit streamsChanged();
        }
        break;
    }
    case Message::ChatSend:
    {
        QString text;
        if (read(body, text) && !text.trimmed().isEmpty())
            emit chatSendRequested(stream.info.matchId, text.trimmed().left(500));
        break;
    }
    case Message::Failed:
    {
        QString reason;
        if (read(body, reason) && stream.failure.isEmpty())
            stream.failure = reason.left(500);
        break;
    }
    default:
        break;
    }
}

void StreamViewerHost::began(quint64 matchId, const QByteArray& manifest)
{
    Stream* stream = find(matchId);
    if (stream == nullptr || stream->armed || stream->closing)
        return;
    stream->armed = true;
    // The window applies it: cheats, saves and game options are per process.
    sendStream(*stream, Message::Manifest, fields(manifest));
}

void StreamViewerHost::data(quint64 matchId, const QByteArray& bytes, int liveFrame, qint64 offset)
{
    Stream* stream = find(matchId);
    if (stream == nullptr || !stream->armed || stream->closing)
        return;
    sendStream(*stream, Message::Data, fields(qint32(liveFrame), qint64(offset), bytes));
}

void StreamViewerHost::keyframe(quint64 matchId, int frame, const QByteArray& savestate)
{
    Stream* stream = find(matchId);
    if (stream == nullptr || !stream->armed || stream->closing)
        return;
    sendStream(*stream, Message::Keyframe, fields(qint32(frame), savestate));
}

void StreamViewerHost::viewers(quint64 matchId, int viewerCount)
{
    if (Stream* stream = find(matchId); stream != nullptr && !stream->closing)
        sendStream(*stream, Message::Viewers, fields(qint32(viewerCount)));
}

void StreamViewerHost::chat(quint64 matchId, const QString& name, const QString& message)
{
    if (Stream* stream = find(matchId); stream != nullptr && !stream->closing)
        sendStream(*stream, Message::Chat, fields(name, message));
}

void StreamViewerHost::ended(quint64 matchId, const QString& reason)
{
    Stream* stream = find(matchId);
    if (stream == nullptr || stream->closing)
        return;
    if (!stream->ready)
    {
        // Nothing on screen yet to end.
        close(matchId);
        return;
    }
    stream->info.state = State::Ended;
    send(*stream, Message::Ended, fields(reason));
    startDeadline(*stream, kEndedTimeoutMs, QString());
    emit streamsChanged();
}

void StreamViewerHost::close(quint64 matchId)
{
    Stream* stream = find(matchId);
    if (stream == nullptr || stream->closing)
        return;
    stream->closing = true;
    stream->pending.clear();
    if (stream->socket && stream->socket->state() == QLocalSocket::ConnectedState)
    {
        send(*stream, Message::Close);
        startDeadline(*stream, kCloseTimeoutMs, QString());
    }
    else if (stream->process != nullptr && stream->process->state() != QProcess::NotRunning)
    {
        stream->process->kill();
    }
    else
    {
        onProcessFinished(stream->process);
    }
}

void StreamViewerHost::closeAll()
{
    QList<quint64> ids;
    for (const Stream* stream : m_streams)
        ids.append(stream->info.matchId);
    for (quint64 id : ids)
        close(id);
}

void StreamViewerHost::raise(quint64 matchId)
{
    Stream* stream = find(matchId);
    if (stream == nullptr)
        return;
#ifdef _WIN32
    // Windows lets only the foreground process hand the foreground on, and
    // that's us while the lobby is being clicked.
    if (stream->process != nullptr)
        AllowSetForegroundWindow(DWORD(stream->process->processId()));
#endif
    send(*stream, Message::Raise);
}

void StreamViewerHost::setMuted(quint64 matchId, bool muted)
{
    Stream* stream = find(matchId);
    if (stream == nullptr)
        return;
    send(*stream, Message::SetMuted, fields(muted));
    if (stream->info.muted != muted)
    {
        // The window confirms; until then the switch shows what was asked.
        stream->info.muted = muted;
        emit streamsChanged();
    }
}

void StreamViewerHost::onProcessFinished(QProcess* process)
{
    Stream* stream = nullptr;
    for (Stream* candidate : m_streams)
    {
        if (candidate->process == process)
        {
            stream = candidate;
            break;
        }
    }
    if (stream == nullptr)
        return;
    const quint64 matchId = stream->info.matchId;
    QString failure = stream->failure;
    const bool crashed = process != nullptr &&
        (process->exitStatus() == QProcess::CrashExit || process->exitCode() != 0);
    if (failure.isEmpty() && crashed && !stream->closing && stream->info.state != State::Ended)
        failure = tr("Its window closed unexpectedly.");
    remove(matchId);
    emit streamClosed(matchId, failure);
    emit streamsChanged();
}

void StreamViewerHost::remove(quint64 matchId)
{
    Stream* stream = find(matchId);
    if (stream == nullptr)
        return;
    m_streams.removeOne(stream);
    if (stream->socket)
    {
        disconnect(stream->socket, nullptr, this, nullptr);
        stream->socket->abort();
        stream->socket->deleteLater();
    }
    if (stream->process != nullptr)
    {
        disconnect(stream->process, nullptr, this, nullptr);
        if (stream->process->state() != QProcess::NotRunning)
            stream->process->kill();
        stream->process->deleteLater();
    }
    if (stream->deadline != nullptr)
        stream->deadline->deleteLater();
    delete stream;
}

#endif // NETPLAY
