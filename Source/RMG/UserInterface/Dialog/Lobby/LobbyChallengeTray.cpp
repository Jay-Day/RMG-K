#ifdef NETPLAY

#include "LobbyChallengeTray.hpp"

#include "LobbySocialWidgets.hpp"
#include "LobbyWidgets.hpp"

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

namespace
{
    constexpr int kTrayWidth = 372;
    constexpr int kCardGap = 8;
    constexpr int kBandHeight = 24;
    constexpr int kCountdownHeight = 3;
    constexpr int kRowHeight = 46;
    constexpr int kPanelHeaderHeight = 40;
    constexpr int kPanelFooterHeight = 36;
    constexpr int kPanelEmptyHeight = 56;
    constexpr int kReasonMaxWidth = 128;
    // The least of the log that stays on screen when live cards fill the window.
    constexpr int kLogMinHeight = kPanelHeaderHeight + 3 * kRowHeight + kPanelFooterHeight;
    // How long a sent challenge's outcome stays, if nobody dismisses it.
    constexpr int kOutcomeMs = 8'000;
    constexpr int kMaxEntries = 50;
    constexpr qreal kCardRadius = 10.0;

    QPainterPath cardShape(const QWidget* widget)
    {
        QPainterPath shape;
        shape.addRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius, kCardRadius);
        return shape;
    }

    // Every card in the tray sits on the same panel.
    void fillCard(QPainter& painter, const QPainterPath& shape)
    {
        const Tokens& t = LobbyTheme::tokens();
        const QRectF box = shape.boundingRect();
        QLinearGradient fill(box.topLeft(), box.bottomLeft());
        fill.setColorAt(0, t.panel2);
        fill.setColorAt(1, t.panel);
        painter.fillPath(shape, fill);
    }

    void strokeCard(QPainter& painter, const QPainterPath& shape, const QColor& color)
    {
        painter.setPen(QPen(color, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(shape);
    }

    // The time a challenge has left, draining along the card's bottom edge.
    void paintCountdown(QPainter& painter, const QWidget* widget, qint64 expiresAtMs, qint64 durationMs)
    {
        const Tokens& t = LobbyTheme::tokens();
        const qint64 left = std::max<qint64>(0, expiresAtMs - QDateTime::currentMSecsSinceEpoch());
        const qreal fraction = durationMs > 0 ? std::min<qreal>(1.0, qreal(left) / durationMs) : 0.0;
        const QRectF strip(0, widget->height() - kCountdownHeight, widget->width(), kCountdownHeight);
        painter.fillRect(strip, t.tint);
        painter.fillRect(QRectF(strip.left(), strip.top(), strip.width() * fraction, strip.height()), t.accentHi);
    }

    // A tinted square with an icon, painted with the theme of the moment.
    void paintBadge(QPainter& painter, const QRectF& box, qreal radius, const QString& icon, int iconSide,
                    const QColor& ink, const QColor& fill, qreal dpr)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(box, radius, radius);
        painter.drawPixmap(QPointF(box.center().x() - iconSide / 2.0, box.center().y() - iconSide / 2.0),
                           LobbyTheme::icon(icon, ink, iconSide, dpr));
    }

    QColor tintFor(Role role)
    {
        const Tokens& t = LobbyTheme::tokens();
        switch (role)
        {
            case Role::Warn: return t.warnTint;
            case Role::Ok:   return t.okTint;
            default:         return t.raise;
        }
    }

    QLabel* mutedLabel(int px, QWidget* parent)
    {
        auto* label = new QLabel(parent);
        label->setFont(LobbyTheme::uiFont(px));
        label->setProperty("role", "muted");
        return label;
    }

    ElidedLabel* elided(Role role, const QFont& font, QWidget* parent)
    {
        auto* label = new ElidedLabel(role, parent);
        label->setFont(font);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        return label;
    }

    // The challenge this PC sent, while it waits for an answer.
    class OutgoingCard : public QFrame
    {
    public:
        explicit OutgoingCard(QWidget* parent)
            : QFrame(parent)
        {
            setObjectName("OutgoingChallengeCard");
            auto* root = new QVBoxLayout(this);
            root->setContentsMargins(0, 0, 0, kCountdownHeight);
            root->setSpacing(0);

            auto* head = new QWidget(this);
            head->setFixedHeight(kBandHeight + 4);
            auto* headLay = new QHBoxLayout(head);
            headLay->setContentsMargins(12, 6, 12, 0);
            headLay->setSpacing(7);
            m_spinner = new Spinner(13, head);
            headLay->addWidget(m_spinner);
            m_title = new ElidedLabel(Role::Accent, head);
            m_title->setFont(LobbyTheme::pixelFont(11, 0.8));
            headLay->addWidget(m_title, 1);
            m_seconds = new ElidedLabel(Role::Muted, head);
            m_seconds->setFont(LobbyTheme::pixelFont(11));
            m_seconds->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            headLay->addWidget(m_seconds);
            root->addWidget(head);

            auto* body = new QVBoxLayout;
            body->setContentsMargins(12, 6, 12, 10);
            body->setSpacing(8);

            auto* who = new QHBoxLayout;
            who->setSpacing(10);
            m_tile = new PlayerTile(34, this);
            who->addWidget(m_tile);
            auto* words = new QVBoxLayout;
            words->setSpacing(1);
            words->setAlignment(Qt::AlignVCenter);
            auto* nameRow = new QHBoxLayout;
            nameRow->setSpacing(6);
            m_waiting = new ElidedLabel(Role::Ink, this);
            m_waiting->setFont(LobbyTheme::uiFont(13, QFont::Bold));
            nameRow->addWidget(m_waiting);
            m_flag = new CountryFlag(this);
            nameRow->addWidget(m_flag);
            nameRow->addStretch(1);
            words->addLayout(nameRow);
            auto* sub = new QHBoxLayout;
            sub->setSpacing(5);
            m_bars = new SignalBars(this);
            sub->addWidget(m_bars);
            m_ping = mutedLabel(11, this);
            sub->addWidget(m_ping);
            sub->addStretch(1);
            words->addLayout(sub);
            who->addLayout(words, 1);
            body->addLayout(who);

            auto* game = new QHBoxLayout;
            game->setSpacing(10);
            m_cart = new CartridgeTile(CartridgeTile::Size::Small, this);
            game->addWidget(m_cart);
            m_game = elided(Role::Ink, LobbyTheme::uiFont(12, QFont::DemiBold), this);
            game->addWidget(m_game, 1);
            m_cancel = new LobbyButton(QString(), LobbyButton::Kind::Secondary, this);
            m_cancel->setObjectName("ChallengeCancel");
            m_cancel->setButtonSize(LobbyButton::Size::Small);
            m_cancel->setIconName(QStringLiteral("close-line"));
            game->addWidget(m_cancel);
            body->addLayout(game);
            root->addLayout(body);

            m_timer = new QTimer(this);
            m_timer->setInterval(100);
            connect(m_timer, &QTimer::timeout, this, [this]() { tick(); });
        }

        LobbyButton* cancelButton() const { return m_cancel; }

        void setView(const OutgoingChallengeView& view, const QString& cancelText)
        {
            if (view.challengeId != m_view.challengeId)
                m_cancel->setDown(false);
            m_view = view;
            const Tokens& t = LobbyTheme::tokens();
            setAccessibleName(view.waitingFor);
            m_spinner->setColors(t.accentTint, t.accentInk);
            m_title->setText(view.title.toUpper());
            m_tile->setPlayer(view.toName);
            m_waiting->setText(view.waitingFor);
            m_waiting->setToolTip(view.waitingFor);
            m_flag->setCountry(view.toCountry);
            m_bars->setPing(view.pingMs, view.pingEstimate);
            m_bars->setVisible(view.pingMs >= 0);
            m_ping->setText(view.pingMs >= 0 ? QString(view.pingEstimate ? "~%1 ms" : "%1 ms").arg(view.pingMs) : QString());
            m_ping->setVisible(view.pingMs >= 0);
            m_cart->setGame(view.game, view.coverPath);
            m_game->setText(view.game);
            m_game->setToolTip(view.game);
            m_cancel->setText(cancelText);
            m_timer->start();
            tick();
        }

        void stop() { m_timer->stop(); }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const Tokens& t = LobbyTheme::tokens();
            const QPainterPath shape = cardShape(this);
            fillCard(painter, shape);
            painter.save();
            painter.setClipPath(shape);
            paintCountdown(painter, this, m_view.expiresAtMs, m_view.durationMs);
            painter.restore();
            strokeCard(painter, shape, LobbyTheme::mix(t.accent, t.line2, 0.35));
        }

    private:
        void tick()
        {
            m_seconds->setText(ChallengeCard::secondsLeft(m_view.expiresAtMs - QDateTime::currentMSecsSinceEpoch()));
            update(0, height() - kCountdownHeight - 1, width(), kCountdownHeight + 1);
        }

        OutgoingChallengeView m_view;
        QTimer* m_timer;
        Spinner* m_spinner;
        ElidedLabel* m_title;
        ElidedLabel* m_seconds;
        PlayerTile* m_tile;
        ElidedLabel* m_waiting;
        CountryFlag* m_flag;
        SignalBars* m_bars;
        QLabel* m_ping;
        CartridgeTile* m_cart;
        ElidedLabel* m_game;
        LobbyButton* m_cancel;
    };

    // How the challenge this PC sent ended, in the words the play bar used.
    class OutcomeCard : public QFrame
    {
    public:
        explicit OutcomeCard(QWidget* parent)
            : QFrame(parent)
        {
            setObjectName("ChallengeOutcomeCard");
            auto* lay = new QHBoxLayout(this);
            lay->setContentsMargins(12, 10, 8, 10);
            lay->setSpacing(10);
            m_badgeSlot = new QWidget(this);
            m_badgeSlot->setFixedSize(34, 34);
            m_badgeSlot->setAttribute(Qt::WA_TransparentForMouseEvents);
            lay->addWidget(m_badgeSlot, 0, Qt::AlignTop);
            auto* words = new QVBoxLayout;
            words->setSpacing(3);
            words->setContentsMargins(0, 2, 0, 0);
            m_title = elided(Role::Muted, LobbyTheme::pixelFont(12, 0.4), this);
            words->addWidget(m_title);
            m_detail = mutedLabel(11, this);
            m_detail->setWordWrap(true);
            words->addWidget(m_detail);
            lay->addLayout(words, 1);
            m_close = new IconButton(QStringLiteral("close-line"), 22, this);
            m_close->setObjectName("ChallengeOutcomeDismiss");
            lay->addWidget(m_close, 0, Qt::AlignTop);
        }

        IconButton* closeButton() const { return m_close; }

        void setOutcome(const QString& title, const QString& detail, Role role, const QString& icon,
                        const QString& dismissText)
        {
            m_role = role;
            m_icon = icon;
            m_title->setRole(role);
            m_title->setText(title.toUpper());
            m_title->setToolTip(title);
            m_detail->setText(detail);
            m_close->setToolTip(dismissText);
            m_close->setAccessibleName(dismissText);
            setAccessibleName(title);
            setAccessibleDescription(detail);
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const Tokens& t = LobbyTheme::tokens();
            const QColor color = LobbyTheme::roleColor(m_role);
            const QPainterPath shape = cardShape(this);
            fillCard(painter, shape);
            paintBadge(painter, QRectF(m_badgeSlot->geometry()), 8, m_icon, 17, color,
                       LobbyTheme::mix(color, t.tint, 0.13), devicePixelRatioF());
            strokeCard(painter, shape, LobbyTheme::mix(color, t.line, 0.45));
        }

    private:
        Role m_role = Role::Muted;
        QString m_icon;
        QWidget* m_badgeSlot;
        ElidedLabel* m_title;
        QLabel* m_detail;
        IconButton* m_close;
    };

    // After a match: who challenged this PC while it played.
    class SummaryCard : public QFrame
    {
    public:
        explicit SummaryCard(QWidget* parent)
            : QFrame(parent)
        {
            setObjectName("MissedChallengesCard");
            auto* lay = new QHBoxLayout(this);
            lay->setContentsMargins(12, 10, 8, 10);
            lay->setSpacing(10);
            m_badgeSlot = new QWidget(this);
            m_badgeSlot->setFixedSize(34, 34);
            m_badgeSlot->setAttribute(Qt::WA_TransparentForMouseEvents);
            lay->addWidget(m_badgeSlot, 0, Qt::AlignVCenter);
            auto* words = new QVBoxLayout;
            words->setSpacing(2);
            m_title = new QLabel(this);
            m_title->setFont(LobbyTheme::uiFont(12, QFont::Bold));
            m_title->setWordWrap(true);
            words->addWidget(m_title);
            m_names = elided(Role::Muted, LobbyTheme::uiFont(11), this);
            words->addWidget(m_names);
            lay->addLayout(words, 1);
            m_view = new LobbyButton(QString(), LobbyButton::Kind::Secondary, this);
            m_view->setObjectName("MissedChallengesView");
            m_view->setButtonSize(LobbyButton::Size::Small);
            lay->addWidget(m_view, 0, Qt::AlignVCenter);
            m_close = new IconButton(QStringLiteral("close-line"), 22, this);
            m_close->setObjectName("MissedChallengesDismiss");
            lay->addWidget(m_close, 0, Qt::AlignTop);
        }

        LobbyButton* viewButton() const { return m_view; }
        IconButton* closeButton() const { return m_close; }

        void setSummary(const QString& title, const QString& names, const QString& viewText, const QString& dismissText)
        {
            m_title->setText(title);
            m_names->setText(names);
            m_names->setToolTip(names);
            m_view->setText(viewText);
            m_close->setToolTip(dismissText);
            m_close->setAccessibleName(dismissText);
            setAccessibleName(title);
            setAccessibleDescription(names);
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const Tokens& t = LobbyTheme::tokens();
            const QPainterPath shape = cardShape(this);
            fillCard(painter, shape);
            paintBadge(painter, QRectF(m_badgeSlot->geometry()), 8, QStringLiteral("sword-fill"), 17, t.warn, t.warnTint,
                       devicePixelRatioF());
            strokeCard(painter, shape, LobbyTheme::mix(t.warn, t.line2, 0.45));
        }

    private:
        QWidget* m_badgeSlot;
        QLabel* m_title;
        ElidedLabel* m_names;
        LobbyButton* m_view;
        IconButton* m_close;
    };

    // The log's panel.
    class LogPanel : public QFrame
    {
    public:
        using QFrame::QFrame;

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const Tokens& t = LobbyTheme::tokens();
            const QPainterPath shape = cardShape(this);
            painter.fillPath(shape, t.panel2);
            painter.fillRect(QRectF(1, kPanelHeaderHeight - 1, width() - 2, 1), t.line);
            strokeCard(painter, shape, t.line2);
        }
    };

    // One entry of the log. The words are painted; the buttons are real ones,
    // so the keyboard and screen readers reach them.
    class LogRow : public QWidget
    {
    public:
        explicit LogRow(QWidget* parent)
            : QWidget(parent)
        {
            setObjectName("ChallengeLogRow");
            setFixedHeight(kRowHeight);
            auto* lay = new QHBoxLayout(this);
            lay->setContentsMargins(10, 0, 10, 0);
            lay->setSpacing(6);
            lay->addStretch(1);
            m_message = new IconButton(QStringLiteral("mail-line"), 24, this);
            m_message->setObjectName("ChallengeLogMessage");
            lay->addWidget(m_message);
            m_action = new LobbyButton(QString(), LobbyButton::Kind::Secondary, this);
            m_action->setObjectName("ChallengeLogAction");
            m_action->setButtonSize(LobbyButton::Size::Compact);
            lay->addWidget(m_action);
            // Some reasons run long in some languages; the tooltip has all of it.
            m_reason = new ElidedLabel(Role::Muted, this);
            m_reason->setObjectName("ChallengeLogReason");
            m_reason->setFont(LobbyTheme::uiFont(10));
            m_reason->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_reason->setMaximumWidth(kReasonMaxWidth);
            lay->addWidget(m_reason);
        }

        quint64 key() const { return m_view.key; }
        LobbyButton* actionButton() const { return m_action; }
        IconButton* messageButton() const { return m_message; }

        void setView(const ChallengeLogRowView& view, bool last, const QString& messageText)
        {
            m_view = view;
            m_last = last;
            m_action->setText(view.action);
            m_action->setVisible(view.unavailable.isEmpty());
            m_reason->setText(view.unavailable);
            m_reason->setToolTip(view.unavailable);
            m_reason->setAccessibleName(view.unavailable);
            m_reason->setVisible(!view.unavailable.isEmpty());
            m_message->setVisible(view.canMessage);
            m_message->setToolTip(messageText);
            m_message->setAccessibleName(messageText);
            setToolTip(view.tip);
            QStringList words { view.name, view.what };
            if (!view.game.isEmpty())
                words << view.game;
            setAccessibleName(words.join(QStringLiteral(", ")));
            refreshTime();
            update();
        }

        void refreshTime()
        {
            setAccessibleDescription(ChallengeTray::relativeTime(m_view.timeMs));
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const Tokens& t = LobbyTheme::tokens();
            const QColor whatColor = m_view.whatRole == Role::Muted ? t.ink2 : LobbyTheme::roleColor(m_view.whatRole);
            if (m_view.fresh)
            {
                painter.fillRect(rect(), LobbyTheme::mix(t.accent, t.panel2, 0.07));
                painter.fillRect(QRect(0, 0, 2, height()), t.accent);
            }

            const QRectF badge(10, (height() - 26) / 2.0, 26, 26);
            paintBadge(painter, badge, 7,
                       m_view.incoming ? QStringLiteral("arrow-left-down-line") : QStringLiteral("arrow-right-up-line"), 14,
                       m_view.whatRole == Role::Muted ? t.ink2 : whatColor, tintFor(m_view.whatRole), devicePixelRatioF());

            // The words end where the first button begins.
            int right = width() - 10;
            for (const QWidget* child : { static_cast<const QWidget*>(m_message), static_cast<const QWidget*>(m_action),
                                          static_cast<const QWidget*>(m_reason) })
            {
                if (child->isVisible())
                    right = std::min(right, child->x());
            }
            right -= 8;
            const int left = 46;

            const QFont timeFont = LobbyTheme::uiFont(10);
            const QString when = ChallengeTray::relativeTime(m_view.timeMs);
            const int timeWidth = QFontMetrics(timeFont).horizontalAdvance(when);
            painter.setFont(timeFont);
            painter.setPen(t.faint);
            const QRect line1(left, 6, std::max(0, right - left), 18);
            painter.drawText(line1, Qt::AlignRight | Qt::AlignVCenter, when);

            const QFont nameFont = LobbyTheme::uiFont(12, QFont::Bold);
            const QFont countFont = LobbyTheme::uiFont(11, QFont::Bold);
            const QString count = m_view.count > 1 ? QStringLiteral("×%1").arg(m_view.count) : QString();
            const QPixmap flag = LobbyTheme::flag(m_view.country, devicePixelRatioF());
            const int flagWidth = flag.isNull() ? 0 : int(flag.deviceIndependentSize().width()) + 6;
            const int countWidth = count.isEmpty() ? 0 : QFontMetrics(countFont).horizontalAdvance(count) + 6;
            const int nameRoom = std::max(0, line1.width() - timeWidth - 8 - flagWidth - countWidth);
            const QFontMetrics nameMetrics(nameFont);
            const QString name = nameMetrics.elidedText(m_view.name, Qt::ElideRight, nameRoom);
            painter.setFont(nameFont);
            painter.setPen(t.ink);
            painter.drawText(QRect(left, line1.top(), nameRoom + 1, line1.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
            int x = left + nameMetrics.horizontalAdvance(name);
            if (!flag.isNull())
            {
                painter.drawPixmap(QPointF(x + 6, line1.center().y() - flag.deviceIndependentSize().height() / 2.0 + 0.5), flag);
                x += flagWidth;
            }
            if (!count.isEmpty())
            {
                painter.setFont(countFont);
                painter.setPen(t.muted);
                painter.drawText(QRect(x + 6, line1.top(), countWidth, line1.height()), Qt::AlignLeft | Qt::AlignVCenter, count);
            }

            const QRect line2(left, 24, std::max(0, right - left), 16);
            const QFont whatFont = LobbyTheme::uiFont(11, QFont::DemiBold);
            const QFontMetrics whatMetrics(whatFont);
            const QString what = whatMetrics.elidedText(m_view.what, Qt::ElideRight, line2.width());
            painter.setFont(whatFont);
            painter.setPen(whatColor);
            painter.drawText(line2, Qt::AlignLeft | Qt::AlignVCenter, what);
            const int gameLeft = line2.left() + whatMetrics.horizontalAdvance(what);
            if (!m_view.game.isEmpty() && gameLeft < line2.right() - 20)
            {
                const QFont gameFont = LobbyTheme::uiFont(11);
                const QRect gameRect(gameLeft, line2.top(), line2.right() - gameLeft, line2.height());
                const QString game = QFontMetrics(gameFont).elidedText(QStringLiteral(" · ") + m_view.game, Qt::ElideRight,
                                                                       gameRect.width());
                painter.setFont(gameFont);
                painter.setPen(t.muted);
                painter.drawText(gameRect, Qt::AlignLeft | Qt::AlignVCenter, game);
            }

            if (!m_last)
                painter.fillRect(QRect(10, height() - 1, width() - 20, 1), t.line);
        }

    private:
        ChallengeLogRowView m_view;
        bool m_last = false;
        IconButton* m_message;
        LobbyButton* m_action;
        ElidedLabel* m_reason;
    };

    bool sameGame(const ChallengeLogEntry& a, const ChallengeLogEntry& b)
    {
        if (!a.md5.isEmpty() || !b.md5.isEmpty())
            return a.md5.compare(b.md5, Qt::CaseInsensitive) == 0;
        return a.game == b.game;
    }
} // namespace

// ── ChallengeLog ─────────────────────────────────────────────────────

ChallengeLog& ChallengeLog::session()
{
    static ChallengeLog log;
    return log;
}

bool ChallengeLog::countsUnseen(ChallengeLogOutcome outcome)
{
    return outcome == ChallengeLogOutcome::Missed || outcome == ChallengeLogOutcome::WhilePlaying ||
           outcome == ChallengeLogOutcome::WhileNotificationsOff;
}

void ChallengeLog::add(ChallengeLogEntry entry, bool seen)
{
    entry.count = 1;
    entry.unseen = !seen && countsUnseen(entry.outcome) ? 1 : 0;
    const auto newest = std::find_if(m_entries.begin(), m_entries.end(), [&entry](const ChallengeLogEntry& other) {
        return other.name.compare(entry.name, Qt::CaseInsensitive) == 0;
    });
    if (newest != m_entries.end() && newest->incoming == entry.incoming && newest->outcome == entry.outcome &&
        sameGame(*newest, entry))
    {
        ChallengeLogEntry again = *newest;
        again.count += 1;
        again.unseen += entry.unseen;
        again.timeMs = entry.timeMs;
        again.forName = entry.forName;
        again.roomName = entry.roomName;
        m_entries.erase(newest);
        m_entries.prepend(again);
        return;
    }
    entry.key = m_nextKey++;
    m_entries.prepend(entry);
    while (m_entries.size() > kMaxEntries)
        m_entries.removeLast();
}

void ChallengeLog::markSeen()
{
    for (ChallengeLogEntry& entry : m_entries)
        entry.unseen = 0;
}

void ChallengeLog::clear()
{
    m_entries.clear();
}

// ── ChallengesButton ─────────────────────────────────────────────────

ChallengesButton::ChallengesButton(QWidget* parent)
    : QPushButton(parent)
{
    setObjectName("ChallengesButton");
    setAccessibleName(tr("Challenges"));
    setAutoDefault(false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setFixedSize(36, 36);
    refreshTip();
}

void ChallengesButton::setUnseen(int count)
{
    if (count == m_unseen)
        return;
    m_unseen = count;
    refreshTip();
    update();
}

void ChallengesButton::setOpen(bool open)
{
    if (open == m_open)
        return;
    m_open = open;
    update();
}

void ChallengesButton::refreshTip()
{
    const QString tip = m_unseen > 0 ? tr("Challenges: %1 missed").arg(m_unseen) : tr("Challenges");
    setToolTip(tip);
    setAccessibleDescription(tip);
}

void ChallengesButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const bool hover = underMouse();

    // The account chip's box beside it, ringed in the accent while the log shows.
    painter.setPen(QPen(m_open ? t.accent : t.line2, 1.0));
    painter.setBrush(hover || m_open || isDown() ? t.raise : t.tint);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
    constexpr int side = 18;
    painter.drawPixmap(QPointF((width() - side) / 2.0, (height() - side) / 2.0),
                       LobbyTheme::icon(QStringLiteral("sword-fill"), m_open ? t.accentInk : hover ? t.ink : t.ink2, side,
                                        devicePixelRatioF()));

    if (m_unseen <= 0)
        return;
    const QString text = m_unseen > 99 ? QStringLiteral("99+") : QString::number(m_unseen);
    const QFont font = LobbyTheme::uiFont(9, QFont::Bold);
    const qreal w = std::max<qreal>(15, QFontMetrics(font).horizontalAdvance(text) + 8);
    const QRectF badge(width() - w - 1, 1, w, 15);
    painter.setPen(Qt::NoPen);
    painter.setBrush(t.liveBg);
    painter.drawRoundedRect(badge, 7.5, 7.5);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(badge, Qt::AlignCenter, text);
}

// ── ChallengeCard ────────────────────────────────────────────────────

ChallengeCard::ChallengeCard(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("ChallengeCard");
    setAttribute(Qt::WA_StyledBackground, false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, kCountdownHeight);
    root->setSpacing(0);

    auto* band = new QWidget(this);
    band->setFixedHeight(kBandHeight);
    auto* bandLay = new QHBoxLayout(band);
    bandLay->setContentsMargins(11, 0, 11, 0);
    bandLay->setSpacing(7);
    m_bandIcon = new IconLabel(QStringLiteral("sword-fill"), Role::Ink, 13, band);
    m_bandIcon->setColorOverride(Qt::white);
    bandLay->addWidget(m_bandIcon);
    m_bandTitle = new QLabel(band);
    m_bandTitle->setFont(LobbyTheme::pixelFont(11, 0.8));
    m_bandTitle->setStyleSheet(QStringLiteral("color: #ffffff;"));
    centerDisplayCaps(m_bandTitle);
    bandLay->addWidget(m_bandTitle);
    bandLay->addStretch(1);
    m_seconds = new QLabel(band);
    m_seconds->setFont(LobbyTheme::pixelFont(11));
    m_seconds->setStyleSheet(QStringLiteral("color: #ffffff;"));
    centerDisplayCaps(m_seconds);
    bandLay->addWidget(m_seconds);
    root->addWidget(band);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(8);

    // Who it's from.
    auto* who = new QHBoxLayout;
    who->setSpacing(10);
    m_fromTile = new PlayerTile(34, this);
    who->addWidget(m_fromTile);
    auto* whoWords = new QVBoxLayout;
    whoWords->setSpacing(1);
    whoWords->setAlignment(Qt::AlignVCenter);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(6);
    m_fromName = new ElidedLabel(Role::Ink, this);
    m_fromName->setFont(LobbyTheme::uiFont(13, QFont::Bold));
    nameRow->addWidget(m_fromName);
    m_fromFlag = new CountryFlag(this);
    nameRow->addWidget(m_fromFlag);
    nameRow->addStretch(1);
    whoWords->addLayout(nameRow);
    auto* sub = new QHBoxLayout;
    sub->setSpacing(5);
    m_fromBars = new SignalBars(this);
    sub->addWidget(m_fromBars);
    m_fromPing = mutedLabel(11, this);
    sub->addWidget(m_fromPing);
    m_fromConnection = new ConnectionGlyph(this);
    sub->addWidget(m_fromConnection);
    m_fromConnectionName = mutedLabel(11, this);
    sub->addWidget(m_fromConnectionName);
    sub->addStretch(1);
    whoWords->addLayout(sub);
    who->addLayout(whoWords, 1);
    body->addLayout(who);

    // What: the game, and the sender's message or the room.
    auto* game = new QHBoxLayout;
    game->setSpacing(10);
    m_tile = new CartridgeTile(CartridgeTile::Size::Small, this);
    game->addWidget(m_tile);
    auto* gameWords = new QVBoxLayout;
    gameWords->setSpacing(1);
    gameWords->setAlignment(Qt::AlignVCenter);
    m_game = elided(Role::Ink, LobbyTheme::uiFont(12, QFont::DemiBold), this);
    gameWords->addWidget(m_game);
    m_gameDetail = elided(Role::Muted, LobbyTheme::uiFont(11), this);
    gameWords->addWidget(m_gameDetail);
    game->addLayout(gameWords, 1);
    m_seatsBox = new QWidget(this);
    auto* seats = new QVBoxLayout(m_seatsBox);
    seats->setContentsMargins(0, 0, 0, 0);
    seats->setSpacing(4);
    seats->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    m_seatsLabel = mutedLabel(10, m_seatsBox);
    m_seatsLabel->setAlignment(Qt::AlignRight);
    seats->addWidget(m_seatsLabel);
    m_seatsPips = new SeatPips(m_seatsBox);
    seats->addWidget(m_seatsPips, 0, Qt::AlignRight);
    game->addWidget(m_seatsBox);
    body->addLayout(game);

    // Who on this PC it's for, when there's more than one player here.
    m_forRow = new QWidget(this);
    auto* forLay = new QHBoxLayout(m_forRow);
    forLay->setContentsMargins(0, 0, 0, 0);
    forLay->setSpacing(6);
    forLay->addWidget(new IconLabel(QStringLiteral("gamepad-line"), Role::Muted, 13, m_forRow));
    m_forText = elided(Role::Muted, LobbyTheme::uiFont(11), m_forRow);
    forLay->addWidget(m_forText, 1);
    body->addWidget(m_forRow);

    // What accepting costs.
    m_warnRow = new QWidget(this);
    auto* warnLay = new QHBoxLayout(m_warnRow);
    warnLay->setContentsMargins(0, 0, 0, 0);
    warnLay->setSpacing(6);
    warnLay->addWidget(new IconLabel(QStringLiteral("error-warning-line"), Role::Warn, 13, m_warnRow));
    m_warn = elided(Role::Warn, LobbyTheme::uiFont(11), m_warnRow);
    warnLay->addWidget(m_warn, 1);
    body->addWidget(m_warnRow);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch(1);
    m_decline = new LobbyButton(tr("Decline"), LobbyButton::Kind::Secondary, this);
    m_decline->setObjectName("ChallengeDecline");
    m_decline->setButtonSize(LobbyButton::Size::Small);
    buttons->addWidget(m_decline);
    m_accept = new LobbyButton(tr("Accept"), LobbyButton::Kind::Primary, this);
    m_accept->setObjectName("ChallengeAccept");
    m_accept->setButtonSize(LobbyButton::Size::Small);
    m_accept->setIconName(QStringLiteral("check-line"));
    m_accept->setPixelLabel(true);
    buttons->addWidget(m_accept);
    body->addLayout(buttons);
    root->addLayout(body);

    connect(m_accept, &QPushButton::clicked, this, &ChallengeCard::acceptClicked);
    connect(m_decline, &QPushButton::clicked, this, &ChallengeCard::declineClicked);

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &ChallengeCard::tick);
    m_timer->start();
}

