#include "LobbyButtonHints.hpp"
#include "LobbyIce.hpp"
#include "Plugins.hpp"
#include "RomHeader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

namespace
{
constexpr unsigned int axisX = 1, axisY = 2;
constexpr unsigned int smashStickCapability = 1;
constexpr unsigned int maxRevisions = 8;

bool isSmashRom()
{
    CoreRomHeader rom{};
    if (!CoreGetCurrentRomHeader(rom)) return false;
    if (rom.GameID == "NALJ")
        return rom.CRC1 == 0x67D20729 && rom.CRC2 == 0xF696774C;
    if (rom.GameID != "NALE") return false;
    rom.Name.erase(rom.Name.find_last_not_of(' ') + 1);
    return rom.Name == "SMASH REMIX" || (rom.CRC1 == 0x916B8B5B && rom.CRC2 == 0x780B85A4);
}

int stickValue(std::uint32_t pad, int shift)
{
    return static_cast<std::int8_t>(pad >> shift);
}

// ftCommonDashCheckInterruptCommon, ftCommonKneeBendGetInputTypeRun/Common,
// and ftPhysicsCheckSetFastFall.
// These are stick ranges. Smash evaluates its tap timers and fighter state.
int dashRange(int x) { return (x >= 56) - (x <= -56); }
constexpr int verticalStickRange(int y)
{
    if (y <= -53) return -1; // Fastfall
    return (y >= 45) + (y >= 53); // Running jump, then normal stick jump
}

struct StickAxis
{
    int previous, reported;
    bool crossed = false, mayRefine = false;

    bool observe(int value, int (*range)(int))
    {
        const bool crossing = range(value) != range(previous);
        // A fast flick can cross the threshold before it reaches its final
        // position. Allow one settled reading after each crossing.
        const bool settled = mayRefine && previous == value && reported != value;
        previous = value;
        if (!crossing && !settled) return false;
        crossed = true;
        mayRefine = crossing;
        return true;
    }
};

struct Destination
{
    std::uint64_t match = 0;
    std::uint64_t peer = 0;
    unsigned int epoch = 0;
};

Destination session;
RaphnetArmHint armPlugin = nullptr;
RaphnetArmPadSamples armPadSamples = nullptr;
bool peerSupportsHints = false;
bool localSmash = false, peerSmash = false;
std::chrono::steady_clock::time_point nextHello;
struct Hint
{
    unsigned int revision = 0;
    std::uint32_t pad = 0;
    unsigned int axes = 0;
};
std::map<int, Hint> received;
// Score the first prediction for each frame against the real input when it
// arrives. A repair can revisit a frame before confirmation. Keep the first
// choice so a replay cannot retroactively improve the score.
struct Prediction
{
    int frame = -1;
    std::uint32_t previous = 0;
    std::uint32_t predicted = 0;
};
std::array<Prediction, 32> predictions;
std::uint32_t latestActual = 0;
unsigned int helped = 0, hurt = 0, bothWrong = 0, packetsReceived = 0;
unsigned int stickRangeHelped = 0, stickRangeHurt = 0;

// USB reads must keep their cadence even when ICE is busy. Coalesce unsent
// hints and let a separate sender take any transport locks.
std::mutex outgoingMutex;
std::uint64_t ticket = 0;
Destination outgoing;
int targetFrame = 0;
// The official sample starts each ticket. Only the adapter callback updates it
// afterwards, under outgoingMutex. Each revision stands on its own after loss.
StickAxis xAxis{}, yAxis{};
Hint sampled;
struct PendingHint
{
    Destination dest;
    int frame = 0;
    Hint hint;
};
PendingHint pending;
std::condition_variable_any ready;
std::jthread sender;

void put(unsigned char* dest, std::uint64_t value, int bytes)
{
    while (bytes > 0) {
        dest[--bytes] = static_cast<unsigned char>(value);
        value >>= 8;
    }
}

std::uint64_t get(const unsigned char* src, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value = (value << 8) | src[i];
    return value;
}

void send(const Destination& dest, int frame, const Hint& hint)
{
    std::array<unsigned char, kLobbyStickHintSize> wire{};
    put(wire.data(), dest.match, 8);
    put(wire.data() + 8, dest.epoch, 4);
    put(wire.data() + 12, static_cast<unsigned int>(frame), 4);
    wire[16] = static_cast<unsigned char>(hint.revision);
    put(wire.data() + 17, hint.pad >> 16, 2);
    if (hint.axes) {
        wire[19] = static_cast<unsigned char>(hint.axes);
        put(wire.data() + 20, hint.pad, 2);
    }
    LobbyIce::send(dest.peer, LobbyIceChannel::ButtonHint,
        reinterpret_cast<const char*>(wire.data()), hint.axes ? wire.size() : kLobbyButtonHintSize);
}

void publish(std::uint64_t sampleTicket, unsigned int buttons, unsigned int revision)
{
    std::unique_lock lock(outgoingMutex, std::try_to_lock);
    if (!lock || ticket != sampleTicket || !outgoing.peer) return;
    pending = {outgoing, targetFrame, {revision, buttons << 16, 0}};
    lock.unlock();
    ready.notify_one();
}

void onPadSample(std::uint64_t sampleTicket, std::uint32_t pad)
{
    std::unique_lock lock(outgoingMutex, std::try_to_lock);
    if (!lock || ticket != sampleTicket || !outgoing.peer || sampled.revision >= maxRevisions) return;
    const bool buttonsChanged = (sampled.pad >> 16) != (pad >> 16);
    const int x = stickValue(pad, 8), y = stickValue(pad, 0);
    const bool xChanged = xAxis.observe(x, dashRange);
    const bool yChanged = yAxis.observe(y, verticalStickRange);
    if (!buttonsChanged && !xChanged && !yChanged) return;
    // Reserve the last two revisions for digital input on a noisy stick.
    if (!buttonsChanged && sampled.revision >= maxRevisions - 2) return;
    xAxis.reported = x;
    yAxis.reported = y;
    sampled.pad = pad;
    sampled.axes = (xAxis.crossed ? axisX : 0) | (yAxis.crossed ? axisY : 0);
    ++sampled.revision;
    pending = {outgoing, targetFrame, sampled};
    lock.unlock();
    ready.notify_one();
}

void dispatch(std::stop_token stop)
{
    std::unique_lock lock(outgoingMutex);
    while (ready.wait(lock, stop, [] { return pending.dest.peer != 0; })) {
        if (stop.stop_requested()) return;
        const auto hint = pending;
        pending = {};
        lock.unlock();
        send(hint.dest, hint.frame, hint.hint);
        lock.lock();
    }
}
}

