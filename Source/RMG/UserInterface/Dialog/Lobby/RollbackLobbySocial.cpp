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

#include "LobbyRegions.hpp"
#include "LobbySocialDialogs.hpp"
#include "LobbySocialWidgets.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

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
    const char* const kNoChallengesSetting = "Lobby/NoChallenges";
    const char* const kSpectatorOverlaySetting = "Lobby/SpectatorChatOverGame";

    // The most of one spectator's comment that goes over the game.
    constexpr int kOverlayCommentChars = 120;
    // How often one person can make this PC make a noise.
    constexpr qint64 kAlertIntervalMs = 30'000;
    // How long the play bar shows how a challenge ended, if nobody clicks OK.
    constexpr int kOutcomeMs = 8'000;
    // How long past its deadline to wait for the lobby to end a challenge.
    constexpr int kChallengeGraceMs = 3'000;
    constexpr int kMaxMentionRows = 5;
    constexpr int kMaxCardWidth = 756;

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
        const int tab = m_chatTabs->currentIndex();
        ChatView* view = tab == kChatTabLobby ? m_chatViewLobby
            : tab == kChatTabRoom ? m_chatViewRoom
            : tab == kChatTabSpectators ? m_chatViewSpectate : nullptr;
        if (!view || m_pendingMentions[tab].isEmpty())
            return;
        view->setTextCursor(QTextCursor(view->document()->findBlockByNumber(m_pendingMentions[tab].first())));
        view->ensureCursorVisible();
        refreshMentionJump();
    });

    QSettings settings("RMG-K", "n02");
    m_ignored = settings.value(kIgnoredSetting).toStringList();
    m_doNotDisturb = settings.value(kDoNotDisturbSetting, false).toBool();
    m_client->setChallengesOff(settings.value(kNoChallengesSetting, false).toBool());
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
    m_incomingChallenges.clear();
    m_outgoingChallenge = LobbyClient::Challenge{};
    m_challengeOutcome.title.clear();
    m_challengeCard->setVisible(false);
    m_pmPanel->clear();
    m_lastPmLocal = 0;
    m_lastPmRemote = 0;
    m_lastAlertMs.clear();
    for (QList<int>& pending : m_pendingMentions)
        pending.clear();
    m_mentionJump->setVisible(false);
    if (m_chatTabs->currentIndex() == kChatTabMail)
        m_chatTabs->setCurrentIndex(kChatTabLobby);
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
    {
        m_chatTabs->setTabIcon(kChatTabMail, QIcon(LobbyTheme::icon(QStringLiteral("mail-line"),
            LobbyTheme::tokens().ink2, 14, devicePixelRatioF())));
        refreshSpectatorTab();
    }
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
        QString why;
        if (!user.social)
            why = tr("Needs an update");
        else if (user.noChallenges)
            why = tr("Not taking challenges");
        else if (playing)
            why = tr("In a match");
        else if (inLiveMatch())
            why = tr("You're playing");
        else if (m_outgoingChallenge.id != 0)
            why = tr("One at a time");
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
        if (user.social)
        {
            connect(menu.addItem(QStringLiteral("mail-line"), tr("Send a message")), &QAction::triggered, this,
                    [this, userId]() { openPrivateMessage(userId); });
        }
        else
        {
            menu.addDisabledItem(QStringLiteral("mail-line"), tr("Send a message"), tr("Needs an update"));
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
    if (m_client->isVerified())
    {
        // The account's actions, as they stand (an email to add or change,
        // Discord to link or unlink). Logging out is how the name changes.
        const auto addAccountItem = [&](QAction* action, const QString& icon) {
            if (!action || !action->isVisible())
                return;
            QAction* item = menu.addItem(icon, action->text());
            item->setEnabled(action->isEnabled());
            item->setToolTip(action->toolTip());
            connect(item, &QAction::triggered, action, &QAction::trigger);
        };
        addAccountItem(m_changePasswordAction, QStringLiteral("lock-line"));
        addAccountItem(m_emailAction, QStringLiteral("mail-line"));
        addAccountItem(m_discordAction, QStringLiteral("login-box-line"));
        addAccountItem(m_logOutAction, QStringLiteral("logout-box-r-line"));
    }
    else
    {
        // A guest's way to a new name, or into an account.
        QAction* rename = menu.addItem(QStringLiteral("edit-line"), tr("Change username…"));
        rename->setEnabled(!matchTransportInProgress());
        // Queued: the prompt opens once this menu has closed.
        connect(rename, &QAction::triggered, this, &RollbackLobbyDialog::changeUsername, Qt::QueuedConnection);
    }

    if (m_client->socialSupported())
    {
        // Public: others see it and can't send one. Do not disturb below is
        // private: challenges just go unanswered.
        auto* challengesRow = new QWidget(&menu);
        auto* challengesLay = new QHBoxLayout(challengesRow);
        challengesLay->setContentsMargins(11, 4, 11, 4);
        auto* challenges = new ToggleSwitch(tr("Take challenges"), challengesRow);
        challenges->setObjectName("TakeChallenges");
        challenges->setSubText(m_client->challengeSettingsSupported() ? tr("Off: nobody can send you one")
                                                                     : tr("Off: ones sent to you go unanswered"));
        challenges->setChecked(!m_client->challengesOff());
        connect(challenges, &QCheckBox::toggled, this, [this](bool on) {
            m_client->setChallengesOff(!on);
            QSettings("RMG-K", "n02").setValue(kNoChallengesSetting, !on);
            if (!on)
            {
                // Those already waiting are over too.
                for (const auto& challenge : std::as_const(m_incomingChallenges))
                    m_client->respondToChallenge(challenge.id, false);
                m_incomingChallenges.clear();
                showNextChallenge();
            }
        });
        challengesLay->addWidget(challenges);
        menu.addLiveWidget(challengesRow);

        auto* row = new QWidget(&menu);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(11, 4, 11, 4);
        auto* toggle = new ToggleSwitch(tr("Do not disturb"), row);
        toggle->setObjectName("DoNotDisturb");
        toggle->setSubText(tr("No challenges or sounds"));
        toggle->setChecked(m_doNotDisturb);
        connect(toggle, &QCheckBox::toggled, this, [this](bool on) {
            m_doNotDisturb = on;
            QSettings("RMG-K", "n02").setValue(kDoNotDisturbSetting, on);
        });
        rowLay->addWidget(toggle);
        menu.addLiveWidget(row);

        connect(menu.addItem(QStringLiteral("forbid-line"), tr("Ignored players…")), &QAction::triggered,
                this, &RollbackLobbyDialog::showIgnoredPlayers);
    }

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

    menu.addHeading(QStringLiteral("layout-column-line"), tr("Live and open rooms"), false);
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
    QAction* above = menu.addCheckItem(QStringLiteral("layout-row-line"), tr("Live above the rooms"), stacked, layouts);
    connect(above, &QAction::toggled, this, [pickLayout](bool on) { if (on) pickLayout(true); });

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

void RollbackLobbyDialog::startChallenge(quint64 userId)
{
    const auto found = m_client->users().constFind(userId);
    if (found == m_client->users().constEnd() || m_client->isOnThisPc(userId) || m_outgoingChallenge.id != 0)
        return;

    // From a room it's an invite to that room, so there's nothing to set up.
    if (m_currentRoomId != 0)
    {
        m_challengeTargetName = found->username;
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

    auto* dialog = new ChallengeDialog(self, target, games, selectedBrowseRom().value("md5").toString(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setCoverLookup([this](const QString& md5) { return coverPathForMd5(md5); });
    connect(m_client, &LobbyClient::sharedGamesReceived, dialog, [dialog, userId](quint64 to, const QStringList& md5s) {
        if (to == userId)
            dialog->setSharedGames(md5s);
    });
    const QString targetName = found->username;
    connect(dialog, &QDialog::accepted, this, [this, dialog, userId, targetName]() {
        m_challengeTargetName = targetName;
        m_client->sendChallenge(userId, dialog->romName(), dialog->romMd5(), dialog->note(), dialog->liveReplay());
    });
    m_client->requestSharedGames(userId);
    m_challengeDialog = dialog;
    dialog->open();
}

void RollbackLobbyDialog::onChallengeSent(const LobbyClient::Challenge& challenge)
{
    m_challengeOutcome.title.clear();
    m_outgoingChallenge = challenge;
    expireChallengeLater(challenge.id, challenge.expiresAtMs);
    if (challenge.invite)
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("Invited <b>%1</b> to the room").arg(challenge.toUsername.toHtmlEscaped()),
                             QStringLiteral("mail-send-line"));
    }
    refreshPlayBar();
}

void RollbackLobbyDialog::onChallengeIncoming(const LobbyClient::Challenge& challenge)
{
    // The sender just sees it go unanswered, so they can't tell they're ignored.
    // With challenges off, only an older lobby still sends them.
    if (m_doNotDisturb || m_client->challengesOff() || isIgnored(challenge.fromUsername))
        return;
    m_incomingChallenges.append(challenge);
    expireChallengeLater(challenge.id, challenge.expiresAtMs);
    showNextChallenge();
    alertFrom(challenge.fromUserId);
    // Shown only if a game is running: someone watching can't see the lobby behind it.
    emit overlayMessage(tr("%1 challenges you. Open the lobby to answer.").arg(challenge.fromUsername));
}

void RollbackLobbyDialog::showNextChallenge()
{
    if (m_incomingChallenges.isEmpty())
    {
        m_challengeCard->setVisible(false);
        return;
    }
    const LobbyClient::Challenge& challenge = m_incomingChallenges.first();
    const auto& users = m_client->users();

    ChallengeCardView view;
    view.challengeId = challenge.id;
    view.invite = challenge.invite;
    view.fromName = challenge.fromUsername;
    if (const auto from = users.constFind(challenge.fromUserId); from != users.constEnd())
    {
        view.fromCountry = from->anonymous ? QString() : from->country;
        view.fromConnection = from->connection;
        view.fromPingMs = m_client->measuredPingMs(challenge.fromUserId);
        view.fromPingEstimate = view.fromPingMs < 0;
        if (view.fromPingEstimate)
        {
            const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), from->region);
            view.fromPingMs = estimate > 0 ? estimate : -1;
        }
    }

    view.toName = challenge.toUsername;
    const bool forSelf = challenge.toUserId == m_client->selfUserId();
    view.toTag = forSelf ? tr("You") : tr("This PC");
    if (const auto to = users.constFind(challenge.toUserId); to != users.constEnd())
    {
        view.toCountry = to->anonymous ? QString() : to->country;
        view.toConnection = to->connection;
        view.toDetail = ConnectionGlyph::displayName(to->connection);
    }
    // Everyone on this PC plays. Only this PC's screen ever says so.
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
        view.toConnection.clear();
        view.toDetail = others.size() == 1 ? tr("%1 plays too").arg(others.first())
                                           : tr("%1 play too").arg(others.join(QStringLiteral(", ")));
    }

    view.roomPlayers = challenge.roomPlayers;
    view.roomMaxPlayers = challenge.roomMaxPlayers;
    view.game = challenge.romName;
    view.coverPath = coverPathForMd5(challenge.romMd5);
    view.gameDetail = challenge.invite ? tr("%1 invites you to their room").arg(challenge.fromUsername)
        : challenge.note.isEmpty() ? QString() : QStringLiteral("“%1”").arg(challenge.note);
    if (challenge.invite)
        view.gameDetail = challenge.roomName;
    view.waitingBehind = int(m_incomingChallenges.size()) - 1;
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

    m_challengeCard->setView(view);
    positionStageOverlays();
    m_challengeCard->setVisible(true);
    m_challengeCard->raise();
}

