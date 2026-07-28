/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyClient.hpp"

#include <RMG-Core/Cheats.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Error.hpp>
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
#include <QNetworkInformation>
#include <QDateTime>
#include <QTimeZone>
#include <QCoreApplication>
#include <QtEndian>
#include <QRandomGenerator>
#include <QDebug>
#include <QFile>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <filesystem>

using namespace UserInterface::Dialog;

namespace
{
    constexpr int HEARTBEAT_INTERVAL_MS  = 15'000;
    constexpr int UDP_KEEPALIVE_INTERVAL = 20'000;

    constexpr char ANCHOR_MAGIC[4] = { 'R', 'M', 'G', 'K' };
    constexpr quint8 ANCHOR_OP_REGISTER    = 0x01;
    constexpr quint8 ANCHOR_OP_KEEPALIVE   = 0x02;
    constexpr quint8 ANCHOR_OP_PUNCH       = 0x03;
    constexpr quint8 ANCHOR_OP_PROBE       = 0x04; // client → peer: ping request
    constexpr quint8 ANCHOR_OP_PROBE_REPLY = 0x05; // peer → client: ping echo
    constexpr quint8 ANCHOR_OP_PREMATCH_MANIFEST = 0x06;
    constexpr quint8 ANCHOR_OP_PREMATCH_ACK      = 0x07;

    // Burst size for NAT punch-through. Defends against single-packet loss
    // and brief mapping-creation latency on consumer routers — NOT against
    // simultaneity failures (those need retries, which we don't do here yet).
    constexpr int ANCHOR_PUNCH_BURST = 10;
    constexpr int MATCH_ENDPOINT_PROBE_TIMEOUT_MS = 2000;
    constexpr int MATCH_ENDPOINT_PROBE_INTERVAL_MS = 90;
    constexpr int PREMATCH_ENDPOINT_PROBE_INTERVAL_MS = 200;

    // Lobby ping probes are coordinated by the server in both directions. Keep
    // sending for a short window so endpoint-dependent NAT mappings have time
    // to converge instead of relying on a single packet.
    constexpr int LOBBY_PING_PROBE_DURATION_MS = 1'500;
    constexpr int LOBBY_PING_PROBE_INTERVAL_MS = 100;
    constexpr int LOBBY_PING_TIMER_INTERVAL_MS = 25;
    constexpr int LOBBY_PING_LEARNED_ENDPOINT_TTL_MS = 5'000;
    constexpr qint64 LOBBY_PING_DIAGNOSTIC_MAX_BYTES = 100ll * 1024ll * 1024ll;

    // PROBE/PROBE_REPLY packet: [magic(4) | op(1) | senderUserId(8) | nonce(8)]
    constexpr int PROBE_PACKET_SIZE = 4 + 1 + 8 + 8;

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

    struct UdpEndpointCandidate
    {
        QHostAddress address;
        quint16 port = 0;
        QString kind;
    };

    QString endpointKey(const QHostAddress& address, quint16 port)
    {
        return address.toString() + QLatin1Char(':') + QString::number(port);
    }

    void appendEndpointCandidate(QList<UdpEndpointCandidate>& out,
                                 const QString& ip, quint16 port,
                                 const QString& kind)
    {
        const QHostAddress address(ip);
        if (address.isNull() || port == 0)
            return;

        const QString key = endpointKey(address, port);
        for (const auto& candidate : out)
        {
            if (endpointKey(candidate.address, candidate.port) == key)
                return;
        }

        UdpEndpointCandidate candidate;
        candidate.address = address;
        candidate.port = port;
        candidate.kind = kind;
        out.append(candidate);
    }

    bool parseEndpointString(const QString& endpoint, UdpEndpointCandidate& out,
                             const QString& kind)
    {
        if (endpoint.isEmpty())
            return false;

        const int colon = endpoint.lastIndexOf(':');
        if (colon <= 0)
            return false;

        bool portOk = false;
        const uint parsedPort = endpoint.mid(colon + 1).toUInt(&portOk);
        const QHostAddress address(endpoint.left(colon));
        if (!portOk || parsedPort == 0 || parsedPort > 65535 || address.isNull())
            return false;

        out.address = address;
        out.port = static_cast<quint16>(parsedPort);
        out.kind = kind;
        return true;
    }

    QList<UdpEndpointCandidate> peerEndpointCandidates(
        const LobbyClient::LobbyMatchPeer& local,
        const LobbyClient::LobbyMatchPeer& peer)
    {
        QList<UdpEndpointCandidate> out;
        const bool samePublicIp = !local.publicIp.isEmpty() &&
                                  !peer.publicIp.isEmpty() &&
                                  local.publicIp == peer.publicIp;

        if (samePublicIp && !peer.localIp.isEmpty())
        {
            // New servers provide the exact LAN-side game socket port. When
            // talking to an old server, retain the previous local-IP/public-
            // port candidate as a compatibility fallback.
            const quint16 lanPort = peer.localPort != 0 ? peer.localPort : peer.publicPort;
            appendEndpointCandidate(out, peer.localIp, lanPort,
                                    peer.localPort != 0 ? QStringLiteral("local")
                                                        : QStringLiteral("local-legacy"));
        }

        appendEndpointCandidate(out, peer.publicIp, peer.publicPort, QStringLiteral("public"));
        return out;
    }

    QByteArray buildProbePacket(quint64 senderUserId, quint64 nonce, quint8 op = ANCHOR_OP_PROBE)
    {
        QByteArray packet;
        packet.reserve(PROBE_PACKET_SIZE);
        packet.append(ANCHOR_MAGIC, 4);
        packet.append(static_cast<char>(op));
        const quint64 senderBE = qToBigEndian(senderUserId);
        packet.append(reinterpret_cast<const char*>(&senderBE), sizeof(senderBE));
        const quint64 nonceBE = qToBigEndian(nonce);
        packet.append(reinterpret_cast<const char*>(&nonceBE), sizeof(nonceBE));
        return packet;
    }

