#ifdef NETPLAY

#include "LobbySocialWidgets.hpp"

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <memory>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

// ── LobbyMenu ────────────────────────────────────────────────────────

namespace
{
    constexpr int kMenuIconLeft = 11;
    constexpr int kMenuIconSize = 15;
    constexpr int kMenuTextLeft = 36;
    constexpr int kMenuRadius   = 8;

    const char* const kIconProperty   = "lobbyMenuIcon";
    const char* const kDangerProperty = "lobbyMenuDanger";
    const char* const kNoteProperty   = "lobbyMenuNote";
    const char* const kPingProperty   = "lobbyMenuPing";     // ping limit bars as the icon
    const char* const kStayProperty   = "lobbyMenuStayOpen"; // triggering it leaves the menu open

    // A heading between a menu's groups of items.
    class MenuHeading : public QWidget
    {
    public:
        MenuHeading(const QString& iconName, const QString& text, bool moderator, QWidget* parent)
            : QWidget(parent), m_icon(iconName), m_text(text.toUpper()), m_moderator(moderator)
        {
            setFixedHeight(28);
            setAttribute(Qt::WA_TransparentForMouseEvents);
        }

        // QMenu leaves out a widget whose size hint is empty, which a fixed
        // height alone (width 0) is.
        QSize sizeHint() const override
        {
            const int text = QFontMetrics(LobbyTheme::pixelFont(9, 0.9)).horizontalAdvance(m_text);
            return QSize(kMenuIconLeft + (m_icon.isEmpty() ? 0 : 16) + text + 8, 28);
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            const Tokens& t = LobbyTheme::tokens();
            const QColor color = m_moderator ? t.accentInk : t.muted;
            painter.fillRect(QRect(0, 4, width(), 1), t.line);
            int x = kMenuIconLeft;
            if (!m_icon.isEmpty())
            {
                painter.drawPixmap(x, 13, LobbyTheme::icon(m_icon, color, 11, devicePixelRatioF()));
                x += 16;
            }
            painter.setFont(LobbyTheme::pixelFont(9, 0.9));
            painter.setPen(color);
            painter.drawText(QRect(x, 11, width() - x - 8, 15), Qt::AlignLeft | Qt::AlignVCenter, m_text);
        }

    private:
        QString m_icon;
        QString m_text;
        bool m_moderator;
    };
} // namespace

LobbyMenu::LobbyMenu(QWidget* parent)
    : QMenu(parent)
{
    setObjectName("LobbyMenu");
    setFont(LobbyTheme::uiFont(12));
    // Rounded corners need the window itself to be see-through at the corners.
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowFlag(Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumWidth(232);
}

QAction* LobbyMenu::addItem(const QString& iconName, const QString& text)
{
    QAction* action = addAction(text);
    action->setProperty(kIconProperty, iconName);
    return action;
}

QAction* LobbyMenu::addDangerItem(const QString& iconName, const QString& text)
{
    QAction* action = addItem(iconName, text);
    action->setProperty(kDangerProperty, true);
    return action;
}

QAction* LobbyMenu::addDisabledItem(const QString& iconName, const QString& text, const QString& why)
{
    QAction* action = addItem(iconName, text);
    action->setEnabled(false);
    action->setProperty(kNoteProperty, why);
    return action;
}

LobbyMenu* LobbyMenu::addSubmenu(const QString& iconName, const QString& text)
{
    auto* submenu = new LobbyMenu(this);
    submenu->setTitle(text);
    submenu->setMinimumWidth(150);
    QAction* action = addMenu(submenu);
    action->setProperty(kIconProperty, iconName);
    return submenu;
}

void LobbyMenu::addHeading(const QString& iconName, const QString& text, bool moderator)
{
    addHeaderWidget(new MenuHeading(iconName, text, moderator, this));
}

void LobbyMenu::addHeaderWidget(QWidget* widget)
{
    auto* action = new QWidgetAction(this);
    action->setDefaultWidget(widget);
    action->setEnabled(false);
    addAction(action);
}

void LobbyMenu::addLiveWidget(QWidget* widget)
{
    auto* action = new QWidgetAction(this);
    action->setDefaultWidget(widget);
    addAction(action);
}

QAction* LobbyMenu::addCheckItem(const QString& iconName, const QString& text, bool checked, QActionGroup* group)
{
    QAction* action = addItem(iconName, text);
    action->setCheckable(true);
    action->setChecked(checked);
    action->setProperty(kStayProperty, true);
    if (group)
        group->addAction(action);
    return action;
}

QAction* LobbyMenu::addPingItem(int maxPingMs, const QString& text, bool checked, QActionGroup* group)
{
    QAction* action = addCheckItem(QString(), text, checked, group);
    action->setProperty(kPingProperty, maxPingMs);
    return action;
}

void LobbyMenu::mouseReleaseEvent(QMouseEvent* event)
{
    QAction* action = actionAt(event->position().toPoint());
    if (action && action->isEnabled() && action->property(kStayProperty).toBool())
    {
        action->trigger();
        update();
        return;
    }
    QMenu::mouseReleaseEvent(event);
}

void LobbyMenu::keyPressEvent(QKeyEvent* event)
{
    QAction* action = activeAction();
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) &&
        action && action->isEnabled() && action->property(kStayProperty).toBool())
    {
        action->trigger();
        update();
        return;
    }
    QMenu::keyPressEvent(event);
}

void LobbyMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
    // Item widths are measured before notes exist as far as Qt knows.
    int wanted = minimumWidth();
    const QFontMetrics fm(font());
    const QFontMetrics noteMetrics(LobbyTheme::uiFont(10));
    for (QAction* action : actions())
    {
        const QString note = action->property(kNoteProperty).toString();
        if (!note.isEmpty())
            wanted = std::max(wanted, kMenuTextLeft + fm.horizontalAdvance(action->text()) + 16 +
                                          noteMetrics.horizontalAdvance(note) + 11);
    }
    if (wanted > width())
        resize(wanted, height());
}

