/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

// Must precede anything that could pull in <windows.h>, which otherwise drags
// in the old winsock.h and collides with winsock2.h.
#ifdef _WIN32
#include <winsock2.h>
// SIO_UDP_CONNRESET lives in mswsock.h under MinGW-w64 and in mstcpip.h under
// the MSVC SDK, so pull in both and fall back to the documented control code.
#include <mswsock.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#include "LobbyClient.hpp"

#include <RMG-Core/Cheats.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/LobbyIce.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Version.hpp>

#include <QWebSocket>
#include <QUdpSocket>
#include <QAbstractSocket>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonValue>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QDateTime>
#include <QTimeZone>
#include <QCoreApplication>
#include <QtEndian>
#include <QRandomGenerator>
#include <QThread>
#include <QDebug>
#include <QSet>
#include <QEvent>
#include <QFile>
#include <QDir>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

using namespace UserInterface::Dialog;

namespace
{
    constexpr int HEARTBEAT_INTERVAL_MS  = 15'000;
    constexpr int UDP_KEEPALIVE_INTERVAL = 20'000;

    // Pinned-anchor-port rebind retry (see initiateUdpAnchor): attempt cadence
    // and how long to keep trying before surrendering the port.
    constexpr int ANCHOR_PORT_RETRY_INTERVAL_MS = 250;
    constexpr qint64 ANCHOR_PORT_RETRY_WINDOW_MS = 4'000;

    // No keyboard/mouse input for this long => report "away" on the heartbeat.
    // Presence-only; nothing functional hangs off it.
    constexpr qint64 AWAY_AFTER_MS = 5 * 60'000;

    constexpr char ANCHOR_MAGIC[4] = { 'R', 'M', 'G', 'K' };
    constexpr quint8 ANCHOR_OP_REGISTER    = 0x01;
    constexpr quint8 ANCHOR_OP_KEEPALIVE   = 0x02;
    constexpr quint8 ANCHOR_OP_PUNCH       = 0x03;
    constexpr quint8 ANCHOR_OP_PROBE       = 0x04; // client → peer: ping request
    constexpr quint8 ANCHOR_OP_PROBE_REPLY = 0x05; // peer → client: ping echo
    constexpr quint8 ANCHOR_OP_PREMATCH_MANIFEST = 0x06;
    constexpr quint8 ANCHOR_OP_PREMATCH_ACK      = 0x07;
    constexpr quint8 ANCHOR_OP_PREMATCH_FRAGMENT = 0x08;

    // Burst size for NAT punch-through. Defends against single-packet loss
    // and brief mapping-creation latency on consumer routers — NOT against
    // simultaneity failures (those need retries, which we don't do here yet).
    constexpr int ANCHOR_PUNCH_BURST = 10;

    // PROBE/PROBE_REPLY packet: [magic(4) | op(1) | senderUserId(8) | nonce(8)]
    constexpr int PROBE_PACKET_SIZE = 4 + 1 + 8 + 8;

    // Ping probes punch the same way every other path here does, and the same
    // way n02/NATLinker does on JOIN: a burst, then retries. A single packet
    // per attempt is fine on a short clean link and close to a coin flip on a
    // long or lossy one — which is exactly how the failures presented, working
    // for nearby peers and never for distant ones.
    //
    // All packets in a burst share one nonce: we only want one RTT sample and
    // one pending entry, and the peer echoes whichever ones arrive.
    constexpr int PROBE_BURST = 10;         // packets per attempt
    constexpr int PROBE_ATTEMPTS = 4;       // initial burst + 3 retries
    constexpr int PROBE_RETRY_INTERVAL_MS = 300;
    constexpr int PROBE_RETRY_TICK_MS = 100;
    // Bounded so a long session can't grow it without limit; only needs to
    // outlive the echoes of one burst.
    constexpr int PROBE_MATCHED_NONCE_CAP = 64;

    constexpr quint8 ICE_PING_OP_REQUEST = 1;
    constexpr quint8 ICE_PING_OP_REPLY   = 2;
    constexpr quint8 ICE_PING_OP_REPORT  = 3;
    constexpr int ICE_PING_PACKET_SIZE = 1 + int(sizeof(quint64));
    constexpr int ICE_PING_REPORT_SIZE = 1 + int(sizeof(quint64)) + int(sizeof(quint16));

    // How long a learned route stays trusted without fresh inbound traffic.
    // Seated peers refresh it every ~3s via the seat probe loop; 60s tolerates
    // a couple of missed cycles without trusting a mapping the peer's NAT has
    // likely since expired.
    constexpr qint64 LEARNED_ROUTE_TTL_MS = 60'000;

    bool parseEndpoint(const QString& endpoint, QHostAddress& addr, quint16& port)
    {
        const int colon = endpoint.lastIndexOf(':');
        if (colon <= 0)
            return false;
        addr = QHostAddress(endpoint.left(colon));
        port = static_cast<quint16>(endpoint.mid(colon + 1).toUInt());
        return !addr.isNull() && port != 0;
    }

    // Where the opt-in ping trace lands. Prefers RMG-K's own Logs folder;
    // falls back to a relative Logs/ so a portable install with an unwritable
    // library dir still produces a file rather than silently logging nowhere.
    std::filesystem::path lobbyPingLogDirectory()
    {
        std::error_code errorCode;
        std::filesystem::path directory = CoreGetLibraryDirectory() / "Logs";

        if (std::filesystem::is_directory(directory, errorCode) ||
            std::filesystem::create_directories(directory, errorCode))
        {
            return directory.make_preferred();
        }

        errorCode.clear();
        directory = "Logs";
        if (std::filesystem::is_directory(directory, errorCode) ||
            std::filesystem::create_directories(directory, errorCode))
        {
            return directory.make_preferred();
        }

        return std::filesystem::path();
    }

    uint64_t prematchHashBytes(const std::string& data)
    {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char byte : data)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void appendPrematchU32(std::string& data, uint32_t value)
    {
        data.push_back(static_cast<char>(value & 0xffu));
        data.push_back(static_cast<char>((value >> 8) & 0xffu));
        data.push_back(static_cast<char>((value >> 16) & 0xffu));
        data.push_back(static_cast<char>((value >> 24) & 0xffu));
    }

    void appendPrematchU64(QByteArray& data, uint64_t value)
    {
        for (int i = 0; i < 8; i++)
            data.append(static_cast<char>((value >> (i * 8)) & 0xffu));
    }

    bool readPrematchU32(const std::string& data, size_t& offset, uint32_t& value)
    {
        if (offset + sizeof(uint32_t) > data.size())
            return false;

        value = static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) |
            (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8) |
            (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24);
        offset += sizeof(uint32_t);
        return true;
    }

    bool readPrematchU64(const QByteArray& data, int offset, uint64_t& value)
    {
        if (offset < 0 || data.size() < offset + static_cast<int>(sizeof(uint64_t)))
            return false;

        value = 0;
        for (int i = 0; i < 8; i++)
            value |= static_cast<uint64_t>(static_cast<unsigned char>(data[offset + i])) << (i * 8);
        return true;
    }

    void appendPrematchU16(QByteArray& data, quint16 value)
    {
        data.append(static_cast<char>(value & 0xffu));
        data.append(static_cast<char>((value >> 8) & 0xffu));
    }

    void appendPrematchU32(QByteArray& data, quint32 value)
    {
        for (int i = 0; i < 4; ++i)
            data.append(static_cast<char>((value >> (i * 8)) & 0xffu));
    }

    bool readPrematchU16(const QByteArray& data, int offset, quint16& value)
    {
        if (offset < 0 || data.size() < offset + 2)
            return false;
        value = static_cast<quint16>(static_cast<unsigned char>(data[offset])) |
            (static_cast<quint16>(static_cast<unsigned char>(data[offset + 1])) << 8);
        return true;
    }

    bool readPrematchU32(const QByteArray& data, int offset, quint32& value)
    {
        if (offset < 0 || data.size() < offset + 4)
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value |= static_cast<quint32>(static_cast<unsigned char>(data[offset + i])) << (i * 8);
        return true;
    }

    bool buildPrematchManifest(const QString& romFile, std::string& manifest, uint64_t& manifestHash, size_t& cheatCount)
    {
        std::vector<CoreCheat> cheats;
        std::string cheatManifest;

        if (!CoreGetEnabledNetplayCheats(romFile.toStdU32String(), cheats) || !CoreSerializeNetplayCheats(cheats, cheatManifest))
            return false;

        manifest.clear();
        manifest.append("RMGKPMAN", 8);
        appendPrematchU32(manifest, 1);
        appendPrematchU32(manifest, 1);
        appendPrematchU32(manifest, static_cast<uint32_t>(cheatManifest.size()));
        manifest.append(cheatManifest);
        manifestHash = prematchHashBytes(manifest);
        cheatCount = cheats.size();
        return true;
    }

    bool applyPrematchManifest(const std::string& manifest, uint64_t& manifestHash, size_t& cheatCount)
    {
        size_t offset = 8;
        uint32_t version = 0;
        uint32_t sectionMask = 0;
        uint32_t cheatManifestLen = 0;
        std::vector<CoreCheat> cheats;

        manifestHash = prematchHashBytes(manifest);
        cheatCount = 0;

        if (manifest.size() < 8 || std::memcmp(manifest.data(), "RMGKPMAN", 8) != 0 ||
            !readPrematchU32(manifest, offset, version) ||
            !readPrematchU32(manifest, offset, sectionMask) ||
            version != 1 || (sectionMask & ~1u) != 0 ||
            !readPrematchU32(manifest, offset, cheatManifestLen) ||
            offset + cheatManifestLen != manifest.size())
        {
            return false;
        }

        const std::string cheatManifest(manifest.data() + offset, manifest.data() + offset + cheatManifestLen);
        if (!CoreDeserializeNetplayCheats(cheatManifest, cheats))
            return false;

        cheatCount = cheats.size();
        return CoreSetNetplayCheats(cheats);
    }

    QByteArray buildPrematchPacket(quint8 op, quint64 senderUserId, uint64_t manifestHash, const std::string& manifest = {})
    {
        QByteArray packet;
        packet.reserve(4 + 1 + 8 + 8 + static_cast<int>(manifest.size()));
        packet.append(ANCHOR_MAGIC, 4);
        packet.append(static_cast<char>(op));
        const quint64 senderBE = qToBigEndian(senderUserId);
        packet.append(reinterpret_cast<const char*>(&senderBE), sizeof(senderBE));
        appendPrematchU64(packet, manifestHash);
        if (!manifest.empty())
            packet.append(manifest.data(), static_cast<int>(manifest.size()));
        return packet;
    }

    bool readPrematchSenderAndHash(const QByteArray& packet, quint64& senderUserId, uint64_t& manifestHash)
    {
        if (packet.size() < 4 + 1 + 8 + 8)
            return false;

        quint64 senderBE = 0;
        std::memcpy(&senderBE, packet.constData() + 5, sizeof(senderBE));
        senderUserId = qFromBigEndian(senderBE);
        return readPrematchU64(packet, 13, manifestHash);
    }

    QList<QByteArray> fragmentPrematchPacket(const QByteArray& packet,
        quint64 senderUserId, uint64_t manifestHash)
    {
        constexpr int kPayloadBytes = 1000;
        if (packet.size() <= kPayloadBytes)
            return {packet};

        const int count = (packet.size() + kPayloadBytes - 1) / kPayloadBytes;
        if (count > 2048)
            return {};
        QList<QByteArray> fragments;
        fragments.reserve(count);
        for (int index = 0; index < count; ++index)
        {
            QByteArray fragment;
            fragment.reserve(29 + kPayloadBytes);
            fragment.append(ANCHOR_MAGIC, 4);
            fragment.append(static_cast<char>(ANCHOR_OP_PREMATCH_FRAGMENT));
            const quint64 senderBE = qToBigEndian(senderUserId);
            fragment.append(reinterpret_cast<const char*>(&senderBE), sizeof(senderBE));
            appendPrematchU64(fragment, manifestHash);
            appendPrematchU16(fragment, static_cast<quint16>(index));
            appendPrematchU16(fragment, static_cast<quint16>(count));
            appendPrematchU32(fragment, static_cast<quint32>(packet.size()));
            fragment.append(packet.mid(index * kPayloadBytes, kPayloadBytes));
            fragments.append(std::move(fragment));
        }
        return fragments;
    }


    QString detectLocalIPv4()
    {
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
        {
            const auto flags = iface.flags();
            if (!(flags & QNetworkInterface::IsUp) ||
                !(flags & QNetworkInterface::IsRunning) ||
                (flags & QNetworkInterface::IsLoopBack))
            {
                continue;
            }

            for (const QNetworkAddressEntry& entry : iface.addressEntries())
            {
                const QHostAddress address = entry.ip();
                if (address.protocol() != QAbstractSocket::IPv4Protocol ||
                    address.isLoopback())
                {
                    continue;
                }

                const quint32 ipv4 = address.toIPv4Address();
                const bool isPrivate =
                    ((ipv4 & 0xff000000u) == 0x0a000000u) ||
                    ((ipv4 & 0xfff00000u) == 0xac100000u) ||
                    ((ipv4 & 0xffff0000u) == 0xc0a80000u);
                if (isPrivate)
                    return address.toString();
            }
        }

        return QString();
    }
} // namespace

