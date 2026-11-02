/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbySocialDialogs.hpp"

#include "LobbySocialWidgets.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    QLabel* fieldLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        label->setFont(font);
        label->setProperty("role", "muted");
        return label;
    }

    QFrame* rule(QWidget* parent)
    {
        auto* line = new QFrame(parent);
        line->setObjectName("FieldRule");
        return line;
    }

    // The dialog's band: an icon or tile, a pixel-font title, and a line under it.
    QWidget* makeHeader(QWidget* lead, const QString& title, const QString& detail, QWidget* parent)
    {
        auto* header = new LobbyBand(parent);
        auto* lay = new QHBoxLayout(header);
        lay->setContentsMargins(18, 12, 18, 12);
        lay->setSpacing(12);
        lead->setParent(header);
        lay->addWidget(lead);
        auto* words = new QVBoxLayout;
        words->setSpacing(1);
        auto* titleLabel = new QLabel(title.toUpper(), header);
        titleLabel->setFont(LobbyTheme::pixelFont(15, 0.5));
        words->addWidget(titleLabel);
        auto* detailLabel = new QLabel(detail, header);
        detailLabel->setFont(LobbyTheme::uiFont(11));
        detailLabel->setProperty("role", "muted");
        words->addWidget(detailLabel);
        lay->addLayout(words, 1);
        return header;
    }

    // One side of the VS line.
    QWidget* makeVsSide(const ChallengeDialogPlayer& player, bool right, QWidget* parent)
    {
        auto* side = new QWidget(parent);
        auto* lay = new QHBoxLayout(side);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(11);
        auto* tile = new PlayerTile(44, side);
        tile->setPlayer(player.name);

        auto* words = new QVBoxLayout;
        words->setSpacing(2);
        words->setAlignment(Qt::AlignVCenter);
        auto* top = new QHBoxLayout;
        top->setSpacing(6);
        if (right)
            top->addStretch(1);
        auto* name = new ElidedLabel(Role::Ink, side);
        name->setFont(LobbyTheme::uiFont(13, QFont::Bold));
        name->setText(player.name);
        top->addWidget(name);
        auto* flag = new CountryFlag(side);
        flag->setCountry(player.country);
        top->addWidget(flag);
        if (!right)
            top->addStretch(1);
        words->addLayout(top);

        auto* sub = new QHBoxLayout;
        sub->setSpacing(5);
        if (right)
            sub->addStretch(1);
        if (player.pingMs >= 0)
        {
            auto* bars = new SignalBars(side);
            bars->setPing(player.pingMs, player.pingEstimate);
            sub->addWidget(bars);
            auto* ping = new QLabel(QString(player.pingEstimate ? "~%1 ms" : "%1 ms").arg(player.pingMs), side);
            ping->setFont(LobbyTheme::uiFont(11));
            ping->setProperty("role", "muted");
            sub->addWidget(ping);
        }
        auto* glyph = new ConnectionGlyph(side);
        glyph->setConnection(player.connection);
        sub->addWidget(glyph);
        auto* connection = new QLabel(ConnectionGlyph::displayName(player.connection), side);
        connection->setFont(LobbyTheme::uiFont(11));
        connection->setProperty("role", "muted");
        sub->addWidget(connection);
        if (!right)
            sub->addStretch(1);
        words->addLayout(sub);

        if (right)
        {
            lay->addLayout(words, 1);
            lay->addWidget(tile);
        }
        else
        {
            lay->addWidget(tile);
            lay->addLayout(words, 1);
        }
        return side;
    }
} // namespace

// ── ChallengeDialog ──────────────────────────────────────────────────

