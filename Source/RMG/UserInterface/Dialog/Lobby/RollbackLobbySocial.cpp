/*
 * The parts of the rollback lobby that are about other people: the menu on a
 * player's name, challenges, private messages, @mentions and moderation. They all
 * hang off that one menu, so whatever a name shows up in can offer all of them.
 *
 * Nothing here says that a PC is shared, except to moderators. See
 * LobbyClient::LocalPlayer: a message or a challenge to one of this PC's other
 * players arrives here like any other, and an answer goes out as that player.
 */
#ifdef NETPLAY

#include "RollbackLobbyDialog.hpp"

#include "LobbyBrowsePanels.hpp"
#include "LobbyPrivateChats.hpp"
#include "LobbyRegions.hpp"
#include "LobbyPanelLayout.hpp"
#include "LobbySocialDialogs.hpp"
#include "LobbySocialWidgets.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"
#include "OnScreenDisplay.hpp"

#include <RMG-Core/Settings.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QIcon>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    const char* const CHANNEL_LOBBY = "lobby";
    const char* const CHANNEL_ROOM  = "room";
    const char* const CHANNEL_SPECTATE = "spectate";

    const char* const kIgnoredSetting = "Lobby/IgnoredPlayers";
    const char* const kDoNotDisturbSetting = "Lobby/DoNotDisturb";
    const char* const kNoSoundsSetting = "Lobby/NoSounds";
    const char* const kNoChallengesSetting = "Lobby/NoChallenges";
    const char* const kNoPrivateMessagesSetting = "Lobby/NoPrivateMessages";
    const char* const kSpectatorOverlaySetting = "Lobby/SpectatorChatOverGame";

    // The most of one spectator's comment that goes over the game.
    constexpr int kOverlayCommentChars = 120;
    // How often one person can make this PC make a noise.
    constexpr qint64 kAlertIntervalMs = 30'000;
    // How long past its deadline to wait for the lobby to end a challenge.
    constexpr int kChallengeGraceMs = 3'000;
    constexpr int kMaxMentionRows = 5;
    // How long someone who wrote to us can be answered even with their private
    // messages off. The lobby keeps the same window.
    constexpr qint64 kPmReplyWindowMs = 10 * 60'000;
    // How many names the after-match summary lists before "and 2 more".
    constexpr int kSummaryNames = 3;

    // The top of a player's menu: who this is and what they're up to.
    class PlayerMenuHeader : public QWidget
    {
    public:
        PlayerMenuHeader(const QString& name, const QString& country, const QColor& dot,
                         const QString& status, QWidget* parent)
            : QWidget(parent)
        {
            auto* lay = new QHBoxLayout(this);
            lay->setContentsMargins(11, 6, 11, 9);
            lay->setSpacing(9);
            auto* tile = new PlayerTile(32, this);
            tile->setPlayer(name);
            lay->addWidget(tile);
            auto* words = new QVBoxLayout;
            words->setSpacing(2);
            auto* top = new QHBoxLayout;
            top->setSpacing(6);
            auto* nameLabel = new ElidedLabel(Role::Ink, this);
            nameLabel->setFont(LobbyTheme::uiFont(13, QFont::Bold));
            nameLabel->setText(name);
            top->addWidget(nameLabel);
            auto* flag = new CountryFlag(this);
            flag->setCountry(country);
            top->addWidget(flag);
            top->addStretch(1);
            words->addLayout(top);
            auto* bottom = new QHBoxLayout;
            bottom->setSpacing(5);
            auto* led = new LedDot(this);
            led->setColor(dot);
            bottom->addWidget(led);
            auto* statusLabel = new QLabel(status, this);
            statusLabel->setTextFormat(Qt::PlainText); // it can hold a room's name
            statusLabel->setFont(LobbyTheme::uiFont(10));
            statusLabel->setProperty("role", "muted");
            bottom->addWidget(statusLabel);
            bottom->addStretch(1);
            words->addLayout(bottom);
            lay->addLayout(words, 1);
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(QRect(0, height() - 1, width(), 1), LobbyTheme::tokens().line);
        }
    };

    bool isNameChar(QChar ch)
    {
        return ch.isLetterOrNumber() || ch.isMark() || ch.isSurrogate() || ch == QChar(0x30FB) ||
               ch == QLatin1Char('_') || ch == QLatin1Char('-') || ch == QLatin1Char('.');
    }
} // namespace

// ──────────────────────────────────────────────────────────────────────
//  Set-up
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::connectSocialSignals()
{
    connect(m_client, &LobbyClient::challengeSent, this, &RollbackLobbyDialog::onChallengeSent);
    connect(m_client, &LobbyClient::challengeIncoming, this, &RollbackLobbyDialog::onChallengeIncoming);
    connect(m_client, &LobbyClient::challengeEnded, this, &RollbackLobbyDialog::onChallengeEnded);
    connect(m_client, &LobbyClient::challengeMissed, this, &RollbackLobbyDialog::onChallengeMissed);

    connect(m_challengesButton, &QPushButton::clicked, this, &RollbackLobbyDialog::toggleChallengeLog);
    connect(m_challengeTray, &ChallengeTray::acceptClicked, this,
            [this](quint64 challengeId) { answerChallenge(challengeId, true); });
    connect(m_challengeTray, &ChallengeTray::declineClicked, this,
            [this](quint64 challengeId) { answerChallenge(challengeId, false); });
    connect(m_challengeTray, &ChallengeTray::cancelClicked, this, &RollbackLobbyDialog::cancelOutgoingChallenge);
    connect(m_challengeTray, &ChallengeTray::summaryViewClicked, this, &RollbackLobbyDialog::openChallengeLog);
    connect(m_challengeTray, &ChallengeTray::summaryDismissed, this, &RollbackLobbyDialog::dismissMissedChallengesSummary);
    connect(m_challengeTray, &ChallengeTray::actionClicked, this, &RollbackLobbyDialog::onChallengeLogAction);
    connect(m_challengeTray, &ChallengeTray::messageClicked, this, &RollbackLobbyDialog::onChallengeLogMessage);
    connect(m_challengeTray, &ChallengeTray::logClosed, this, &RollbackLobbyDialog::onChallengeLogClosed);
    connect(m_challengeTray, &ChallengeTray::clearClicked, this, [this]() {
        ChallengeLog::session().clear();
        m_freshLogKeys.clear();
        refreshChallengeTray();
    });
    positionChallengeTray();

    connect(m_client, &LobbyClient::privateMessageFailed, this, &RollbackLobbyDialog::onPrivateMessageFailed);
    connect(m_client, &LobbyClient::modResult, this, &RollbackLobbyDialog::onModResult);
    connect(m_client, &LobbyClient::adminLoggedOut, this, [this]() {
        if (m_moderationDialog)
        {
            m_moderationDialog->deleteLater();
            m_moderationDialog = nullptr;
        }
        updateUserChip();
        appendChatSystemLine(CHANNEL_LOBBY, tr("You've signed out of moderation."));
    });
    connect(m_mentionJump, &QPushButton::clicked, this, [this]() {
        const int tab = m_mentionJumpTab;
        ChatView* view = chatViewFor(tab);
        if (!view || m_pendingMentions[tab].isEmpty())
            return;
        view->setTextCursor(QTextCursor(view->document()->findBlockByNumber(m_pendingMentions[tab].first())));
        view->ensureCursorVisible();
        refreshMentionJump();
    });

    QSettings settings("RMG-K", "n02");
    m_ignored = settings.value(kIgnoredSetting).toStringList();
    m_doNotDisturb = settings.value(kDoNotDisturbSetting, false).toBool();
    m_noSounds = settings.value(kNoSoundsSetting, false).toBool();
    m_client->setChallengesOff(settings.value(kNoChallengesSetting, false).toBool());
    m_client->setPrivateMessagesOff(settings.value(kNoPrivateMessagesSetting, false).toBool());
    m_spectatorOverlayToggle->setChecked(settings.value(kSpectatorOverlaySetting, false).toBool());
    connect(m_spectatorOverlayToggle, &QCheckBox::toggled, this, [](bool checked) {
        QSettings("RMG-K", "n02").setValue(kSpectatorOverlaySetting, checked);
    });
}

void RollbackLobbyDialog::connectChatView(ChatView* view)
{
    connect(view, &ChatView::userActivated, this, &RollbackLobbyDialog::showPlayerMenu);
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { refreshMentionJump(); });
}

void RollbackLobbyDialog::resetSocialState()
{
    // The challenges go with the session; the log stays for the app's.
    m_incomingChallenges.clear();
    m_outgoingChallenge = LobbyClient::Challenge{};
    m_acceptedChallenge = LobbyClient::Challenge{};
    m_selfPlaying = false;
    m_challengeTray->clearOutcome();
    if (m_challengeTray->isLogOpen())
        closeChallengeLog();
    refreshChallengeTray();
    m_privateChats->clear();
    m_lastPmLocal = 0;
    m_lastPmRemote = 0;
    m_pmReceivedMs.clear();
    m_lastAlertMs.clear();
    for (QList<int>& pending : m_pendingMentions)
        pending.clear();
    m_mentionJump->setVisible(false);
    if (m_moderationDialog)
    {
        m_moderationDialog->deleteLater();
        m_moderationDialog = nullptr;
    }
    // They name players of a session that's over, and ids get reused by the next one.
    if (m_challengeDialog)
        m_challengeDialog->reject();
    if (m_sanctionDialog)
        m_sanctionDialog->reject();
}

bool RollbackLobbyDialog::socialEventFilter(QObject* watched, QEvent* event)
{
    // The message box with focus is the one mentions and Send work on.
    if ((watched == m_chatTopInput || watched == m_chatSplitInput) && event->type() == QEvent::FocusIn &&
        watched != m_chatInput)
    {
        m_mentionPopup->hide();
        m_chatInput = static_cast<QLineEdit*>(watched);
    }
    if (watched == m_stage && event->type() == QEvent::Resize)
        layoutStage();
    else if (watched == m_chatCard && event->type() == QEvent::Resize)
        positionChatOverlays();
    else if (watched == m_chatInput && event->type() == QEvent::FocusOut)
        m_mentionPopup->hide();
    else if (watched == m_chatInput && event->type() == QEvent::KeyPress && m_mentionPopup->isVisible())
    {
        switch (static_cast<QKeyEvent*>(event)->key())
        {
            case Qt::Key_Up:
                m_mentionPopup->moveSelection(-1);
                return true;
            case Qt::Key_Down:
                m_mentionPopup->moveSelection(1);
                return true;
            case Qt::Key_Tab:
            case Qt::Key_Return:
            case Qt::Key_Enter:
                insertMention(m_mentionPopup->selectedName());
                return true;
            case Qt::Key_Escape:
                m_mentionPopup->hide();
                return true;
        }
    }
    return false;
}