LobbyClient::LobbyClient(QObject* parent)
    : QObject(parent)
{
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected,           this, &LobbyClient::onWsConnected);
    connect(m_ws, &QWebSocket::disconnected,        this, &LobbyClient::onWsDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &LobbyClient::onWsTextMessageReceived);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_ws, &QWebSocket::errorOccurred,       this, &LobbyClient::onWsErrorOccurred);
#else
    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &LobbyClient::onWsErrorOccurred);
#endif

    m_udp = new QUdpSocket(this);
    connect(m_udp, &QUdpSocket::readyRead, this, &LobbyClient::onUdpReadyRead);

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(HEARTBEAT_INTERVAL_MS);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &LobbyClient::onHeartbeatTimer);

    m_udpKeepaliveTimer = new QTimer(this);
    m_udpKeepaliveTimer->setInterval(UDP_KEEPALIVE_INTERVAL);
    connect(m_udpKeepaliveTimer, &QTimer::timeout, this, &LobbyClient::onUdpKeepaliveTimer);

    // Always running: it's a cheap no-op with no probes in flight, and probes
    // can start from an unsolicited introduction, not just our own request.
    m_probeRetryTimer = new QTimer(this);
    m_probeRetryTimer->setInterval(PROBE_RETRY_TICK_MS);
    connect(m_probeRetryTimer, &QTimer::timeout, this, &LobbyClient::onProbeRetryTimer);
    m_probeRetryTimer->start();

    // libjuice callbacks run on its worker thread. Poll their thread-safe
    // queues here so all WebSocket signaling and ping UI stays on Qt's thread.
    m_iceTimer = new QTimer(this);
    m_iceTimer->setInterval(25);
    connect(m_iceTimer, &QTimer::timeout, this, &LobbyClient::flushIceEvents);
    m_iceTimer->start();

    // Single-shot; each failed pinned-port bind schedules the next attempt.
    m_anchorRetryTimer = new QTimer(this);
    m_anchorRetryTimer->setSingleShot(true);
    m_anchorRetryTimer->setInterval(ANCHOR_PORT_RETRY_INTERVAL_MS);
    connect(m_anchorRetryTimer, &QTimer::timeout, this, [this]() {
        // Abandon the retry if the connection went away mid-window; a fresh
        // connect starts its own window.
        if (m_state != ConnectionState::Connected)
        {
            m_anchorRetryDeadlineMs = 0;
            return;
        }
        initiateUdpAnchor();
    });

    // Watch app-wide input for the away flag. Cheap: the filter only stamps a
    // timestamp for input-type events and passes everything through.
    m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    QCoreApplication::instance()->installEventFilter(this);
}

LobbyClient::~LobbyClient()
{
    disconnectFromServer();
}

void LobbyClient::connectToServer(const QString& wsUrl, const QString& username,
                                   const QStringList& romHashes, const QString& udpAddr)
{
    if (m_state != ConnectionState::Disconnected && m_state != ConnectionState::Failed)
    {
        return;
    }

    m_pendingUsername  = username;
    m_pendingRomHashes = romHashes;
    m_pendingLocalIp   = detectLocalIPv4();
    m_selfUserId       = 0;
    m_users.clear();
    m_rooms.clear();

    qInfo() << "Rollback lobby local IPv4 detection"
            << "localIp" << (m_pendingLocalIp.isEmpty() ? QString("<none>") : m_pendingLocalIp);

    QUrl url(wsUrl);
    if (!udpAddr.isEmpty())
    {
        const int sep = udpAddr.lastIndexOf(':');
        if (sep > 0)
        {
            m_udpAnchorHost = udpAddr.left(sep);
            m_udpAnchorPort = static_cast<quint16>(udpAddr.mid(sep + 1).toUInt());
        }
        else
        {
            m_udpAnchorHost = udpAddr;
            m_udpAnchorPort = 6364;
        }
    }
    else
    {
        m_udpAnchorHost = url.host();
        m_udpAnchorPort = 6364;
    }

    setState(ConnectionState::Connecting);
    // Narrate the connect stages to the terminal (visible on Linux, DebugView
    // on Windows): this line, then "WebSocket connected", then "authenticated".
    // Whichever line is missing names the stage that stalled — a silent gap
    // after this one means TCP itself is black-holing toward this address.
    qInfo() << "Rollback lobby connecting to" << url.toString();
    m_ws->open(url);
}

void LobbyClient::disconnectFromServer()
{
    stopPingDiagnosticLog(QStringLiteral("disconnect"));
    if (m_anchorRetryTimer)
        m_anchorRetryTimer->stop();
    m_anchorRetryDeadlineMs = 0;
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    resetIceMesh();
    LobbyIce::reset();
    if (m_ws && m_ws->state() != QAbstractSocket::UnconnectedState)
    {
        m_ws->close();
    }
    // See onWsDisconnected: a socket lent to a running match must survive the
    // lobby connection; it's recycled by the next connect's pre-bind abort.
    if (m_udp && !m_anchorLent)
    {
        m_udp->close();
    }
    setState(ConnectionState::Disconnected);
}

void LobbyClient::setState(ConnectionState s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(m_state);
}

void LobbyClient::sendEnvelope(const QString& type, const QJsonObject& data, const QString& id)
{
    if (m_ws->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject env;
    env["type"] = type;
    if (!id.isEmpty())
        env["id"] = id;
    if (!data.isEmpty())
        env["data"] = data;

    const QByteArray payload = QJsonDocument(env).toJson(QJsonDocument::Compact);
    m_ws->sendTextMessage(QString::fromUtf8(payload));
}

// -------- WebSocket lifecycle --------

// Classify the transport actually carrying the lobby connection ("lan"/
// "wifi"), reported in HELLO so peers can gauge connection-quality risk
// Slippi-style. Deliberately NOT QNetworkInformation: its Windows transport
// support is written against C++/WinRT and compiled out of MinGW Qt builds
// (the backend loads but never claims the feature), and its Linux backend
// needs both a plugin the AppImage doesn't bundle and a running
// NetworkManager. QNetworkInterface avoids all of that — plain Win32 / sysfs
// underneath — and is more truthful anyway: the connected socket's local
// address names the interface the OS actually routed the lobby through, so a
// machine with ethernet and wifi both up reports the one really in use.
// Returns empty for anything unclassifiable (VPNs, virtual adapters) rather
// than guessing.
static QString detectTransportMedium(const QHostAddress& localAddress)
{
    if (localAddress.isNull())
        return QString();
    for (const QNetworkInterface& ifc : QNetworkInterface::allInterfaces())
    {
        for (const QNetworkAddressEntry& entry : ifc.addressEntries())
        {
            if (entry.ip() != localAddress)
                continue;
            switch (ifc.type())
            {
            case QNetworkInterface::Ethernet: return QStringLiteral("lan");
            case QNetworkInterface::Wifi:     return QStringLiteral("wifi");
            default:                          return QString();
            }
        }
    }
    return QString();
}

void LobbyClient::onWsConnected()
{
    qInfo() << "Rollback lobby WebSocket connected, authenticating";
    setState(ConnectionState::Authenticating);

    QJsonObject data;
    data["username"]      = m_pendingUsername;
    data["clientVersion"] = QString::fromStdString(CoreGetVersion());
    // The WebSocket is connected by the time this slot runs, so its local
    // address reflects the route the OS actually chose for lobby traffic.
    const QString transport = detectTransportMedium(m_ws->localAddress());
    if (!transport.isEmpty())
        data["connection"] = transport;
    // Ask the server to withhold our country from presence and flag us
    // anonymous, so peers show no flag at all (not even the region badge).
    // The coarse region still flows — it drives ping estimation only.
    if (CoreSettingsGetBoolValue(SettingsID::Rollback_HideLocation))
        data["anonymous"] = true;
    // Standard (non-DST) UTC offset — the server uses it to split the North
    // America country bucket into east/central/west (New York -5 h, Chicago
    // -6 h, Denver -7 h, Los Angeles -8 h). standardTimeOffset is DST-immune,
    // unlike the raw current offset which collides LA with Phoenix in summer.
    const int tzOffsetSec = QTimeZone::systemTimeZone()
        .standardTimeOffset(QDateTime::currentDateTimeUtc());
    if (tzOffsetSec != 0)
        data["tzOffset"] = tzOffsetSec;
    QJsonArray romArr;
    for (const auto& h : m_pendingRomHashes)
        romArr.append(h);
    data["romHashes"] = romArr;
    data["iceSupported"] = true;
    if (!m_pendingLocalIp.isEmpty())
        data["localIp"] = m_pendingLocalIp;

    sendEnvelope("HELLO", data, "hello-1");
}

void LobbyClient::onWsDisconnected()
{
    stopPingDiagnosticLog(QStringLiteral("connection_lost"));
    resetIceMesh();
    LobbyIce::reset();
    // Passive drops (server restart, network cut, server-side kick) never go
    // through disconnectFromServer, so the anchor socket used to stay bound —
    // and the next connect's pinned-port bind then failed against our own
    // zombie socket, with the fallback bind failing too (a failed bind leaves
    // QUdpSocket needing a reset before it can bind again). The session ran
    // anchorless from there. Tear the socket down on every disconnect path.
    // While a match borrows the socket, leave both the socket and the lent
    // flag alone: aborting would cut the game's transport out from under
    // GekkoNet, and clearing the flag would re-enable our reads mid-match
    // (stealing game packets). The match ends through its own path; the next
    // connect's pre-bind abort recycles the socket safely.
    if (m_udp && !m_anchorLent)
        m_udp->abort();
    if (m_anchorRetryTimer)
        m_anchorRetryTimer->stop();
    m_anchorRetryDeadlineMs = 0;
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    m_isModerator = false; // role is per-connection; must re-auth after reconnect
    // User ids restart when the server does — drop measurements so a recycled
    // id can't inherit another player's ping history.
    m_measuredPing.clear();
    // In-flight probe series belong to the old connection: their endpoints and
    // user ids are stale, so retrying them would burst at whoever now holds
    // those ids and emit failure notices for peers we're no longer talking to.
    m_pendingProbes.clear();
    m_probeFailStreak.clear();
    m_recentlyMatchedNonces.clear();
    // Learned routes are keyed by user id too — same recycled-id hazard.
    m_learnedRoutes.clear();
    // A socket error or HELLO_FAIL sets Failed before closing the WebSocket.
    // Keep that state so the dialog can preserve the useful error for retry.
    // connectToServer() permits retries from Failed.
    if (m_state != ConnectionState::Failed)
        setState(ConnectionState::Disconnected);
}

void LobbyClient::onWsErrorOccurred()
{
    emit connectError(m_ws->errorString());
    setState(ConnectionState::Failed);
}

void LobbyClient::onWsTextMessageReceived(const QString& msg)
{
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        qWarning() << "lobby: bad JSON" << err.errorString();
        return;
    }
    handleEnvelope(doc.object());
}

void LobbyClient::handleEnvelope(const QJsonObject& env)
{
    const QString type = env.value("type").toString();
    const QJsonObject data = env.value("data").toObject();

    if      (type == "HELLO_OK")             handleHelloOk(data);
    else if (type == "HELLO_FAIL")           handleHelloFail(data);
    else if (type == "HEARTBEAT_ACK")        handleHeartbeatAck(data);
    else if (type == "PRESENCE_FULL")        handlePresenceFull(data);
    else if (type == "PRESENCE_DELTA")       handlePresenceDelta(data);
    else if (type == "ROOM_LIST")            handleRoomList(data);
    else if (type == "ROOM_CREATED")         handleRoomCreated(data);
    else if (type == "ROOM_CREATE_FAIL")     handleRoomCreateFail(data);
    else if (type == "ROOM_STATE")           handleRoomState(data);
    else if (type == "ROOM_JOIN_OK")         handleRoomJoinOk(data);
    else if (type == "ROOM_JOIN_FAIL")       handleRoomJoinFail(data);
    else if (type == "ROOM_LEFT")            handleRoomLeft(data);
    else if (type == "CHAT_MSG")             handleChatMsg(data);
    else if (type == "CHAT_HISTORY_REPLY")   handleChatHistoryReply(data);
    else if (type == "PING_PROBE_REPLY")     handlePingProbeReply(data);
    else if (type == "PING_PROBE_INCOMING")  handlePingProbeIncoming(data);
    else if (type == "ICE_SIGNAL")           handleIceSignal(data);
    else if (type == "MATCH_BEGIN")          handleMatchBegin(data);
    else if (type == "MATCH_PEER_LEFT")      handleMatchPeerLeft(data);
    else if (type == "QUICK_MATCH_STATUS")   handleQuickMatchStatus(data);
    else if (type == "SPECTATE_BEGIN")       handleSpectateBegin(data);
    else if (type == "SPECTATE_DATA")        handleSpectateData(data);
    else if (type == "SPECTATE_KEYFRAME")    handleSpectateKeyframe(data);
    else if (type == "SPECTATE_END")         handleSpectateEnd(data);
    else if (type == "SPECTATE_FAIL")        handleSpectateFail(data);
    else if (type == "ADMIN_AUTH_OK")        handleAdminAuthOk(data);
    else if (type == "ADMIN_AUTH_FAIL")      handleAdminAuthFail(data);
    else if (type == "MOD_NOTICE")           handleModNotice(data);
    else if (type == "MOD_LIST")             handleModList(data);
    else qDebug() << "lobby: unknown message type" << type;
}

