/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef PLAYERRATINGS_HPP
#define PLAYERRATINGS_HPP

#ifdef NETPLAY

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

namespace UserInterface
{
namespace Dialog
{

// Players' ranked standings for hover cards, read from the lobby's public
// player API (/api/ranked/player) and cached for a short while.
class PlayerRatings : public QObject
{
    Q_OBJECT

public:
    enum class Status
    {
        Loading,
        Found,
        NotFound, // no such account (guest) or the lookup failed
    };

    explicit PlayerRatings(QObject* parent = nullptr);

    void setServer(const QString& wsUrl);
    // The cached standings (the API's "standings" array); fetches them when
    // missing or stale, and updated(name) follows.
    Status lookup(const QString& name, QJsonArray& standings, QString& region);
    // drops a cached player, e.g. after their set was rated
    void invalidate(const QString& name);

signals:
    void updated(const QString& name);

private:
    struct Entry
    {
        Status     status = Status::Loading;
        QJsonArray standings;
        QString    region;
        qint64     fetchedMs = 0;
    };

    QNetworkAccessManager* m_network = nullptr;
    QUrl m_base;
    QHash<QString, Entry> m_cache; // by lowercased name
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // PLAYERRATINGS_HPP