void LobbyMenu::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(box, kMenuRadius, kMenuRadius);
    painter.fillPath(shape, t.panel2);
    painter.setClipPath(shape);

    for (QAction* action : actions())
    {
        const QRect r = actionGeometry(action).adjusted(1, 0, -1, 0);
        if (!action->isVisible() || r.isEmpty() || qobject_cast<QWidgetAction*>(action))
            continue;
        if (action->isSeparator())
        {
            painter.fillRect(QRect(0, r.center().y(), width(), 1), t.line);
            continue;
        }

        const bool enabled = action->isEnabled();
        const bool danger = action->property(kDangerProperty).toBool();
        const bool active = enabled && action == activeAction();
        if (active)
            painter.fillRect(r, danger ? LobbyTheme::mix(t.bad, t.panel2, 0.16) : t.raise);

        const QColor ink = !enabled ? t.faint : danger ? t.bad : active ? t.ink : t.ink2;
        const QColor iconColor = !enabled ? t.faint : danger ? t.bad : active ? t.ink2 : t.muted;

        const QString iconName = action->property(kIconProperty).toString();
        const QVariant ping = action->property(kPingProperty);
        const int textLeft = iconName.isEmpty() && !ping.isValid() ? 13 : kMenuTextLeft;
        if (!iconName.isEmpty())
        {
            painter.drawPixmap(r.left() + kMenuIconLeft, r.center().y() - kMenuIconSize / 2,
                               LobbyTheme::icon(iconName, iconColor, kMenuIconSize, devicePixelRatioF()));
        }
        else if (ping.isValid())
        {
            painter.drawPixmap(r.left() + kMenuIconLeft, r.center().y() - 7,
                               LobbyTheme::pingLimitIcon(ping.toInt(), iconColor, devicePixelRatioF()));
        }
        if (action->isCheckable() && action->isChecked())
        {
            painter.drawPixmap(r.right() - 24, r.center().y() - 7,
                               LobbyTheme::icon(QStringLiteral("check-line"), enabled ? t.accentInk : t.faint, 14,
                                                devicePixelRatioF()));
        }
        painter.setFont(font());
        painter.setPen(ink);
        painter.drawText(r.adjusted(textLeft, 0, -11, 0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextHideMnemonic, action->text());

        const QString note = action->property(kNoteProperty).toString();
        if (action->menu())
        {
            painter.drawPixmap(r.right() - 20, r.center().y() - 7,
                               LobbyTheme::icon(QStringLiteral("arrow-right-s-line"), t.muted, 14, devicePixelRatioF()));
        }
        else if (!note.isEmpty())
        {
            painter.setFont(LobbyTheme::uiFont(10));
            painter.setPen(t.faint);
            painter.drawText(r.adjusted(textLeft, 0, -11, 0), Qt::AlignRight | Qt::AlignVCenter, note);
        }
    }

    painter.setClipping(false);
    painter.setPen(QPen(t.line2, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
}

// ── ChatTabBar ───────────────────────────────────────────────────────

void ChatTabBar::showTab(int index, bool visible)
{
    if (isTabVisible(index) != visible)
        setTabVisible(index, visible);
}

QSize ChatTabBar::tabSizeHint(int index) const
{
    QSize size = QTabBar::tabSizeHint(index);
    if (tabText(index).isEmpty())
    {
        const QWidget* badge = tabButton(index, QTabBar::RightSide);
        size.setWidth(38 + (badge ? badge->width() + 2 : 0));
    }
    return size;
}

// ── PlayerTile ───────────────────────────────────────────────────────

PlayerTile::PlayerTile(int side, QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(side, side);
}

void PlayerTile::setPlayer(const QString& name)
{
    m_name = name;
    update();
}

void PlayerTile::setDimmed(bool dimmed)
{
    m_dimmed = dimmed;
    update();
}

void PlayerTile::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    uint hash = 0;
    for (const QChar ch : m_name)
        hash = hash * 31 + ch.toLower().unicode();
    const int slot = 1 + int(hash % 4);
    QColor color = LobbyTheme::portColor(slot);
    if (m_dimmed)
        color = LobbyTheme::mix(color, t.line2, 0.25);

    const qreal radius = width() * 0.22;
    QPainterPath shape;
    shape.addRoundedRect(QRectF(rect()), radius, radius);
    QLinearGradient fill(0, 0, width(), height());
    fill.setColorAt(0, LobbyTheme::mix(color, QColor(Qt::white), 0.85));
    fill.setColorAt(1, LobbyTheme::mix(color, QColor(Qt::black), 0.55));
    painter.fillPath(shape, fill);
    painter.setClipPath(shape);
    painter.fillRect(QRect(0, height() - 2, width(), 2), QColor(0, 0, 0, 56));
    painter.fillRect(QRect(0, 0, width(), 1), QColor(255, 255, 255, 70));
    painter.setRenderHint(QPainter::Antialiasing, false);
    LobbyTheme::paintScanlines(painter, QRectF(rect()), QColor(0, 0, 0, 28));

    const bool darkInk = !t.dark ? false : (slot == 3 || slot == 4);
    painter.setPen(darkInk ? QColor(0, 0, 0, 200) : QColor(Qt::white));
    painter.setFont(LobbyTheme::pixelFont(int(width() * 0.46)));
    painter.drawText(rect(), Qt::AlignCenter, LobbyTheme::nameInitial(m_name));
}

// ── ChatView ─────────────────────────────────────────────────────────

namespace
{
    const QString kUserScheme = QStringLiteral("lobby-user:");

    // QTextEdit::append only follows new lines while the view is exactly at
    // the bottom. When the text grows without an append, the reader is left
    // a few lines up and every later append stays parked there. That happens
    // when the splitter narrows the chat column and lines rewrap. Remember
    // whether the reader was at the bottom and keep them there when the range
    // grows; scrolling up still holds the view.
    void keepChatAtBottom(QTextEdit* view)
    {
        QScrollBar* bar = view->verticalScrollBar();
        auto atBottom = std::make_shared<bool>(true);
        QObject::connect(bar, &QScrollBar::valueChanged, view, [bar, atBottom](int value) {
            *atBottom = value >= bar->maximum();
        });
        QObject::connect(bar, &QScrollBar::rangeChanged, view, [bar, atBottom](int, int max) {
            if (*atBottom) bar->setValue(max);
        });
    }
} // namespace

ChatView::ChatView(QWidget* parent)
    : QTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setFrameShape(QFrame::NoFrame);
    setFont(LobbyTheme::uiFont(12));
    viewport()->setAutoFillBackground(false);
    viewport()->setMouseTracking(true);
    keepChatAtBottom(this);
}

QString ChatView::userHref(quint64 userId)
{
    return kUserScheme + QString::number(userId);
}