ChallengeDialog::ChallengeDialog(const ChallengeDialogPlayer& self, const ChallengeDialogPlayer& target,
                                 const QList<QVariantMap>& games, const QString& selectedMd5, QWidget* parent)
    : QDialog(parent), m_targetName(target.name), m_games(games)
{
    setWindowTitle(tr("Challenge"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setFixedWidth(478);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* mark = new IconLabel(QStringLiteral("sword-fill"), Role::Accent, 22, this);
    root->addWidget(makeHeader(mark, tr("Challenge %1").arg(target.name),
                               tr("If they accept, you're both moved into a private room."), this));
    root->addWidget(rule(this));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(18, 14, 18, 14);
    body->setSpacing(13);

    auto* versus = new QFrame(this);
    versus->setObjectName("ChallengeVersus");
    auto* versusLay = new QHBoxLayout(versus);
    versusLay->setContentsMargins(12, 11, 12, 11);
    versusLay->setSpacing(8);
    versusLay->addWidget(makeVsSide(self, false, versus), 1);
    auto* vs = new QLabel(QStringLiteral("VS"), versus);
    QFont vsFont(QStringLiteral("Arial Black"));
    vsFont.setPixelSize(18);
    vsFont.setItalic(true);
    vsFont.setWeight(QFont::Black);
    vs->setFont(vsFont);
    vs->setProperty("role", "faint");
    versusLay->addWidget(vs);
    versusLay->addWidget(makeVsSide(target, true, versus), 1);
    body->addWidget(versus);

    auto* gameBox = new QVBoxLayout;
    gameBox->setSpacing(6);
    gameBox->addWidget(fieldLabel(tr("Game"), this));
    auto* picker = new QFrame(this);
    picker->setObjectName("RomPicker");
    picker->setFixedHeight(44);
    auto* pickerLay = new QHBoxLayout(picker);
    pickerLay->setContentsMargins(7, 0, 6, 0);
    pickerLay->setSpacing(10);
    m_tile = new CartridgeTile(CartridgeTile::Size::Small, picker);
    pickerLay->addWidget(m_tile);
    auto* pickerWords = new QVBoxLayout;
    pickerWords->setContentsMargins(0, 3, 0, 3);
    pickerWords->setSpacing(0);
    m_combo = new QComboBox(picker);
    m_combo->setObjectName("ChallengeRomCombo");
    m_combo->setFont(LobbyTheme::uiFont(13, QFont::Bold));
    m_combo->setFixedHeight(18);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(8);
    m_combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    pickerWords->addWidget(m_combo);
    m_gameStatus = new QLabel(picker);
    m_gameStatus->setFont(LobbyTheme::uiFont(11));
    m_gameStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pickerWords->addWidget(m_gameStatus);
    pickerLay->addLayout(pickerWords, 1);
    auto* open = new IconButton(QStringLiteral("arrow-down-s-line"), 24, picker);
    open->setToolTip(tr("All your games"));
    open->setAccessibleName(tr("All your games"));
    connect(open, &QToolButton::clicked, this, [this]() { m_combo->showPopup(); });
    pickerLay->addWidget(open);
    gameBox->addWidget(picker);
    body->addLayout(gameBox);

    auto* noteBox = new QVBoxLayout;
    noteBox->setSpacing(6);
    noteBox->addWidget(fieldLabel(tr("Message (optional)"), this));
    m_note = new QLineEdit(this);
    m_note->setObjectName("ChallengeNote");
    m_note->setMaxLength(80);
    m_note->setMinimumHeight(34);
    noteBox->addWidget(m_note);
    body->addLayout(noteBox);

    m_liveReplay = new ToggleSwitch(tr("Live replay for spectators"), this);
    m_liveReplay->setObjectName("LiveReplaySwitch");
    m_liveReplay->setSubText(tr("Shows in Live now for anyone to watch, with spectator chat"));
    m_liveReplay->setChecked(QSettings("RMG-K", "n02").value("RollbackLobby/LiveReplayEnabled", true).toBool());
    body->addWidget(m_liveReplay);
    root->addLayout(body);

    root->addWidget(rule(this));

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    auto* hint = new QLabel(tr("They have 30 seconds to answer."), this);
    hint->setFont(LobbyTheme::uiFont(11));
    hint->setProperty("role", "muted");
    footer->addWidget(hint);
    footer->addStretch(1);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Ghost, this);
    cancel->setButtonSize(LobbyButton::Size::Large);
    cancel->setAutoDefault(false);
    m_send = new LobbyButton(tr("Send"), LobbyButton::Kind::Primary, this);
    m_send->setObjectName("ChallengeSend");
    m_send->setIconName(QStringLiteral("sword-fill"));
    m_send->setPixelLabel(true);
    m_send->setButtonSize(LobbyButton::Size::Large);
    m_send->setDefault(true);
    footer->addWidget(cancel);
    footer->addWidget(m_send);
    root->addLayout(footer);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, [this]() {
        QSettings("RMG-K", "n02").setValue("RollbackLobby/LiveReplayEnabled", m_liveReplay->isChecked());
        accept();
    });
    connect(m_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refreshGame(); });

    fillCombo(selectedMd5);
    refreshGame();
    m_send->setFocus();
}