QString ChallengeCard::secondsLeft(qint64 msLeft)
{
    return tr("%1 sec").arg((std::max<qint64>(0, msLeft) + 999) / 1000).toUpper();
}

void ChallengeCard::setView(const ChallengeCardView& view)
{
    // A press that began on another challenge mustn't answer this one.
    if (view.challengeId != m_view.challengeId)
    {
        m_accept->setDown(false);
        m_decline->setDown(false);
    }
    m_view = view;
    setAccessibleName((view.invite ? tr("Room invite from %1") : tr("Challenge from %1")).arg(view.fromName));
    m_bandIcon->setIconName(view.invite ? QStringLiteral("mail-send-line") : QStringLiteral("sword-fill"));
    m_bandTitle->setText((view.invite ? tr("Room invite") : tr("New challenger")).toUpper());

    m_fromTile->setPlayer(view.fromName);
    m_fromName->setText(view.fromName);
    m_fromName->setToolTip(view.fromName);
    m_fromFlag->setCountry(view.fromCountry);
    m_fromBars->setPing(view.fromPingMs, view.fromPingEstimate);
    m_fromBars->setVisible(view.fromPingMs >= 0);
    m_fromPing->setText(view.fromPingMs >= 0
        ? QString(view.fromPingEstimate ? "~%1 ms" : "%1 ms").arg(view.fromPingMs) : QString());
    m_fromPing->setVisible(view.fromPingMs >= 0);
    m_fromConnection->setConnection(view.fromConnection);
    m_fromConnectionName->setText(ConnectionGlyph::displayName(view.fromConnection));

    m_tile->setGame(view.game, view.coverPath);
    // An invite leads with its room.
    m_game->setText(view.invite ? view.gameDetail : view.game);
    m_game->setToolTip(m_game->text());
    m_gameDetail->setText(view.invite ? view.game : view.gameDetail);
    m_gameDetail->setToolTip(m_gameDetail->text());
    m_gameDetail->setVisible(!m_gameDetail->text().isEmpty());
    m_seatsBox->setVisible(view.invite && view.roomMaxPlayers > 0);
    m_seatsLabel->setText(tr("%1 of %2 seats").arg(view.roomPlayers).arg(view.roomMaxPlayers));
    m_seatsPips->setSeats(view.roomPlayers, view.roomMaxPlayers);

    m_forText->setText(view.forDetail);
    m_forText->setToolTip(view.forDetail);
    m_forRow->setVisible(!view.forDetail.isEmpty());
    m_warn->setText(view.warning);
    m_warn->setToolTip(view.warning);
    m_warnRow->setVisible(!view.warning.isEmpty());
    m_accept->setEnabled(view.canAccept);
    tick();
}