void ChatView::addLine(const QString& html)
{
    append(html);

    // Breathing room between messages.
    QTextCursor cursor(document()->lastBlock());
    QTextBlockFormat format = cursor.blockFormat();
    format.setBottomMargin(5);
    cursor.setBlockFormat(format);
}

quint64 ChatView::userAt(const QPoint& pos) const
{
    const QString href = anchorAt(pos);
    return href.startsWith(kUserScheme) ? href.mid(kUserScheme.size()).toULongLong() : 0;
}

void ChatView::mousePressEvent(QMouseEvent* event)
{
    m_pressPos = event->position().toPoint();
    QTextEdit::mousePressEvent(event);
}

void ChatView::mouseMoveEvent(QMouseEvent* event)
{
    viewport()->setCursor(userAt(event->position().toPoint()) != 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    QTextEdit::mouseMoveEvent(event);
}

void ChatView::mouseReleaseEvent(QMouseEvent* event)
{
    QTextEdit::mouseReleaseEvent(event);
    // A click, not the end of selecting text.
    const QPoint pos = event->position().toPoint();
    if (event->button() != Qt::LeftButton || (pos - m_pressPos).manhattanLength() > 3)
        return;
    if (const quint64 userId = userAt(pos))
        emit userActivated(userId, event->globalPosition().toPoint());
}

void ChatView::contextMenuEvent(QContextMenuEvent* event)
{
    // A scroll area receives this from its viewport, in the viewport's coordinates.
    if (const quint64 userId = userAt(event->pos()))
    {
        emit userActivated(userId, event->globalPos());
        return;
    }
    QTextEdit::contextMenuEvent(event);
}

// ── ChallengeCard ────────────────────────────────────────────────────

namespace
{
    constexpr int kBandHeight = 30;
    constexpr int kCountdownHeight = 3;

    QLabel* makeVsLabel(QWidget* parent)
    {
        auto* vs = new QLabel(QStringLiteral("VS"), parent);
        QFont font(QStringLiteral("Arial Black"));
        font.setPixelSize(24);
        font.setItalic(true);
        font.setWeight(QFont::Black);
        vs->setFont(font);
        vs->setProperty("role", "faint");
        return vs;
    }
} // namespace

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
    bandLay->setContentsMargins(12, 0, 12, 0);
    bandLay->setSpacing(8);
    m_bandIcon = new IconLabel(QStringLiteral("sword-fill"), Role::Ink, 15, band);
    m_bandIcon->setColorOverride(Qt::white);
    bandLay->addWidget(m_bandIcon);
    m_bandTitle = new QLabel(band);
    m_bandTitle->setFont(LobbyTheme::pixelFont(13, 1.0));
    m_bandTitle->setStyleSheet(QStringLiteral("color: #ffffff;"));
    bandLay->addWidget(m_bandTitle);
    bandLay->addStretch(1);
    m_more = new QLabel(band);
    m_more->setFont(LobbyTheme::uiFont(10, QFont::Bold));
    m_more->setStyleSheet(QStringLiteral("color: #ffffff; background: rgba(0,0,0,70); border-radius: 4px; padding: 1px 6px;"));
    bandLay->addWidget(m_more);
    m_seconds = new QLabel(band);
    m_seconds->setFont(LobbyTheme::pixelFont(13));
    m_seconds->setStyleSheet(QStringLiteral("color: #ffffff;"));
    bandLay->addWidget(m_seconds);
    root->addWidget(band);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(18, 14, 18, 12);
    body->setSpacing(10);

    // The challenger, on the left.
    auto* fromSide = new QHBoxLayout;
    fromSide->setSpacing(11);
    m_fromTile = new PlayerTile(44, this);
    fromSide->addWidget(m_fromTile);
    auto* fromWords = new QVBoxLayout;
    fromWords->setSpacing(2);
    fromWords->setAlignment(Qt::AlignVCenter);
    auto* fromNameRow = new QHBoxLayout;
    fromNameRow->setSpacing(6);
    m_fromName = new ElidedLabel(Role::Ink, this);
    m_fromName->setFont(LobbyTheme::uiFont(15, QFont::Bold));
    m_fromFlag = new CountryFlag(this);
    fromNameRow->addWidget(m_fromName);
    fromNameRow->addWidget(m_fromFlag);
    fromNameRow->addStretch(1);
    fromWords->addLayout(fromNameRow);
    auto* fromSub = new QHBoxLayout;
    fromSub->setSpacing(5);
    m_fromBars = new SignalBars(this);
    m_fromPing = new QLabel(this);
    m_fromPing->setFont(LobbyTheme::uiFont(11));
    m_fromPing->setProperty("role", "muted");
    m_fromConnection = new ConnectionGlyph(this);
    m_fromConnectionName = new QLabel(this);
    m_fromConnectionName->setFont(LobbyTheme::uiFont(11));
    m_fromConnectionName->setProperty("role", "muted");
    fromSub->addWidget(m_fromBars);
    fromSub->addWidget(m_fromPing);
    fromSub->addWidget(m_fromConnection);
    fromSub->addWidget(m_fromConnectionName);
    fromSub->addStretch(1);
    fromWords->addLayout(fromSub);
    fromSide->addLayout(fromWords, 1);
    body->addLayout(fromSide, 1);

    m_vs = makeVsLabel(this);
    body->addWidget(m_vs, 0, Qt::AlignCenter);

    // Who it's for, on the right.
    m_toSide = new QWidget(this);
    auto* toSide = new QHBoxLayout(m_toSide);
    toSide->setContentsMargins(0, 0, 0, 0);
    toSide->setSpacing(11);
    auto* toWords = new QVBoxLayout;
    toWords->setSpacing(2);
    toWords->setAlignment(Qt::AlignVCenter);
    auto* toNameRow = new QHBoxLayout;
    toNameRow->setSpacing(6);
    toNameRow->addStretch(1);
    m_toName = new ElidedLabel(Role::Ink, m_toSide);
    m_toName->setFont(LobbyTheme::uiFont(15, QFont::Bold));
    m_toTag = new StatusTag(StatusTag::Kind::Neutral, m_toSide);
    m_toFlag = new CountryFlag(m_toSide);
    toNameRow->addWidget(m_toName);
    toNameRow->addWidget(m_toTag);
    toNameRow->addWidget(m_toFlag);
    toWords->addLayout(toNameRow);
    auto* toSub = new QHBoxLayout;
    toSub->setSpacing(5);
    toSub->addStretch(1);
    m_toConnection = new ConnectionGlyph(m_toSide);
    m_toDetail = new QLabel(m_toSide);
    m_toDetail->setFont(LobbyTheme::uiFont(11));
    m_toDetail->setProperty("role", "muted");
    toSub->addWidget(m_toConnection);
    toSub->addWidget(m_toDetail);
    toWords->addLayout(toSub);
    toSide->addLayout(toWords, 1);
    m_toTile = new PlayerTile(44, m_toSide);
    toSide->addWidget(m_toTile);
    body->addWidget(m_toSide, 1);

    // An invite has a room to show instead of an opponent.
    m_seatsSide = new QWidget(this);
    auto* seats = new QVBoxLayout(m_seatsSide);
    seats->setContentsMargins(0, 0, 0, 0);
    seats->setSpacing(5);
    seats->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    m_seatsLabel = new QLabel(m_seatsSide);
    m_seatsLabel->setFont(LobbyTheme::uiFont(15, QFont::Bold));
    m_seatsLabel->setAlignment(Qt::AlignRight);
    seats->addWidget(m_seatsLabel);
    m_seatsPips = new SeatPips(m_seatsSide);
    seats->addWidget(m_seatsPips, 0, Qt::AlignRight);
    body->addWidget(m_seatsSide, 1);
    root->addLayout(body);

    auto* foot = new QHBoxLayout;
    foot->setContentsMargins(18, 0, 12, 12);
    foot->setSpacing(10);
    m_tile = new CartridgeTile(CartridgeTile::Size::Small, this);
    foot->addWidget(m_tile);
    auto* gameWords = new QVBoxLayout;
    gameWords->setSpacing(1);
    gameWords->setAlignment(Qt::AlignVCenter);
    m_game = new ElidedLabel(Role::Ink, this);
    m_game->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    m_game->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_gameDetail = new ElidedLabel(Role::Muted, this);
    m_gameDetail->setFont(LobbyTheme::uiFont(11));
    m_gameDetail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    gameWords->addWidget(m_game);
    gameWords->addWidget(m_gameDetail);
    foot->addLayout(gameWords, 1);
    m_warnIcon = new IconLabel(QStringLiteral("error-warning-line"), Role::Warn, 13, this);
    foot->addWidget(m_warnIcon);
    m_warn = new ElidedLabel(Role::Warn, this);
    m_warn->setFont(LobbyTheme::uiFont(11));
    foot->addWidget(m_warn);
    m_decline = new LobbyButton(tr("Decline"), LobbyButton::Kind::Secondary, this);
    m_decline->setObjectName("ChallengeDecline");
    m_accept = new LobbyButton(tr("Accept"), LobbyButton::Kind::Primary, this);
    m_accept->setObjectName("ChallengeAccept");
    m_accept->setIconName(QStringLiteral("check-line"));
    m_accept->setPixelLabel(true);
    foot->addWidget(m_decline);
    foot->addWidget(m_accept);
    root->addLayout(foot);

    connect(m_accept, &QPushButton::clicked, this, &ChallengeCard::acceptClicked);
    connect(m_decline, &QPushButton::clicked, this, &ChallengeCard::declineClicked);

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &ChallengeCard::tick);
    m_timer->start();
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
    m_bandIcon->setIconName(view.invite ? QStringLiteral("mail-send-line") : QStringLiteral("sword-fill"));
    m_bandTitle->setText((view.invite ? tr("Room invite") : tr("New challenger")).toUpper());
    m_more->setText(tr("+%1 more").arg(view.waitingBehind));
    m_more->setVisible(view.waitingBehind > 0);

    m_fromTile->setPlayer(view.fromName);
    m_fromName->setText(view.fromName);
    m_fromFlag->setCountry(view.fromCountry);
    m_fromBars->setPing(view.fromPingMs, view.fromPingEstimate);
    m_fromBars->setVisible(view.fromPingMs >= 0);
    m_fromPing->setText(view.fromPingMs >= 0
        ? QString(view.fromPingEstimate ? "~%1 ms" : "%1 ms").arg(view.fromPingMs) : QString());
    m_fromPing->setVisible(view.fromPingMs >= 0);
    m_fromConnection->setConnection(view.fromConnection);
    m_fromConnectionName->setText(ConnectionGlyph::displayName(view.fromConnection));

    m_vs->setVisible(!view.invite);
    m_toSide->setVisible(!view.invite);
    m_seatsSide->setVisible(view.invite);
    m_toTile->setPlayer(view.toName);
    m_toName->setText(view.toName);
    m_toTag->setTag(StatusTag::Kind::Neutral, view.toTag);
    m_toFlag->setCountry(view.toCountry);
    m_toConnection->setConnection(view.toConnection);
    m_toConnection->setVisible(!view.toConnection.isEmpty());
    m_toDetail->setText(view.toDetail);
    m_seatsLabel->setText(tr("%1 of %2 seats").arg(view.roomPlayers).arg(view.roomMaxPlayers));
    m_seatsPips->setSeats(view.roomPlayers, view.roomMaxPlayers);

    m_tile->setGame(view.game, view.coverPath);
    m_game->setText(view.invite ? view.gameDetail : view.game);
    m_gameDetail->setText(view.invite ? view.game : view.gameDetail);
    m_gameDetail->setVisible(!m_gameDetail->text().isEmpty());
    m_warn->setText(view.warning);
    m_warn->setVisible(!view.warning.isEmpty());
    m_warnIcon->setVisible(!view.warning.isEmpty());
    m_accept->setEnabled(view.canAccept);
    tick();
}