void RollbackLobbyDialog::refreshChatTabIcons()
{
    if (m_chatTabs)
        refreshSpectatorTab();
}

void RollbackLobbyDialog::alertFrom(quint64 userId)
{
    if (m_doNotDisturb)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto last = m_lastAlertMs.constFind(userId);
    if (last != m_lastAlertMs.constEnd() && now - *last < kAlertIntervalMs)
        return;
    m_lastAlertMs.insert(userId, now);
    QApplication::alert(this);
    if (!m_noSounds)
        QApplication::beep();
}

// ──────────────────────────────────────────────────────────────────────
//  The player menu
// ──────────────────────────────────────────────────────────────────────

quint64 RollbackLobbyDialog::userIdForName(const QString& name) const
{
    for (const auto& user : m_client->users())
    {
        if (user.username.compare(name, Qt::CaseInsensitive) == 0)
            return user.id;
    }
    return 0;
}

void RollbackLobbyDialog::showPlayerMenuForName(const QString& name, const QPoint& globalPos)
{
    if (const quint64 userId = userIdForName(name))
        showPlayerMenu(userId, globalPos);
}

void RollbackLobbyDialog::showPlayerMenu(quint64 userId, const QPoint& globalPos)
{
    const auto found = m_client->users().constFind(userId);
    if (found == m_client->users().constEnd() || m_client->isOnThisPc(userId))
        return;
    // A copy: the menu runs an event loop, and presence can change under it.
    const LobbyClient::LobbyUser user = *found;
    const QString name = user.username;
    const bool social = m_client->socialSupported();
    const bool inMyRoom = m_currentRoomId != 0 && user.currentRoomId == m_currentRoomId;
    const bool playing = user.state == QLatin1String("playing");
    const auto theirRoom = m_client->rooms().constFind(user.currentRoomId);
    const bool hasRoom = user.currentRoomId != 0 && theirRoom != m_client->rooms().constEnd();

    QString status;
    if (inMyRoom)
        status = tr("In your room");
    else if (playing && hasRoom)
        status = tr("Playing %1").arg(theirRoom->romName);
    else if (hasRoom)
        status = theirRoom->name;
    else
        status = stateGlyph(user.state);
    const int measured = m_client->measuredPingMs(userId);
    const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), user.region);
    if (measured >= 0)
        status += tr(" · %1 ms").arg(measured);
    else if (estimate > 0)
        status += tr(" · ~%1 ms").arg(estimate);
    const QString connection = ConnectionGlyph::displayName(user.connection);
    if (!connection.isEmpty())
        status += QStringLiteral(" · ") + connection;

    LobbyMenu menu(this);
    menu.addHeaderWidget(new PlayerMenuHeader(name, user.anonymous ? QString() : user.country,
                                              presenceColor(user.state), status, &menu));

    if (social && !inMyRoom)
    {
        const QString title = m_currentRoomId != 0 ? tr("Invite to your room") : tr("Challenge…");
        const QString why = challengeUnavailableReason(user);
        if (why.isEmpty())
        {
            connect(menu.addItem(QStringLiteral("sword-fill"), title), &QAction::triggered, this,
                    [this, userId]() { startChallenge(userId); });
        }
        else
        {
            menu.addDisabledItem(QStringLiteral("sword-fill"), title, why);
        }
    }
    if (social)
    {
        if (!user.social)
            menu.addDisabledItem(QStringLiteral("mail-line"), tr("Send a message"), tr("Needs an update"));
        else if (!canMessage(user))
            menu.addDisabledItem(QStringLiteral("mail-line"), tr("Send a message"), tr("Not taking messages"));
        else
        {
            connect(menu.addItem(QStringLiteral("mail-line"), tr("Send a message")), &QAction::triggered, this,
                    [this, userId]() { openPrivateMessage(userId); });
        }
    }
    connect(menu.addItem(QStringLiteral("at-line"), tr("Mention in chat")), &QAction::triggered, this,
            [this, name]() { startMention(name); });

    if (hasRoom && !inMyRoom && m_currentRoomId == 0)
    {
        const quint64 roomId = user.currentRoomId;
        if (theirRoom->state == QLatin1String("in_game"))
        {
            const bool canWatch = theirRoom->broadcasting && theirRoom->matchId != 0 &&
                                  !localRomPathForMd5(theirRoom->romMd5).isEmpty();
            if (canWatch)
            {
                connect(menu.addItem(QStringLiteral("eye-line"), tr("Watch match")), &QAction::triggered, this,
                        [this, roomId]() { onMatchWatchRequested(roomId); });
            }
        }
        else if (!theirRoom->privateRoom)
        {
            connect(menu.addItem(QStringLiteral("login-box-line"), tr("Join room")), &QAction::triggered, this,
                    [this, roomId]() { onRoomJoinRequested(roomId); });
        }
    }

    menu.addSeparator();
    const bool ignored = isIgnored(name);
    connect(menu.addItem(QStringLiteral("forbid-line"), ignored ? tr("Stop ignoring") : tr("Ignore")),
            &QAction::triggered, this, [this, name, ignored]() { setIgnored(name, !ignored); });

    // The host's kick, for someone seated in their room.
    if (inMyRoom && m_currentRoomHostId == m_client->selfUserId())
    {
        menu.addHeading(QStringLiteral("vip-crown-fill"), tr("Your room"), false);
        connect(menu.addDangerItem(QStringLiteral("user-unfollow-line"), tr("Remove from room")),
                &QAction::triggered, this, [this, userId, name]() {
            if (QMessageBox::question(this, tr("Remove player"),
                    QString(tr("Remove %1 from the match?")).arg(name),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                m_client->kickFromRoom(userId);
        });
    }

    if (m_client->isModerator())
    {
        menu.addHeading(QStringLiteral("shield-star-fill"), tr("Moderator"), true);
        if (m_client->isModeratorUser(userId))
        {
            // The server refuses these; saying so here saves the trip.
            menu.addDisabledItem(QStringLiteral("logout-circle-r-line"), tr("Kick from lobby"), tr("Is a moderator"));
            menu.addDisabledItem(QStringLiteral("volume-mute-line"), tr("Mute"), QString());
            menu.addDisabledItem(QStringLiteral("auction-line"), tr("Ban…"), QString());
        }
        else
        {
            connect(menu.addItem(QStringLiteral("logout-circle-r-line"), tr("Kick from lobby")),
                    &QAction::triggered, this, [this, name]() { m_client->sendModAction(QStringLiteral("kick"), name); });
            LobbyMenu* mute = menu.addSubmenu(QStringLiteral("volume-mute-line"), tr("Mute"));
            const std::pair<QString, QString> durations[] = {
                { tr("10 minutes"), QStringLiteral("10m") },
                { tr("1 hour"), QStringLiteral("1h") },
                { tr("1 day"), QStringLiteral("1d") },
                { tr("Permanent"), QString() },
            };
            for (const auto& [label, duration] : durations)
            {
                connect(mute->addItem(QString(), label), &QAction::triggered, this, [this, name, duration]() {
                    m_client->sendModAction(QStringLiteral("mute"), name, duration);
                });
            }
            mute->addSeparator();
            connect(mute->addItem(QString(), tr("With a reason…")), &QAction::triggered, this,
                    [this, name]() { openSanctionDialog(false, name); });
            connect(menu.addDangerItem(QStringLiteral("auction-line"), tr("Ban…")), &QAction::triggered, this,
                    [this, name]() { openSanctionDialog(true, name); });
        }
    }

    menu.exec(globalPos);
}

void RollbackLobbyDialog::showUserMenu()
{
    LobbyMenu menu(this);
    QAction* rename = menu.addItem(QStringLiteral("edit-line"), tr("Change username…"));
    rename->setEnabled(!matchTransportInProgress());
    // Queued: the prompt opens once this menu has closed.
    connect(rename, &QAction::triggered, this, &RollbackLobbyDialog::changeUsername, Qt::QueuedConnection);

    if (m_client->socialSupported())
    {
        // The lobby advertises whether you take challenges. Notification preferences are local.
        auto* challengesRow = new QWidget(&menu);
        auto* challengesLay = new QHBoxLayout(challengesRow);
        challengesLay->setContentsMargins(11, 4, 11, 4);
        auto* challenges = new ToggleSwitch(tr("Allow challenges"), challengesRow);
        challenges->setObjectName("TakeChallenges");
        challenges->setChecked(!m_client->challengesOff());
        connect(challenges, &QCheckBox::toggled, this, [this](bool on) {
            m_client->setChallengesOff(!on);
            QSettings("RMG-K", "n02").setValue(kNoChallengesSetting, !on);
            if (!on)
            {
                // Those already waiting are over too.
                for (const auto& challenge : std::as_const(m_incomingChallenges))
                {
                    m_client->respondToChallenge(challenge.id, false);
                    logIncomingChallenge(challenge, ChallengeLogOutcome::YouDeclined);
                }
                m_incomingChallenges.clear();
            }
            refreshChallengeTray();
        });
        challengesLay->addWidget(challenges);
        menu.addLiveWidget(challengesRow);

        // Only where the lobby can hold back messages for us.
        if (m_client->pmSettingsSupported())
        {
            auto* messagesRow = new QWidget(&menu);
            auto* messagesLay = new QHBoxLayout(messagesRow);
            messagesLay->setContentsMargins(11, 4, 11, 4);
            auto* messages = new ToggleSwitch(tr("Allow private messages"), messagesRow);
            messages->setObjectName("AllowPrivateMessages");
            messages->setToolTip(tr("Off: only players you've written to in the last 10 minutes, and moderators, can message you."));
            messages->setChecked(!m_client->privateMessagesOff());
            connect(messages, &QCheckBox::toggled, this, [this](bool on) {
                m_client->setPrivateMessagesOff(!on);
                QSettings("RMG-K", "n02").setValue(kNoPrivateMessagesSetting, !on);
            });
            messagesLay->addWidget(messages);
            menu.addLiveWidget(messagesRow);
        }

        auto* row = new QWidget(&menu);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(11, 4, 11, 4);
        auto* toggle = new ToggleSwitch(tr("Notifications"), row);
        toggle->setObjectName("LobbyNotifications");
        toggle->setChecked(!m_doNotDisturb);
        connect(toggle, &QCheckBox::toggled, this, [this](bool on) {
            m_doNotDisturb = !on;
            QSettings("RMG-K", "n02").setValue(kDoNotDisturbSetting, m_doNotDisturb);
        });
        rowLay->addWidget(toggle);
        menu.addLiveWidget(row);

        // Separate from the above: badges and the taskbar flash stay, the chime goes.
        auto* soundsRow = new QWidget(&menu);
        auto* soundsLay = new QHBoxLayout(soundsRow);
        soundsLay->setContentsMargins(11, 4, 11, 4);
        auto* sounds = new ToggleSwitch(tr("Notification sounds"), soundsRow);
        sounds->setObjectName("LobbyNotificationSounds");
        sounds->setToolTip(tr("Off: notifications still show, without a sound."));
        sounds->setChecked(!m_noSounds);
        connect(sounds, &QCheckBox::toggled, this, [this](bool on) {
            m_noSounds = !on;
            QSettings("RMG-K", "n02").setValue(kNoSoundsSetting, m_noSounds);
        });
        soundsLay->addWidget(sounds);
        menu.addLiveWidget(soundsRow);

        connect(menu.addItem(QStringLiteral("forbid-line"), tr("Ignored players…")), &QAction::triggered,
                this, &RollbackLobbyDialog::showIgnoredPlayers);
    }

    // A few of the settings, here as well as in Settings: the same stored
    // values under the same names, taking effect the way they do there.
    menu.addSeparator();
    const auto addSetting = [&](const char* objectName, const QString& text, const QString& tip, SettingsID id,
                                bool enabled, bool reloadOsd) {
        auto* row = new QWidget(&menu);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(11, 4, 11, 4);
        auto* toggle = new ToggleSwitch(text, row);
        toggle->setObjectName(QString::fromLatin1(objectName));
        toggle->setToolTip(tip);
        toggle->setChecked(CoreSettingsGetBoolValue(id));
        toggle->setEnabled(enabled);
        connect(toggle, &QCheckBox::toggled, this, [id, reloadOsd](bool on) {
            CoreSettingsSetValue(id, on);
            CoreSettingsSave();
            if (reloadOsd)
                OnScreenDisplayLoadSettings();
        });
        rowLay->addWidget(toggle);
        menu.addLiveWidget(row);
    };
    const char* const settingsContext = "UserInterface::Dialog::SettingsDialog";
    addSetting("HideLocationSwitch", QCoreApplication::translate(settingsContext, "Hide my location from other players"),
               QCoreApplication::translate(settingsContext,
                   "Your row in the netplay lobby shows no country flag, and no country in its\n"
                   "hover tooltip, for everyone on the server. Region-based ping estimates still\n"
                   "work. Players you actually play with still exchange connection addresses —\n"
                   "this hides the flag, not your traffic. Applies the next time you connect."),
               SettingsID::Rollback_HideLocation, true, false);
    addSetting("RememberInputDelaySwitch", QCoreApplication::translate(settingsContext, "Remember my last input delay"),
               QCoreApplication::translate(settingsContext,
                   "Reuse your last frame delay choice when hosting or joining a rollback lobby room,\n"
                   "including Quick Match. Choosing Auto remembers Auto instead of its calculated value.\n"
                   "When disabled, each new room starts on Auto. Applies the next time you enter a room."),
               SettingsID::Rollback_RememberInputDelay, true, false);
    // On the settings page these two sit with the on-screen display, and are
    // off limits while it's off.
    const bool osd = CoreSettingsGetBoolValue(SettingsID::GUI_OnScreenDisplayEnabled);
    addSetting("PortLabelsSwitch", QCoreApplication::translate("SettingsDialog", "Show Kaillera port labels"), QString(),
               SettingsID::GUI_OnScreenDisplayKailleraPortLabels, osd, true);
    addSetting("ViewerCountSwitch", QCoreApplication::translate("SettingsDialog", "Show live replay viewer count"),
               QCoreApplication::translate("SettingsDialog",
                   "Show the live replay viewer count in the top-left corner while playing or watching a match."),
               SettingsID::GUI_OnScreenDisplayLiveReplayStatus, osd, true);

    // How the lobby looks on this PC. Picking one leaves the menu open, so the
    // change shows straight away.
    menu.addHeading(QStringLiteral("palette-line"), tr("Theme"), false);
    auto* themes = new QActionGroup(&menu);
    const auto addTheme = [&](const QString& iconName, const QString& text, LobbyTheme::ThemeChoice choice) {
        QAction* item = menu.addCheckItem(iconName, text, LobbyTheme::themeChoice() == choice, themes);
        connect(item, &QAction::toggled, this, [this, choice](bool on) {
            if (on)
                setLobbyTheme(choice);
        });
    };
    addTheme(QStringLiteral("computer-line"), tr("Same as the app"), LobbyTheme::ThemeChoice::App);
    addTheme(QStringLiteral("moon-line"), tr("Dark"), LobbyTheme::ThemeChoice::Dark);
    addTheme(QStringLiteral("sun-line"), tr("Light"), LobbyTheme::ThemeChoice::Light);

    menu.addHeading(QStringLiteral("layout-column-line"), tr("Layout"), false);
    auto* layouts = new QActionGroup(&menu);
    const auto pickLayout = [this](bool stacked) {
        setBrowseStacked(stacked);
        // An open room window covers them: minimize it so the change shows.
        if (roomWindowOpen())
            switchToRoomsView();
    };
    const bool stacked = m_browseSplitter && m_browseSplitter->orientation() == Qt::Vertical;
    QAction* side = menu.addCheckItem(QStringLiteral("layout-column-line"), tr("Side by side"), !stacked, layouts);
    connect(side, &QAction::toggled, this, [pickLayout](bool on) { if (on) pickLayout(false); });
    QAction* above = menu.addCheckItem(QStringLiteral("layout-row-line"), tr("Vertical"), stacked, layouts);
    connect(above, &QAction::toggled, this, [pickLayout](bool on) { if (on) pickLayout(true); });
    // Not a third layout: either one can draw its lists a line a row.
    menu.addSeparator();
    QAction* minimal = menu.addCheckItem(QStringLiteral("menu-line"), tr("Minimal"),
                                         m_liveGallery && m_liveGallery->isMinimal());
    connect(minimal, &QAction::toggled, this, [this](bool on) {
        QSettings("RMG-K", "n02").setValue(kMinimalListsSetting, on);
        m_liveGallery->setMinimal(on);
        m_roomsColumn->setMinimal(on);
    });
    QAction* split = menu.addCheckItem(QStringLiteral("layout-row-line"), tr("Split chat"), m_chatSplitOn);
    connect(split, &QAction::toggled, this, [this](bool on) { setChatSplit(on); });
    menu.addSeparator();
    m_panelLayout->addMenuItems(menu);

    if (m_client->isModerator())
    {
        menu.addHeading(QStringLiteral("shield-star-fill"), tr("Moderator"), true);
        connect(menu.addItem(QStringLiteral("shield-star-line"), tr("Moderation…")), &QAction::triggered,
                this, &RollbackLobbyDialog::showModerationDialog);
        connect(menu.addItem(QStringLiteral("logout-box-r-line"), tr("Sign out of moderation")), &QAction::triggered,
                this, [this]() { m_client->sendAdminLogout(); });
    }
    menu.exec(m_userChip->mapToGlobal(QPoint(0, m_userChip->height())));
}

// ──────────────────────────────────────────────────────────────────────
//  Ignoring
// ──────────────────────────────────────────────────────────────────────

bool RollbackLobbyDialog::isIgnored(const QString& name) const
{
    return m_ignored.contains(name, Qt::CaseInsensitive);
}

void RollbackLobbyDialog::setIgnored(const QString& name, bool ignored)
{
    m_ignored.removeIf([&name](const QString& other) { return other.compare(name, Qt::CaseInsensitive) == 0; });
    if (ignored)
        m_ignored.append(name);
    QSettings("RMG-K", "n02").setValue(kIgnoredSetting, m_ignored);
}

void RollbackLobbyDialog::showIgnoredPlayers()
{
    auto* dialog = new IgnoredPlayersDialog(m_ignored, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &IgnoredPlayersDialog::unignoreRequested, this,
            [this](const QString& name) { setIgnored(name, false); });
    dialog->open();
}

// ──────────────────────────────────────────────────────────────────────
//  Challenges
// ──────────────────────────────────────────────────────────────────────

QString RollbackLobbyDialog::challengeUnavailableReason(const LobbyClient::LobbyUser& user) const
{
    if (!user.social)
        return tr("Needs an update");
    if (user.noChallenges)
        return tr("Not taking challenges");
    if (user.state == QLatin1String("playing"))
        return tr("In a match");
    if (inLiveMatch())
        return tr("You're playing");
    if (m_outgoingChallenge.id != 0)
        return tr("One at a time");
    // An invite needs a free seat. How many it needs isn't known here: the
    // lobby keeps how many players share a PC to itself.
    if (m_currentRoomId != 0)
    {
        const auto room = m_client->rooms().constFind(m_currentRoomId);
        if (room != m_client->rooms().constEnd() && room->maxPlayers > 0 && room->players >= room->maxPlayers)
            return tr("Room is full");
    }
    return QString();
}

void RollbackLobbyDialog::startChallenge(quint64 userId, const QString& preferredMd5)
{
    const auto found = m_client->users().constFind(userId);
    if (found == m_client->users().constEnd() || m_client->isOnThisPc(userId) || m_outgoingChallenge.id != 0)
        return;

    // From a room it's an invite to that room, so there's nothing to set up.
    if (m_currentRoomId != 0)
    {
        m_challengeTargetName = found->username;
        m_challengeTargetGame = m_currentRoomGame;
        m_challengeTargetMd5 = m_currentRoomMd5;
        m_client->sendChallenge(userId, QString(), QString(), QString(), false);
        return;
    }

    const auto selfUser = m_client->users().constFind(m_client->selfUserId());
    ChallengeDialogPlayer self;
    self.name = m_username;
    if (selfUser != m_client->users().constEnd())
    {
        self.name = selfUser->username;
        self.country = selfUser->anonymous ? QString() : selfUser->country;
        self.connection = selfUser->connection;
    }
    ChallengeDialogPlayer target;
    target.name = found->username;
    target.country = found->anonymous ? QString() : found->country;
    target.connection = found->connection;
    target.pingMs = m_client->measuredPingMs(userId);
    target.pingEstimate = target.pingMs < 0;
    if (target.pingEstimate)
    {
        const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), found->region);
        target.pingMs = estimate > 0 ? estimate : -1;
    }

    QList<QVariantMap> games;
    for (int i = 0; i < m_browseRomCombo->count(); ++i)
    {
        const QVariantMap game = m_browseRomCombo->itemData(i).toMap();
        if (!game.isEmpty())
            games << game;
    }

    // Challenging someone back starts on the game they wanted, if this PC has it.
    const QString selected = !preferredMd5.isEmpty() && !localRomPathForMd5(preferredMd5).isEmpty()
        ? preferredMd5 : selectedBrowseRom().value("md5").toString();
    auto* dialog = new ChallengeDialog(self, target, games, selected, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setCoverLookup([this](const QString& md5) { return coverPathForMd5(md5); });
    connect(m_client, &LobbyClient::sharedGamesReceived, dialog, [dialog, userId](quint64 to, const QStringList& md5s) {
        if (to == userId)
            dialog->setSharedGames(md5s);
    });
    const QString targetName = found->username;
    connect(dialog, &QDialog::accepted, this, [this, dialog, userId, targetName]() {
        m_challengeTargetName = targetName;
        m_challengeTargetGame = dialog->romName();
        m_challengeTargetMd5 = dialog->romMd5();
        m_client->sendChallenge(userId, dialog->romName(), dialog->romMd5(), dialog->note(), dialog->liveReplay());
    });
    m_client->requestSharedGames(userId);
    m_challengeDialog = dialog;
    dialog->open();
}