void ChallengeDialog::setCoverLookup(std::function<QString(const QString&)> coverForMd5)
{
    m_coverForMd5 = std::move(coverForMd5);
    refreshGame();
}

void ChallengeDialog::fillCombo(const QString& selectMd5)
{
    const QSignalBlocker blocker(m_combo);
    m_combo->clear();
    // Games you both have come first; the rest are still there, greyed by the
    // status line, in case the server's list is out of date.
    const auto add = [this](bool shared) {
        for (const QVariantMap& game : m_games)
        {
            const bool has = m_shared.contains(game.value("md5").toString().toUpper());
            if (has == shared)
                m_combo->addItem(game.value("name").toString(), game);
        }
    };
    add(true);
    add(false);
    for (int i = 0; i < m_combo->count(); ++i)
    {
        if (m_combo->itemData(i).toMap().value("md5").toString().compare(selectMd5, Qt::CaseInsensitive) == 0)
        {
            m_combo->setCurrentIndex(i);
            break;
        }
    }
}

void ChallengeDialog::setSharedGames(const QStringList& md5s)
{
    m_shared.clear();
    for (const QString& md5 : md5s)
        m_shared << md5.toUpper();
    // An empty answer may just mean the server doesn't know their games.
    m_sharedKnown = !m_shared.isEmpty();
    fillCombo(romMd5());
    refreshGame();
}

void ChallengeDialog::refreshGame()
{
    const Tokens& t = LobbyTheme::tokens();
    const QString md5 = romMd5();
    m_tile->setGame(romName(), m_coverForMd5 ? m_coverForMd5(md5) : QString());
    if (!m_sharedKnown)
    {
        m_gameStatus->setText(QString());
    }
    else if (m_shared.contains(md5.toUpper()))
    {
        m_gameStatus->setText(tr("✓ %1 has this game").arg(m_targetName));
        m_gameStatus->setStyleSheet(QString("color: %1;").arg(t.ok.name()));
    }
    else
    {
        m_gameStatus->setText(tr("%1 doesn't have this game").arg(m_targetName));
        m_gameStatus->setStyleSheet(QString("color: %1;").arg(t.warn.name()));
    }
    m_send->setEnabled(!md5.isEmpty() && (!m_sharedKnown || m_shared.contains(md5.toUpper())));
}

QString ChallengeDialog::romName() const
{
    return m_combo->currentData().toMap().value("name").toString();
}

QString ChallengeDialog::romMd5() const
{
    return m_combo->currentData().toMap().value("md5").toString();
}

QString ChallengeDialog::note() const
{
    return m_note->text().trimmed();
}

bool ChallengeDialog::liveReplay() const
{
    return m_liveReplay->isChecked();
}

// ── SanctionDialog ───────────────────────────────────────────────────

namespace
{
    struct DurationChoice
    {
        const char* label;
        const char* value; // what the server parses; empty is permanent
    };
    constexpr DurationChoice kDurations[] = {
        { QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "10 min"), "10m" },
        { QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "1 hour"), "1h" },
        { QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "1 day"), "1d" },
        { QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "7 days"), "7d" },
        { QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Permanent"), "" },
    };
    constexpr const char* kReasons[] = {
        QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Harassment"),
        QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Slurs"),
        QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Spam"),
        QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Quitting matches"),
        QT_TRANSLATE_NOOP("UserInterface::Dialog::SanctionDialog", "Cheating"),
    };
} // namespace