void ChallengeCard::tick()
{
    const qint64 left = std::max<qint64>(0, m_view.expiresAtMs - QDateTime::currentMSecsSinceEpoch());
    m_seconds->setText(tr("%1 sec").arg((left + 999) / 1000).toUpper());
    update(0, height() - kCountdownHeight - 1, width(), kCountdownHeight + 1);
}

void ChallengeCard::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(box, 12, 12);
    QLinearGradient fill(box.topLeft(), box.bottomLeft());
    fill.setColorAt(0, t.panel2);
    fill.setColorAt(1, t.panel);
    painter.fillPath(shape, fill);

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

    // The time left drains along the bottom edge.
    const qint64 left = std::max<qint64>(0, m_view.expiresAtMs - QDateTime::currentMSecsSinceEpoch());
    const qreal fraction = m_view.durationMs > 0 ? qreal(left) / m_view.durationMs : 0.0;
    painter.fillRect(QRectF(0, height() - kCountdownHeight, width(), kCountdownHeight), t.tint);
    painter.fillRect(QRectF(0, height() - kCountdownHeight, width() * std::min(1.0, fraction), kCountdownHeight), t.accentHi);
    painter.restore();

    painter.setPen(QPen(LobbyTheme::mix(t.accent, t.line2, 0.7), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
}

// ── UndoToast ────────────────────────────────────────────────────────

namespace
{
    constexpr qint64 kToastMs = 8'000;

    // How much of the toast's time is left, as a thin bar.
    class ToastMeter : public QWidget
    {
    public:
        explicit ToastMeter(QWidget* parent) : QWidget(parent) { setFixedSize(34, 3); }
        void setFraction(qreal fraction) { m_fraction = fraction; update(); }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            const Tokens& t = LobbyTheme::tokens();
            painter.fillRect(rect(), t.line2);
            painter.fillRect(QRectF(0, 0, width() * m_fraction, height()), t.ink2);
        }

    private:
        qreal m_fraction = 1.0;
    };
} // namespace