void RollbackLobbyDialog::onChallengeSent(const LobbyClient::Challenge& challenge)
{
    m_challengeTray->clearOutcome();
    m_outgoingChallenge = challenge;
    expireChallengeLater(challenge.id, challenge.expiresAtMs);
    if (challenge.invite)
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("Invited <b>%1</b> to the room").arg(challenge.toUsername.toHtmlEscaped()),
                             QStringLiteral("mail-send-line"));
    }
    refreshPlayBar();
    refreshChallengeTray();
}

void RollbackLobbyDialog::onChallengeIncoming(const LobbyClient::Challenge& challenge)
{
    // The sender just sees it go unanswered, so they can't tell they're ignored.
    // With challenges off, only an older lobby still sends them.
    if (m_client->challengesOff() || isIgnored(challenge.fromUsername))
        return;
    // With notifications off it goes in the log and nowhere else: no card, no
    // sound, nothing over the game.
    if (m_doNotDisturb)
    {
        logIncomingChallenge(challenge, ChallengeLogOutcome::WhileNotificationsOff);
        return;
    }
    m_incomingChallenges.append(challenge);
    expireChallengeLater(challenge.id, challenge.expiresAtMs);
    // Only one this PC shows gets measured: an ignored sender never gets an
    // ICE connection to it.
    m_client->measureChallengePing(challenge);
    refreshChallengeTray();
    alertFrom(challenge.fromUserId);
    // Shown only if a game is running: someone watching can't see the lobby behind it.
    emit overlayMessage(tr("%1 challenges you. Open the lobby to answer.").arg(challenge.fromUsername));
}

