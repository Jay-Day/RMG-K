#ifndef LOBBYPINGTRACKER_HPP
#define LOBBYPINGTRACKER_HPP

// The bookkeeping behind the lobby's ping numbers, kept free of Qt so it can
// be tested on its own: ping series over the ICE ping channel, how long a
// measurement stays a measurement, and the smoothing of the lobby round trip.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace LobbyPing
{

// How long a measurement still counts as one once its peer has no live ICE
// connection. Past this the UI falls back to the region estimate.
inline constexpr std::int64_t kMeasurementTtlMs = 2 * 60'000;

inline bool measurementCurrent(std::int64_t sampleAtMs, std::int64_t nowMs, bool connected)
{
    return connected || nowMs - sampleAtMs <= kMeasurementTtlMs;
}

// Light smoothing: the first sample as it is, then each new one counts a quarter.
inline int smoothRtt(int previousMs, int sampleMs)
{
    if (previousMs <= 0)
        return sampleMs;
    return (previousMs * 3 + sampleMs + 2) / 4;
}

// Ping series to peers, one at a time per peer. Every attempt goes out under a
// nonce of its own, so a reply is timed against the packet it answers: a slow
// reply to attempt 1 that lands after attempt 2 went out reads as its real
// round trip, not as the time since attempt 2. It still completes the series,
// and one arriving after the series gave up still counts, for a while.
class ProbeSeries
{
  public:
    struct Attempt
    {
        std::uint64_t peer = 0;
        std::uint64_t nonce = 0;
        int attempt = 0; // 1-based
    };

    struct Sample
    {
        std::uint64_t peer = 0;
        int rttMs = 0;
        int attempt = 0; // which attempt the reply answered
        bool late = false; // its series had already ended
    };

    ProbeSeries(int attempts, std::int64_t retryIntervalMs, std::int64_t lateWindowMs)
        : m_attempts(std::max(1, attempts)), m_retryIntervalMs(retryIntervalMs), m_lateWindowMs(lateWindowMs)
    {
    }

    bool inFlight(std::uint64_t peer) const
    {
        return m_series.find(peer) != m_series.end();
    }

    // The first attempt, sent under nonce. False when a series to the peer is
    // already in flight, or the nonce is taken.
    bool start(std::uint64_t peer, std::uint64_t nonce, std::int64_t nowMs)
    {
        if (peer == 0 || nonce == 0 || inFlight(peer) || m_sent.find(nonce) != m_sent.end())
            return false;
        Series series;
        series.nextAttemptMs = nowMs + m_retryIntervalMs;
        series.attempt = 1;
        series.startedMs = nowMs;
        series.nonces.push_back(nonce);
        m_series.emplace(peer, std::move(series));
        m_sent.emplace(nonce, Sent{peer, nowMs, 1});
        return true;
    }

    // The attempts due now, each under a fresh nonce from makeNonce (never 0,
    // never one in use), and the peers whose series ran out of attempts.
    void tick(std::int64_t nowMs, const std::function<std::uint64_t()>& makeNonce,
              std::vector<Attempt>& resend, std::vector<std::uint64_t>& failed)
    {
        for (auto it = m_sent.begin(); it != m_sent.end();)
        {
            if (!ownedByLiveSeries(it->first, it->second.peer) && nowMs - it->second.sentMs > m_lateWindowMs)
                it = m_sent.erase(it);
            else
                ++it;
        }

        for (auto it = m_series.begin(); it != m_series.end();)
        {
            Series& series = it->second;
            if (nowMs < series.nextAttemptMs)
            {
                ++it;
                continue;
            }
            if (series.attempt >= m_attempts)
            {
                // Its nonces stay in m_sent for the late window.
                failed.push_back(it->first);
                it = m_series.erase(it);
                continue;
            }
            std::uint64_t nonce = makeNonce();
            while (nonce == 0 || m_sent.find(nonce) != m_sent.end())
                nonce = makeNonce();
            series.attempt += 1;
            series.nextAttemptMs = nowMs + m_retryIntervalMs;
            series.nonces.push_back(nonce);
            m_sent.emplace(nonce, Sent{it->first, nowMs, series.attempt});
            resend.push_back(Attempt{it->first, nonce, series.attempt});
            ++it;
        }
    }

    // A reply from peer carrying nonce. transportRttUs is the transport's own
    // timing of that nonce, 0 when it had none; the fallback is the time
    // since this nonce went out.
    std::optional<Sample> reply(std::uint64_t peer, std::uint64_t nonce, std::uint64_t transportRttUs,
                                std::int64_t nowMs)
    {
        const auto sent = m_sent.find(nonce);
        if (sent == m_sent.end() || sent->second.peer != peer)
            return std::nullopt;

        Sample sample;
        sample.peer = peer;
        sample.attempt = sent->second.attempt;
        const std::int64_t rttMs = transportRttUs != 0 ? std::int64_t((transportRttUs + 500) / 1000)
                                                       : nowMs - sent->second.sentMs;
        sample.rttMs = int(std::clamp<std::int64_t>(rttMs, 0, 65535));

        const auto series = m_series.find(peer);
        if (series != m_series.end() && ownedBy(series->second, nonce))
        {
            // One sample per series: the other attempts' replies are duplicates.
            for (std::uint64_t other : series->second.nonces)
                m_sent.erase(other);
            m_series.erase(series);
            sample.late = false;
        }
        else
        {
            m_sent.erase(sent);
            sample.late = true;
        }
        return sample;
    }

    // Forgets the peer's series and every nonce it sent, live or late.
    void cancel(std::uint64_t peer)
    {
        m_series.erase(peer);
        for (auto it = m_sent.begin(); it != m_sent.end();)
        {
            if (it->second.peer == peer)
                it = m_sent.erase(it);
            else
                ++it;
        }
    }

    void clear()
    {
        m_series.clear();
        m_sent.clear();
    }

    bool empty() const { return m_series.empty() && m_sent.empty(); }
    std::size_t seriesCount() const { return m_series.size(); }
    std::size_t trackedNonces() const { return m_sent.size(); }
    // When the peer's series in flight started, or -1.
    std::int64_t startedMs(std::uint64_t peer) const
    {
        const auto it = m_series.find(peer);
        return it == m_series.end() ? -1 : it->second.startedMs;
    }

  private:
    struct Sent
    {
        std::uint64_t peer = 0;
        std::int64_t sentMs = 0;
        int attempt = 0;
    };

    struct Series
    {
        std::int64_t startedMs = 0;
        std::int64_t nextAttemptMs = 0;
        int attempt = 0;
        std::vector<std::uint64_t> nonces;
    };

    static bool ownedBy(const Series& series, std::uint64_t nonce)
    {
        return std::find(series.nonces.begin(), series.nonces.end(), nonce) != series.nonces.end();
    }

    bool ownedByLiveSeries(std::uint64_t nonce, std::uint64_t peer) const
    {
        const auto it = m_series.find(peer);
        return it != m_series.end() && ownedBy(it->second, nonce);
    }

    int m_attempts;
    std::int64_t m_retryIntervalMs;
    std::int64_t m_lateWindowMs;
    std::unordered_map<std::uint64_t, Series> m_series; // by peer
    std::unordered_map<std::uint64_t, Sent> m_sent;     // by nonce: live series' and late ones'
};

} // namespace LobbyPing

#endif // LOBBYPINGTRACKER_HPP