SanctionDialog::SanctionDialog(Kind kind, const QString& target, QWidget* parent)
    : QDialog(parent), m_kind(kind), m_target(target)
{
    const bool ban = kind == Kind::Ban;
    setWindowTitle(ban ? tr("Ban player") : tr("Mute player"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    setFixedWidth(484);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* mark = new IconLabel(ban ? QStringLiteral("auction-line") : QStringLiteral("volume-mute-line"), Role::Bad, 22, this);
    root->addWidget(makeHeader(mark, (ban ? tr("Ban %1") : tr("Mute %1")).arg(target),
                               ban ? tr("Disconnects them now and blocks their IP.")
                                   : tr("Stops chat and private messages from their IP."), this));
    root->addWidget(rule(this));

    auto* body = new QVBoxLayout;
    body->setContentsMargins(18, 14, 18, 14);
    body->setSpacing(13);

    auto* durationBox = new QVBoxLayout;
    durationBox->setSpacing(6);
    durationBox->addWidget(fieldLabel(tr("For how long"), this));
    auto* seg = new QFrame(this);
    seg->setObjectName("Seg");
    auto* segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(3, 3, 3, 3);
    segLay->setSpacing(3);
    m_durations = new QButtonGroup(this);
    int index = 0;
    for (const DurationChoice& choice : kDurations)
    {
        auto* button = new QPushButton(tr(choice.label), seg);
        button->setProperty("seg", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        button->setObjectName(QString("SanctionDuration%1").arg(index));
        m_durations->addButton(button, index);
        segLay->addWidget(button, 1);
        ++index;
    }
    m_durations->button(ban ? 2 : 1)->setChecked(true);
    durationBox->addWidget(seg);
    body->addLayout(durationBox);

    auto* reasonBox = new QVBoxLayout;
    reasonBox->setSpacing(6);
    reasonBox->addWidget(fieldLabel(tr("Reason"), this));
    auto* chips = new QHBoxLayout;
    chips->setSpacing(5);
    for (const char* reason : kReasons)
    {
        const QString text = tr(reason);
        auto* chip = new QPushButton(text, this);
        chip->setProperty("chip", true);
        chip->setAutoDefault(false);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        connect(chip, &QPushButton::clicked, this, [this, text]() {
            m_reason->setText(text);
            m_reason->setFocus();
        });
        chips->addWidget(chip);
    }
    chips->addStretch(1);
    reasonBox->addLayout(chips);
    m_reason = new QLineEdit(this);
    m_reason->setObjectName("SanctionReason");
    m_reason->setPlaceholderText(tr("Why? They'll see this."));
    m_reason->setMaxLength(120);
    m_reason->setMinimumHeight(34);
    m_reason->installEventFilter(this);
    reasonBox->addWidget(m_reason);
    body->addLayout(reasonBox);

    auto* noteBox = new QFrame(this);
    noteBox->setObjectName("SanctionNote");
    auto* noteLay = new QHBoxLayout(noteBox);
    noteLay->setContentsMargins(10, 8, 10, 8);
    noteLay->setSpacing(7);
    noteLay->addWidget(new IconLabel(QStringLiteral("error-warning-line"), Role::Warn, 14, noteBox), 0, Qt::AlignTop);
    m_note = new QLabel(noteBox);
    m_note->setFont(LobbyTheme::uiFont(11));
    m_note->setProperty("role", "ink2");
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::RichText);
    noteLay->addWidget(m_note, 1);
    body->addWidget(noteBox);
    root->addLayout(body);
    setOthers({});

    root->addWidget(rule(this));

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 11, 18, 15);
    footer->setSpacing(10);
    auto* hint = new QLabel(tr("They'll see the reason."), this);
    hint->setFont(LobbyTheme::uiFont(11));
    hint->setProperty("role", "muted");
    footer->addWidget(hint);
    footer->addStretch(1);
    auto* cancel = new LobbyButton(tr("Cancel"), LobbyButton::Kind::Secondary, this);
    cancel->setButtonSize(LobbyButton::Size::Large);
    cancel->setAutoDefault(false);
    m_confirm = new LobbyButton(QString(), LobbyButton::Kind::Primary, this);
    m_confirm->setObjectName("SanctionConfirm");
    m_confirm->setIconName(ban ? QStringLiteral("auction-line") : QStringLiteral("volume-mute-line"));
    m_confirm->setPixelLabel(true);
    m_confirm->setButtonSize(LobbyButton::Size::Large);
    m_confirm->setAutoDefault(false);
    footer->addWidget(cancel);
    footer->addWidget(m_confirm);
    root->addLayout(footer);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_confirm, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_durations, &QButtonGroup::idToggled, this, [this](int, bool checked) {
        if (checked)
            refreshConfirm();
    });
    refreshConfirm();
}