UndoToast::UndoToast(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("UndoToast");
    setFixedHeight(40);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 0, 8, 0);
    lay->setSpacing(10);
    lay->addWidget(new IconLabel(QStringLiteral("check-line"), Role::Ok, 16, this));
    m_text = new QLabel(this);
    m_text->setFont(LobbyTheme::uiFont(12));
    m_text->setTextFormat(Qt::RichText);
    lay->addWidget(m_text);
    m_meter = new ToastMeter(this);
    lay->addWidget(m_meter);
    m_undo = new LobbyButton(tr("Undo"), LobbyButton::Kind::Ghost, this);
    m_undo->setObjectName("ToastUndo");
    m_undo->setButtonSize(LobbyButton::Size::Small);
    lay->addWidget(m_undo);
    connect(m_undo, &QPushButton::clicked, this, [this]() {
        hide();
        m_timer->stop();
        emit undoClicked();
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &UndoToast::tick);
    setVisible(false);
}

void UndoToast::showMessage(const QString& text, bool canUndo)
{
    m_text->setText(text);
    m_undo->setVisible(canUndo);
    m_meter->setVisible(canUndo);
    m_shownAtMs = QDateTime::currentMSecsSinceEpoch();
    m_timer->start();
    adjustSize();
    show();
    raise();
    tick();
}

void UndoToast::tick()
{
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_shownAtMs;
    if (elapsed >= kToastMs)
    {
        m_timer->stop();
        hide();
        return;
    }
    static_cast<ToastMeter*>(m_meter)->setFraction(1.0 - qreal(elapsed) / kToastMs);
}

void UndoToast::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    painter.setPen(QPen(t.line2, 1.0));
    painter.setBrush(t.panel2);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
}

// ── MentionPopup ─────────────────────────────────────────────────────

namespace
{
    constexpr int kMentionRow = 26;
    constexpr int kMentionHeading = 22;
    constexpr int kMentionFooter = 25;

} // namespace

QColor UserInterface::Dialog::presenceColor(const QString& state)
{
    const Tokens& t = LobbyTheme::tokens();
    if (state == QLatin1String("hosting") || state == QLatin1String("in_room")) return t.stateRoom;
    if (state == QLatin1String("searching")) return t.warn;
    if (state == QLatin1String("playing"))   return t.live;
    if (state == QLatin1String("spectating")) return t.stateWatch;
    if (state == QLatin1String("idle") || state == QLatin1String("browsing")) return t.ok;
    return t.muted;
}

MentionPopup::MentionPopup(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("MentionPopup");
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setVisible(false);
}

void MentionPopup::setCandidates(const QList<MentionCandidate>& candidates, int typed, const QString& heading)
{
    m_rows = candidates;
    m_typed = typed;
    m_heading = heading;
    m_selected = 0;
    updateGeometry();
    update();
}

void MentionPopup::moveSelection(int delta)
{
    if (m_rows.isEmpty())
        return;
    m_selected = (m_selected + delta + int(m_rows.size())) % int(m_rows.size());
    update();
}

QString MentionPopup::selectedName() const
{
    return m_selected >= 0 && m_selected < m_rows.size() ? m_rows.at(m_selected).name : QString();
}

int MentionPopup::rowsTop() const
{
    return m_heading.isEmpty() ? 1 : kMentionHeading;
}

QSize MentionPopup::sizeHint() const
{
    return QSize(200, rowsTop() + int(m_rows.size()) * kMentionRow + kMentionFooter);
}

int MentionPopup::rowAt(const QPoint& pos) const
{
    const int row = (pos.y() - rowsTop()) / kMentionRow;
    return pos.y() >= rowsTop() && row < m_rows.size() ? row : -1;
}

void MentionPopup::mouseMoveEvent(QMouseEvent* event)
{
    const int row = rowAt(event->position().toPoint());
    if (row >= 0 && row != m_selected)
    {
        m_selected = row;
        update();
    }
}

void MentionPopup::mousePressEvent(QMouseEvent* event)
{
    const int row = rowAt(event->position().toPoint());
    if (row >= 0)
        emit picked(m_rows.at(row).name);
}