void RollbackLobbyDialog::onChallengeMissed(const LobbyClient::Challenge& challenge, const QString& reason)
{
    // Turned away while this PC played: only the badge and the log say so. No
    // card, no sound, nothing over the game.
    if ((!reason.isEmpty() && reason != QLatin1String("busy")) || m_client->challengesOff() ||
        isIgnored(challenge.fromUsername))
        return;
    logIncomingChallenge(challenge, ChallengeLogOutcome::WhilePlaying);
}

QList<ChallengeCardView> RollbackLobbyDialog::incomingChallengeViews()
{
    QList<ChallengeCardView> views;
    const auto& users = m_client->users();
    for (const LobbyClient::Challenge& challenge : std::as_const(m_incomingChallenges))
    {
        ChallengeCardView view;
        view.challengeId = challenge.id;
        view.invite = challenge.invite;
        view.fromName = challenge.fromUsername;
        if (const auto from = users.constFind(challenge.fromUserId); from != users.constEnd())
        {
            view.fromCountry = from->anonymous ? QString() : from->country;
            view.fromConnection = from->connection;
            view.fromPingMs = challengePingMs(challenge, false, &view.fromPingEstimate);
        }

        // Everyone on this PC plays. Only this PC's screen ever says so.
        const bool forSelf = challenge.toUserId == m_client->selfUserId();
        QStringList others;
        if (!forSelf)
            others << lobbyUserName(m_client->selfUserId());
        for (const auto& local : m_client->localPlayers())
        {
            if (local.userId != challenge.toUserId)
                others << local.name;
        }
        if (!others.isEmpty())
        {
            const QString forWhom = forSelf ? tr("For you") : tr("For %1").arg(challenge.toUsername);
            const QString alsoPlaying = others.size() == 1 ? tr("%1 plays too").arg(others.first())
                                                           : tr("%1 play too").arg(others.join(QStringLiteral(", ")));
            view.forDetail = forWhom + QStringLiteral(" · ") + alsoPlaying;
        }

        view.roomPlayers = challenge.roomPlayers;
        view.roomMaxPlayers = challenge.roomMaxPlayers;
        view.game = challenge.romName;
        view.coverPath = coverPathForMd5(challenge.romMd5);
        view.gameDetail = challenge.invite ? challenge.roomName
            : challenge.note.isEmpty() ? QString() : QStringLiteral("“%1”").arg(challenge.note);
        view.expiresAtMs = challenge.expiresAtMs;
        view.durationMs = 30'000;

        view.canAccept = !localRomPathForMd5(challenge.romMd5).isEmpty();
        if (!view.canAccept)
            view.warning = tr("You don't have this game");
        else if (m_currentRoomId != 0)
        {
            const auto room = m_client->rooms().constFind(m_currentRoomId);
            view.warning = room != m_client->rooms().constEnd() ? tr("Accepting leaves %1").arg(room->name)
                                                                : tr("Accepting leaves your room");
        }
        else if (m_quickMatchActive)
            view.warning = tr("Accepting stops your search");
        else if (m_spectatingMatchId != 0)
            view.warning = tr("Accepting stops watching");
        views << view;
    }
    return views;
}

OutgoingChallengeView RollbackLobbyDialog::outgoingChallengeView()
{
    OutgoingChallengeView view;
    if (m_outgoingChallenge.id == 0)
        return view;
    view.challengeId = m_outgoingChallenge.id;
    view.title = m_outgoingChallenge.invite ? tr("Invite sent") : tr("Challenge sent");
    view.waitingFor = tr("Waiting for %1").arg(m_outgoingChallenge.toUsername);
    view.toName = m_outgoingChallenge.toUsername;
    const auto to = m_client->users().constFind(m_outgoingChallenge.toUserId);
    if (to != m_client->users().constEnd() && !to->anonymous)
        view.toCountry = to->country;
    // Their ping, measured once the lobby's ping check lands, estimated till then.
    view.pingMs = challengePingMs(m_outgoingChallenge, true, &view.pingEstimate);
    view.game = m_outgoingChallenge.romName;
    view.coverPath = coverPathForMd5(m_outgoingChallenge.romMd5);
    view.expiresAtMs = m_outgoingChallenge.expiresAtMs;
    view.durationMs = 30'000;
    return view;
}

int RollbackLobbyDialog::challengePingMs(const LobbyClient::Challenge& challenge, bool outgoing, bool* estimate) const
{
    // The lobby's ping check measures under the id this PC knows them by.
    const quint64 other = outgoing ? challenge.toUserId : challenge.fromUserId;
    const quint64 pingId = challenge.pingPeerUserId != 0 ? challenge.pingPeerUserId : other;
    const int measured = m_client->measuredPingMs(pingId);
    *estimate = measured < 0;
    if (measured >= 0)
        return measured;
    const auto user = m_client->users().constFind(other);
    if (user == m_client->users().constEnd())
        return -1;
    const int guess = LobbyRegions::estimatedRttMs(m_client->selfRegion(), user->region);
    return guess > 0 ? guess : -1;
}

void RollbackLobbyDialog::answerChallenge(quint64 challengeId, bool accept)
{
    const auto found = std::find_if(m_incomingChallenges.cbegin(), m_incomingChallenges.cend(),
        [challengeId](const LobbyClient::Challenge& challenge) { return challenge.id == challengeId; });
    if (found == m_incomingChallenges.cend())
        return;
    const LobbyClient::Challenge challenge = *found;
    m_incomingChallenges.removeAt(int(found - m_incomingChallenges.cbegin()));
    if (accept)
    {
        stopWatchingForRoom();
        cancelPendingPingCheck();
        // Logged once the lobby says whether it went ahead.
        m_acceptedChallenge = challenge;
    }
    else
    {
        logIncomingChallenge(challenge, ChallengeLogOutcome::YouDeclined);
    }
    m_client->respondToChallenge(challenge.id, accept);
    refreshChallengeTray();
}

void RollbackLobbyDialog::cancelOutgoingChallenge()
{
    if (m_outgoingChallenge.id == 0)
        return;
    // Not waiting for the reply: there's none if the challenge has already ended.
    m_client->cancelChallenge(m_outgoingChallenge.id);
    m_outgoingChallenge = LobbyClient::Challenge{};
    refreshPlayBar();
    refreshChallengeTray();
}

// The lobby's word on how a challenge ended can go missing: its queue to this PC was
// full, or it overtook the message that started the challenge. Nothing else would end it.
void RollbackLobbyDialog::expireChallengeLater(quint64 challengeId, qint64 expiresAtMs)
{
    const qint64 left = expiresAtMs - QDateTime::currentMSecsSinceEpoch();
    QTimer::singleShot(std::max<qint64>(0, left) + kChallengeGraceMs, this, [this, challengeId]() {
        const bool waiting = m_outgoingChallenge.id == challengeId ||
            std::any_of(m_incomingChallenges.cbegin(), m_incomingChallenges.cend(),
                        [challengeId](const LobbyClient::Challenge& challenge) { return challenge.id == challengeId; });
        if (waiting)
            onChallengeEnded(challengeId, 0, QStringLiteral("timeout"));
    });
}