// -------- Specific handlers --------

void LobbyClient::handleHelloOk(const QJsonObject& data)
{
    m_selfUserId  = static_cast<quint64>(data.value("userId").toDouble());
    m_observedIp  = data.value("observedIp").toString();
    m_region      = data.value("region").toString();

    std::string stunHost = "stun.l.google.com";
    quint16 stunPort = 19302;
    std::vector<LobbyIceTurnServer> turnServers;
    const QJsonObject ice = data.value("ice").toObject();
    const QJsonObject stun = ice.value("stun").toObject();
    if (!stun.value("host").toString().isEmpty())
        stunHost = stun.value("host").toString().toStdString();
    if (stun.value("port").toInt() > 0 && stun.value("port").toInt() <= 65535)
        stunPort = static_cast<quint16>(stun.value("port").toInt());
    for (const QJsonValue& value : ice.value("turn").toArray())
    {
        const QJsonObject object = value.toObject();
        LobbyIceTurnServer server;
        server.host = object.value("host").toString().toStdString();
        const int port = object.value("port").toInt(3478);
        if (port < 1 || port > 65535)
            continue;
        server.port = static_cast<quint16>(port);
        server.username = object.value("username").toString().toStdString();
        server.password = object.value("password").toString().toStdString();
        if (!server.host.empty() && server.port != 0)
            turnServers.push_back(std::move(server));
    }
    if (!LobbyIce::configure(m_selfUserId, stunHost, stunPort, turnServers))
    {
        emit connectError(QStringLiteral("Failed to initialize ICE transport."));
        setState(ConnectionState::Failed);
        return;
    }
    qInfo() << "Rollback lobby authenticated" << "userId" << m_selfUserId
            << "observedIp" << m_observedIp << "region" << m_region;

    const QString udpAnchor = data.value("udpAnchor").toString();
    if (!udpAnchor.isEmpty() && udpAnchor != "TODO:6364")
    {
        const int sep = udpAnchor.lastIndexOf(':');
        if (sep > 0)
        {
            m_udpAnchorHost = udpAnchor.left(sep);
            m_udpAnchorPort = static_cast<quint16>(udpAnchor.mid(sep + 1).toUInt());
        }
    }

    // Started here rather than at connectToServer so the trace already knows
    // our user id and anchor — the fields every later line is keyed on.
    startPingDiagnosticLog();
    writePingDiagnostic(QStringLiteral("HELLO_OK"),
                        QStringLiteral("observed_ip=%1 region=%2 anchor=%3:%4")
                            .arg(m_observedIp, m_region, m_udpAnchorHost)
                            .arg(m_udpAnchorPort));

    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();
    // ICE owns the UDP sockets. The legacy lobby anchor is intentionally not
    // opened by ICE-capable clients.
}

void LobbyClient::handleHelloFail(const QJsonObject& data)
{
    const QString reason = data.value("reason").toString();
    emit helloFailed(reason);
    setState(ConnectionState::Failed);
    if (m_ws)
        m_ws->close();
}

void LobbyClient::handleHeartbeatAck(const QJsonObject& data)
{
    Q_UNUSED(data);
    // TODO: use serverTime drift if we care
}

void LobbyClient::handlePresenceFull(const QJsonObject& data)
{
    m_users.clear();
    for (const auto& v : data.value("users").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
    }
    emit presenceFull();
}

void LobbyClient::handlePresenceDelta(const QJsonObject& data)
{
    for (const auto& v : data.value("added").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
        emit userAdded(u.id);
    }
    for (const auto& v : data.value("updated").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
        emit userUpdated(u.id);
    }
    for (const auto& v : data.value("removed").toArray())
    {
        const quint64 id = static_cast<quint64>(v.toDouble());
        m_users.remove(id);
        emit userRemoved(id);
    }
}

void LobbyClient::handleRoomList(const QJsonObject& data)
{
    m_rooms.clear();
    for (const auto& v : data.value("rooms").toArray())
    {
        const auto o = v.toObject();
        LobbyRoomSummary r;
        r.id          = static_cast<quint64>(o.value("id").toDouble());
        r.name        = o.value("name").toString();
        r.hostId      = static_cast<quint64>(o.value("hostId").toDouble());
        r.hostName    = o.value("hostName").toString();
        const auto rom = o.value("rom").toObject();
        r.romName     = rom.value("name").toString();
        r.romMd5      = rom.value("md5").toString();
        r.players     = o.value("players").toInt();
        r.maxPlayers  = o.value("maxPlayers").toInt();
        r.state       = o.value("state").toString();
        r.hasPassword = o.value("hasPassword").toBool();
        r.startedAtMs = static_cast<qint64>(o.value("startedAt").toDouble());
        r.broadcasting = o.value("broadcasting").toBool();
        r.matchId      = static_cast<quint64>(o.value("matchId").toDouble());
        for (const auto& n : o.value("playerNames").toArray())
            r.playerNames << n.toString();
        m_rooms.insert(r.id, r);
    }
    emit roomListChanged();
}

void LobbyClient::handleRoomCreated(const QJsonObject& data)
{
    emit roomCreated(static_cast<quint64>(data.value("roomId").toDouble()));
}

void LobbyClient::handleRoomCreateFail(const QJsonObject& data)
{
    emit roomCreateFailed(data.value("reason").toString());
}

void LobbyClient::handleRoomLeft(const QJsonObject& data)
{
    resetIceMesh();
    emit roomLeft(data.value("reason").toString());
}

void LobbyClient::handleRoomState(const QJsonObject& data)
{
    reconcileIcePeers(data);
    emit roomStateChanged(data);
}

void LobbyClient::handleRoomJoinOk(const QJsonObject& data)
{
    reconcileIcePeers(data);
    emit roomJoinOk(static_cast<quint64>(data.value("id").toDouble()));
    emit roomStateChanged(data);
}

void LobbyClient::handleRoomJoinFail(const QJsonObject& data)
{
    emit roomJoinFailed(data.value("reason").toString());
}

void LobbyClient::reconcileIcePeers(const QJsonObject& roomState)
{
    const quint64 roomId = static_cast<quint64>(roomState.value("id").toDouble());
    if (roomId == 0)
        return;
    if (m_currentIceRoomId != 0 && m_currentIceRoomId != roomId)
        resetIceMesh();
    m_currentIceRoomId = roomId;
    const quint64 hostUserId = static_cast<quint64>(roomState.value("hostId").toDouble());
    if (m_currentIceHostUserId != 0 && m_currentIceHostUserId != hostUserId)
        m_roomPeerPings.clear();
    m_currentIceHostUserId = hostUserId;

    QSet<quint64> desired;
    for (const QJsonValue& value : roomState.value("players").toArray())
    {
        const QJsonObject player = value.toObject();
        const quint64 userId = static_cast<quint64>(player.value("userId").toDouble());
        if (userId == 0 || userId == m_selfUserId)
            continue;
        if (!player.value("iceSupported").toBool())
        {
            qWarning() << "Rollback lobby peer does not support ICE" << userId;
            continue;
        }
        desired.insert(userId);
        if (!m_icePeerIds.contains(userId) && LobbyIce::add_peer(userId))
        {
            m_icePeerIds.insert(userId);
            writePingDiagnostic(QStringLiteral("ICE_PEER_ADDED"),
                                QStringLiteral("peer=%1 room=%2").arg(pingUserLabel(userId)).arg(roomId));
        }
    }

    const QSet<quint64> stale = m_icePeerIds - desired;
    for (quint64 userId : stale)
    {
        LobbyIce::remove_peer(userId);
        m_icePeerIds.remove(userId);
        m_lastIceStates.remove(userId);
        m_roomPeerPings.remove(userId);
        for (auto it = m_roomPeerPings.begin(); it != m_roomPeerPings.end(); ++it)
            it.value().remove(userId);
    }
}

void LobbyClient::resetIceMesh()
{
    for (quint64 userId : std::as_const(m_icePeerIds))
        LobbyIce::remove_peer(userId);
    m_icePeerIds.clear();
    m_lastIceStates.clear();
    m_currentIceRoomId = 0;
    m_currentIceHostUserId = 0;
    m_roomPeerPings.clear();
    m_pendingProbes.clear();
}

void LobbyClient::handleIceSignal(const QJsonObject& data)
{
    const quint64 roomId = static_cast<quint64>(data.value("roomId").toDouble());
    const quint64 fromUserId = static_cast<quint64>(data.value("fromUserId").toDouble());
    const QString kind = data.value("kind").toString();
    if (roomId == 0 || roomId != m_currentIceRoomId || fromUserId == 0 ||
        fromUserId == m_selfUserId)
        return;
    LobbyIce::handle_remote_signal(fromUserId, kind.toStdString(),
                                   data.value("value").toString().toStdString());
}

void LobbyClient::sendIcePing(quint64 userId, quint64 nonce)
{
    QByteArray payload(ICE_PING_PACKET_SIZE, Qt::Uninitialized);
    payload[0] = char(ICE_PING_OP_REQUEST);
    const quint64 nonceBE = qToBigEndian(nonce);
    std::memcpy(payload.data() + 1, &nonceBE, sizeof(nonceBE));
    LobbyIce::send(userId, LobbyIceChannel::Ping, payload.constData(), size_t(payload.size()));
}

void LobbyClient::sendRoomPingReport(quint64 targetUserId, int rttMs)
{
    // The host already measures all of its own paths. For every non-host pair,
    // the lower user id is the sole reporter, which makes one sample sufficient
    // and avoids two clients racing to overwrite the same unordered path.
    if (m_currentIceHostUserId == 0 || m_currentIceHostUserId == m_selfUserId ||
        targetUserId == 0 || targetUserId == m_currentIceHostUserId ||
        m_selfUserId >= targetUserId || !m_icePeerIds.contains(targetUserId))
    {
        return;
    }

    QByteArray payload(ICE_PING_REPORT_SIZE, Qt::Uninitialized);
    payload[0] = char(ICE_PING_OP_REPORT);
    const quint64 targetBE = qToBigEndian(targetUserId);
    const quint16 rttBE = qToBigEndian(static_cast<quint16>(std::clamp(rttMs, 0, 65535)));
    std::memcpy(payload.data() + 1, &targetBE, sizeof(targetBE));
    std::memcpy(payload.data() + 1 + sizeof(targetBE), &rttBE, sizeof(rttBE));
    LobbyIce::send(m_currentIceHostUserId, LobbyIceChannel::Ping,
                   payload.constData(), size_t(payload.size()));
}

void LobbyClient::flushIceEvents()
{
    if (m_ws && m_ws->state() == QAbstractSocket::ConnectedState && m_currentIceRoomId != 0)
    {
        for (LobbyIceSignal& signal : LobbyIce::take_local_signals())
        {
            QJsonObject data;
            data["targetUserId"] = QJsonValue(qint64(signal.peerUserId));
            data["roomId"] = QJsonValue(qint64(m_currentIceRoomId));
            data["kind"] = QString::fromStdString(signal.kind);
            if (!signal.value.empty())
                data["value"] = QString::fromStdString(signal.value);
            sendEnvelope("ICE_SIGNAL", data);
        }
    }

    for (const LobbyIcePacket& packet : LobbyIce::take_packets(LobbyIceChannel::Ping))
    {
        if (packet.data.size() == ICE_PING_REPORT_SIZE &&
            static_cast<quint8>(packet.data[0]) == ICE_PING_OP_REPORT)
        {
            quint64 targetBE = 0;
            quint16 rttBE = 0;
            std::memcpy(&targetBE, packet.data.data() + 1, sizeof(targetBE));
            std::memcpy(&rttBE, packet.data.data() + 1 + sizeof(targetBE), sizeof(rttBE));
            const quint64 targetUserId = qFromBigEndian(targetBE);
            const int rttMs = int(qFromBigEndian(rttBE));

            // Reports are meaningful only to the room host, from the canonical
            // lower-id reporter, and for two current non-host room members.
            if (m_selfUserId == m_currentIceHostUserId &&
                m_icePeerIds.contains(packet.peerUserId) &&
                m_icePeerIds.contains(targetUserId) &&
                packet.peerUserId < targetUserId &&
                m_roomPeerPings[packet.peerUserId].value(targetUserId, -1) != rttMs)
            {
                m_roomPeerPings[packet.peerUserId][targetUserId] = rttMs;
                writePingDiagnostic(QStringLiteral("ICE_PING_REPORT"),
                                    QStringLiteral("path=%1<->%2 rtt_ms=%3")
                                        .arg(pingUserLabel(packet.peerUserId))
                                        .arg(pingUserLabel(targetUserId)).arg(rttMs));
                emit roomPingMeasurementsChanged();
            }
            continue;
        }

        if (packet.data.size() != ICE_PING_PACKET_SIZE)
            continue;
        quint64 nonceBE = 0;
        std::memcpy(&nonceBE, packet.data.data() + 1, sizeof(nonceBE));
        const quint64 nonce = qFromBigEndian(nonceBE);
        if (static_cast<quint8>(packet.data[0]) == ICE_PING_OP_REQUEST)
        {
            std::vector<char> reply = packet.data;
            reply[0] = char(ICE_PING_OP_REPLY);
            LobbyIce::send(packet.peerUserId, LobbyIceChannel::Ping,
                           reply.data(), reply.size());
            continue;
        }
        if (static_cast<quint8>(packet.data[0]) != ICE_PING_OP_REPLY)
            continue;
        const auto it = m_pendingProbes.find(nonce);
        if (it == m_pendingProbes.end() || it->targetUserId != packet.peerUserId)
            continue;
        const qint64 rttUs = packet.roundTripUs == 0 ? -1 : qint64(packet.roundTripUs);
        const int rttMs = rttUs >= 0
            ? int(std::clamp<qint64>((rttUs + 500) / 1000, 0, 65535))
            : int(std::clamp<qint64>(
                  QDateTime::currentMSecsSinceEpoch() - it->attemptSendMs, 0, 65535));
        m_pendingProbes.erase(it);
        m_probeFailStreak[packet.peerUserId] = 0;
        m_measuredPing[packet.peerUserId] = rttMs;
        writePingDiagnostic(QStringLiteral("ICE_PING_REPLY"),
                            QStringLiteral("peer=%1 rtt_ms=%2 rtt_us=%3")
                                .arg(pingUserLabel(packet.peerUserId)).arg(rttMs).arg(rttUs));
        sendRoomPingReport(packet.peerUserId, rttMs);
        emit pingProbeMeasured(packet.peerUserId, rttMs);
    }

    for (quint64 userId : std::as_const(m_icePeerIds))
    {
        const int state = static_cast<int>(LobbyIce::peer_state(userId));
        if (m_lastIceStates.value(userId, -1) == state)
            continue;
        m_lastIceStates[userId] = state;
        writePingDiagnostic(QStringLiteral("ICE_STATE"),
                            QStringLiteral("peer=%1 state=%2 selected=%3")
                                .arg(pingUserLabel(userId)).arg(state)
                                .arg(QString::fromStdString(LobbyIce::peer_selected_addresses(userId))));
        const bool connected = LobbyIce::peer_connected(userId);
        emit icePeerConnectionChanged(userId, connected);
        if (connected && userId == m_currentIceHostUserId)
        {
            // A peer-to-peer measurement may have completed before our host
            // path did. Send any canonical reports now instead of waiting for
            // the next three-second probe cycle.
            for (auto it = m_measuredPing.constBegin(); it != m_measuredPing.constEnd(); ++it)
                sendRoomPingReport(it.key(), it.value());
        }
    }
}