void MentionPopup::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(box, 8, 8);
    painter.fillPath(shape, t.panel2);
    painter.setClipPath(shape);

    if (!m_heading.isEmpty())
    {
        QFont font = LobbyTheme::uiFont(9, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        painter.setFont(font);
        painter.setPen(t.muted);
        painter.drawText(QRect(10, 0, width() - 20, kMentionHeading), Qt::AlignLeft | Qt::AlignVCenter, m_heading.toUpper());
    }

    int y = rowsTop();
    for (int i = 0; i < m_rows.size(); ++i, y += kMentionRow)
    {
        const MentionCandidate& row = m_rows.at(i);
        const QRect r(0, y, width(), kMentionRow);
        if (i == m_selected)
        {
            painter.fillRect(r, t.raise);
            painter.fillRect(QRect(0, y, 2, kMentionRow), t.accent);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(presenceColor(row.state));
        painter.drawEllipse(QPointF(13.5, r.center().y() + 0.5), 3.5, 3.5);

        const QFont strong = LobbyTheme::uiFont(12, QFont::Bold);
        const QFont plain = LobbyTheme::uiFont(12);
        const QString head = row.name.left(m_typed);
        const QString rest = row.name.mid(m_typed);
        int x = 24;
        painter.setFont(strong);
        painter.setPen(t.ink);
        painter.drawText(QRect(x, y, 200, kMentionRow), Qt::AlignLeft | Qt::AlignVCenter, head);
        x += QFontMetrics(strong).horizontalAdvance(head);
        painter.setFont(plain);
        painter.setPen(i == m_selected ? t.ink : t.ink2);
        painter.drawText(QRect(x, y, 200, kMentionRow), Qt::AlignLeft | Qt::AlignVCenter, rest);
        x += QFontMetrics(plain).horizontalAdvance(rest);

        const QPixmap flag = LobbyTheme::flag(row.country, devicePixelRatioF());
        if (!flag.isNull())
            painter.drawPixmap(QPointF(x + 7, r.center().y() - 4.5), flag);

        painter.setFont(LobbyTheme::uiFont(10));
        painter.setPen(t.muted);
        painter.drawText(r.adjusted(0, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, row.note);
    }

    painter.fillRect(QRect(0, y, width(), 1), t.line);
    painter.setFont(LobbyTheme::uiFont(10));
    painter.setPen(t.muted);
    painter.drawText(QRect(10, y + 1, width() - 20, kMentionFooter - 1), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("↑↓ choose   Tab insert   Esc close"));

    painter.setClipping(false);
    painter.setPen(QPen(t.line2, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);
}

// ── PrivateMessagesPanel ─────────────────────────────────────────────

namespace
{
    // One conversation in the inbox.
    class InboxRow : public QFrame
    {
    public:
        InboxRow(const QString& name, const QString& country, const QString& preview,
                 const QString& time, int unread, bool gone, QWidget* parent)
            : QFrame(parent)
        {
            setObjectName("PmInboxRow");
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
            setFixedHeight(46);
            auto* lay = new QHBoxLayout(this);
            lay->setContentsMargins(12, 7, 12, 7);
            lay->setSpacing(9);
            auto* tile = new PlayerTile(30, this);
            tile->setPlayer(name);
            tile->setDimmed(gone);
            lay->addWidget(tile);

            auto* words = new QVBoxLayout;
            words->setSpacing(0);
            auto* top = new QHBoxLayout;
            top->setSpacing(5);
            auto* nameLabel = new ElidedLabel(gone ? Role::Faint : Role::Ink, this);
            nameLabel->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
            nameLabel->setText(name);
            top->addWidget(nameLabel);
            auto* flag = new CountryFlag(this);
            flag->setCountry(country);
            top->addWidget(flag);
            top->addStretch(1);
            auto* timeLabel = new QLabel(time, this);
            timeLabel->setFont(LobbyTheme::uiFont(10));
            timeLabel->setProperty("role", "muted");
            top->addWidget(timeLabel);
            words->addLayout(top);

            auto* bottom = new QHBoxLayout;
            bottom->setSpacing(6);
            auto* previewLabel = new ElidedLabel(gone ? Role::Faint : unread > 0 ? Role::Ink2 : Role::Muted, this);
            previewLabel->setFont(LobbyTheme::uiFont(11, unread > 0 ? QFont::DemiBold : QFont::Normal));
            previewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            previewLabel->setText(preview);
            bottom->addWidget(previewLabel, 1);
            if (unread > 0)
            {
                auto* badge = new QLabel(unread > 99 ? QStringLiteral("99+") : QString::number(unread), this);
                badge->setObjectName("ChatBadge");
                badge->setFont(LobbyTheme::uiFont(9, QFont::Bold));
                badge->setAlignment(Qt::AlignCenter);
                bottom->addWidget(badge);
            }
            words->addLayout(bottom);
            lay->addLayout(words, 1);
        }

        std::function<void()> onClick;

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && onClick)
                onClick();
        }

        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            const Tokens& t = LobbyTheme::tokens();
            if (underMouse())
                painter.fillRect(rect(), t.tint);
            painter.fillRect(QRect(0, 0, width(), 1), t.line);
        }
    };
} // namespace

