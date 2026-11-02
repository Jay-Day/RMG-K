/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifdef NETPLAY

#include "LobbyBrowsePanels.hpp"

#include <QActionGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace UserInterface
{
namespace Dialog
{

using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    constexpr int kCardMinWidth = 250;
    constexpr int kCardHeight = 120;
    constexpr int kCardGap = 10;

    QString formatDuration(qint64 startedAtMs)
    {
        if (startedAtMs <= 0)
            return QStringLiteral("—");
        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedAtMs;
        const qint64 total = elapsedMs > 0 ? elapsedMs / 1000 : 0;
        const qint64 h = total / 3600;
        const qint64 m = (total % 3600) / 60;
        const qint64 s = total % 60;
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    }

    QWidget* makeNote(QLabel*& title, QLabel*& body, QWidget* parent)
    {
        auto* note = new QWidget(parent);
        auto* layout = new QVBoxLayout(note);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(5);
        title = new QLabel(note);
        title->setFont(LobbyTheme::pixelFont(11, 0.6));
        title->setAlignment(Qt::AlignCenter);
        title->setWordWrap(true);
        title->setProperty("role", "ink2");
        body = new QLabel(note);
        body->setFont(LobbyTheme::uiFont(12));
        body->setAlignment(Qt::AlignCenter);
        body->setWordWrap(true);
        body->setProperty("role", "muted");
        layout->addWidget(title);
        layout->addWidget(body);
        layout->addStretch(1);
        return note;
    }
} // namespace

// ── Match cards ──────────────────────────────────────────────────────

namespace
{
    // Whose name, if anyone's, is under pos. The widgets showing one carry it in
    // a "playerName" property.
    QString playerNameAt(QWidget* root, const QPoint& pos)
    {
        for (QWidget* widget = root->childAt(pos); widget && widget != root; widget = widget->parentWidget())
        {
            const QVariant name = widget->property("playerName");
            if (name.isValid())
                return name.toString();
        }
        return QString();
    }
} // namespace

// Chrome and behaviour shared by the regular and the featured card.
class MatchCardBase : public QFrame
{
    Q_OBJECT

public:
    explicit MatchCardBase(QWidget* parent)
        : QFrame(parent)
    {
        setAttribute(Qt::WA_Hover);
        setContextMenuPolicy(Qt::DefaultContextMenu);
    }

    quint64 roomId() const { return m_view.roomId; }

    virtual void apply(const LiveMatchView& view)
    {
        m_view = view;
        update();
    }

    virtual void updateDuration() = 0;

signals:
    void watchRequested(quint64 roomId);
    void stopWatchingRequested();
    void joinRequested(quint64 roomId);
    void contextMenuRequested(quint64 roomId, const QPoint& globalPos);
    void playerMenuRequested(const QString& name, const QPoint& globalPos);

protected:
    bool canWatch() const
    {
        return m_view.matchId != 0 && m_view.hasRom && !m_view.inRoom && !m_view.watching;
    }

    // The watch button's face for this match's state, shared so the two cards
    // can't disagree. Returns false when there's nothing to watch.
    bool applyWatchButton(LobbyButton* button, bool compact) const
    {
        if (m_view.matchId == 0 || !m_view.hasRom)
        {
            button->setVisible(false);
            return false;
        }
        button->setVisible(true);
        button->setEnabled(true);
        button->setToolTip(QString());
        if (m_view.watching)
        {
            button->setText(tr("Stop"));
            button->setKind(LobbyButton::Kind::Secondary);
            button->setIconName(QStringLiteral("stop-fill"));
            button->setToolTip(tr("Stop watching this match."));
        }
        else if (m_view.inRoom)
        {
            button->setText(tr("Watch"));
            button->setKind(LobbyButton::Kind::Secondary);
            button->setIconName(QStringLiteral("eye-line"));
            button->setEnabled(false);
            button->setToolTip(tr("Leave your room to watch a match."));
        }
        else if (m_view.watchingOther)
        {
            button->setText(tr("Switch"));
            button->setKind(LobbyButton::Kind::Secondary);
            button->setIconName(QStringLiteral("eye-line"));
            button->setToolTip(tr("Stop what you're watching and watch this match instead."));
        }
        else
        {
            button->setText(tr("Watch"));
            button->setKind(LobbyButton::Kind::Primary);
            button->setIconName(compact ? QString() : QStringLiteral("eye-line"));
        }
        return true;
    }

    void onWatchClicked()
    {
        if (m_view.watching)
            emit stopWatchingRequested();
        else
            emit watchRequested(m_view.roomId);
    }

    void applyDim(QWidget* content)
    {
        const bool dim = m_view.matchId == 0;
        if (dim && !content->graphicsEffect())
        {
            auto* effect = new QGraphicsOpacityEffect(content);
            effect->setOpacity(0.68);
            content->setGraphicsEffect(effect);
        }
        else if (!dim && content->graphicsEffect())
        {
            content->setGraphicsEffect(nullptr);
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const Tokens& t = LobbyTheme::tokens();
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        QColor clear = t.tint;
        clear.setAlpha(0);
        QLinearGradient fill(box.topLeft(), box.bottomLeft());
        fill.setColorAt(0, m_view.watching ? t.watchTint : t.tint);
        fill.setColorAt(1, clear);
        painter.setBrush(fill);

        QColor border = t.line;
        qreal width = 1.0;
        if (m_view.watching)
        {
            border = t.stateWatch;
            width = 2.0;
        }
        else if (m_view.matchId != 0)
        {
            border = LobbyTheme::mix(t.live, t.line, 0.28);
        }
        if (underMouse() && canWatch())
            border = t.line2.lighter(t.dark ? 130 : 90);
        painter.setPen(QPen(border, width));
        painter.drawRoundedRect(box.adjusted(width / 2 - 0.5, width / 2 - 0.5, 0.5 - width / 2, 0.5 - width / 2), 10, 10);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && canWatch())
            emit watchRequested(m_view.roomId);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        const QString player = playerNameAt(this, event->pos());
        if (!player.isEmpty())
            emit playerMenuRequested(player, event->globalPos());
        else
            emit contextMenuRequested(m_view.roomId, event->globalPos());
    }

    LiveMatchView m_view;
};

// A port chip, a name and a flag on one line.
class PlayerLine : public QWidget
{
public:
    explicit PlayerLine(QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        m_chip = new PortChip(1, this);
        m_name = new ElidedLabel(Role::Ink, this);
        m_flag = new CountryFlag(this);
        layout->addWidget(m_chip);
        layout->addWidget(m_name);
        layout->addWidget(m_flag);
        layout->addStretch(1);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void set(int slot, const LiveMatchView::Player& player, bool compact)
    {
        m_chip->setSlot(slot);
        m_name->setFont(LobbyTheme::uiFont(compact ? 12 : 13, QFont::DemiBold));
        m_name->setText(player.name);
        m_name->setToolTip(player.name);
        setProperty("playerName", player.name);
        m_flag->setCountry(compact ? QString() : player.country);
    }

    QSize sizeHint() const override
    {
        return QSize(40, std::max(15, QFontMetrics(LobbyTheme::uiFont(13, QFont::DemiBold)).height()));
    }

private:
    PortChip* m_chip;
    ElidedLabel* m_name;
    CountryFlag* m_flag;
};

class MatchCard : public MatchCardBase
{
    Q_OBJECT

public:
    explicit MatchCard(QWidget* parent)
        : MatchCardBase(parent)
    {
        auto* outer = new QHBoxLayout(this);
        outer->setContentsMargins(10, 9, 10, 9);
        outer->setSpacing(10);

        m_tile = new CartridgeTile(CartridgeTile::Size::Normal, this);
        outer->addWidget(m_tile, 0, Qt::AlignTop);

        m_content = new QWidget(this);
        auto* column = new QVBoxLayout(m_content);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);

        auto* top = new QHBoxLayout;
        top->setSpacing(5);
        m_game = new ElidedLabel(Role::Muted, m_content);
        m_game->setFont(LobbyTheme::uiFont(11));
        m_game->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_status = new StatusTag(StatusTag::Kind::Live, m_content);
        top->addWidget(m_game, 1);
        top->addWidget(m_status);
        column->addLayout(top);

        m_players = new QGridLayout;
        m_players->setContentsMargins(0, 0, 0, 0);
        m_players->setHorizontalSpacing(8);
        m_players->setVerticalSpacing(2);
        for (auto& line : m_lines)
            line = new PlayerLine(m_content);
        m_openSeats = new ElidedLabel(Role::Muted, m_content);
        QFont openFont = LobbyTheme::uiFont(13);
        openFont.setItalic(true);
        m_openSeats->setFont(openFont);
        m_openSeats->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_fallbackName = new ElidedLabel(Role::Ink, m_content);
        m_fallbackName->setFont(LobbyTheme::uiFont(13, QFont::DemiBold));
        m_fallbackName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        column->addStretch(1);
        column->addLayout(m_players);
        column->addStretch(1);

        auto* footer = new QHBoxLayout;
        footer->setSpacing(8);
        m_time = new QLabel(m_content);
        m_time->setFont(LobbyTheme::pixelFont(11));
        m_time->setProperty("role", "ink2");
        footer->addWidget(m_time);
        m_viewerIcon = new IconLabel(QStringLiteral("eye-line"), Role::Muted, 13, m_content);
        m_viewers = new QLabel(m_content);
        m_viewers->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        m_viewers->setProperty("role", "muted");
        auto* viewerBox = new QHBoxLayout;
        viewerBox->setSpacing(3);
        viewerBox->addWidget(m_viewerIcon);
        viewerBox->addWidget(m_viewers);
        footer->addLayout(viewerBox);
        footer->addStretch(1);

        m_noRom = new QLabel(tr("No ROM"), m_content);
        m_noRom->setObjectName("NoRomLabel");
        m_noRom->setFont(LobbyTheme::uiFont(10, QFont::DemiBold));
        m_noRom->setToolTip(tr("You don't have the ROM this match is using."));
        footer->addWidget(m_noRom);

        m_watch = new LobbyButton(tr("Watch"), LobbyButton::Kind::Primary, m_content);
        m_watch->setObjectName("MatchWatchButton");
        m_watch->setButtonSize(LobbyButton::Size::Small);
        footer->addWidget(m_watch);
        m_join = new LobbyButton(tr("Join"), LobbyButton::Kind::Secondary, m_content);
        m_join->setObjectName("MatchJoinButton");
        m_join->setButtonSize(LobbyButton::Size::Small);
        m_join->setToolTip(tr("Take the free seat. The host can then add you to the game."));
        footer->addWidget(m_join);
        column->addLayout(footer);

        outer->addWidget(m_content, 1);

        connect(m_watch, &QPushButton::clicked, this, [this]() { onWatchClicked(); });
        connect(m_join, &QPushButton::clicked, this, [this]() { emit joinRequested(m_view.roomId); });
    }

    void apply(const LiveMatchView& view) override
    {
        MatchCardBase::apply(view);
        const bool crowded = view.players.size() > 2;

        m_tile->setTileSize(crowded ? CartridgeTile::Size::Small : CartridgeTile::Size::Normal);
        m_tile->setGame(view.game, view.coverPath);
        m_game->setText(view.game);
        m_game->setToolTip(view.roomName.isEmpty() ? view.game : view.roomName + QStringLiteral(" · ") + view.game);

        if (view.watching)
            m_status->setTag(StatusTag::Kind::Purple, tr("Watching"));
        else if (view.matchId == 0)
            m_status->setTag(StatusTag::Kind::Neutral, tr("No replay"));
        else
            m_status->setTag(StatusTag::Kind::Live, tr("Live"));

        // Up to two players stack with their flags; three or four share a grid.
        while (m_players->count() > 0)
            m_players->takeAt(0);
        for (auto* line : m_lines)
            line->setVisible(false);
        m_openSeats->setVisible(false);
        m_fallbackName->setVisible(false);

        int row = 0, col = 0;
        const auto place = [&](QWidget* widget) {
            m_players->addWidget(widget, row, col);
            if (crowded)
            {
                col = (col + 1) % 2;
                row += col == 0 ? 1 : 0;
            }
            else
            {
                ++row;
            }
        };
        const int shown = std::min<int>(view.players.size(), 4);
        for (int i = 0; i < shown; ++i)
        {
            m_lines[i]->set(i + 1, view.players.at(i), crowded);
            m_lines[i]->setVisible(true);
            place(m_lines[i]);
        }
        if (shown == 0)
        {
            m_fallbackName->setText(view.roomName);
            m_fallbackName->setVisible(true);
            place(m_fallbackName);
        }
        if (view.openSeats > 0 && (shown < 4))
        {
            m_openSeats->setFont([crowded]() {
                QFont f = LobbyTheme::uiFont(crowded ? 12 : 13);
                f.setItalic(true);
                return f;
            }());
            m_openSeats->setText(view.openSeats == 1 ? tr("1 seat open") : tr("%1 seats open").arg(view.openSeats));
            m_openSeats->setVisible(true);
            place(m_openSeats);
        }

        const bool streaming = view.matchId != 0;
        m_viewerIcon->setVisible(streaming);
        m_viewers->setVisible(streaming);
        m_viewers->setText(QString::number(view.viewers));
        updateDuration();

        const bool canJoin = view.openSeats > 0 && view.openSeats >= view.seatsNeeded && view.hasRom && !view.inRoom &&
                             !view.watching && !view.watchingOther;
        applyWatchButton(m_watch, canJoin);
        m_noRom->setVisible(!view.hasRom);
        m_join->setVisible(canJoin);
        applyDim(m_content);
    }

    void updateDuration() override
    {
        m_time->setText(formatDuration(m_view.startedAtMs));
    }

private:
    CartridgeTile* m_tile = nullptr;
    QWidget* m_content = nullptr;
    ElidedLabel* m_game = nullptr;
    StatusTag* m_status = nullptr;
    QGridLayout* m_players = nullptr;
    PlayerLine* m_lines[4] = {};
    ElidedLabel* m_openSeats = nullptr;
    ElidedLabel* m_fallbackName = nullptr;
    QLabel* m_time = nullptr;
    IconLabel* m_viewerIcon = nullptr;
    QLabel* m_viewers = nullptr;
    QLabel* m_noRom = nullptr;
    LobbyButton* m_watch = nullptr;
    LobbyButton* m_join = nullptr;
};

// The double-width card for the match with the biggest audience.
class FeaturedMatchCard : public MatchCardBase
{
    Q_OBJECT

public:
    explicit FeaturedMatchCard(QWidget* parent)
        : MatchCardBase(parent)
    {
        auto* outer = new QHBoxLayout(this);
        outer->setContentsMargins(12, 11, 12, 11);
        outer->setSpacing(11);

        m_tile = new CartridgeTile(CartridgeTile::Size::Large, this);
        outer->addWidget(m_tile, 0, Qt::AlignVCenter);

        auto* middle = new QVBoxLayout;
        middle->setSpacing(8);
        auto* tags = new QHBoxLayout;
        tags->setSpacing(6);
        m_status = new StatusTag(StatusTag::Kind::Live, this);
        m_mostTag = new StatusTag(StatusTag::Kind::Amber, this);
        m_mostTag->setTag(StatusTag::Kind::Amber, tr("Most watched"));
        m_game = new ElidedLabel(Role::Muted, this);
        m_game->setFont(LobbyTheme::uiFont(11));
        m_game->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        tags->addWidget(m_status);
        tags->addWidget(m_mostTag);
        tags->addWidget(m_game, 1);
        middle->addStretch(1);
        middle->addLayout(tags);

        auto* versus = new QGridLayout;
        versus->setHorizontalSpacing(7);
        versus->setColumnStretch(0, 1);
        versus->setColumnStretch(2, 1);
        versus->addLayout(makeSide(0), 0, 0);
        auto* vs = new QLabel(QStringLiteral("VS"), this);
        QFont vsFont(QStringLiteral("Arial Black"));
        vsFont.setPixelSize(20);
        vsFont.setItalic(true);
        vsFont.setWeight(QFont::Black);
        vs->setFont(vsFont);
        vs->setProperty("role", "faint");
        versus->addWidget(vs, 0, 1, Qt::AlignCenter);
        versus->addLayout(makeSide(1), 0, 2);
        middle->addLayout(versus);
        middle->addStretch(1);
        outer->addLayout(middle, 1);

        auto* right = new QVBoxLayout;
        right->setSpacing(4);
        right->addStretch(1);
        m_viewers = new QLabel(this);
        m_viewers->setFont(LobbyTheme::pixelFont(22));
        m_viewers->setAlignment(Qt::AlignRight);
        right->addWidget(m_viewers);
        auto* watchingWord = new QLabel(tr("watching").toUpper(), this);
        QFont wordFont = LobbyTheme::uiFont(9, QFont::DemiBold);
        wordFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        watchingWord->setFont(wordFont);
        watchingWord->setAlignment(Qt::AlignRight);
        watchingWord->setProperty("role", "muted");
        right->addWidget(watchingWord);
        m_time = new QLabel(this);
        m_time->setFont(LobbyTheme::pixelFont(11));
        m_time->setAlignment(Qt::AlignRight);
        m_time->setProperty("role", "ink2");
        right->addWidget(m_time);
        m_watch = new LobbyButton(tr("Watch"), LobbyButton::Kind::Primary, this);
        m_watch->setObjectName("MatchWatchButton");
        m_watch->setButtonSize(LobbyButton::Size::Medium);
        m_watch->setPixelLabel(true);
        right->addSpacing(2);
        right->addWidget(m_watch, 0, Qt::AlignRight);
        right->addStretch(1);
        outer->addLayout(right);

        connect(m_watch, &QPushButton::clicked, this, [this]() { onWatchClicked(); });
    }

    void apply(const LiveMatchView& view) override
    {
        MatchCardBase::apply(view);
        m_tile->setGame(view.game, view.coverPath);
        if (view.watching)
            m_status->setTag(StatusTag::Kind::Purple, tr("Watching"));
        else
            m_status->setTag(StatusTag::Kind::Live, tr("Live"));
        m_game->setText(view.roomName.isEmpty() || view.roomName == view.game
            ? view.game : view.game + QStringLiteral(" · ") + view.roomName);
        m_game->setToolTip(m_game->text());

        for (int i = 0; i < 2; ++i)
        {
            const bool known = i < view.players.size();
            m_sideNames[i]->setText(known ? view.players.at(i).name : QString());
            m_sideNames[i]->setToolTip(m_sideNames[i]->text());
            m_sideNames[i]->setProperty("playerName", m_sideNames[i]->text());
            m_sideFlags[i]->setCountry(known ? view.players.at(i).country : QString());
        }
        m_viewers->setText(QString::number(view.viewers));
        updateDuration();
        applyWatchButton(m_watch, false);
    }

    void updateDuration() override
    {
        m_time->setText(formatDuration(m_view.startedAtMs));
    }

private:
    QHBoxLayout* makeSide(int index)
    {
        auto* side = new QHBoxLayout;
        side->setSpacing(6);
        auto* chip = new PortChip(index + 1, this);
        m_sideNames[index] = new ElidedLabel(Role::Ink, this);
        m_sideNames[index]->setFont(LobbyTheme::uiFont(14, QFont::Bold));
        m_sideFlags[index] = new CountryFlag(this);
        if (index == 0)
        {
            side->addWidget(chip);
            side->addWidget(m_sideNames[index]);
            side->addWidget(m_sideFlags[index]);
            side->addStretch(1);
        }
        else
        {
            m_sideNames[index]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            side->addStretch(1);
            side->addWidget(m_sideFlags[index]);
            side->addWidget(m_sideNames[index]);
            side->addWidget(chip);
        }
        return side;
    }

    CartridgeTile* m_tile = nullptr;
    StatusTag* m_status = nullptr;
    StatusTag* m_mostTag = nullptr;
    ElidedLabel* m_game = nullptr;
    ElidedLabel* m_sideNames[2] = {};
    CountryFlag* m_sideFlags[2] = {};
    QLabel* m_viewers = nullptr;
    QLabel* m_time = nullptr;
    LobbyButton* m_watch = nullptr;
};

// ── LiveGallery ──────────────────────────────────────────────────────

LiveGallery::LiveGallery(QWidget* parent)
    : LobbyCard(parent)
{
    setObjectName("LiveGallery");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    header->setFixedHeight(42);
    auto* head = new QHBoxLayout(header);
    head->setContentsMargins(12, 0, 12, 0);
    head->setSpacing(6);
    head->addWidget(new SectionMarker(SectionMarker::Kind::OnAir, header));
    head->addWidget(makeSectionLabel(tr("Live now"), header));
    m_countLabel = new QLabel(header);
    m_countLabel->setFont(LobbyTheme::uiFont(11, QFont::Bold));
    m_countLabel->setProperty("role", "muted");
    head->addWidget(m_countLabel);
    head->addStretch(1);

    const auto makeChip = [header](const QString& name) {
        auto* chip = new QPushButton(header);
        chip->setObjectName(name);
        chip->setProperty("chip", true);
        chip->setCheckable(true);
        chip->setAutoDefault(false);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFocusPolicy(Qt::NoFocus);
        chip->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
        return chip;
    };
    m_allChip = makeChip(QStringLiteral("LiveAllChip"));
    m_allChip->setChecked(true);
    m_thisGameChip = makeChip(QStringLiteral("LiveThisGameChip"));
    m_thisGameChip->setToolTip(tr("Only show matches of the game selected in the picker."));
    m_thisGameChip->setEnabled(false);
    head->addWidget(m_allChip);
    head->addWidget(m_thisGameChip);
    connect(m_allChip, &QPushButton::clicked, this, [this]() {
        m_thisGameOnly = false;
        m_allChip->setChecked(true);
        m_thisGameChip->setChecked(false);
        rebuild();
    });
    connect(m_thisGameChip, &QPushButton::clicked, this, [this]() {
        m_thisGameOnly = true;
        m_allChip->setChecked(false);
        m_thisGameChip->setChecked(true);
        rebuild();
    });

    m_sortCombo = new QComboBox(header);
    m_sortCombo->setObjectName("LiveSort");
    m_sortCombo->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
    m_sortCombo->setFocusPolicy(Qt::NoFocus);
    m_sortCombo->addItem(tr("Most watched"), static_cast<int>(Sort::MostWatched));
    m_sortCombo->addItem(tr("Newest"), static_cast<int>(Sort::Newest));
    m_sortCombo->addItem(tr("Longest running"), static_cast<int>(Sort::Longest));
    {
        QSettings settings("RMG-K", "n02");
        const int saved = m_sortCombo->findData(settings.value("RollbackLobby/LiveSort", 0).toInt());
        if (saved >= 0)
            m_sortCombo->setCurrentIndex(saved);
    }
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        QSettings settings("RMG-K", "n02");
        settings.setValue("RollbackLobby/LiveSort", m_sortCombo->currentData());
        m_order.clear();
        rebuild();
    });
    head->addWidget(m_sortCombo);
    root->addWidget(header);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("LiveScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_grid = new QWidget(m_scroll);
    m_grid->setObjectName("LiveGrid");
    m_grid->setAttribute(Qt::WA_Hover);
    m_grid->installEventFilter(this);
    m_scroll->setWidget(m_grid);
    m_grid->setAutoFillBackground(false); // setWidget turns it on
    m_scroll->viewport()->installEventFilter(this);
    root->addWidget(m_scroll, 1);

    m_featured = new FeaturedMatchCard(m_grid);
    connectCard(m_featured);
    m_featured->setVisible(false);

    m_note = makeNote(m_noteTitle, m_noteBody, m_grid);

    rebuild();
}