void LobbyClient::handleChatMsg(const QJsonObject& data)
{
    ChatMessage m;
    m.channel        = data.value("channel").toString();
    m.fromUserId     = static_cast<quint64>(data.value("fromUserId").toDouble());
    m.fromUsername   = data.value("fromUsername").toString();
    m.message        = data.value("message").toString();
    m.serverTimeMs   = static_cast<qint64>(data.value("serverTime").toDouble());
    m.fromAdmin      = data.value("admin").toBool();
    emit chatMessageReceived(m);
}

void LobbyClient::handleChatHistoryReply(const QJsonObject& data)
{
    const QString channel = data.value("channel").toString();
    QList<ChatMessage> out;
    for (const auto& v : data.value("messages").toArray())
    {
        const auto o = v.toObject();
        ChatMessage m;
        m.channel       = o.value("channel").toString();
        m.fromUserId    = static_cast<quint64>(o.value("fromUserId").toDouble());
        m.fromUsername  = o.value("fromUsername").toString();
        m.message       = o.value("message").toString();
        m.serverTimeMs  = static_cast<qint64>(o.value("serverTime").toDouble());
        out.append(m);
    }
    emit chatHistoryReceived(channel, out);
}

void LobbyClient::startPingDiagnosticLog()
{
    // Read once per connection: flipping the setting mid-session shouldn't
    // start a half-populated trace that's missing its own connect sequence.
    if (!CoreSettingsGetBoolValue(SettingsID::Rollback_PingDiagnostics))
        return;

    if (m_pingDiagnosticFile == nullptr)
        m_pingDiagnosticFile = new QFile(this);
    if (m_pingDiagnosticFile->isOpen())
        m_pingDiagnosticFile->close();

    const std::filesystem::path directory = lobbyPingLogDirectory();
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString filename = QStringLiteral("lobby_ping_%1_pid%2.log")
                                 .arg(timestamp)
                                 .arg(QCoreApplication::applicationPid());

    QString path = filename;
    if (!directory.empty())
        path = QString::fromStdString((directory / filename.toStdString()).string());

    m_pingDiagnosticFile->setFileName(path);
    if (!m_pingDiagnosticFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        qWarning() << "Rollback lobby could not open ping diagnostic log"
                   << path << m_pingDiagnosticFile->errorString();
        return;
    }

    m_pingDiagnosticStartMs = QDateTime::currentMSecsSinceEpoch();
    qInfo() << "Rollback lobby ping diagnostic log" << path;
    writePingDiagnostic(QStringLiteral("LOG_START"),
                        QStringLiteral("path=%1 version=%2 username=%3")
                            .arg(path,
                                 QString::fromStdString(CoreGetVersion()),
                                 m_pendingUsername));
}

void LobbyClient::stopPingDiagnosticLog(const QString& reason)
{
    if (m_pingDiagnosticFile == nullptr || !m_pingDiagnosticFile->isOpen())
        return;
    writePingDiagnostic(QStringLiteral("LOG_END"), QStringLiteral("reason=%1").arg(reason));
    m_pingDiagnosticFile->close();
}

// Every line is flushed: the failures this exists to catch (hangs, and the
// crash-or-kill that often follows) would otherwise lose the buffered tail
// that holds the interesting part.
void LobbyClient::writePingDiagnostic(const QString& event, const QString& details)
{
    if (m_pingDiagnosticFile == nullptr || !m_pingDiagnosticFile->isOpen())
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QString cleanDetails = details;
    cleanDetails.replace(QLatin1Char('\n'), QLatin1Char(' '));
    cleanDetails.replace(QLatin1Char('\r'), QLatin1Char(' '));

    QString line = QStringLiteral("%1 elapsed_ms=%2 self=%3 local_udp=%4 event=%5")
                       .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                       .arg(m_pingDiagnosticStartMs == 0 ? 0 : nowMs - m_pingDiagnosticStartMs)
                       .arg(m_selfUserId)
                       .arg(localUdpPort())
                       .arg(event);
    if (!cleanDetails.isEmpty())
        line += QLatin1Char(' ') + cleanDetails;
    line += QLatin1Char('\n');

    m_pingDiagnosticFile->write(line.toUtf8());
    m_pingDiagnosticFile->flush();
}

// id:username, so a trace stays readable after the fact without cross
// referencing the presence list.
QString LobbyClient::pingUserLabel(quint64 userId) const
{
    const auto it = m_users.constFind(userId);
    if (it == m_users.constEnd())
        return QStringLiteral("%1:<unknown>").arg(userId);
    return QStringLiteral("%1:%2").arg(userId).arg(it->username);
}

void LobbyClient::learnRoute(quint64 userId, const QHostAddress& sender, quint16 senderPort)
{
    if (userId == 0 || userId == m_selfUserId || senderPort == 0)
        return;

    // Normalise v4-mapped addresses so the same endpoint doesn't oscillate
    // between "::ffff:1.2.3.4" and "1.2.3.4" spellings.
    bool isV4 = false;
    const quint32 v4 = sender.toIPv4Address(&isV4);
    const QString observed = QStringLiteral("%1:%2")
        .arg(isV4 ? QHostAddress(v4).toString() : sender.toString())
        .arg(senderPort);

    LearnedRoute& route = m_learnedRoutes[userId];
    const bool changed = (route.endpoint != observed);
    if (changed)
    {
        writePingDiagnostic(QStringLiteral("ROUTE_LEARNED"),
                            QStringLiteral("peer=%1 endpoint=%2 previous=%3")
                                .arg(pingUserLabel(userId), observed,
                                     route.endpoint.isEmpty() ? QStringLiteral("<none>") : route.endpoint));
    }
    route.endpoint   = observed;
    route.lastSeenMs = QDateTime::currentMSecsSinceEpoch();

    // Splice into any series already in flight so the current attempt benefits
    // instead of only the next one.
    if (changed)
    {
        for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); ++it)
        {
            if (it->targetUserId == userId && it->endpoint != observed)
                it->altEndpoint = observed;
        }
    }
}

QString LobbyClient::freshLearnedRoute(quint64 userId) const
{
    const auto it = m_learnedRoutes.constFind(userId);
    if (it == m_learnedRoutes.constEnd())
        return QString();
    if (QDateTime::currentMSecsSinceEpoch() - it->lastSeenMs > LEARNED_ROUTE_TTL_MS)
        return QString();
    return it->endpoint;
}

// Fire a UDP PROBE at a peer's anchor socket. The peer recognises the opcode
// and echoes back a PROBE_REPLY; we match by nonce to compute RTT. Shared by
// the reply path (we asked) and the incoming path (the server told us someone
// else asked about us) so both punch identically.
void LobbyClient::sendProbeTo(quint64 userId, const QString& endpoint)
{
    if (endpoint.isEmpty())
    {
        // Server knows the user but has never seen a UDP packet from them, so
        // there's nothing to probe. Logged because this used to fail silently
        // and looked identical to a dropped probe.
        qInfo() << "Rollback lobby probe skipped: no endpoint for user" << userId;
        writePingDiagnostic(QStringLiteral("PROBE_SKIP"),
                            QStringLiteral("peer=%1 reason=no_endpoint").arg(pingUserLabel(userId)));
        return;
    }
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState || m_selfUserId == 0)
    {
        writePingDiagnostic(QStringLiteral("PROBE_SKIP"),
                            QStringLiteral("peer=%1 reason=no_socket").arg(pingUserLabel(userId)));
        return;
    }
    if (m_anchorLent)
    {
        // Sends would work, but the echoes come back on the lent socket where
        // GekkoNet's filter drops them — the probe could never complete.
        writePingDiagnostic(QStringLiteral("PROBE_SKIP"),
                            QStringLiteral("peer=%1 reason=anchor_lent").arg(pingUserLabel(userId)));
        return;
    }

    const int colon = endpoint.lastIndexOf(':');
    if (colon <= 0)
    {
        writePingDiagnostic(QStringLiteral("PROBE_SKIP"),
                            QStringLiteral("peer=%1 reason=malformed_endpoint endpoint=%2")
                                .arg(pingUserLabel(userId), endpoint));
        return;
    }
    const QHostAddress addr(endpoint.left(colon));
    const quint16 port = static_cast<quint16>(endpoint.mid(colon + 1).toUInt());
    if (addr.isNull() || port == 0)
    {
        writePingDiagnostic(QStringLiteral("PROBE_SKIP"),
                            QStringLiteral("peer=%1 reason=unparsable_endpoint endpoint=%2")
                                .arg(pingUserLabel(userId), endpoint));
        return;
    }

    // One probe per peer in flight at a time. A second request while a burst is
    // still being retried would otherwise start a competing series and make the
    // retry counter shown in the room meaningless.
    for (auto it = m_pendingProbes.constBegin(); it != m_pendingProbes.constEnd(); ++it)
    {
        if (it->targetUserId == userId)
            return;
    }

    const quint64 nonce = QRandomGenerator::global()->generate64();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    ProbeInFlight in;
    in.targetUserId  = userId;
    in.sendMs        = nowMs;
    in.attemptSendMs = nowMs;
    in.endpoint      = endpoint;
    in.attempt       = 1;
    in.nextAttemptMs = nowMs + PROBE_RETRY_INTERVAL_MS;
    // If inbound traffic has shown us a different working route, burst that
    // too. Costs one extra burst; makes CGNAT/symmetric peers reachable the
    // moment any packet of theirs has ever landed here.
    const QString learned = freshLearnedRoute(userId);
    if (!learned.isEmpty() && learned != endpoint)
        in.altEndpoint = learned;
    m_pendingProbes.insert(nonce, in);

    sendProbeBurst(addr, port, nonce);
    QHostAddress altAddr; quint16 altPort = 0;
    if (!in.altEndpoint.isEmpty() && parseEndpoint(in.altEndpoint, altAddr, altPort))
        sendProbeBurst(altAddr, altPort, nonce);
    writePingDiagnostic(QStringLiteral("PROBE_SENT"),
                        QStringLiteral("peer=%1 endpoint=%2 alt=%3 nonce=%4 attempt=1/%5 burst=%6 pending=%7")
                            .arg(pingUserLabel(userId), endpoint,
                                 in.altEndpoint.isEmpty() ? QStringLiteral("-") : in.altEndpoint)
                            .arg(nonce)
                            .arg(PROBE_ATTEMPTS)
                            .arg(PROBE_BURST)
                            .arg(m_pendingProbes.size()));
}

// One burst = PROBE_BURST copies of the same packet. Defends against single
// packet loss and against routers that need more than one outbound packet
// before the mapping settles.
void LobbyClient::sendProbeBurst(const QHostAddress& addr, quint16 port, quint64 nonce)
{
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState)
        return;

    QByteArray pkt;
    pkt.reserve(PROBE_PACKET_SIZE);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_PROBE));
    const quint64 selfIdBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&selfIdBE), sizeof(selfIdBE));
    const quint64 nonceBE = qToBigEndian(nonce);
    pkt.append(reinterpret_cast<const char*>(&nonceBE), sizeof(nonceBE));

    for (int i = 0; i < PROBE_BURST; i++)
        m_udp->writeDatagram(pkt, addr, port);
}