    bool readProbePacket(const QByteArray& packet, quint64& senderUserId, quint64& nonce)
    {
        if (packet.size() != PROBE_PACKET_SIZE ||
            std::memcmp(packet.constData(), ANCHOR_MAGIC, 4) != 0)
        {
            return false;
        }

        quint64 senderBE = 0;
        quint64 nonceBE = 0;
        std::memcpy(&senderBE, packet.constData() + 5, sizeof(senderBE));
        std::memcpy(&nonceBE, packet.constData() + 13, sizeof(nonceBE));
        senderUserId = qFromBigEndian(senderBE);
        nonce = qFromBigEndian(nonceBE);
        return true;
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
    m_pingDiagnosticFile = new QFile(this);

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

    m_pingProbeBurstTimer = new QTimer(this);
    m_pingProbeBurstTimer->setInterval(LOBBY_PING_TIMER_INTERVAL_MS);
    connect(m_pingProbeBurstTimer, &QTimer::timeout, this, &LobbyClient::onPingProbeBurstTimer);
}

LobbyClient::~LobbyClient()
{
    disconnectFromServer();
}

void LobbyClient::startPingDiagnosticLog()
{
    // Opt-in via Settings → Rollback → Logging; read here so it takes effect
    // on the next lobby connect without any live plumbing.
    m_pingDiagnosticsEnabled = CoreSettingsGetBoolValue(SettingsID::Rollback_PingDiagnostics);
    if (!m_pingDiagnosticsEnabled)
        return;
    if (!m_pingDiagnosticFile)
        return;

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

void LobbyClient::writePingDiagnostic(const QString& event, const QString& details)
{
    if (!m_pingDiagnosticFile || !m_pingDiagnosticFile->isOpen())
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

    const QByteArray encoded = line.toUtf8();
    if (m_pingDiagnosticFile->size() + encoded.size() > LOBBY_PING_DIAGNOSTIC_MAX_BYTES)
    {
        const QByteArray stopLine = QStringLiteral(
            "%1 elapsed_ms=%2 self=%3 local_udp=%4 event=LOG_STOP reason=max_size limit_bytes=%5\n")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
            .arg(m_pingDiagnosticStartMs == 0 ? 0 : nowMs - m_pingDiagnosticStartMs)
            .arg(m_selfUserId)
            .arg(localUdpPort())
            .arg(LOBBY_PING_DIAGNOSTIC_MAX_BYTES)
            .toUtf8();
        m_pingDiagnosticFile->write(stopLine);
        m_pingDiagnosticFile->flush();
        m_pingDiagnosticFile->close();
        qWarning() << "Rollback lobby ping diagnostic log reached size limit"
                   << LOBBY_PING_DIAGNOSTIC_MAX_BYTES;
        return;
    }

    m_pingDiagnosticFile->write(encoded);
    m_pingDiagnosticFile->flush();
}

QString LobbyClient::pingUserLabel(quint64 userId) const
{
    const auto it = m_users.constFind(userId);
    if (it == m_users.constEnd())
        return QStringLiteral("%1:<unknown>").arg(userId);
    return QStringLiteral("%1:%2").arg(userId).arg(it->username);
}

QString LobbyClient::pendingPingSummary() const
{
    QStringList entries;
    entries.reserve(m_pendingProbes.size());
    for (auto it = m_pendingProbes.constBegin(); it != m_pendingProbes.constEnd(); ++it)
    {
        entries.append(QStringLiteral("nonce=%1 target=%2 origin=%3 endpoints=[%4] rounds=%5 datagrams=%6")
                           .arg(it.key())
                           .arg(pingUserLabel(it->targetUserId))
                           .arg(it->origin)
                           .arg(it->endpoints.join(QLatin1Char(',')))
                           .arg(it->sendRounds)
                           .arg(it->datagramsSent));
    }
    return entries.join(QStringLiteral(" | "));
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
    m_recentInboundPingEndpoints.clear();

    startPingDiagnosticLog();

    qInfo() << "Rollback lobby local IPv4 detection"
            << "localIp" << (m_pendingLocalIp.isEmpty() ? QString("<none>") : m_pendingLocalIp);
    writePingDiagnostic(QStringLiteral("CONNECT_BEGIN"),
                        QStringLiteral("ws=%1 username=%2 detected_local_ip=%3")
                            .arg(wsUrl,
                                 username,
                                 m_pendingLocalIp.isEmpty() ? QStringLiteral("<none>") : m_pendingLocalIp));

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

    writePingDiagnostic(QStringLiteral("ANCHOR_CONFIG"),
                        QStringLiteral("host=%1 port=%2")
                            .arg(m_udpAnchorHost)
                            .arg(m_udpAnchorPort));

    // Bind the eventual GekkoNet port before HELLO so updated servers can
    // advertise the exact LAN-side endpoint immediately. Registration still
    // waits for HELLO_OK because it requires the assigned user id.
    ensureUdpAnchorBound();

    setState(ConnectionState::Connecting);
    m_ws->open(url);
}

void LobbyClient::disconnectFromServer()
{
    writePingDiagnostic(QStringLiteral("DISCONNECT_REQUEST"),
                        QStringLiteral("ws_state=%1 udp_state=%2 pending_count=%3 pending={%4}")
                            .arg(m_ws ? static_cast<int>(m_ws->state()) : -1)
                            .arg(m_udp ? static_cast<int>(m_udp->state()) : -1)
                            .arg(m_pendingProbes.size())
                            .arg(pendingPingSummary()));
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    m_pingProbeBurstTimer->stop();
    m_pendingProbes.clear();
    m_recentInboundPingEndpoints.clear();
    if (m_ws && m_ws->state() != QAbstractSocket::UnconnectedState)
    {
        m_ws->close();
    }
    if (m_udp)
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

// Best-effort detection of this machine's transport medium ("wifi"/"lan"/...),
// reported in HELLO so peers can gauge connection-quality risk Slippi-style.
// Returns an empty string when the OS backend can't say.
static QString detectTransportMedium()
{
    if (!QNetworkInformation::instance() &&
        !QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::TransportMedium))
        return QString();

    const auto* info = QNetworkInformation::instance();
    if (info == nullptr)
        return QString();

    switch (info->transportMedium())
    {
    case QNetworkInformation::TransportMedium::Ethernet:  return QStringLiteral("lan");
    case QNetworkInformation::TransportMedium::WiFi:      return QStringLiteral("wifi");
    case QNetworkInformation::TransportMedium::Cellular:  return QStringLiteral("cellular");
    case QNetworkInformation::TransportMedium::Bluetooth: return QStringLiteral("bluetooth");
    default:                                              return QString();
    }
}

void LobbyClient::onWsConnected()
{
    setState(ConnectionState::Authenticating);
    writePingDiagnostic(QStringLiteral("WS_CONNECTED"),
                        QStringLiteral("state=%1")
                            .arg(static_cast<int>(m_ws->state())));

    QJsonObject data;
    data["username"]      = m_pendingUsername;
    data["clientVersion"] = QString::fromStdString(CoreGetVersion());
    const QString transport = detectTransportMedium();
    if (!transport.isEmpty())
        data["connection"] = transport;
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
    if (!m_pendingLocalIp.isEmpty())
        data["localIp"] = m_pendingLocalIp;
    const quint16 boundPort = localUdpPort();
    if (boundPort != 0)
        data["localPort"] = static_cast<int>(boundPort);

    writePingDiagnostic(QStringLiteral("HELLO_SEND"),
                        QStringLiteral("username=%1 local_ip=%2 local_port=%3 transport=%4")
                            .arg(m_pendingUsername,
                                 m_pendingLocalIp.isEmpty() ? QStringLiteral("<none>") : m_pendingLocalIp)
                            .arg(boundPort)
                            .arg(transport.isEmpty() ? QStringLiteral("<unknown>") : transport));
    sendEnvelope("HELLO", data, "hello-1");
}

void LobbyClient::onWsDisconnected()
{
    writePingDiagnostic(QStringLiteral("WS_DISCONNECTED"),
                        QStringLiteral("pending_count=%1 pending={%2}")
                            .arg(m_pendingProbes.size())
                            .arg(pendingPingSummary()));
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    m_pingProbeBurstTimer->stop();
    m_pendingProbes.clear();
    m_recentInboundPingEndpoints.clear();
    m_isModerator = false; // role is per-connection; must re-auth after reconnect
    // User ids restart when the server does — drop measurements so a recycled
    // id can't inherit another player's ping history.
    m_measuredPing.clear();
    m_pingSamples.clear();
    setState(ConnectionState::Disconnected);
}

void LobbyClient::onWsErrorOccurred()
{
    writePingDiagnostic(QStringLiteral("WS_ERROR"),
                        QStringLiteral("code=%1 text=%2")
                            .arg(static_cast<int>(m_ws->error()))
                            .arg(m_ws->errorString()));
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

    writePingDiagnostic(QStringLiteral("HELLO_OK"),
                        QStringLiteral("username=%1 observed_ip=%2 region=%3 anchor=%4:%5 bound_local_ip=%6 bound_local_port=%7")
                            .arg(m_pendingUsername,
                                 m_observedIp,
                                 m_region,
                                 m_udpAnchorHost)
                            .arg(m_udpAnchorPort)
                            .arg(m_pendingLocalIp.isEmpty() ? QStringLiteral("<none>") : m_pendingLocalIp)
                            .arg(localUdpPort()));

    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();
    initiateUdpAnchor();
}

void LobbyClient::handleHelloFail(const QJsonObject& data)
{
    const QString reason = data.value("reason").toString();
    writePingDiagnostic(QStringLiteral("HELLO_FAIL"), QStringLiteral("reason=%1").arg(reason));
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
    QStringList roster;
    for (const auto& v : data.value("users").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
        roster.append(QStringLiteral("%1:%2 state=%3 room=%4")
                          .arg(u.id)
                          .arg(u.username)
                          .arg(u.state)
                          .arg(u.currentRoomId));
    }
    writePingDiagnostic(QStringLiteral("PRESENCE_FULL"),
                        QStringLiteral("count=%1 users={%2}")
                            .arg(m_users.size())
                            .arg(roster.join(QStringLiteral(" | "))));
    emit presenceFull();
}

void LobbyClient::handlePresenceDelta(const QJsonObject& data)
{
    for (const auto& v : data.value("added").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
        writePingDiagnostic(QStringLiteral("PRESENCE_ADD"),
                            QStringLiteral("user=%1:%2 state=%3 room=%4")
                                .arg(u.id)
                                .arg(u.username)
                                .arg(u.state)
                                .arg(u.currentRoomId));
        emit userAdded(u.id);
    }
    for (const auto& v : data.value("updated").toArray())
    {
        const auto u = parsePresenceUser(v.toObject());
        m_users.insert(u.id, u);
        writePingDiagnostic(QStringLiteral("PRESENCE_UPDATE"),
                            QStringLiteral("user=%1:%2 state=%3 room=%4")
                                .arg(u.id)
                                .arg(u.username)
                                .arg(u.state)
                                .arg(u.currentRoomId));
        emit userUpdated(u.id);
    }
    for (const auto& v : data.value("removed").toArray())
    {
        const quint64 id = static_cast<quint64>(v.toDouble());
        const QString removedLabel = pingUserLabel(id);
        bool hadPending = false;
        for (auto it = m_pendingProbes.constBegin(); it != m_pendingProbes.constEnd(); ++it)
        {
            if (it->targetUserId == id)
            {
                hadPending = true;
                break;
            }
        }
        writePingDiagnostic(QStringLiteral("PRESENCE_REMOVE"),
                            QStringLiteral("user=%1 had_pending=%2 pending={%3}")
                                .arg(removedLabel)
                                .arg(hadPending ? 1 : 0)
                                .arg(hadPending ? pendingPingSummary() : QString()));
        for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); )
        {
            if (it->targetUserId == id)
                it = m_pendingProbes.erase(it);
            else
                ++it;
        }
        m_recentInboundPingEndpoints.remove(id);
        m_measuredPing.remove(id);
        m_pingSamples.remove(id);
        if (m_pendingProbes.isEmpty())
            m_pingProbeBurstTimer->stop();
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
    writePingDiagnostic(QStringLiteral("ROOM_LEFT"),
                        QStringLiteral("reason=%1 pending_count=%2 pending={%3}")
                            .arg(data.value("reason").toString())
                            .arg(m_pendingProbes.size())
                            .arg(pendingPingSummary()));
    emit roomLeft(data.value("reason").toString());
}

void LobbyClient::handleRoomState(const QJsonObject& data)
{
    writePingDiagnostic(QStringLiteral("ROOM_STATE"),
                        QStringLiteral("json=%1")
                            .arg(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))));
    emit roomStateChanged(data);
}