void RollbackLobbyDialog::stopWatchingForRoom()
{
    if (m_spectatingMatchId == 0)
        return;
    m_pendingSpectate = PendingSpectate{};
    emit spectateStreamClosed(QStringLiteral("stopped"));
}

void RollbackLobbyDialog::onChallengeEnded(quint64 challengeId, quint64 toUserId, const QString& result)
{
    // One this PC was asked: the card goes, and the log says how it ended.
    for (int i = 0; i < m_incomingChallenges.size(); ++i)
    {
        if (m_incomingChallenges.at(i).id != challengeId)
            continue;
        const LobbyClient::Challenge challenge = m_incomingChallenges.takeAt(i);
        // Busy: this PC went into a match before answering.
        ChallengeLogOutcome outcome = ChallengeLogOutcome::CouldNotGoAhead;
        if (result == QLatin1String("timeout") || result == QLatin1String("busy"))
            outcome = ChallengeLogOutcome::Missed;
        else if (result == QLatin1String("cancelled"))
            outcome = ChallengeLogOutcome::Withdrawn;
        logIncomingChallenge(challenge, outcome);
        refreshChallengeTray();
        return;
    }
    const bool mine = challengeId != 0 && challengeId == m_outgoingChallenge.id;
    if (challengeId != 0 && !mine)
    {
        // This PC's answer got there first, and saying yes landed in the room.
        if (challengeId == m_acceptedChallenge.id)
        {
            const LobbyClient::Challenge accepted = m_acceptedChallenge;
            m_acceptedChallenge = LobbyClient::Challenge{};
            const QString from = accepted.fromUsername.toHtmlEscaped();
            if (result == QLatin1String("accepted"))
            {
                appendChatSystemLine(CHANNEL_ROOM,
                    (accepted.invite ? tr("You accepted <b>%1</b>'s invite")
                                     : tr("You accepted <b>%1</b>'s challenge")).arg(from),
                    QStringLiteral("sword-fill"));
                logIncomingChallenge(accepted, ChallengeLogOutcome::YouAccepted);
            }
            else
            {
                // Too late: they went into a match or left while this was on its way.
                appendChatSystemLine(CHANNEL_LOBBY, tr("<b>%1</b>'s challenge can't go ahead any more.").arg(from),
                                     QStringLiteral("sword-fill"));
                logIncomingChallenge(accepted, ChallengeLogOutcome::CouldNotGoAhead);
            }
        }
        return;
    }

    QString name = m_challengeTargetName;
    if (mine)
        name = m_outgoingChallenge.toUsername;
    else if (name.isEmpty())
        name = lobbyUserName(toUserId);
    const bool invite = mine ? m_outgoingChallenge.invite : m_currentRoomId != 0;
    ChallengeLogEntry entry;
    entry.name = name;
    entry.incoming = false;
    entry.invite = invite;
    entry.game = mine ? m_outgoingChallenge.romName : m_challengeTargetGame;
    entry.md5 = mine ? m_outgoingChallenge.romMd5 : m_challengeTargetMd5;
    if (invite)
    {
        const auto room = m_client->rooms().constFind(m_currentRoomId);
        if (room != m_client->rooms().constEnd())
            entry.roomName = room->name;
    }
    if (mine)
        m_outgoingChallenge = LobbyClient::Challenge{};

    if (result == QLatin1String("accepted"))
    {
        if (!invite)
            stopWatchingForRoom();
        appendChatSystemLine(CHANNEL_ROOM,
            (invite ? tr("<b>%1</b> accepted your invite") : tr("<b>%1</b> accepted your challenge")).arg(name.toHtmlEscaped()),
            QStringLiteral("sword-fill"));
        logChallenge(entry, ChallengeLogOutcome::Accepted);
        refreshPlayBar();
        refreshChallengeTray();
        return;
    }
    // Called off from here, or one was already out: nothing to say or keep.
    if (result == QLatin1String("cancelled") || result == QLatin1String("pending"))
    {
        refreshPlayBar();
        refreshChallengeTray();
        return;
    }

    QString title;
    QString detail;
    Role role = Role::Muted;
    QString icon = QStringLiteral("close-circle-line");
    ChallengeLogOutcome outcome = ChallengeLogOutcome::Unavailable;
    if (result == QLatin1String("declined"))
    {
        title = tr("%1 declined").arg(name);
        detail = tr("You can challenge them again in a minute.");
        outcome = ChallengeLogOutcome::Declined;
    }
    else if (result == QLatin1String("cooldown"))
    {
        title = tr("%1 declined a moment ago").arg(name);
        detail = tr("You can challenge them again in a minute.");
        outcome = ChallengeLogOutcome::DeclinedRecently;
    }
    else if (result == QLatin1String("timeout"))
    {
        title = tr("No answer");
        detail = tr("%1 didn't answer in time.").arg(name);
        icon = QStringLiteral("time-line");
        outcome = ChallengeLogOutcome::NoAnswer;
    }
    else if (result == QLatin1String("busy"))
    {
        title = tr("%1 is busy").arg(name);
        detail = tr("They've just started a match. You can watch it from Live now.");
        role = Role::Warn;
        icon = QStringLiteral("gamepad-line");
        outcome = ChallengeLogOutcome::InAMatch;
    }
    else if (result == QLatin1String("challenges_off"))
    {
        title = tr("%1 isn't taking challenges").arg(name);
        detail = tr("They turned challenges off. You can still send them a message.");
        icon = QStringLiteral("forbid-line");
        outcome = ChallengeLogOutcome::NotTakingChallenges;
    }
    else if (result == QLatin1String("no_game"))
    {
        title = tr("%1 doesn't have that game").arg(name);
        detail = tr("Try a game you both have.");
        role = Role::Warn;
        icon = QStringLiteral("error-warning-line");
        outcome = ChallengeLogOutcome::NoGame;
    }
    else if (result == QLatin1String("blocked_note"))
    {
        title = tr("Message blocked");
        detail = tr("The chat filter blocked your message. Try again without it.");
        role = Role::Warn;
        icon = QStringLiteral("error-warning-line");
        outcome = ChallengeLogOutcome::NoteBlocked;
    }
    else if (result == QLatin1String("needs_update"))
    {
        title = tr("%1 needs an update").arg(name);
        detail = tr("Their version of RMG-K can't receive challenges.");
        outcome = ChallengeLogOutcome::NeedsUpdate;
    }
    else if (result == QLatin1String("too_fast"))
    {
        title = tr("That's a lot of challenges");
        detail = tr("Try again in a moment.");
        outcome = ChallengeLogOutcome::TooFast;
    }
    else
    {
        title = tr("Can't challenge right now");
        detail = tr("%1 can't take a challenge at the moment.").arg(name);
    }
    logChallenge(entry, outcome);

    // The room's chat says it too, where the room window covers the lobby.
    if (m_currentRoomId != 0)
        appendChatSystemLine(CHANNEL_ROOM, QStringLiteral("%1. %2").arg(title.toHtmlEscaped(), detail.toHtmlEscaped()), icon);
    m_challengeTray->showOutcome(title, detail, role, icon);
    refreshPlayBar();
    refreshChallengeTray();
}

void RollbackLobbyDialog::positionStageOverlays()
{
    if (!m_stage || !m_toast)
        return;
    m_toast->move(14, m_stage->height() - m_toast->height() - 14);
}

// ──────────────────────────────────────────────────────────────────────
//  The challenges tray and its log
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::scheduleChallengeTrayRefresh()
{
    if (m_challengeTrayRefreshQueued)
        return;
    m_challengeTrayRefreshQueued = true;
    QTimer::singleShot(0, this, &RollbackLobbyDialog::refreshChallengeTray);
}

void RollbackLobbyDialog::refreshChallengeTray()
{
    m_challengeTrayRefreshQueued = false;
    if (!m_challengeTray || !m_challengesButton || !m_client)
        return;
    const bool available = m_client->state() == LobbyClient::ConnectionState::Connected && m_client->socialSupported();
    m_challengesButton->setVisible(available);
    m_challengesButton->setUnseen(unseenChallengeCount());
    if (!available && m_challengeTray->isLogOpen())
    {
        m_challengeTray->setLogOpen(false);
        m_challengesButton->setOpen(false);
        m_freshLogKeys.clear();
    }
    m_challengeTray->setIncoming(incomingChallengeViews());
    m_challengeTray->setOutgoing(outgoingChallengeView());
    if (m_challengeTray->isLogOpen())
        m_challengeTray->setLogRows(challengeLogRows());
    m_challengeTray->setAvailable(available);
}

int RollbackLobbyDialog::unseenChallengeCount() const
{
    int unseen = 0;
    for (const ChallengeLogEntry& entry : ChallengeLog::session().entries())
    {
        if (!isIgnored(entry.name))
            unseen += entry.unseen;
    }
    return unseen;
}

QString RollbackLobbyDialog::challengeOutcomeText(ChallengeLogOutcome outcome, Role* role) const
{
    *role = Role::Muted;
    switch (outcome)
    {
        case ChallengeLogOutcome::Missed:
            *role = Role::Warn;
            return tr("Missed");
        case ChallengeLogOutcome::WhilePlaying:
            *role = Role::Warn;
            return tr("While you were playing");
        case ChallengeLogOutcome::WhileNotificationsOff:
            *role = Role::Warn;
            return tr("While notifications were off");
        case ChallengeLogOutcome::Withdrawn:
            return tr("Withdrawn");
        case ChallengeLogOutcome::YouDeclined:
            return tr("You declined");
        case ChallengeLogOutcome::YouAccepted:
            *role = Role::Ok;
            return tr("You accepted");
        case ChallengeLogOutcome::CouldNotGoAhead:
            return tr("Couldn't go ahead");
        case ChallengeLogOutcome::Accepted:
            *role = Role::Ok;
            return tr("Accepted");
        case ChallengeLogOutcome::Declined:
            return tr("Declined");
        case ChallengeLogOutcome::DeclinedRecently:
            return tr("Declined a moment ago");
        case ChallengeLogOutcome::NoAnswer:
            return tr("No answer");
        case ChallengeLogOutcome::InAMatch:
            *role = Role::Warn;
            return tr("In a match");
        case ChallengeLogOutcome::NotTakingChallenges:
            return tr("Not taking challenges");
        case ChallengeLogOutcome::NoGame:
            *role = Role::Warn;
            return tr("Doesn't have the game");
        case ChallengeLogOutcome::NoteBlocked:
            *role = Role::Warn;
            return tr("Message blocked");
        case ChallengeLogOutcome::NeedsUpdate:
            return tr("Needs an update");
        case ChallengeLogOutcome::TooFast:
            return tr("Too many challenges");
        case ChallengeLogOutcome::Unavailable:
            return tr("Couldn't challenge");
    }
    return QString();
}