void LobbyButtonHints::reset(std::uint64_t match, int epoch, std::uint64_t peer)
{
    sender = {}; // request stop, wake the wait, and join before replacing the session
    const char* enabled = std::getenv("RMGK_RAPHNET_HINTS");
    session = {match, match && !(enabled && std::strcmp(enabled, "0") == 0) ? peer : 0,
               static_cast<unsigned int>(epoch)};
    armPlugin = session.peer ? CoreGetRaphnetArmHint() : nullptr;
    const char* stickEnabled = std::getenv("RMGK_SMASH_STICK_HINTS");
    localSmash = session.peer && !(stickEnabled && std::strcmp(stickEnabled, "0") == 0) && isSmashRom();
    armPadSamples = localSmash ? CoreGetRaphnetArmPadSamples() : nullptr;
    peerSmash = false;
    peerSupportsHints = false;
    nextHello = {};
    received.clear();
    predictions.fill(Prediction{});
    latestActual = 0;
    helped = hurt = bothWrong = packetsReceived = 0;
    stickRangeHelped = stickRangeHurt = 0;
    std::lock_guard<std::mutex> lock(outgoingMutex);
    ++ticket;
    outgoing = {};
    pending = {};
}

void LobbyButtonHints::receive(int currentFrame)
{
    if (!session.peer) return;
    // Keep advertising even if our peer's first hello arrived but ours was lost.
    // Older clients discard this ICE channel and never cause extra USB reads.
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextHello) {
        // Old peers ignore the buttons field in a hello and keep using 19 bytes.
        send(session, 0, {0, localSmash ? smashStickCapability << 16 : 0, 0});
        nextHello = now + std::chrono::seconds(2);
    }
    constexpr int history = 32; // room delay and prediction are each capped at ten
    const int oldest = std::max(1, currentFrame - history);
    received.erase(received.begin(), received.lower_bound(oldest));
    for (const auto& packet : LobbyIce::take_packets(LobbyIceChannel::ButtonHint)) {
        const bool extended = packet.data.size() == kLobbyStickHintSize;
        if (packet.peerUserId != session.peer || (!extended && packet.data.size() != kLobbyButtonHintSize))
            continue;
        const auto* data = reinterpret_cast<const unsigned char*>(packet.data.data());
        if (get(data, 8) != session.match || get(data + 8, 4) != session.epoch) continue;
        const auto frame = get(data + 12, 4);
        const unsigned int revision = data[16];
        const auto buttons = static_cast<std::uint32_t>(get(data + 17, 2));
        if (!frame && !revision && !extended) {
            peerSmash = (buttons & smashStickCapability) != 0;
            if (!peerSupportsHints && (armPlugin || armPadSamples)) {
                try {
                    sender = std::jthread(dispatch);
                } catch (const std::system_error&) {
                    // Receiving still works if a sender thread cannot be started.
                    armPlugin = nullptr;
                    armPadSamples = nullptr;
                }
            }
            peerSupportsHints = true;
            continue;
        }
        if (revision == 0 || revision > maxRevisions || frame < static_cast<unsigned int>(oldest) ||
            frame > static_cast<std::uint64_t>(currentFrame) + history) continue;
        if (extended && (!localSmash || !peerSmash || !data[19] || data[19] > (axisX | axisY)))
            continue;
        auto& hint = received[static_cast<int>(frame)];
        if (revision > hint.revision) {
            const auto stick = extended ? static_cast<std::uint32_t>(get(data + 20, 2)) : 0;
            hint = {revision, (buttons << 16) | stick, extended ? static_cast<unsigned int>(data[19]) : 0};
            ++packetsReceived;
        }
    }
}