void LobbyClient::handleRoomJoinOk(const QJsonObject& data)
{
    writePingDiagnostic(QStringLiteral("ROOM_JOIN_OK"),
                        QStringLiteral("room_id=%1 json=%2")
                            .arg(static_cast<quint64>(data.value("id").toDouble()))
                            .arg(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))));
    emit roomJoinOk(static_cast<quint64>(data.value("id").toDouble()));
    emit roomStateChanged(data);
}

void LobbyClient::handleRoomJoinFail(const QJsonObject& data)
{
    emit roomJoinFailed(data.value("reason").toString());
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

void LobbyClient::handlePingProbeReply(const QJsonObject& data)
{
    const quint64 uid = static_cast<quint64>(data.value("targetUserId").toDouble());
    const QString targetEndpoint = data.value("targetEndpoint").toString();
    const QString publicEndpoint = data.value("publicEndpoint").toString();
    const QString localEndpoint  = data.value("localEndpoint").toString();
    writePingDiagnostic(QStringLiteral("PING_SERVER_START"),
                        QStringLiteral("target=%1 preferred=%2 public=%3 local=%4 udp_state=%5 pending_count=%6")
                            .arg(pingUserLabel(uid),
                                 targetEndpoint.isEmpty() ? QStringLiteral("<empty>") : targetEndpoint,
                                 publicEndpoint.isEmpty() ? QStringLiteral("<empty>") : publicEndpoint,
                                 localEndpoint.isEmpty() ? QStringLiteral("<empty>") : localEndpoint)
                            .arg(m_udp ? static_cast<int>(m_udp->state()) : -1)
                            .arg(m_pendingProbes.size()));
    emit pingProbeReply(uid, targetEndpoint);

    if (!m_udp ||
        m_udp->state() == QAbstractSocket::UnconnectedState ||
        m_selfUserId == 0)
    {
        writePingDiagnostic(QStringLiteral("PING_SERVER_START_IGNORED"),
                            QStringLiteral("target=%1 reason=udp_unavailable_or_no_self_id")
                                .arg(pingUserLabel(uid)));
        return;
    }

    QList<UdpEndpointCandidate> candidates;
    const auto appendParsed = [&](const QString& endpoint, const QString& kind) {
        UdpEndpointCandidate candidate;
        if (parseEndpointString(endpoint, candidate, kind))
            appendEndpointCandidate(candidates, candidate.address.toString(), candidate.port, candidate.kind);
    };

    // New servers provide both candidates. targetEndpoint remains first for
    // old-server compatibility, while duplicate endpoints are collapsed.
    appendParsed(targetEndpoint, QStringLiteral("target"));
    appendParsed(localEndpoint, QStringLiteral("local"));
    appendParsed(publicEndpoint, QStringLiteral("public"));
    if (candidates.isEmpty())
    {
        writePingDiagnostic(QStringLiteral("PING_SERVER_START_IGNORED"),
                            QStringLiteral("target=%1 reason=no_valid_candidates")
                                .arg(pingUserLabel(uid)));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QStringList endpointStrings;
    for (const auto& candidate : candidates)
        endpointStrings.append(endpointKey(candidate.address, candidate.port));

    // UDP can beat the reciprocal WebSocket start. Preserve the most recently
    // observed source endpoint for a few seconds and merge it when the server
    // start arrives, rather than spawning an unbounded reciprocal burst from
    // every inbound probe.
    const auto learnedIt = m_recentInboundPingEndpoints.find(uid);
    if (learnedIt != m_recentInboundPingEndpoints.end())
    {
        const qint64 learnedAgeMs = nowMs - learnedIt->seenMs;
        if (learnedAgeMs >= 0 && learnedAgeMs <= LOBBY_PING_LEARNED_ENDPOINT_TTL_MS)
        {
            if (!endpointStrings.contains(learnedIt->endpoint))
                endpointStrings.append(learnedIt->endpoint);
            writePingDiagnostic(QStringLiteral("PING_USE_CACHED_ENDPOINT"),
                                QStringLiteral("target=%1 endpoint=%2 age_ms=%3")
                                    .arg(pingUserLabel(uid))
                                    .arg(learnedIt->endpoint)
                                    .arg(learnedAgeMs));
        }
        else
        {
            writePingDiagnostic(QStringLiteral("PING_DROP_CACHED_ENDPOINT"),
                                QStringLiteral("target=%1 endpoint=%2 age_ms=%3 reason=expired")
                                    .arg(pingUserLabel(uid))
                                    .arg(learnedIt->endpoint)
                                    .arg(learnedAgeMs));
            m_recentInboundPingEndpoints.erase(learnedIt);
        }
    }

    // The updated server sends a reciprocal PING_PROBE_REPLY to the target, so
    // both peers may receive near-simultaneous starts for the same pair. Merge
    // those into one in-flight burst instead of multiplying traffic.
    for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); ++it)
    {
        if (it->targetUserId != uid)
            continue;
        QStringList addedEndpoints;
        for (const QString& endpoint : endpointStrings)
        {
            if (!it->endpoints.contains(endpoint))
            {
                it->endpoints.append(endpoint);
                addedEndpoints.append(endpoint);
            }
        }
        if (!addedEndpoints.isEmpty())
            it->nextSendMs = 0;
        writePingDiagnostic(QStringLiteral("PING_BURST_MERGE"),
                            QStringLiteral("nonce=%1 target=%2 origin=%3 added=[%4] all=[%5] deadline_in_ms=%6")
                                .arg(it.key())
                                .arg(pingUserLabel(uid))
                                .arg(it->origin)
                                .arg(addedEndpoints.join(QLatin1Char(',')))
                                .arg(it->endpoints.join(QLatin1Char(',')))
                                .arg(it->deadlineMs - nowMs));
        if (!m_pingProbeBurstTimer->isActive())
            m_pingProbeBurstTimer->start();
        if (!addedEndpoints.isEmpty())
            onPingProbeBurstTimer();
        return;
    }

    quint64 nonce = 0;
    do
    {
        nonce = QRandomGenerator::global()->generate64();
    }
    while (nonce == 0 || m_pendingProbes.contains(nonce));

    ProbeInFlight in;
    in.targetUserId = uid;
    in.createdMs    = nowMs;
    in.nextSendMs   = 0;
    in.deadlineMs   = nowMs + LOBBY_PING_PROBE_DURATION_MS;
    in.origin       = QStringLiteral("server-start");
    in.endpoints    = endpointStrings;
    m_pendingProbes.insert(nonce, in);

    writePingDiagnostic(QStringLiteral("PING_BURST_CREATE"),
                        QStringLiteral("nonce=%1 target=%2 origin=%3 endpoints=[%4] duration_ms=%5")
                            .arg(nonce)
                            .arg(pingUserLabel(uid))
                            .arg(in.origin)
                            .arg(in.endpoints.join(QLatin1Char(',')))
                            .arg(LOBBY_PING_PROBE_DURATION_MS));

    if (!m_pingProbeBurstTimer->isActive())
        m_pingProbeBurstTimer->start();
    onPingProbeBurstTimer();
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
        p.localPort  = static_cast<quint16>(o.value("localPort").toInt());
        p.slot       = o.value("slot").toInt();
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
    sendEnvelope("HEARTBEAT", data);
}