QList<ChallengeLogRowView> RollbackLobbyDialog::challengeLogRows() const
{
    QList<ChallengeLogRowView> rows;
    const bool inRoom = m_currentRoomId != 0;
    const auto& users = m_client->users();
    for (const ChallengeLogEntry& entry : ChallengeLog::session().entries())
    {
        if (isIgnored(entry.name))
            continue;
        ChallengeLogRowView row;
        row.key = entry.key;
        row.name = entry.name;
        row.incoming = entry.incoming;
        row.game = entry.game;
        row.what = challengeOutcomeText(entry.outcome, &row.whatRole);
        row.timeMs = entry.timeMs;
        row.count = entry.count;
        row.fresh = entry.unseen > 0 || m_freshLogKeys.contains(entry.key);
        // From a room, the same button sends an invite to it.
        row.action = inRoom ? tr("Invite") : entry.incoming ? tr("Challenge back") : tr("Challenge again");

        // By name: ids change when people reconnect.
        const quint64 userId = userIdForName(entry.name);
        const auto user = users.constFind(userId);
        if (userId == 0 || user == users.constEnd())
        {
            row.unavailable = tr("Offline");
        }
        else
        {
            row.country = user->anonymous ? QString() : user->country;
            const bool here = m_client->isOnThisPc(userId);
            if (here)
                row.unavailable = tr("On this PC");
            else if (inRoom && user->currentRoomId == m_currentRoomId)
                row.unavailable = tr("In your room");
            else
                row.unavailable = challengeUnavailableReason(*user);
            row.canMessage = !here && user->social && canMessage(*user);
        }

        QStringList tip;
        if (entry.invite && !entry.roomName.isEmpty())
            tip << tr("Room invite: %1").arg(entry.roomName);
        if (!entry.game.isEmpty())
            tip << entry.game;
        if (!entry.forName.isEmpty())
            tip << tr("For %1").arg(entry.forName);
        tip << QDateTime::fromMSecsSinceEpoch(entry.timeMs).toString(QStringLiteral("hh:mm:ss"));
        row.tip = tip.join(QLatin1Char('\n'));
        rows << row;
    }
    return rows;
}

void RollbackLobbyDialog::logIncomingChallenge(const LobbyClient::Challenge& challenge, ChallengeLogOutcome outcome)
{
    ChallengeLogEntry entry;
    entry.name = challenge.fromUsername;
    entry.incoming = true;
    entry.invite = challenge.invite;
    entry.game = challenge.romName;
    entry.md5 = challenge.romMd5;
    entry.roomName = challenge.roomName;
    if (challenge.toUserId != 0 && challenge.toUserId != m_client->selfUserId())
        entry.forName = challenge.toUsername;
    logChallenge(entry, outcome);
}

void RollbackLobbyDialog::logChallenge(ChallengeLogEntry entry, ChallengeLogOutcome outcome)
{
    if (entry.name.isEmpty() || isIgnored(entry.name))
        return;
    entry.outcome = outcome;
    entry.timeMs = QDateTime::currentMSecsSinceEpoch();
    // Seen as it happens only by someone looking at the open log.
    const bool looking = m_challengeTray->isLogOpen() && isActiveWindow();
    ChallengeLog::session().add(entry, looking);
    scheduleChallengeTrayRefresh();
}

void RollbackLobbyDialog::toggleChallengeLog()
{
    if (m_challengeTray->isLogOpen())
        closeChallengeLog();
    else
        openChallengeLog();
}

void RollbackLobbyDialog::openChallengeLog()
{
    // What nobody had seen stays marked while the log is open, and counts as seen now.
    m_freshLogKeys.clear();
    for (const ChallengeLogEntry& entry : ChallengeLog::session().entries())
    {
        if (entry.unseen > 0)
            m_freshLogKeys.insert(entry.key);
    }
    ChallengeLog::session().markSeen();
    dismissMissedChallengesSummary();
    m_challengeTray->setLogOpen(true);
    m_challengesButton->setOpen(true);
    refreshChallengeTray();
}

void RollbackLobbyDialog::closeChallengeLog()
{
    m_challengeTray->setLogOpen(false);
    onChallengeLogClosed();
}

void RollbackLobbyDialog::onChallengeLogClosed()
{
    m_challengesButton->setOpen(false);
    m_freshLogKeys.clear();
    refreshChallengeTray();
}

void RollbackLobbyDialog::onChallengeLogAction(quint64 key)
{
    const auto& entries = ChallengeLog::session().entries();
    const auto entry = std::find_if(entries.cbegin(), entries.cend(),
                                    [key](const ChallengeLogEntry& other) { return other.key == key; });
    if (entry == entries.cend())
        return;
    const quint64 userId = userIdForName(entry->name);
    const auto user = m_client->users().constFind(userId);
    // The row's button only shows when this would work, but presence can change under a click.
    if (userId == 0 || user == m_client->users().constEnd() || m_client->isOnThisPc(userId) ||
        !challengeUnavailableReason(*user).isEmpty())
    {
        refreshChallengeTray();
        return;
    }
    startChallenge(userId, entry->md5);
}

void RollbackLobbyDialog::onChallengeLogMessage(quint64 key)
{
    const auto& entries = ChallengeLog::session().entries();
    const auto entry = std::find_if(entries.cbegin(), entries.cend(),
                                    [key](const ChallengeLogEntry& other) { return other.key == key; });
    if (entry == entries.cend())
        return;
    const quint64 userId = userIdForName(entry->name);
    const auto user = m_client->users().constFind(userId);
    if (userId == 0 || user == m_client->users().constEnd() || !user->social || !canMessage(*user))
        return;
    // The conversation opens in the chat, which the log may cover.
    closeChallengeLog();
    openPrivateMessage(userId);
}

void RollbackLobbyDialog::noteSelfPresence()
{
    const auto self = m_client->users().constFind(m_client->selfUserId());
    const bool playing = self != m_client->users().constEnd() && self->state == QLatin1String("playing");
    if (playing == m_selfPlaying)
        return;
    m_selfPlaying = playing;
    if (playing)
    {
        m_playingSinceMs = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    showMissedChallengesSummary();
}

void RollbackLobbyDialog::showMissedChallengesSummary()
{
    // A summary still up from an earlier match takes in this one's too.
    const qint64 since = m_missedSummarySinceMs != 0 ? m_missedSummarySinceMs : m_playingSinceMs;
    int count = 0;
    QStringList names;
    for (const ChallengeLogEntry& entry : ChallengeLog::session().entries())
    {
        if (entry.unseen <= 0 || entry.timeMs < since || isIgnored(entry.name))
            continue;
        count += entry.unseen;
        if (!names.contains(entry.name, Qt::CaseInsensitive))
            names << entry.name;
    }
    if (count == 0)
        return;
    m_missedSummarySinceMs = since;
    // The log lists them newest first; the summary goes in the order they came.
    std::reverse(names.begin(), names.end());
    const QString title = count == 1 ? tr("1 challenge while you were playing")
                                     : tr("%1 challenges while you were playing").arg(count);
    const QString who = names.size() <= kSummaryNames
        ? names.join(QStringLiteral(", "))
        : tr("%1 and %2 more").arg(names.mid(0, kSummaryNames).join(QStringLiteral(", "))).arg(names.size() - kSummaryNames);
    m_challengeTray->setSummary(title, who);
}

void RollbackLobbyDialog::dismissMissedChallengesSummary()
{
    m_missedSummarySinceMs = 0;
    m_challengeTray->setSummary(QString(), QString());
}

// ──────────────────────────────────────────────────────────────────────
//  Private messages
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::openPrivateMessage(quint64 userId)
{
    const auto found = m_client->users().constFind(userId);
    if (found == m_client->users().constEnd() || m_client->isOnThisPc(userId))
        return;
    // Carry on any conversation they're already in with this PC.
    quint64 local = m_privateChats->localFor(userId);
    if (local == 0)
        local = m_client->selfUserId();
    m_privateChats->conversation(local, lobbyUserName(local), userId, found->username, local == m_client->selfUserId());
    refreshPrivateMessageStatus(userId);
    m_privateChats->open(local, userId);
    m_chatTopInput->setFocus();
}

bool RollbackLobbyDialog::canMessage(const LobbyClient::LobbyUser& user) const
{
    if (!user.noPrivateMessages || m_client->isModerator())
        return true;
    const auto wrote = m_pmReceivedMs.constFind(user.id);
    return wrote != m_pmReceivedMs.constEnd() &&
           QDateTime::currentMSecsSinceEpoch() - *wrote < kPmReplyWindowMs;
}

void RollbackLobbyDialog::sendCurrentPrivateMessage(const QString& text)
{
    if (const PrivateConversation* pm = m_privateChats->conversationAt(m_chatTabs->currentIndex()))
        m_client->sendPrivateMessage(pm->remoteId, pm->localId, text);
}

void RollbackLobbyDialog::onPrivateMessage(const LobbyClient::ChatMessage& msg)
{
    const bool outgoing = m_client->isOnThisPc(msg.fromUserId);
    if (!outgoing && !msg.fromAdmin && isIgnored(msg.fromUsername))
        return;
    const quint64 local = outgoing ? msg.fromUserId : msg.toUserId;
    const quint64 remote = outgoing ? msg.toUserId : msg.fromUserId;
    const QString localName = outgoing ? msg.fromUsername : msg.toUsername;
    const QString remoteName = outgoing ? msg.toUsername : msg.fromUsername;

    ChatView* view = m_privateChats->conversation(local, localName, remote, remoteName, local == m_client->selfUserId());
    refreshPrivateMessageStatus(remote);
    const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
    view->addLine(chatMessageHtml(ts, msg.fromUserId, msg.fromUsername, chatBodyHtml(msg.message, nullptr), msg.fromAdmin, outgoing));
    // Its tab shows up if it isn't open, but never takes over the one being read.
    m_privateChats->noteMessage(local, remote, msg.serverTimeMs, !outgoing);
    if (outgoing)
        return;

    m_lastPmLocal = local;
    m_lastPmRemote = remote;
    // They can be answered for a while even if they take no messages.
    m_pmReceivedMs.insert(remote, QDateTime::currentMSecsSinceEpoch());
    const auto sender = m_client->users().constFind(remote);
    if (QTreeWidgetItem* row = m_userItems.value(remote); row && sender != m_client->users().constEnd())
        refreshPlayerRow(row, *sender);
    refreshChatInput();
    const bool looking = isActiveWindow() && m_privateChats->isCurrent(local, remote);
    if (!looking)
        alertFrom(remote);
    // Not roomChatReceived: that also writes the line into a saved replay.
    emit overlayMessage(tr("%1 (private): %2").arg(msg.fromUsername, msg.message));
}

void RollbackLobbyDialog::onPrivateMessageFailed(quint64 toUserId, const QString& reason)
{
    ChatView* view = m_privateChats->existingView(toUserId);
    if (!view)
        return;
    QString why = tr("That message couldn't be delivered.");
    if (reason == QLatin1String("user_gone"))
        why = tr("They've left the lobby, so that message went nowhere.");
    else if (reason == QLatin1String("needs_update"))
        why = tr("Their version of RMG-K can't receive private messages.");
    else if (reason == QLatin1String("pms_off"))
        why = tr("They aren't taking private messages, so that one wasn't delivered.");
    view->addLine(QString("<i style='color:%1;'>%2</i>").arg(LobbyTheme::tokens().muted.name(), why.toHtmlEscaped()));
}

void RollbackLobbyDialog::refreshPrivateMessageStatus(quint64 userId)
{
    const auto found = m_client->users().constFind(userId);
    if (found == m_client->users().constEnd())
        return;
    QString status = stateGlyph(found->state);
    const int measured = m_client->measuredPingMs(userId);
    const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), found->region);
    if (measured >= 0)
        status += tr(" · %1 ms").arg(measured);
    else if (estimate > 0)
        status += tr(" · ~%1 ms").arg(estimate);
    m_privateChats->setRemoteStatus(userId, found->state, status, found->anonymous ? QString() : found->country,
                                    found->social && found->state != QLatin1String("playing"));
    // Whether they take messages may have changed under the open conversation.
    if (const PrivateConversation* pm = m_privateChats->conversationAt(m_chatTabs->currentIndex());
        pm && pm->remoteId == userId)
        refreshChatInput();
}

