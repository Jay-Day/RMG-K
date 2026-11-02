/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef LOBBYSOCIALDIALOGS_HPP
#define LOBBYSOCIALDIALOGS_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QJsonArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QButtonGroup;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace UserInterface
{
namespace Dialog
{

class CartridgeTile;
class LobbyButton;
class ToggleSwitch;

// Who a challenge is between, as the dialog shows it.
struct ChallengeDialogPlayer
{
    QString name;
    QString country;
    QString connection;
    int pingMs = -1;
    bool pingEstimate = true;
};

// Sets up a challenge: which game, a short message, and whether the match streams.
class ChallengeDialog : public QDialog
{
    Q_OBJECT

public:
    // games are the library's items, as the lobby's picker holds them ({name, md5});
    // selectedMd5 starts selected.
    ChallengeDialog(const ChallengeDialogPlayer& self, const ChallengeDialogPlayer& target,
                    const QList<QVariantMap>& games, const QString& selectedMd5, QWidget* parent = nullptr);

    // Which of the games the target has too, once the server says.
    void setSharedGames(const QStringList& md5s);
    void setCoverLookup(std::function<QString(const QString&)> coverForMd5);

    QString romName() const;
    QString romMd5() const;
    QString note() const;
    bool liveReplay() const;

private:
    void refreshGame();
    void fillCombo(const QString& selectMd5);

    QString m_targetName;
    QList<QVariantMap> m_games;
    QStringList m_shared;
    bool m_sharedKnown = false;
    std::function<QString(const QString&)> m_coverForMd5;

    CartridgeTile* m_tile = nullptr;
    QComboBox* m_combo = nullptr;
    QLabel* m_gameStatus = nullptr;
    QLineEdit* m_note = nullptr;
    ToggleSwitch* m_liveReplay = nullptr;
    LobbyButton* m_send = nullptr;
};

// Bans or mutes a player, with how long and why.
class SanctionDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Kind { Ban, Mute };

    SanctionDialog(Kind kind, const QString& target, QWidget* parent = nullptr);

    // The server's answer about who else is on that connection.
    void setOthers(const QStringList& others);

    QString targetName() const { return m_target; }
    // What to send: "ban", "timeout" or "mute", with its duration.
    QString action() const;
    QString duration() const;
    QString reason() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshConfirm();

    Kind m_kind;
    QString m_target;
    QButtonGroup* m_durations = nullptr;
    QLineEdit* m_reason = nullptr;
    QLabel* m_note = nullptr;
    LobbyButton* m_confirm = nullptr;
};

// Active bans and mutes with a way to lift them, and what this moderator did this
// session. Modeless, so the lobby stays usable beside it.
class ModerationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModerationDialog(const QString& moderatorName, QWidget* parent = nullptr);

    void setSanctions(const QJsonArray& bans, const QJsonArray& mutes);
    void addLogEntry(const QString& text);

signals:
    // kind is "unban" or "unmute".
    void liftRequested(const QString& kind, const QString& ip);

private:
    void showPage(int page);

    QPushButton* m_activeChip = nullptr;
    QPushButton* m_logChip = nullptr;
    QFrame* m_activeHeading = nullptr;
    QWidget* m_activeBody = nullptr;
    QVBoxLayout* m_activeRows = nullptr;
    QLabel* m_activeEmpty = nullptr;
    QWidget* m_logBody = nullptr;
    QVBoxLayout* m_logRows = nullptr;
    QLabel* m_logEmpty = nullptr;
    int m_logCount = 0;
};

// The players this PC ignores, with a way to stop.
class IgnoredPlayersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit IgnoredPlayersDialog(const QStringList& names, QWidget* parent = nullptr);

signals:
    void unignoreRequested(const QString& name);

private:
    void addRow(const QString& name);

    QVBoxLayout* m_rows = nullptr;
    QLabel* m_empty = nullptr;
    int m_count = 0;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // LOBBYSOCIALDIALOGS_HPP