// -------- UDP anchor --------

bool LobbyClient::ensureUdpAnchorBound()
{
    if (!m_udp)
    {
        writePingDiagnostic(QStringLiteral("UDP_BIND_FAIL"), QStringLiteral("reason=no_socket"));
        return false;
    }

    if (m_udp->state() != QAbstractSocket::UnconnectedState)
    {
        writePingDiagnostic(QStringLiteral("UDP_BIND_REUSE"),
                            QStringLiteral("state=%1 local_address=%2 local_port=%3")
                                .arg(static_cast<int>(m_udp->state()))
                                .arg(m_udp->localAddress().toString())
                                .arg(m_udp->localPort()));
        return m_udp->localPort() != 0;
    }

    if (!m_udp->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress))
    {
        qWarning() << "lobby: udp bind failed:" << m_udp->errorString();
        writePingDiagnostic(QStringLiteral("UDP_BIND_FAIL"),
                            QStringLiteral("error=%1 code=%2")
                                .arg(m_udp->errorString())
                                .arg(static_cast<int>(m_udp->error())));
        return false;
    }

    qInfo() << "Rollback lobby UDP anchor bound"
            << "localIp" << (m_pendingLocalIp.isEmpty() ? QString("<none>") : m_pendingLocalIp)
            << "localPort" << m_udp->localPort();
    writePingDiagnostic(QStringLiteral("UDP_BOUND"),
                        QStringLiteral("socket_address=%1 detected_local_ip=%2 local_port=%3 share_address=1")
                            .arg(m_udp->localAddress().toString(),
                                 m_pendingLocalIp.isEmpty() ? QStringLiteral("<none>") : m_pendingLocalIp)
                            .arg(m_udp->localPort()));
    return true;
}

void LobbyClient::initiateUdpAnchor()
{
    if (!ensureUdpAnchorBound())
        return;

    sendUdpRegister();
    m_udpKeepaliveTimer->start();
}

void LobbyClient::sendUdpRegister()
{
    if (m_selfUserId == 0)
    {
        writePingDiagnostic(QStringLiteral("UDP_REGISTER_SKIP"), QStringLiteral("reason=no_self_id"));
        return;
    }
    QByteArray pkt;
    pkt.reserve(15);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_REGISTER));
    quint64 idBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&idBE), sizeof(idBE));
    const quint16 portBE = qToBigEndian(localUdpPort());
    pkt.append(reinterpret_cast<const char*>(&portBE), sizeof(portBE));
    const qint64 written = m_udp->writeDatagram(pkt, QHostAddress(m_udpAnchorHost), m_udpAnchorPort);
    writePingDiagnostic(QStringLiteral("UDP_REGISTER_SEND"),
                        QStringLiteral("anchor=%1:%2 advertised_local_port=%3 bytes=%4 result=%5 error=%6")
                            .arg(m_udpAnchorHost)
                            .arg(m_udpAnchorPort)
                            .arg(localUdpPort())
                            .arg(pkt.size())
                            .arg(written)
                            .arg(written == pkt.size() ? QStringLiteral("ok") : m_udp->errorString()));
}

void LobbyClient::sendUdpKeepalive()
{
    if (m_selfUserId == 0)
        return;
    QByteArray pkt;
    pkt.reserve(15);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_KEEPALIVE));
    quint64 idBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&idBE), sizeof(idBE));
    const quint16 portBE = qToBigEndian(localUdpPort());
    pkt.append(reinterpret_cast<const char*>(&portBE), sizeof(portBE));
    const qint64 written = m_udp->writeDatagram(pkt, QHostAddress(m_udpAnchorHost), m_udpAnchorPort);
    writePingDiagnostic(QStringLiteral("UDP_KEEPALIVE_SEND"),
                        QStringLiteral("anchor=%1:%2 advertised_local_port=%3 result=%4 error=%5")
                            .arg(m_udpAnchorHost)
                            .arg(m_udpAnchorPort)
                            .arg(localUdpPort())
                            .arg(written)
                            .arg(written == pkt.size() ? QStringLiteral("ok") : m_udp->errorString()));
}

void LobbyClient::onUdpKeepaliveTimer()
{
    sendUdpKeepalive();
}

void LobbyClient::onPingProbeBurstTimer()
{
    if (m_inPrematchSync)
        return;

    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState || m_selfUserId == 0)
    {
        writePingDiagnostic(QStringLiteral("PING_BURSTS_CLEARED"),
                            QStringLiteral("reason=udp_unavailable_or_no_self_id count=%1 pending={%2}")
                                .arg(m_pendingProbes.size())
                                .arg(pendingPingSummary()));
        m_pendingProbes.clear();
        m_pingProbeBurstTimer->stop();
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); )
    {
        if (nowMs > it->deadlineMs)
        {
            writePingDiagnostic(QStringLiteral("PING_BURST_EXPIRE"),
                                QStringLiteral("nonce=%1 target=%2 origin=%3 age_ms=%4 rounds=%5 datagrams=%6 endpoints=[%7]")
                                    .arg(it.key())
                                    .arg(pingUserLabel(it->targetUserId))
                                    .arg(it->origin)
                                    .arg(nowMs - it->createdMs)
                                    .arg(it->sendRounds)
                                    .arg(it->datagramsSent)
                                    .arg(it->endpoints.join(QLatin1Char(','))));
            it = m_pendingProbes.erase(it);
            continue;
        }

        if (nowMs >= it->nextSendMs)
        {
            const QByteArray packet = buildProbePacket(m_selfUserId, it.key());
            bool sentAny = false;
            int successfulDatagrams = 0;
            QStringList sendResults;
            for (const QString& endpoint : it->endpoints)
            {
                UdpEndpointCandidate candidate;
                if (!parseEndpointString(endpoint, candidate, QStringLiteral("ping")))
                {
                    sendResults.append(endpoint + QStringLiteral("=invalid"));
                    continue;
                }
                const qint64 written = m_udp->writeDatagram(packet, candidate.address, candidate.port);
                if (written == packet.size())
                {
                    sentAny = true;
                    successfulDatagrams++;
                    sendResults.append(endpoint + QStringLiteral("=ok"));
                }
                else
                {
                    sendResults.append(QStringLiteral("%1=fail(%2:%3)")
                                           .arg(endpoint)
                                           .arg(written)
                                           .arg(m_udp->errorString()));
                }
            }
            // Stamp only the FIRST actual send. Replies are matched by nonce,
            // which every resend shares — measuring a reply to packet #1
            // against the latest resend time under-reads any RTT above the
            // resend interval (a real 150 ms would display as 50 ms). A route
            // that only opens mid-burst over-reads once instead, which the
            // median sample window absorbs.
            if (sentAny && it->sendMs == 0)
                it->sendMs = nowMs;
            it->lastSendMs = nowMs;
            it->sendRounds++;
            it->datagramsSent += successfulDatagrams;
            writePingDiagnostic(QStringLiteral("PING_SEND_ROUND"),
                                QStringLiteral("nonce=%1 target=%2 origin=%3 round=%4 successful=%5 endpoints=[%6] results=[%7] first_send_age_ms=%8")
                                    .arg(it.key())
                                    .arg(pingUserLabel(it->targetUserId))
                                    .arg(it->origin)
                                    .arg(it->sendRounds)
                                    .arg(successfulDatagrams)
                                    .arg(it->endpoints.join(QLatin1Char(',')))
                                    .arg(sendResults.join(QLatin1Char(',')))
                                    .arg(it->sendMs == 0 ? -1 : nowMs - it->sendMs));
            it->nextSendMs = nowMs + LOBBY_PING_PROBE_INTERVAL_MS;
        }
        ++it;
    }

    if (m_pendingProbes.isEmpty())
        m_pingProbeBurstTimer->stop();
}