PrivateMessagesPanel::PrivateMessagesPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_pages = new QStackedWidget(this);
    root->addWidget(m_pages);

    // ── Inbox ──
    auto* inbox = new QWidget(m_pages);
    auto* inboxLay = new QVBoxLayout(inbox);
    inboxLay->setContentsMargins(0, 0, 0, 0);
    inboxLay->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(12, 5, 8, 3);
    auto* title = new QLabel(tr("Private messages").toUpper(), inbox);
    QFont titleFont = LobbyTheme::uiFont(9, QFont::DemiBold);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
    title->setFont(titleFont);
    title->setProperty("role", "muted");
    heading->addWidget(title);
    heading->addStretch(1);
    auto* compose = new IconButton(QStringLiteral("edit-2-line"), 22, inbox);
    compose->setObjectName("PmNewMessage");
    compose->setToolTip(tr("New message"));
    compose->setAccessibleName(tr("New message"));
    connect(compose, &QToolButton::clicked, this, &PrivateMessagesPanel::newMessageRequested);
    heading->addWidget(compose);
    inboxLay->addLayout(heading);

    auto* scroll = new QScrollArea(inbox);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    m_inboxRows = new QWidget(scroll);
    m_inboxLayout = new QVBoxLayout(m_inboxRows);
    m_inboxLayout->setContentsMargins(0, 0, 0, 0);
    m_inboxLayout->setSpacing(0);
    m_inboxLayout->addStretch(1);
    scroll->setWidget(m_inboxRows);
    m_inboxRows->setAutoFillBackground(false); // setWidget turns it on
    inboxLay->addWidget(scroll, 1);
    m_inboxEmpty = new QLabel(tr("Nothing yet. Right-click a player to send them a message."), inbox);
    m_inboxEmpty->setFont(LobbyTheme::uiFont(12));
    m_inboxEmpty->setProperty("role", "muted");
    m_inboxEmpty->setWordWrap(true);
    m_inboxEmpty->setAlignment(Qt::AlignCenter);
    m_inboxEmpty->setContentsMargins(20, 0, 20, 30);
    inboxLay->addWidget(m_inboxEmpty, 1);
    m_pages->addWidget(inbox);

    // ── One thread ──
    auto* thread = new QWidget(m_pages);
    auto* threadLay = new QVBoxLayout(thread);
    threadLay->setContentsMargins(0, 0, 0, 0);
    threadLay->setSpacing(0);
    auto* header = new QFrame(thread);
    header->setObjectName("PmThreadHeader");
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(6, 6, 8, 6);
    headerLay->setSpacing(8);
    auto* back = new IconButton(QStringLiteral("arrow-left-s-line"), 22, header);
    back->setObjectName("PmBack");
    back->setToolTip(tr("All conversations"));
    back->setAccessibleName(tr("All conversations"));
    connect(back, &QToolButton::clicked, this, &PrivateMessagesPanel::showInbox);
    headerLay->addWidget(back);
    m_headerTile = new PlayerTile(26, header);
    headerLay->addWidget(m_headerTile);
    auto* who = new QVBoxLayout;
    who->setSpacing(1);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(5);
    m_headerName = new ElidedLabel(Role::Ink, header);
    m_headerName->setFont(LobbyTheme::uiFont(12, QFont::Bold));
    nameRow->addWidget(m_headerName);
    m_headerFlag = new CountryFlag(header);
    nameRow->addWidget(m_headerFlag);
    m_headerTo = new StatusTag(StatusTag::Kind::Neutral, header);
    nameRow->addWidget(m_headerTo);
    nameRow->addStretch(1);
    who->addLayout(nameRow);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(4);
    m_headerDot = new LedDot(header);
    statusRow->addWidget(m_headerDot);
    m_headerStatus = new ElidedLabel(Role::Muted, header);
    m_headerStatus->setFont(LobbyTheme::uiFont(10));
    m_headerStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    statusRow->addWidget(m_headerStatus, 1);
    who->addLayout(statusRow);
    headerLay->addLayout(who, 1);
    m_headerChallenge = new IconButton(QStringLiteral("sword-fill"), 24, header);
    m_headerChallenge->setObjectName("PmChallenge");
    m_headerChallenge->setToolTip(tr("Challenge"));
    m_headerChallenge->setAccessibleName(tr("Challenge"));
    connect(m_headerChallenge, &QToolButton::clicked, this, [this]() {
        if (m_openRemote != 0)
            emit challengeRequested(m_openRemote);
    });
    headerLay->addWidget(m_headerChallenge);
    m_headerMore = new IconButton(QStringLiteral("more-2-fill"), 24, header);
    m_headerMore->setObjectName("PmMore");
    m_headerMore->setToolTip(tr("More"));
    m_headerMore->setAccessibleName(tr("More"));
    connect(m_headerMore, &QToolButton::clicked, this, [this]() {
        if (m_openRemote != 0)
            emit menuRequested(m_openRemote, m_headerMore->mapToGlobal(QPoint(0, m_headerMore->height())));
    });
    headerLay->addWidget(m_headerMore);
    threadLay->addWidget(header);

    m_threadStack = new QStackedWidget(thread);
    auto* stackWrap = new QVBoxLayout;
    stackWrap->setContentsMargins(10, 6, 4, 2);
    stackWrap->addWidget(m_threadStack);
    threadLay->addLayout(stackWrap, 1);
    m_pages->addWidget(thread);

    rebuildInbox();
}

ChatView* PrivateMessagesPanel::threadView(quint64 localId, const QString& localName,
                                           quint64 remoteId, const QString& remoteName, bool localIsOwner)
{
    Thread& thread = m_threads[keyOf(localId, remoteId)];
    if (thread.view)
        return thread.view;

    thread.localId = localId;
    thread.localName = localName;
    thread.localIsOwner = localIsOwner;
    thread.remoteId = remoteId;
    thread.remoteName = remoteName;
    thread.view = new ChatView(m_threadStack);
    m_threadStack->addWidget(thread.view);
    connect(thread.view, &ChatView::userActivated, this, &PrivateMessagesPanel::userActivated);
    thread.view->addLine(QString("<i style='color:%1;'>%2</i>")
        .arg(LobbyTheme::tokens().muted.name(),
             tr("Only the two of you see this. It's gone when either of you leaves the lobby.")));
    rebuildInbox();
    return thread.view;
}

void PrivateMessagesPanel::noteMessage(quint64 localId, quint64 remoteId, const QString& preview, qint64 timeMs, bool incoming)
{
    const auto it = m_threads.find(keyOf(localId, remoteId));
    if (it == m_threads.end())
        return;
    it->preview = preview;
    it->lastMs = timeMs;
    const bool seen = m_active && m_openLocal == localId && m_openRemote == remoteId;
    if (incoming && !seen)
    {
        ++it->unread;
        emit unreadChanged();
    }
    rebuildInbox();
}

void PrivateMessagesPanel::openThread(quint64 localId, quint64 remoteId)
{
    const auto it = m_threads.find(keyOf(localId, remoteId));
    if (it == m_threads.end())
        return;
    m_openLocal = localId;
    m_openRemote = remoteId;
    m_threadStack->setCurrentWidget(it->view);
    m_pages->setCurrentIndex(1);
    if (m_active && it->unread > 0)
    {
        it->unread = 0;
        emit unreadChanged();
        rebuildInbox();
    }
    refreshHeader();
    emit currentChanged();
}

void PrivateMessagesPanel::showInbox()
{
    m_openLocal = 0;
    m_openRemote = 0;
    m_pages->setCurrentIndex(0);
    emit currentChanged();
}

void PrivateMessagesPanel::clear()
{
    for (const Thread& thread : m_threads)
    {
        if (thread.view)
            thread.view->deleteLater();
    }
    m_threads.clear();
    m_openLocal = 0;
    m_openRemote = 0;
    m_pages->setCurrentIndex(0);
    rebuildInbox();
    emit unreadChanged();
    emit currentChanged();
}

quint64 PrivateMessagesPanel::threadLocalFor(quint64 remoteId) const
{
    const Thread* latest = nullptr;
    for (const Thread& thread : m_threads)
    {
        if (thread.remoteId == remoteId && !thread.gone && (!latest || thread.lastMs > latest->lastMs))
            latest = &thread;
    }
    return latest ? latest->localId : 0;
}

