/*
 * Lag stats for rollback matches. Every client measures its own frames (see
 * rmgk_gekko::lag_stats) and sends the numbers to the other players once a
 * second. With everyone's numbers in hand, any player can tell who is holding
 * the game up and why:
 *
 *   - a PC that can't run a frame in the time a frame allows makes everyone
 *     else slow down or wait for its inputs;
 *   - when the others wait on a player and nothing at all arrives from them,
 *     that player's PC froze or their packets went missing (a player who is
 *     only behind, waiting on someone else, still sends acks and resends);
 *   - inputs that do arrive, but too late for the delay and prediction in
 *     use, point at the link they come over;
 *   - a PC that freezes now and then (shaders compiling, background programs)
 *     stutters on its own.
 *
 * Waiting alone can't name anyone: one player's hold-up makes the others stop
 * sending new inputs, and then the first player waits on them in turn.
 */
#ifdef NETPLAY

#include "LagStats.hpp"
#include "LobbyTheme.hpp"

#include <RMG-Core/LobbyIce.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <cmath>

using namespace UserInterface::Dialog;

namespace
{
constexpr int kWireVersion = 1;
constexpr int kTotalsFields = 26;
// A peer's numbers older than this are flagged: they arrive once a second.
constexpr qint64 kStaleReportMs = 5000;
// Own work above this share of a frame, or this many frames running long,
// means the PC can't keep up.
constexpr double kSlowFrameShare = 0.9;
constexpr int kSlowFramePercent = 10;
constexpr int kMinTimedFrames = 60;
// Waits shorter than this per window are noise.
constexpr qint64 kMinHeldUpMs = 250;
// Game packets are read once a frame at each end, which alone makes their
// round trip vary by up to about 15 ms. Well past that, the line itself is
// jittery.
constexpr int kJitteryMs = 30;

QString tr(const char* text)
{
    return QCoreApplication::translate("LagStats", text);
}

// RMG-K has no English plural forms, so "%n time(s)" would show as is.
QString plural(int n, const char* one, const char* many)
{
    return n == 1 ? tr(one) : tr(many).arg(n);
}

QString number(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

QJsonArray totalsToJson(const RollbackLagTotals& totals)
{
    QJsonArray array;
    array << totals.seconds << totals.frames << totals.timedFrames << totals.slowFrames
          << totals.hitches << totals.localHitches
          << qint64(totals.busyUs) << qint64(totals.busyMaxUs)
          << qint64(totals.waitUs) << qint64(totals.longestWaitUs)
          << totals.rollbacks << totals.rollbackFrames << totals.maxRollbackFrames;
    for (long long wait : totals.waitOnSlotUs)
        array << qint64(wait);
    array << qint64(totals.freezeUs);
    for (long long silent : totals.silentOnSlotUs)
        array << qint64(silent);
    for (long long longest : totals.longestSilenceOnSlotUs)
        array << qint64(longest);
    return array;
}

bool totalsFromJson(const QJsonValue& value, RollbackLagTotals& totals)
{
    const QJsonArray array = value.toArray();
    if (array.size() < kTotalsFields)
        return false;
    auto at = [&array](int i) { return array.at(i).toInteger(); };
    totals.seconds = int(at(0));
    totals.frames = int(at(1));
    totals.timedFrames = int(at(2));
    totals.slowFrames = int(at(3));
    totals.hitches = int(at(4));
    totals.localHitches = int(at(5));
    totals.busyUs = at(6);
    totals.busyMaxUs = at(7);
    totals.waitUs = at(8);
    totals.longestWaitUs = at(9);
    totals.rollbacks = int(at(10));
    totals.rollbackFrames = int(at(11));
    totals.maxRollbackFrames = int(at(12));
    for (int i = 0; i < 4; ++i)
        totals.waitOnSlotUs[i] = at(13 + i);
    totals.freezeUs = at(17);
    for (int i = 0; i < 4; ++i)
    {
        totals.silentOnSlotUs[i] = at(18 + i);
        totals.longestSilenceOnSlotUs[i] = at(22 + i);
    }
    return totals.seconds >= 0 && totals.frames >= 0;
}

QString joinNames(const QStringList& names)
{
    if (names.size() <= 1)
        return names.value(0);
    return tr("%1 and %2").arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")), names.last());
}

QString cell(double value, int decimals, const QString& suffix = QString())
{
    return value < 0 ? QStringLiteral("—") : number(value, decimals) + suffix;
}
} // namespace

QString LagStats::formatDuration(qint64 ms)
{
    if (ms < 1000)
        return tr("%1 ms").arg(ms);
    return tr("%1 s").arg(number(ms / 1000.0, ms < 10000 ? 1 : 0));
}

bool LagStats::parseCommand(const QString& text, bool* wholeMatch)
{
    const QStringList parts = text.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    const QString command = parts.first().toLower();
    if (command != QLatin1String("/lagstat") && command != QLatin1String("/lagstats"))
        return false;
    if (wholeMatch != nullptr)
        *wholeMatch = parts.size() > 1 && parts.at(1).compare(QLatin1String("match"), Qt::CaseInsensitive) == 0;
    return true;
}

QList<LagPlayer> LagStats::localPlayers(quint64 selfUserId, const SeatNamer& nameOf)
{
    QList<LagPlayer> players;
    RollbackLagStats local;
    rmgk_gekko::lag_stats(local);
    if (!local.valid)
        return players;

    // Every seat on this PC shares its numbers: one PC, one frame time.
    const std::vector<int> localSlots = local.localSlots.empty() ? std::vector<int>{local.localSlot} : local.localSlots;
    for (int slot : localSlots)
    {
        LagPlayer self;
        self.slot = slot;
        self.userId = selfUserId;
        self.name = nameOf(slot, selfUserId);
        if (self.name.isEmpty())
            self.name = tr("P%1").arg(slot);
        self.self = true;
        self.reported = true;
        self.stats = local;
        players << self;
    }
    for (const RollbackLagPeer& peer : local.peers)
    {
        LagPlayer player;
        player.slot = peer.slot;
        player.userId = peer.userId;
        player.name = nameOf(peer.slot, peer.userId);
        if (player.name.isEmpty())
            player.name = tr("P%1").arg(peer.slot);
        players << player;
    }
    std::sort(players.begin(), players.end(),
              [](const LagPlayer& a, const LagPlayer& b) { return a.slot < b.slot; });
    return players;
}

QByteArray LagStats::encode(const RollbackLagStats& stats)
{
    QJsonObject object;
    object["v"] = kWireVersion;
    object["act"] = stats.active;
    object["slot"] = stats.localSlot;
    QJsonArray localSlots;
    for (int slot : stats.localSlots)
        localSlots << slot;
    object["ls"] = localSlots;
    object["d"] = stats.delay;
    object["p"] = stats.prediction;
    object["hz"] = stats.frameHz;
    object["st"] = stats.stalledMs;
    object["sto"] = stats.stalledOnSlot;
    object["r"] = totalsToJson(stats.recent);
    object["m"] = totalsToJson(stats.match);
    QJsonArray peers;
    for (const RollbackLagPeer& peer : stats.peers)
    {
        // Tenths of a millisecond and hundredths of a frame keep it to ints.
        peers << QJsonArray{peer.slot, QString::number(peer.userId), peer.connected, peer.pingMs,
                            qRound(peer.avgPingMs * 10.0f), qRound(peer.jitterMs * 10.0f),
                            qRound(peer.framesAhead * 100.0f), peer.directPingMs};
    }
    object["peers"] = peers;
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool LagStats::decode(const QByteArray& data, RollbackLagStats& stats)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    if (object.value("v").toInt() < kWireVersion)
        return false;

    stats = RollbackLagStats{};
    stats.valid = true;
    stats.active = object.value("act").toBool();
    stats.localSlot = object.value("slot").toInt();
    for (const QJsonValue& slot : object.value("ls").toArray())
        if (slot.toInt() >= 1 && slot.toInt() <= 4)
            stats.localSlots.push_back(slot.toInt());
    // Clients without several players on one PC send no list.
    if (stats.localSlots.empty())
        stats.localSlots.push_back(stats.localSlot);
    stats.delay = object.value("d").toInt();
    stats.prediction = object.value("p").toInt();
    stats.frameHz = object.value("hz").toDouble(60.0);
    stats.stalledMs = object.value("st").toInt();
    stats.stalledOnSlot = object.value("sto").toInt();
    if (stats.localSlot < 1 || stats.localSlot > 4 || stats.frameHz < 1.0 ||
        !totalsFromJson(object.value("r"), stats.recent) ||
        !totalsFromJson(object.value("m"), stats.match))
    {
        return false;
    }
    for (const QJsonValue& value : object.value("peers").toArray())
    {
        const QJsonArray array = value.toArray();
        if (array.size() < 7)
            continue;
        RollbackLagPeer peer;
        peer.slot = array.at(0).toInt();
        peer.userId = array.at(1).toString().toULongLong();
        peer.connected = array.at(2).toBool();
        peer.pingMs = array.at(3).toInt(-1);
        peer.avgPingMs = float(array.at(4).toInt()) / 10.0f;
        peer.jitterMs = float(array.at(5).toInt()) / 10.0f;
        peer.framesAhead = float(array.at(6).toInt()) / 100.0f;
        peer.directPingMs = array.at(7).toInt(-1);
        if (peer.slot >= 1 && peer.slot <= 4)
            stats.peers.push_back(peer);
    }
    return true;
}

LagReport LagStats::build(const QList<LagPlayer>& players, bool wholeMatch)
{
    LagReport report;
    report.wholeMatch = wholeMatch;

    const LagPlayer* self = nullptr;
    for (const LagPlayer& player : players)
    {
        if (player.self)
            self = &player;
    }
    if (self == nullptr || !self->stats.valid)
        return report;

    auto totalsOf = [wholeMatch](const LagPlayer& player) -> const RollbackLagTotals& {
        return wholeMatch ? player.stats.match : player.stats.recent;
    };

    report.active = self->stats.active;
    report.budgetMs = 1000.0 / (self->stats.frameHz > 1.0 ? self->stats.frameHz : 60.0);
    report.seconds = totalsOf(*self).seconds;
    report.window = wholeMatch ? tr("this match") : tr("the last %1 s").arg(std::max(report.seconds, 1));

    QMap<int, const LagPlayer*> bySlot;
    for (const LagPlayer& player : players)
        bySlot.insert(player.slot, &player);
    auto nameOf = [&bySlot](int slot) {
        const LagPlayer* player = bySlot.value(slot, nullptr);
        return player != nullptr ? player->name : tr("P%1").arg(slot);
    };

    // waited[a][b]: how long a stood still for b's input. silent[a][b]: the
    // part of it when nothing at all arrived from b.
    QMap<int, QMap<int, qint64>> waited;
    QMap<int, QMap<int, qint64>> silent;
    QMap<int, qint64> heldUp;
    QMap<int, qint64> dropout;
    QMap<int, qint64> longestDropout;
    for (const LagPlayer& player : players)
    {
        if (!player.reported)
            continue;
        const RollbackLagTotals& totals = totalsOf(player);
        for (int slot = 1; slot <= 4; ++slot)
        {
            if (slot == player.slot)
                continue;
            const qint64 waitMs = totals.waitOnSlotUs[slot - 1] / 1000;
            const qint64 silentMs = totals.silentOnSlotUs[slot - 1] / 1000;
            if (waitMs > 0)
                waited[player.slot][slot] = waitMs;
            if (silentMs > 0)
                silent[player.slot][slot] = silentMs;
            // The longest anyone waited, so it reads against the window.
            heldUp[slot] = std::max(heldUp.value(slot), waitMs);
            dropout[slot] = std::max(dropout.value(slot), silentMs);
            longestDropout[slot] = std::max(longestDropout.value(slot), totals.longestSilenceOnSlotUs[slot - 1] / 1000);
        }
    }

    // Each link as either end sees it; keep the worse view.
    QMap<QPair<int, int>, QPair<int, int>> links;
    for (const LagPlayer& player : players)
    {
        if (!player.reported)
            continue;
        for (const RollbackLagPeer& peer : player.stats.peers)
        {
            const QPair<int, int> key(std::min(player.slot, peer.slot), std::max(player.slot, peer.slot));
            // The game's packets wait about a frame in all to be read at the
            // two ends. Past that, they took longer than the lobby's own ping
            // (stale, or the line queues under load): trust the slower one.
            const double frameMs = 1000.0 / (player.stats.frameHz > 1.0 ? player.stats.frameHz : 60.0);
            const int gamePing = peer.avgPingMs > 0.0f ? std::max(0, qRound(peer.avgPingMs - frameMs)) : peer.pingMs;
            const int ping = std::max(peer.directPingMs, gamePing);
            const int jitter = qRound(peer.jitterMs);
            auto it = links.find(key);
            if (it == links.end())
                links.insert(key, qMakePair(ping, jitter));
            else
                *it = qMakePair(std::max(it->first, ping), std::max(it->second, jitter));
        }
    }
    QMap<int, QPair<int, int>> worstLink;
    for (auto it = links.cbegin(); it != links.cend(); ++it)
    {
        LagReport::Link link;
        link.between = tr("%1 and %2").arg(nameOf(it.key().first), nameOf(it.key().second));
        link.pingMs = it->first;
        link.jitterMs = it->second;
        report.links << link;
        for (int slot : {it.key().first, it.key().second})
        {
            auto worst = worstLink.find(slot);
            if (worst == worstLink.end())
                worstLink.insert(slot, it.value());
            else
                *worst = qMakePair(std::max(worst->first, it->first), std::max(worst->second, it->second));
        }
    }

    for (const LagPlayer& player : players)
    {
        LagReport::Row row;
        row.slot = player.slot;
        row.name = player.name;
        row.self = player.self;
        row.reported = player.reported;
        row.ageMs = player.ageMs;
        row.heldUpMs = heldUp.value(player.slot);
        row.dropoutMs = dropout.value(player.slot);
        row.longestDropoutMs = longestDropout.value(player.slot);
        row.pingMs = worstLink.contains(player.slot) ? worstLink.value(player.slot).first : -1;
        row.jitterMs = worstLink.contains(player.slot) ? worstLink.value(player.slot).second : -1;
        if (player.reported)
        {
            const RollbackLagTotals& totals = totalsOf(player);
            const double seconds = std::max(totals.seconds, 1);
            row.fps = totals.seconds > 0 ? totals.frames / seconds : 0.0;
            row.frameMs = totals.timedFrames > 0 ? totals.busyUs / 1000.0 / totals.timedFrames : -1.0;
            row.frameMaxMs = totals.busyMaxUs / 1000.0;
            row.slowPercent = totals.timedFrames > 0 ? qRound(100.0 * totals.slowFrames / totals.timedFrames) : 0;
            row.stutters = totals.hitches;
            row.ownStutters = totals.localHitches;
            row.freezeMs = totals.freezeUs / 1000;
            row.waitedMs = totals.waitUs / 1000;
            row.rollbacksPerSecond = totals.rollbacks / seconds;
            row.maxRollback = totals.maxRollbackFrames;
        }
        report.rows << row;
    }
    std::sort(report.rows.begin(), report.rows.end(),
              [](const LagReport::Row& a, const LagReport::Row& b) { return a.slot < b.slot; });

    auto rowFor = [&report](int slot) -> LagReport::Row* {
        for (LagReport::Row& row : report.rows)
            if (row.slot == slot)
                return &row;
        return nullptr;
    };
    auto pcTooSlow = [&report, &totalsOf, &bySlot](const LagReport::Row& row) {
        const LagPlayer* player = bySlot.value(row.slot, nullptr);
        return row.reported && player != nullptr && totalsOf(*player).timedFrames >= kMinTimedFrames &&
               (row.slowPercent >= kSlowFramePercent || row.frameMs >= report.budgetMs * kSlowFrameShare);
    };
    auto pcKeepsUp = [&report](const LagReport::Row& row) {
        return row.reported && row.frameMs >= 0 && row.frameMs < report.budgetMs * 0.75 && row.slowPercent < 5;
    };
    QList<int> reporters;
    for (const LagReport::Row& row : report.rows)
        if (row.reported)
            reporters << row.slot;

    const qint64 windowMs = qint64(std::max(report.seconds, 1)) * 1000;
    const qint64 threshold = std::max(kMinHeldUpMs, windowMs / 80);
    const QString advice = tr("A higher frame delay or prediction window hides short delays; a wired connection helps with dropouts and jitter.");

    // 1. A PC that can't run frames as fast as the game needs them.
    QStringList slowNames;
    for (LagReport::Row& row : report.rows)
    {
        if (!pcTooSlow(row))
            continue;
        row.culprit = true;
        slowNames << row.name;
    }
    if (!slowNames.isEmpty())
    {
        report.severity = LagReport::Severity::Lag;
        report.headline = tr("%1 can't keep the game at full speed.").arg(joinNames(slowNames));
        for (const LagReport::Row& row : report.rows)
        {
            if (!row.culprit)
                continue;
            report.details << tr("%1's PC needs %2 ms to run a frame on average, against the %3 ms a frame allows; %4% of frames ran long.")
                                  .arg(row.name, number(row.frameMs, 1), number(report.budgetMs, 1))
                                  .arg(row.slowPercent);
            if (row.heldUpMs >= threshold)
                report.details << tr("The others spent up to %1 of %2 waiting for %3's inputs.")
                                      .arg(formatDuration(row.heldUpMs), report.window, row.name);
        }
        report.details << tr("Lower graphics settings (resolution, anti-aliasing) or close background programs on that PC.");
    }

    // 2. The others waited on someone while nothing at all arrived from them:
    //    their PC froze, or their packets went missing.
    int dropSlot = 0;
    for (auto it = dropout.cbegin(); it != dropout.cend(); ++it)
        if (it.value() >= threshold && (dropSlot == 0 || it.value() > dropout.value(dropSlot)))
            dropSlot = it.key();
    LagReport::Row* dropper = rowFor(dropSlot);
    if (report.severity == LagReport::Severity::None && dropper != nullptr)
    {
        report.severity = LagReport::Severity::Lag;
        dropper->culprit = true;
        // Its own freezes account for the silence: the PC, not the line.
        const bool froze = dropper->reported && dropper->ownStutters > 0 && dropper->freezeMs * 2 >= dropper->dropoutMs;
        if (froze)
        {
            report.headline = tr("%1's PC keeps freezing.").arg(dropper->name);
            report.details << plural(dropper->ownStutters, "%3's game froze once, for %2, and everyone else waited for %3 meanwhile.",
                                     "%3's game froze %1 times, for up to %2, and everyone else waited for %3 meanwhile.")
                                  .arg(formatDuration(qint64(dropper->frameMaxMs)), dropper->name);
            report.details << tr("Freezes like these usually come from shaders compiling, programs running in the background, or a PC running too hot.");
        }
        else
        {
            // Did every other player lose them, or only one?
            int lostBy = 0;
            int worstListener = 0;
            for (int slot : reporters)
            {
                if (slot == dropSlot)
                    continue;
                const qint64 ms = silent.value(slot).value(dropSlot);
                if (ms >= threshold / 2)
                    lostBy++;
                if (worstListener == 0 || ms > silent.value(worstListener).value(dropSlot))
                    worstListener = slot;
            }
            const int others = int(reporters.size()) - (dropper->reported ? 1 : 0);
            // Two players losing each other's packets about equally: either
            // end of the link could be at fault.
            const bool mutual = report.rows.size() == 2 && silent.value(dropSlot).value(worstListener) * 2 >= dropper->dropoutMs;
            if (mutual || (others >= 2 && lostBy == 1))
            {
                if (LagReport::Row* other = rowFor(worstListener))
                    other->culprit = mutual;
                report.headline = tr("The connection between %1 and %2 keeps dropping out.").arg(nameOf(worstListener), dropper->name);
            }
            else
            {
                report.headline = tr("%1's connection keeps dropping out.").arg(dropper->name);
            }
            report.details << tr("The others waited on %1 for up to %2 in %3 while nothing at all arrived from them, %4 at the worst.")
                                  .arg(dropper->name, formatDuration(dropper->dropoutMs), report.window,
                                       formatDuration(dropper->longestDropoutMs));
            if (pcKeepsUp(*dropper))
                report.details << tr("%1's PC keeps up (%2 ms per frame), so their packets are going missing on the way.")
                                      .arg(dropper->name, number(dropper->frameMs, 1));
            report.details << tr("A wired connection, or pausing downloads and streams on that network, usually fixes dropouts.");
        }
    }

    // 3. Inputs that arrive, but late: a link too slow for the delay and
    //    prediction in use, or a player who is behind.
    if (report.severity == LagReport::Severity::None)
    {
        QList<QPair<int, int>> badLinks;
        for (int a = 1; a <= 4; ++a)
            for (int b = a + 1; b <= 4; ++b)
                if (waited.value(a).value(b) + waited.value(b).value(a) >= threshold)
                    badLinks << qMakePair(a, b);

        int culpritSlot = 0;
        QPair<int, int> culpritLink(0, 0);
        if (badLinks.size() == 1)
        {
            const QPair<int, int> link = badLinks.first();
            const qint64 ab = waited.value(link.first).value(link.second);
            const qint64 ba = waited.value(link.second).value(link.first);
            const bool bothReported = reporters.contains(link.first) && reporters.contains(link.second);
            if (bothReported && std::min(ab, ba) * 3 >= std::max(ab, ba))
                culpritLink = link;
            else
                culpritSlot = ab >= ba ? link.second : link.first;
        }
        else if (!badLinks.isEmpty())
        {
            // The one player every troubled link runs through.
            QList<int> common = {badLinks.first().first, badLinks.first().second};
            for (const QPair<int, int>& link : badLinks)
                common.erase(std::remove_if(common.begin(), common.end(),
                                            [&link](int slot) { return slot != link.first && slot != link.second; }),
                             common.end());
            if (common.size() == 1)
                culpritSlot = common.first();
            else
                for (auto it = heldUp.cbegin(); it != heldUp.cend(); ++it)
                    if (culpritSlot == 0 || it.value() > heldUp.value(culpritSlot))
                        culpritSlot = it.key();
        }

        if (culpritLink.first != 0)
        {
            report.severity = LagReport::Severity::Lag;
            const QString a = nameOf(culpritLink.first);
            const QString b = nameOf(culpritLink.second);
            report.headline = tr("The connection between %1 and %2 is too slow for these settings.").arg(a, b);
            report.details << tr("They waited on each other's inputs: %1 waited %2 and %3 waited %4 in %5.")
                                  .arg(a, formatDuration(waited.value(culpritLink.first).value(culpritLink.second)), b,
                                       formatDuration(waited.value(culpritLink.second).value(culpritLink.first)), report.window);
            const auto link = links.constFind(culpritLink);
            if (link != links.constEnd() && link->first >= 0)
                report.details << tr("Ping between them is %1 ms.").arg(link->first);
            report.details << advice;
        }
        else if (LagReport::Row* worst = rowFor(culpritSlot))
        {
            report.severity = LagReport::Severity::Lag;
            worst->culprit = true;
            QStringList waiters;
            int otherReporters = 0;
            for (int slot : reporters)
            {
                if (slot == culpritSlot)
                    continue;
                otherReporters++;
                if (waited.value(slot).value(culpritSlot) >= kMinHeldUpMs / 2)
                    waiters << nameOf(slot);
            }
            if (!waiters.isEmpty() && waiters.size() == otherReporters)
            {
                report.headline = otherReporters > 1
                    ? tr("Everyone is waiting on %1.").arg(worst->name)
                    : tr("%1 is waiting on %2.").arg(waiters.first(), worst->name);
            }
            else
            {
                report.headline = tr("%1 is holding the game up.").arg(worst->name);
            }
            report.details << tr("%1's inputs arrived late and held the game up for %2 in %3.")
                                  .arg(worst->name, formatDuration(worst->heldUpMs), report.window);
            if (!worst->reported)
                report.details << tr("There are no lag stats from %1, so it can't tell whether their PC or their connection is the cause.")
                                      .arg(worst->name);
            else if (pcKeepsUp(*worst))
                report.details << tr("%1's PC keeps up (%2 ms per frame) and their packets arrive, only late: their ping is up to %3 ms.")
                                      .arg(worst->name, number(worst->frameMs, 1))
                                      .arg(worst->pingMs >= 0 ? QString::number(worst->pingMs) : QStringLiteral("?"));
            report.details << advice;
        }
    }

    // 4. Nobody held the game up.
    if (report.severity == LagReport::Severity::None)
    {
        const LagReport::Row* stutterer = nullptr;
        for (const LagReport::Row& row : report.rows)
        {
            const bool stuttered = row.ownStutters >= 3 || (row.ownStutters >= 1 && row.frameMaxMs >= 250.0);
            if (stuttered && (stutterer == nullptr || row.ownStutters > stutterer->ownStutters))
                stutterer = &row;
        }
        if (stutterer != nullptr)
        {
            report.severity = LagReport::Severity::Minor;
            report.headline = plural(stutterer->ownStutters, "%2's game stuttered once.", "%2's game stuttered %1 times.")
                                  .arg(stutterer->name);
            report.details << tr("The longest freeze took %1. Short freezes like these usually come from shaders compiling or programs running in the background on that PC.")
                                  .arg(formatDuration(qint64(stutterer->frameMaxMs)));
        }
        else
        {
            report.severity = LagReport::Severity::Fine;
            report.headline = tr("No one is lagging.");
            report.details << tr("Everyone ran at full speed and nobody waited on anyone's inputs.");
        }
    }

    for (const LagReport::Link& link : report.links)
    {
        if (link.jitterMs >= kJitteryMs)
            report.details << tr("The round trip between %1 varies by %2 ms, which points to Wi-Fi or a busy line.")
                                  .arg(link.between).arg(link.jitterMs);
    }

    int maxRollback = 0;
    for (const LagReport::Row& row : report.rows)
        maxRollback = std::max(maxRollback, row.maxRollback);
    if (report.severity != LagReport::Severity::Lag && maxRollback >= 5)
    {
        report.details << tr("Rollbacks went up to %1 frames. That comes from ping, not lag: a higher frame delay means fewer and shorter rollbacks.")
                              .arg(maxRollback);
    }

    if (report.active && self->stats.stalledMs >= 500)
    {
        report.notes << tr("The game is frozen right now: it has waited %1 for %2.")
                            .arg(formatDuration(self->stats.stalledMs),
                                 self->stats.stalledOnSlot > 0 ? nameOf(self->stats.stalledOnSlot) : tr("the other players"));
    }
    for (const LagPlayer& player : players)
    {
        if (player.self)
            continue;
        if (!player.reported)
            report.notes << tr("No lag stats from %1: they may be on an older RMG-K, or their connection to you is down.").arg(player.name);
        else if (report.active && player.ageMs > kStaleReportMs)
            report.notes << tr("%1's numbers are %2 old.").arg(player.name, formatDuration(player.ageMs));
    }
    if (!report.active)
        report.notes << tr("The match has ended; these are its final numbers.");
    if (report.seconds == 0)
        report.notes << tr("The match just started, so there's little to go on yet.");
    return report;
}

QColor LagStats::severityColor(LagReport::Severity severity)
{
    const LobbyTheme::Tokens& t = LobbyTheme::tokens();
    switch (severity)
    {
    case LagReport::Severity::Lag:
        return t.bad;
    case LagReport::Severity::Minor:
        return t.warn;
    case LagReport::Severity::Fine:
        return t.ok;
    case LagReport::Severity::None:
        break;
    }
    return t.muted;
}

QString LagStats::chatHtml(const LagReport& report)
{
    const LobbyTheme::Tokens& t = LobbyTheme::tokens();
    const QString color = severityColor(report.severity).name();
    const QString muted = t.muted.name();

    QString html = QStringLiteral("<b style='color:%1;'>%2</b> <span style='color:%3;'>%4</span>")
                       .arg(color, report.headline.toHtmlEscaped(), muted, report.window.toHtmlEscaped());
    for (const QString& line : report.details)
        html += QStringLiteral("<br>%1").arg(line.toHtmlEscaped());
    for (const LagReport::Row& row : report.rows)
    {
        QString line = QStringLiteral("<b style='color:%1;'>P%2</b> <b>%3</b>")
                           .arg(LobbyTheme::portColor(row.slot).name()).arg(row.slot).arg(row.name.toHtmlEscaped());
        if (!row.reported)
        {
            line += QStringLiteral(" · ") + tr("no stats").toHtmlEscaped();
        }
        else
        {
            QStringList parts;
            parts << tr("%1 fps").arg(number(row.fps, 1));
            if (row.frameMs >= 0)
                parts << tr("%1 ms/frame (%2% long)").arg(number(row.frameMs, 1)).arg(row.slowPercent);
            parts << tr("others waited %1").arg(formatDuration(row.heldUpMs));
            if (row.dropoutMs > 0)
                parts << tr("dropouts %1").arg(formatDuration(row.dropoutMs));
            parts << tr("waited on others %1").arg(formatDuration(row.waitedMs));
            if (row.pingMs >= 0)
                parts << tr("ping %1 ms").arg(row.pingMs);
            line += QStringLiteral(" · ") + parts.join(QStringLiteral(" · ")).toHtmlEscaped();
        }
        if (row.culprit)
            line = QStringLiteral("<span style='color:%1;'>%2</span>").arg(color, line);
        html += QStringLiteral("<br>") + line;
    }
    for (const QString& note : report.notes)
        html += QStringLiteral("<br><span style='color:%1;'>%2</span>").arg(muted, note.toHtmlEscaped());
    return html;
}

std::vector<std::vector<std::string>> LagStats::overlayTable(const LagReport& report)
{
    std::vector<std::vector<std::string>> table;
    table.push_back({tr("Player").toStdString(), tr("FPS").toStdString(), tr("ms/frame").toStdString(),
                     tr("Others waited").toStdString(), tr("Dropouts").toStdString(), tr("Waited on others").toStdString(),
                     tr("Ping").toStdString()});
    for (const LagReport::Row& row : report.rows)
    {
        QString name = QStringLiteral("P%1 %2").arg(row.slot).arg(row.name);
        if (row.culprit)
            name = QStringLiteral("> ") + name;
        if (!row.reported)
        {
            table.push_back({name.toStdString(), "-", "-", LagStats::formatDuration(row.heldUpMs).toStdString(),
                             LagStats::formatDuration(row.dropoutMs).toStdString(), "-",
                             row.pingMs >= 0 ? tr("%1 ms").arg(row.pingMs).toStdString() : std::string("-")});
            continue;
        }
        table.push_back({name.toStdString(), number(row.fps, 1).toStdString(),
                         row.frameMs >= 0 ? number(row.frameMs, 1).toStdString() : std::string("-"),
                         LagStats::formatDuration(row.heldUpMs).toStdString(),
                         LagStats::formatDuration(row.dropoutMs).toStdString(),
                         LagStats::formatDuration(row.waitedMs).toStdString(),
                         row.pingMs >= 0 ? tr("%1 ms").arg(row.pingMs).toStdString() : std::string("-")});
    }
    return table;
}

QString LagStats::plainText(const LagReport& report)
{
    QStringList lines;
    lines << tr("RMG-K lag check, %1").arg(report.window);
    lines << report.headline;
    for (const QString& line : report.details)
        lines << QStringLiteral("- ") + line;
    lines << QString();

    const QStringList header = {tr("Player"), tr("FPS"), tr("ms/frame"), tr("long"), tr("stutters"),
                                tr("others waited"), tr("dropouts"), tr("waited on others"), tr("ping"), tr("rollbacks")};
    QList<QStringList> table{header};
    for (const LagReport::Row& row : report.rows)
    {
        QString name = QStringLiteral("P%1 %2").arg(row.slot).arg(row.name);
        if (row.culprit)
            name += QStringLiteral(" *");
        if (!row.reported)
        {
            table << QStringList{name, tr("no stats"), QString(), QString(), QString(),
                                 formatDuration(row.heldUpMs), formatDuration(row.dropoutMs), QString(),
                                 row.pingMs >= 0 ? QString::number(row.pingMs) : QString(), QString()};
            continue;
        }
        table << QStringList{name, number(row.fps, 1), cell(row.frameMs, 1),
                             QStringLiteral("%1%").arg(row.slowPercent),
                             QString::number(row.stutters),
                             formatDuration(row.heldUpMs), formatDuration(row.dropoutMs), formatDuration(row.waitedMs),
                             row.pingMs >= 0 ? QString::number(row.pingMs) : QString(),
                             tr("%1/s, max %2").arg(number(row.rollbacksPerSecond, 1)).arg(row.maxRollback)};
    }
    QList<int> widths(header.size(), 0);
    for (const QStringList& row : table)
        for (int i = 0; i < row.size(); ++i)
            widths[i] = std::max(widths[i], int(row.at(i).size()));
    for (const QStringList& row : table)
    {
        QString line;
        for (int i = 0; i < row.size(); ++i)
            line += row.at(i).leftJustified(widths[i] + 2);
        lines << line.trimmed();
    }
    for (const LagReport::Link& link : report.links)
        lines << tr("%1: ping %2 ms, jitter %3 ms").arg(link.between).arg(link.pingMs).arg(link.jitterMs);
    for (const QString& note : report.notes)
        lines << note;
    return lines.join('\n');
}

LagStatsExchange::LagStatsExchange(std::function<int(quint64)> directPing, QObject* parent)
    : QObject(parent), m_directPing(std::move(directPing))
{
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &LagStatsExchange::tick);
    m_timer->start();
}