void LobbyClient::onProbeRetryTimer()
{
    if (m_pendingProbes.isEmpty())
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); )
    {
        if (nowMs < it->nextAttemptMs)
        {
            ++it;
            continue;
        }

        if (it->attempt >= PROBE_ATTEMPTS)
        {
            // Out of attempts. Drop it here rather than leaving it for the
            // stale sweep so the room hears about it promptly.
            const quint64 uid = it->targetUserId;
            writePingDiagnostic(QStringLiteral("PROBE_FAILED"),
                                QStringLiteral("peer=%1 transport=ice attempts=%2 age_ms=%3")
                                    .arg(pingUserLabel(uid))
                                    .arg(PROBE_ATTEMPTS)
                                    .arg(nowMs - it->sendMs));
            it = m_pendingProbes.erase(it);
            noteProbeSeriesFailed(uid);
            continue;
        }

        it->attempt      += 1;
        it->attemptSendMs = nowMs;
        it->nextAttemptMs = nowMs + PROBE_RETRY_INTERVAL_MS;
        sendIcePing(it->targetUserId, it.key());
        writePingDiagnostic(QStringLiteral("PROBE_RETRY"),
                            QStringLiteral("peer=%1 transport=ice nonce=%2 attempt=%3/%4")
                                .arg(pingUserLabel(it->targetUserId))
                                .arg(it.key())
                                .arg(it->attempt)
                                .arg(PROBE_ATTEMPTS));
        emit pingProbeRetrying(it->targetUserId, it->attempt, PROBE_ATTEMPTS);
        ++it;
    }
}

void LobbyClient::noteProbeSeriesFailed(quint64 userId)
{
    const int streak = ++m_probeFailStreak[userId];
    // A peer we've never measured gets the hard verdict immediately — there is
    // no number to fall back on and first contact failing is the case the
    // retry/unreachable UI exists for. An established peer gets one free miss:
    // right after a match the far side is usually still handing its socket
    // back, and one aged-out series proves nothing.
    if (streak >= 2 || !m_measuredPing.contains(userId))
        emit pingProbeFailed(userId);
    else
        emit pingProbeSoftFailed(userId);
}

void LobbyClient::cancelPendingProbes(const QString& reason)
{
    if (m_pendingProbes.isEmpty())
        return;
    writePingDiagnostic(QStringLiteral("PROBE_CANCELLED"),
                        QStringLiteral("count=%1 reason=%2")
                            .arg(m_pendingProbes.size())
                            .arg(reason));
    m_pendingProbes.clear();
}

void LobbyClient::handlePingProbeReply(const QJsonObject& data)
{
    const quint64 uid = static_cast<quint64>(data.value("targetUserId").toDouble());
    const QString endpoint = data.value("targetEndpoint").toString();
    writePingDiagnostic(QStringLiteral("PROBE_REPLY_RECV"),
                        QStringLiteral("peer=%1 endpoint=%2")
                            .arg(pingUserLabel(uid),
                                 endpoint.isEmpty() ? QStringLiteral("<none>") : endpoint));
    emit pingProbeReply(uid, endpoint);
    sendProbeTo(uid, endpoint);
}

// The server is introducing us to a peer that asked about us, or that we were
// just seated with. Probing back opens our NAT mapping at the same moment they
// open theirs — without this our side stays closed and their probe is dropped.
// We don't need a measurement from it ourselves; the PROBE_REPLY they echo is
// what makes their reading work, and any reply we get updates ours for free.
void LobbyClient::handlePingProbeIncoming(const QJsonObject& data)
{
    const quint64 uid = static_cast<quint64>(data.value("fromUserId").toDouble());
    const QString endpoint = data.value("fromEndpoint").toString();
    writePingDiagnostic(QStringLiteral("INTRODUCED"),
                        QStringLiteral("peer=%1 endpoint=%2")
                            .arg(pingUserLabel(uid),
                                 endpoint.isEmpty() ? QStringLiteral("<none>") : endpoint));
    if (uid == 0 || uid == m_selfUserId)
        return;
    sendProbeTo(uid, endpoint);
}

void LobbyClient::handleMatchBegin(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    QList<LobbyMatchPeer> peers;
    for (const auto& v : data.value("peers").toArray())
    {
        const auto o = v.toObject();
        LobbyMatchPeer p;
        p.userId     = static_cast<quint64>(o.value("userId").toDouble());
        p.username   = o.value("username").toString();
        p.publicIp   = o.value("publicIp").toString();
        p.publicPort = static_cast<quint16>(o.value("publicPort").toInt());
        p.localIp    = o.value("localIp").toString();
        p.slot       = o.value("slot").toInt();
        p.iceSupported = o.value("iceSupported").toBool();
        peers.append(p);
    }
    emit matchBegin(matchId, peers);
}

void LobbyClient::handleMatchPeerLeft(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const quint64 userId  = static_cast<quint64>(data.value("userId").toDouble());
    const QString reason  = data.value("reason").toString();
    const int slot        = data.value("slot").toInt(0);
    emit matchPeerLeft(matchId, userId, slot, reason);
}

void LobbyClient::handleQuickMatchStatus(const QJsonObject& data)
{
    emit quickMatchStatus(data.value("searching").toBool(), data.value("queueSize").toInt());
}

void LobbyClient::handleSpectateBegin(const QJsonObject& data)
{
    emit spectateBegan(static_cast<quint64>(data.value("matchId").toDouble()));
}

void LobbyClient::handleSpectateData(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const QByteArray raw = QByteArray::fromBase64(data.value("data").toString().toLatin1());
    const int liveFrame = data.value("frame").toInt();
    if (!raw.isEmpty())
        emit spectateData(matchId, raw, liveFrame);
}

void LobbyClient::handleSpectateKeyframe(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const int frame      = data.value("frame").toInt();
    const int chunkIndex = data.value("chunkIndex").toInt();
    const int chunkCount = data.value("chunkCount").toInt();
    const QByteArray raw = QByteArray::fromBase64(data.value("data").toString().toLatin1());
    if (chunkCount <= 0 || chunkIndex < 0 || chunkIndex >= chunkCount)
        return;

    // (Re)start reassembly when a new keyframe appears. Chunks arrive in order over
    // the WS, but track per-index seen flags so a stray/duplicate can't corrupt it.
    if (m_kfRecvFrame != frame || m_kfRecvCount != chunkCount || m_kfRecvChunkSeen.size() != chunkCount)
    {
        m_kfRecvFrame = frame;
        m_kfRecvCount = chunkCount;
        m_kfRecvGot = 0;
        m_kfRecvBuf.clear();
        m_kfRecvChunkSeen = QList<bool>();
        for (int i = 0; i < chunkCount; i++)
            m_kfRecvChunkSeen.append(false);
    }
    if (!m_kfRecvChunkSeen[chunkIndex])
    {
        m_kfRecvChunkSeen[chunkIndex] = true;
        m_kfRecvBuf.append(raw); // in-order append (server sends chunks sequentially)
        m_kfRecvGot++;
    }
    if (m_kfRecvGot < m_kfRecvCount)
        return;

    const QByteArray full = m_kfRecvBuf;
    const int doneFrame = m_kfRecvFrame;
    m_kfRecvFrame = -1;
    m_kfRecvCount = 0;
    m_kfRecvGot = 0;
    m_kfRecvBuf.clear();
    m_kfRecvChunkSeen = QList<bool>();
    emit spectateKeyframe(matchId, doneFrame, full);
}

void LobbyClient::handleSpectateEnd(const QJsonObject& data)
{
    emit spectateEnded(static_cast<quint64>(data.value("matchId").toDouble()),
                       data.value("reason").toString());
}

void LobbyClient::handleSpectateFail(const QJsonObject& data)
{
    emit spectateFailed(static_cast<quint64>(data.value("matchId").toDouble()),
                        data.value("reason").toString());
}

// -------- Heartbeat --------

void LobbyClient::onHeartbeatTimer()
{
    if (m_state != ConnectionState::Connected)
        return;
    QJsonObject data;
    // TODO: include measured ping to server once we add WS ping/pong sampling
    m_reportedAway = (QDateTime::currentMSecsSinceEpoch() - m_lastActivityMs) >= AWAY_AFTER_MS;
    data["away"] = m_reportedAway;
    sendEnvelope("HEARTBEAT", data);
}

bool LobbyClient::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type())
    {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        m_lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        // First input after reporting away: snap back now rather than looking
        // AFK for up to another heartbeat interval.
        if (m_reportedAway && m_state == ConnectionState::Connected)
        {
            m_reportedAway = false;
            onHeartbeatTimer();
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

// -------- UDP anchor --------

void LobbyClient::initiateUdpAnchor()
{
    // Reconnected to the lobby while a match still borrows the socket: leave
    // the match's transport untouched and run without an anchor for now — the
    // match-end reclaim will register this same socket with the fresh session.
    if (m_anchorLent)
    {
        writePingDiagnostic(QStringLiteral("UDP_BIND"),
                            QStringLiteral("result=deferred reason=anchor_lent"));
        return;
    }

    // A socket that is already bound (zombie from a passive disconnect) or
    // left over from a failed bind cannot bind again until it's reset.
    if (m_udp->state() != QAbstractSocket::UnconnectedState)
        m_udp->abort();

    // Reclaim the port we've been using all session. Only the very first bind
    // asks the OS to pick one; every rebind after a match must land on the same
    // port or every peer's cached endpoint for us goes stale at once.
    bool bound = false;
    bool reusedPort = false;
    if (m_anchorLocalPort != 0)
    {
        bound = m_udp->bind(QHostAddress::AnyIPv4, m_anchorLocalPort, QUdpSocket::ShareAddress);
        reusedPort = bound;
        if (!bound)
        {
            // GekkoNet hasn't let go of the port yet — the emulation thread is
            // still tearing down while the UI-side room cleanup runs, a race
            // of about a second. A failed bind holds nothing, and a UDP port
            // frees the instant its socket closes (no TIME_WAIT), so instead
            // of surrendering the port immediately — which invalidates every
            // peer's route to us and can permanently break pairs with
            // strict-NAT peers who can't re-punch fresh mappings — retry it
            // briefly on a timer. Ephemeral fallback only once the window
            // expires.
            m_udp->abort();

            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_anchorRetryDeadlineMs == 0)
                m_anchorRetryDeadlineMs = nowMs + ANCHOR_PORT_RETRY_WINDOW_MS;
            if (nowMs < m_anchorRetryDeadlineMs)
            {
                writePingDiagnostic(QStringLiteral("UDP_BIND"),
                                    QStringLiteral("result=port_taken_retrying wanted=%1 window_left_ms=%2")
                                        .arg(m_anchorLocalPort)
                                        .arg(m_anchorRetryDeadlineMs - nowMs));
                m_anchorRetryTimer->start();
                return;
            }

            qWarning() << "lobby: could not rebind anchor port" << m_anchorLocalPort
                       << "after retries -" << m_udp->errorString() << "- falling back to a new port";
            writePingDiagnostic(QStringLiteral("UDP_BIND"),
                                QStringLiteral("result=port_retry_exhausted wanted=%1 error=%2")
                                    .arg(m_anchorLocalPort).arg(m_udp->errorString()));
        }
    }
    if (!bound)
        bound = m_udp->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);

    if (!bound)
    {
        qWarning() << "lobby: udp bind failed:" << m_udp->errorString();
        writePingDiagnostic(QStringLiteral("UDP_BIND"),
                            QStringLiteral("result=fail error=%1").arg(m_udp->errorString()));
        m_anchorRetryDeadlineMs = 0;
        return;
    }

    m_anchorRetryDeadlineMs = 0;
    m_anchorRetryTimer->stop();
    disableUdpConnReset();
    const quint16 previousPort = m_anchorLocalPort;
    m_anchorLocalPort = m_udp->localPort();
    writePingDiagnostic(QStringLiteral("UDP_BIND"),
                        QStringLiteral("result=ok port=%1 reused=%2 previous=%3")
                            .arg(m_anchorLocalPort)
                            .arg(reusedPort ? "yes" : "no")
                            .arg(previousPort));
    sendUdpRegister();
    m_udpKeepaliveTimer->start();
}