bool SanctionDialog::eventFilter(QObject* watched, QEvent* event)
{
    // Enter in the reason field must never ban anyone by accident.
    if (watched == m_reason && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
            return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SanctionDialog::refreshConfirm()
{
    const bool ban = m_kind == Kind::Ban;
    if (duration().isEmpty())
    {
        m_confirm->setText(ban ? tr("Ban permanently") : tr("Mute permanently"));
        return;
    }
    const QString length = m_durations->checkedButton()->text();
    m_confirm->setText(ban ? tr("Ban for %1").arg(length) : tr("Mute for %1").arg(length));
}

void SanctionDialog::setOthers(const QStringList& others)
{
    QStringList sentences;
    if (!others.isEmpty())
    {
        QStringList escaped;
        for (const QString& name : others)
            escaped << QString("<b>%1</b>").arg(name.toHtmlEscaped());
        sentences << (m_kind == Kind::Ban
            ? tr("%1 on the same PC goes too.").arg(escaped.join(QStringLiteral(", ")))
            : tr("%1 on the same PC is muted too.").arg(escaped.join(QStringLiteral(", "))));
    }
    sentences << (m_kind == Kind::Ban
        ? tr("Anyone else on this IP can't connect until the ban ends.")
        : tr("Anyone else on this IP is muted too."));
    m_note->setText(sentences.join(QLatin1Char(' ')));
}

QString SanctionDialog::action() const
{
    if (m_kind == Kind::Mute)
        return QStringLiteral("mute");
    return duration().isEmpty() ? QStringLiteral("ban") : QStringLiteral("timeout");
}

QString SanctionDialog::duration() const
{
    return QString::fromLatin1(kDurations[m_durations->checkedId()].value);
}

QString SanctionDialog::reason() const
{
    return m_reason->text().trimmed();
}

// ── ModerationDialog ─────────────────────────────────────────────────

namespace
{
    constexpr int kTagColumn = 44;
    constexpr int kPlayerColumn = 150;
    constexpr int kByColumn = 80;
    constexpr int kEndsColumn = 110;
    constexpr int kLiftColumn = 70;

    QString untilText(qint64 untilMs)
    {
        if (untilMs == 0)
            return QCoreApplication::translate("UserInterface::Dialog::ModerationDialog", "Permanent");
        const qint64 left = (untilMs - QDateTime::currentMSecsSinceEpoch()) / 60'000;
        if (left < 60)
            return QCoreApplication::translate("UserInterface::Dialog::ModerationDialog", "In %1 min").arg(std::max<qint64>(1, left));
        if (left < 24 * 60)
            return QCoreApplication::translate("UserInterface::Dialog::ModerationDialog", "In %1 h").arg(left / 60);
        return QDateTime::fromMSecsSinceEpoch(untilMs).toString("yyyy-MM-dd hh:mm");
    }

    void clearRows(QVBoxLayout* rows)
    {
        while (rows->count() > 1)
        {
            QLayoutItem* item = rows->takeAt(0);
            if (QWidget* widget = item->widget())
            {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
    }

    QScrollArea* makeRowsArea(QWidget* parent, QWidget** body, QVBoxLayout** rows)
    {
        auto* scroll = new QScrollArea(parent);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->viewport()->setAutoFillBackground(false);
        *body = new QWidget(scroll);
        *rows = new QVBoxLayout(*body);
        (*rows)->setContentsMargins(0, 0, 0, 0);
        (*rows)->setSpacing(0);
        (*rows)->addStretch(1);
        scroll->setWidget(*body);
        (*body)->setAutoFillBackground(false); // setWidget turns it on
        return scroll;
    }
} // namespace

ModerationDialog::ModerationDialog(const QString& moderatorName, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Moderation"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    resize(760, 400);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* band = new LobbyBand(this);
    auto* bandLay = new QHBoxLayout(band);
    bandLay->setContentsMargins(18, 12, 18, 12);
    bandLay->setSpacing(12);
    bandLay->addWidget(new IconLabel(QStringLiteral("shield-star-line"), Role::Accent, 22, band));
    auto* words = new QVBoxLayout;
    words->setSpacing(1);
    auto* title = new QLabel(tr("Moderation").toUpper(), band);
    title->setFont(LobbyTheme::pixelFont(15, 0.5));
    words->addWidget(title);
    auto* detail = new QLabel(tr("Active bans and mutes. Signed in as %1.").arg(moderatorName), band);
    detail->setFont(LobbyTheme::uiFont(11));
    detail->setProperty("role", "muted");
    words->addWidget(detail);
    bandLay->addLayout(words, 1);
    const auto makeChip = [band](const QString& text) {
        auto* chip = new QPushButton(text, band);
        chip->setProperty("chip", true);
        chip->setCheckable(true);
        chip->setAutoDefault(false);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        return chip;
    };
    m_activeChip = makeChip(tr("Active"));
    m_activeChip->setObjectName("ModerationActive");
    m_logChip = makeChip(tr("This session's log"));
    m_logChip->setObjectName("ModerationLog");
    bandLay->addWidget(m_activeChip);
    bandLay->addWidget(m_logChip);
    root->addWidget(band);
    root->addWidget(rule(this));

    auto* pages = new QWidget(this);
    auto* pagesLay = new QVBoxLayout(pages);
    pagesLay->setContentsMargins(6, 0, 6, 6);
    pagesLay->setSpacing(0);
    m_activeHeading = new QFrame(pages);
    m_activeHeading->setObjectName("ModerationRow");
    auto* headingLay = new QHBoxLayout(m_activeHeading);
    headingLay->setContentsMargins(10, 7, 10, 7);
    headingLay->setSpacing(10);
    const auto addHeading = [this, headingLay](const QString& text, int width, int stretch) {
        auto* label = fieldLabel(text, m_activeHeading);
        if (width > 0)
            label->setFixedWidth(width);
        headingLay->addWidget(label, stretch);
    };
    addHeading(QString(), kTagColumn, 0);
    addHeading(tr("Player"), kPlayerColumn, 0);
    addHeading(tr("Reason"), 0, 1);
    addHeading(tr("By"), kByColumn, 0);
    addHeading(tr("Ends"), kEndsColumn, 0);
    addHeading(QString(), kLiftColumn, 0);
    pagesLay->addWidget(m_activeHeading);
    QWidget* activeBody = nullptr;
    QWidget* logBody = nullptr;
    m_activeBody = makeRowsArea(pages, &activeBody, &m_activeRows);
    m_logBody = makeRowsArea(pages, &logBody, &m_logRows);
    m_activeEmpty = new QLabel(tr("Nobody is banned or muted."), pages);
    m_logEmpty = new QLabel(tr("Nothing yet this session."), pages);
    for (QLabel* empty : { m_activeEmpty, m_logEmpty })
    {
        empty->setFont(LobbyTheme::uiFont(12));
        empty->setProperty("role", "muted");
        empty->setAlignment(Qt::AlignCenter);
    }
    pagesLay->addWidget(m_activeBody, 1);
    pagesLay->addWidget(m_activeEmpty, 1);
    pagesLay->addWidget(m_logBody, 1);
    pagesLay->addWidget(m_logEmpty, 1);
    root->addWidget(pages, 1);

    connect(m_activeChip, &QPushButton::clicked, this, [this]() { showPage(0); });
    connect(m_logChip, &QPushButton::clicked, this, [this]() { showPage(1); });
    showPage(0);
}

void ModerationDialog::showPage(int page)
{
    m_activeChip->setChecked(page == 0);
    m_logChip->setChecked(page == 1);
    const bool anyActive = m_activeRows->count() > 1;
    m_activeHeading->setVisible(page == 0 && anyActive);
    m_activeBody->setVisible(page == 0 && anyActive);
    m_activeEmpty->setVisible(page == 0 && !anyActive);
    m_logBody->setVisible(page == 1 && m_logCount > 0);
    m_logEmpty->setVisible(page == 1 && m_logCount == 0);
}

void ModerationDialog::setSanctions(const QJsonArray& bans, const QJsonArray& mutes)
{
    clearRows(m_activeRows);
    int position = 0;
    const auto addRows = [&](const QJsonArray& list, bool ban) {
        for (const QJsonValue& value : list)
        {
            const QJsonObject o = value.toObject();
            const QString ip = o.value("ip").toString();
            const QString name = o.value("name").toString();
            const QString reason = o.value("reason").toString();

            auto* row = new QFrame(m_activeRows->parentWidget());
            row->setObjectName("ModerationRow");
            row->setFixedHeight(46);
            auto* lay = new QHBoxLayout(row);
            lay->setContentsMargins(10, 4, 10, 4);
            lay->setSpacing(10);
            auto* tag = new StatusTag(ban ? StatusTag::Kind::Red : StatusTag::Kind::Amber, row);
            tag->setTag(ban ? StatusTag::Kind::Red : StatusTag::Kind::Amber, ban ? tr("Ban") : tr("Mute"));
            auto* tagBox = new QWidget(row);
            tagBox->setFixedWidth(kTagColumn);
            auto* tagLay = new QHBoxLayout(tagBox);
            tagLay->setContentsMargins(0, 0, 0, 0);
            tagLay->addWidget(tag);
            tagLay->addStretch(1);
            lay->addWidget(tagBox);

            auto* who = new QVBoxLayout;
            who->setContentsMargins(0, 0, 0, 0);
            who->setSpacing(0);
            auto* nameLabel = new ElidedLabel(name.isEmpty() ? Role::Muted : Role::Ink, row);
            nameLabel->setFont(LobbyTheme::uiFont(12, name.isEmpty() ? QFont::Normal : QFont::DemiBold));
            nameLabel->setText(name.isEmpty() ? tr("Name not recorded") : name);
            who->addWidget(nameLabel);
            auto* ipLabel = new QLabel(ip, row);
            ipLabel->setFont(LobbyTheme::uiFont(10));
            ipLabel->setProperty("role", "muted");
            who->addWidget(ipLabel);
            auto* whoBox = new QWidget(row);
            whoBox->setLayout(who);
            whoBox->setFixedWidth(kPlayerColumn);
            lay->addWidget(whoBox);

            auto* reasonLabel = new ElidedLabel(reason.isEmpty() ? Role::Faint : Role::Ink2, row);
            reasonLabel->setFont(LobbyTheme::uiFont(12));
            reasonLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            reasonLabel->setText(reason.isEmpty() ? tr("None given") : reason);
            reasonLabel->setToolTip(reason);
            lay->addWidget(reasonLabel, 1);

            auto* by = new QLabel(o.value("by").toString(), row);
            by->setFont(LobbyTheme::uiFont(12));
            by->setProperty("role", "ink2");
            by->setFixedWidth(kByColumn);
            lay->addWidget(by);

            auto* ends = new QLabel(untilText(static_cast<qint64>(o.value("until").toDouble())), row);
            ends->setFont(LobbyTheme::uiFont(12));
            ends->setProperty("role", "ink2");
            ends->setFixedWidth(kEndsColumn);
            lay->addWidget(ends);

            auto* lift = new LobbyButton(ban ? tr("Unban") : tr("Unmute"), LobbyButton::Kind::Secondary, row);
            lift->setObjectName(ban ? "ModerationUnban" : "ModerationUnmute");
            lift->setButtonSize(LobbyButton::Size::Small);
            lift->setAutoDefault(false);
            connect(lift, &QPushButton::clicked, this, [this, ban, ip]() {
                emit liftRequested(ban ? QStringLiteral("unban") : QStringLiteral("unmute"), ip);
            });
            lift->setFixedWidth(kLiftColumn);
            lay->addWidget(lift);
            m_activeRows->insertWidget(position++, row);
        }
    };
    addRows(bans, true);
    addRows(mutes, false);
    m_activeChip->setText(tr("Active %1").arg(position));
    showPage(m_logChip->isChecked() ? 1 : 0);
}

void ModerationDialog::addLogEntry(const QString& text)
{
    auto* row = new QFrame(m_logRows->parentWidget());
    row->setObjectName("ModerationRow");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 7, 10, 7);
    lay->setSpacing(10);
    auto* time = new QLabel(QDateTime::currentDateTime().toString("hh:mm"), row);
    time->setFont(LobbyTheme::uiFont(11));
    time->setProperty("role", "muted");
    lay->addWidget(time);
    auto* label = new ElidedLabel(Role::Ink2, row);
    label->setFont(LobbyTheme::uiFont(12));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    label->setText(text);
    label->setToolTip(text);
    lay->addWidget(label, 1);
    m_logRows->insertWidget(0, row);
    ++m_logCount;
    m_logChip->setText(tr("This session's log %1").arg(m_logCount));
    showPage(m_logChip->isChecked() ? 1 : 0);
}

// ── IgnoredPlayersDialog ─────────────────────────────────────────────

IgnoredPlayersDialog::IgnoredPlayersDialog(const QStringList& names, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Ignored players"));
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setModal(true);
    resize(340, 320);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* mark = new IconLabel(QStringLiteral("forbid-line"), Role::Muted, 22, this);
    root->addWidget(makeHeader(mark, tr("Ignored players"), tr("You don't see their chat, messages or challenges."), this));
    root->addWidget(rule(this));

    QWidget* body = nullptr;
    auto* scroll = makeRowsArea(this, &body, &m_rows);
    root->addWidget(scroll, 1);
    m_empty = new QLabel(tr("You aren't ignoring anyone."), this);
    m_empty->setFont(LobbyTheme::uiFont(12));
    m_empty->setProperty("role", "muted");
    m_empty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_empty, 1);

    for (const QString& name : names)
        addRow(name);
    m_empty->setVisible(m_count == 0);
}

void IgnoredPlayersDialog::addRow(const QString& name)
{
    auto* row = new QFrame(m_rows->parentWidget());
    row->setObjectName("ModerationRow");
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(18, 6, 12, 6);
    auto* label = new ElidedLabel(Role::Ink, row);
    label->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    label->setText(name);
    lay->addWidget(label, 1);
    auto* stop = new LobbyButton(tr("Stop ignoring"), LobbyButton::Kind::Secondary, row);
    stop->setButtonSize(LobbyButton::Size::Small);
    stop->setAutoDefault(false);
    connect(stop, &QPushButton::clicked, this, [this, row, name]() {
        emit unignoreRequested(name);
        row->hide();
        row->deleteLater();
        --m_count;
        m_empty->setVisible(m_count == 0);
    });
    lay->addWidget(stop);
    m_rows->insertWidget(m_count++, row);
}

#endif // NETPLAY
