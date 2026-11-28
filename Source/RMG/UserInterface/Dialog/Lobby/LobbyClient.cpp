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
#include <RMG-Core/Compression.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/LobbyIce.hpp>
#include <RMG-Core/NetplaySave.hpp>
#include <RMG-Core/GameOptions.hpp>
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
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

using namespace UserInterface::Dialog;

namespace
{
    constexpr int HEARTBEAT_INTERVAL_MS  = 15'000;
    constexpr int UDP_KEEPALIVE_INTERVAL = 20'000;
    constexpr int KEYFRAME_UPLOAD_CHUNK_BYTES = 64 * 1024;
    constexpr int MAX_KEYFRAME_RAW_BYTES = 24 * 1024 * 1024;
    const QByteArray KEYFRAME_ZLIB_MAGIC("RMGKZ1", 6);
    // An encoded keyframe or late-join blob: the raw state, or the magic, a
    // size and a smaller compressed copy (encodeSpectateKeyframe).
    constexpr int MAX_KEYFRAME_ENCODED_BYTES = MAX_KEYFRAME_RAW_BYTES + 6 + 4;

    // What a keyframe or late-join blob may arrive as. The lobby enforces its
    // own limits (broadcast.go, latejoin.go); these hold even if it doesn't,
    // so a stream of chunks can't grow without bound here. Generous on count:
    // the total byte cap is the real limit.
    constexpr int MAX_RECEIVED_CHUNK_COUNT = 4096;
    constexpr int MAX_RECEIVED_CHUNK_BYTES = 1024 * 1024;

    // The largest save-data section (RMGKSAV1, uncompressed) is about 162 KiB:
    // 2 KiB EEPROM + 32 KiB SRAM + 128 KiB FlashRAM plus a few headers.
    constexpr int MAX_SAVE_SECTION_RAW_BYTES = 256 * 1024;

    // The largest pre-match manifest a spectator or late joiner takes; the
    // lobby holds broadcasters to the same size (broadcast.go).
    constexpr int MAX_RELAYED_MANIFEST_BYTES = 512 * 1024;

    // Inflates qCompress() output (a 4-byte big-endian size, then a zlib
    // stream) from another machine. qUncompress() trusts that size, and keeps
    // growing past it, so a few bytes could make it allocate gigabytes; here
    // the output is a buffer of the declared size, at most maxBytes, that the
    // stream must fill exactly.
    bool uncompressFromPeer(const char* data, qsizetype size, qsizetype maxBytes, QByteArray& out)
    {
        out.clear();
        if (data == nullptr || size <= qsizetype(sizeof(quint32)))
            return false;

        const quint32 declared = qFromBigEndian<quint32>(data);
        if (declared == 0 || qsizetype(declared) > maxBytes)
            return false;

        QByteArray inflated(qsizetype(declared), Qt::Uninitialized);
        if (!CoreZlibInflateExact(data + sizeof(quint32), size_t(size) - sizeof(quint32),
                                  inflated.data(), size_t(declared)))
            return false;

        out = std::move(inflated);
        return true;
    }

    QByteArray encodeSpectateKeyframe(const QByteArray& raw)
    {
        if (raw.isEmpty() || raw.size() > MAX_KEYFRAME_RAW_BYTES)
            return {};

        const QByteArray compressed = qCompress(raw, 6);
        QByteArray packed;
        packed.reserve(KEYFRAME_ZLIB_MAGIC.size() + int(sizeof(quint32)) + compressed.size());
        packed.append(KEYFRAME_ZLIB_MAGIC);
        const quint32 rawSizeBE = qToBigEndian(static_cast<quint32>(raw.size()));
        packed.append(reinterpret_cast<const char*>(&rawSizeBE), sizeof(rawSizeBE));
        packed.append(compressed);

        // Preserve compatibility with the raw format if a pathological state does
        // not compress. Normal N64 states contain large sparse regions and win easily.
        return packed.size() < raw.size() ? packed : raw;
    }

    // maxRawBytes is the most the decoded blob may hold.
    bool decodeSpectateKeyframe(const QByteArray& encoded, QByteArray& raw,
                                int maxRawBytes = MAX_KEYFRAME_RAW_BYTES)
    {
        if (!encoded.startsWith(KEYFRAME_ZLIB_MAGIC))
        {
            if (encoded.isEmpty() || encoded.size() > maxRawBytes)
                return false;
            raw = encoded; // keyframe from a pre-compression broadcaster
            return true;
        }

        const int headerBytes = KEYFRAME_ZLIB_MAGIC.size() + int(sizeof(quint32));
        if (encoded.size() <= headerBytes)
            return false;
        quint32 rawSizeBE = 0;
        std::memcpy(&rawSizeBE, encoded.constData() + KEYFRAME_ZLIB_MAGIC.size(), sizeof(rawSizeBE));
        const quint32 expectedSize = qFromBigEndian(rawSizeBE);
        if (expectedSize == 0 || expectedSize > quint32(maxRawBytes))
            return false;

        // The compressed copy carries its own size too; it must agree.
        QByteArray inflated;
        if (!uncompressFromPeer(encoded.constData() + headerBytes, encoded.size() - headerBytes,
                                qsizetype(expectedSize), inflated) ||
            inflated.size() != qsizetype(expectedSize))
            return false;
        raw = std::move(inflated);
        return true;
    }

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
    // Twenty three-second ICE generations give each peer pair up to 60 seconds.
    // Every failed generation gets fresh credentials and a different local UDP
    // port; only the twentieth failure reaches the UI.
    constexpr int MAX_ICE_AUTO_RESTARTS = 19;
    constexpr int MAX_ICE_CONNECTION_ATTEMPTS = MAX_ICE_AUTO_RESTARTS + 1;
    // How long a reply to an ICE ping series that has ended still counts: a
    // real round trip, just slower than the retries allowed for.
    constexpr qint64 ICE_PING_LATE_REPLY_MS = 5'000;

    // Ping checks. A join waits this long for the host's ping before it goes
    // ahead unmeasured, and decides on the lowest of a few samples so one slow
    // reply doesn't turn it away.
    constexpr qint64 PING_CHECK_TIMEOUT_MS = 6'000;
    constexpr int PING_CHECK_SAMPLES = 3;
    // A challenge's probe agent outlives the challenge by the lobby's 5 s grant
    // grace, so a last sample can land; a little more covers a lost
    // PING_CHECK_END.
    constexpr qint64 PROBE_PEER_GRACE_MS = 6'000;
    constexpr qint64 PROBE_PEER_SLACK_MS = 3'000;
    // A probe agent gets a few fresh ICE generations, not the mesh's twenty:
    // the measurement is a nicety, and its grant is short.
    constexpr int MAX_PROBE_PEER_RESTARTS = 3;
    // Quick samples first so a number shows within a second or two, then
    // the room's 3 s cadence while a challenge stays open.
    qint64 probePeerIntervalMs(int seriesStarted)
    {
        return seriesStarted < 3 ? 500 : seriesStarted < 6 ? 1'000 : 3'000;
    }

    // Unanswered heartbeats are forgotten after this.
    constexpr qint64 HEARTBEAT_ACK_TIMEOUT_MS = 60'000;

    // Round trips and expiry use a clock that never jumps.
    qint64 monotonicMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    QString iceStateName(int state)
    {
        switch (static_cast<LobbyIcePeerState>(state))
        {
        case LobbyIcePeerState::Disconnected: return QStringLiteral("disconnected");
        case LobbyIcePeerState::Gathering: return QStringLiteral("gathering");
        case LobbyIcePeerState::Connecting: return QStringLiteral("connecting");
        case LobbyIcePeerState::Connected: return QStringLiteral("connected");
        case LobbyIcePeerState::Completed: return QStringLiteral("completed");
        case LobbyIcePeerState::Failed: return QStringLiteral("failed");
        }
        return QStringLiteral("unknown");
    }

    QString iceSignalSummary(const QString& kind, const QString& value)
    {
        if (kind == QStringLiteral("candidate"))
            return QStringLiteral("candidate=%1").arg(value.trimmed());
        if (kind == QStringLiteral("description"))
        {
            const int candidates = value.count(QStringLiteral("candidate:"), Qt::CaseInsensitive);
            return QStringLiteral("bytes=%1 embedded_candidates=%2 credentials=<redacted>")
                .arg(value.toUtf8().size()).arg(candidates);
        }
        if (kind == QStringLiteral("gathering_done"))
            return QStringLiteral("complete=true");
        return QStringLiteral("bytes=%1").arg(value.toUtf8().size());
    }

    QString redactLibjuiceMessage(QString message)
    {
        // DEBUG messages can contain short-lived ICE credentials. They are not
        // useful for diagnosis and should not survive in a shareable log.
        static const QRegularExpression secret(
            QStringLiteral(R"((ufrag|pwd|password|username|expected|actual)=\"[^\"]*\")"),
            QRegularExpression::CaseInsensitiveOption);
        message.replace(secret, QStringLiteral("\\1=\"<redacted>\""));
        return message;
    }

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

    // Manifest sections, in this order after the header.
    constexpr uint32_t PREMATCH_SECTION_CHEATS       = 1u << 0;
    constexpr uint32_t PREMATCH_SECTION_SAVE_DATA    = 1u << 1;
    constexpr uint32_t PREMATCH_SECTION_GAME_OPTIONS = 1u << 2;

    // saveData is the host's starting save, or null to start blank. It rides
    // compressed in its own section, which clients before save data refuse;
    // the lobby keeps them out of matches that use it. gameOptions, when there
    // are any, ride the same way (uncompressed: they're small), and a match
    // without them gets no section, so its manifest stays as it always was.
    bool buildPrematchManifest(const QString& romFile, const CoreNetplaySaveData* saveData,
                               const CoreNetplayGameOptions& gameOptions,
                               std::string& manifest, uint64_t& manifestHash, size_t& cheatCount)
    {
        std::vector<CoreCheat> cheats;
        std::string cheatManifest;
        std::string saveSection;
        std::string gameOptionsSection;

        if (!CoreGetEnabledNetplayCheats(romFile.toStdU32String(), cheats) || !CoreSerializeNetplayCheats(cheats, cheatManifest))
            return false;
        if (saveData != nullptr)
        {
            std::string raw;
            if (!CoreSerializeNetplaySaveData(*saveData, raw))
                return false;
            const QByteArray packed = qCompress(QByteArray::fromStdString(raw), 9);
            saveSection.assign(packed.constData(), size_t(packed.size()));
        }
        const bool hasGameOptions = !gameOptions.IsEmpty();
        if (hasGameOptions && !CoreSerializeNetplayGameOptions(gameOptions, gameOptionsSection))
            return false;

        manifest.clear();
        manifest.append("RMGKPMAN", 8);
        appendPrematchU32(manifest, 1);
        appendPrematchU32(manifest, PREMATCH_SECTION_CHEATS |
            (saveData != nullptr ? PREMATCH_SECTION_SAVE_DATA : 0u) |
            (hasGameOptions ? PREMATCH_SECTION_GAME_OPTIONS : 0u));
        appendPrematchU32(manifest, static_cast<uint32_t>(cheatManifest.size()));
        manifest.append(cheatManifest);
        if (saveData != nullptr)
        {
            appendPrematchU32(manifest, static_cast<uint32_t>(saveSection.size()));
            manifest.append(saveSection);
        }
        if (hasGameOptions)
        {
            appendPrematchU32(manifest, static_cast<uint32_t>(gameOptionsSection.size()));
            manifest.append(gameOptionsSection);
        }
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
            version != 1 ||
            (sectionMask & ~(PREMATCH_SECTION_CHEATS | PREMATCH_SECTION_SAVE_DATA | PREMATCH_SECTION_GAME_OPTIONS)) != 0 ||
            !readPrematchU32(manifest, offset, cheatManifestLen) ||
            offset + cheatManifestLen > manifest.size())
        {
            return false;
        }

        const std::string cheatManifest(manifest.data() + offset, manifest.data() + offset + cheatManifestLen);
        if (!CoreDeserializeNetplayCheats(cheatManifest, cheats))
            return false;
        offset += cheatManifestLen;

        // Without a save section the match starts blank, so a save staged for
        // an earlier match must not carry over.
        CoreNetplaySaveData saveData;
        if ((sectionMask & PREMATCH_SECTION_SAVE_DATA) != 0)
        {
            uint32_t saveLen = 0;
            if (!readPrematchU32(manifest, offset, saveLen) || offset + saveLen > manifest.size())
                return false;
            QByteArray raw;
            if (!uncompressFromPeer(manifest.data() + offset, qsizetype(saveLen),
                                    MAX_SAVE_SECTION_RAW_BYTES, raw))
                return false;
            offset += saveLen;
            if (!CoreDeserializeNetplaySaveData(raw.toStdString(), saveData))
                return false;
        }
        // Likewise game options: none without the section.
        CoreNetplayGameOptions gameOptions;
        if ((sectionMask & PREMATCH_SECTION_GAME_OPTIONS) != 0)
        {
            uint32_t gameOptionsLen = 0;
            if (!readPrematchU32(manifest, offset, gameOptionsLen) || gameOptionsLen > manifest.size() - offset)
                return false;
            const std::string section(manifest.data() + offset, gameOptionsLen);
            offset += gameOptionsLen;
            if (!CoreDeserializeNetplayGameOptions(section, gameOptions) || gameOptions.IsEmpty())
                return false;
        }
        if (offset != manifest.size())
            return false;