void LiveGallery::connectCard(MatchCardBase* card)
{
    connect(card, &MatchCardBase::watchRequested, this, &LiveGallery::watchRequested);
    connect(card, &MatchCardBase::stopWatchingRequested, this, &LiveGallery::stopWatchingRequested);
    connect(card, &MatchCardBase::joinRequested, this, &LiveGallery::joinRequested);
    connect(card, &MatchCardBase::contextMenuRequested, this, &LiveGallery::contextMenuRequested);
    connect(card, &MatchCardBase::playerMenuRequested, this, &LiveGallery::playerMenuRequested);
}

void LiveGallery::setMatches(const QList<LiveMatchView>& matches)
{
    m_matches = matches;
    rebuild();
}

void LiveGallery::setThisGameAvailable(bool available)
{
    m_thisGameChip->setEnabled(available);
    if (!available && m_thisGameOnly)
    {
        m_thisGameOnly = false;
        m_allChip->setChecked(true);
        m_thisGameChip->setChecked(false);
    }
}

void LiveGallery::setOffline(bool offline)
{
    m_offline = offline;
    rebuild();
}

void LiveGallery::updateDurations()
{
    if (m_featuredVisible)
        m_featured->updateDuration();
    for (auto* card : m_cards)
        card->updateDuration();
}