// ──────────────────────────────────────────────────────────────────────
//  Spectator chat, as a player
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::refreshPlayerSpectatorChat()
{
    if (!m_chatTabs || !m_client)
        return;

    if (m_currentRoomId == 0)
    {
        if (m_playerChatMatchId != 0)
        {
            m_playerChatMatchId = 0;
            m_playerChatLive = false;
            m_spectatorStrip->setVisible(false);
            m_chatViewSpectate->clear();
            setSpectatorChatVisible(false);
        }
        return;
    }
    const auto room = m_client->rooms().constFind(m_currentRoomId);
    if (room == m_client->rooms().constEnd())
        return; // this room's first listing hasn't arrived yet

    const bool live = m_client->spectatorChatSupported() && m_currentMatchId != 0 &&
                      room->broadcasting && room->matchId == m_currentMatchId;
    if (live && m_playerChatMatchId != m_currentMatchId)
    {
        m_playerChatMatchId = m_currentMatchId;
        m_chatViewSpectate->clear();
        m_pendingMentions[kChatTabSpectators].clear();
        setChatTabAvailable(kChatTabSpectators, true);
        refreshSpectatorTab();
        appendChatSystemLine(CHANNEL_SPECTATE, tr("Spectators' comments show up here, and they can see your replies."),
                             QStringLiteral("eye-line"));
        setChatUnread(kChatTabSpectators, 0);
    }
    else if (!live && m_playerChatLive)
    {
        const int unread = m_chatUnread[kChatTabSpectators];
        appendChatSystemLine(CHANNEL_SPECTATE, tr("Spectator chat is closed until your next match."), QStringLiteral("eye-line"));
        setChatUnread(kChatTabSpectators, unread);
    }
    const bool changed = live != m_playerChatLive;
    m_playerChatLive = live;
    m_spectatorStrip->setVisible(m_playerChatMatchId != 0);
    const QString audience = tr("%1 watching").arg(room->viewerCount);
    m_spectatorCount->setText(audience);
    refreshRoomTitleTip();
    if (changed)
    {
        refreshSpectatorTab();
        refreshChatInput();
    }
}

void RollbackLobbyDialog::refreshSpectatorTab()
{
    // Beside Room there's no space for the word.
    const bool icon = m_currentRoomId != 0;
    m_chatTabs->setTabText(kChatTabSpectators, icon ? QString() : tr("Spectators").toUpper());
    m_chatTabs->setTabIcon(kChatTabSpectators, icon
        ? QIcon(LobbyTheme::icon(QStringLiteral("eye-line"), LobbyTheme::tokens().ink2, 14, devicePixelRatioF()))
        : QIcon());
    m_spectatorEye->setColorOverride(LobbyTheme::tokens().stateWatch);

    const bool shared = m_client && m_client->spectatorChatSupported();
    m_chatTabs->setTabToolTip(kChatTabSpectators, m_playerChatMatchId != 0
        ? tr("Everyone watching your match. They can see your replies.")
        : shared ? tr("Everyone watching this match, and its players.")
                 : tr("Everyone watching this match. The players can't see it."));
    // The split chat's pane below names it the same way.
    if (m_chatSplitTab == kChatTabSpectators)
    {
        m_chatSplitTabs->setTabText(0, m_chatTabs->tabText(kChatTabSpectators));
        m_chatSplitTabs->setTabIcon(0, m_chatTabs->tabIcon(kChatTabSpectators));
        m_chatSplitTabs->setTabToolTip(0, m_chatTabs->tabToolTip(kChatTabSpectators));
    }
}

void RollbackLobbyDialog::showSpectatorCommentOverGame(const LobbyClient::ChatMessage& msg)
{
    if (!m_spectatorOverlayToggle->isChecked() || m_client->isOnThisPc(msg.fromUserId))
        return;
    QString text = msg.message.simplified();
    if (text.size() > kOverlayCommentChars)
        text = text.left(kOverlayCommentChars) + QStringLiteral("…");
    // Not roomChatReceived: that also writes the line into a saved replay.
    emit overlayMessage((msg.fromPlayer ? tr("%1: %2") : tr("%1 (watching): %2")).arg(msg.fromUsername, text));
}

// ──────────────────────────────────────────────────────────────────────
//  Mentions
// ──────────────────────────────────────────────────────────────────────

QString RollbackLobbyDialog::chatIconHtml(const QString& icon, const QColor& color, int px) const
{
    // The document needs each tinted icon registered under a name of its own.
    const QString name = QString("lobby-icon://%1/%2").arg(icon, color.name().mid(1));
    const QPixmap pixmap = LobbyTheme::icon(icon, color, px, devicePixelRatioF());
    QList<ChatView*> views = m_privateChats ? m_privateChats->views() : QList<ChatView*>();
    views << m_chatViewLobby << m_chatViewRoom << m_chatViewSpectate;
    for (ChatView* view : views)
    {
        if (view)
            view->document()->addResource(QTextDocument::ImageResource, QUrl(name), pixmap);
    }
    return QString("<img src='%1' width='%2' height='%2' style='vertical-align:middle;'>").arg(name).arg(px);
}

QString RollbackLobbyDialog::chatBodyHtml(const QString& message, bool* mentionsMe) const
{
    if (mentionsMe)
        *mentionsMe = false;

    const Tokens& t = LobbyTheme::tokens();
    QString out;
    int copied = 0;
    for (int at = message.indexOf(QLatin1Char('@')); at >= 0; at = message.indexOf(QLatin1Char('@'), at + 1))
    {
        if (at > 0 && isNameChar(message.at(at - 1)))
            continue;
        // Names can hold . and -, so which one this is takes the online list, and
        // the longest that fits wins. "@Mira." is Mira unless someone is "Mira.".
        const LobbyClient::LobbyUser* best = nullptr;
        for (const auto& user : m_client->users())
        {
            const int length = int(user.username.size());
            if (at + 1 + length > message.size() || (best && length <= best->username.size()))
                continue;
            if (QStringView(message).mid(at + 1, length).compare(user.username, Qt::CaseInsensitive) != 0)
                continue;
            const QChar next = at + 1 + length < message.size() ? message.at(at + 1 + length) : QChar();
            if (next.isLetterOrNumber() || next.isMark() || next.isSurrogate() || next == QLatin1Char('_'))
                continue;
            best = &user;
        }
        if (!best)
            continue;

        const bool here = m_client->isOnThisPc(best->id);
        if (here && mentionsMe)
            *mentionsMe = true;
        out += message.mid(copied, at - copied).toHtmlEscaped();
        const QString token = QStringLiteral("@") + best->username.toHtmlEscaped();
        out += here
            ? QString("<b style='color:%1;'>%2</b>").arg(t.warn.name(), token)
            : QString("<a href='%1' style='color:%2; text-decoration:none;'><b>%3</b></a>")
                  .arg(ChatView::userHref(best->id), t.ink.name(), token);
        copied = at + 1 + int(best->username.size());
        at = copied - 1;
    }
    out += message.mid(copied).toHtmlEscaped();
    return out;
}

QString RollbackLobbyDialog::chatMentionLineHtml(const QString& line) const
{
    // A table because QTextEdit's rich-text subset only fills table cells, not
    // plain blocks. Mixed down to a solid colour for the same reason, and square
    // because it can't round them.
    const Tokens& t = LobbyTheme::tokens();
    return QString("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"background-color:%1;\"><tr>"
                   "<td width=\"3\" style=\"background-color:%2;\"></td>"
                   "<td style=\"padding:3px 4px 3px 6px; color:%3;\">%4</td></tr></table>")
        .arg(LobbyTheme::mix(t.warn, t.panel, 0.13).name(), t.warn.name(), t.ink.name(), line);
}