void LobbyClient::resolvePeerEndpoints(QList<LobbyMatchPeer>& peers)
{
    if (!m_udp || m_udp->state() == QAbstractSocket::UnconnectedState || m_selfUserId == 0)
        return;

    int localIndex = -1;
    for (int i = 0; i < peers.size(); i++)
    {
        if (peers[i].userId == m_selfUserId)
        {
            localIndex = i;
            break;
        }
    }
    if (localIndex < 0)
        return;

    const LobbyMatchPeer local = peers[localIndex];
    QHash<quint64, int> pendingPeerByNonce;
    QHash<quint64, QList<UdpEndpointCandidate>> candidatesByNonce;

    for (int i = 0; i < peers.size(); i++)
    {
        LobbyMatchPeer& peer = peers[i];
        if (peer.userId == m_selfUserId)
            continue;

        const QList<UdpEndpointCandidate> candidates = peerEndpointCandidates(local, peer);
        if (candidates.isEmpty())
            continue;

        // Always retain a deterministic fallback. With an old server this is
        // exactly the client's prior same-public-IP behavior; with a new server
        // it uses the exact LAN port when available.
        peer.selectedIp = candidates.first().address.toString();
        peer.selectedPort = candidates.first().port;
        peer.selectedKind = candidates.first().kind;
        peer.selectedEndpointVerified = false;

        // Probe even when there is only one advertised candidate. A peer behind
        // endpoint-dependent NAT may contact us from a different translated port,
        // and an inbound probe lets us learn that usable source endpoint.
        quint64 nonce = 0;
        do
        {
            nonce = QRandomGenerator::global()->generate64();
        }
        while (nonce == 0 || pendingPeerByNonce.contains(nonce) || m_pendingProbes.contains(nonce));

        pendingPeerByNonce.insert(nonce, i);
        candidatesByNonce.insert(nonce, candidates);
    }

    if (pendingPeerByNonce.isEmpty())
        return;

    struct SynchronousUdpGuard
    {
        LobbyClient& client;
        explicit SynchronousUdpGuard(LobbyClient& client) : client(client) { client.m_inPrematchSync = true; }
        ~SynchronousUdpGuard() { client.m_inPrematchSync = false; }
    } guard(*this);

    qInfo() << "Rollback lobby endpoint race begin"
            << "peers" << pendingPeerByNonce.size()
            << "localPort" << m_udp->localPort();

    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + MATCH_ENDPOINT_PROBE_TIMEOUT_MS;
    qint64 nextSendMs = 0;

    while (!pendingPeerByNonce.isEmpty() && QDateTime::currentMSecsSinceEpoch() < deadlineMs)
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs >= nextSendMs)
        {
            for (auto it = pendingPeerByNonce.constBegin(); it != pendingPeerByNonce.constEnd(); ++it)
            {
                const quint64 nonce = it.key();
                const QByteArray packet = buildProbePacket(m_selfUserId, nonce);
                const auto candidatesIt = candidatesByNonce.constFind(nonce);
                if (candidatesIt == candidatesByNonce.constEnd())
                    continue;

                for (const auto& candidate : candidatesIt.value())
                    m_udp->writeDatagram(packet, candidate.address, candidate.port);
            }
            nextSendMs = nowMs + MATCH_ENDPOINT_PROBE_INTERVAL_MS;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!m_udp->hasPendingDatagrams())
            m_udp->waitForReadyRead(20);

        while (m_udp->hasPendingDatagrams())
        {
            QByteArray datagram;
            QHostAddress sender;
            quint16 senderPort = 0;
            datagram.resize(int(m_udp->pendingDatagramSize()));
            m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

            if (datagram.size() != PROBE_PACKET_SIZE ||
                std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0)
            {
                continue;
            }

            const quint8 op = static_cast<quint8>(datagram.at(4));
            if (op == ANCHOR_OP_PROBE)
            {
                quint64 senderUserId = 0;
                quint64 nonce = 0;
                if (!readProbePacket(datagram, senderUserId, nonce))
                    continue;

                // An inbound probe is proof that this exact source endpoint can
                // reach us. Learn it even when our probe to the server-observed
                // endpoint never received a reply (common with symmetric NAT).
                for (int peerIndex = 0; peerIndex < peers.size(); ++peerIndex)
                {
                    LobbyMatchPeer& peer = peers[peerIndex];
                    if (peer.userId != senderUserId || peer.userId == m_selfUserId)
                        continue;

                    peer.selectedIp = sender.toString();
                    peer.selectedPort = senderPort;
                    peer.selectedKind = QStringLiteral("inbound-probe");
                    peer.selectedEndpointVerified = true;

                    for (auto pendingIt = pendingPeerByNonce.begin(); pendingIt != pendingPeerByNonce.end();)
                    {
                        if (pendingIt.value() == peerIndex)
                        {
                            candidatesByNonce.remove(pendingIt.key());
                            pendingIt = pendingPeerByNonce.erase(pendingIt);
                        }
                        else
                        {
                            ++pendingIt;
                        }
                    }

                    qInfo() << "Rollback lobby endpoint race selected from inbound probe"
                            << "peerUserId" << peer.userId
                            << "slot" << peer.slot
                            << "endpoint" << peer.selectedIp << peer.selectedPort;
                    break;
                }

                const QByteArray reply = buildProbePacket(m_selfUserId, nonce, ANCHOR_OP_PROBE_REPLY);
                m_udp->writeDatagram(reply, sender, senderPort);
                continue;
            }

            if (op != ANCHOR_OP_PROBE_REPLY)
                continue;

            quint64 senderUserId = 0;
            quint64 nonce = 0;
            if (!readProbePacket(datagram, senderUserId, nonce))
                continue;

            auto routeIt = pendingPeerByNonce.find(nonce);
            if (routeIt != pendingPeerByNonce.end())
            {
                LobbyMatchPeer& peer = peers[routeIt.value()];
                if (senderUserId != peer.userId)
                    continue;

                peer.selectedIp = sender.toString();
                peer.selectedPort = senderPort;
                peer.selectedKind = QStringLiteral("reply");
                peer.selectedEndpointVerified = true;

                const auto candidatesIt = candidatesByNonce.constFind(nonce);
                if (candidatesIt != candidatesByNonce.constEnd())
                {
                    for (const auto& candidate : candidatesIt.value())
                    {
                        if (candidate.address == sender && candidate.port == senderPort)
                        {
                            peer.selectedKind = candidate.kind;
                            break;
                        }
                    }
                }

                qInfo() << "Rollback lobby endpoint race selected"
                        << "peerUserId" << peer.userId
                        << "slot" << peer.slot
                        << "kind" << peer.selectedKind
                        << "endpoint" << peer.selectedIp << peer.selectedPort;

                candidatesByNonce.remove(nonce);
                pendingPeerByNonce.erase(routeIt);
                continue;
            }

            // A normal lobby ping may complete while the match-start race owns
            // the socket. Preserve that result instead of dropping the reply.
            auto pingIt = m_pendingProbes.find(nonce);
            if (pingIt != m_pendingProbes.end() && senderUserId == pingIt->targetUserId)
            {
                const int rttMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - pingIt->sendMs);
                const quint64 uid = pingIt->targetUserId;
                m_pendingProbes.erase(pingIt);
                emit pingProbeMeasured(uid, recordPingSample(uid, rttMs));
            }
        }
    }

    for (auto it = pendingPeerByNonce.constBegin(); it != pendingPeerByNonce.constEnd(); ++it)
    {
        const LobbyMatchPeer& peer = peers[it.value()];
        qWarning() << "Rollback lobby endpoint race timed out; using fallback"
                   << "peerUserId" << peer.userId
                   << "slot" << peer.slot
                   << "kind" << peer.selectedKind
                   << "endpoint" << peer.selectedIp << peer.selectedPort;
    }
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

    LobbyMatchPeer local{};
    bool foundLocal = false;
    for (const auto& peer : peers)
    {
        if (peer.userId == m_selfUserId)
        {
            local = peer;
            foundLocal = true;
            break;
        }
    }

    for (const auto& p : peers)
    {
        if (p.userId == m_selfUserId)
            continue;

        QList<UdpEndpointCandidate> candidates;
        appendEndpointCandidate(candidates, p.selectedIp, p.selectedPort,
                                p.selectedKind.isEmpty() ? QStringLiteral("selected") : p.selectedKind);
        if (foundLocal)
        {
            const auto advertised = peerEndpointCandidates(local, p);
            for (const auto& candidate : advertised)
                appendEndpointCandidate(candidates, candidate.address.toString(), candidate.port, candidate.kind);
        }
        else
        {
            appendEndpointCandidate(candidates, p.publicIp, p.publicPort, QStringLiteral("public"));
        }

        if (candidates.isEmpty())
        {
            qWarning() << "Rollback lobby punch skipped peer"
                       << "userId" << p.userId
                       << "slot" << p.slot
                       << "public" << p.publicIp << p.publicPort
                       << "local" << p.localIp << p.localPort;
            continue;
        }

        for (const auto& candidate : candidates)
        {
            qInfo() << "Rollback lobby punching peer"
                    << "selfUserId" << m_selfUserId
                    << "localPort" << m_udp->localPort()
                    << "peerUserId" << p.userId
                    << "slot" << p.slot
                    << "kind" << candidate.kind
                    << "endpoint" << candidate.address.toString() << candidate.port
                    << "burst" << ANCHOR_PUNCH_BURST;
            for (int i = 0; i < ANCHOR_PUNCH_BURST; ++i)
                m_udp->writeDatagram(pkt, candidate.address, candidate.port);
        }
    }
}