QList<LiveMatchView> LiveGallery::visibleMatches() const
{
    QList<LiveMatchView> visible;
    for (const auto& match : m_matches)
    {
        if (!m_thisGameOnly || match.sameGame)
            visible.append(match);
    }

    const Sort sort = static_cast<Sort>(m_sortCombo->currentData().toInt());
    std::stable_sort(visible.begin(), visible.end(), [sort](const LiveMatchView& a, const LiveMatchView& b) {
        if (sort == Sort::Newest && a.startedAtMs != b.startedAtMs)
            return a.startedAtMs > b.startedAtMs;
        if (sort == Sort::Longest && a.startedAtMs != b.startedAtMs)
            return a.startedAtMs < b.startedAtMs;
        // Matches you can't watch sink below every one you can, even an empty one.
        const int viewersA = a.matchId != 0 ? a.viewers : -1;
        const int viewersB = b.matchId != 0 ? b.viewers : -1;
        if (viewersA != viewersB)
            return viewersA > viewersB;
        if (a.startedAtMs != b.startedAtMs)
            return a.startedAtMs < b.startedAtMs;
        return a.roomId < b.roomId;
    });

    // While the pointer is over the gallery, a viewer count ticking up mustn't
    // slide a card out from under it. Known matches hold their places and new
    // ones join the end; the real order comes back once the pointer leaves.
    if (m_grid->underMouse() && !m_order.isEmpty())
    {
        QList<LiveMatchView> held;
        for (const quint64 id : m_order)
        {
            const auto it = std::find_if(visible.cbegin(), visible.cend(),
                                         [id](const LiveMatchView& m) { return m.roomId == id; });
            if (it != visible.cend())
                held.append(*it);
        }
        for (const auto& match : visible)
        {
            if (!m_order.contains(match.roomId))
                held.append(match);
        }
        return held;
    }
    return visible;
}