void ChallengeCard::tick()
{
    m_seconds->setText(secondsLeft(m_view.expiresAtMs - QDateTime::currentMSecsSinceEpoch()));
    update(0, height() - kCountdownHeight - 1, width(), kCountdownHeight + 1);
}

void ChallengeCard::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    const QPainterPath shape = cardShape(this);
    fillCard(painter, shape);

    painter.save();
    painter.setClipPath(shape);
    const QRectF band(0, 0, width(), kBandHeight);
    QLinearGradient bandFill(band.topLeft(), band.bottomLeft());
    bandFill.setColorAt(0, t.accentHi);
    bandFill.setColorAt(1, t.accentLo);
    painter.fillRect(band, bandFill);
    painter.fillRect(QRectF(0, kBandHeight - 2, width(), 2), QColor(0, 0, 0, 50));
    painter.setRenderHint(QPainter::Antialiasing, false);
    LobbyTheme::paintScanlines(painter, band, QColor(0, 0, 0, 23));
    paintCountdown(painter, this, m_view.expiresAtMs, m_view.durationMs);
    painter.restore();

    strokeCard(painter, shape, LobbyTheme::mix(t.accent, t.line2, 0.7));
}

// ── ChallengeTray ────────────────────────────────────────────────────

ChallengeTray::ChallengeTray(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("ChallengeTray");
    setVisible(false);

    m_cardsArea = new QScrollArea(this);
    m_cardsArea->setObjectName("ChallengeTrayCards");
    m_cardsArea->setFrameShape(QFrame::NoFrame);
    m_cardsArea->setWidgetResizable(false);
    m_cardsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cardsArea->viewport()->setAutoFillBackground(false);
    m_cardsBody = new QWidget;
    m_cardsArea->setWidget(m_cardsBody);
    // After setWidget, which turns it on: the app's palette isn't the lobby's.
    m_cardsBody->setAutoFillBackground(false);

    auto* summary = new SummaryCard(m_cardsBody);
    summary->hide();
    connect(summary->viewButton(), &QPushButton::clicked, this, &ChallengeTray::summaryViewClicked);
    connect(summary->closeButton(), &QToolButton::clicked, this, &ChallengeTray::summaryDismissed);
    m_summaryCard = summary;

    auto* outgoing = new OutgoingCard(m_cardsBody);
    outgoing->hide();
    connect(outgoing->cancelButton(), &QPushButton::clicked, this, &ChallengeTray::cancelClicked);
    m_outgoingCard = outgoing;

    auto* outcome = new OutcomeCard(m_cardsBody);
    outcome->hide();
    connect(outcome->closeButton(), &QToolButton::clicked, this, &ChallengeTray::clearOutcome);
    m_outcomeCard = outcome;

    m_logPanel = new LogPanel(this);
    m_logPanel->setObjectName("ChallengeLog");
    m_logPanel->hide();
    auto* panel = new QVBoxLayout(m_logPanel);
    panel->setContentsMargins(1, 0, 1, 1);
    panel->setSpacing(0);
    auto* header = new QWidget(m_logPanel);
    header->setFixedHeight(kPanelHeaderHeight);
    auto* head = new QHBoxLayout(header);
    head->setContentsMargins(12, 0, 12, 0);
    head->setSpacing(6);
    head->addWidget(new SectionMarker(SectionMarker::Kind::Plain, header));
    head->addWidget(makeSectionLabel(tr("Challenge log"), header));
    head->addStretch(1);
    panel->addWidget(header);

    m_rowsArea = new QScrollArea(m_logPanel);
    m_rowsArea->setObjectName("ChallengeLogRows");
    m_rowsArea->setFrameShape(QFrame::NoFrame);
    m_rowsArea->setWidgetResizable(true);
    m_rowsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rowsArea->viewport()->setAutoFillBackground(false);
    m_rowsBody = new QWidget;
    m_rowsLayout = new QVBoxLayout(m_rowsBody);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    m_rowsLayout->addStretch(1);
    m_rowsArea->setWidget(m_rowsBody);
    m_rowsBody->setAutoFillBackground(false);
    panel->addWidget(m_rowsArea, 1);

    m_empty = new QLabel(tr("Challenges you send, get or miss show up here."), m_logPanel);
    m_empty->setObjectName("ChallengeLogEmpty");
    m_empty->setFont(LobbyTheme::uiFont(11));
    m_empty->setProperty("role", "muted");
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setContentsMargins(16, 0, 16, 0);
    m_empty->setFixedHeight(kPanelEmptyHeight);
    panel->addWidget(m_empty);

    m_footer = new QWidget(m_logPanel);
    m_footer->setFixedHeight(kPanelFooterHeight);
    auto* foot = new QHBoxLayout(m_footer);
    foot->setContentsMargins(10, 0, 10, 2);
    foot->addStretch(1);
    auto* clear = new LobbyButton(tr("Clear log"), LobbyButton::Kind::Ghost, m_footer);
    clear->setObjectName("ChallengeLogClear");
    clear->setButtonSize(LobbyButton::Size::Compact);
    foot->addWidget(clear);
    panel->addWidget(m_footer);
    connect(clear, &QPushButton::clicked, this, &ChallengeTray::clearClicked);

    // Keeps "4 min ago" true while the log is open.
    m_clock = new QTimer(this);
    m_clock->setInterval(20'000);
    connect(m_clock, &QTimer::timeout, this, [this]() {
        for (QWidget* row : std::as_const(m_rows))
            static_cast<LogRow*>(row)->refreshTime();
    });
}

void ChallengeTray::setAnchor(QWidget* anchor)
{
    m_anchor = anchor;
}

void ChallengeTray::setAvailable(bool available)
{
    if (available == m_available)
        return;
    m_available = available;
    refreshVisibility();
}

void ChallengeTray::setIncoming(const QList<ChallengeCardView>& views)
{
    QList<ChallengeCard*> cards;
    for (const ChallengeCardView& view : views)
    {
        const auto found = std::find_if(m_incoming.cbegin(), m_incoming.cend(), [&view](const ChallengeCard* card) {
            return card->challengeId() == view.challengeId;
        });
        ChallengeCard* card = found != m_incoming.cend() ? *found : nullptr;
        if (!card)
        {
            card = new ChallengeCard(m_cardsBody);
            connect(card, &ChallengeCard::acceptClicked, this, [this, card]() { emit acceptClicked(card->challengeId()); });
            connect(card, &ChallengeCard::declineClicked, this, [this, card]() { emit declineClicked(card->challengeId()); });
        }
        card->setView(view);
        cards << card;
    }
    for (ChallengeCard* card : std::as_const(m_incoming))
    {
        // Later: this can run from inside one of the card's own clicks.
        if (!cards.contains(card))
        {
            card->hide();
            card->deleteLater();
        }
    }
    m_incoming = cards;
    relayout();
}

void ChallengeTray::setOutgoing(const OutgoingChallengeView& view)
{
    auto* card = static_cast<OutgoingCard*>(m_outgoingCard);
    m_outgoingShown = view.challengeId != 0;
    if (m_outgoingShown)
        card->setView(view, tr("Cancel"));
    else
        card->stop();
    relayout();
}

void ChallengeTray::showOutcome(const QString& title, const QString& detail, Role role, const QString& icon)
{
    static_cast<OutcomeCard*>(m_outcomeCard)->setOutcome(title, detail, role, icon, tr("Dismiss"));
    m_outcomeShown = true;
    const int generation = ++m_outcomeGeneration;
    QTimer::singleShot(kOutcomeMs, this, [this, generation]() {
        if (generation == m_outcomeGeneration)
            clearOutcome();
    });
    relayout();
}

void ChallengeTray::clearOutcome()
{
    ++m_outcomeGeneration;
    if (!m_outcomeShown)
        return;
    m_outcomeShown = false;
    relayout();
}

void ChallengeTray::setSummary(const QString& title, const QString& names)
{
    m_summaryShown = !title.isEmpty();
    if (m_summaryShown)
        static_cast<SummaryCard*>(m_summaryCard)->setSummary(title, names, tr("View"), tr("Dismiss"));
    relayout();
}

void ChallengeTray::setLogRows(const QList<ChallengeLogRowView>& rows)
{
    while (m_rows.size() < rows.size())
    {
        auto* row = new LogRow(m_rowsBody);
        m_rowsLayout->insertWidget(int(m_rows.size()), row);
        connect(row->actionButton(), &QPushButton::clicked, this, [this, row]() { emit actionClicked(row->key()); });
        connect(row->messageButton(), &QToolButton::clicked, this, [this, row]() { emit messageClicked(row->key()); });
        m_rows << row;
    }
    while (m_rows.size() > rows.size())
    {
        // Later: this can run from inside one of the row's own clicks.
        QWidget* row = m_rows.takeLast();
        row->hide();
        row->deleteLater();
    }
    for (int i = 0; i < rows.size(); ++i)
        static_cast<LogRow*>(m_rows.at(i))->setView(rows.at(i), i == rows.size() - 1, tr("Send a message"));
    m_empty->setVisible(rows.isEmpty());
    m_footer->setVisible(!rows.isEmpty());
    m_rowsArea->setVisible(!rows.isEmpty());
    relayout();
}

void ChallengeTray::setLogOpen(bool open)
{
    if (open == m_logOpen)
        return;
    m_logOpen = open;
    if (open)
    {
        qApp->installEventFilter(this);
        m_clock->start();
        m_rowsArea->verticalScrollBar()->setValue(0);
    }
    else
    {
        qApp->removeEventFilter(this);
        m_clock->stop();
    }
    relayout();
}

bool ChallengeTray::hasLiveCards() const
{
    return m_summaryShown || !m_incoming.isEmpty() || m_outgoingShown || m_outcomeShown;
}

void ChallengeTray::place(const QPoint& topRight, int maxWidth, int maxHeight)
{
    m_topRight = topRight;
    m_maxWidth = maxWidth;
    m_maxHeight = maxHeight;
    relayout();
}

QString ChallengeTray::relativeTime(qint64 timeMs)
{
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(timeMs);
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - timeMs;
    if (age < 60'000)
        return tr("just now");
    if (age < 60 * 60'000)
        return tr("%1 min ago").arg(age / 60'000);
    if (when.date() == QDate::currentDate())
        return when.toString(QStringLiteral("hh:mm"));
    return QLocale().toString(when.date(), QLocale::ShortFormat) + QLatin1Char(' ') + when.toString(QStringLiteral("hh:mm"));
}

void ChallengeTray::paintEvent(QPaintEvent*)
{
    // The window's own colour under the cards, so the gaps between them show
    // no slivers of the lobby.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(LobbyTheme::tokens().bg);
    painter.drawRoundedRect(QRectF(rect()), kCardRadius, kCardRadius);
}

bool ChallengeTray::eventFilter(QObject* watched, QEvent* event)
{
    // A click anywhere else in the lobby window closes the log. Other windows
    // (a dialog the log opened, a menu) leave it be.
    if (m_logOpen && event->type() == QEvent::MouseButtonPress && watched->isWidgetType() &&
        static_cast<QWidget*>(watched)->window() == window())
    {
        const QPoint global = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
        const auto contains = [&global](const QWidget* widget) {
            return widget && widget->isVisible() && QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size()).contains(global);
        };
        if (!contains(this) && !contains(m_anchor))
        {
            setLogOpen(false);
            emit logClosed();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChallengeTray::relayout()
{
    const int width = std::max(0, std::min(kTrayWidth, m_maxWidth));

    QList<QWidget*> cards;
    if (m_summaryShown)
        cards << m_summaryCard;
    for (ChallengeCard* card : std::as_const(m_incoming))
        cards << card;
    if (m_outgoingShown)
        cards << m_outgoingCard;
    if (m_outcomeShown)
        cards << m_outcomeCard;

    const auto heightAt = [](QWidget* card, int cardWidth) {
        return card->hasHeightForWidth() ? card->heightForWidth(cardWidth) : card->sizeHint().height();
    };
    const auto stackHeight = [&](int cardWidth) {
        int total = 0;
        for (QWidget* card : std::as_const(cards))
            total += heightAt(card, cardWidth) + kCardGap;
        return cards.isEmpty() ? 0 : total - kCardGap;
    };

    int logWanted = 0;
    if (m_logOpen)
    {
        logWanted = kPanelHeaderHeight + 1 +
                    (m_rows.isEmpty() ? kPanelEmptyHeight : int(m_rows.size()) * kRowHeight + kPanelFooterHeight);
    }
    // The log keeps a few rows on screen; the live cards scroll before it goes.
    const int logFloor = std::min(logWanted, kLogMinHeight);
    const int cardsRoom = m_logOpen ? std::max(0, m_maxHeight - logFloor - kCardGap) : m_maxHeight;
    int cardsHeight = stackHeight(width);
    const bool scrolls = cardsHeight > cardsRoom;
    const int cardWidth = scrolls ? width - 10 : width;
    if (scrolls)
        cardsHeight = stackHeight(cardWidth);

    int y = 0;
    for (QWidget* card : std::as_const(cards))
    {
        const int height = heightAt(card, cardWidth);
        card->setGeometry(0, y, cardWidth, height);
        card->show();
        y += height + kCardGap;
    }
    for (QWidget* card : { m_summaryCard, m_outgoingCard, m_outcomeCard })
    {
        if (!cards.contains(card))
            card->hide();
    }
    m_cardsBody->setFixedSize(std::max(1, cardWidth), std::max(1, cardsHeight));
    const int cardsShown = std::min(cardsHeight, cardsRoom);
    m_cardsArea->setVerticalScrollBarPolicy(scrolls ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    m_cardsArea->setGeometry(0, 0, width, cardsShown);
    m_cardsArea->setVisible(cardsShown > 0);

    int height = cardsShown;
    if (m_logOpen)
    {
        const int top = cardsShown > 0 ? cardsShown + kCardGap : 0;
        const int logHeight = std::max(0, std::min(logWanted, m_maxHeight - top));
        m_logPanel->setGeometry(0, top, width, logHeight);
        m_logPanel->setVisible(logHeight > 0);
        height = top + logHeight;
    }
    else
    {
        m_logPanel->hide();
    }
    setGeometry(m_topRight.x() - width, m_topRight.y(), width, height);
    refreshVisibility();
}

void ChallengeTray::refreshVisibility()
{
    const bool shown = m_available && (m_logOpen || hasLiveCards());
    if (shown != isVisible())
        setVisible(shown);
    if (shown)
        raise();
}

#endif // NETPLAY