        cheatCount = cheats.size();
        if (!CoreSetNetplayCheats(cheats))
            return false;
        CoreSetNetplaySaveData(saveData);
        CoreSetNetplayGameOptions(gameOptions);
        return true;
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

    // Manifest bytes per fragment datagram, after its 29-byte header.
    constexpr int PREMATCH_FRAGMENT_PAYLOAD_BYTES = 1000;

    QList<QByteArray> fragmentPrematchPacket(const QByteArray& packet,
        quint64 senderUserId, uint64_t manifestHash)
    {
        constexpr int kPayloadBytes = PREMATCH_FRAGMENT_PAYLOAD_BYTES;
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
    , m_iceProbes(PROBE_ATTEMPTS, PROBE_RETRY_INTERVAL_MS, ICE_PING_LATE_REPLY_MS)
{
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_ws, &QWebSocket::connected,           this, &LobbyClient::onWsConnected);
    connect(m_ws, &QWebSocket::disconnected,        this, &LobbyClient::onWsDisconnected);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &LobbyClient::onWsTextMessageReceived);
    connect(m_ws, &QWebSocket::bytesWritten,        this, &LobbyClient::onWsBytesWritten,
            Qt::QueuedConnection);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_ws, &QWebSocket::errorOccurred,       this, &LobbyClient::onWsErrorOccurred);
#else
    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &LobbyClient::onWsErrorOccurred);
#endif

    m_kfCompressWatcher = new QFutureWatcher<QByteArray>(this);
    connect(m_kfCompressWatcher, &QFutureWatcher<QByteArray>::finished,
            this, &LobbyClient::onKeyframeCompressionFinished);

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
    // HELLO_OK calls LobbyIce::configure(), which resets the process-wide ICE
    // mesh. A running GekkoNet lobby match owns that mesh, so reconnecting now
    // would sever otherwise healthy peer-to-peer gameplay. The lobby dialog
    // offers the connection prompt again after emulation finishes.
    if (m_matchTransportActive)
    {
        qWarning() << "lobby: reconnect deferred while match ICE transport is active";
        return;
    }
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
    // The server drops local players with the connection.
    m_localPlayers.clear();
    m_pendingLocalControllers.clear();
    m_localPlayersSupported = false;
    m_socialSupported = false;
    m_pingCheckSupported = false;
    m_spectatorChatSupported = false;
    m_challengeSettingsSupported = false;
    m_pmSettingsSupported = false;
    // Subscriptions end with the old connection.
    m_spectateMatchIds.clear();
    m_kfRecv.clear();
    emit localPlayersChanged();

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
    resetBroadcastKeyframeUpload();
    stopPingDiagnosticLog(QStringLiteral("disconnect"));
    if (m_anchorRetryTimer)
        m_anchorRetryTimer->stop();
    m_anchorRetryDeadlineMs = 0;
    m_heartbeatTimer->stop();
    m_udpKeepaliveTimer->stop();
    resetIceTransport();
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

qint64 LobbyClient::sendEnvelope(const QString& type, const QJsonObject& data, const QString& id)
{
    if (m_ws->state() != QAbstractSocket::ConnectedState)
        return -1;

    QJsonObject env;
    env["type"] = type;
    if (!id.isEmpty())
        env["id"] = id;
    if (!data.isEmpty())
        env["data"] = data;

    const QByteArray payload = QJsonDocument(env).toJson(QJsonDocument::Compact);
    return m_ws->sendTextMessage(QString::fromUtf8(payload));
}

void LobbyClient::onWsBytesWritten(qint64 bytes)
{
    (void)bytes;
    if (m_kfUploadActive && m_ws->bytesToWrite() == 0)
        pumpBroadcastKeyframeUpload();
}

void LobbyClient::onKeyframeCompressionFinished()
{
    const QByteArray encoded = m_kfCompressWatcher->result();
    const quint64 matchId = m_kfCompressMatchId;
    const int frame = m_kfCompressFrame;
    const int rawBytes = m_kfCompressRawBytes;
    m_kfCompressing = false;
    m_kfCompressMatchId = 0;
    m_kfCompressFrame = -1;
    m_kfCompressRawBytes = 0;

    // A stop/disconnect invalidates the target while compression finishes in
    // the worker pool. Never let that stale result leak into a later broadcast.
    if (matchId == 0 || encoded.isEmpty() ||
        m_ws->state() != QAbstractSocket::ConnectedState)
        return;

    const bool compressed = encoded.startsWith(KEYFRAME_ZLIB_MAGIC);
    const double ratio = rawBytes > 0 ? (100.0 * encoded.size() / rawBytes) : 100.0;
    const double saved = std::max(0.0, 100.0 - ratio);
    qInfo().nospace() << "Live replay keyframe prepared: raw=" << rawBytes
                      << " bytes, payload=" << encoded.size() << " bytes ("
                      << QString::number(saved, 'f', 1) << "% saved before base64, "
                      << (compressed ? "zlib" : "raw fallback") << ")";

    m_kfUploadActive = true;
    m_kfUploadMatchId = matchId;
    m_kfUploadFrame = frame;
    m_kfUploadChunk = 0;
    m_kfUploadChunkCount = (encoded.size() + KEYFRAME_UPLOAD_CHUNK_BYTES - 1) /
                           KEYFRAME_UPLOAD_CHUNK_BYTES;
    m_kfUploadData = encoded;
    pumpBroadcastKeyframeUpload();
}

void LobbyClient::pumpBroadcastKeyframeUpload()
{
    if (!m_kfUploadActive || m_ws->bytesToWrite() != 0 ||
        m_ws->state() != QAbstractSocket::ConnectedState)
        return;

    if (m_kfUploadChunk >= m_kfUploadChunkCount)
    {
        qInfo() << "Live replay keyframe upload completed for frame" << m_kfUploadFrame;
        m_kfUploadActive = false;
        m_kfUploadMatchId = 0;
        m_kfUploadFrame = -1;
        m_kfUploadChunk = 0;
        m_kfUploadChunkCount = 0;
        m_kfUploadData.clear();
        return;
    }

    const int start = m_kfUploadChunk * KEYFRAME_UPLOAD_CHUNK_BYTES;
    const int len = std::min(KEYFRAME_UPLOAD_CHUNK_BYTES,
                             static_cast<int>(m_kfUploadData.size()) - start);
    QJsonObject data;
    data["matchId"] = QJsonValue(qint64(m_kfUploadMatchId));
    data["frame"] = m_kfUploadFrame;
    data["chunkIndex"] = m_kfUploadChunk;
    data["chunkCount"] = m_kfUploadChunkCount;
    data["data"] = QString::fromLatin1(m_kfUploadData.mid(start, len).toBase64());
    if (sendEnvelope("BROADCAST_KEYFRAME", data) <= 0)
    {
        resetBroadcastKeyframeUpload(m_kfUploadMatchId);
        return;
    }
    ++m_kfUploadChunk;
}

void LobbyClient::resetBroadcastKeyframeUpload(quint64 matchId)
{
    if (matchId == 0 || m_kfCompressMatchId == matchId)
    {
        // qCompress itself is not cancellable, so invalidate its destination and
        // let the worker finish harmlessly. m_kfCompressing remains true until then.
        m_kfCompressMatchId = 0;
        m_kfCompressFrame = -1;
        m_kfCompressRawBytes = 0;
    }
    if (matchId == 0 || m_kfUploadMatchId == matchId)
    {
        m_kfUploadActive = false;
        m_kfUploadMatchId = 0;
        m_kfUploadFrame = -1;
        m_kfUploadChunk = 0;
        m_kfUploadChunkCount = 0;
        m_kfUploadData.clear();
    }
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
    m_connection = transport;
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
    data["lateJoin"] = true;
    data["localPlayers"] = true;
    data["social"] = true;
    data["spectatorChat"] = true;
    data["saveData"] = true;
    data["gameOptions"] = true;
    // A room with more options than older clients take keeps them out of its
    // matches (CoreGameOptionsLegacyMaxOptions).
    data["gameOptionsMax"] = CoreGameOptionsMaxOptions;
    data["pingCheck"] = true;
    if (m_challengesOff)
        data["noChallenges"] = true;
    if (m_privateMessagesOff)
        data["noPrivateMessages"] = true;
    if (!m_pendingLocalIp.isEmpty())
        data["localIp"] = m_pendingLocalIp;

    sendEnvelope("HELLO", data, "hello-1");
}

void LobbyClient::onWsDisconnected()
{
    resetBroadcastKeyframeUpload();
    stopPingDiagnosticLog(QStringLiteral("connection_lost"));
    // The WebSocket is only ICE's signaling/control plane. Once a match has
    // started, its established agents carry GekkoNet traffic peer-to-peer and
    // must outlive a lobby restart or network interruption. Defer teardown
    // until the emulation owner releases the transport.
    resetIceTransport();
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
    m_moderatorUsers.clear();
    // User ids restart when the server does — drop measurements so a recycled
    // id can't inherit another player's ping history.
    m_measuredPing.clear();
    // Ping checks need the lobby to signal and end them. Their agents never
    // carry a match, so they go now even while a match defers the reset.
    for (const quint64 peerId : m_probePeers.keys())
        removeProbePeer(peerId, QStringLiteral("disconnected"));
    m_probeGenerations.clear();
    m_pingCheck = PingCheck{};
    m_heartbeatSentMs.clear();
    m_serverRttMs = 0;
    // In-flight probe series belong to the old connection: their endpoints and
    // user ids are stale, so retrying them would burst at whoever now holds
    // those ids and emit failure notices for peers we're no longer talking to.
    m_pendingProbes.clear();
    m_iceProbes.clear();
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
    else if (type == "HEARTBEAT_ACK")        handleHeartbeatAck(env.value("id").toString());
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
    else if (type == "CHAT_FAIL")            handleChatFail(data);
    else if (type == "CHALLENGE_SENT")       handleChallengeSent(data);
    else if (type == "CHALLENGE_INCOMING")   handleChallengeIncoming(data);
    else if (type == "CHALLENGE_RESULT")     handleChallengeResult(data);
    else if (type == "CHALLENGE_GAMES_REPLY") handleChallengeGamesReply(data);
    else if (type == "PING_PROBE_REPLY")     handlePingProbeReply(data);
    else if (type == "PING_PROBE_INCOMING")  handlePingProbeIncoming(data);
    else if (type == "ICE_SIGNAL")           handleIceSignal(data);
    else if (type == "PING_CHECK_REPLY")     handlePingCheckReply(data, env.value("id").toString());
    else if (type == "PING_CHECK_INCOMING")  handlePingCheckIncoming(data);
    else if (type == "PING_CHECK_END")       handlePingCheckEnd(data);
    else if (type == "MATCH_BEGIN")          handleMatchBegin(data);
    else if (type == "MATCH_PEER_LEFT")      handleMatchPeerLeft(data);
    else if (type == "MATCH_ADMIT_BEGIN")    handleMatchAdmitBegin(data);
    else if (type == "MATCH_ADMIT_SIGNAL")   handleMatchAdmitSignal(data);
    else if (type == "MATCH_ADMIT_FAIL")     handleMatchAdmitFail(data);
    else if (type == "QUICK_MATCH_STATUS")   handleQuickMatchStatus(data);
    else if (type == "LOCAL_PLAYER_ADDED")   handleLocalPlayerAdded(data);
    else if (type == "LOCAL_PLAYER_REMOVED") handleLocalPlayerRemoved(data);
    else if (type == "LOCAL_PLAYER_FAIL")    handleLocalPlayerFail(data);
    else if (type == "SPECTATE_BEGIN")       handleSpectateBegin(data);
    else if (type == "SPECTATE_DATA")        handleSpectateData(data);
    else if (type == "SPECTATE_KEYFRAME")    handleSpectateKeyframe(data);
    else if (type == "SPECTATE_END")         handleSpectateEnd(data);
    else if (type == "SPECTATE_FAIL")        handleSpectateFail(data);
    else if (type == "BROADCAST_VIEWER_COUNT") handleBroadcastViewerCount(data);
    else if (type == "BROADCAST_KEYFRAME_REQUEST") handleBroadcastKeyframeRequest(data);
    else if (type == "ADMIN_AUTH_OK")        handleAdminAuthOk(data);
    else if (type == "ADMIN_AUTH_FAIL")      handleAdminAuthFail(data);
    else if (type == "MOD_NOTICE")           handleModNotice(data);
    else if (type == "MOD_LIST")             handleModList(data);
    else if (type == "ADMIN_LOGGED_OUT")     handleAdminLoggedOut();
    else if (type == "MOD_RESULT")           handleModResult(data);
    else if (type == "MOD_TARGET_INFO")      handleModTargetInfo(data);
    else if (type == "MOD_PEERS")            handleModPeers(data);
    else qDebug() << "lobby: unknown message type" << type;
}