int LiveGallery::columnCount() const
{
    const int width = m_scroll->viewport()->width() - 2 * 12;
    return std::max(1, (width + kCardGap) / (kCardMinWidth + kCardGap));
}

void LiveGallery::rebuild()
{
    const QList<LiveMatchView> visible = visibleMatches();
    m_builtColumns = columnCount();

    m_order.clear();
    for (const auto& match : visible)
        m_order.append(match.roomId);

    int totalViewers = 0;
    int sameGame = 0;
    for (const auto& match : m_matches)
    {
        totalViewers += match.matchId != 0 ? match.viewers : 0;
        sameGame += match.sameGame ? 1 : 0;
    }
    m_countLabel->setText(QStringLiteral("%1 · %2 %3")
        .arg(m_matches.size()).arg(totalViewers).arg(tr("watching").toUpper()));
    m_allChip->setText(QStringLiteral("%1  %2").arg(tr("All")).arg(m_matches.size()));
    m_thisGameChip->setText(QStringLiteral("%1  %2").arg(tr("This game")).arg(sameGame));

    // The most watched 1v1 earns the wide card, when there's room to span two
    // columns and somebody is actually watching it.
    const Sort sort = static_cast<Sort>(m_sortCombo->currentData().toInt());
    quint64 featuredId = 0;
    if (sort == Sort::MostWatched && m_builtColumns >= 2 && !visible.isEmpty())
    {
        const LiveMatchView& top = visible.first();
        if (top.matchId != 0 && top.viewers > 0 && top.players.size() == 2 && top.hasRom)
            featuredId = top.roomId;
    }
    m_featuredRoomId = featuredId;
    m_featuredVisible = featuredId != 0;
    m_featured->setVisible(m_featuredVisible);

    QHash<quint64, MatchCard*> kept;
    for (const auto& match : visible)
    {
        if (match.roomId == featuredId)
        {
            m_featured->apply(match);
            continue;
        }
        MatchCard* card = m_cards.take(match.roomId);
        if (card == nullptr)
        {
            card = new MatchCard(m_grid);
            connectCard(card);
        }
        card->apply(match);
        card->setVisible(true);
        kept.insert(match.roomId, card);
    }
    // Later, not now: this can run inside a click on the very card being dropped.
    for (auto* card : m_cards)
    {
        card->hide();
        card->deleteLater();
    }
    m_cards = kept;

    // The note says why the gallery is short, or empty.
    if (m_offline)
    {
        m_noteTitle->setText(tr("Not connected").toUpper());
        m_noteBody->setText(tr("Connect to the lobby to see running matches."));
    }
    else if (visible.isEmpty())
    {
        m_noteTitle->setText((m_thisGameOnly && !m_matches.isEmpty()
            ? tr("Nothing live for this game") : tr("Nothing live right now")).toUpper());
        m_noteBody->setText(tr("New matches appear here as they start. Rooms with Live replay on can be watched by anyone."));
    }
    else
    {
        m_noteTitle->setText(tr("That's everything live right now").toUpper());
        m_noteBody->setText(tr("New matches appear here as they start. Rooms with Live replay on can be watched by anyone."));
    }

    relayout();
}

