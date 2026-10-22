/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAYLINK_HPP
#define REPLAYLINK_HPP

#include <QObject>
#include <QString>
#include <QUrl>

class QLocalServer;

//
// rmgk:// replay links, the rankings site's "Watch in RMG-K" button:
//
//   rmgk://replay?url=<http(s)://<lobby>/api/ranked/sets/<id>/replay>[&record=<n>]
//
// record, when given, is the .krec input record to start from (a game's
// bookmark on the set page).
//
// Only that download URL shape is accepted; RMG-K asks before downloading.
//
namespace ReplayLink
{
// whether a command line argument is an rmgk:// link
bool IsLink(const QString& argument);

// validates a link, giving the replay's download URL, set id and the record
// to start from (0 = the beginning)
bool Parse(const QString& link, QUrl& replayUrl, qint64& setId, int& record, QString& error);

// hands the link to an RMG-K that's already running, false when none is
bool ForwardToRunningInstance(const QString& link);

// makes this executable the handler of rmgk:// links for the current user:
// the registry on Windows; on Linux a desktop entry for portable builds and
// AppImages (system packages install com.github.Rosalie241.RMG.ReplayLink.desktop)
void RegisterHandler(void);

// the Linux desktop entry that opens rmgk:// links with `executable`
QString DesktopEntry(const QString& executable);

// receives links forwarded by later launches
class Server : public QObject
{
    Q_OBJECT

  public:
    explicit Server(QObject* parent = nullptr);
    bool Listen(void);

  signals:
    void linkReceived(QString link);

  private:
    QLocalServer* server = nullptr;
};
} // namespace ReplayLink

#endif // REPLAYLINK_HPP