void LobbyButtonHints::arm(int currentFrame, int delay, std::uint32_t input, bool liveInput)
{
    const bool useStick = peerSmash && armPadSamples;
    if ((!armPlugin && !useStick) || !peerSupportsHints) return;
    std::uint64_t sampleTicket;
    {
        std::lock_guard<std::mutex> lock(outgoingMutex);
        sampleTicket = ++ticket;
        outgoing = session;
        targetFrame = currentFrame + 1 + delay;
        if (useStick) {
            const int x = stickValue(input, 8), y = stickValue(input, 0);
            xAxis = {x, x};
            yAxis = {y, y};
            sampled = {0, input, 0};
        }
    }
    if (liveInput && currentFrame > 0) {
        if (useStick) armPadSamples(sampleTicket, onPadSample);
        else armPlugin(sampleTicket, input >> 16, publish);
    }
}

void LobbyButtonHints::predict(int frame, int lastReal, unsigned char* input)
{
    if (!peerSupportsHints) return;
    std::uint32_t pad;
    std::memcpy(&pad, input, sizeof(pad));
    // A later button-only hint can follow a dash hint while both official
    // frames are still in flight. Apply each observed axis in frame order.
    for (auto it = received.upper_bound(lastReal); it != received.end() && it->first <= frame; ++it) {
        const auto& hint = it->second;
        const std::uint32_t mask = 0xffff0000u | ((hint.axes & axisX) ? 0xff00u : 0) |
                                  ((hint.axes & axisY) ? 0xffu : 0);
        pad = (pad & ~mask) | (hint.pad & mask);
    }
    std::memcpy(input, &pad, sizeof(pad));
    auto& first = predictions[static_cast<unsigned int>(frame) % predictions.size()];
    if (first.frame != frame)
        first = {frame, latestActual, pad};
}

void LobbyButtonHints::confirm(int frame, const unsigned char* actual)
{
    std::memcpy(&latestActual, actual, sizeof(latestActual));
    const auto& first = predictions[static_cast<unsigned int>(frame) % predictions.size()];
    if (first.frame != frame || first.previous == first.predicted) return;
    if (localSmash) {
        const auto rangeMatches = [](std::uint32_t pad) {
            return dashRange(stickValue(pad, 8)) == dashRange(stickValue(latestActual, 8)) &&
                   verticalStickRange(stickValue(pad, 0)) == verticalStickRange(stickValue(latestActual, 0));
        };
        const bool previousMatch = rangeMatches(first.previous), predictedMatch = rangeMatches(first.predicted);
        if (predictedMatch && !previousMatch) ++stickRangeHelped;
        if (previousMatch && !predictedMatch) ++stickRangeHurt;
    }
    if (first.predicted == latestActual) ++helped;
    else if (first.previous == latestActual) ++hurt;
    else ++bothWrong;
}

std::string LobbyButtonHints::summary()
{
    std::ostringstream out;
    out << "button_hints peer_supported=" << peerSupportsHints
        << " received=" << packetsReceived << " helped_frames=" << helped
        << " hurt_frames=" << hurt << " both_wrong_frames=" << bothWrong
        << " smash_stick_negotiated=" << (localSmash && peerSmash)
        << " stick_range_helped_frames=" << stickRangeHelped << " stick_range_hurt_frames=" << stickRangeHurt;
    return out.str();
}