// Windows delivers ICMP "port unreachable" to a *connectionless* UDP socket as
// a WSAECONNRESET on the next receive. One peer's port going away (a match
// partner quitting, a game socket closing) therefore poisons receives on the
// socket that sent to it — and under the shared-transport model that is the
// one socket the lobby also depends on, so the client keeps sending fine while
// silently receiving nothing until the process restarts. SIO_UDP_CONNRESET
// off is the standard fix: unreachable peers are simply ignored, which is the
// right semantics for peer-to-peer UDP where peers come and go. No-op on
// non-Windows; failure here is not fatal, so it's logged rather than raised.
void LobbyClient::disableUdpConnReset()
{
#ifdef _WIN32
    if (!m_udp)
        return;
    const qintptr fd = m_udp->socketDescriptor();
    if (fd == -1)
        return;
    BOOL enable = FALSE;
    DWORD returned = 0;
    if (::WSAIoctl(static_cast<SOCKET>(fd), SIO_UDP_CONNRESET, &enable, sizeof(enable),
                   nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR)
    {
        writePingDiagnostic(QStringLiteral("UDP_CONNRESET"),
                            QStringLiteral("result=fail error=%1").arg(::WSAGetLastError()));
    }
#endif
}

void LobbyClient::sendUdpRegister()
{
    if (m_selfUserId == 0)
        return;
    QByteArray pkt;
    pkt.reserve(13);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_REGISTER));
    quint64 idBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&idBE), sizeof(idBE));
    // Single fire-and-forget packet: if it's lost the server has no endpoint
    // for us and every peer's probe of us fails until the next keepalive.
    // Logged so that shows up as a cause rather than a mystery.
    const qint64 written = m_udp->writeDatagram(pkt, QHostAddress(m_udpAnchorHost), m_udpAnchorPort);
    writePingDiagnostic(QStringLiteral("ANCHOR_REGISTER"),
                        QStringLiteral("anchor=%1:%2 bytes=%3")
                            .arg(m_udpAnchorHost).arg(m_udpAnchorPort).arg(written));
}

void LobbyClient::sendUdpKeepalive()
{
    if (m_selfUserId == 0)
        return;
    QByteArray pkt;
    pkt.reserve(13);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_KEEPALIVE));
    quint64 idBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&idBE), sizeof(idBE));
    m_udp->writeDatagram(pkt, QHostAddress(m_udpAnchorHost), m_udpAnchorPort);
}

void LobbyClient::onUdpKeepaliveTimer()
{
    sendUdpKeepalive();
}

void LobbyClient::punchPeerEndpoints(const QList<LobbyMatchPeer>& peers)
{
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState)
    {
        qWarning() << "Rollback lobby punch skipped: udp socket not connected";
        return;
    }
    if (m_selfUserId == 0)
    {
        qWarning() << "Rollback lobby punch skipped: missing self user id";
        return;
    }

    QByteArray pkt;
    pkt.reserve(13);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_PUNCH));
    quint64 idBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&idBE), sizeof(idBE));

    for (const auto& p : peers)
    {
        if (p.userId == m_selfUserId)
            continue;
        if (p.publicIp.isEmpty() || p.publicPort == 0)
        {
            qWarning() << "Rollback lobby punch skipped peer"
                       << "userId" << p.userId
                       << "slot" << p.slot
                       << "public" << p.publicIp << p.publicPort
                       << "local" << p.localIp;
            continue;
        }
        QHostAddress addr(p.publicIp);
        if (addr.isNull())
        {
            qWarning() << "Rollback lobby punch skipped peer with invalid address"
                       << "userId" << p.userId
                       << "slot" << p.slot
                       << "public" << p.publicIp << p.publicPort;
            continue;
        }
        qInfo() << "Rollback lobby punching peer"
                << "selfUserId" << m_selfUserId
                << "localPort" << m_udp->localPort()
                << "peerUserId" << p.userId
                << "slot" << p.slot
                << "public" << p.publicIp << p.publicPort
                << "local" << p.localIp
                << "burst" << ANCHOR_PUNCH_BURST;
        for (int i = 0; i < ANCHOR_PUNCH_BURST; ++i)
            m_udp->writeDatagram(pkt, addr, p.publicPort);
    }
}

bool LobbyClient::syncPrematchManifest(const QList<LobbyMatchPeer>& peers, int localSlot,
                                       quint64 hostUserId, const QString& romFile, QString& error)
{
    error.clear();
    if (m_selfUserId == 0 || localSlot < 1)
    {
        error = "Pre-match sync failed: missing local identity";
        return false;
    }
    if (romFile.isEmpty())
    {
        error = "Pre-match sync failed: local ROM path was not resolved";
        return false;
    }
    if (!allIcePeersConnected(peers, &error))
    {
        error = QStringLiteral("Pre-match sync failed: %1").arg(error);
        return false;
    }

    LobbyMatchPeer local{};
    LobbyMatchPeer host{};
    bool foundLocal = false;
    bool foundHost = false;
    const bool haveHostId = (hostUserId != 0);
    for (const LobbyMatchPeer& peer : peers)
    {
        if (peer.userId == m_selfUserId)
        {
            local = peer;
            foundLocal = true;
        }
        if (haveHostId ? (peer.userId == hostUserId) : (peer.slot == 1))
        {
            host = peer;
            foundHost = true;
        }
    }
    if (!foundLocal || !foundHost)
    {
        error = "Pre-match sync failed: incomplete peer list";
        return false;
    }

    constexpr qint64 kPrematchSyncTimeoutMs = 10'000;
    LobbyIce::take_packets(LobbyIceChannel::Prematch);

    if (host.userId == m_selfUserId)
    {
        std::string manifest;
        uint64_t manifestHash = 0;
        size_t cheatCount = 0;
        if (!buildPrematchManifest(romFile, manifest, manifestHash, cheatCount))
        {
            error = QString::fromStdString(CoreGetError().empty()
                ? std::string("Pre-match sync failed: could not build manifest") : CoreGetError());
            return false;
        }

        const QByteArray packet = buildPrematchPacket(
            ANCHOR_OP_PREMATCH_MANIFEST, m_selfUserId, manifestHash, manifest);
        const QList<QByteArray> outboundPackets = fragmentPrematchPacket(
            packet, m_selfUserId, manifestHash);
        if (outboundPackets.isEmpty())
        {
            error = QStringLiteral("Pre-match sync failed: settings manifest is too large.");
            return false;
        }
        QSet<quint64> pendingAcks;
        for (const LobbyMatchPeer& peer : peers)
        {
            if (peer.userId != m_selfUserId)
                pendingAcks.insert(peer.userId);
        }

        const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
        qint64 nextSendMs = 0;
        while (!pendingAcks.isEmpty())
        {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs > deadlineMs)
            {
                error = QStringLiteral("Pre-match sync timed out — a player's direct ICE connection stopped responding.");
                return false;
            }
            if (nowMs >= nextSendMs)
            {
                for (quint64 userId : std::as_const(pendingAcks))
                {
                    for (const QByteArray& outbound : outboundPackets)
                        LobbyIce::send(userId, LobbyIceChannel::Prematch,
                                       outbound.constData(), size_t(outbound.size()));
                }
                nextSendMs = nowMs + 100;
            }

            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            for (const LobbyIcePacket& icePacket : LobbyIce::take_packets(LobbyIceChannel::Prematch))
            {
                const QByteArray datagram(icePacket.data.data(), int(icePacket.data.size()));
                if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                    static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_ACK)
                    continue;
                quint64 senderUserId = 0;
                uint64_t ackHash = 0;
                if (readPrematchSenderAndHash(datagram, senderUserId, ackHash) &&
                    senderUserId == icePacket.peerUserId && ackHash == manifestHash)
                    pendingAcks.remove(senderUserId);
            }
            QThread::msleep(10);
        }

        if (!applyPrematchManifest(manifest, manifestHash, cheatCount))
        {
            error = QString::fromStdString(CoreGetError());
            return false;
        }
        qInfo() << "Rollback lobby ICE prematch host complete"
                << "hash" << static_cast<qulonglong>(manifestHash)
                << "cheats" << static_cast<qulonglong>(cheatCount);
        return true;
    }

    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
    uint64_t fragmentHash = 0;
    int fragmentCount = 0;
    quint32 fragmentTotalBytes = 0;
    QList<QByteArray> fragmentParts;
    while (QDateTime::currentMSecsSinceEpoch() <= deadlineMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        for (const LobbyIcePacket& icePacket : LobbyIce::take_packets(LobbyIceChannel::Prematch))
        {
            if (icePacket.peerUserId != host.userId)
                continue;
            QByteArray datagram(icePacket.data.data(), int(icePacket.data.size()));
            if (datagram.size() >= 29 && std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) == 0 &&
                static_cast<quint8>(datagram.at(4)) == ANCHOR_OP_PREMATCH_FRAGMENT)
            {
                quint64 senderUserId = 0;
                uint64_t incomingHash = 0;
                quint16 index = 0;
                quint16 count = 0;
                quint32 totalBytes = 0;
                if (!readPrematchSenderAndHash(datagram, senderUserId, incomingHash) ||
                    senderUserId != host.userId ||
                    !readPrematchU16(datagram, 21, index) ||
                    !readPrematchU16(datagram, 23, count) ||
                    !readPrematchU32(datagram, 25, totalBytes) ||
                    count == 0 || count > 2048 || index >= count ||
                    totalBytes == 0 || totalBytes > 2 * 1024 * 1024)
                    continue;
                if (fragmentHash != incomingHash || fragmentCount != count ||
                    fragmentTotalBytes != totalBytes)
                {
                    fragmentHash = incomingHash;
                    fragmentCount = count;
                    fragmentTotalBytes = totalBytes;
                    fragmentParts.clear();
                    fragmentParts.resize(count);
                }
                fragmentParts[index] = datagram.mid(29);
                bool complete = true;
                for (const QByteArray& part : std::as_const(fragmentParts))
                    complete = complete && !part.isEmpty();
                if (!complete)
                    continue;
                datagram.clear();
                datagram.reserve(int(fragmentTotalBytes));
                for (const QByteArray& part : std::as_const(fragmentParts))
                    datagram.append(part);
                if (datagram.size() != int(fragmentTotalBytes))
                    continue;
            }
            if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_MANIFEST)
                continue;

            quint64 senderUserId = 0;
            uint64_t manifestHash = 0;
            if (!readPrematchSenderAndHash(datagram, senderUserId, manifestHash) ||
                senderUserId != host.userId)
                continue;
            if (fragmentHash != 0 && manifestHash != fragmentHash)
                continue;
            const int manifestOffset = 4 + 1 + 8 + 8;
            const std::string manifest(datagram.constData() + manifestOffset,
                                       datagram.constData() + datagram.size());
            if (prematchHashBytes(manifest) != manifestHash)
            {
                error = QStringLiteral("Pre-match sync failed: settings manifest was corrupted in transit.");
                return false;
            }
            size_t cheatCount = 0;
            if (!applyPrematchManifest(manifest, manifestHash, cheatCount))
            {
                error = QString::fromStdString(CoreGetError());
                return false;
            }

            const QByteArray ack = buildPrematchPacket(
                ANCHOR_OP_PREMATCH_ACK, m_selfUserId, manifestHash);
            for (int i = 0; i < 8; ++i)
            {
                LobbyIce::send(host.userId, LobbyIceChannel::Prematch,
                               ack.constData(), size_t(ack.size()));
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            qInfo() << "Rollback lobby ICE prematch client complete"
                    << "hash" << static_cast<qulonglong>(manifestHash)
                    << "cheats" << static_cast<qulonglong>(cheatCount);
            return true;
        }
        QThread::msleep(10);
    }
    error = QStringLiteral("Pre-match sync timed out — never heard from the host over ICE.");
    return false;