void RollbackLobbyDialog::answerChallenge(bool accept)
{
    if (m_incomingChallenges.isEmpty())
        return;
    const LobbyClient::Challenge challenge = m_incomingChallenges.takeFirst();
    if (accept)
    {
        stopWatchingForRoom();
        m_acceptedChallengeId = challenge.id;
        m_acceptedChallengeFrom = challenge.fromUsername;
        m_acceptedChallengeInvite = challenge.invite;
    }
    m_client->respondToChallenge(challenge.id, accept);
    showNextChallenge();
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

void RollbackLobbyDialog::setChallengeOutcome(const QString& title, const QString& detail, const QColor& color, const QString& icon)
{
    const int generation = m_challengeOutcome.generation + 1;
    m_challengeOutcome = ChallengeOutcome{ title, detail, color, icon, generation };
    refreshPlayBar();
    QTimer::singleShot(kOutcomeMs, this, [this, generation]() {
        if (m_challengeOutcome.generation != generation || m_challengeOutcome.title.isEmpty())
            return;
        m_challengeOutcome.title.clear();
        refreshPlayBar();
    });
}

void RollbackLobbyDialog::onChallengeEnded(quint64 challengeId, quint64 toUserId, const QString& result)
{
    const Tokens& t = LobbyTheme::tokens();

    // One this PC was asked: the card goes, and that's the whole of it.
    for (int i = 0; i < m_incomingChallenges.size(); ++i)
    {
        if (m_incomingChallenges.at(i).id == challengeId)
        {
            m_incomingChallenges.removeAt(i);
            showNextChallenge();
            return;
        }
    }
    const bool mine = challengeId != 0 && challengeId == m_outgoingChallenge.id;
    if (challengeId != 0 && !mine)
    {
        // This PC's answer got there first, and saying yes landed in the room.
        if (challengeId == m_acceptedChallengeId)
        {
            const QString from = m_acceptedChallengeFrom.toHtmlEscaped();
            if (result == QLatin1String("accepted"))
            {
                appendChatSystemLine(CHANNEL_ROOM,
                    (m_acceptedChallengeInvite ? tr("You accepted <b>%1</b>'s invite")
                                               : tr("You accepted <b>%1</b>'s challenge")).arg(from),
                    QStringLiteral("sword-fill"));
            }
            else
            {
                // Too late: they went into a match or left while this was on its way.
                appendChatSystemLine(CHANNEL_LOBBY, tr("<b>%1</b>'s challenge can't go ahead any more.").arg(from),
                                     QStringLiteral("sword-fill"));
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
    if (mine)
        m_outgoingChallenge = LobbyClient::Challenge{};

    if (result == QLatin1String("accepted"))
    {
        if (!invite)
            stopWatchingForRoom();
        appendChatSystemLine(CHANNEL_ROOM,
            (invite ? tr("<b>%1</b> accepted your invite") : tr("<b>%1</b> accepted your challenge")).arg(name.toHtmlEscaped()),
            QStringLiteral("sword-fill"));
        refreshPlayBar();
        return;
    }
    if (result == QLatin1String("cancelled") || result == QLatin1String("pending"))
    {
        refreshPlayBar();
        return;
    }

    QString title;
    QString detail;
    QColor color = t.muted;
    QString icon = QStringLiteral("close-circle-line");
    if (result == QLatin1String("declined"))
    {
        title = tr("%1 declined").arg(name);
        detail = tr("You can challenge them again in a minute.");
    }
    else if (result == QLatin1String("cooldown"))
    {
        title = tr("%1 declined a moment ago").arg(name);
        detail = tr("You can challenge them again in a minute.");
    }
    else if (result == QLatin1String("timeout"))
    {
        title = tr("No answer");
        detail = tr("%1 didn't answer in time.").arg(name);
        icon = QStringLiteral("time-line");
    }
    else if (result == QLatin1String("busy"))
    {
        title = tr("%1 is busy").arg(name);
        detail = tr("They've just started a match. You can watch it from Live now.");
        color = t.warn;
        icon = QStringLiteral("gamepad-line");
    }
    else if (result == QLatin1String("challenges_off"))
    {
        title = tr("%1 isn't taking challenges").arg(name);
        detail = tr("They turned challenges off. You can still send them a message.");
        icon = QStringLiteral("forbid-line");
    }
    else if (result == QLatin1String("no_game"))
    {
        title = tr("%1 doesn't have that game").arg(name);
        detail = tr("Try a game you both have.");
        color = t.warn;
        icon = QStringLiteral("error-warning-line");
    }
    else if (result == QLatin1String("blocked_note"))
    {
        title = tr("Message blocked");
        detail = tr("The chat filter blocked your message. Try again without it.");
        color = t.warn;
        icon = QStringLiteral("error-warning-line");
    }
    else if (result == QLatin1String("needs_update"))
    {
        title = tr("%1 needs an update").arg(name);
        detail = tr("Their version of Dale64 can't receive challenges.");
    }
    else if (result == QLatin1String("too_fast"))
    {
        title = tr("That's a lot of challenges");
        detail = tr("Try again in a moment.");
    }
    else
    {
        title = tr("Can't challenge right now");
        detail = tr("%1 can't take a challenge at the moment.").arg(name);
    }

    // The play bar isn't on screen in a room, so a line of chat says it there.
    if (m_currentRoomId != 0)
    {
        appendChatSystemLine(CHANNEL_ROOM, QStringLiteral("%1. %2").arg(title.toHtmlEscaped(), detail.toHtmlEscaped()), icon);
        refreshPlayBar();
        return;
    }
    setChallengeOutcome(title, detail, color, icon);
}

void RollbackLobbyDialog::positionStageOverlays()
{
    if (!m_stage || !m_challengeCard || !m_toast)
        return;
    const int available = m_stage->width();
    const int cardWidth = std::min(kMaxCardWidth, available - 40);
    m_challengeCard->setFixedWidth(cardWidth);
    m_challengeCard->adjustSize();
    m_challengeCard->move((available - cardWidth) / 2, 10);
    m_toast->move(14, m_stage->height() - m_toast->height() - 14);
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
    quint64 local = m_pmPanel->threadLocalFor(userId);
    if (local == 0)
        local = m_client->selfUserId();
    m_pmPanel->threadView(local, lobbyUserName(local), userId, found->username, local == m_client->selfUserId());
    refreshPrivateMessageStatus(userId);
    m_chatTabs->setCurrentIndex(kChatTabMail);
    m_pmPanel->openThread(local, userId);
    m_chatInput->setFocus();
}

void RollbackLobbyDialog::sendCurrentPrivateMessage(const QString& text)
{
    if (m_pmPanel->threadOpen())
        m_client->sendPrivateMessage(m_pmPanel->openRemoteId(), m_pmPanel->openLocalId(), text);
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

    ChatView* view = m_pmPanel->threadView(local, localName, remote, remoteName, local == m_client->selfUserId());
    refreshPrivateMessageStatus(remote);
    const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
    view->addLine(chatMessageHtml(ts, msg.fromUserId, msg.fromUsername, chatBodyHtml(msg.message, nullptr), msg.fromAdmin, outgoing));
    m_pmPanel->noteMessage(local, remote, outgoing ? tr("You: %1").arg(msg.message) : msg.message, msg.serverTimeMs, !outgoing);
    if (outgoing)
        return;

    m_lastPmLocal = local;
    m_lastPmRemote = remote;
    const bool looking = isActiveWindow() && m_chatTabs->currentIndex() == kChatTabMail &&
                         m_pmPanel->openLocalId() == local && m_pmPanel->openRemoteId() == remote;
    if (!looking)
        alertFrom(remote);
    // Not roomChatReceived: that also writes the line into a saved replay.
    emit overlayMessage(tr("%1 (private): %2").arg(msg.fromUsername, msg.message));
}

void RollbackLobbyDialog::onPrivateMessageFailed(quint64 toUserId, const QString& reason)
{
    ChatView* view = m_pmPanel->existingView(toUserId);
    if (!view)
        return;
    QString why = tr("That message couldn't be delivered.");
    if (reason == QLatin1String("user_gone"))
        why = tr("They've left the lobby, so that message went nowhere.");
    else if (reason == QLatin1String("needs_update"))
        why = tr("Their version of Dale64 can't receive private messages.");
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
    m_pmPanel->setRemoteStatus(userId, found->state, status, found->anonymous ? QString() : found->country,
                               found->social && found->state != QLatin1String("playing"));
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
        m_chatTabs->showTab(kChatTabSpectators, true);
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
    QList<ChatView*> views = m_pmPanel ? m_pmPanel->views() : QList<ChatView*>();
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

    if (tab != m_chatTabs->currentIndex())
    {
        if (m_chatTabs->isTabVisible(tab))
        {
            m_chatMentioned[tab] = true;
            setChatUnread(tab, m_chatUnread[tab]);
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
    // A mention belongs in a channel people are reading, so not the mail tab.
    if (m_chatTabs->currentIndex() == kChatTabMail)
        m_chatTabs->setCurrentIndex(m_currentRoomId != 0 ? kChatTabRoom : kChatTabLobby);
    m_chatInput->insert(QStringLiteral("@%1 ").arg(name));
    m_chatInput->setFocus();
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
        (m_chatTabs->currentIndex() == kChatTabMail && !whisper))
    {
        m_mentionPopup->hide();
        return;
    }

    const QString typed = text.mid(start, cursor - start);
    const bool inRoomTab = m_chatTabs->currentIndex() == kChatTabRoom;
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
    const int tab = m_chatTabs->currentIndex();
    ChatView* view = tab == kChatTabLobby ? m_chatViewLobby
        : tab == kChatTabRoom ? m_chatViewRoom
        : tab == kChatTabSpectators ? m_chatViewSpectate : nullptr;
    if (!view)
    {
        m_mentionJump->hide();
        return;
    }
    QList<int>& pending = m_pendingMentions[tab];
    pending.removeIf([view](int blockNumber) {
        const QTextCursor cursor(view->document()->findBlockByNumber(blockNumber));
        return view->cursorRect(cursor).top() < view->viewport()->height();
    });
    if (pending.isEmpty())
    {
        m_mentionJump->hide();
        return;
    }
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
    // Both sit just above the message box.
    const int composeTop = m_chatInput->parentWidget()->mapTo(m_chatCard, QPoint(0, 0)).y();
    if (m_mentionPopup)
    {
        const int height = m_mentionPopup->sizeHint().height();
        m_mentionPopup->setGeometry(8, composeTop - 5 - height, m_chatCard->width() - 16, height);
    }
    if (m_mentionJump)
        m_mentionJump->move((m_chatCard->width() - m_mentionJump->width()) / 2, composeTop - 6 - m_mentionJump->height());
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