void LiveGallery::relayout()
{
    const int columns = columnCount();
    const int width = m_scroll->viewport()->width();
    const int inner = std::max(kCardMinWidth, width - 2 * 12);
    const int cellWidth = (inner - (columns - 1) * kCardGap) / columns;

    int row = 0;
    int column = 0;
    const auto advance = [&](int span) {
        column += span;
        if (column >= columns)
        {
            column = 0;
            ++row;
        }
    };
    const auto cellX = [&](int c) { return 12 + c * (cellWidth + kCardGap); };
    const auto cellY = [&](int r) { return r * (kCardHeight + kCardGap); };

    for (const quint64 id : m_order)
    {
        if (id == m_featuredRoomId && m_featuredVisible)
        {
            const int span = std::min(2, columns);
            m_featured->setGeometry(cellX(column), cellY(row),
                                    span * cellWidth + (span - 1) * kCardGap, kCardHeight);
            advance(span);
            continue;
        }
        if (MatchCard* card = m_cards.value(id))
        {
            card->setGeometry(cellX(column), cellY(row), cellWidth, kCardHeight);
            advance(1);
        }
    }
    const int nextRow = column == 0 ? row : row + 1;

    // A short gallery explains itself; a full one doesn't need to.
    const int viewportHeight = m_scroll->viewport()->height();
    const int cardsHeight = nextRow > 0 ? cellY(nextRow) : 0;
    const bool roomForNote = m_order.isEmpty() || viewportHeight - cardsHeight >= 100;
    m_note->setVisible(roomForNote);
    if (roomForNote)
        m_note->setGeometry(12, cardsHeight, inner, std::max(100, viewportHeight - cardsHeight));

    m_grid->setMinimumHeight(cardsHeight + (roomForNote ? 100 : 2));
}