void RollbackLobbyDialog::noteMention(const QString& channel, const LobbyClient::ChatMessage& msg)
{
    const int tab = channel == QLatin1String(CHANNEL_ROOM) ? kChatTabRoom
        : channel == QLatin1String(CHANNEL_SPECTATE) ? kChatTabSpectators : kChatTabLobby;
    ChatView* view = tab == kChatTabRoom ? m_chatViewRoom : tab == kChatTabSpectators ? m_chatViewSpectate : m_chatViewLobby;

    if (!chatTabShown(tab))
    {
        if (m_chatTabs->isTabVisible(tab))
        {
            m_chatMentioned[tab] = true;
            // appendChatLine counted the line already, except on the lobby's tab.
            setChatUnread(tab, m_chatUnread[tab] + (tab == kChatTabLobby ? 1 : 0));
        }
    }
    else if (view->verticalScrollBar()->value() < view->verticalScrollBar()->maximum())
    {
        m_pendingMentions[tab].append(view->document()->lastBlock().blockNumber());
        refreshMentionJump();
    }
    // Someone playing isn't interrupted: the badge waits for them, and their switch
    // says whether comments reach the game.
    if (tab == kChatTabSpectators && m_spectatingMatchId == 0)
        return;
    alertFrom(msg.fromUserId);
    // Room and spectator chat already reach the in-game overlay. Not roomChatReceived
    // for the lobby: that also writes the line into a saved replay.
    if (channel == QLatin1String(CHANNEL_LOBBY))
        emit overlayMessage(tr("%1: %2").arg(msg.fromUsername, msg.message));
}

void RollbackLobbyDialog::startMention(const QString& name)
{
    // A mention belongs in a channel people are reading, so not a private
    // conversation: the split chat's pane below holds one, else a tab does.
    QLineEdit* input = m_chatInput;
    if (m_privateChats->conversationAt(activeChatTab()))
    {
        if (m_chatSplitTab >= 0)
        {
            input = m_chatSplitInput;
        }
        else
        {
            m_chatTabs->setCurrentIndex(m_currentRoomId != 0 ? kChatTabRoom : kChatTabLobby);
            input = m_chatTopInput;
        }
    }
    input->insert(QStringLiteral("@%1 ").arg(name));
    input->setFocus();
}

void RollbackLobbyDialog::refreshMentionPopup()
{
    if (!m_mentionPopup || !m_chatInput)
        return;

    const QString text = m_chatInput->text();
    const int cursor = m_chatInput->cursorPosition();
    int start = cursor;
    while (start > 0 && isNameChar(text.at(start - 1)))
        --start;
    const bool mention = start > 0 && text.at(start - 1) == QLatin1Char('@') &&
                         (start == 1 || !isNameChar(text.at(start - 2)));
    // "/w " at the start of the box completes a name the same way.
    const bool whisper = !mention && start == 3 && text.startsWith(QLatin1String("/w "));
    if (!(mention || whisper) ||
        (m_privateChats->conversationAt(activeChatTab()) && !whisper))
    {
        m_mentionPopup->hide();
        return;
    }

    const QString typed = text.mid(start, cursor - start);
    const bool inRoomTab = activeChatTab() == kChatTabRoom;
    QList<MentionCandidate> seated;
    QList<MentionCandidate> others;
    for (const auto& user : m_client->users())
    {
        // Players on this PC can be mentioned, but there's no one to message.
        if (user.id == m_client->selfUserId() || (whisper && m_client->isOnThisPc(user.id)) ||
            !user.username.startsWith(typed, Qt::CaseInsensitive))
            continue;
        MentionCandidate candidate;
        candidate.name = user.username;
        candidate.state = user.state;
        candidate.country = user.anonymous ? QString() : user.country;
        candidate.note = stateGlyph(user.state);
        int slot = 0;
        for (const auto& seat : m_seats)
        {
            if (seat.userId == user.id)
                slot = seat.slot;
        }
        if (inRoomTab && slot != 0)
        {
            candidate.note = QStringLiteral("P%1").arg(slot);
            seated << candidate;
        }
        else
        {
            others << candidate;
        }
    }
    const auto byName = [](const MentionCandidate& a, const MentionCandidate& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    };
    std::sort(seated.begin(), seated.end(), byName);
    std::sort(others.begin(), others.end(), byName);
    const QString heading = seated.isEmpty() ? QString() : tr("In this room");
    QList<MentionCandidate> rows = seated + others;
    if (rows.size() > kMaxMentionRows)
        rows.resize(kMaxMentionRows);
    if (rows.isEmpty())
    {
        m_mentionPopup->hide();
        return;
    }
    m_mentionPopup->setCandidates(rows, int(typed.size()), heading);
    positionChatOverlays();
    m_mentionPopup->show();
    m_mentionPopup->raise();
}

void RollbackLobbyDialog::insertMention(const QString& name)
{
    if (name.isEmpty())
        return;
    const QString text = m_chatInput->text();
    const int cursor = m_chatInput->cursorPosition();
    int start = cursor;
    while (start > 0 && isNameChar(text.at(start - 1)))
        --start;
    const QString replaced = text.left(start) + name + QLatin1Char(' ');
    m_chatInput->setText(replaced + text.mid(cursor));
    m_chatInput->setCursorPosition(int(replaced.size()));
    m_mentionPopup->hide();
}

void RollbackLobbyDialog::refreshMentionJump()
{
    if (!m_mentionJump || !m_chatTabs)
        return;
    // The selected tab's, else the split chat's pane below.
    m_mentionJumpTab = -1;
    for (const int tab : {m_chatTabs->currentIndex(), m_chatSplitTab})
    {
        ChatView* view = chatViewFor(tab);
        if (!view)
            continue;
        QList<int>& pending = m_pendingMentions[tab];
        pending.removeIf([view](int blockNumber) {
            const QTextCursor cursor(view->document()->findBlockByNumber(blockNumber));
            return view->cursorRect(cursor).top() < view->viewport()->height();
        });
        if (!pending.isEmpty() && m_mentionJumpTab < 0)
            m_mentionJumpTab = tab;
    }
    if (m_mentionJumpTab < 0)
    {
        m_mentionJump->hide();
        return;
    }
    const QList<int>& pending = m_pendingMentions[m_mentionJumpTab];
    m_mentionJump->setText(pending.size() == 1 ? tr("@ 1 mention below ↓") : tr("@ %1 mentions below ↓").arg(pending.size()));
    m_mentionJump->adjustSize();
    positionChatOverlays();
    m_mentionJump->show();
    m_mentionJump->raise();
}

void RollbackLobbyDialog::positionChatOverlays()
{
    if (!m_chatCard || !m_chatInput)
        return;
    // Both sit just above a message box: the names above the one in use, the
    // jump above the one under its chat.
    const auto topOf = [this](QWidget* compose) { return compose->mapTo(m_chatCard, QPoint(0, 0)).y(); };
    if (m_mentionPopup)
    {
        const int height = m_mentionPopup->sizeHint().height();
        m_mentionPopup->setGeometry(8, topOf(m_chatInput->parentWidget()) - 5 - height, m_chatCard->width() - 16, height);
    }
    if (m_mentionJump)
    {
        QWidget* compose = m_mentionJumpTab >= 0 && m_mentionJumpTab == m_chatSplitTab ? m_chatSplitCompose : m_chatTopCompose;
        m_mentionJump->move((m_chatCard->width() - m_mentionJump->width()) / 2, topOf(compose) - 6 - m_mentionJump->height());
    }
}

// ──────────────────────────────────────────────────────────────────────
//  Moderation
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::openSanctionDialog(bool ban, const QString& name)
{
    auto* dialog = new SanctionDialog(ban ? SanctionDialog::Kind::Ban : SanctionDialog::Kind::Mute, name, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_client, &LobbyClient::modTargetInfo, dialog,
            [dialog, name](const QString& target, const QStringList& others, bool) {
        if (target.compare(name, Qt::CaseInsensitive) == 0)
            dialog->setOthers(others);
    });
    connect(dialog, &QDialog::accepted, this, [this, dialog, name]() {
        m_client->sendModAction(dialog->action(), name, dialog->duration(), dialog->reason());
    });
    m_client->sendModAction(QStringLiteral("inspect"), name);
    m_sanctionDialog = dialog;
    dialog->open();
}

void RollbackLobbyDialog::showModerationDialog()
{
    if (!m_moderationDialog)
    {
        m_moderationDialog = new ModerationDialog(m_moderatorName, this);
        connect(m_moderationDialog, &ModerationDialog::liftRequested, this, [this](const QString& kind, const QString& ip) {
            m_client->sendModAction(kind, ip);
            m_client->sendModAction(QStringLiteral("list"), QString());
        });
    }
    m_moderationDialog->show();
    m_moderationDialog->raise();
    m_client->sendModAction(QStringLiteral("list"), QString());
}

void RollbackLobbyDialog::onModResult(const QString& action, const QString& target, const QString& ip, qint64 untilMs)
{
    QString span;
    if (untilMs > 0)
    {
        const qint64 minutes = std::max<qint64>(1, (untilMs - QDateTime::currentMSecsSinceEpoch() + 30'000) / 60'000);
        span = minutes < 60 ? tr("%1 min").arg(minutes)
             : minutes < 48 * 60 ? tr("%1 h").arg(minutes / 60)
                                 : tr("%1 days").arg(minutes / (24 * 60));
    }
    const QString who = target.toHtmlEscaped();
    QString text;
    m_undoAction.clear();
    m_undoIp = ip;
    if (action == QLatin1String("kick"))
    {
        text = tr("Kicked <b>%1</b>").arg(who);
    }
    else if (action == QLatin1String("mute"))
    {
        text = span.isEmpty() ? tr("Muted <b>%1</b>").arg(who) : tr("Muted <b>%1</b> for %2").arg(who, span);
        m_undoAction = QStringLiteral("unmute");
    }
    else
    {
        text = span.isEmpty() ? tr("Banned <b>%1</b>").arg(who) : tr("Banned <b>%1</b> for %2").arg(who, span);
        m_undoAction = QStringLiteral("unban");
    }
    m_toast->showMessage(text, !m_undoAction.isEmpty());
    positionStageOverlays();

    if (m_moderationDialog)
    {
        QString plain = text;
        plain.remove(QStringLiteral("<b>"));
        plain.remove(QStringLiteral("</b>"));
        m_moderationDialog->addLogEntry(plain);
        if (m_moderationDialog->isVisible())
            m_client->sendModAction(QStringLiteral("list"), QString());
    }
}

#endif // NETPLAY
