/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyClient.hpp"

#include <RMG-Core/Cheats.hpp>
#include <RMG-Core/Error.hpp>

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
#include <QCoreApplication>
#include <QtEndian>
#include <QRandomGenerator>
#include <QDebug>
#include <QSet>
#include <cstring>

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

    // PROBE/PROBE_REPLY packet: [magic(4) | op(1) | senderUserId(8) | nonce(8)]
    constexpr int PROBE_PACKET_SIZE = 4 + 1 + 8 + 8;

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
    m_ws->open(url);
}

void LobbyClient::disconnectFromServer()
{
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
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

void LobbyClient::onWsConnected()
{
    setState(ConnectionState::Authenticating);

    QJsonObject data;
    data["username"]      = m_pendingUsername;
    data["clientVersion"] = "rmgk-dev"; // TODO: pull real version
    QJsonArray romArr;
    for (const auto& h : m_pendingRomHashes)
        romArr.append(h);
    data["romHashes"] = romArr;
    if (!m_pendingLocalIp.isEmpty())
        data["localIp"] = m_pendingLocalIp;

    sendEnvelope("HELLO", data, "hello-1");
}

void LobbyClient::onWsDisconnected()
{
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    m_isModerator = false; // role is per-connection; must re-auth after reconnect
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

    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();
    initiateUdpAnchor();
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
    emit roomLeft(data.value("reason").toString());
}

void LobbyClient::handleRoomState(const QJsonObject& data)
{
    emit roomStateChanged(data);
}

void LobbyClient::handleRoomJoinOk(const QJsonObject& data)
{
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
    const QString endpoint = data.value("targetEndpoint").toString();
    emit pingProbeReply(uid, endpoint);

    if (endpoint.isEmpty() || !m_udp ||
        m_udp->state() == QAbstractSocket::UnconnectedState ||
        m_selfUserId == 0)
        return;

    const int colon = endpoint.lastIndexOf(':');
    if (colon <= 0)
        return;
    const QHostAddress addr(endpoint.left(colon));
    const quint16 port = static_cast<quint16>(endpoint.mid(colon + 1).toUInt());
    if (addr.isNull() || port == 0)
        return;

    // Fire a UDP PROBE at the peer's anchor socket. The peer recognises the
    // opcode and echoes back a PROBE_REPLY; we match by nonce to compute RTT.
    const quint64 nonce = QRandomGenerator::global()->generate64();

    QByteArray pkt;
    pkt.reserve(PROBE_PACKET_SIZE);
    pkt.append(ANCHOR_MAGIC, 4);
    pkt.append(static_cast<char>(ANCHOR_OP_PROBE));
    const quint64 selfIdBE = qToBigEndian(m_selfUserId);
    pkt.append(reinterpret_cast<const char*>(&selfIdBE), sizeof(selfIdBE));
    const quint64 nonceBE = qToBigEndian(nonce);
    pkt.append(reinterpret_cast<const char*>(&nonceBE), sizeof(nonceBE));

    ProbeInFlight in;
    in.targetUserId = uid;
    in.sendMs       = QDateTime::currentMSecsSinceEpoch();
    m_pendingProbes.insert(nonce, in);

    m_udp->writeDatagram(pkt, addr, port);
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

void LobbyClient::initiateUdpAnchor()
{
    if (!m_udp->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress))
    {
        qWarning() << "lobby: udp bind failed:" << m_udp->errorString();
        return;
    }
    sendUdpRegister();
    m_udpKeepaliveTimer->start();
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
    m_udp->writeDatagram(pkt, QHostAddress(m_udpAnchorHost), m_udpAnchorPort);
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

bool LobbyClient::syncPrematchManifest(const QList<LobbyMatchPeer>& peers, int localSlot, const QString& romFile, QString& error)
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

    LobbyMatchPeer local{};
    LobbyMatchPeer host{};
    bool foundLocal = false;
    bool foundHost = false;
    for (const auto& peer : peers)
    {
        if (peer.userId == m_selfUserId)
        {
            local = peer;
            foundLocal = true;
        }
        if (peer.slot == 1)
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
            break;
        }
        case ANCHOR_OP_PROBE_REPLY:
        {
            if (datagram.size() != PROBE_PACKET_SIZE)
                break;
            quint64 nonceBE = 0;
            std::memcpy(&nonceBE, datagram.constData() + 13, sizeof(nonceBE));
            const quint64 nonce = qFromBigEndian(nonceBE);

            const auto it = m_pendingProbes.find(nonce);
            if (it == m_pendingProbes.end())
                break;
            const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
            const int    rttMs  = static_cast<int>(nowMs - it->sendMs);
            const quint64 uid   = it->targetUserId;
            m_pendingProbes.erase(it);
            m_measuredPing[uid] = rttMs;
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
            it = m_pendingProbes.erase(it);
        else
            ++it;
    }
}

int LobbyClient::measuredPingMs(quint64 userId) const
{
    const auto it = m_measuredPing.constFind(userId);
    return it == m_measuredPing.constEnd() ? -1 : it.value();
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
    u.pingToServer    = static_cast<quint16>(obj.value("pingToServer").toInt());
    u.currentRoomId   = static_cast<quint64>(obj.value("currentRoomId").toDouble());
    u.currentRoomName = obj.value("currentRoomName").toString();
    u.searchingRom    = obj.value("searchingRom").toString();
    return u;
}

#endif // NETPLAY