void LiveGallery::resizeEvent(QResizeEvent* event)
{
    LobbyCard::resizeEvent(event);
    // A narrow gallery sheds its header's extras, least useful first.
    m_countLabel->setVisible(width() >= 500);
    m_sortCombo->setVisible(width() >= 400);
    m_thisGameChip->setVisible(width() >= 320);
}

bool LiveGallery::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_scroll->viewport() && event->type() == QEvent::Resize)
    {
        if (columnCount() != m_builtColumns)
            rebuild();
        else
            relayout();
    }
    else if (watched == m_grid && event->type() == QEvent::Leave)
    {
        rebuild(); // the real order was on hold while the pointer was here
    }
    return LobbyCard::eventFilter(watched, event);
}

// ── Room rows ────────────────────────────────────────────────────────

class RoomRow : public QFrame
{
    Q_OBJECT

public:
    explicit RoomRow(QWidget* parent)
        : QFrame(parent)
    {
        setAttribute(Qt::WA_Hover);
        setFixedHeight(68);

        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(12, 7, 10, 7);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(1);

        auto* nameRow = new QHBoxLayout;
        nameRow->setSpacing(5);
        m_lock = new IconLabel(QStringLiteral("lock-fill"), Role::Muted, 12, this);
        m_inGame = new StatusTag(StatusTag::Kind::Red, this);
        m_inGame->setTag(StatusTag::Kind::Red, tr("In game"));
        m_name = new ElidedLabel(Role::Ink, this);
        m_name->setFont(LobbyTheme::uiFont(13, QFont::DemiBold));
        m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        nameRow->addWidget(m_lock);
        nameRow->addWidget(m_inGame);
        nameRow->addWidget(m_name, 1);
        grid->addLayout(nameRow, 0, 0);

        m_game = new ElidedLabel(Role::Muted, this);
        m_game->setFont(LobbyTheme::uiFont(11));
        m_game->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        grid->addWidget(m_game, 1, 0);

        m_join = new LobbyButton(tr("Join"), LobbyButton::Kind::Secondary, this);
        m_join->setObjectName("RoomJoinButton");
        m_join->setButtonSize(LobbyButton::Size::Small);
        grid->addWidget(m_join, 0, 1, 2, 1, Qt::AlignVCenter | Qt::AlignRight);

        auto* meta = new QHBoxLayout;
        meta->setSpacing(6);
        m_flag = new CountryFlag(this);
        m_host = new ElidedLabel(Role::Muted, this);
        m_host->setFont(LobbyTheme::uiFont(11));
        m_host->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_pips = new SeatPips(this);
        m_seats = new QLabel(this);
        m_seats->setFont(LobbyTheme::uiFont(11));
        m_seats->setProperty("role", "muted");
        m_seats->setMinimumWidth(22);
        m_bars = new SignalBars(this);
        m_ping = new QLabel(this);
        m_ping->setFont(LobbyTheme::uiFont(11));
        m_ping->setProperty("role", "muted");
        m_ping->setMinimumWidth(22);
        meta->addWidget(m_flag);
        meta->addWidget(m_host, 1);
        meta->addWidget(m_pips);
        meta->addWidget(m_seats);
        meta->addWidget(m_bars);
        meta->addWidget(m_ping);
        grid->addLayout(meta, 2, 0, 1, 2);
        grid->setColumnStretch(0, 1);

        connect(m_join, &QPushButton::clicked, this, [this]() {
            if (m_view.mine)
                emit returnRequested();
            else
                emit joinRequested(m_view.roomId);
        });
    }

    void apply(const RoomRowView& view)
    {
        m_view = view;
        m_lock->setVisible(view.hasPassword);
        m_inGame->setVisible(view.inGame);
        m_name->setText(view.name);
        m_name->setToolTip(view.name);
        m_game->setText(view.game);
        m_game->setToolTip(view.game);
        m_flag->setCountry(view.hostCountry);
        m_host->setText(view.host);
        m_host->setProperty("playerName", view.host);
        m_pips->setSeats(view.players, view.maxPlayers);
        m_seats->setText(QString("%1/%2").arg(view.players).arg(view.maxPlayers));
        m_bars->setPing(view.pingMs, !view.pingMeasured);
        m_ping->setText(view.pingMs >= 0
            ? QString(view.pingMeasured ? "%1" : "~%1").arg(view.pingMs)
            : QStringLiteral("—"));
        const QString pingTip =
            view.pingMeasured ? tr("Your measured ping to the host")
            : view.pingMs >= 0 ? tr("Estimated from regions — click the row to measure")
                               : tr("Click the row to measure your ping to the host");
        m_bars->setToolTip(pingTip);
        m_ping->setToolTip(pingTip);

        const bool full = view.maxPlayers > 0 && view.players >= view.maxPlayers;
        // Everyone on this PC sits down together or nobody does.
        const int freeSeats = view.maxPlayers - view.players;
        const bool tooFewSeats = view.maxPlayers > 0 && !full && view.seatsNeeded > freeSeats;
        m_name->setRole(view.mine ? Role::Accent : view.hasRom ? Role::Ink : Role::Muted);
        m_game->setRole(view.hasRom ? Role::Muted : Role::Faint);
        m_join->setDashed(!view.mine && (!view.hasRom || tooFewSeats));
        m_join->setIconName(view.hasPassword && !view.mine && view.hasRom && !full && !tooFewSeats
            ? QStringLiteral("lock-line") : QString());
        m_join->setEnabled(view.mine || (view.hasRom && !full && !tooFewSeats));
        if (view.mine)
        {
            m_join->setText(tr("Return"));
            m_join->setToolTip(tr("Back to your room."));
        }
        else if (!view.hasRom)
        {
            m_join->setText(tr("No ROM"));
            m_join->setToolTip(tr("You don't have the ROM this room is using."));
        }
        else if (full)
        {
            m_join->setText(tr("Full"));
            m_join->setToolTip(QString());
        }
        else if (tooFewSeats)
        {
            m_join->setText(tr("Needs %1 seats").arg(view.seatsNeeded));
            m_join->setToolTip(freeSeats == 1
                ? tr("You have %1 players on this PC, and this room has 1 free seat.").arg(view.seatsNeeded)
                : tr("You have %1 players on this PC, and this room has %2 free seats.").arg(view.seatsNeeded).arg(freeSeats));
        }
        else
        {
            m_join->setText(tr("Join"));
            m_join->setToolTip(view.inGame
                ? tr("Match in progress. Join, and the host can add you to the game.")
                : QString());
        }
        refreshSelection();
        update();
    }