ChatView* PrivateMessagesPanel::existingView(quint64 remoteId) const
{
    const quint64 local = threadLocalFor(remoteId);
    const auto it = m_threads.constFind(keyOf(local, remoteId));
    return local != 0 && it != m_threads.constEnd() ? it->view : nullptr;
}

void PrivateMessagesPanel::setActive(bool active)
{
    m_active = active;
    if (!active)
        return;
    if (Thread* thread = openThreadData(); thread && thread->unread > 0)
    {
        thread->unread = 0;
        emit unreadChanged();
        rebuildInbox();
    }
}

void PrivateMessagesPanel::setRemoteStatus(quint64 remoteId, const QString& state, const QString& statusText,
                                           const QString& country, bool canChallenge)
{
    bool changed = false;
    for (Thread& thread : m_threads)
    {
        if (thread.remoteId != remoteId || thread.gone)
            continue;
        thread.state = state;
        thread.statusText = statusText;
        thread.canChallenge = canChallenge;
        if (thread.remoteCountry != country)
        {
            thread.remoteCountry = country;
            changed = true;
        }
    }
    if (changed)
        rebuildInbox();
    if (m_openRemote == remoteId)
        refreshHeader();
}

void PrivateMessagesPanel::markGone(quint64 userId)
{
    bool found = false;
    for (Thread& thread : m_threads)
    {
        if ((thread.remoteId != userId && thread.localId != userId) || thread.gone)
            continue;
        thread.gone = true;
        thread.localGone = thread.localId == userId;
        found = true;
        thread.preview = thread.localGone ? tr("%1 left this PC").arg(thread.localName) : tr("Left the lobby");
        thread.view->addLine(QString("<i style='color:%1;'>%2</i>")
            .arg(LobbyTheme::tokens().muted.name(),
                 thread.localGone ? tr("%1 left this PC. Nobody can answer as them any more.").arg(thread.localName.toHtmlEscaped())
                                  : tr("%1 left the lobby. Messages can't reach them.").arg(thread.remoteName.toHtmlEscaped())));
    }
    if (!found)
        return;
    rebuildInbox();
    if (m_openRemote == userId || m_openLocal == userId)
    {
        refreshHeader();
        emit currentChanged();
    }
}

PrivateMessagesPanel::Thread* PrivateMessagesPanel::openThreadData()
{
    const auto it = m_threads.find(keyOf(m_openLocal, m_openRemote));
    return m_openLocal != 0 && it != m_threads.end() ? &it.value() : nullptr;
}

const PrivateMessagesPanel::Thread* PrivateMessagesPanel::openThreadData() const
{
    const auto it = m_threads.constFind(keyOf(m_openLocal, m_openRemote));
    return m_openLocal != 0 && it != m_threads.constEnd() ? &it.value() : nullptr;
}

QString PrivateMessagesPanel::openRemoteName() const
{
    const Thread* thread = openThreadData();
    return thread ? thread->remoteName : QString();
}

QString PrivateMessagesPanel::openLocalName() const
{
    const Thread* thread = openThreadData();
    return thread ? thread->localName : QString();
}

bool PrivateMessagesPanel::openLocalIsOwner() const
{
    const Thread* thread = openThreadData();
    return !thread || thread->localIsOwner;
}

bool PrivateMessagesPanel::openRemoteGone() const
{
    const Thread* thread = openThreadData();
    return thread && thread->gone;
}

bool PrivateMessagesPanel::openLocalGone() const
{
    const Thread* thread = openThreadData();
    return thread && thread->localGone;
}

int PrivateMessagesPanel::unreadTotal() const
{
    int total = 0;
    for (const Thread& thread : m_threads)
        total += thread.unread;
    return total;
}

QList<ChatView*> PrivateMessagesPanel::views() const
{
    QList<ChatView*> out;
    for (const Thread& thread : m_threads)
    {
        if (thread.view)
            out << thread.view;
    }
    return out;
}

void PrivateMessagesPanel::rebuildInbox()
{
    while (m_inboxLayout->count() > 1)
    {
        QLayoutItem* item = m_inboxLayout->takeAt(0);
        if (QWidget* widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    QList<const Thread*> sorted;
    for (const Thread& thread : m_threads)
        sorted << &thread;
    std::sort(sorted.begin(), sorted.end(), [](const Thread* a, const Thread* b) { return a->lastMs > b->lastMs; });

    int position = 0;
    for (const Thread* thread : sorted)
    {
        const QString time = thread->lastMs > 0
            ? QDateTime::fromMSecsSinceEpoch(thread->lastMs).toString("hh:mm") : QString();
        auto* row = new InboxRow(thread->remoteName, thread->remoteCountry, thread->preview, time,
                                 thread->unread, thread->gone, m_inboxRows);
        row->onClick = [this, local = thread->localId, remote = thread->remoteId]() {
            // Later: the click is inside a row this rebuild would delete.
            QTimer::singleShot(0, this, [this, local, remote]() { openThread(local, remote); });
        };
        m_inboxLayout->insertWidget(position++, row);
    }
    m_inboxEmpty->setVisible(sorted.isEmpty());
    m_inboxRows->parentWidget()->parentWidget()->setVisible(!sorted.isEmpty());
}

void PrivateMessagesPanel::refreshHeader()
{
    const Thread* thread = openThreadData();
    if (!thread)
        return;
    const Tokens& t = LobbyTheme::tokens();
    m_headerTile->setPlayer(thread->remoteName);
    m_headerTile->setDimmed(thread->gone);
    m_headerName->setText(thread->remoteName);
    m_headerFlag->setCountry(thread->remoteCountry);
    m_headerTo->setVisible(!thread->localIsOwner);
    if (!thread->localIsOwner)
        m_headerTo->setTag(StatusTag::Kind::Neutral, tr("To %1").arg(thread->localName));
    m_headerStatus->setText(thread->localGone ? tr("%1 left this PC").arg(thread->localName)
                            : thread->gone ? tr("Left the lobby") : thread->statusText);
    m_headerDot->setColor(thread->gone ? t.faint : presenceColor(thread->state));
    // A challenge goes out as this PC's owner, so answering as someone else it would
    // come from a name the other side wasn't talking to.
    m_headerChallenge->setVisible(!thread->gone && thread->canChallenge && thread->localIsOwner);
    m_headerMore->setVisible(!thread->gone);
}

#endif // NETPLAY