bool LobbyClient::syncPrematchManifest(QList<LobbyMatchPeer>& peers, int localSlot, const QString& romFile, QString& error)
{
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

    int localIndex = -1;
    int hostIndex = -1;
    for (int i = 0; i < peers.size(); i++)
    {
        if (peers[i].userId == m_selfUserId)
            localIndex = i;
        if (peers[i].slot == 1)
            hostIndex = i;
    }
    if (localIndex < 0 || hostIndex < 0)
    {
        error = "Pre-match sync failed: incomplete peer list";
        return false;
    }

    LobbyMatchPeer& local = peers[localIndex];
    LobbyMatchPeer& host = peers[hostIndex];

    auto endpointsFor = [&](const LobbyMatchPeer& peer) {
        QList<UdpEndpointCandidate> endpoints;
        appendEndpointCandidate(endpoints, peer.selectedIp, peer.selectedPort,
                                peer.selectedKind.isEmpty() ? QStringLiteral("selected") : peer.selectedKind);

        // If the endpoint race did not receive a reply, continue racing during
        // pre-match sync instead of committing to an unverified fallback.
        if (!peer.selectedEndpointVerified)
        {
            const auto advertised = peerEndpointCandidates(local, peer);
            for (const auto& candidate : advertised)
                appendEndpointCandidate(endpoints, candidate.address.toString(), candidate.port, candidate.kind);
        }
        return endpoints;
    };

    // Keep one stable nonce per unresolved peer for the duration of pre-match
    // synchronization. Every 200 ms we resend the same probe to every available
    // candidate until that peer answers. This extends the short initial race and
    // is especially important for non-host-to-non-host routes in 3/4-player games.
    QHash<quint64, quint64> prematchProbeNonceByUser;
    QHash<quint64, quint64> prematchProbeUserByNonce;

    auto clearPrematchProbe = [&](quint64 userId) {
        const auto nonceIt = prematchProbeNonceByUser.find(userId);
        if (nonceIt == prematchProbeNonceByUser.end())
            return;

        prematchProbeUserByNonce.remove(nonceIt.value());
        prematchProbeNonceByUser.erase(nonceIt);
    };

    auto recordSelectedEndpoint = [&](quint64 userId, const QHostAddress& sender, quint16 senderPort) {
        for (LobbyMatchPeer& peer : peers)
        {
            if (peer.userId != userId)
                continue;

            peer.selectedIp = sender.toString();
            peer.selectedPort = senderPort;
            peer.selectedKind = QStringLiteral("reply");
            peer.selectedEndpointVerified = true;
            const auto advertised = peerEndpointCandidates(local, peer);
            for (const auto& candidate : advertised)
            {
                if (candidate.address == sender && candidate.port == senderPort)
                {
                    peer.selectedKind = candidate.kind;
                    break;
                }
            }

            clearPrematchProbe(userId);
            return;
        }
    };

    for (const LobbyMatchPeer& peer : peers)
    {
        if (peer.userId == m_selfUserId || peer.selectedEndpointVerified || endpointsFor(peer).isEmpty())
            continue;

        quint64 nonce = 0;
        do
        {
            nonce = QRandomGenerator::global()->generate64();
        }
        while (nonce == 0 || prematchProbeUserByNonce.contains(nonce) || m_pendingProbes.contains(nonce));

        prematchProbeNonceByUser.insert(peer.userId, nonce);
        prematchProbeUserByNonce.insert(nonce, peer.userId);
    }

    auto sendUnresolvedPeerProbes = [&](qint64 nowMs, qint64& nextSendMs) {
        if (prematchProbeNonceByUser.isEmpty() || nowMs < nextSendMs)
            return;

        for (const LobbyMatchPeer& peer : peers)
        {
            const auto nonceIt = prematchProbeNonceByUser.constFind(peer.userId);
            if (nonceIt == prematchProbeNonceByUser.constEnd())
                continue;

            const QByteArray packet = buildProbePacket(m_selfUserId, nonceIt.value());
            const auto endpoints = endpointsFor(peer);
            for (const auto& endpoint : endpoints)
                m_udp->writeDatagram(packet, endpoint.address, endpoint.port);
        }

        nextSendMs = nowMs + PREMATCH_ENDPOINT_PROBE_INTERVAL_MS;
    };

    auto handleProbeDatagram = [&](const QByteArray& datagram, const QHostAddress& sender, quint16 senderPort) {
        if (datagram.size() != PROBE_PACKET_SIZE ||
            std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0)
        {
            return false;
        }

        const quint8 op = static_cast<quint8>(datagram.at(4));
        if (op == ANCHOR_OP_PROBE)
        {
            quint64 senderUserId = 0;
            quint64 nonce = 0;
            if (readProbePacket(datagram, senderUserId, nonce))
            {
                // Learn the actual source endpoint before replying. This is the
                // endpoint that is demonstrably usable for this peer-to-peer path,
                // even if it differs from the lobby server's observed mapping.
                recordSelectedEndpoint(senderUserId, sender, senderPort);

                const QByteArray reply = buildProbePacket(m_selfUserId, nonce, ANCHOR_OP_PROBE_REPLY);
                m_udp->writeDatagram(reply, sender, senderPort);
            }
            return true;
        }

        if (op != ANCHOR_OP_PROBE_REPLY)
            return false;

        quint64 senderUserId = 0;
        quint64 nonce = 0;
        if (!readProbePacket(datagram, senderUserId, nonce))
            return true;

        const auto expectedIt = prematchProbeUserByNonce.constFind(nonce);
        if (expectedIt != prematchProbeUserByNonce.constEnd() && expectedIt.value() == senderUserId)
        {
            recordSelectedEndpoint(senderUserId, sender, senderPort);
            qInfo() << "Rollback lobby prematch probe selected"
                    << "peerUserId" << senderUserId
                    << "endpoint" << sender.toString() << senderPort
                    << "remaining" << prematchProbeNonceByUser.size();
            return true;
        }

        // A normal lobby ping may complete while pre-match sync owns the UDP
        // socket. Preserve that measurement instead of dropping the reply.
        auto pingIt = m_pendingProbes.find(nonce);
        if (pingIt != m_pendingProbes.end() && senderUserId == pingIt->targetUserId)
        {
            const int rttMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - pingIt->sendMs);
            const quint64 uid = pingIt->targetUserId;
            m_pendingProbes.erase(pingIt);
            emit pingProbeMeasured(uid, recordPingSample(uid, rttMs));
        }
        return true;
    };

    qint64 nextProbeSendMs = 0;

    if (localSlot == 1)
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
                << "peers" << pendingAcks.size()
                << "unresolvedRoutes" << prematchProbeNonceByUser.size();

        const qint64 hostSyncDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
        while (!pendingAcks.isEmpty())
        {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs > hostSyncDeadlineMs)
            {
                QStringList failedNames;
                for (quint64 failedId : pendingAcks)
                {
                    for (const auto& peer : peers)
                    {
                        if (peer.userId == failedId)
                        {
                            failedNames << QString("P%1 (%2)").arg(peer.slot).arg(peer.username);
                            break;
                        }
                    }
                }
                error = QString("Pre-match sync timed out — %1 didn't respond (blocked UDP connection or missing ROM).")
                            .arg(failedNames.join(", "));
                return false;
            }

            sendUnresolvedPeerProbes(nowMs, nextProbeSendMs);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            for (const auto& peer : peers)
            {
                if (!pendingAcks.contains(peer.userId))
                    continue;
                const auto endpoints = endpointsFor(peer);
                for (const auto& endpoint : endpoints)
                    m_udp->writeDatagram(packet, endpoint.address, endpoint.port);
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

                    if (handleProbeDatagram(datagram, sender, senderPort))
                        continue;

                    if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                        static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_ACK)
                    {
                        continue;
                    }

                    quint64 senderUserId = 0;
                    uint64_t ackHash = 0;
                    if (readPrematchSenderAndHash(datagram, senderUserId, ackHash) &&
                        ackHash == manifestHash && pendingAcks.contains(senderUserId))
                    {
                        recordSelectedEndpoint(senderUserId, sender, senderPort);
                        pendingAcks.remove(senderUserId);
                    }
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
                << "cheats" << static_cast<qulonglong>(cheatCount)
                << "unresolvedRoutes" << prematchProbeNonceByUser.size();
        return true;
    }

    const auto hostEndpoints = endpointsFor(host);
    if (hostEndpoints.isEmpty())
    {
        error = "Pre-match sync failed: invalid host endpoint";
        return false;
    }

    qInfo() << "Rollback lobby prematch client waiting"
            << "hostUserId" << host.userId
            << "hostEndpoint" << hostEndpoints.first().address.toString()
            << hostEndpoints.first().port
            << "unresolvedRoutes" << prematchProbeNonceByUser.size();

    const qint64 clientSyncDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kPrematchSyncTimeoutMs;
    for (;;)
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs > clientSyncDeadlineMs)
        {
            error = QStringLiteral("Pre-match sync timed out — never heard from the host.");
            return false;
        }

        sendUnresolvedPeerProbes(nowMs, nextProbeSendMs);
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

            if (handleProbeDatagram(datagram, sender, senderPort))
                continue;

            if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0 ||
                static_cast<quint8>(datagram.at(4)) != ANCHOR_OP_PREMATCH_MANIFEST)
            {
                continue;
            }

            quint64 senderUserId = 0;
            uint64_t manifestHash = 0;
            if (!readPrematchSenderAndHash(datagram, senderUserId, manifestHash) || senderUserId != host.userId)
                continue;

            recordSelectedEndpoint(senderUserId, sender, senderPort);

            const int manifestOffset = 4 + 1 + 8 + 8;
            const std::string manifest(datagram.constData() + manifestOffset, datagram.constData() + datagram.size());
            size_t cheatCount = 0;
            if (!applyPrematchManifest(manifest, manifestHash, cheatCount))
            {
                error = QString::fromStdString(CoreGetError());
                return false;
            }

            const QHostAddress manifestSender = sender;
            const quint16 manifestSenderPort = senderPort;
            const QByteArray ack = buildPrematchPacket(ANCHOR_OP_PREMATCH_ACK, m_selfUserId, manifestHash);
            for (int i = 0; i < ANCHOR_PUNCH_BURST; i++)
                m_udp->writeDatagram(ack, manifestSender, manifestSenderPort);

            // Do not immediately release the UDP socket after receiving the
            // host manifest. Keep answering and sending probes until every
            // still-unresolved peer route has replied, or until the existing
            // pre-match deadline expires. This gives non-host peers time to
            // punch directly through to each other in 3/4-player matches.
            qint64 nextAckSendMs = QDateTime::currentMSecsSinceEpoch() + PREMATCH_ENDPOINT_PROBE_INTERVAL_MS;
            while (!prematchProbeNonceByUser.isEmpty())
            {
                const qint64 convergeNowMs = QDateTime::currentMSecsSinceEpoch();
                if (convergeNowMs > clientSyncDeadlineMs)
                {
                    qWarning() << "Rollback lobby prematch route probing timed out; using fallbacks"
                               << "unresolvedUserIds" << prematchProbeNonceByUser.keys();
                    break;
                }

                sendUnresolvedPeerProbes(convergeNowMs, nextProbeSendMs);
                if (convergeNowMs >= nextAckSendMs)
                {
                    m_udp->writeDatagram(ack, manifestSender, manifestSenderPort);
                    nextAckSendMs = convergeNowMs + PREMATCH_ENDPOINT_PROBE_INTERVAL_MS;
                }

                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                if (!m_udp->hasPendingDatagrams())
                    m_udp->waitForReadyRead(20);

                while (m_udp->hasPendingDatagrams())
                {
                    QByteArray incoming;
                    QHostAddress incomingSender;
                    quint16 incomingSenderPort = 0;
                    incoming.resize(int(m_udp->pendingDatagramSize()));
                    m_udp->readDatagram(incoming.data(), incoming.size(), &incomingSender, &incomingSenderPort);

                    if (handleProbeDatagram(incoming, incomingSender, incomingSenderPort))
                        continue;

                    // The host may retransmit the same manifest until it sees
                    // our ACK. Acknowledge duplicates without reapplying it.
                    if (incoming.size() >= 5 && std::memcmp(incoming.constData(), ANCHOR_MAGIC, 4) == 0 &&
                        static_cast<quint8>(incoming.at(4)) == ANCHOR_OP_PREMATCH_MANIFEST)
                    {
                        quint64 duplicateSenderUserId = 0;
                        uint64_t duplicateHash = 0;
                        if (readPrematchSenderAndHash(incoming, duplicateSenderUserId, duplicateHash) &&
                            duplicateSenderUserId == host.userId && duplicateHash == manifestHash)
                        {
                            m_udp->writeDatagram(ack, incomingSender, incomingSenderPort);
                        }
                    }
                }
            }

            qInfo() << "Rollback lobby prematch client complete"
                    << "hash" << static_cast<qulonglong>(manifestHash)
                    << "cheats" << static_cast<qulonglong>(cheatCount)
                    << "unresolvedRoutes" << prematchProbeNonceByUser.size();
            return true;
        }
    }
}