    void setSelected(bool selected)
    {
        m_selected = selected;
        refreshSelection();
        update();
    }

    const RoomRowView& view() const { return m_view; }

signals:
    void joinRequested(quint64 roomId);
    void returnRequested();
    void clicked(quint64 roomId);
    void contextMenuRequested(quint64 roomId, const QPoint& globalPos);
    void playerMenuRequested(const QString& name, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const Tokens& t = LobbyTheme::tokens();
        if (m_selected)
        {
            QColor clear = t.accentTint;
            clear.setAlpha(0);
            QLinearGradient fill(0, 0, width(), 0);
            fill.setColorAt(0, t.accentTint);
            fill.setColorAt(1, clear);
            painter.fillRect(rect(), fill);
            painter.fillRect(QRect(0, 0, 3, height()), t.accent);
        }
        else if (underMouse())
        {
            painter.fillRect(rect(), t.tint);
        }
        painter.fillRect(QRect(0, height() - 1, width(), 1), t.line);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
            emit clicked(m_view.roomId);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_view.mine)
            emit returnRequested();
        else
            emit joinRequested(m_view.roomId);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        const QString player = playerNameAt(this, event->pos());
        if (!player.isEmpty())
            emit playerMenuRequested(player, event->globalPos());
        else
            emit contextMenuRequested(m_view.roomId, event->globalPos());
    }

private:
    void refreshSelection()
    {
        m_join->setKind(m_selected ? LobbyButton::Kind::Primary : LobbyButton::Kind::Secondary);
    }

    RoomRowView m_view;
    bool m_selected = false;
    IconLabel* m_lock = nullptr;
    StatusTag* m_inGame = nullptr;
    ElidedLabel* m_name = nullptr;
    ElidedLabel* m_game = nullptr;
    LobbyButton* m_join = nullptr;
    CountryFlag* m_flag = nullptr;
    ElidedLabel* m_host = nullptr;
    SeatPips* m_pips = nullptr;
    QLabel* m_seats = nullptr;
    SignalBars* m_bars = nullptr;
    QLabel* m_ping = nullptr;
};

// ── RoomsColumn ──────────────────────────────────────────────────────