#if 0 // Retained temporarily for protocol archaeology; the rollback lobby uses ICE above.
    struct PrematchSyncGuard
    {
        LobbyClient& client;
        explicit PrematchSyncGuard(LobbyClient& client) : client(client) { client.m_inPrematchSync = true; }
        ~PrematchSyncGuard() { client.m_inPrematchSync = false; }
    } guard(*this);

    error.clear();
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState)
    {
        error = "Pre-match sync failed: UDP anchor is not connected";
        return false;
    }
    if (m_selfUserId == 0 || localSlot < 1)
    {
        error = "Pre-match sync failed: missing local identity";
        return false;
    }
    if (romFile.isEmpty())
    {
        error = "Pre-match sync failed: local ROM path was not resolved";
        return false;
    }

    // Pre-match sync spins a nested event loop on the UI thread. Without a wall-
    // clock cap, a peer that never answers (UDP blocked, crashed after
    // MATCH_BEGIN, missing ROM) would hang the whole client forever — the bug
    // that left users force-killing the window. Bail out gracefully after this.
    const qint64 kPrematchSyncTimeoutMs = 10'000;

    LobbyMatchPeer local{};
    LobbyMatchPeer host{};
    bool foundLocal = false;
    bool foundHost = false;
    // The room's actual host is the sync authority. Fall back to the old
    // "seat 1 is the authority" rule only when the caller couldn't resolve a
    // host id at all (e.g. an auto-created quick-match room whose ROOM_STATE we
    // never matched) — that's the pre-existing behavior, so this degrades
    // rather than failing a match that would otherwise have worked.
    const bool haveHostId = (hostUserId != 0);
    for (const auto& peer : peers)
    {
        if (peer.userId == m_selfUserId)
        {
            local = peer;
            foundLocal = true;
        }
        if (haveHostId ? (peer.userId == hostUserId) : (peer.slot == 1))
        {
            host = peer;
            foundHost = true;
        }
    }
    if (!foundLocal || !foundHost)
    {
        error = "Pre-match sync failed: incomplete peer list";
        return false;
    }

    auto endpointFor = [&](const LobbyMatchPeer& peer) {
        QString ip = peer.publicIp;
        if (!local.publicIp.isEmpty() && local.publicIp == peer.publicIp && !peer.localIp.isEmpty())
            ip = peer.localIp;
        return QPair<QHostAddress, quint16>(QHostAddress(ip), peer.publicPort);
    };

    // Whoever we resolved as host above sends; everyone else waits for them.
    // Keyed off `host`, not the seat, so the two branches can never disagree.
    if (host.userId == m_selfUserId)
    {
        std::string manifest;
        uint64_t manifestHash = 0;
        size_t cheatCount = 0;
        if (!buildPrematchManifest(romFile, manifest, manifestHash, cheatCount))
        {
            error = QString::fromStdString(CoreGetError().empty() ? std::string("Pre-match sync failed: could not build manifest") : CoreGetError());
            return false;
        }

        const QByteArray packet = buildPrematchPacket(ANCHOR_OP_PREMATCH_MANIFEST, m_selfUserId, manifestHash, manifest);
        QSet<quint64> pendingAcks;
        for (const auto& peer : peers)
        {
            if (peer.userId != m_selfUserId)
                pendingAcks.insert(peer.userId);
        }

        qInfo() << "Rollback lobby prematch host begin"
                << "hash" << static_cast<qulonglong>(manifestHash)
                << "bytes" << manifest.size()
                << "cheats" << static_cast<qulonglong>(cheatCount)
                << "peers" << pendingAcks.size();

        const qint64 hostSyncDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
        while (!pendingAcks.isEmpty())
        {
            if (QDateTime::currentMSecsSinceEpoch() > hostSyncDeadlineMs)
            {
                error = QStringLiteral("Pre-match sync timed out — a player didn't respond (missing ROM or blocked connection).");
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            for (const auto& peer : peers)
            {
                if (!pendingAcks.contains(peer.userId))
                    continue;
                const auto endpoint = endpointFor(peer);
                if (!endpoint.first.isNull() && endpoint.second != 0)
                    m_udp->writeDatagram(packet, endpoint.first, endpoint.second);
            }

            for (int i = 0; i < 5; i++)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!m_udp->waitForReadyRead(10))
                    continue;
                while (m_udp->hasPendingDatagrams())
                {
                    QByteArray datagram;
                    QHostAddress sender;
                    quint16 senderPort = 0;
                    datagram.resize(int(m_udp->pendingDatagramSize()));
                    m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

                    if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                        static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_ACK)
                    {
                        continue;
                    }

                    quint64 senderUserId = 0;
                    uint64_t ackHash = 0;
                    if (readPrematchSenderAndHash(datagram, senderUserId, ackHash) && ackHash == manifestHash)
                        pendingAcks.remove(senderUserId);
                }
            }
        }

        if (!applyPrematchManifest(manifest, manifestHash, cheatCount))
        {
            error = QString::fromStdString(CoreGetError());
            return false;
        }

        qInfo() << "Rollback lobby prematch host complete"
                << "hash" << static_cast<qulonglong>(manifestHash)
                << "cheats" << static_cast<qulonglong>(cheatCount);
        return true;
    }

    const auto hostEndpoint = endpointFor(host);
    if (hostEndpoint.first.isNull() || hostEndpoint.second == 0)
    {
        error = "Pre-match sync failed: invalid host endpoint";
        return false;
    }

    qInfo() << "Rollback lobby prematch client waiting"
            << "hostUserId" << host.userId
            << "hostEndpoint" << hostEndpoint.first.toString()
            << hostEndpoint.second;

    const qint64 clientSyncDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
    for (;;)
    {
        if (QDateTime::currentMSecsSinceEpoch() > clientSyncDeadlineMs)
        {
            error = QStringLiteral("Pre-match sync timed out — never heard from the host.");
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (!m_udp->waitForReadyRead(50))
            continue;

        while (m_udp->hasPendingDatagrams())
        {
            QByteArray datagram;
            QHostAddress sender;
            quint16 senderPort = 0;
            datagram.resize(int(m_udp->pendingDatagramSize()));
            m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

            if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_MANIFEST)
            {
                continue;
            }

            quint64 senderUserId = 0;
            uint64_t manifestHash = 0;
            if (!readPrematchSenderAndHash(datagram, senderUserId, manifestHash) || senderUserId != host.userId)
                continue;

            const int manifestOffset = 4 + 1 + 8 + 8;
            const std::string manifest(datagram.constData() + manifestOffset, datagram.constData() + datagram.size());
            size_t cheatCount = 0;
            if (!applyPrematchManifest(manifest, manifestHash, cheatCount))
            {
                error = QString::fromStdString(CoreGetError());
                return false;
            }

            const QByteArray ack = buildPrematchPacket(ANCHOR_OP_PREMATCH_ACK, m_selfUserId, manifestHash);
            for (int i = 0; i < ANCHOR_PUNCH_BURST; i++)
                m_udp->writeDatagram(ack, hostEndpoint.first, hostEndpoint.second);

            qInfo() << "Rollback lobby prematch client complete"
                    << "hash" << static_cast<qulonglong>(manifestHash)
                    << "cheats" << static_cast<qulonglong>(cheatCount);
            return true;
        }
    }
#endif
}

quint16 LobbyClient::localUdpPort() const
{
    return m_udp ? m_udp->localPort() : 0;
}

void LobbyClient::releaseUdpAnchor()
{
    // A match is taking the socket over — the pinned port now legitimately
    // belongs to GekkoNet, so any pending rebind retry must not fight it.
    if (m_anchorRetryTimer)
        m_anchorRetryTimer->stop();
    m_anchorRetryDeadlineMs = 0;
    if (m_udpKeepaliveTimer)
        m_udpKeepaliveTimer->stop();
    // Same reasoning as lendAnchorToMatch: an in-flight series can't survive
    // the socket going away, and letting it age out would report the match
    // peer as unreachable.
    cancelPendingProbes(QStringLiteral("anchor_released"));
    m_probeFailStreak.clear();
    if (m_udp && m_udp->state() != QAbstractSocket::UnconnectedState)
    {
        qInfo() << "Rollback lobby releasing UDP anchor"
                << "localPort" << m_udp->localPort()
                << "state" << m_udp->state();
        // abort() is more aggressive than close(): forces immediate socket
        // teardown rather than the graceful path. Necessary so GekkoNet can
        // re-bind the same port without racing the OS's lingering release.
        m_udp->abort();
    }
    else
    {
        qWarning() << "Rollback lobby releaseUdpAnchor skipped: udp not connected";
    }
}

void LobbyClient::reopenUdpAnchor()
{
    // Shared-socket model: the socket never went away — just resume our side.
    if (m_anchorLent)
    {
        reclaimAnchorFromMatch();
        return;
    }
    if (m_state == ConnectionState::Connected && m_udp &&
        m_udp->state() == QAbstractSocket::UnconnectedState)
    {
        initiateUdpAnchor();
    }
}

qintptr LobbyClient::lendAnchorToMatch()
{
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState)
        return -1;
    const qintptr fd = m_udp->socketDescriptor();
    if (fd == -1)
        return -1;

    if (m_anchorRetryTimer)
        m_anchorRetryTimer->stop();
    m_anchorRetryDeadlineMs = 0;
    // Keepalives pause: GekkoNet's receive filter drops RMGK-tagged datagrams,
    // so acks would vanish anyway, and the server keeps our endpoint entry —
    // reclaim re-registers the same address the moment the match ends.
    if (m_udpKeepaliveTimer)
        m_udpKeepaliveTimer->stop();
    // A series caught in flight by the handoff would burn its retries against
    // a socket whose echoes GekkoNet drops, then age out into a failure signal
    // — flagging the very peer we're starting a match with as unreachable, and
    // nothing clears it until probing resumes at match end. Drop them quietly;
    // the miss streak resets too so the first post-match cycle starts clean.
    cancelPendingProbes(QStringLiteral("anchor_lent"));
    m_probeFailStreak.clear();
    m_anchorLent = true;
    qInfo() << "Rollback lobby lending anchor socket to match"
            << "localPort" << m_udp->localPort();
    writePingDiagnostic(QStringLiteral("ANCHOR_LENT"),
                        QStringLiteral("port=%1").arg(m_udp->localPort()));
    return fd;
}

void LobbyClient::reclaimAnchorFromMatch()
{
    if (!m_anchorLent)
        return;
    m_anchorLent = false;
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState ||
        m_state != ConnectionState::Connected)
        return;
    // Same socket, same port, same NAT mappings — nothing to rebind. The
    // REGISTER is a refresh, not a re-punch; peers' learned routes to us are
    // still valid, which is the whole point of the shared-socket model.
    writePingDiagnostic(QStringLiteral("ANCHOR_RECLAIMED"),
                        QStringLiteral("port=%1").arg(m_udp->localPort()));
    sendUdpRegister();
    if (m_udpKeepaliveTimer)
        m_udpKeepaliveTimer->start();
    // Drain anything that arrived between GekkoNet's last poll and now, so no
    // stale datagram sits in the buffer wedging the read notifier.
    //
    // Unconditional on purpose. Throughout the lend our readyRead slot returns
    // without reading, and Qt disables the read notification for an unbuffered
    // socket whose readyRead went unserviced — it is re-armed by the next read,
    // not by time. Gating this drain on hasPendingDatagrams() meant that a
    // reclaim landing on a momentarily empty buffer performed no read at all
    // and left the notifier disabled, so the socket kept sending while silently
    // receiving nothing until the process restarted. onUdpReadyRead is a
    // read-until-empty loop and is safe to call with nothing pending.
    m_drainedDatagrams = 0;
    onUdpReadyRead();
    // How much had piled up while nobody was reading tells us which failure we
    // are looking at when a peer goes deaf after a match: a large backlog means
    // datagrams were arriving and going unserviced (our reader / the notifier),
    // while zero means nothing reached the socket at all (blocked inbound —
    // firewall or NAT), which no amount of draining would fix.
    writePingDiagnostic(QStringLiteral("ANCHOR_DRAIN"),
                        QStringLiteral("datagrams=%1").arg(m_drainedDatagrams));
}

void LobbyClient::onUdpReadyRead()
{
    if (m_inPrematchSync)
        return;
    // While the socket is lent to GekkoNet, its thread owns recvfrom; reading
    // here would steal game packets (the n02 "packet stealing" race).
    if (m_anchorLent)
        return;

    while (m_udp->hasPendingDatagrams())
    {
        QByteArray datagram;
        QHostAddress sender;
        quint16 senderPort = 0;
        datagram.resize(int(m_udp->pendingDatagramSize()));
        m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        m_drainedDatagrams++;

        if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0)
            continue;

        const quint8 op = static_cast<quint8>(datagram.at(4));
        switch (op)
        {
        case ANCHOR_OP_PROBE:
        {
            if (datagram.size() != PROBE_PACKET_SIZE)
                break;
            // Echo as PROBE_REPLY with our userId in the sender slot; nonce
            // bytes (offset 13..21) carry through unchanged so the originator
            // can match on its end.
            QByteArray reply(datagram);
            reply[4] = static_cast<char>(ANCHOR_OP_PROBE_REPLY);
            const quint64 selfIdBE = qToBigEndian(m_selfUserId);
            std::memcpy(reply.data() + 5, &selfIdBE, sizeof(selfIdBE));
            m_udp->writeDatagram(reply, sender, senderPort);

            quint64 fromBE = 0;
            std::memcpy(&fromBE, datagram.constData() + 5, sizeof(fromBE));
            const quint64 fromUserId = qFromBigEndian(fromBE);
            // This packet just proved this exact source traverses both NATs —
            // remember it as the peer's working route (n02-style: trust caller
            // ID over the directory).
            learnRoute(fromUserId, sender, senderPort);
            // Proves the far side reached us — i.e. our NAT let their probe in.
            // Its absence is the signature of a punch that never landed.
            writePingDiagnostic(QStringLiteral("INBOUND_PROBE"),
                                QStringLiteral("peer=%1 from=%2:%3")
                                    .arg(pingUserLabel(fromUserId),
                                         sender.toString())
                                    .arg(senderPort));
            break;
        }
        case ANCHOR_OP_PROBE_REPLY:
        {
            if (datagram.size() != PROBE_PACKET_SIZE)
                break;
            quint64 nonceBE = 0;
            std::memcpy(&nonceBE, datagram.constData() + 13, sizeof(nonceBE));
            const quint64 nonce = qFromBigEndian(nonceBE);

            // The echoing peer stamps its own id into the sender slot, so a
            // reply teaches us their working route too — including the
            // duplicate echoes of a burst, which keep the TTL fresh.
            quint64 echoerBE = 0;
            std::memcpy(&echoerBE, datagram.constData() + 5, sizeof(echoerBE));
            learnRoute(qFromBigEndian(echoerBE), sender, senderPort);

            const auto it = m_pendingProbes.find(nonce);
            if (it == m_pendingProbes.end())
            {
                // Expected: the other 9 echoes of a burst we already measured.
                // Silent, or they'd drown the log.
                if (m_recentlyMatchedNonces.contains(nonce))
                    break;
                // Genuinely unmatched — a reply to a series we gave up on.
                // A run of these means probes land, just too slowly.
                writePingDiagnostic(QStringLiteral("REPLY_UNMATCHED"),
                                    QStringLiteral("nonce=%1 from=%2:%3 reason=no_pending_probe")
                                        .arg(nonce)
                                        .arg(sender.toString())
                                        .arg(senderPort));
                break;
            }
            const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
            const int    rttMs  = static_cast<int>(nowMs - it->attemptSendMs);
            const quint64 uid   = it->targetUserId;
            const int attempt   = it->attempt;
            m_pendingProbes.erase(it);
            if (m_recentlyMatchedNonces.size() >= PROBE_MATCHED_NONCE_CAP)
                m_recentlyMatchedNonces.clear();
            m_recentlyMatchedNonces.insert(nonce);
            m_measuredPing[uid] = rttMs;
            m_probeFailStreak.remove(uid);
            writePingDiagnostic(QStringLiteral("RTT"),
                                QStringLiteral("peer=%1 rtt_ms=%2 attempt=%3/%4 nonce=%5 from=%6:%7")
                                    .arg(pingUserLabel(uid))
                                    .arg(rttMs)
                                    .arg(attempt)
                                    .arg(PROBE_ATTEMPTS)
                                    .arg(nonce)
                                    .arg(sender.toString())
                                    .arg(senderPort));
            emit pingProbeMeasured(uid, rttMs);
            break;
        }
        case ANCHOR_OP_REGISTER:
        case ANCHOR_OP_KEEPALIVE:
        case ANCHOR_OP_PUNCH:
        default:
            // Server acks for register/keepalive aren't actionable yet, and
            // PUNCH packets from peers are intentional no-ops — drop silently.
            break;
        }
    }

    // Cheap stale-probe cleanup: anything older than 5 seconds is never
    // coming back, so don't let the map grow unbounded.
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - 5'000;
    for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); )
    {
        if (it->sendMs < cutoff)
        {
            // The headline symptom: we sent, nothing came back. Paired with
            // whether an INBOUND_PROBE from that peer ever appeared, this
            // separates "our packet died at their NAT" from "theirs died at
            // ours" — the whole point of the mutual introduction.
            writePingDiagnostic(QStringLiteral("PROBE_EXPIRED"),
                                QStringLiteral("peer=%1 age_ms=%2")
                                    .arg(pingUserLabel(it->targetUserId))
                                    .arg(QDateTime::currentMSecsSinceEpoch() - it->sendMs));
            it = m_pendingProbes.erase(it);
        }
        else
            ++it;
    }
}