void LagStatsExchange::reset()
{
    m_reports.clear();
    LobbyIce::take_packets(LobbyIceChannel::LagStats);
}

void LagStatsExchange::tick()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const LobbyIcePacket& packet : LobbyIce::take_packets(LobbyIceChannel::LagStats))
    {
        RollbackLagStats stats;
        if (LagStats::decode(QByteArray(packet.data.data(), qsizetype(packet.data.size())), stats))
            m_reports.insert(packet.peerUserId, Received{std::move(stats), nowMs});
    }

    RollbackLagStats local;
    rmgk_gekko::lag_stats(local);
    if (!local.valid || !local.active)
        return;
    for (RollbackLagPeer& peer : local.peers)
        peer.directPingMs = peer.userId != 0 ? m_directPing(peer.userId) : -1;
    const QByteArray payload = LagStats::encode(local);
    // A PC with several players has one connection for all their seats.
    QSet<quint64> sentTo;
    for (const RollbackLagPeer& peer : local.peers)
    {
        if (peer.userId == 0 || !peer.connected || sentTo.contains(peer.userId))
            continue;
        sentTo.insert(peer.userId);
        LobbyIce::send(peer.userId, LobbyIceChannel::LagStats, payload.constData(), size_t(payload.size()));
    }
}

QList<LagPlayer> LagStatsExchange::players(quint64 selfUserId, const LagStats::SeatNamer& nameOf) const
{
    QList<LagPlayer> players = LagStats::localPlayers(selfUserId, nameOf);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (LagPlayer& player : players)
    {
        if (player.self)
        {
            for (RollbackLagPeer& peer : player.stats.peers)
                peer.directPingMs = peer.userId != 0 ? m_directPing(peer.userId) : -1;
            continue;
        }
        const auto it = m_reports.constFind(player.userId);
        if (player.self || player.userId == 0 || it == m_reports.constEnd() ||
            std::find(it->stats.localSlots.begin(), it->stats.localSlots.end(), player.slot) == it->stats.localSlots.end())
            continue;
        player.reported = true;
        player.ageMs = nowMs - it->receivedMs;
        player.stats = it->stats;
    }
    return players;
}

#endif // NETPLAY