RoomsColumn::RoomsColumn(QWidget* parent)
    : LobbyCard(parent)
{
    setObjectName("RoomsColumn");
    {
        QSettings settings("RMG-K", "n02");
        const int saved = settings.value("RollbackLobby/RoomsSort", 0).toInt();
        if (saved >= 0 && saved <= static_cast<int>(Sort::Name))
            m_sort = static_cast<Sort>(saved);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName("FieldHeader");
    header->setFixedHeight(42);
    auto* head = new QHBoxLayout(header);
    head->setContentsMargins(12, 0, 8, 0);
    head->setSpacing(6);
    head->addWidget(new SectionMarker(SectionMarker::Kind::Plain, header));
    head->addWidget(makeSectionLabel(tr("Open rooms"), header));
    m_countLabel = new QLabel(header);
    m_countLabel->setFont(LobbyTheme::uiFont(11, QFont::Bold));
    m_countLabel->setProperty("role", "muted");
    head->addWidget(m_countLabel);
    head->addStretch(1);
    m_filterButton = new IconButton(QStringLiteral("filter-3-line"), 26, header);
    m_filterButton->setToolTip(tr("Filter and sort rooms"));
    m_filterButton->setAccessibleName(tr("Filter and sort rooms"));
    head->addWidget(m_filterButton);
    m_searchButton = new IconButton(QStringLiteral("search-line"), 26, header);
    m_searchButton->setCheckable(true);
    m_searchButton->setToolTip(tr("Search rooms"));
    m_searchButton->setAccessibleName(tr("Search rooms"));
    head->addWidget(m_searchButton);
    root->addWidget(header);

    auto* rule = new QFrame(this);
    rule->setObjectName("FieldRule");
    root->addWidget(rule);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName("RoomsSearch");
    m_searchEdit->setPlaceholderText(tr("Room, host or game…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setVisible(false);
    auto* searchWrap = new QVBoxLayout;
    searchWrap->setContentsMargins(8, 6, 8, 6);
    searchWrap->addWidget(m_searchEdit);
    root->addLayout(searchWrap);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { rebuild(); });
    connect(m_searchButton, &QToolButton::toggled, this, [this, searchWrap](bool on) {
        m_searchEdit->setVisible(on);
        searchWrap->setContentsMargins(8, on ? 6 : 0, 8, on ? 6 : 0);
        if (on)
        {
            m_searchEdit->setFocus();
        }
        else
        {
            m_searchEdit->clear();
        }
    });
    searchWrap->setContentsMargins(8, 0, 8, 0);

    connect(m_filterButton, &QToolButton::clicked, this, &RoomsColumn::showFilterMenu);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("RoomsScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_list = new QWidget(m_scroll);
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(0);
    m_note = makeNote(m_noteTitle, m_noteBody, m_list);
    m_listLayout->addWidget(m_note);
    m_listLayout->addStretch(1);
    m_scroll->setWidget(m_list);
    m_list->setAutoFillBackground(false); // setWidget turns it on
    root->addWidget(m_scroll, 1);

    rebuild();
}

void RoomsColumn::showFilterMenu()
{
    QMenu menu(this);
    QAction* thisGame = menu.addAction(tr("This game only"));
    thisGame->setCheckable(true);
    thisGame->setChecked(m_thisGameOnly);
    thisGame->setEnabled(m_thisGameAvailable);
    menu.addSeparator();

    auto* group = new QActionGroup(&menu);
    const auto addSort = [&](const QString& label, Sort sort) {
        QAction* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(m_sort == sort);
        action->setData(static_cast<int>(sort));
        group->addAction(action);
    };
    addSort(tr("Sort: this game first"), Sort::Default);
    addSort(tr("Sort: lowest ping"), Sort::Ping);
    addSort(tr("Sort: most players"), Sort::Players);
    addSort(tr("Sort: name"), Sort::Name);

    QAction* chosen = menu.exec(m_filterButton->mapToGlobal(QPoint(0, m_filterButton->height())));
    if (chosen == nullptr)
        return;
    if (chosen == thisGame)
    {
        m_thisGameOnly = thisGame->isChecked();
    }
    else
    {
        m_sort = static_cast<Sort>(chosen->data().toInt());
        QSettings settings("RMG-K", "n02");
        settings.setValue("RollbackLobby/RoomsSort", static_cast<int>(m_sort));
    }
    m_filterButton->setIconColorOverride(m_thisGameOnly ? LobbyTheme::tokens().accentInk : QColor());
    rebuild();
}

void RoomsColumn::setRooms(const QList<RoomRowView>& rooms)
{
    m_rooms = rooms;
    rebuild();
}

void RoomsColumn::setThisGameAvailable(bool available)
{
    m_thisGameAvailable = available;
    if (!available && m_thisGameOnly)
    {
        m_thisGameOnly = false;
        m_filterButton->setIconColorOverride(QColor());
    }
}

void RoomsColumn::setOffline(bool offline)
{
    m_offline = offline;
    rebuild();
}

void RoomsColumn::selectRow(quint64 roomId)
{
    m_selectedRoomId = roomId;
    for (auto* row : m_rows)
        row->setSelected(row->view().roomId == roomId);
}

void RoomsColumn::rebuild()
{
    const QString search = m_searchEdit->text().trimmed();
    QList<RoomRowView> visible;
    for (const auto& room : m_rooms)
    {
        if (m_thisGameOnly && !room.sameGame)
            continue;
        if (!search.isEmpty() && !room.name.contains(search, Qt::CaseInsensitive) &&
            !room.host.contains(search, Qt::CaseInsensitive) &&
            !room.game.contains(search, Qt::CaseInsensitive))
            continue;
        visible.append(room);
    }

    const Sort sort = m_sort;
    std::stable_sort(visible.begin(), visible.end(), [sort](const RoomRowView& a, const RoomRowView& b) {
        if (a.mine != b.mine)
            return a.mine;
        switch (sort)
        {
            case Sort::Ping:
            {
                const int pingA = a.pingMs < 0 ? 1'000'000 : a.pingMs;
                const int pingB = b.pingMs < 0 ? 1'000'000 : b.pingMs;
                if (pingA != pingB)
                    return pingA < pingB;
                break;
            }
            case Sort::Players:
                if (a.players != b.players)
                    return a.players > b.players;
                break;
            case Sort::Name:
            {
                const int order = a.name.compare(b.name, Qt::CaseInsensitive);
                if (order != 0)
                    return order < 0;
                break;
            }
            case Sort::Default:
                // Rooms you can actually sit in first, then the selected game.
                if (a.hasRom != b.hasRom)
                    return a.hasRom;
                if (a.sameGame != b.sameGame)
                    return a.sameGame;
                break;
        }
        return a.roomId < b.roomId;
    });

    // Reuse rows so hover and the scroll position survive a refresh.
    QHash<quint64, RoomRow*> kept;
    for (const auto& room : visible)
    {
        RoomRow* row = m_rows.take(room.roomId);
        if (row == nullptr)
        {
            row = new RoomRow(m_list);
            connect(row, &RoomRow::joinRequested, this, &RoomsColumn::joinRequested);
            connect(row, &RoomRow::returnRequested, this, &RoomsColumn::returnRequested);
            connect(row, &RoomRow::contextMenuRequested, this, &RoomsColumn::contextMenuRequested);
            connect(row, &RoomRow::playerMenuRequested, this, &RoomsColumn::playerMenuRequested);
            connect(row, &RoomRow::clicked, this, [this](quint64 roomId) {
                selectRow(roomId);
                const auto it = m_rows.constFind(roomId);
                if (it != m_rows.constEnd())
                    emit hostProbeRequested(it.value()->view().hostId);
            });
        }
        row->apply(room);
        row->setSelected(room.roomId == m_selectedRoomId);
        kept.insert(room.roomId, row);
    }
    // Later, not now: this can run inside a click on the very row being dropped.
    for (auto* row : m_rows)
    {
        m_listLayout->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    m_rows = kept;

    // Lay the rows out in order, ahead of the note and the stretch.
    for (auto* row : m_rows)
        m_listLayout->removeWidget(row);
    int index = 0;
    for (const auto& room : visible)
    {
        RoomRow* row = m_rows.value(room.roomId);
        m_listLayout->insertWidget(index++, row);
        row->setVisible(true);
    }

    m_countLabel->setText(QString::number(m_rooms.size()));
    if (m_offline)
    {
        m_noteTitle->setText(tr("Not connected").toUpper());
        m_noteBody->setText(QString());
    }
    else if (visible.isEmpty() && !m_rooms.isEmpty())
    {
        m_noteTitle->setText(tr("No rooms match").toUpper());
        m_noteBody->setText(tr("Clear the search or filter to see every room."));
    }
    else
    {
        m_noteTitle->setText(tr("That's all for now").toUpper());
        m_noteBody->setText(tr("Create a room, or Quick Match finds you a player."));
    }
    m_noteBody->setVisible(!m_noteBody->text().isEmpty());
    // Only a list that doesn't already fill the column has room to explain itself.
    m_note->setVisible(m_offline || visible.size() <= 4);
}

} // namespace Dialog
} // namespace UserInterface

#include "LobbyBrowsePanels.moc"

#endif // NETPLAY
