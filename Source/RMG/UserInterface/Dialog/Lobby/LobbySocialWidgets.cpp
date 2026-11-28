#ifdef NETPLAY

#include "LobbySocialWidgets.hpp"

#include <QAction>
#include <QActionGroup>
#include <QChildEvent>
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
#include <QScrollBar>
#include <QStyleOptionTab>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
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
            const QFont font = LobbyTheme::pixelFont(9, 0.9);
            painter.setFont(font);
            painter.setPen(color);
            painter.drawText(QRectF(x, 11 + LobbyTheme::pixelTextDrop(font), width() - x - 8, 15),
                             Qt::AlignLeft | Qt::AlignVCenter, m_text);
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

namespace
{
    const char* const kTabFaceProperty = "chatTabFace";
    const char* const kTabAttentionProperty = "chatTabAttention";
    // Shorter than the tab, so it never makes the tab taller.
    constexpr int kTabFaceHeight = 22;
} // namespace

ChatTabBar::ChatTabBar(QWidget* parent)
    : QTabBar(parent)
{
    connect(this, &QTabBar::currentChanged, this, &ChatTabBar::positionBadges);

    // QTabBar's own scroll buttons, drawn to match the lobby instead of the platform.
    for (const char* name : { "ScrollLeftButton", "ScrollRightButton" })
    {
        if (auto* button = findChild<QToolButton*>(QLatin1String(name), Qt::FindDirectChildrenOnly))
            m_scrollButtons << button;
    }
    for (QToolButton* button : std::as_const(m_scrollButtons))
    {
        button->installEventFilter(this);
        button->setAttribute(Qt::WA_Hover);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QToolButton::clicked, this, &ChatTabBar::positionBadges);
    }
}

void ChatTabBar::showTab(int index, bool visible)
{
    if (isTabVisible(index) != visible)
        setTabVisible(index, visible);
}

void ChatTabBar::setTabFace(int index, QWidget* face)
{
    face->setProperty(kTabFaceProperty, true);
    // Until it's placed: QTabBar counts a button's current size into the tab's height.
    face->resize(face->sizeHint().width(), kTabFaceHeight);
    setTabButton(index, QTabBar::LeftSide, face);
}

QWidget* ChatTabBar::tabFace(int index) const
{
    QWidget* face = tabButton(index, QTabBar::LeftSide);
    return face && face->property(kTabFaceProperty).toBool() ? face : nullptr;
}

void ChatTabBar::setTabAttention(int index, bool attention)
{
    if (QWidget* face = tabFace(index))
        face->setProperty(kTabAttentionProperty, attention);
    for (QToolButton* button : std::as_const(m_scrollButtons))
        button->update();
}

void ChatTabBar::refreshTab(int index)
{
    // The same text, but QTabBar lays every tab out again for it.
    setTabText(index, tabText(index));
}

QSize ChatTabBar::tabSizeHint(int index) const
{
    QSize size = QTabBar::tabSizeHint(index);
    if (const QWidget* face = tabFace(index))
    {
        size.setWidth(face->sizeHint().width() + 2);
        return size;
    }
    if (tabText(index).isEmpty())
    {
        const QWidget* badge = tabButton(index, QTabBar::RightSide);
        size.setWidth(38 + (badge ? badge->width() + 2 : 0));
    }
    return size;
}

QSize ChatTabBar::minimumTabSizeHint(int index) const
{
    // Only tabs with a face give way. The others keep their size, and the bar scrolls.
    if (const QWidget* face = tabFace(index))
        return QSize(face->minimumSizeHint().width() + 2, tabSizeHint(index).height());
    return tabSizeHint(index);
}

void ChatTabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    positionBadges();
}

void ChatTabBar::childEvent(QChildEvent* event)
{
    QTabBar::childEvent(event);
    // Faces and badges. QTabBar moves them each time it lays out or scrolls the
    // tabs, so each move gets corrected.
    if (event->added() && event->child()->isWidgetType())
        event->child()->installEventFilter(this);
}

bool ChatTabBar::eventFilter(QObject* watched, QEvent* event)
{
    for (QToolButton* button : std::as_const(m_scrollButtons))
    {
        if (watched == button)
        {
            if (event->type() == QEvent::Paint)
            {
                paintScrollButton(button);
                return true;
            }
            return QTabBar::eventFilter(watched, event);
        }
    }
    if (event->type() == QEvent::Move && watched->parent() == this)
    {
        for (int index = 0; index < count(); ++index)
        {
            if (watched == tabButton(index, QTabBar::LeftSide) || watched == tabButton(index, QTabBar::RightSide))
            {
                placeTabButtons(index);
                break;
            }
        }
    }
    return QTabBar::eventFilter(watched, event);
}

void ChatTabBar::mousePressEvent(QMouseEvent* event)
{
    // Taken, so the release comes here too.
    if (event->button() == Qt::MiddleButton && tabFace(tabAt(event->position().toPoint())))
    {
        event->accept();
        return;
    }
    QTabBar::mousePressEvent(event);
}

void ChatTabBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton)
    {
        const int index = tabAt(event->position().toPoint());
        if (tabFace(index))
        {
            emit tabCloseRequested(index);
            return;
        }
    }
    QTabBar::mouseReleaseEvent(event);
}

void ChatTabBar::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = tabAt(event->pos());
    if (index >= 0)
    {
        emit tabMenuRequested(index, event->globalPos());
        return;
    }
    QTabBar::contextMenuEvent(event);
}

void ChatTabBar::positionBadges()
{
    for (int index = 0; index < count(); ++index)
        placeTabButtons(index);
    // Any layout or scroll can move a tab with a badge into view or out of it.
    for (QToolButton* button : std::as_const(m_scrollButtons))
        button->update();
}

void ChatTabBar::placeTabButtons(int index)
{
    if (!isTabVisible(index))
        return;
    if (QWidget* face = tabFace(index))
    {
        // Across the whole tab, inside its border.
        const QRect tab = tabRect(index).adjusted(1, 0, -1, 0);
        const QRect rect(tab.left(), tab.top() + (tab.height() - kTabFaceHeight + 1) / 2, tab.width(), kTabFaceHeight);
        if (face->geometry() != rect)
            face->setGeometry(rect);
    }

    constexpr int gap = 5;
    QWidget* badge = tabButton(index, QTabBar::RightSide);
    if (!badge || tabText(index).isEmpty())
        return;

    QStyleOptionTab option;
    initStyleOption(&option, index);
    const QRect button = style()->subElementRect(QStyle::SE_TabBarTabRightButton, &option, this);
    const QRect text = style()->subElementRect(QStyle::SE_TabBarTabText, &option, this);
    const QPoint pos(std::min(button.left(), text.right() + 1 + gap), button.top());
    if (text.isValid() && badge->pos() != pos)
        badge->move(pos);
}

QRect ChatTabBar::tabArea() const
{
    // The style puts the scroll buttons at either end or both at one end.
    QRect area = rect();
    for (const QToolButton* button : m_scrollButtons)
    {
        if (!button->isVisible())
            continue;
        if (button->geometry().center().x() < width() / 2)
            area.setLeft(std::max(area.left(), button->geometry().right() + 1));
        else
            area.setRight(std::min(area.right(), button->x() - 1));
    }
    return area;
}

ChatTabBar::HiddenUnread ChatTabBar::unreadOutOfView(bool left) const
{
    const QRect area = tabArea();
    HiddenUnread found = HiddenUnread::None;
    for (int index = 0; index < count(); ++index)
    {
        if (!isTabVisible(index))
            continue;
        const QWidget* face = tabFace(index);
        const QWidget* badge = tabButton(index, QTabBar::RightSide);
        if (face ? !face->property(kTabAttentionProperty).toBool() : !badge)
            continue;
        const QRect rect = tabRect(index);
        if (!(left ? rect.left() < area.left() : rect.right() > area.right()))
            continue;
        // A fixed tab's badge is amber for a mention; the dot matches it.
        if (face || !badge->property("mention").toBool())
            return HiddenUnread::Message;
        found = HiddenUnread::Mention;
    }
    return found;
}

void ChatTabBar::paintScrollButton(QToolButton* button)
{
    QPainter painter(button);
    painter.setRenderHint(QPainter::Antialiasing);
    const Tokens& t = LobbyTheme::tokens();
    const bool left = button == m_scrollButtons.value(0);
    // Opaque: the tabs scroll underneath. A rule on the side the tabs are.
    painter.fillRect(button->rect(), t.panel);
    const QRect area = tabArea();
    if (button->geometry().right() < area.left())
        painter.fillRect(QRect(button->width() - 1, 5, 1, button->height() - 10), t.line);
    else
        painter.fillRect(QRect(0, 5, 1, button->height() - 10), t.line);
    const bool hover = button->isEnabled() && button->underMouse();
    if (hover || button->isDown())
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(t.raise);
        painter.drawRoundedRect(QRectF(button->rect()).adjusted(2.5, 4, -2.5, -4), 5, 5);
    }
    const QColor color = !button->isEnabled() ? t.faint : hover ? t.ink : t.muted;
    constexpr int side = 16;
    const QPixmap arrow = LobbyTheme::icon(left ? QStringLiteral("arrow-left-s-line") : QStringLiteral("arrow-right-s-line"),
                                           color, side, button->devicePixelRatioF());
    painter.drawPixmap(QRect((button->width() - side) / 2, (button->height() - side) / 2, side, side), arrow);
    // Something unread that way: a dot on the arrow's outer corner.
    const HiddenUnread unread = unreadOutOfView(left);
    if (unread != HiddenUnread::None)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(unread == HiddenUnread::Mention ? t.warn : t.liveBg);
        painter.drawEllipse(QPointF(left ? 5.5 : button->width() - 5.5, 7.5), 3.0, 3.0);
    }
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
    painter.drawText(QRectF(rect()).translated(0, LobbyTheme::pixelTextDrop(painter.font())), Qt::AlignCenter, LobbyTheme::nameInitial(m_name));
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

#endif // NETPLAY