quint16 LobbyClient::localUdpPort() const
{
    return m_udp ? m_udp->localPort() : 0;
}

void LobbyClient::releaseUdpAnchor()
{
    if (m_udpKeepaliveTimer)
        m_udpKeepaliveTimer->stop();
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
    if (m_state == ConnectionState::Connected && m_udp &&
        m_udp->state() == QAbstractSocket::UnconnectedState)
    {
        initiateUdpAnchor();
    }
}

void LobbyClient::onUdpReadyRead()
{
    if (m_inPrematchSync)
        return;

    while (m_udp->hasPendingDatagrams())
    {
        QByteArray datagram;
        QHostAddress sender;
        quint16 senderPort = 0;
        datagram.resize(int(m_udp->pendingDatagramSize()));
        m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.size() < 5 || std::memcmp(datagram.constData(), ANCHOR_MAGIC, 4) != 0)
            continue;

        const quint8 op = static_cast<quint8>(datagram.at(4));
        switch (op)
        {
        case ANCHOR_OP_PROBE:
        {
            quint64 senderUserId = 0;
            quint64 nonce = 0;
            if (!readProbePacket(datagram, senderUserId, nonce))
            {
                writePingDiagnostic(QStringLiteral("PING_RECV_PROBE_INVALID"),
                                    QStringLiteral("source=%1:%2 bytes=%3")
                                        .arg(sender.toString())
                                        .arg(senderPort)
                                        .arg(datagram.size()));
                break;
            }

            writePingDiagnostic(QStringLiteral("PING_RECV_PROBE"),
                                QStringLiteral("source=%1:%2 sender=%3 nonce=%4 known_user=%5 pending_count=%6")
                                    .arg(sender.toString())
                                    .arg(senderPort)
                                    .arg(pingUserLabel(senderUserId))
                                    .arg(nonce)
                                    .arg(m_users.contains(senderUserId) ? 1 : 0)
                                    .arg(m_pendingProbes.size()));

            // The source of an inbound probe is a demonstrably usable endpoint
            // for this peer-to-peer NAT mapping. Cache it even if the reciprocal
            // WebSocket start has not arrived yet. If a burst is already active,
            // add the route once and let the fixed-rate timer send through it.
            // Never create a new burst from an inbound probe: late packets from a
            // completed burst would otherwise cause both peers to recursively
            // restart each other and grow into a packet storm.
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const QString learnedEndpoint = endpointKey(sender, senderPort);
            const bool knownRemoteUser = senderUserId != 0 &&
                                         senderUserId != m_selfUserId &&
                                         m_users.contains(senderUserId);
            bool foundActiveBurst = false;
            bool appendedActiveRoute = false;

            if (knownRemoteUser)
            {
                RecentInboundPingEndpoint& cached = m_recentInboundPingEndpoints[senderUserId];
                const bool cacheChanged = cached.endpoint != learnedEndpoint;
                cached.endpoint = learnedEndpoint;
                cached.seenMs = nowMs;

                for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end(); ++it)
                {
                    if (it->targetUserId != senderUserId)
                        continue;

                    foundActiveBurst = true;
                    if (!it->endpoints.contains(learnedEndpoint))
                    {
                        it->endpoints.append(learnedEndpoint);
                        it->nextSendMs = 0;
                        appendedActiveRoute = true;
                        qInfo() << "Rollback lobby ping learned endpoint from inbound probe"
                                << "peerUserId" << senderUserId
                                << "endpoint" << learnedEndpoint;
                        writePingDiagnostic(QStringLiteral("PING_LEARN_ENDPOINT"),
                                            QStringLiteral("nonce=%1 target=%2 origin=%3 endpoint=%4 action=append all=[%5]")
                                                .arg(it.key())
                                                .arg(pingUserLabel(senderUserId))
                                                .arg(it->origin)
                                                .arg(learnedEndpoint)
                                                .arg(it->endpoints.join(QLatin1Char(','))));
                    }
                    break;
                }

                if (!foundActiveBurst)
                {
                    writePingDiagnostic(QStringLiteral("PING_CACHE_ENDPOINT"),
                                        QStringLiteral("target=%1 endpoint=%2 action=%3 trigger_nonce=%4")
                                            .arg(pingUserLabel(senderUserId))
                                            .arg(learnedEndpoint)
                                            .arg(cacheChanged ? QStringLiteral("store") : QStringLiteral("refresh"))
                                            .arg(nonce));
                }
            }
            else
            {
                writePingDiagnostic(QStringLiteral("PING_ENDPOINT_NOT_CACHED"),
                                    QStringLiteral("sender=%1 endpoint=%2 reason=unknown_or_self")
                                        .arg(pingUserLabel(senderUserId))
                                        .arg(learnedEndpoint));
            }

            const QByteArray reply = buildProbePacket(m_selfUserId, nonce, ANCHOR_OP_PROBE_REPLY);
            const qint64 replyWritten = m_udp->writeDatagram(reply, sender, senderPort);
            writePingDiagnostic(QStringLiteral("PING_SEND_REPLY"),
                                QStringLiteral("to=%1:%2 peer=%3 nonce=%4 bytes=%5 result=%6 error=%7")
                                    .arg(sender.toString())
                                    .arg(senderPort)
                                    .arg(pingUserLabel(senderUserId))
                                    .arg(nonce)
                                    .arg(reply.size())
                                    .arg(replyWritten)
                                    .arg(replyWritten == reply.size() ? QStringLiteral("ok") : m_udp->errorString()));

            if (appendedActiveRoute)
            {
                if (!m_pingProbeBurstTimer->isActive())
                    m_pingProbeBurstTimer->start();
                onPingProbeBurstTimer();
            }
            break;
        }
        case ANCHOR_OP_PROBE_REPLY:
        {
            quint64 senderUserId = 0;
            quint64 nonce = 0;
            if (!readProbePacket(datagram, senderUserId, nonce))
            {
                writePingDiagnostic(QStringLiteral("PING_RECV_REPLY_INVALID"),
                                    QStringLiteral("source=%1:%2 bytes=%3")
                                        .arg(sender.toString())
                                        .arg(senderPort)
                                        .arg(datagram.size()));
                break;
            }

            writePingDiagnostic(QStringLiteral("PING_RECV_REPLY"),
                                QStringLiteral("source=%1:%2 sender=%3 nonce=%4 pending_count=%5")
                                    .arg(sender.toString())
                                    .arg(senderPort)
                                    .arg(pingUserLabel(senderUserId))
                                    .arg(nonce)
                                    .arg(m_pendingProbes.size()));

            const auto it = m_pendingProbes.find(nonce);
            if (it == m_pendingProbes.end())
            {
                writePingDiagnostic(QStringLiteral("PING_REPLY_UNMATCHED"),
                                    QStringLiteral("reason=unknown_nonce source=%1:%2 sender=%3 nonce=%4 pending={%5}")
                                        .arg(sender.toString())
                                        .arg(senderPort)
                                        .arg(pingUserLabel(senderUserId))
                                        .arg(nonce)
                                        .arg(pendingPingSummary()));
                break;
            }
            if (senderUserId != it->targetUserId)
            {
                writePingDiagnostic(QStringLiteral("PING_REPLY_UNMATCHED"),
                                    QStringLiteral("reason=sender_mismatch nonce=%1 expected=%2 actual=%3 source=%4:%5")
                                        .arg(nonce)
                                        .arg(pingUserLabel(it->targetUserId))
                                        .arg(pingUserLabel(senderUserId))
                                        .arg(sender.toString())
                                        .arg(senderPort));
                break;
            }
            const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
            const bool sendWasRecorded = it->sendMs != 0;
            const int    rttMs  = static_cast<int>(nowMs - it->sendMs);
            const quint64 uid   = it->targetUserId;
            const QString origin = it->origin;
            const int rounds = it->sendRounds;
            const int datagramsSent = it->datagramsSent;
            const QStringList endpoints = it->endpoints;
            RecentInboundPingEndpoint& successfulEndpoint = m_recentInboundPingEndpoints[uid];
            successfulEndpoint.endpoint = endpointKey(sender, senderPort);
            successfulEndpoint.seenMs = nowMs;
            m_pendingProbes.erase(it);
            if (m_pendingProbes.isEmpty())
                m_pingProbeBurstTimer->stop();
            if (!sendWasRecorded)
            {
                writePingDiagnostic(QStringLiteral("PING_REPLY_ANOMALY"),
                                    QStringLiteral("reason=reply_before_recorded_send nonce=%1 target=%2 source=%3:%4 origin=%5 computed_rtt_ms=%6")
                                        .arg(nonce)
                                        .arg(pingUserLabel(uid))
                                        .arg(sender.toString())
                                        .arg(senderPort)
                                        .arg(origin)
                                        .arg(rttMs));
            }
            const int median = recordPingSample(uid, rttMs);
            writePingDiagnostic(QStringLiteral("PING_SUCCESS"),
                                QStringLiteral("nonce=%1 target=%2 source=%3:%4 origin=%5 raw_rtt_ms=%6 median_ms=%7 rounds=%8 datagrams=%9 endpoints=[%10]")
                                    .arg(nonce)
                                    .arg(pingUserLabel(uid))
                                    .arg(sender.toString())
                                    .arg(senderPort)
                                    .arg(origin)
                                    .arg(rttMs)
                                    .arg(median)
                                    .arg(rounds)
                                    .arg(datagramsSent)
                                    .arg(endpoints.join(QLatin1Char(','))));
            emit pingProbeMeasured(uid, median);
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

}

int LobbyClient::measuredPingMs(quint64 userId) const
{
    const auto it = m_measuredPing.constFind(userId);
    return it == m_measuredPing.constEnd() ? -1 : it.value();
}

// Fold a fresh RTT sample into the per-user window and return the median.
// Single samples are noisy — the first probe after a rebind eats NAT setup
// time and any probe can catch a jitter spike — so measuredPingMs() reports
// the median of the last few samples instead of the latest one.
int LobbyClient::recordPingSample(quint64 userId, int rttMs)
{
    QList<int>& samples = m_pingSamples[userId];
    samples.append(rttMs);
    while (samples.size() > 9)
        samples.removeFirst();

    QList<int> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const int median = sorted[sorted.size() / 2];
    m_measuredPing[userId] = median;
    QStringList sampleStrings;
    for (const int sample : samples)
        sampleStrings.append(QString::number(sample));
    writePingDiagnostic(QStringLiteral("PING_SAMPLE"),
                        QStringLiteral("target=%1 raw_ms=%2 median_ms=%3 samples=[%4]")
                            .arg(pingUserLabel(userId))
                            .arg(rttMs)
                            .arg(median)
                            .arg(sampleStrings.join(QLatin1Char(','))));
    return median;
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
    writePingDiagnostic(QStringLiteral("PING_REQUEST"),
                        QStringLiteral("target=%1 previous_median_ms=%2 pending_count=%3")
                            .arg(pingUserLabel(targetUserId))
                            .arg(measuredPingMs(targetUserId))
                            .arg(m_pendingProbes.size()));
    QJsonObject d;
    d["targetUserId"] = QJsonValue(qint64(targetUserId));
    sendEnvelope("PING_PROBE_REQUEST", d);
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

void LobbyClient::startSpectate(quint64 matchId, bool fromStart)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    if (fromStart)
        d["fromStart"] = true;
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
    u.pingToServer    = static_cast<quint16>(obj.value("pingToServer").toInt());
    u.currentRoomId   = static_cast<quint64>(obj.value("currentRoomId").toDouble());
    u.currentRoomName = obj.value("currentRoomName").toString();
    u.searchingRom    = obj.value("searchingRom").toString();
    return u;
}

#endif // NETPLAY