int LobbyClient::measuredPingMs(quint64 userId) const
{
    const auto it = m_measuredPing.constFind(userId);
    return it == m_measuredPing.constEnd() ? -1 : it.value();
}

int LobbyClient::worstRoomPingMs(const QList<quint64>& roomUserIds, bool* complete) const
{
    QSet<quint64> uniqueIds;
    for (quint64 userId : roomUserIds)
    {
        if (userId != 0)
            uniqueIds.insert(userId);
    }
    QList<quint64> userIds = uniqueIds.values();
    std::sort(userIds.begin(), userIds.end());

    bool allMeasured = userIds.size() >= 2;
    int worst = -1;
    for (int i = 0; i < userIds.size(); ++i)
    {
        for (int j = i + 1; j < userIds.size(); ++j)
        {
            const quint64 first = userIds[i];
            const quint64 second = userIds[j];
            int ping = -1;
            if (first == m_selfUserId || second == m_selfUserId)
            {
                const quint64 remote = first == m_selfUserId ? second : first;
                ping = measuredPingMs(remote);
            }
            else
            {
                ping = m_roomPeerPings.value(first).value(second, -1);
            }

            if (ping < 0)
                allMeasured = false;
            else
                worst = std::max(worst, ping);
        }
    }

    if (complete)
        *complete = allMeasured;
    return worst;
}

bool LobbyClient::allIcePeersConnected(const QList<LobbyMatchPeer>& peers, QString* error) const
{
    for (const LobbyMatchPeer& peer : peers)
    {
        if (peer.userId == m_selfUserId)
            continue;
        if (!peer.iceSupported)
        {
            if (error)
                *error = QStringLiteral("%1 is using a lobby client without ICE support.")
                             .arg(peer.username.isEmpty() ? QString::number(peer.userId) : peer.username);
            return false;
        }
        if (!LobbyIce::peer_connected(peer.userId))
        {
            if (error)
                *error = QStringLiteral("Waiting for a direct ICE connection to %1.")
                             .arg(peer.username.isEmpty() ? QString::number(peer.userId) : peer.username);
            return false;
        }
    }
    if (error)
        error->clear();
    return true;
}

// -------- Chat API --------

void LobbyClient::sendChat(const QString& channel, const QString& message)
{
    QJsonObject d;
    d["channel"] = channel;
    d["message"] = message;
    sendEnvelope("CHAT_SEND", d);
}

void LobbyClient::requestChatHistory(const QString& channel)
{
    QJsonObject d;
    d["channel"] = channel;
    sendEnvelope("CHAT_HISTORY", d);
}

// --- Moderation ---

void LobbyClient::sendAdminAuth(const QString& password)
{
    QJsonObject d;
    d["password"] = password;
    sendEnvelope("ADMIN_AUTH", d);
}

void LobbyClient::sendModAction(const QString& action, const QString& target,
                                const QString& duration, const QString& reason)
{
    QJsonObject d;
    d["action"] = action;
    d["target"] = target;
    if (!duration.isEmpty()) d["duration"] = duration;
    if (!reason.isEmpty())   d["reason"]   = reason;
    sendEnvelope("MOD_ACTION", d);
}

void LobbyClient::handleAdminAuthOk(const QJsonObject& data)
{
    m_isModerator = true;
    emit adminAuthResult(true, data.value("name").toString());
}

void LobbyClient::handleAdminAuthFail(const QJsonObject& data)
{
    m_isModerator = false;
    emit adminAuthResult(false, data.value("text").toString());
}

void LobbyClient::handleModNotice(const QJsonObject& data)
{
    emit modNotice(data.value("severity").toString(), data.value("text").toString());
}

void LobbyClient::handleModList(const QJsonObject& data)
{
    emit modListReceived(data.value("bans").toArray(), data.value("mutes").toArray());
}

// -------- Room API --------

void LobbyClient::createRoom(const QString& name, const QString& romName, const QString& romMd5,
                              const QString& romRegion, int maxPlayers, int delay, int prediction,
                              int pacing, const QString& password)
{
    QJsonObject rom;
    rom["name"]   = romName;
    rom["md5"]    = romMd5;
    rom["region"] = romRegion;

    QJsonObject d;
    d["name"]       = name;
    d["rom"]        = rom;
    d["maxPlayers"] = maxPlayers;
    d["delay"]      = delay;
    d["prediction"] = prediction;
    d["pacing"]     = pacing;
    if (!password.isEmpty())
        d["password"] = password;

    sendEnvelope("ROOM_CREATE", d);
}

void LobbyClient::joinRoom(quint64 roomId, const QString& password)
{
    QJsonObject d;
    d["roomId"] = QJsonValue(qint64(roomId));
    if (!password.isEmpty())
        d["password"] = password;
    sendEnvelope("ROOM_JOIN", d);
}

void LobbyClient::leaveRoom()
{
    sendEnvelope("ROOM_LEAVE");
}

void LobbyClient::startRoom()
{
    sendEnvelope("ROOM_START");
}

// Host-only: change the active room's rollback parameters. The server's
// ROOM_UPDATE_SETTINGS handler validates (host, state == "waiting"), clamps,
// and rebroadcasts ROOM_STATE with the new values + auto flags so every seated
// client picks them up and the match starts on the resolved delay.
void LobbyClient::updateRoomSettings(int delay, int prediction, int pacing, bool delayAuto, bool predictionAuto)
{
    QJsonObject d;
    d["delay"]          = delay;
    d["prediction"]     = prediction;
    d["pacing"]         = pacing;
    d["delayAuto"]      = delayAuto;
    d["predictionAuto"] = predictionAuto;
    sendEnvelope("ROOM_UPDATE_SETTINGS", d);
}

void LobbyClient::kickFromRoom(quint64 userId)
{
    QJsonObject d;
    d["userId"] = QJsonValue(qint64(userId));
    sendEnvelope("ROOM_KICK", d);
}

void LobbyClient::swapSeats(int slotA, int slotB)
{
    QJsonObject d;
    d["slotA"] = slotA;
    d["slotB"] = slotB;
    // Host-only; the server validates (host, state == "waiting", valid slots),
    // swaps the two seats and rebroadcasts ROOM_STATE so every seated client
    // re-renders with the new P1-P4 order.
    sendEnvelope("ROOM_SWAP_SLOTS", d);
}

void LobbyClient::requestPingProbe(quint64 targetUserId)
{
    if (targetUserId == 0 || targetUserId == m_selfUserId ||
        !LobbyIce::peer_connected(targetUserId))
        return;
    for (auto it = m_pendingProbes.cbegin(); it != m_pendingProbes.cend(); ++it)
    {
        if (it->targetUserId == targetUserId)
            return;
    }

    quint64 nonce = QRandomGenerator::global()->generate64();
    while (nonce == 0 || m_pendingProbes.contains(nonce))
        nonce = QRandomGenerator::global()->generate64();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    ProbeInFlight probe;
    probe.targetUserId = targetUserId;
    probe.sendMs = nowMs;
    probe.attemptSendMs = nowMs;
    probe.nextAttemptMs = nowMs + PROBE_RETRY_INTERVAL_MS;
    m_pendingProbes.insert(nonce, probe);
    sendIcePing(targetUserId, nonce);
    writePingDiagnostic(QStringLiteral("PROBE_REQUEST"),
                        QStringLiteral("peer=%1 transport=ice nonce=%2")
                            .arg(pingUserLabel(targetUserId)).arg(nonce));
}

void LobbyClient::reportMatchConnected(quint64 matchId, quint64 peerUserId)
{
    QJsonObject d;
    d["matchId"]    = QJsonValue(qint64(matchId));
    d["peerUserId"] = QJsonValue(qint64(peerUserId));
    sendEnvelope("MATCH_CONNECTED", d);
}

void LobbyClient::reportMatchPunchFailed(quint64 matchId, quint64 peerUserId)
{
    QJsonObject d;
    d["matchId"]    = QJsonValue(qint64(matchId));
    d["peerUserId"] = QJsonValue(qint64(peerUserId));
    sendEnvelope("MATCH_PUNCH_FAILED", d);
}

void LobbyClient::reportMatchFinished(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("MATCH_FINISHED", d);
}

void LobbyClient::sendBroadcastBegin(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("BROADCAST_BEGIN", d);
}

void LobbyClient::sendBroadcastData(quint64 matchId, const QByteArray& chunk, int liveFrame)
{
    if (chunk.isEmpty())
        return;
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    d["data"]    = QString::fromLatin1(chunk.toBase64());
    d["frame"]   = liveFrame; // broadcaster's live krec frame after this chunk
    sendEnvelope("BROADCAST_DATA", d);
}

void LobbyClient::sendBroadcastKeyframe(quint64 matchId, const QByteArray& savestate, int frame)
{
    if (savestate.isEmpty())
        return;
    // Split into chunks so each BROADCAST_KEYFRAME message stays under the server's
    // 1 MiB read limit (savestate is multi-MB even compressed). ~512 KiB raw per
    // chunk → ~683 KiB base64, comfortably under the limit.
    const int chunkBytes = 512 * 1024;
    const int total = static_cast<int>((savestate.size() + chunkBytes - 1) / chunkBytes);
    for (int i = 0; i < total; i++)
    {
        const int start = i * chunkBytes;
        const int len = qMin(chunkBytes, static_cast<int>(savestate.size()) - start);
        QJsonObject d;
        d["matchId"]    = QJsonValue(qint64(matchId));
        d["frame"]      = frame;
        d["chunkIndex"] = i;
        d["chunkCount"] = total;
        d["data"]       = QString::fromLatin1(savestate.mid(start, len).toBase64());
        sendEnvelope("BROADCAST_KEYFRAME", d);
    }
}

void LobbyClient::sendBroadcastEnd(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("BROADCAST_END", d);
}

void LobbyClient::startSpectate(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("SPECTATE_START", d);
}

void LobbyClient::stopSpectate(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("SPECTATE_STOP", d);
}

void LobbyClient::quickMatchJoin(const QString& romName, const QString& romMd5)
{
    QJsonObject rom;
    rom["name"]   = romName;
    rom["md5"]    = romMd5;
    rom["region"] = QString();
    sendEnvelope("QUICK_MATCH_JOIN", { {"rom", rom} });
}

void LobbyClient::quickMatchCancel()
{
    sendEnvelope("QUICK_MATCH_CANCEL");
}

LobbyClient::LobbyUser LobbyClient::parsePresenceUser(const QJsonObject& obj)
{
    LobbyUser u;
    u.id              = static_cast<quint64>(obj.value("id").toDouble());
    u.username        = obj.value("username").toString();
    u.state           = obj.value("state").toString();
    u.region          = obj.value("region").toString();
    u.country         = obj.value("country").toString();
    u.clientVersion   = obj.value("clientVersion").toString();
    u.connection      = obj.value("connection").toString();
    u.anonymous       = obj.value("anonymous").toBool();
    u.pingToServer    = static_cast<quint16>(obj.value("pingToServer").toInt());
    u.currentRoomId   = static_cast<quint64>(obj.value("currentRoomId").toDouble());
    u.currentRoomName = obj.value("currentRoomName").toString();
    u.searchingRom    = obj.value("searchingRom").toString();
    return u;
}

#endif // NETPLAY