// -------- Specific handlers --------

void LobbyClient::handleHelloOk(const QJsonObject& data)
{
    m_selfUserId  = static_cast<quint64>(data.value("userId").toDouble());
    // The lobby keeps one spelling of a name, which may not be the one typed.
    const QString keptUsername = data.value("username").toString();
    if (!keptUsername.isEmpty())
        m_pendingUsername = keptUsername;
    m_observedIp  = data.value("observedIp").toString();
    m_region      = data.value("region").toString();
    m_localPlayersSupported = data.value("localPlayers").toBool();
    m_socialSupported = data.value("social").toBool();
    m_spectatorChatSupported = data.value("spectatorChat").toBool();
    m_challengeSettingsSupported = data.value("challengeSettings").toBool();
    m_pmSettingsSupported = data.value("pmSettings").toBool();
    m_pingCheckSupported = data.value("pingCheck").toBool();
    emit localPlayersChanged();

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
    const bool iceDiagnostics = CoreSettingsGetBoolValue(SettingsID::Rollback_PingDiagnostics);
    LobbyIce::set_diagnostics_enabled(iceDiagnostics);
    if (!LobbyIce::configure(m_selfUserId, stunHost, stunPort, turnServers))
    {
        emit connectError(QCoreApplication::translate("LobbyClient","Failed to initialize ICE transport."));
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
    QStringList turnEndpoints;
    for (const LobbyIceTurnServer& server : turnServers)
    {
        turnEndpoints.append(QStringLiteral("%1:%2")
                                 .arg(QString::fromStdString(server.host))
                                 .arg(server.port));
    }
    writePingDiagnostic(QStringLiteral("ICE_CONFIG"),
                        QStringLiteral("stun=%1:%2 turn_count=%3 turn=%4 libjuice_level=debug")
                            .arg(QString::fromStdString(stunHost)).arg(stunPort)
                            .arg(static_cast<qulonglong>(turnServers.size()))
                            .arg(turnEndpoints.isEmpty() ? QStringLiteral("<none>")
                                                         : turnEndpoints.join(QLatin1Char(','))));

    setState(ConnectionState::Connected);
    m_heartbeatTimer->start();
    // One now, so the lobby round trip is known by the next heartbeat rather
    // than the one after.
    onHeartbeatTimer();
    // ICE owns the UDP sockets. The legacy lobby anchor is intentionally not
    // opened by ICE-capable clients.
}

void LobbyClient::handleHelloFail(const QJsonObject& data)
{
    const QString reason = data.value("reason").toString();
    const QString message = data.value("message").toString();
    emit helloFailed(message.isEmpty() ? reason : message);
    setState(ConnectionState::Failed);
    if (m_ws)
        m_ws->close();
}

void LobbyClient::handleHeartbeatAck(const QString& envelopeId)
{
    const auto sent = m_heartbeatSentMs.constFind(envelopeId);
    if (envelopeId.isEmpty() || sent == m_heartbeatSentMs.constEnd())
        return;
    const qint64 rttMs = monotonicMs() - sent.value();
    m_heartbeatSentMs.erase(sent);
    if (rttMs < 0 || rttMs > HEARTBEAT_ACK_TIMEOUT_MS)
        return;
    m_serverRttMs = LobbyPing::smoothRtt(m_serverRttMs, int(std::max<qint64>(1, rttMs)));
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
        r.viewerCount  = qMax(0, o.value("viewerCount").toInt());
        r.lateJoin     = o.value("lateJoin").toBool();
        r.privateRoom  = o.value("private").toBool();
        r.maxPing      = qMax(0, o.value("maxPing").toInt());
        r.wiredOnly    = o.value("wiredOnly").toBool();
        r.gameOptions  = parseGameOptions(o.value("gameOptions"));
        r.gameOptionsPreset = parseGameOptionsPreset(o.value("gameOptionsPreset"));
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

quint64 LobbyClient::seatPeerId(const QJsonObject& seat)
{
    const quint64 peerId = static_cast<quint64>(seat.value("peerId").toDouble());
    return peerId != 0 ? peerId : static_cast<quint64>(seat.value("userId").toDouble());
}

QList<LobbyClient::LobbyMatchPeer> LobbyClient::connectionPeers(const QList<LobbyMatchPeer>& peers)
{
    QList<LobbyMatchPeer> connections;
    for (const LobbyMatchPeer& peer : peers)
    {
        if (peer.userId == peer.peerId)
            connections.append(peer);
    }
    return connections;
}

bool LobbyClient::isLocalPlayer(quint64 userId) const
{
    return std::any_of(m_localPlayers.cbegin(), m_localPlayers.cend(),
                       [userId](const LocalPlayer& player) { return player.userId == userId; });
}

int LobbyClient::localPlayerController(quint64 userId) const
{
    for (const LocalPlayer& player : m_localPlayers)
    {
        if (player.userId == userId)
            return player.controller;
    }
    return 0;
}

void LobbyClient::addLocalPlayer(const QString& name, int controller)
{
    m_pendingLocalControllers.insert(name, controller);
    QJsonObject d;
    d["name"] = name;
    sendEnvelope("LOCAL_PLAYER_ADD", d);
}

void LobbyClient::removeLocalPlayer(quint64 userId)
{
    QJsonObject d;
    d["userId"] = QJsonValue(qint64(userId));
    sendEnvelope("LOCAL_PLAYER_REMOVE", d);
}

void LobbyClient::setLocalPlayerController(quint64 userId, int controller)
{
    for (LocalPlayer& player : m_localPlayers)
    {
        if (player.userId == userId)
            player.controller = controller;
    }
    emit localPlayersChanged();
}

void LobbyClient::handleLocalPlayerAdded(const QJsonObject& data)
{
    LocalPlayer player;
    player.userId = static_cast<quint64>(data.value("userId").toDouble());
    player.name = data.value("name").toString();
    player.controller = m_pendingLocalControllers.take(player.name);
    m_localPlayers.append(player);
    emit localPlayersChanged();
}

void LobbyClient::handleLocalPlayerRemoved(const QJsonObject& data)
{
    const quint64 userId = static_cast<quint64>(data.value("userId").toDouble());
    m_localPlayers.removeIf([userId](const LocalPlayer& player) { return player.userId == userId; });
    emit localPlayersChanged();
}

void LobbyClient::handleLocalPlayerFail(const QJsonObject& data)
{
    const QString name = data.value("name").toString();
    m_pendingLocalControllers.remove(name);
    emit localPlayerFailed(name, data.value("reason").toString());
}

void LobbyClient::reconcileIcePeers(const QJsonObject& roomState)
{
    const quint64 roomId = static_cast<quint64>(roomState.value("id").toDouble());

    m_seatPeers.clear();
    for (const QJsonValue& value : roomState.value("players").toArray())
    {
        const QJsonObject player = value.toObject();
        const quint64 userId = static_cast<quint64>(player.value("userId").toDouble());
        const quint64 peerId = seatPeerId(player);
        m_seatPeers.insert(userId, peerId);
        // Seated with us: the room mesh measures them from here on. The other
        // side drops its probe agent on the same room state.
        dropProbePeerForMesh(userId);
        dropProbePeerForMesh(peerId);
    }

    // Room state is control-plane data. Once GekkoNet owns the established
    // mesh, no server-side room transition may replace or remove the agents
    // it's using; actual actor teardown is driven by emulation finish. Members
    // outside the match are another matter: one who joins mid-match gets an
    // agent, so a late join finds them already connected, and one who leaves
    // loses it, since their next agent can't pair with the old one.
    if (m_matchTransportActive)
    {
        if (roomId != m_currentIceRoomId)
            return;
        m_iceOutsideMatch.clear();
        QSet<quint64> seated;
        for (const QJsonValue& value : roomState.value("players").toArray())
        {
            const QJsonObject player = value.toObject();
            const quint64 userId = seatPeerId(player);
            if (userId == 0 || userId == m_selfUserId)
                continue;
            seated.insert(userId);
            // Older servers don't send it; everyone seated is in their matches.
            if (!player.value("inMatch").toBool(true))
                m_iceOutsideMatch.insert(userId);
            if (player.value("iceSupported").toBool())
                ensureIcePeer(userId);
        }
        // The server drops anyone leaving from the match before unseating them.
        for (quint64 userId : m_icePeerIds - seated)
            removeIcePeer(userId);
        return;
    }

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
        const quint64 userId = seatPeerId(player);
        if (userId == 0 || userId == m_selfUserId)
            continue;
        if (!player.value("iceSupported").toBool())
        {
            qWarning() << "Rollback lobby peer does not support ICE" << userId;
            continue;
        }
        desired.insert(userId);
        if (!m_icePeerIds.contains(userId))
        {
            if (!m_iceGenerations.contains(userId))
                m_iceGenerations.insert(userId, 0);
            if (!m_iceRestartAttempts.contains(userId))
                m_iceRestartAttempts.insert(userId, 0);
            if (LobbyIce::add_peer(userId))
            {
                m_icePeerIds.insert(userId);
                emit icePeerConnectionAttemptChanged(
                    userId, m_iceRestartAttempts.value(userId, 0) + 1,
                    MAX_ICE_CONNECTION_ATTEMPTS);
                writePingDiagnostic(QStringLiteral("ICE_PEER_ADDED"),
                                    QStringLiteral("peer=%1 room=%2")
                                        .arg(pingUserLabel(userId)).arg(roomId));
            }
            else
            {
                writePingDiagnostic(QStringLiteral("ICE_PEER_ADD_FAILED"),
                                    QStringLiteral("peer=%1 room=%2")
                                        .arg(pingUserLabel(userId)).arg(roomId));
            }
        }
    }

    const QSet<quint64> stale = m_icePeerIds - desired;
    for (quint64 userId : stale)
        removeIcePeer(userId);
}

void LobbyClient::removeIcePeer(quint64 userId)
{
    writePingDiagnostic(QStringLiteral("ICE_PEER_REMOVED"),
                        QStringLiteral("peer=%1 room=%2 reason=not_in_room_state")
                            .arg(pingUserLabel(userId)).arg(m_currentIceRoomId));
    LobbyIce::remove_peer(userId);
    m_icePeerIds.remove(userId);
    m_lastIceStates.remove(userId);
    m_iceGenerations.remove(userId);
    m_iceRestartAttempts.remove(userId);
    m_roomPeerPings.remove(userId);
    for (auto it = m_roomPeerPings.begin(); it != m_roomPeerPings.end(); ++it)
        it.value().remove(userId);
}

bool LobbyClient::iceAgentInUseByMatch(quint64 userId) const
{
    return m_matchTransportActive && !m_iceOutsideMatch.contains(userId);
}

void LobbyClient::resetIceMesh()
{
    if (m_matchTransportActive)
    {
        m_deferredIceMeshReset = true;
        return;
    }

    for (quint64 userId : std::as_const(m_icePeerIds))
    {
        LobbyIce::remove_peer(userId);
        m_iceProbes.cancel(userId);
    }
    m_icePeerIds.clear();
    m_lastIceStates.clear();
    m_iceGenerations.clear();
    m_iceRestartAttempts.clear();
    m_currentIceRoomId = 0;
    m_currentIceHostUserId = 0;
    m_roomPeerPings.clear();
    m_pendingProbes.clear();
    m_seatPeers.clear();
}

void LobbyClient::resetIceTransport()
{
    if (m_matchTransportActive)
    {
        // A full reset was requested (normally because the lobby WebSocket
        // went away). Keep both the peer map and queued Gekko packets intact
        // until the running match has actually stopped.
        m_deferredIceTransportReset = true;
        return;
    }

    resetIceMesh();
    LobbyIce::reset();
    m_probePeers.clear();
    m_probeGenerations.clear();
    m_iceProbes.clear();
    m_deferredIceMeshReset = false;
    m_deferredIceTransportReset = false;
}

void LobbyClient::setMatchTransportActive(bool active)
{
    if (m_matchTransportActive == active)
        return;

    m_matchTransportActive = active;
    m_iceOutsideMatch.clear();
    if (active)
        return;

    // A lobby disconnect requires a full reset, while an ordinary ROOM_LEFT
    // only requires removal of the old room mesh. Full reset subsumes mesh
    // reset and intentionally clears the obsolete lobby identity/config.
    if (m_deferredIceTransportReset)
    {
        resetIceTransport();
    }
    else if (m_deferredIceMeshReset)
    {
        m_deferredIceMeshReset = false;
        resetIceMesh();
    }
}

void LobbyClient::handleIceSignal(const QJsonObject& data)
{
    if (data.value("probe").toBool())
    {
        handleProbeIceSignal(data);
        return;
    }

    const quint64 roomId = static_cast<quint64>(data.value("roomId").toDouble());
    const quint64 fromUserId = static_cast<quint64>(data.value("fromUserId").toDouble());
    const QString kind = data.value("kind").toString();
    const quint32 generation = static_cast<quint32>(data.value("generation").toInt());
    if (roomId == 0 || roomId != m_currentIceRoomId || fromUserId == 0 ||
        fromUserId == m_selfUserId)
    {
        writePingDiagnostic(QStringLiteral("ICE_SIGNAL_IN_DROPPED"),
                            QStringLiteral("from_user=%1 signal_room=%2 current_room=%3 kind=%4 reason=scope_validation")
                                .arg(fromUserId).arg(roomId).arg(m_currentIceRoomId).arg(kind));
        return;
    }
    // They're building the room's agent for us, so our ping-check agent for
    // them can't take this. Without it the signal waits for the mesh's own.
    dropProbePeerForMesh(fromUserId);

    const quint32 currentGeneration = m_iceGenerations.value(fromUserId, 0);
    if (generation < currentGeneration)
    {
        writePingDiagnostic(QStringLiteral("ICE_SIGNAL_IN_DROPPED"),
                            QStringLiteral("peer=%1 room=%2 kind=%3 generation=%4 current_generation=%5 reason=stale_generation")
                                .arg(pingUserLabel(fromUserId)).arg(roomId).arg(kind)
                                .arg(generation).arg(currentGeneration));
        return;
    }

    // Replacing an agent underneath GekkoNet interrupts the active data plane.
    // Same-generation trickle candidates remain safe to apply, but ICE restart
    // signaling is deferred until the match has released the mesh. Someone
    // waiting beside the match isn't using it yet and has to be able to connect.
    if (iceAgentInUseByMatch(fromUserId) &&
        (kind == QStringLiteral("restart") || generation > currentGeneration))
    {
        writePingDiagnostic(QStringLiteral("ICE_SIGNAL_IN_DROPPED"),
                            QStringLiteral("peer=%1 room=%2 kind=%3 generation=%4 reason=match_transport_active")
                                .arg(pingUserLabel(fromUserId)).arg(roomId).arg(kind).arg(generation));
        return;
    }

    if (kind == QStringLiteral("restart"))
    {
        if (generation == 0 || generation == currentGeneration)
        {
            writePingDiagnostic(QStringLiteral("ICE_RESTART_IN_IGNORED"),
                                QStringLiteral("peer=%1 room=%2 generation=%3 current_generation=%4 reason=invalid_or_duplicate")
                                    .arg(pingUserLabel(fromUserId)).arg(roomId)
                                    .arg(generation).arg(currentGeneration));
            return;
        }

        if (!m_icePeerIds.contains(fromUserId))
        {
            LobbyIce::remove_peer(fromUserId);
            m_iceGenerations[fromUserId] = generation;
            m_iceRestartAttempts[fromUserId] =
                m_iceRestartAttempts.value(fromUserId, 0) + 1;
            writePingDiagnostic(QStringLiteral("ICE_RESTART_IN_QUEUED"),
                                QStringLiteral("peer=%1 room=%2 generation=%3")
                                    .arg(pingUserLabel(fromUserId)).arg(roomId).arg(generation));
            return;
        }

        restartIcePeer(fromUserId, generation, QStringLiteral("remote_request"), false);
        return;
    }

    // A generation-scoped description is also an implicit restart request.
    // This covers a client that reconnects between the explicit restart and
    // the new description without ever allowing old candidates into its agent.
    if (generation > currentGeneration)
    {
        if (m_icePeerIds.contains(fromUserId))
            restartIcePeer(fromUserId, generation, QStringLiteral("new_generation_signal"), false);
        else
        {
            LobbyIce::remove_peer(fromUserId);
            m_iceGenerations[fromUserId] = generation;
        }
    }

    const QString value = data.value("value").toString();
    const bool hadPeer = LobbyIce::has_peer(fromUserId);
    const bool accepted = LobbyIce::handle_remote_signal(fromUserId, kind.toStdString(),
                                                         value.toStdString());
    writePingDiagnostic(QStringLiteral("ICE_SIGNAL_IN"),
                        QStringLiteral("peer=%1 room=%2 kind=%3 generation=%4 disposition=%5 accepted=%6 %7")
                            .arg(pingUserLabel(fromUserId)).arg(roomId).arg(kind)
                            .arg(generation)
                            .arg(hadPeer ? QStringLiteral("applied") : QStringLiteral("queued"))
                            .arg(accepted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(iceSignalSummary(kind, value)));
}

bool LobbyClient::restartIcePeer(quint64 userId, quint32 generation,
                                 const QString& reason, bool notifyPeer)
{
    if (iceAgentInUseByMatch(userId) || m_currentIceRoomId == 0 || !m_icePeerIds.contains(userId) ||
        generation == 0 || generation <= m_iceGenerations.value(userId, 0) ||
        m_iceRestartAttempts.value(userId, 0) >= MAX_ICE_AUTO_RESTARTS)
    {
        return false;
    }

    if (notifyPeer)
    {
        QJsonObject data;
        data["targetUserId"] = QJsonValue(qint64(userId));
        data["roomId"] = QJsonValue(qint64(m_currentIceRoomId));
        data["kind"] = QStringLiteral("restart");
        data["generation"] = int(generation);
        writePingDiagnostic(QStringLiteral("ICE_RESTART_OUT"),
                            QStringLiteral("peer=%1 room=%2 generation=%3 reason=%4")
                                .arg(pingUserLabel(userId)).arg(m_currentIceRoomId)
                                .arg(generation).arg(reason));
        sendEnvelope("ICE_SIGNAL", data);
    }

    m_iceGenerations[userId] = generation;
    m_iceRestartAttempts[userId] = m_iceRestartAttempts.value(userId, 0) + 1;
    m_lastIceStates.remove(userId);
    emit icePeerConnectionAttemptChanged(
        userId, m_iceRestartAttempts.value(userId) + 1,
        MAX_ICE_CONNECTION_ATTEMPTS);

    m_iceProbes.cancel(userId);
    for (auto it = m_pendingProbes.begin(); it != m_pendingProbes.end();)
    {
        if (it->targetUserId == userId)
            it = m_pendingProbes.erase(it);
        else
            ++it;
    }

    LobbyIce::remove_peer(userId);
    const bool added = LobbyIce::add_peer(userId);
    writePingDiagnostic(QStringLiteral("ICE_RESTART_APPLIED"),
                        QStringLiteral("peer=%1 room=%2 generation=%3 reason=%4 added=%5")
                            .arg(pingUserLabel(userId)).arg(m_currentIceRoomId)
                            .arg(generation).arg(reason)
                            .arg(added ? QStringLiteral("true") : QStringLiteral("false")));
    if (!added)
        emit icePeerConnectionChanged(userId, false, true);
    return added;
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
    for (LobbyIceDiagnostic& diagnostic : LobbyIce::take_diagnostics())
    {
        writePingDiagnostic(QStringLiteral("LIBJUICE"),
                            QStringLiteral("level=%1 message=%2")
                                .arg(QString::fromStdString(diagnostic.level),
                                     redactLibjuiceMessage(
                                         QString::fromStdString(diagnostic.message))));
    }

    if (m_ws && m_ws->state() == QAbstractSocket::ConnectedState &&
        (m_currentIceRoomId != 0 || !m_probePeers.isEmpty()))
    {
        for (LobbyIceSignal& signal : LobbyIce::take_local_signals())
        {
            // A ping check's agent signals outside any room.
            if (const auto probe = m_probePeers.constFind(signal.peerUserId); probe != m_probePeers.constEnd())
            {
                QJsonObject data;
                data["targetUserId"] = QJsonValue(qint64(signal.peerUserId));
                data["roomId"] = 0;
                data["probe"] = true;
                data["kind"] = QString::fromStdString(signal.kind);
                data["generation"] = int(probe->generation);
                if (!signal.value.empty())
                    data["value"] = QString::fromStdString(signal.value);
                writePingDiagnostic(QStringLiteral("PROBE_SIGNAL_OUT"),
                                    QStringLiteral("peer=%1 kind=%2 generation=%3 %4")
                                        .arg(pingUserLabel(signal.peerUserId), QString::fromStdString(signal.kind))
                                        .arg(probe->generation)
                                        .arg(iceSignalSummary(QString::fromStdString(signal.kind),
                                                              QString::fromStdString(signal.value))));
                sendEnvelope("ICE_SIGNAL", data);
                continue;
            }
            if (m_currentIceRoomId == 0)
                continue; // no room mesh for it to belong to
            QJsonObject data;
            data["targetUserId"] = QJsonValue(qint64(signal.peerUserId));
            data["roomId"] = QJsonValue(qint64(m_currentIceRoomId));
            data["kind"] = QString::fromStdString(signal.kind);
            data["generation"] = int(m_iceGenerations.value(signal.peerUserId, 0));
            if (!signal.value.empty())
                data["value"] = QString::fromStdString(signal.value);
            const QString kind = QString::fromStdString(signal.kind);
            const QString value = QString::fromStdString(signal.value);
            writePingDiagnostic(QStringLiteral("ICE_SIGNAL_OUT"),
                                QStringLiteral("peer=%1 room=%2 kind=%3 generation=%4 %5")
                                    .arg(pingUserLabel(signal.peerUserId))
                                    .arg(m_currentIceRoomId).arg(kind)
                                    .arg(m_iceGenerations.value(signal.peerUserId, 0))
                                    .arg(iceSignalSummary(kind, value)));
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
        // Timed against the attempt this nonce went out with: libjuice's own
        // timing when it has one, else the time since that attempt.
        const auto sample = m_iceProbes.reply(packet.peerUserId, nonce, packet.roundTripUs, monotonicMs());
        if (!sample)
            continue;
        const int rttMs = sample->rttMs;
        m_probeFailStreak[packet.peerUserId] = 0;
        writePingDiagnostic(QStringLiteral("ICE_PING_REPLY"),
                            QStringLiteral("peer=%1 rtt_ms=%2 rtt_us=%3 attempt=%4 late=%5")
                                .arg(pingUserLabel(packet.peerUserId)).arg(rttMs)
                                .arg(packet.roundTripUs == 0 ? qint64(-1) : qint64(packet.roundTripUs))
                                .arg(sample->attempt)
                                .arg(sample->late ? QStringLiteral("true") : QStringLiteral("false")));

        const auto probe = m_probePeers.find(packet.peerUserId);
        if (probe == m_probePeers.end())
        {
            sendRoomPingReport(packet.peerUserId, rttMs);
            recordPingSample(packet.peerUserId, rttMs, false);
            continue;
        }
        // A ping check's sample is kept under the id this PC knows them by.
        recordPingSample(probe->displayId, rttMs, true);
        if (m_pingCheck.roomId != 0 && m_pingCheck.peerId == packet.peerUserId)
        {
            m_pingCheck.samples.append(rttMs);
            const int best = *std::min_element(m_pingCheck.samples.cbegin(), m_pingCheck.samples.cend());
            if (best <= m_pingCheck.maxPingMs || m_pingCheck.samples.size() >= PING_CHECK_SAMPLES)
                finishPingCheck(best, true, QStringLiteral("measured"));
        }
    }

    std::vector<quint64> peersToRestart;
    for (quint64 userId : std::as_const(m_icePeerIds))
    {
        const int state = static_cast<int>(LobbyIce::peer_state(userId));
        if (m_lastIceStates.value(userId, -1) == state)
            continue;
        m_lastIceStates[userId] = state;
        writePingDiagnostic(QStringLiteral("ICE_STATE"),
                            QStringLiteral("peer=%1 state=%2 state_name=%3 selected=%4")
                                .arg(pingUserLabel(userId)).arg(state).arg(iceStateName(state))
                                .arg(QString::fromStdString(LobbyIce::peer_selected_addresses(userId))));
        const bool connected = LobbyIce::peer_connected(userId);
        const bool failed = state == static_cast<int>(LobbyIcePeerState::Failed);
        if (failed && m_iceRestartAttempts.value(userId, 0) < MAX_ICE_AUTO_RESTARTS)
        {
            // Keep the seat in its connecting state while the coordinated
            // replacement happens; only the fresh session's failure is final.
            emit icePeerConnectionChanged(userId, false, false);
            peersToRestart.push_back(userId);
            continue;
        }
        if (state == static_cast<int>(LobbyIcePeerState::Completed))
            m_iceRestartAttempts[userId] = 0;
        emit icePeerConnectionChanged(userId, connected, failed);
        if (connected && userId == m_currentIceHostUserId)
        {
            // A peer-to-peer measurement may have completed before our host
            // path did. Send any canonical reports now instead of waiting for
            // the next three-second probe cycle.
            for (auto it = m_measuredPing.constBegin(); it != m_measuredPing.constEnd(); ++it)
                sendRoomPingReport(it.key(), it.value().ms);
        }
    }

    for (quint64 userId : peersToRestart)
    {
        const quint32 nextGeneration = m_iceGenerations.value(userId, 0) + 1;
        restartIcePeer(userId, nextGeneration, QStringLiteral("connectivity_failed"), true);
    }

    // Ping-check agents. Nothing here reaches the room: no seat state, no
    // connection signals, just when to ping and whether to try again.
    std::vector<quint64> probesToRestart;
    std::vector<quint64> probesFailed;
    for (auto it = m_probePeers.begin(); it != m_probePeers.end(); ++it)
    {
        const int state = static_cast<int>(LobbyIce::peer_state(it.key()));
        if (it->lastState == state)
            continue;
        it->lastState = state;
        writePingDiagnostic(QStringLiteral("PROBE_PEER_STATE"),
                            QStringLiteral("peer=%1 state=%2 state_name=%3 generation=%4 selected=%5")
                                .arg(pingUserLabel(it.key())).arg(state).arg(iceStateName(state))
                                .arg(it->generation)
                                .arg(QString::fromStdString(LobbyIce::peer_selected_addresses(it.key()))));
        if (LobbyIce::peer_connected(it.key()))
        {
            if (it->nextProbeMs == 0)
                it->nextProbeMs = monotonicMs();
        }
        else if (state == static_cast<int>(LobbyIcePeerState::Failed))
        {
            if (it->restarts < MAX_PROBE_PEER_RESTARTS)
                probesToRestart.push_back(it.key());
            else
                probesFailed.push_back(it.key());
        }
    }
    for (quint64 peerId : probesToRestart)
    {
        const auto probe = m_probePeers.constFind(peerId);
        if (probe != m_probePeers.constEnd() &&
            !restartProbePeer(peerId, probe->generation + 1, QStringLiteral("connectivity_failed"), true))
            probesFailed.push_back(peerId);
    }
    for (quint64 peerId : probesFailed)
    {
        if (m_pingCheck.roomId != 0 && m_pingCheck.peerId == peerId)
            finishPingCheck(-1, false, QStringLiteral("ice_failed"));
        removeProbePeer(peerId, QStringLiteral("ice_failed"));
    }
}

void LobbyClient::recordPingSample(quint64 sampleId, int rttMs, bool fromProbe)
{
    m_measuredPing.insert(sampleId, MeasuredPing{rttMs, monotonicMs(), fromProbe, false});
    emit pingProbeMeasured(sampleId, rttMs);
}

void LobbyClient::checkMeasurementExpiry()
{
    const qint64 nowMs = monotonicMs();
    if (nowMs < m_nextExpiryCheckMs)
        return;
    m_nextExpiryCheckMs = nowMs + 1'000;
    QList<quint64> expired;
    for (auto it = m_measuredPing.begin(); it != m_measuredPing.end(); ++it)
    {
        if (it->expiryAnnounced ||
            LobbyPing::measurementCurrent(it->atMs, nowMs, LobbyIce::peer_connected(it.key())))
            continue;
        it->expiryAnnounced = true;
        expired.append(it.key());
    }
    for (quint64 userId : expired)
        emit pingMeasurementExpired(userId);
}

// -------- Ping checks (probe-only ICE agents) --------

void LobbyClient::startProbePeer(quint64 peerId, quint64 displayId, qint64 expiresInMs,
                                 quint64 challengeId, bool active, const QString& reason)
{
    if (!m_pingCheckSupported || peerId == 0 || isOnThisPc(peerId) || displayId == 0)
        return;
    // Already connected through the room mesh, which measures them anyway.
    if (m_icePeerIds.contains(peerId))
        return;

    const qint64 expiresAtMs = monotonicMs() + std::clamp<qint64>(expiresInMs, 1'000, 5 * 60'000);
    const auto existing = m_probePeers.find(peerId);
    if (existing != m_probePeers.end())
    {
        // A second reason to measure the same PC shares its agent.
        existing->expiresAtMs = std::max(existing->expiresAtMs, expiresAtMs);
        existing->active = existing->active || active;
        if (challengeId != 0)
            existing->challengeIds.insert(challengeId);
        return;
    }
    if (!LobbyIce::add_peer(peerId, true))
    {
        writePingDiagnostic(QStringLiteral("PROBE_PEER_ADD_FAILED"),
                            QStringLiteral("peer=%1 reason=%2").arg(pingUserLabel(peerId), reason));
        return;
    }
    ProbePeer probe;
    probe.displayId = displayId;
    probe.expiresAtMs = expiresAtMs;
    probe.active = active;
    if (const auto last = m_probeGenerations.constFind(peerId); last != m_probeGenerations.constEnd())
        probe.generation = last.value() + 1;
    m_probeGenerations.insert(peerId, probe.generation);
    if (challengeId != 0)
        probe.challengeIds.insert(challengeId);
    m_probePeers.insert(peerId, probe);
    writePingDiagnostic(QStringLiteral("PROBE_PEER_ADDED"),
                        QStringLiteral("peer=%1 reason=%2 active=%3 expires_in_ms=%4")
                            .arg(pingUserLabel(peerId), reason,
                                 active ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(expiresInMs));
}

void LobbyClient::removeProbePeer(quint64 peerId, const QString& reason)
{
    if (!m_probePeers.remove(peerId))
        return;
    m_iceProbes.cancel(peerId);
    // The mesh may hold an agent under the same id by now; that one stays.
    if (!m_icePeerIds.contains(peerId))
        LobbyIce::remove_peer(peerId);
    writePingDiagnostic(QStringLiteral("PROBE_PEER_REMOVED"),
                        QStringLiteral("peer=%1 reason=%2").arg(pingUserLabel(peerId), reason));
}

void LobbyClient::dropProbePeerForMesh(quint64 userId)
{
    if (userId == 0 || userId == m_selfUserId)
        return;
    // A ping check's sample came over its own agent, not the room's: the room
    // measures again before it gates Start or sets anyone's delay.
    const auto dropProbeSample = [this](quint64 id) {
        const auto sample = m_measuredPing.constFind(id);
        if (sample != m_measuredPing.constEnd() && sample->fromProbe)
            m_measuredPing.erase(sample);
    };
    dropProbeSample(userId);
    const auto probe = m_probePeers.constFind(userId);
    if (probe == m_probePeers.constEnd())
        return;
    dropProbeSample(probe->displayId);
    if (m_pingCheck.roomId != 0 && m_pingCheck.peerId == userId)
        m_pingCheck = PingCheck{};
    removeProbePeer(userId, QStringLiteral("room_mesh"));
}

bool LobbyClient::restartProbePeer(quint64 peerId, quint32 generation, const QString& reason, bool notifyPeer)
{
    const auto probe = m_probePeers.find(peerId);
    if (probe == m_probePeers.end() || generation <= probe->generation || m_icePeerIds.contains(peerId))
        return false;
    if (notifyPeer)
    {
        QJsonObject data;
        data["targetUserId"] = QJsonValue(qint64(peerId));
        data["roomId"] = 0;
        data["probe"] = true;
        data["kind"] = QStringLiteral("restart");
        data["generation"] = int(generation);
        sendEnvelope("ICE_SIGNAL", data);
    }
    probe->generation = generation;
    m_probeGenerations.insert(peerId, generation);
    probe->restarts += 1;
    probe->lastState = -1;
    probe->nextProbeMs = 0;
    m_iceProbes.cancel(peerId);
    LobbyIce::remove_peer(peerId);
    const bool added = LobbyIce::add_peer(peerId, true);
    writePingDiagnostic(QStringLiteral("PROBE_PEER_RESTART"),
                        QStringLiteral("peer=%1 generation=%2 reason=%3 added=%4")
                            .arg(pingUserLabel(peerId)).arg(generation).arg(reason)
                            .arg(added ? QStringLiteral("true") : QStringLiteral("false")));
    return added;
}

void LobbyClient::handleProbeIceSignal(const QJsonObject& data)
{
    const quint64 fromUserId = static_cast<quint64>(data.value("fromUserId").toDouble());
    const QString kind = data.value("kind").toString();
    const quint32 generation = static_cast<quint32>(data.value("generation").toInt());
    const auto probe = m_probePeers.find(fromUserId);
    // Only a ping check this PC is part of, and never one the mesh owns now:
    // a late trickle from their old probe agent must not reach the room's.
    if (!m_pingCheckSupported || probe == m_probePeers.end() || m_icePeerIds.contains(fromUserId))
    {
        writePingDiagnostic(QStringLiteral("PROBE_SIGNAL_IN_DROPPED"),
                            QStringLiteral("from_user=%1 kind=%2 generation=%3 reason=no_ping_check")
                                .arg(fromUserId).arg(kind).arg(generation));
        return;
    }
    if (generation < probe->generation)
    {
        writePingDiagnostic(QStringLiteral("PROBE_SIGNAL_IN_DROPPED"),
                            QStringLiteral("peer=%1 kind=%2 generation=%3 current_generation=%4 reason=stale_generation")
                                .arg(pingUserLabel(fromUserId), kind).arg(generation).arg(probe->generation));
        return;
    }
    if (kind == QStringLiteral("restart"))
    {
        if (generation > probe->generation)
            restartProbePeer(fromUserId, generation, QStringLiteral("remote_request"), false);
        return;
    }
    // Like the mesh: a newer generation's description is a restart too.
    if (generation > probe->generation &&
        !restartProbePeer(fromUserId, generation, QStringLiteral("new_generation_signal"), false))
        return;
    // Never into LobbyIce's queue for an agent that doesn't exist: that queue
    // feeds whatever agent gets the id next, which could be the room's.
    if (!LobbyIce::has_peer(fromUserId))
        return;
    const QString value = data.value("value").toString();
    const bool accepted = LobbyIce::handle_remote_signal(fromUserId, kind.toStdString(), value.toStdString());
    writePingDiagnostic(QStringLiteral("PROBE_SIGNAL_IN"),
                        QStringLiteral("peer=%1 kind=%2 generation=%3 accepted=%4 %5")
                            .arg(pingUserLabel(fromUserId), kind).arg(generation)
                            .arg(accepted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(iceSignalSummary(kind, value)));
}

void LobbyClient::serviceProbePeers()
{
    if (m_probePeers.isEmpty() && m_pingCheck.roomId == 0)
        return;
    const qint64 nowMs = monotonicMs();

    if (m_pingCheck.roomId != 0 && nowMs >= m_pingCheck.deadlineMs)
    {
        if (m_pingCheck.samples.isEmpty())
            finishPingCheck(-1, false, QStringLiteral("timeout"));
        else
            finishPingCheck(*std::min_element(m_pingCheck.samples.cbegin(), m_pingCheck.samples.cend()),
                            true, QStringLiteral("timeout_with_samples"));
    }

    QList<quint64> expired;
    for (auto it = m_probePeers.begin(); it != m_probePeers.end(); ++it)
    {
        if (nowMs >= it->expiresAtMs)
        {
            expired.append(it.key());
            continue;
        }
        if (!it->active || it->nextProbeMs == 0 || nowMs < it->nextProbeMs ||
            !LobbyIce::peer_connected(it.key()))
            continue;
        if (startIcePingSeries(it.key()))
        {
            it->nextProbeMs = nowMs + probePeerIntervalMs(it->seriesStarted);
            it->seriesStarted += 1;
        }
    }
    for (quint64 peerId : expired)
    {
        if (m_pingCheck.roomId != 0 && m_pingCheck.peerId == peerId)
            finishPingCheck(-1, false, QStringLiteral("grant_expired"));
        removeProbePeer(peerId, QStringLiteral("expired"));
    }
}

bool LobbyClient::startPingCheck(quint64 roomId, const QString& password, quint64 hostId, int maxPingMs)
{
    if (!m_pingCheckSupported || m_state != ConnectionState::Connected || roomId == 0 ||
        hostId == 0 || maxPingMs <= 0)
        return false;
    cancelPingCheck();

    PingCheck check;
    check.roomId = roomId;
    check.hostId = hostId;
    check.maxPingMs = maxPingMs;
    check.requestId = QStringLiteral("ping-check-%1").arg(++m_pingCheckSeq);
    check.deadlineMs = monotonicMs() + PING_CHECK_TIMEOUT_MS;
    QJsonObject d;
    d["roomId"] = QJsonValue(qint64(roomId));
    if (!password.isEmpty())
        d["password"] = password;
    if (sendEnvelope("PING_CHECK_REQUEST", d, check.requestId) <= 0)
        return false;
    m_pingCheck = check;
    writePingDiagnostic(QStringLiteral("PING_CHECK_REQUEST"),
                        QStringLiteral("room=%1 host=%2 max_ping_ms=%3")
                            .arg(roomId).arg(pingUserLabel(hostId)).arg(maxPingMs));
    return true;
}

void LobbyClient::cancelPingCheck()
{
    if (m_pingCheck.roomId == 0)
        return;
    const PingCheck check = m_pingCheck;
    m_pingCheck = PingCheck{};
    writePingDiagnostic(QStringLiteral("PING_CHECK_CANCELLED"), QStringLiteral("room=%1").arg(check.roomId));
    const auto probe = m_probePeers.constFind(check.peerId);
    if (check.peerId != 0 && probe != m_probePeers.constEnd() && probe->challengeIds.isEmpty())
        removeProbePeer(check.peerId, QStringLiteral("ping_check_cancelled"));
}

void LobbyClient::finishPingCheck(int pingMs, bool measured, const QString& reason)
{
    if (m_pingCheck.roomId == 0)
        return;
    const PingCheck check = m_pingCheck;
    m_pingCheck = PingCheck{};
    writePingDiagnostic(QStringLiteral("PING_CHECK_FINISHED"),
                        QStringLiteral("room=%1 ping_ms=%2 measured=%3 samples=%4 reason=%5")
                            .arg(check.roomId).arg(pingMs)
                            .arg(measured ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(check.samples.size()).arg(reason));
    // Done with it: if the join goes ahead, the room's own agent replaces it.
    const auto probe = m_probePeers.constFind(check.peerId);
    if (check.peerId != 0 && probe != m_probePeers.constEnd() && probe->challengeIds.isEmpty())
        removeProbePeer(check.peerId, QStringLiteral("ping_check_finished"));
    emit pingCheckFinished(check.roomId, measured ? pingMs : -1, measured);
}

void LobbyClient::handlePingCheckReply(const QJsonObject& data, const QString& envelopeId)
{
    const quint64 roomId = static_cast<quint64>(data.value("roomId").toDouble());
    // Only the check still waiting: a cancelled one's grant just runs out.
    if (m_pingCheck.roomId == 0 || roomId != m_pingCheck.roomId ||
        (!envelopeId.isEmpty() && envelopeId != m_pingCheck.requestId) || m_pingCheck.peerId != 0)
        return;
    const quint64 peerId = static_cast<quint64>(data.value("peerUserId").toDouble());
    if (!data.value("ok").toBool() || peerId == 0 || isOnThisPc(peerId))
    {
        finishPingCheck(-1, false, QStringLiteral("refused:") + data.value("reason").toString());
        return;
    }
    m_pingCheck.peerId = peerId;
    startProbePeer(peerId, m_pingCheck.hostId,
                   static_cast<qint64>(data.value("expiresInMs").toDouble()), 0, true,
                   QStringLiteral("join_check"));
    if (!m_probePeers.contains(peerId))
        finishPingCheck(-1, false, QStringLiteral("agent_failed"));
}

void LobbyClient::handlePingCheckIncoming(const QJsonObject& data)
{
    // A player about to join our room measures us. Their side pings; ours
    // only answers, which libjuice does as the packets arrive.
    const quint64 fromUserId = static_cast<quint64>(data.value("fromUserId").toDouble());
    startProbePeer(fromUserId, fromUserId,
                   static_cast<qint64>(data.value("expiresInMs").toDouble()) + PROBE_PEER_SLACK_MS,
                   0, false, QStringLiteral("join_check_host"));
}

void LobbyClient::handlePingCheckEnd(const QJsonObject& data)
{
    const quint64 peerId = static_cast<quint64>(data.value("peerUserId").toDouble());
    if (!m_probePeers.contains(peerId))
        return;
    if (m_pingCheck.roomId != 0 && m_pingCheck.peerId == peerId)
    {
        if (m_pingCheck.samples.isEmpty())
            finishPingCheck(-1, false, QStringLiteral("ended"));
        else
            finishPingCheck(*std::min_element(m_pingCheck.samples.cbegin(), m_pingCheck.samples.cend()),
                            true, QStringLiteral("ended_with_samples"));
    }
    removeProbePeer(peerId, QStringLiteral("ping_check_end"));
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
    m.fromPlayer     = data.value("fromPlayer").toBool();
    m.matchId        = static_cast<quint64>(data.value("matchId").toDouble());
    m.toUserId       = static_cast<quint64>(data.value("toUserId").toDouble());
    m.toUsername     = data.value("toUsername").toString();
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
    // ICE ping series: every retry goes out under a fresh nonce.
    if (!m_iceProbes.empty())
    {
        std::vector<LobbyPing::ProbeSeries::Attempt> resend;
        std::vector<std::uint64_t> failed;
        m_iceProbes.tick(monotonicMs(), []() { return QRandomGenerator::global()->generate64(); },
                         resend, failed);
        for (const auto& attempt : resend)
        {
            sendIcePing(attempt.peer, attempt.nonce);
            writePingDiagnostic(QStringLiteral("PROBE_RETRY"),
                                QStringLiteral("peer=%1 transport=ice nonce=%2 attempt=%3/%4")
                                    .arg(pingUserLabel(attempt.peer))
                                    .arg(attempt.nonce)
                                    .arg(attempt.attempt)
                                    .arg(PROBE_ATTEMPTS));
            // A ping check's misses aren't a seat's: nothing to show for them.
            if (!m_probePeers.contains(attempt.peer))
                emit pingProbeRetrying(attempt.peer, attempt.attempt, PROBE_ATTEMPTS);
        }
        for (const std::uint64_t peer : failed)
        {
            writePingDiagnostic(QStringLiteral("PROBE_FAILED"),
                                QStringLiteral("peer=%1 transport=ice attempts=%2")
                                    .arg(pingUserLabel(peer))
                                    .arg(PROBE_ATTEMPTS));
            if (!m_probePeers.contains(peer))
                noteProbeSeriesFailed(peer);
        }
    }

    serviceProbePeers();
    checkMeasurementExpiry();

    if (m_pendingProbes.isEmpty())
        return;

    // What's left are legacy UDP-anchor series: they retry as bursts over UDP.
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
                                QStringLiteral("peer=%1 transport=udp attempts=%2 age_ms=%3")
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
        QHostAddress addr;
        quint16 port = 0;
        if (parseEndpoint(it->endpoint, addr, port))
            sendProbeBurst(addr, port, it.key());
        if (!it->altEndpoint.isEmpty() && parseEndpoint(it->altEndpoint, addr, port))
            sendProbeBurst(addr, port, it.key());
        writePingDiagnostic(QStringLiteral("PROBE_RETRY"),
                            QStringLiteral("peer=%1 transport=udp nonce=%2 attempt=%3/%4")
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

static LobbyClient::LobbyMatchPeer parseMatchPeer(const QJsonObject& o)
{
    LobbyClient::LobbyMatchPeer p;
    p.userId     = static_cast<quint64>(o.value("userId").toDouble());
    p.username   = o.value("username").toString();
    p.publicIp   = o.value("publicIp").toString();
    p.publicPort = static_cast<quint16>(o.value("publicPort").toInt());
    p.localIp    = o.value("localIp").toString();
    p.slot       = o.value("slot").toInt();
    p.iceSupported = o.value("iceSupported").toBool();
    p.peerId     = static_cast<quint64>(o.value("peerId").toDouble());
    if (p.peerId == 0)
        p.peerId = p.userId;
    return p;
}

void LobbyClient::handleMatchBegin(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    QList<LobbyMatchPeer> peers;
    for (const auto& v : data.value("peers").toArray())
        peers.append(parseMatchPeer(v.toObject()));
    m_matchSeats = qBound(0, data.value("seats").toInt(), 4);
    // The lobby sets the live replay keyframe interval in its config. Older
    // lobbies don't send it; keep 30 s for them. Same bounds as the lobby.
    const int keyframeSec = data.value("keyframeIntervalSec").toInt(0);
    m_broadcastKeyframeIntervalMs = keyframeSec > 0 ? qint64(std::clamp(keyframeSec, 10, 600)) * 1000 : 30'000;
    // On demand, the lobby asks for a keyframe when a viewer joins and its
    // cached one is older than the interval, saving the upload while nobody
    // joins. Older lobbies don't send it; keep uploading on the interval.
    m_broadcastKeyframeOnDemand = data.value("keyframeOnDemand").toBool(false);
    emit matchBegin(matchId, peers);
}

void LobbyClient::handleMatchAdmitBegin(const QJsonObject& data)
{
    QList<LobbyMatchPeer> peers;
    for (const auto& v : data.value("peers").toArray())
        peers.append(parseMatchPeer(v.toObject()));
    const LobbyMatchPeer joiner = parseMatchPeer(data.value("joiner").toObject());
    QList<LobbyMatchPeer> joiners;
    for (const auto& v : data.value("joiners").toArray())
        joiners.append(parseMatchPeer(v.toObject()));
    if (joiners.isEmpty())
        joiners.append(joiner);
    m_admitBlobId = 0;
    m_admitBlobParts.clear();
    const quint64 admitId = static_cast<quint64>(data.value("admitId").toDouble());
    const quint64 hostId = static_cast<quint64>(data.value("hostId").toDouble());
    const bool joinerIsSelf = m_selfUserId != 0 && joiner.userId == m_selfUserId;
    m_admitJoinId = joinerIsSelf ? admitId : 0;
    m_admitJoinHostId = joinerIsSelf ? hostId : 0;
    emit matchAdmitBegin(static_cast<quint64>(data.value("matchId").toDouble()),
                         admitId,
                         data.value("epoch").toInt(),
                         qBound(0, data.value("seats").toInt(), 4),
                         hostId, joiner, joiners, peers);
}

void LobbyClient::handleMatchAdmitSignal(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const quint64 admitId = static_cast<quint64>(data.value("admitId").toDouble());
    const QString step = data.value("step").toString();
    const int count = data.value("count").toInt();
    if (count <= 0)
    {
        emit matchAdmitStep(matchId, admitId, static_cast<quint64>(data.value("fromUserId").toDouble()),
                            step, data.value("frame").toInt(), data.value("ok").toBool(),
                            data.value("reason").toString());
        return;
    }

    // A blob step arrives as `count` chunks, in order over the one socket.
    // Only the host of the join that is adding this client sends one, and
    // only the settings and the game state travel this way.
    const int index = data.value("index").toInt();
    if (count > MAX_RECEIVED_CHUNK_COUNT || index < 0 || index >= count)
        return;
    const bool isManifest = step == QStringLiteral("manifest");
    if (admitId == 0 || admitId != m_admitJoinId ||
        static_cast<quint64>(data.value("fromUserId").toDouble()) != m_admitJoinHostId ||
        (!isManifest && step != QStringLiteral("state")))
        return;
    const int maxRawBytes = isManifest ? MAX_RELAYED_MANIFEST_BYTES : MAX_KEYFRAME_RAW_BYTES;
    const int maxEncodedBytes = maxRawBytes + 6 + 4;
    if (index == 0 || m_admitBlobId != admitId || m_admitBlobStep != step ||
        m_admitBlobParts.size() != count)
    {
        m_admitBlobId = admitId;
        m_admitBlobStep = step;
        m_admitBlobParts.clear();
        m_admitBlobParts.resize(count);
    }
    QByteArray part = QByteArray::fromBase64(data.value("data").toString().toLatin1());
    qsizetype held = part.size();
    for (const QByteArray& other : std::as_const(m_admitBlobParts))
        held += other.size();
    if (part.size() > MAX_RECEIVED_CHUNK_BYTES || held > maxEncodedBytes)
    {
        // Too big to be a real blob; drop it, and the step times out.
        qWarning() << "Rollback lobby late join: dropped an oversized" << step;
        m_admitBlobParts.clear();
        m_admitBlobId = 0;
        return;
    }
    m_admitBlobParts[index] = std::move(part);
    for (const QByteArray& other : std::as_const(m_admitBlobParts))
    {
        if (other.isEmpty())
            return;
    }
    QByteArray encoded;
    for (const QByteArray& part : std::as_const(m_admitBlobParts))
        encoded.append(part);
    m_admitBlobParts.clear();
    m_admitBlobId = 0;

    QByteArray raw;
    if (!decodeSpectateKeyframe(encoded, raw, maxRawBytes))
    {
        qWarning() << "Rollback lobby late join: couldn't decode" << step;
        raw.clear();
    }
    emit matchAdmitBlob(matchId, admitId, step, raw);
}

void LobbyClient::handleMatchAdmitFail(const QJsonObject& data)
{
    emit matchAdmitFailed(static_cast<quint64>(data.value("matchId").toDouble()),
                          static_cast<quint64>(data.value("userId").toDouble()),
                          data.value("reason").toString());
}

void LobbyClient::sendMatchAdmit(quint64 matchId, quint64 userId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    d["userId"] = QJsonValue(qint64(userId));
    sendEnvelope("MATCH_ADMIT", d);
}

void LobbyClient::sendAdmitStep(quint64 matchId, quint64 admitId, const QString& step,
                                int frame, bool ok, const QString& reason)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    d["admitId"] = QJsonValue(qint64(admitId));
    d["step"] = step;
    if (frame != 0)
        d["frame"] = frame;
    if (ok)
        d["ok"] = true;
    if (!reason.isEmpty())
        d["reason"] = reason;
    sendEnvelope("MATCH_ADMIT_SIGNAL", d);
}

void LobbyClient::sendAdmitBlob(quint64 matchId, quint64 admitId, const QString& step, const QByteArray& raw)
{
    // Chunks stay well under the server's 1 MiB message limit once base64'd.
    constexpr int kChunkBytes = 384 * 1024;
    const QByteArray encoded = encodeSpectateKeyframe(raw);
    if (encoded.isEmpty())
        return;
    const int count = int((encoded.size() + kChunkBytes - 1) / kChunkBytes);
    for (int i = 0; i < count; i++)
    {
        QJsonObject d;
        d["matchId"] = QJsonValue(qint64(matchId));
        d["admitId"] = QJsonValue(qint64(admitId));
        d["step"] = step;
        d["index"] = i;
        d["count"] = count;
        d["data"] = QString::fromLatin1(encoded.mid(qsizetype(i) * kChunkBytes, kChunkBytes).toBase64());
        sendEnvelope("MATCH_ADMIT_SIGNAL", d);
    }
    qInfo() << "Rollback lobby late join: sent" << step << "raw" << raw.size()
            << "compressed" << encoded.size() << "chunks" << count;
}

void LobbyClient::ensureIcePeer(quint64 userId)
{
    if (userId == 0 || userId == m_selfUserId || m_icePeerIds.contains(userId) ||
        m_currentIceRoomId == 0)
        return;
    dropProbePeerForMesh(userId);
    if (!m_iceGenerations.contains(userId))
        m_iceGenerations.insert(userId, 0);
    if (!m_iceRestartAttempts.contains(userId))
        m_iceRestartAttempts.insert(userId, 0);
    if (!LobbyIce::add_peer(userId))
    {
        writePingDiagnostic(QStringLiteral("ICE_PEER_ADD_FAILED"),
                            QStringLiteral("peer=%1 room=%2 reason=mid_match")
                                .arg(pingUserLabel(userId)).arg(m_currentIceRoomId));
        return;
    }
    m_icePeerIds.insert(userId);
    emit icePeerConnectionAttemptChanged(
        userId, m_iceRestartAttempts.value(userId, 0) + 1, MAX_ICE_CONNECTION_ATTEMPTS);
    writePingDiagnostic(QStringLiteral("ICE_PEER_ADDED"),
                        QStringLiteral("peer=%1 room=%2 reason=mid_match")
                            .arg(pingUserLabel(userId)).arg(m_currentIceRoomId));
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
    const QString reason = data.value("reason").toString();
    if (!reason.isEmpty())
        emit quickMatchRefused(reason);
}

void LobbyClient::handleSpectateBegin(const QJsonObject& data)
{
    const QByteArray manifest = QByteArray::fromBase64(
        data.value("manifest").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    emit spectateBegan(static_cast<quint64>(data.value("matchId").toDouble()), manifest);
}

void LobbyClient::handleSpectateData(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const QByteArray raw = QByteArray::fromBase64(data.value("data").toString().toLatin1());
    const int liveFrame = data.value("frame").toInt();
    const qint64 offset = data.contains("offset")
        ? static_cast<qint64>(data.value("offset").toDouble())
        : -1;
    if (!raw.isEmpty())
        emit spectateData(matchId, raw, liveFrame, offset);
}

void LobbyClient::handleSpectateKeyframe(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const int frame      = data.value("frame").toInt();
    const int chunkIndex = data.value("chunkIndex").toInt();
    const int chunkCount = data.value("chunkCount").toInt();
    if (matchId == 0 || !m_spectateMatchIds.contains(matchId))
        return; // not a match being watched
    const QByteArray raw = QByteArray::fromBase64(data.value("data").toString().toLatin1());
    if (chunkCount <= 0 || chunkCount > MAX_RECEIVED_CHUNK_COUNT ||
        chunkIndex < 0 || chunkIndex >= chunkCount || raw.size() > MAX_RECEIVED_CHUNK_BYTES)
        return;

    // (Re)start reassembly when a new keyframe appears. Chunks arrive in order over
    // the WS, but track per-index seen flags so a stray/duplicate can't corrupt it.
    KeyframeAssembly& kf = m_kfRecv[matchId];
    if (kf.frame != frame || kf.count != chunkCount || kf.chunkSeen.size() != chunkCount)
    {
        kf = KeyframeAssembly{};
        kf.frame = frame;
        kf.count = chunkCount;
        for (int i = 0; i < chunkCount; i++)
            kf.chunkSeen.append(false);
    }
    if (!kf.chunkSeen[chunkIndex])
    {
        if (kf.buf.size() + raw.size() > MAX_KEYFRAME_ENCODED_BYTES)
        {
            qWarning() << "lobby: rejected oversized live replay keyframe";
            m_kfRecv.remove(matchId);
            stopSpectate(matchId);
            emit spectateFailed(matchId, QStringLiteral("invalid_keyframe"));
            return;
        }
        kf.chunkSeen[chunkIndex] = true;
        kf.buf.append(raw); // in-order append (server sends chunks sequentially)
        kf.got++;
    }
    if (kf.got < kf.count)
        return;

    const QByteArray encoded = kf.buf;
    const int doneFrame = kf.frame;
    m_kfRecv.remove(matchId);

    QByteArray full;
    if (!decodeSpectateKeyframe(encoded, full))
    {
        qWarning() << "lobby: rejected invalid or oversized live replay keyframe";
        stopSpectate(matchId);
        emit spectateFailed(matchId, QStringLiteral("invalid_keyframe"));
        return;
    }
    if (encoded.startsWith(KEYFRAME_ZLIB_MAGIC))
    {
        qInfo() << "Live replay keyframe decompressed:" << encoded.size()
                << "bytes to" << full.size() << "bytes";
    }
    emit spectateKeyframe(matchId, doneFrame, full);
}

void LobbyClient::handleSpectateEnd(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    m_spectateMatchIds.remove(matchId);
    m_kfRecv.remove(matchId);
    emit spectateEnded(matchId, data.value("reason").toString());
}

void LobbyClient::handleSpectateFail(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    m_spectateMatchIds.remove(matchId);
    m_kfRecv.remove(matchId);
    emit spectateFailed(matchId, data.value("reason").toString());
}

void LobbyClient::handleBroadcastViewerCount(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    const int viewerCount = qMax(0, data.value("viewerCount").toInt());
    if (matchId == 0) return;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
    {
        if (it->matchId == matchId)
            it->viewerCount = viewerCount;
    }
    emit broadcastViewerCount(matchId, viewerCount);
}

void LobbyClient::handleBroadcastKeyframeRequest(const QJsonObject& data)
{
    const quint64 matchId = static_cast<quint64>(data.value("matchId").toDouble());
    if (matchId != 0)
        emit broadcastKeyframeRequested(matchId);
}

// -------- Heartbeat --------

void LobbyClient::onHeartbeatTimer()
{
    if (m_state != ConnectionState::Connected)
        return;
    QJsonObject data;
    if (m_serverRttMs > 0)
        data["pingToServer"] = std::clamp(m_serverRttMs, 1, 65535);
    m_reportedAway = (QDateTime::currentMSecsSinceEpoch() - m_lastActivityMs) >= AWAY_AFTER_MS;
    data["away"] = m_reportedAway;

    // The lobby echoes the envelope id in its ACK. A heartbeat queued behind
    // an upload would time the upload, so only one sent on an idle socket is
    // timed.
    const qint64 nowMs = monotonicMs();
    for (auto it = m_heartbeatSentMs.begin(); it != m_heartbeatSentMs.end();)
    {
        if (nowMs - it.value() > HEARTBEAT_ACK_TIMEOUT_MS)
            it = m_heartbeatSentMs.erase(it);
        else
            ++it;
    }
    const QString id = QStringLiteral("hb-%1").arg(++m_heartbeatSeq);
    const bool timed = m_ws->bytesToWrite() == 0;
    if (sendEnvelope("HEARTBEAT", data, id) > 0 && timed)
        m_heartbeatSentMs.insert(id, nowMs);
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
    m_prematchManifest.clear();
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
        if (!buildPrematchManifest(romFile, m_prematchSaveData ? &*m_prematchSaveData : nullptr,
                                   m_prematchGameOptions, manifest, manifestHash, cheatCount))
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
        m_prematchManifest = QByteArray::fromStdString(manifest);
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
                    totalBytes == 0 || totalBytes > 2 * 1024 * 1024 ||
                    datagram.size() - 29 > PREMATCH_FRAGMENT_PAYLOAD_BYTES)
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
        if (!buildPrematchManifest(romFile, nullptr, {}, manifest, manifestHash, cheatCount))
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
            m_measuredPing.insert(uid, MeasuredPing{rttMs, monotonicMs(), false, false});
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
    // A seat's ping is its connection's.
    const quint64 peerId = m_seatPeers.value(userId, userId);
    const auto it = m_measuredPing.constFind(peerId);
    if (it == m_measuredPing.constEnd())
        return -1;
    // A number from a room left long ago says little about the path now.
    if (!LobbyPing::measurementCurrent(it->atMs, monotonicMs(), LobbyIce::peer_connected(peerId)))
        return -1;
    return it->ms;
}

int LobbyClient::pairPingMs(quint64 first, quint64 second) const
{
    first = m_seatPeers.value(first, first);
    second = m_seatPeers.value(second, second);
    if (first == second)
        return 0;
    if (first == m_selfUserId || second == m_selfUserId)
        return measuredPingMs(first == m_selfUserId ? second : first);
    // Reported by the lower id of the two.
    return m_roomPeerPings.value(std::min(first, second)).value(std::max(first, second), -1);
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
        if (peer.peerId == m_selfUserId)
            continue;
        if (!peer.iceSupported)
        {
            if (error)
                *error = QCoreApplication::translate("LobbyClient","%1 is using a lobby client without ICE support.")
                             .arg(peer.username.isEmpty() ? QString::number(peer.userId) : peer.username);
            return false;
        }
        // A ping check's agent is no connection for a match.
        if (!LobbyIce::peer_connected(peer.peerId) || m_probePeers.contains(peer.peerId))
        {
            if (error)
                *error = QCoreApplication::translate("LobbyClient","Waiting for a direct ICE connection to %1.")
                             .arg(peer.username.isEmpty() ? QString::number(peer.userId) : peer.username);
            return false;
        }
    }
    if (error)
        error->clear();
    return true;
}

// -------- Chat API --------

void LobbyClient::sendChat(const QString& channel, const QString& message, quint64 matchId)
{
    QJsonObject d;
    d["channel"] = channel;
    d["message"] = message;
    if (matchId != 0)
        d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("CHAT_SEND", d);
}

void LobbyClient::requestChatHistory(const QString& channel)
{
    QJsonObject d;
    d["channel"] = channel;
    sendEnvelope("CHAT_HISTORY", d);
}

void LobbyClient::sendPrivateMessage(quint64 toUserId, quint64 asUserId, const QString& message)
{
    QJsonObject d;
    d["channel"] = QStringLiteral("pm");
    d["message"] = message;
    d["toUserId"] = static_cast<double>(toUserId);
    if (asUserId != 0 && asUserId != m_selfUserId)
        d["asUserId"] = static_cast<double>(asUserId);
    sendEnvelope("CHAT_SEND", d);
}

void LobbyClient::handleChatFail(const QJsonObject& data)
{
    emit privateMessageFailed(static_cast<quint64>(data.value("toUserId").toDouble()),
                              data.value("reason").toString());
}

// -------- Challenges --------

void LobbyClient::sendChallenge(quint64 toUserId, const QString& romName, const QString& romMd5,
                                const QString& note, bool liveReplay)
{
    QJsonObject rom;
    rom["name"] = romName;
    rom["md5"]  = romMd5;
    QJsonObject d;
    d["toUserId"] = static_cast<double>(toUserId);
    d["rom"] = rom;
    if (!note.isEmpty())
        d["note"] = note;
    if (liveReplay)
        d["liveReplay"] = true;
    sendEnvelope("CHALLENGE_SEND", d);
}

void LobbyClient::cancelChallenge(quint64 challengeId)
{
    QJsonObject d;
    d["challengeId"] = static_cast<double>(challengeId);
    sendEnvelope("CHALLENGE_CANCEL", d);
}

void LobbyClient::respondToChallenge(quint64 challengeId, bool accept)
{
    QJsonObject d;
    d["challengeId"] = static_cast<double>(challengeId);
    d["accept"] = accept;
    sendEnvelope("CHALLENGE_RESPOND", d);
}

void LobbyClient::requestSharedGames(quint64 toUserId)
{
    QJsonObject d;
    d["toUserId"] = static_cast<double>(toUserId);
    sendEnvelope("CHALLENGE_GAMES", d);
}

void LobbyClient::setChallengesOff(bool off)
{
    if (m_challengesOff == off)
        return;
    m_challengesOff = off;
    if (m_state != ConnectionState::Connected || !m_challengeSettingsSupported)
        return;
    QJsonObject d;
    d["off"] = off;
    sendEnvelope("CHALLENGE_SETTINGS", d);
}

void LobbyClient::setPrivateMessagesOff(bool off)
{
    if (m_privateMessagesOff == off)
        return;
    m_privateMessagesOff = off;
    if (m_state != ConnectionState::Connected || !m_pmSettingsSupported)
        return;
    QJsonObject d;
    d["off"] = off;
    sendEnvelope("PM_SETTINGS", d);
}

void LobbyClient::handleChallengeSent(const QJsonObject& data)
{
    const QJsonObject rom = data.value("rom").toObject();
    Challenge c;
    c.id = static_cast<quint64>(data.value("challengeId").toDouble());
    c.fromUserId = m_selfUserId;
    c.toUserId = static_cast<quint64>(data.value("toUserId").toDouble());
    c.toUsername = data.value("toUsername").toString();
    c.romName = rom.value("name").toString();
    c.romMd5 = rom.value("md5").toString();
    const qint64 expiresInMs = static_cast<qint64>(data.value("expiresInMs").toDouble());
    c.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + expiresInMs;
    c.invite = data.value("invite").toBool();
    c.pingPeerUserId = static_cast<quint64>(data.value("pingPeerUserId").toDouble());
    // Measure them while they make up their mind.
    if (c.pingPeerUserId != 0)
    {
        startProbePeer(c.pingPeerUserId, c.pingPeerUserId,
                       expiresInMs + PROBE_PEER_GRACE_MS + PROBE_PEER_SLACK_MS, c.id, true,
                       QStringLiteral("challenge_sent"));
    }
    emit challengeSent(c);
}

void LobbyClient::handleChallengeIncoming(const QJsonObject& data)
{
    const QJsonObject rom = data.value("rom").toObject();
    Challenge c;
    c.id = static_cast<quint64>(data.value("challengeId").toDouble());
    c.fromUserId = static_cast<quint64>(data.value("fromUserId").toDouble());
    c.fromUsername = data.value("fromUsername").toString();
    c.toUserId = static_cast<quint64>(data.value("toUserId").toDouble());
    c.toUsername = data.value("toUsername").toString();
    c.romName = rom.value("name").toString();
    c.romMd5 = rom.value("md5").toString();
    c.note = data.value("note").toString();
    c.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(data.value("expiresInMs").toDouble());
    c.invite = data.value("invite").toBool();
    c.roomName = data.value("roomName").toString();
    c.roomPlayers = data.value("roomPlayers").toInt();
    c.roomMaxPlayers = data.value("roomMaxPlayers").toInt();
    c.pingPeerUserId = static_cast<quint64>(data.value("pingPeerUserId").toDouble());
    emit challengeIncoming(c);
}

void LobbyClient::measureChallengePing(const Challenge& challenge)
{
    if (challenge.pingPeerUserId == 0)
        return;
    const qint64 expiresInMs = challenge.expiresAtMs - QDateTime::currentMSecsSinceEpoch();
    startProbePeer(challenge.pingPeerUserId, challenge.pingPeerUserId,
                   expiresInMs + PROBE_PEER_GRACE_MS + PROBE_PEER_SLACK_MS, challenge.id, true,
                   QStringLiteral("challenge_incoming"));
}

void LobbyClient::handleChallengeResult(const QJsonObject& data)
{
    const quint64 challengeId = static_cast<quint64>(data.value("challengeId").toDouble());
    // The lobby keeps the pair's grant a few seconds past the end, so a last
    // sample can land; then PING_CHECK_END, or this, closes the agent.
    if (challengeId != 0)
    {
        const qint64 endsAtMs = monotonicMs() + PROBE_PEER_GRACE_MS;
        for (auto it = m_probePeers.begin(); it != m_probePeers.end(); ++it)
        {
            if (it->challengeIds.remove(challengeId) && it->challengeIds.isEmpty())
                it->expiresAtMs = std::min(it->expiresAtMs, endsAtMs);
        }
    }
    emit challengeEnded(challengeId,
                        static_cast<quint64>(data.value("toUserId").toDouble()),
                        data.value("result").toString());
}

void LobbyClient::handleChallengeGamesReply(const QJsonObject& data)
{
    QStringList md5s;
    for (const QJsonValue& value : data.value("md5s").toArray())
        md5s << value.toString();
    emit sharedGamesReceived(static_cast<quint64>(data.value("toUserId").toDouble()), md5s);
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

void LobbyClient::sendAdminLogout()
{
    sendEnvelope("ADMIN_LOGOUT");
}

void LobbyClient::handleAdminLoggedOut()
{
    m_isModerator = false;
    m_moderatorUsers.clear();
    emit adminLoggedOut();
}

void LobbyClient::handleModResult(const QJsonObject& data)
{
    emit modResult(data.value("action").toString(), data.value("target").toString(),
                   data.value("ip").toString(), static_cast<qint64>(data.value("until").toDouble()));
}

void LobbyClient::handleModTargetInfo(const QJsonObject& data)
{
    QStringList others;
    for (const QJsonValue& value : data.value("others").toArray())
        others << value.toString();
    emit modTargetInfo(data.value("target").toString(), others, data.value("moderator").toBool());
}

void LobbyClient::handleModPeers(const QJsonObject& data)
{
    m_moderatorUsers.clear();
    for (const QJsonValue& value : data.value("userIds").toArray())
        m_moderatorUsers.insert(static_cast<quint64>(value.toDouble()));
    emit moderatorsChanged();
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
                              int pacing, const QString& password, int maxPing, bool wiredOnly)
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
    if (maxPing > 0)
        d["maxPing"] = maxPing;
    if (wiredOnly)
        d["wiredOnly"] = true;

    sendEnvelope("ROOM_CREATE", d);
}

void LobbyClient::joinRoom(quint64 roomId, const QString& password, int pingMs, bool pingMeasured)
{
    QJsonObject d;
    d["roomId"] = QJsonValue(qint64(roomId));
    if (!password.isEmpty())
        d["password"] = password;
    // A lobby with ping checks reads pingMeasured and lets an estimate through;
    // one without compares whatever comes, so it only hears a measurement.
    // A measured 0 ms (same PC or LAN) is still a measurement, not "unknown".
    const bool measured = pingMeasured && pingMs >= 0;
    if (measured || (pingMs > 0 && m_pingCheckSupported))
        d["pingMs"] = std::max(0, pingMs);
    if (m_pingCheckSupported)
        d["pingMeasured"] = measured;
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

void LobbyClient::updateRoomLiveReplay(bool enabled)
{
    QJsonObject d;
    d["enabled"] = enabled;
    sendEnvelope("ROOM_UPDATE_LIVE_REPLAY", d);
}

void LobbyClient::updateRoomLateJoin(bool enabled)
{
    QJsonObject d;
    d["enabled"] = enabled;
    sendEnvelope("ROOM_UPDATE_LATE_JOIN", d);
}

void LobbyClient::updateRoomJoinLimits(int maxPing, bool wiredOnly)
{
    QJsonObject d;
    d["maxPing"] = maxPing;
    d["wiredOnly"] = wiredOnly;
    sendEnvelope("ROOM_UPDATE_JOIN_LIMITS", d);
}

void LobbyClient::updateRoomMaxPlayers(int maxPlayers)
{
    QJsonObject d;
    d["maxPlayers"] = maxPlayers;
    sendEnvelope("ROOM_UPDATE_MAX_PLAYERS", d);
}

void LobbyClient::updateRoomSaveData(bool enabled, const QString& label, const QStringList& kinds,
                                     int bytes, const QString& hash)
{
    QJsonObject d;
    d["enabled"] = enabled;
    if (enabled)
    {
        d["label"] = label;
        d["kinds"] = QJsonArray::fromStringList(kinds);
        d["bytes"] = bytes;
        d["hash"] = hash;
    }
    sendEnvelope("ROOM_UPDATE_SAVE_DATA", d);
}

void LobbyClient::updateRoomGameOptions(const QList<LobbyGameOption>& options, const LobbyGameOption& preset)
{
    QJsonArray list;
    for (const LobbyGameOption& option : options)
    {
        QJsonObject o;
        o["id"] = option.id;
        o["label"] = option.label;
        list.append(o);
    }
    QJsonObject d;
    d["options"] = list;
    // Servers from before presets ignore it; the room just shows the options.
    if (!preset.id.isEmpty() && !options.isEmpty())
    {
        QJsonObject p;
        p["id"] = preset.id;
        p["label"] = preset.label;
        d["preset"] = p;
    }
    sendEnvelope("ROOM_UPDATE_GAME_OPTIONS", d);
}

QList<LobbyClient::LobbyGameOption> LobbyClient::parseGameOptions(const QJsonValue& value)
{
    QList<LobbyGameOption> options;
    for (const auto& v : value.toArray())
    {
        const QJsonObject o = v.toObject();
        LobbyGameOption option{ o.value("id").toString(), o.value("label").toString() };
        if (!option.id.isEmpty())
            options.append(option);
    }
    return options;
}

LobbyClient::LobbyGameOption LobbyClient::parseGameOptionsPreset(const QJsonValue& value)
{
    const QJsonObject o = value.toObject();
    LobbyGameOption preset{ o.value("id").toString(), o.value("label").toString() };
    if (preset.id.isEmpty() || preset.label.isEmpty())
        return {};
    return preset;
}

void LobbyClient::updateLocalFrameDelay(int delay, bool delayAuto)
{
    QJsonObject d;
    d["delay"] = delay;
    d["delayAuto"] = delayAuto;
    sendEnvelope("ROOM_UPDATE_DELAY", d);
}

void LobbyClient::updateRoomPrediction(int prediction, bool predictionAuto)
{
    QJsonObject d;
    d["prediction"] = prediction;
    d["predictionAuto"] = predictionAuto;
    sendEnvelope("ROOM_UPDATE_PREDICTION", d);
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
    // A seat is measured through its connection.
    targetUserId = m_seatPeers.value(targetUserId, targetUserId);
    if (targetUserId == 0 || targetUserId == m_selfUserId)
        return;
    startIcePingSeries(targetUserId);
}

bool LobbyClient::startIcePingSeries(quint64 peerId)
{
    if (peerId == 0 || !LobbyIce::peer_connected(peerId) || m_iceProbes.inFlight(peerId))
        return false;
    // start() refuses only a zero or taken nonce now; draw another.
    quint64 nonce = 0;
    const qint64 nowMs = monotonicMs();
    do
        nonce = QRandomGenerator::global()->generate64();
    while (!m_iceProbes.start(peerId, nonce, nowMs));
    sendIcePing(peerId, nonce);
    writePingDiagnostic(QStringLiteral("PROBE_REQUEST"),
                        QStringLiteral("peer=%1 transport=ice nonce=%2")
                            .arg(pingUserLabel(peerId)).arg(nonce));
    return true;
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

bool LobbyClient::sendBroadcastBegin(quint64 matchId)
{
    // Leave room for base64 and the JSON envelope under the server's 1 MiB limit.
    if (m_prematchManifest.isEmpty() || m_prematchManifest.size() > 512 * 1024)
        return false;
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    d["manifest"] = QString::fromLatin1(m_prematchManifest.toBase64());
    // Say we'll upload keyframes when asked. Without it the lobby expects them
    // on the interval and never asks, as for an older client.
    if (m_broadcastKeyframeOnDemand)
        d["keyframeOnDemand"] = true;
    sendEnvelope("BROADCAST_BEGIN", d);
    return true;
}

bool LobbyClient::applySpectateManifest(const QByteArray& manifest)
{
    uint64_t manifestHash = 0;
    size_t cheatCount = 0;
    if (manifest.isEmpty() || manifest.size() > MAX_RELAYED_MANIFEST_BYTES ||
        !applyPrematchManifest(manifest.toStdString(), manifestHash, cheatCount))
        return false;
    qInfo() << "Rollback lobby spectator prematch complete"
            << "hash" << static_cast<qulonglong>(manifestHash)
            << "cheats" << static_cast<qulonglong>(cheatCount);
    return true;
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

bool LobbyClient::sendBroadcastKeyframe(quint64 matchId, const QByteArray& savestate, int frame)
{
    if (matchId == 0 || frame < 0 || savestate.isEmpty() ||
        savestate.size() > MAX_KEYFRAME_RAW_BYTES || broadcastKeyframeUploadBusy() ||
        m_ws->state() != QAbstractSocket::ConnectedState)
        return false;

    m_kfCompressing = true;
    m_kfCompressMatchId = matchId;
    m_kfCompressFrame = frame;
    m_kfCompressRawBytes = savestate.size();
    const QByteArray raw = savestate; // implicitly shared; detached only inside qCompress
    m_kfCompressWatcher->setFuture(QtConcurrent::run([raw]() {
        return encodeSpectateKeyframe(raw);
    }));
    return true;
}

bool LobbyClient::broadcastKeyframeUploadBusy() const
{
    return m_kfCompressing || m_kfUploadActive;
}

void LobbyClient::sendBroadcastEnd(quint64 matchId)
{
    resetBroadcastKeyframeUpload(matchId);
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    sendEnvelope("BROADCAST_END", d);
}

void LobbyClient::startSpectate(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    m_spectateMatchIds.insert(matchId);
    // A fresh subscribe starts its keyframe over.
    m_kfRecv.remove(matchId);
    sendEnvelope("SPECTATE_START", d);
}

void LobbyClient::stopSpectate(quint64 matchId)
{
    QJsonObject d;
    d["matchId"] = QJsonValue(qint64(matchId));
    m_spectateMatchIds.remove(matchId);
    m_kfRecv.remove(matchId);
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
    u.social = obj.value("social").toBool();
    u.noChallenges = obj.value("noChallenges").toBool();
    u.noPrivateMessages = obj.value("noPrivateMessages").toBool();
    return u;
}

#endif // NETPLAY
