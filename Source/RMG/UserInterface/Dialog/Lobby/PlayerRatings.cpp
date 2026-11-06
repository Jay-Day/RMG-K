/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "PlayerRatings.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

using namespace UserInterface::Dialog;

namespace
{
// how long a fetched card stays fresh
constexpr qint64 kFreshMs = 2 * 60 * 1000;
} // namespace

PlayerRatings::PlayerRatings(QObject* parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

void PlayerRatings::setServer(const QString& wsUrl)
{
    QUrl url(wsUrl);
    url.setScheme(url.scheme() == "wss" ? "https" : "http");
    url.setPath("/api/ranked/player");
    url.setQuery(QString());
    if (url != m_base)
    {
        m_base = url;
        m_cache.clear();
    }
}

PlayerRatings::Status PlayerRatings::lookup(const QString& name, QJsonArray& standings, QString& region)
{
    const QString key = name.toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = m_cache.find(key);
    if (it != m_cache.end())
    {
        standings = it->standings;
        region    = it->region;
        const bool stale = it->status != Status::Loading && now - it->fetchedMs > kFreshMs;
        if (!stale)
            return it->status;
    }
    if (!m_base.isValid() || name.isEmpty())
        return Status::NotFound;

    // stale entries keep showing while they refresh
    Entry& entry = m_cache[key];
    const Status shown = entry.fetchedMs != 0 ? entry.status : Status::Loading;
    if (entry.fetchedMs == 0)
        entry.status = Status::Loading;
    entry.fetchedMs = now;

    QUrl url = m_base;
    QUrlQuery query;
    query.addQueryItem("name", name);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(8000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, name]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object();
        auto it = m_cache.find(key);
        if (it == m_cache.end())
            return; // invalidated meanwhile
        if (reply->error() == QNetworkReply::NoError && status == 200)
        {
            it->status    = Status::Found;
            it->standings = data.value("standings").toArray();
            it->region    = data.value("region").toString();
        }
        else if (it->status == Status::Loading || status == 404)
        {
            it->status = Status::NotFound;
            it->standings = QJsonArray();
        }
        it->fetchedMs = QDateTime::currentMSecsSinceEpoch();
        emit updated(name);
    });
    return shown;
}

void PlayerRatings::invalidate(const QString& name)
{
    m_cache.remove(name.toLower());
}

#endif // NETPLAY
