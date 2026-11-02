/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef LAGSTATS_HPP
#define LAGSTATS_HPP

#ifdef NETPLAY

#include <RMG-Core/rmgk_gekko.hpp>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <string>
#include <vector>

class QTimer;

namespace UserInterface
{
namespace Dialog
{

// One player of a rollback match as this client knows them: our own numbers,
// or the last lag report the player sent us.
struct LagPlayer
{
    int slot = 0;
    quint64 userId = 0;
    QString name;
    bool self = false;
    bool reported = false; // we have numbers for them (always true for self)
    qint64 ageMs = 0;      // how old those numbers are
    RollbackLagStats stats;
};

// What /lagstat and the Lag Stats window show: who is holding the game up,
// and the numbers behind it.
struct LagReport
{
    enum class Severity
    {
        None,  // nothing to report on
        Fine,
        Minor, // stutters, but nobody is holding the game up
        Lag,
    };

    struct Row
    {
        int slot = 0;
        QString name;
        bool self = false;
        bool reported = false;
        bool culprit = false;
        double fps = 0.0;
        double frameMs = -1.0;    // own work per frame, -1 if unmeasured
        double frameMaxMs = 0.0;
        int slowPercent = 0;
        int stutters = 0;
        int ownStutters = 0;
        qint64 freezeMs = 0;      // own stutters' time past one frame
        qint64 heldUpMs = 0;      // the longest anyone waited on this player's input
        // ...of it, while nothing at all arrived from them (lost packets, or a
        // frozen PC), and the longest such gap
        qint64 dropoutMs = 0;
        qint64 longestDropoutMs = 0;
        qint64 waitedMs = 0;      // this player's wait on the others
        int pingMs = -1;          // worst link of this player
        int jitterMs = -1;
        double rollbacksPerSecond = 0.0;
        int maxRollback = 0;
        qint64 ageMs = 0;
    };

    struct Link
    {
        QString between;
        int pingMs = -1;
        int jitterMs = -1;
    };

    Severity severity = Severity::None;
    bool active = false;
    bool wholeMatch = false;
    int seconds = 0;
    double budgetMs = 1000.0 / 60.0;
    QString window;      // "the last 20 s" / "this match"
    QString headline;
    QStringList details; // why, and what might help
    QStringList notes;   // missing or stale data
    QList<Row> rows;
    QList<Link> links;
};

namespace LagStats
{
// The ICE payload one player sends the others.
QByteArray encode(const RollbackLagStats& stats);
bool decode(const QByteArray& data, RollbackLagStats& stats);

// True for "/lagstat" (or "/lagstats"); "/lagstat match" asks about the
// whole match instead of the last RecentSeconds.
bool parseCommand(const QString& text, bool* wholeMatch);

// This client and the players its session knows of, without their reports.
QList<LagPlayer> localPlayers(quint64 selfUserId, const std::function<QString(quint64)>& nameOf);

LagReport build(const QList<LagPlayer>& players, bool wholeMatch);

// The report as a room chat block (HTML), a table for the in-game overlay,
// and plain text for the clipboard.
QString chatHtml(const LagReport& report, bool dark);
std::vector<std::vector<std::string>> overlayTable(const LagReport& report);
QString plainText(const LagReport& report);

QString formatDuration(qint64 ms);
} // namespace LagStats

// Sends this client's lag numbers to the other players of a running lobby
// match once a second, over the room's ICE mesh, and keeps the latest numbers
// each of them sent back.
class LagStatsExchange : public QObject
{
    Q_OBJECT

  public:
    // directPing gives the lobby's own round trip to a user (-1 if unknown),
    // which reads truer than the game packets' one.
    explicit LagStatsExchange(std::function<int(quint64)> directPing, QObject* parent = nullptr);

    // Forget the reports of an earlier match.
    void reset();

    // Every player of the match, in seat order. nameOf names a lobby user.
    QList<LagPlayer> players(quint64 selfUserId, const std::function<QString(quint64)>& nameOf) const;

  private:
    void tick();

    struct Received
    {
        RollbackLagStats stats;
        qint64 receivedMs = 0;
    };
    QHash<quint64, Received> m_reports;
    std::function<int(quint64)> m_directPing;
    QTimer* m_timer = nullptr;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LAGSTATS_HPP
