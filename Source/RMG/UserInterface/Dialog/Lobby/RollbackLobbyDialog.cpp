/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 *
 * Visual approach: the lobby draws its own theme, console charcoal when the
 * application palette is dark and Famicom cream when it's light. The colours
 * live in LobbyTheme.hpp; the painted pieces in LobbyWidgets.hpp and
 * LobbyBrowsePanels.hpp. Running matches get the biggest part of the window,
 * because people come here to find something to watch.
 *
 *   ┌───────────────────────────────────────────────────────────────┐
 *   │ Dale64  ● ONLINE  42 players · 14 rooms             Mira [US] │ ← top bar
 *   ├───────────────────────────────────────────┬───────────────────┤
 *   │ [ game picker ] [Quick Match] [Create]    │ Players           │ ← play bar
 *   │ ┌Live now───────────┐┌Open rooms────────┐ │ ● searching...    │
 *   │ │ match             ││ room             │ │ ● playing...      │
 *   │ │ match             ││ room             │ ├───────────────────┤
 *   │ │ match             ││ room             │ │ Lobby | Room      │
 *   │ │                   ││                  │ │ chat...           │
 *   │ └───────────────────┘└──────────────────┘ │ [message]         │
 *   └───────────────────────────────────────────┴───────────────────┘
 *
 * A room opens as a window over the stage, with the lobby dimmed behind it.
 * It can be moved, resized, maximized to fill the stage, or minimized to the
 * play bar while you keep your seat. The sidebar stays put, so chat never moves.
 */
#ifdef NETPLAY

#include "RollbackLobbyDialog.hpp"
#include "LobbyBrowsePanels.hpp"
#include "LobbyConnectDialog.hpp"
#include "LobbyTheme.hpp"
#include "LobbyWidgets.hpp"
#include "CreateRoomDialog.hpp"
#include "LagStatsDialog.hpp"
#include "LobbyRegions.hpp"
#include "LobbyRoomWindow.hpp"
#include "LobbySocialDialogs.hpp"
#include "LobbySocialWidgets.hpp"
#include "NetplaySaveDialogs.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/NetplaySave.hpp>
#include <RMG-Core/GameOptions.hpp>
#include <RMG-Core/rmgk_gekko.hpp> // request_disconnect_player (instant peer drop)

#include "n02_client.h" // setRecordingStreamSink (broadcast tee)
#include "Utilities/Translation.hpp"

#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QKeyEvent>
#include <QSignalBlocker>

#include <QApplication>
#include <QCoreApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QCursor>
#include <QStyle>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QTextDocument>
#include <QLineEdit>
#include <QJsonArray>
#include <QPushButton>
#include <QLabel>
#include <QLinearGradient>
#include <QFrame>
#include <QComboBox>
#include <QListView>
#include <QScreen>
#include <QSortFilterProxyModel>
#include <QCheckBox>
#include <QActionGroup>
#include <QButtonGroup>
#include <QGridLayout>
#include <QSettings>
#include <QMessageBox>
#include <QInputDialog>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QPixmap>
#include <QTimer>
#include <QFont>
#include <QUrl>
#include <QDebug>
#include <QVariant>
#include <QIcon>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QLocale>
#include <QMenu>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstring>

using namespace UserInterface::Dialog;
using LobbyTheme::Role;
using LobbyTheme::Tokens;

// Recording globals live in the n02 static library (exported from RMG-Core).
// Declared at global scope on purpose: a block-scope `extern` inside a
// RollbackLobbyDialog member binds to a phantom UserInterface::Dialog::<name>
// and fails to link, so these mirror p2p_core.cpp's file-scope declarations.
extern bool n02_kaillera_recording_enabled;
extern char recording_player_names[4][32];

namespace {
// Auto input-delay buckets by ping, migrated from the P2P rollback tab.
// Ping < 0 (unknown) falls back to the default. Automatic selection remains at
// least one frame; zero delay is an explicit low-latency choice.
int autoFrameDelayForPing(int ping)
{
    if (ping < 0)    return 2;   // default when ping is unknown
    if (ping <= 50)  return 1;
    if (ping <= 100) return 2;
    if (ping <= 150) return 3;
    if (ping <= 220) return 4;
    return 5;
}

// The host-controlled room prediction defaults to the earlier configurable
// lobby behavior and Slippi's traditional default.
constexpr int kDefaultPredictionWindow = 7;

} // namespace

// ──────────────────────────────────────────────────────────────────────
// Standardized sizes — keep all paddings/heights/font deltas going through
// these so the dialog visually balances and any future tweak only happens
// in one place.
// ──────────────────────────────────────────────────────────────────────
namespace
{
    const char* const CHANNEL_LOBBY = "lobby";
    const char* const CHANNEL_ROOM  = "room";
    // Viewers of one live replay; the server never relays it to the players.
    const char* const CHANNEL_SPECTATE = "spectate";

    constexpr int MARGIN_STAGE       = 14;  // around the stage and the sidebar
    constexpr int MARGIN_STAGE_INNER = 8;   // the stage's edge beside the splitter handle
    constexpr int GAP_STAGE          = 12;  // between the stage's cards
    constexpr int SPLITTER_MAIN      = 6;   // handle, plus MARGIN_STAGE_INNER, makes the gap

    constexpr int MARQUEE_HEIGHT     = 48;
    constexpr int PLAYBAR_HEIGHT     = 62;  // one height for every state, so nothing below it moves
    constexpr int SIDEBAR_WIDTH      = 300;
    constexpr int PLAYERS_HEIGHT     = 256;

    bool isDarkTheme()
    {
        return LobbyTheme::isDark();
    }

    // The account menu's look choices: "app", "dark" or "light"; "side" or "stacked".
    const char* const kLobbyThemeSetting = "RollbackLobby/Theme";
    const char* const kBrowseLayoutSetting = "RollbackLobby/BrowseLayout";

    // Follows the pointer for a seat drag, installed on the application so it
    // sees every mouse event: a room update can hide the handle that took the
    // press, and the release then lands somewhere else.
    class SeatDragTracker : public QObject
    {
    public:
        SeatDragTracker(std::function<void(const QPoint&)> moved, std::function<void(const QPoint&, bool)> ended,
                        QObject* parent)
            : QObject(parent), m_moved(std::move(moved)), m_ended(std::move(ended))
        {
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::ApplicationDeactivate)
            {
                m_ended(QPoint(), false);
                return false;
            }
            if (!watched->isWidgetType())
                return false;
            switch (event->type())
            {
            case QEvent::MouseMove:
                m_moved(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
                return true;
            case QEvent::MouseButtonRelease:
                m_ended(static_cast<QMouseEvent*>(event)->globalPosition().toPoint(), true);
                return true;
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
                return true;
            case QEvent::KeyPress:
                if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
                {
                    m_ended(QPoint(), false);
                    return true;
                }
                return false;
            default:
                return false;
            }
        }

    private:
        std::function<void(const QPoint&)> m_moved;
        std::function<void(const QPoint&, bool)> m_ended;
    };

    struct StatusColors { QString ok; QString wait; QString fail; QString idle; };
    StatusColors statusColors()
    {
        const Tokens& t = LobbyTheme::tokens();
        return StatusColors{ t.ok.name(), t.warn.name(), t.bad.name(), t.muted.name() };
    }

    QString humanState(LobbyClient::ConnectionState s)
    {
        switch (s)
        {
            case LobbyClient::ConnectionState::Disconnected:   return QCoreApplication::translate("RollbackLobbyDialog", "Offline");
            case LobbyClient::ConnectionState::Connecting:     return QCoreApplication::translate("RollbackLobbyDialog", "Connecting");
            case LobbyClient::ConnectionState::Authenticating: return QCoreApplication::translate("RollbackLobbyDialog", "Authenticating");
            case LobbyClient::ConnectionState::Connected:      return QCoreApplication::translate("RollbackLobbyDialog", "Online");
            case LobbyClient::ConnectionState::Failed:         return QCoreApplication::translate("RollbackLobbyDialog", "Connection failed");
        }
        return QCoreApplication::translate("RollbackLobbyDialog", "Unknown");
    }

    QString statusColor(LobbyClient::ConnectionState s)
    {
        const auto c = statusColors();
        switch (s)
        {
            case LobbyClient::ConnectionState::Connected:      return c.ok;
            case LobbyClient::ConnectionState::Connecting:
            case LobbyClient::ConnectionState::Authenticating: return c.wait;
            case LobbyClient::ConnectionState::Failed:         return c.fail;
            default:                                           return c.idle;
        }
    }

    QString formatMatchDuration(qint64 startedAtMs)
    {
        if (startedAtMs <= 0)
            return QStringLiteral("—");

        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - startedAtMs;
        const qint64 totalSecs = elapsedMs > 0 ? elapsedMs / 1000 : 0;
        const qint64 h = totalSecs / 3600;
        const qint64 m = (totalSecs % 3600) / 60;
        const qint64 s = totalSecs % 60;
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
    }

    // Wi-Fi, or anything else wireless, as far as a room's limits go.
    bool isWireless(const QString& connection)
    {
        return connection == QLatin1String("wifi") || connection == QLatin1String("cellular");
    }

    bool liveReplayPreference()
    {
        QSettings settings("RMG-K", "n02");
        return settings.value("RollbackLobby/LiveReplayEnabled", true).toBool();
    }

    // Friendly label for an authoritative *room* state. Distinct from
    // stateGlyph(), which maps per-user presence states ("playing", "in_room",
    // …); the room state machine uses a different vocabulary ("waiting",
    // "starting", "in_game", "finished") that stateGlyph would pass through raw.
    QString roomStateLabel(const QString& state)
    {
        if (state == "waiting")  return QCoreApplication::translate("RollbackLobbyDialog", "Waiting");
        if (state == "starting") return QCoreApplication::translate("RollbackLobbyDialog", "Starting");
        if (state == "in_game")  return QCoreApplication::translate("RollbackLobbyDialog", "In Game");
        if (state == "finished") return QCoreApplication::translate("RollbackLobbyDialog", "Finished");
        return state;
    }

    // Color for a presence/room state string (the values stateGlyph maps).
    QString stateHex(const QString& state, bool /*dark*/)
    {
        const Tokens& t = LobbyTheme::tokens();
        if (state == "idle" || state == "waiting")          return t.ok.name();
        if (state == "hosting" || state == "in_room")       return t.stateRoom.name();
        if (state == "searching" || state == "starting" ||
            state == "connecting")                          return t.warn.name();
        if (state == "playing" || state == "in_game")       return t.live.name();
        if (state == "spectating")                          return t.stateWatch.name();
        return t.muted.name();
    }

    // Roles backing the painted players list (see PlayersItemDelegate).
    // Qt::UserRole itself carries the user id.
    constexpr int kGroupRole          = Qt::UserRole + 1; // this row heads a group
    constexpr int kGroupCountRole     = Qt::UserRole + 2;
    constexpr int kSelfRole           = Qt::UserRole + 3;
    constexpr int kStateRole          = Qt::UserRole + 4;
    constexpr int kCountryRole        = Qt::UserRole + 5;
    constexpr int kPingMsRole         = Qt::UserRole + 6; // int; -1 = unknown
    constexpr int kPingEstimateRole   = Qt::UserRole + 7; // value is a region estimate
    constexpr int kConnectionTypeRole = Qt::UserRole + 8; // "wifi", "lan", ...
    constexpr int kSocialRole         = Qt::UserRole + 9; // can be challenged and messaged
    constexpr int kRegionRole         = Qt::UserRole + 10; // the lobby's region bucket
    constexpr int kNoChallengesRole   = Qt::UserRole + 11; // their PC turned challenges off

    QString countryTooltip(const QString& country)
    {
        if (country.isEmpty())
            return QString();
        const QLocale::Territory territory = QLocale::codeToTerritory(country);
        const QString countryName = territory != QLocale::AnyTerritory
            ? QLocale::territoryToString(territory)
            : country;
        return QString(QCoreApplication::translate("RollbackLobbyDialog", "Country: %1")).arg(countryName);
    }

    // Paints the players list: group headings, and for each player a state
    // dot, name, flag, signal bars and connection symbol on one line. Hovering a
    // row swaps the bars and symbol for Challenge and Message buttons, since a
    // right-click menu is easy to never find.
    class PlayersItemDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        std::function<void(quint64)> onChallenge;
        std::function<void(quint64)> onMessage;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            return QSize(option.rect.width(), index.data(kGroupRole).toBool() ? 22 : 24);
        }

    protected:
        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
        {
            const Tokens& t = LobbyTheme::tokens();
            const QRect r = option.rect;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            if (index.data(kGroupRole).toBool())
            {
                QFont font = LobbyTheme::uiFont(9, QFont::DemiBold);
                font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
                painter->setFont(font);
                painter->setPen(t.muted);
                const QString title = index.data(Qt::DisplayRole).toString().toUpper();
                QRect area = r.adjusted(7, 5, -7, 0);
                painter->drawText(area, Qt::AlignLeft | Qt::AlignVCenter, title);
                area.setLeft(area.left() + QFontMetrics(font).horizontalAdvance(title) + 5);
                painter->drawText(area, Qt::AlignLeft | Qt::AlignVCenter,
                                  QString::number(index.data(kGroupCountRole).toInt()));
                painter->restore();
                return;
            }

            const bool isSelf = index.data(kSelfRole).toBool();
            const QRectF box = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
            if (!isSelf && (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)))
            {
                painter->setPen(Qt::NoPen);
                painter->setBrush(option.state & QStyle::State_Selected ? t.raise : t.tint);
                painter->drawRoundedRect(box, 6, 6);
            }

            const bool quickButtons = showsQuickButtons(option, index);
            const qreal centerY = box.center().y();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(stateHex(index.data(kStateRole).toString(), t.dark)));
            painter->drawEllipse(QPointF(r.left() + 10.5, centerY), 3.5, 3.5);

            // Right side, from the edge inward: connection symbol, bars.
            int right = r.right() - 6;
            const QString connection = index.data(kConnectionTypeRole).toString();
            if (quickButtons)
            {
                const QPoint cursor = option.widget
                    ? static_cast<const QAbstractItemView*>(option.widget)->viewport()->mapFromGlobal(QCursor::pos())
                    : QPoint();
                const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
                const auto paintButton = [&](const QRect& button, const QString& icon, bool enabled) {
                    const bool hot = enabled && button.contains(cursor);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(hot ? t.accent : t.tint);
                    painter->drawRoundedRect(QRectF(button), 5, 5);
                    painter->drawPixmap(button.center().x() - 6, button.center().y() - 6,
                                        LobbyTheme::icon(icon, hot ? QColor(Qt::white) : enabled ? t.ink2 : t.faint, 13, dpr));
                };
                paintButton(challengeRect(r), QStringLiteral("sword-fill"), !index.data(kNoChallengesRole).toBool());
                paintButton(messageRect(r), QStringLiteral("mail-line"), true);
                right = challengeRect(r).left() - 5;
            }
            else if (!connection.isEmpty())
                ConnectionGlyph::paintSymbol(*painter, QRectF(right - 15, r.top(), 16, r.height()), connection, t.muted);
            if (!quickButtons)
                right -= 16 + 5;
            if (!isSelf && !quickButtons)
            {
                LobbyTheme::paintPingBars(*painter, QPointF(right - 13, centerY + 6),
                                          index.data(kPingMsRole).toInt(),
                                          index.data(kPingEstimateRole).toBool());
                right -= 13 + 7;
            }

            const qreal ratio = option.widget ? option.widget->devicePixelRatioF() : 1.0;
            const QPixmap flag = LobbyTheme::flag(index.data(kCountryRole).toString(), ratio);
            const QSize flagSize = flag.deviceIndependentSize().toSize();
            const int flagRoom = flag.isNull() ? 0 : flagSize.width() + 5;

            const int nameLeft = r.left() + 21;
            QFont nameFont = LobbyTheme::uiFont(12, isSelf ? QFont::DemiBold : QFont::Normal);
            const QFontMetrics fm(nameFont);
            const QString you = isSelf ? QStringLiteral(" ") + QCoreApplication::translate("RollbackLobbyDialog", "(you)") : QString();
            const QFont youFont = LobbyTheme::uiFont(11);
            const int youWidth = you.isEmpty() ? 0 : QFontMetrics(youFont).horizontalAdvance(you);
            const int nameRoom = std::max(0, right - nameLeft - flagRoom - youWidth);
            const QString name = fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameRoom);
            const int nameWidth = fm.horizontalAdvance(name);

            painter->setFont(nameFont);
            painter->setPen(t.ink);
            painter->drawText(QRect(nameLeft, r.top(), nameWidth + 1, r.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
            int x = nameLeft + nameWidth;
            if (!you.isEmpty())
            {
                painter->setFont(youFont);
                painter->setPen(t.muted);
                painter->drawText(QRect(x, r.top(), youWidth + 1, r.height()), Qt::AlignLeft | Qt::AlignVCenter, you);
                x += youWidth;
            }
            if (!flag.isNull())
                painter->drawPixmap(QPointF(x + 5, centerY - flagSize.height() / 2.0), flag);

            painter->restore();
        }

        bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                         const QModelIndex& index) override
        {
            const auto type = event->type();
            if (index.data(kSocialRole).toBool() &&
                (type == QEvent::MouseMove || type == QEvent::MouseButtonPress ||
                 type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick))
            {
                auto* mouse = static_cast<QMouseEvent*>(event);
                const QPoint pos = mouse->position().toPoint();
                const bool onChallengeButton = challengeRect(option.rect).contains(pos);
                const bool onMessageButton = messageRect(option.rect).contains(pos);
                if (type == QEvent::MouseMove)
                {
                    if (option.widget)
                        static_cast<const QAbstractItemView*>(option.widget)->viewport()->update(option.rect);
                }
                else if (onChallengeButton || onMessageButton)
                {
                    if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton)
                    {
                        const quint64 userId = index.data(Qt::UserRole).toULongLong();
                        // Shown, but greyed out: they turned challenges off.
                        if (onChallengeButton && onChallenge && !index.data(kNoChallengesRole).toBool())
                            onChallenge(userId);
                        else if (onMessageButton && onMessage)
                            onMessage(userId);
                    }
                    return true;
                }
            }
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

    private:
        static QRect messageRect(const QRect& row)
        {
            return QRect(row.right() - 3 - 22, row.top() + 2, 22, row.height() - 4);
        }

        static QRect challengeRect(const QRect& row)
        {
            return messageRect(row).translated(-24, 0);
        }

        static bool showsQuickButtons(const QStyleOptionViewItem& option, const QModelIndex& index)
        {
            return (option.state & QStyle::State_MouseOver) && index.data(kSocialRole).toBool();
        }
    };

    constexpr int kBrowseGameSearchRole = Qt::UserRole + 1;

    QString browseGameSearchText(QString text)
    {
        text.removeIf([](QChar ch) { return ch.isSpace(); });
        return text;
    }

    class BrowseGameCombo : public QComboBox
    {
    public:
        BrowseGameCombo(const QString& searchPlaceholder, const QString& emptyText, QWidget* parent)
            : QComboBox(parent)
        {
            setCursor(Qt::PointingHandCursor);
            m_popup = new LobbyCard(this);
            m_popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            m_popup->setObjectName("BrowseGamePopup");
            m_popup->setAttribute(Qt::WA_TranslucentBackground);
            m_popup->setFont(LobbyTheme::uiFont(12));
            auto* layout = new QVBoxLayout(m_popup);
            layout->setContentsMargins(7, 7, 7, 7);
            layout->setSpacing(6);

            m_search = new QLineEdit(m_popup);
            m_search->setPlaceholderText(searchPlaceholder);
            m_search->setAccessibleName(searchPlaceholder);
            m_search->setClearButtonEnabled(true);
            m_search->setMinimumHeight(32);
            m_search->installEventFilter(this);
            layout->addWidget(m_search);

            m_filter = new QSortFilterProxyModel(m_popup);
            m_filter->setSourceModel(model());
            m_filter->setFilterRole(kBrowseGameSearchRole);
            m_filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
            m_results = new QListView(m_popup);
            m_results->setModel(m_filter);
            m_results->setFont(LobbyTheme::uiFont(13, QFont::DemiBold));
            m_results->setFocusPolicy(Qt::NoFocus);
            m_results->setMouseTracking(true);
            m_results->viewport()->setCursor(Qt::PointingHandCursor);
            m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            m_results->setUniformItemSizes(true);
            m_empty = new QLabel(emptyText, m_popup);
            m_empty->setAlignment(Qt::AlignCenter);
            m_empty->setProperty("role", "muted");
            m_pages = new QStackedWidget(m_popup);
            m_pages->addWidget(m_results);
            m_pages->addWidget(m_empty);
            layout->addWidget(m_pages, 1);

            connect(m_results, &QListView::clicked, this, &BrowseGameCombo::choose);
            connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
                m_filter->setFilterFixedString(browseGameSearchText(text));
                selectResult();
            });
            m_popup->installEventFilter(this);
        }

        void showPopup() override
        {
            if (!isEnabled() || count() == 0 || m_popup->isVisible())
                return;

            m_search->clear();
            selectResult();

            const QRect available = screen()->availableGeometry();
            const QPoint anchor = mapToGlobal(QPoint(0, 0));
            const int popupWidth = std::min(width(), available.width());
            const QLayout* layout = m_popup->layout();
            const QMargins margins = layout->contentsMargins();
            const int rowHeight = std::max(m_results->sizeHintForRow(0), m_results->fontMetrics().height());
            const int desiredHeight = margins.top() + margins.bottom() + layout->spacing()
                + m_search->sizeHint().expandedTo(m_search->minimumSize()).height()
                + rowHeight * std::clamp(count(), 2, 10);
            const int popupHeight = std::min(desiredHeight, available.height());
            constexpr int gap = 4;
            const int below = anchor.y() + height() + gap;
            const int y = below + popupHeight <= available.bottom() + 1
                ? below : anchor.y() - popupHeight - gap;
            m_popup->setGeometry(std::clamp(anchor.x(), available.left(), available.right() + 1 - popupWidth),
                                 std::clamp(y, available.top(), available.bottom() + 1 - popupHeight),
                                 popupWidth, popupHeight);
            m_popup->show();
            m_results->scrollTo(m_results->currentIndex());
            m_search->setFocus(Qt::PopupFocusReason);
        }

        void hidePopup() override
        {
            if (m_popup->isVisible())
                m_popup->hide();
            else
                QComboBox::hidePopup();
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QStyleOptionComboBox option;
            initStyleOption(&option);
            // The art, title and room counts are laid out inside the selector.
            option.currentText.clear();
            QPainter painter(this);
            style()->drawComplexControl(QStyle::CC_ComboBox, &option, &painter, this);
        }

        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_popup && event->type() == QEvent::Hide)
                QComboBox::hidePopup();

            if (watched == m_search && event->type() == QEvent::KeyPress)
            {
                const int key = static_cast<QKeyEvent*>(event)->key();
                if (key == Qt::Key_Escape)
                    hidePopup();
                else if (key == Qt::Key_Return || key == Qt::Key_Enter)
                    choose(m_results->currentIndex());
                else if (key == Qt::Key_Up || key == Qt::Key_Down)
                {
                    const int current = m_results->currentIndex().row();
                    const int last = m_filter->rowCount() - 1;
                    if (last >= 0)
                    {
                        const int row = std::clamp(current + (key == Qt::Key_Up ? -1 : 1), 0, last);
                        const QModelIndex next = m_filter->index(row, 0);
                        m_results->setCurrentIndex(next);
                        m_results->scrollTo(next);
                    }
                }
                else
                    return QComboBox::eventFilter(watched, event);
                return true;
            }
            return QComboBox::eventFilter(watched, event);
        }

    private:
        void selectResult()
        {
            m_pages->setCurrentWidget(m_filter->rowCount() > 0 ? static_cast<QWidget*>(m_results) : m_empty);
            const QModelIndex selected = m_search->text().trimmed().isEmpty()
                ? m_filter->mapFromSource(model()->index(currentIndex(), 0)) : QModelIndex();
            const QModelIndex result = selected.isValid() ? selected : m_filter->index(0, 0);
            m_results->setCurrentIndex(result);
            if (result.isValid())
                m_results->scrollTo(result);
        }

        void choose(const QModelIndex& result)
        {
            const QModelIndex source = m_filter->mapToSource(result);
            if (!source.isValid())
                return;
            const int row = source.row();
            hidePopup();
            setCurrentIndex(row);
        }

        QFrame* m_popup;
        QLineEdit* m_search;
        QSortFilterProxyModel* m_filter;
        QListView* m_results;
        QLabel* m_empty;
        QStackedWidget* m_pages;
    };

    void transparentForMouse(QWidget* widget)
    {
        widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    // A value over a tiny caption, for the play bar's ticket.
    struct KeyValue
    {
        QWidget* box = nullptr;
        QLabel* value = nullptr;
        QLabel* caption = nullptr;
    };

    KeyValue makeKeyValue(QWidget* parent)
    {
        KeyValue kv;
        kv.box = new QWidget(parent);
        auto* layout = new QVBoxLayout(kv.box);
        layout->setContentsMargins(10, 0, 10, 0);
        layout->setSpacing(3);
        kv.value = new QLabel(kv.box);
        kv.value->setFont(LobbyTheme::pixelFont(17));
        kv.value->setAlignment(Qt::AlignCenter);
        layout->addWidget(kv.value);
        kv.caption = new QLabel(kv.box);
        QFont captionFont = LobbyTheme::uiFont(9, QFont::DemiBold);
        captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);
        kv.caption->setFont(captionFont);
        kv.caption->setAlignment(Qt::AlignCenter);
        kv.caption->setProperty("role", "muted");
        layout->addWidget(kv.caption);
        return kv;
    }

    QWidget* makeSeparator(const char* name, QWidget* parent, int fixedHeight = 0)
    {
        auto* line = new QFrame(parent);
        line->setObjectName(name);
        if (fixedHeight > 0)
            line->setFixedHeight(fixedHeight);
        return line;
    }

    QLabel* makeFieldLabel(const QString& text, QWidget* parent)
    {
        auto* label = new QLabel(text.toUpper(), parent);
        QFont font = LobbyTheme::uiFont(10, QFont::DemiBold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.9);
        label->setFont(font);
        label->setProperty("role", "muted");
        return label;
    }
} // namespace

// ──────────────────────────────────────────────────────────────────────
//  Construction
// ──────────────────────────────────────────────────────────────────────

RollbackLobbyDialog::RollbackLobbyDialog(QWidget* parent)
    // Pass Qt::Window flags directly to the base — calling setWindowFlags
    // after construction doesn't always reliably remove the default Qt::Dialog
    // behavior on Windows. We want this to be a normal top-level window:
    //   • appears in the taskbar with its own entry (groupable under RMG-K)
    //   • can be minimized independently of the main window
    //   • doesn't float on top of the emulator render
    : QDialog(parent,
              Qt::Window
              | Qt::WindowTitleHint
              | Qt::WindowSystemMenuHint
              | Qt::WindowMinMaxButtonsHint
              | Qt::WindowCloseButtonHint)
{
#ifdef RMGK_TEST_BUILD
    setWindowTitle(tr("Dale64 Lobby") + " [TEST]");
#else
    setWindowTitle(tr("Dale64 Lobby"));
#endif
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setWindowModality(Qt::NonModal);
    resize(1180, 720);
    setObjectName("RollbackLobbyDialog");
    {
        // The dialog instance is kept alive between opens, so its size carries
        // within a run on its own — but only the saved geometry survives an
        // app restart. The 1180×720 above is the first-run default.
        QSettings s("RMG-K", "n02");
        const QByteArray geometry = s.value("RollbackLobby/WindowGeometry").toByteArray();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
    }

    m_client = new LobbyClient(this);
    m_lagStats = new LagStatsExchange([this](quint64 userId) { return m_client->measuredPingMs(userId); }, this);

    // Before anything is built: some widgets take their colours once.
    {
        const QString theme = QSettings("RMG-K", "n02").value(kLobbyThemeSetting).toString();
        LobbyTheme::setThemeChoice(theme == QLatin1String("dark")    ? LobbyTheme::ThemeChoice::Dark
                                   : theme == QLatin1String("light") ? LobbyTheme::ThemeChoice::Light
                                                                     : LobbyTheme::ThemeChoice::App);
    }

    buildUi();
    applyStylesheet();

    connect(m_client, &LobbyClient::stateChanged,         this, &RollbackLobbyDialog::onClientStateChanged);
    connect(m_client, &LobbyClient::helloFailed,          this, &RollbackLobbyDialog::onHelloFailed);
    connect(m_client, &LobbyClient::connectError,         this, &RollbackLobbyDialog::onConnectError);
    connect(m_client, &LobbyClient::presenceFull,         this, &RollbackLobbyDialog::onPresenceFull);
    connect(m_client, &LobbyClient::userAdded,            this, &RollbackLobbyDialog::onUserAdded);
    connect(m_client, &LobbyClient::userRemoved,          this, &RollbackLobbyDialog::onUserRemoved);
    connect(m_client, &LobbyClient::userUpdated,          this, &RollbackLobbyDialog::onUserUpdated);
    connect(m_client, &LobbyClient::roomListChanged,      this, &RollbackLobbyDialog::onRoomListChanged);
    connect(m_client, &LobbyClient::roomCreated,          this, &RollbackLobbyDialog::onRoomCreated);
    connect(m_client, &LobbyClient::roomCreateFailed,     this, &RollbackLobbyDialog::onRoomCreateFailed);
    connect(m_client, &LobbyClient::roomJoinOk,           this, &RollbackLobbyDialog::onRoomJoinOk);
    connect(m_client, &LobbyClient::roomJoinFailed,       this, &RollbackLobbyDialog::onRoomJoinFailed);
    connect(m_client, &LobbyClient::roomLeft,             this, &RollbackLobbyDialog::onRoomLeft);
    connect(m_client, &LobbyClient::roomStateChanged,     this, &RollbackLobbyDialog::onRoomStateChanged);
    connect(m_client, &LobbyClient::quickMatchStatus,     this, &RollbackLobbyDialog::onQuickMatchStatusChanged);
    connect(m_client, &LobbyClient::quickMatchRefused,    this, &RollbackLobbyDialog::onQuickMatchRefused);
    connect(m_client, &LobbyClient::localPlayersChanged,  this, &RollbackLobbyDialog::refreshLocalRoster);
    connect(m_client, &LobbyClient::localPlayerFailed,    this, &RollbackLobbyDialog::onLocalPlayerFailed);
    connect(m_client, &LobbyClient::chatMessageReceived,  this, &RollbackLobbyDialog::onChatMessageReceived);
    connect(m_client, &LobbyClient::adminAuthResult,      this, &RollbackLobbyDialog::onAdminAuthResult);
    connect(m_client, &LobbyClient::modNotice,            this, &RollbackLobbyDialog::onModNotice);
    connect(m_client, &LobbyClient::modListReceived,      this, &RollbackLobbyDialog::onModListReceived);
    connect(m_client, &LobbyClient::matchBegin,           this, &RollbackLobbyDialog::onMatchBegin);
    connect(m_client, &LobbyClient::matchPeerLeft,        this, &RollbackLobbyDialog::onMatchPeerLeft);
    connect(m_client, &LobbyClient::matchAdmitBegin,      this, &RollbackLobbyDialog::onMatchAdmitBegin);
    connect(m_client, &LobbyClient::matchAdmitStep,       this, &RollbackLobbyDialog::onMatchAdmitStep);
    connect(m_client, &LobbyClient::matchAdmitBlob,       this, &RollbackLobbyDialog::onMatchAdmitBlob);
    connect(m_client, &LobbyClient::matchAdmitFailed,     this, &RollbackLobbyDialog::onMatchAdmitFailed);
    connect(m_client, &LobbyClient::icePeerConnectionChanged,
            this, &RollbackLobbyDialog::onIcePeerConnectionChanged);
    connect(m_client, &LobbyClient::icePeerConnectionAttemptChanged,
            this, &RollbackLobbyDialog::onIcePeerConnectionAttemptChanged);
    connect(m_client, &LobbyClient::roomPingMeasurementsChanged,
            this, &RollbackLobbyDialog::onRoomPingMeasurementsChanged);
    connect(m_client, &LobbyClient::pingProbeMeasured,    this, &RollbackLobbyDialog::onPingMeasured);
    connect(m_client, &LobbyClient::pingProbeRetrying,    this, &RollbackLobbyDialog::onPingProbeRetrying);
    connect(m_client, &LobbyClient::pingProbeFailed,      this, &RollbackLobbyDialog::onPingProbeFailed);
    connect(m_client, &LobbyClient::pingProbeSoftFailed,  this, &RollbackLobbyDialog::onPingProbeSoftFailed);
    connect(m_client, &LobbyClient::spectateBegan,        this, &RollbackLobbyDialog::onSpectateBegan);
    connect(m_client, &LobbyClient::spectateData,         this, &RollbackLobbyDialog::onSpectateData);
    connect(m_client, &LobbyClient::spectateKeyframe,     this, &RollbackLobbyDialog::onSpectateKeyframe);
    connect(m_client, &LobbyClient::spectateEnded,        this, &RollbackLobbyDialog::onSpectateEnded);
    connect(m_client, &LobbyClient::spectateFailed,       this, &RollbackLobbyDialog::onSpectateFailed);
    connect(m_client, &LobbyClient::broadcastViewerCount, this, &RollbackLobbyDialog::onBroadcastViewerCount);
    connectSocialSignals();

    // Drains staged krec bytes to the WebSocket while broadcasting. ~80 ms keeps
    // WS frame overhead low without adding meaningful latency to spectators.
    m_broadcastDrainTimer = new QTimer(this);
    m_broadcastDrainTimer->setInterval(80);
    connect(m_broadcastDrainTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onBroadcastDrainTick);

    // 3 s cadence is a balance: fast enough that the displayed ping tracks real
    // conditions, slow enough that we're not flooding peers' anchor sockets. The
    // *first* ping for a seated peer doesn't wait on this tick — onRoomStateChanged
    // fires an immediate probe the moment a peer is seated. Runs whenever the
    // connection is up (see onClientStateChanged): seated peers refresh every
    // tick, the rest of the lobby on a slow round-robin for the hover tooltip.
    m_pingProbeTimer = new QTimer(this);
    m_pingProbeTimer->setInterval(3'000);
    connect(m_pingProbeTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onPingProbeTick);

    // The lobby shell is visible before a connection exists, so initialize its
    // controls and status without waiting for a state-change signal.
    onClientStateChanged(LobbyClient::ConnectionState::Disconnected);
}

RollbackLobbyDialog::~RollbackLobbyDialog()
{
    // A drag holds the cursor shape app-wide.
    endSeatDrag(QPoint(), false);

    // Saved here rather than closeEvent because Esc rejects the dialog
    // without a close event.
    QSettings s("RMG-K", "n02");
    s.setValue("RollbackLobby/WindowGeometry", saveGeometry());
    if (m_splitter)
        s.setValue("RollbackLobby/MainSplitterState", m_splitter->saveState());
    if (m_browseSplitter)
        s.setValue(browseSplitterKey(), m_browseSplitter->saveState());
    if (m_roomWindow)
    {
        s.setValue("RollbackLobby/RoomWindowGeometry", m_roomWindow->normalGeometry());
        s.setValue("RollbackLobby/RoomWindowMaximized", m_roomWindow->isMaximized());
    }
    if (m_sideSplitter)
        s.setValue("RollbackLobby/SidebarSplitterState", m_sideSplitter->saveState());

    // Detach the recording sink before we're gone — it captures `this` and is
    // invoked on the emulation thread (emulation should already be stopped here).
    if (m_broadcasting)
        n02::setRecordingStreamSink(nullptr);
    if (m_client)
        m_client->disconnectFromServer();
}

// ──────────────────────────────────────────────────────────────────────
//  UI construction
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildLobbyView());
}

QWidget* RollbackLobbyDialog::buildLobbyView()
{
    auto* container = new LobbyBackdrop(this);
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    lay->addWidget(buildMarquee());
    lay->addWidget(new PortStripe(container));

    m_splitter = new QSplitter(Qt::Horizontal, container);
    m_splitter->setHandleWidth(SPLITTER_MAIN);
    m_splitter->setChildrenCollapsible(false);

    // The browse view fills the stage. The room window floats over it, above a
    // scrim that dims it; the challenge card and the undo toast float over both.
    m_stage = new QWidget(container);
    auto* stageLay = new QVBoxLayout(m_stage);
    stageLay->setContentsMargins(0, 0, 0, 0);
    stageLay->addWidget(buildBrowseView());
    m_stage->installEventFilter(this);
    m_scrim = new StageScrim(m_stage);
    m_scrim->setVisible(false);
    connect(m_scrim, &StageScrim::clicked, this, &RollbackLobbyDialog::switchToRoomsView);
    m_roomWindow = new RoomWindow(m_stage);
    m_roomWindow->setVisible(false);
    m_roomWindow->setBody(buildInRoomView());
    {
        QSettings s("RMG-K", "n02");
        m_roomWindow->setNormalGeometry(s.value("RollbackLobby/RoomWindowGeometry").toRect());
        m_roomWindow->setMaximized(s.value("RollbackLobby/RoomWindowMaximized", false).toBool());
    }
    connect(m_roomWindow, &RoomWindow::minimizeRequested, this, &RollbackLobbyDialog::switchToRoomsView);
    connect(m_roomWindow, &RoomWindow::geometryChanged, this, [this]() {
        m_scrim->setWindowRect(m_roomWindow->geometry());
    });
    m_challengeCard = new ChallengeCard(m_stage);
    m_challengeCard->setVisible(false);
    connect(m_challengeCard, &ChallengeCard::acceptClicked, this, [this]() { answerChallenge(true); });
    connect(m_challengeCard, &ChallengeCard::declineClicked, this, [this]() { answerChallenge(false); });
    m_toast = new UndoToast(m_stage);
    connect(m_toast, &UndoToast::undoClicked, this, [this]() {
        if (!m_undoAction.isEmpty())
            m_client->sendModAction(m_undoAction, m_undoIp);
    });
    m_splitter->addWidget(m_stage);

    m_splitter->addWidget(buildSidebar());

    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setSizes({ width() - SIDEBAR_WIDTH - SPLITTER_MAIN, SIDEBAR_WIDTH });
    {
        QSettings s("RMG-K", "n02");
        const QByteArray splitState = s.value("RollbackLobby/MainSplitterState").toByteArray();
        if (!splitState.isEmpty())
            m_splitter->restoreState(splitState);
    }

    lay->addWidget(m_splitter, 1);
    return container;
}

// ── Top bar ──────────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildMarquee()
{
    m_marquee = new LobbyBand(this);
    m_marquee->setObjectName("LobbyMarquee");
    m_marquee->setFixedHeight(MARQUEE_HEIGHT);

    auto* h = new QHBoxLayout(m_marquee);
    h->setContentsMargins(MARGIN_STAGE, 0, MARGIN_STAGE, 0);
    h->setSpacing(MARGIN_STAGE);

    auto* brand = new QHBoxLayout;
    brand->setSpacing(10);
    brand->addWidget(new BrandMark(m_marquee));
    auto* words = new QVBoxLayout;
    words->setSpacing(1);
    words->setAlignment(Qt::AlignVCenter);
    m_brandLabel = new QLabel(QStringLiteral("Dale64"), m_marquee);
    m_brandLabel->setObjectName("LobbyBrand");
    m_brandLabel->setFont(LobbyTheme::pixelFont(18, 0.3));
    words->addWidget(m_brandLabel);
    auto* tagline = new QLabel(tr("ROLLBACK NETPLAY"), m_marquee);
    QFont taglineFont = LobbyTheme::uiFont(8, QFont::Bold);
    taglineFont.setLetterSpacing(QFont::AbsoluteSpacing, 2.2);
    tagline->setFont(taglineFont);
    tagline->setProperty("role", "muted");
    words->addWidget(tagline);
    brand->addLayout(words);
    h->addLayout(brand);

    h->addWidget(makeSeparator("BrandSeparator", m_marquee, 22));

    // Connection status: an LED and a status-coloured word (set in
    // updateStatusIndicator).
    auto* status = new QHBoxLayout;
    status->setSpacing(6);
    m_statusLed = new LedDot(m_marquee);
    status->addWidget(m_statusLed);
    m_statusText = new QLabel(tr("Offline"), m_marquee);
    m_statusText->setFont(LobbyTheme::pixelFont(11, 0.8));
    status->addWidget(m_statusText);
    h->addLayout(status);

    m_serverMeta = new QLabel(tr("Not connected"), m_marquee);
    m_serverMeta->setFont(LobbyTheme::uiFont(12));
    m_serverMeta->setTextFormat(Qt::RichText);
    m_serverMeta->setProperty("role", "muted");
    h->addWidget(m_serverMeta);

    h->addStretch(1);

    m_userChip = new LayoutButton(m_marquee);
    m_userChip->setObjectName("UserChip");
    m_userChip->setAutoDefault(false);
    m_userChip->setCursor(Qt::PointingHandCursor);
    m_userChip->setFocusPolicy(Qt::NoFocus);
    m_userChip->setVisible(false);
    auto* chip = new QHBoxLayout(m_userChip);
    chip->setContentsMargins(3, 3, 8, 3);
    chip->setSpacing(8);
    m_userAvatar = new UserAvatar(m_userChip);
    chip->addWidget(m_userAvatar);
    auto* who = new QVBoxLayout;
    who->setSpacing(0);
    // Your flag goes by your name, where everyone else's does too.
    auto* nameLine = new QHBoxLayout;
    nameLine->setSpacing(6);
    m_userName = new QLabel(m_userChip);
    m_userName->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
    transparentForMouse(m_userName);
    nameLine->addWidget(m_userName);
    m_userFlag = new CountryFlag(m_userChip);
    transparentForMouse(m_userFlag);
    nameLine->addWidget(m_userFlag);
    nameLine->addStretch(1);
    who->addLayout(nameLine);
    auto* sub = new QHBoxLayout;
    sub->setSpacing(4);
    m_userRoleIcon = new IconLabel(QStringLiteral("shield-star-fill"), Role::Accent, 11, m_userChip);
    transparentForMouse(m_userRoleIcon);
    m_userRoleIcon->setVisible(false);
    sub->addWidget(m_userRoleIcon);
    m_userRole = new QLabel(tr("Guest"), m_userChip);
    m_userRole->setFont(LobbyTheme::uiFont(10));
    m_userRole->setProperty("role", "muted");
    transparentForMouse(m_userRole);
    sub->addWidget(m_userRole);
    sub->addStretch(1);
    who->addLayout(sub);
    chip->addLayout(who);
    auto* chevron = new IconLabel(QStringLiteral("arrow-down-s-line"), Role::Muted, 15, m_userChip);
    transparentForMouse(chevron);
    chip->addWidget(chevron);
    connect(m_userChip, &QPushButton::clicked, this, &RollbackLobbyDialog::showUserMenu);
    h->addWidget(m_userChip, 0, Qt::AlignVCenter);

    return m_marquee;
}

void RollbackLobbyDialog::updateUserChip()
{
    const bool connected = m_client->state() == LobbyClient::ConnectionState::Connected;
    m_userChip->setVisible(connected);
    if (!connected)
        return;

    QString name = m_username;
    QString country;
    const auto self = m_client->users().constFind(m_client->selfUserId());
    if (self != m_client->users().constEnd())
    {
        name = self->username;
        country = self->anonymous ? QString() : self->country;
    }
    m_userName->setText(name);
    m_userAvatar->setInitial(LobbyTheme::nameInitial(name));
    m_userFlag->setCountry(country);
    m_userFlag->setToolTip(countryTooltip(country));
    const bool moderator = m_client->isModerator();
    m_userFlag->setVisible(!country.isEmpty());
    m_userRoleIcon->setVisible(moderator);
    m_userRole->setText(moderator ? tr("Moderator") : tr("Guest"));
    m_userRole->setStyleSheet(moderator ? QString("color: %1; font-weight: 600;").arg(LobbyTheme::tokens().accentInk.name()) : QString());
}

// ── Play bar ─────────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildPlayBar()
{
    m_playBar = new LobbyCard(this, /*scanlines=*/true);
    m_playBar->setObjectName("PlayBar");
    m_playBar->setFixedHeight(PLAYBAR_HEIGHT);

    auto* outer = new QVBoxLayout(m_playBar);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);
    m_playBarStack = new QStackedWidget(m_playBar);
    outer->addWidget(m_playBarStack);

    // ── Idle: pick a game, then Quick Match or Create Room ──
    auto* idle = new QWidget(m_playBarStack);
    auto* idleLay = new QHBoxLayout(idle);
    idleLay->setContentsMargins(0, 0, 0, 0);
    idleLay->setSpacing(8);

    auto* picker = new BrowseGameCombo(tr("Search your games"), tr("No matching games"), idle);
    m_browseRomCombo = picker;
    picker->setObjectName("BrowseRomCombo");
    picker->setAccessibleName(tr("Game"));
    picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    picker->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    picker->setFixedHeight(44);
    auto* pickerLay = new QHBoxLayout(picker);
    pickerLay->setContentsMargins(7, 0, 28, 0);
    pickerLay->setSpacing(10);
    m_pickerTile = new CartridgeTile(CartridgeTile::Size::Small, picker);
    transparentForMouse(m_pickerTile);
    pickerLay->addWidget(m_pickerTile);

    auto* pickerText = new QVBoxLayout;
    pickerText->setAlignment(Qt::AlignVCenter);
    pickerText->setSpacing(1);
    m_pickerName = new ElidedLabel(Role::Ink, picker);
    m_pickerName->setFont(LobbyTheme::uiFont(13, QFont::Bold));
    m_pickerName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    transparentForMouse(m_pickerName);
    pickerText->addWidget(m_pickerName);
    // Remember the last game picked here — Create Room and Quick Match both key
    // off this combo, so persisting on selection (not just on Create) means the
    // choice survives across sessions however the user proceeds. populateBrowseRoms
    // blocks signals while restoring, so this never fires for the restore itself.
    connect(m_browseRomCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (idx < 0 || !m_browseRomCombo->isEnabled()) return;
                const QString name = m_browseRomCombo->itemText(idx);
                if (name.isEmpty()) return;
                QSettings s("RMG-K", "n02");
                s.beginGroup("Lobby/CreateRoom");
                s.setValue("Rom", name);
                s.endGroup();
                refreshSameGameFilter();
            });
    m_pickerInfo = new QLabel(picker);
    m_pickerInfo->setFont(LobbyTheme::uiFont(11));
    m_pickerInfo->setTextFormat(Qt::RichText);
    m_pickerInfo->setProperty("role", "muted");
    m_pickerInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    transparentForMouse(m_pickerInfo);
    pickerText->addWidget(m_pickerInfo);
    pickerLay->addLayout(pickerText, 1);
    idleLay->addWidget(picker, 1);

    m_quickMatchBtn = new LobbyButton(tr("Quick Match"), LobbyButton::Kind::Primary, idle);
    m_quickMatchBtn->setObjectName("QuickMatchBtn");
    m_quickMatchBtn->setEnabled(false);
    m_quickMatchBtn->setButtonSize(LobbyButton::Size::Large);
    m_quickMatchBtn->setPixelLabel(true);
    m_quickMatchBtn->setIconName(QStringLiteral("flashlight-fill"));
    m_quickMatchBtn->setToolTip(
        tr("Auto-match with another player searching for the selected game.\n"
           "Uses Auto frame delay unless remembering your last choice is enabled.\n"
           "Your frame delay can be changed during the warmup. Prediction defaults to 7 frames."));
    m_createRoomBtn = new LobbyButton(tr("Create Room"), LobbyButton::Kind::Secondary, idle);
    m_createRoomBtn->setObjectName("CreateRoomBtn");
    m_createRoomBtn->setButtonSize(LobbyButton::Size::Large);
    m_createRoomBtn->setPixelLabel(true);
    m_createRoomBtn->setIconName(QStringLiteral("add-line"));

    connect(m_quickMatchBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onQuickMatchClicked);
    connect(m_createRoomBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onCreateRoomClicked);

    idleLay->addWidget(m_quickMatchBtn);
    idleLay->addWidget(m_createRoomBtn);
    m_playBarStack->addWidget(idle);

    // ── Ticket: searching, watching, or seated in a room ──
    auto* ticket = new QWidget(m_playBarStack);
    auto* ticketLay = new QHBoxLayout(ticket);
    ticketLay->setContentsMargins(6, 0, 0, 0);
    ticketLay->setSpacing(12);

    auto* mark = new QWidget(ticket);
    mark->setFixedSize(38, 38);
    m_ticketBadge = new IconBadge(38, 9, 18, mark);
    m_ticketSpinner = new Spinner(20, mark);
    m_ticketSpinner->move(9, 9);
    m_ticketSpinner->setVisible(false);
    ticketLay->addWidget(mark);

    auto* words = new QVBoxLayout;
    words->setSpacing(3);
    words->setAlignment(Qt::AlignVCenter);
    m_ticketTitle = new QLabel(ticket);
    m_ticketTitle->setFont(LobbyTheme::pixelFont(14, 0.4));
    words->addWidget(m_ticketTitle);
    m_ticketSub = new ElidedLabel(Role::Muted, ticket);
    m_ticketSub->setFont(LobbyTheme::uiFont(11));
    m_ticketSub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    words->addWidget(m_ticketSub);
    ticketLay->addLayout(words, 1);

    KeyValue first = makeKeyValue(ticket);
    KeyValue second = makeKeyValue(ticket);
    m_ticketValueBoxes[0] = first.box;
    m_ticketValues[0] = first.value;
    m_ticketCaptions[0] = first.caption;
    m_ticketValueBoxes[1] = second.box;
    m_ticketValues[1] = second.value;
    m_ticketCaptions[1] = second.caption;
    ticketLay->addWidget(first.box);
    ticketLay->addWidget(second.box);

    m_ticketLeave = new LobbyButton(tr("Leave Room"), LobbyButton::Kind::Danger, ticket);
    m_ticketLeave->setObjectName("PlayBarLeaveRoom");
    m_ticketLeave->setButtonSize(LobbyButton::Size::Large);
    m_ticketLeave->setIconName(QStringLiteral("logout-box-r-line"));
    m_ticketLeave->hide();
    connect(m_ticketLeave, &QPushButton::clicked, this, &RollbackLobbyDialog::onLeaveRoomClicked);
    ticketLay->addWidget(m_ticketLeave);

    m_ticketAction = new LobbyButton(QString(), LobbyButton::Kind::Secondary, ticket);
    m_ticketAction->setObjectName("PlayBarAction");
    m_ticketAction->setButtonSize(LobbyButton::Size::Large);
    connect(m_ticketAction, &QPushButton::clicked, this, &RollbackLobbyDialog::onPlayBarActionClicked);
    ticketLay->addWidget(m_ticketAction);
    m_playBarStack->addWidget(ticket);

    populateBrowseRoms(); // fill from whatever library we have so far
    return m_playBar;
}

// ── Browse view ──────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildBrowseView()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(MARGIN_STAGE, MARGIN_STAGE, MARGIN_STAGE_INNER, MARGIN_STAGE);
    lay->setSpacing(GAP_STAGE);

    lay->addWidget(buildPlayBar());

    // ── Live gallery beside the open rooms (or above them, the player's
    //    choice), dividing a draggable splitter so each person can give
    //    running games as much of the window as they like ──
    const bool stacked = QSettings("RMG-K", "n02").value(kBrowseLayoutSetting).toString() == QLatin1String("stacked");
    m_browseSplitter = new QSplitter(stacked ? Qt::Vertical : Qt::Horizontal, this);
    m_browseSplitter->setHandleWidth(GAP_STAGE);
    m_browseSplitter->setChildrenCollapsible(false);

    m_liveGallery = new LiveGallery(this);
    m_liveGallery->setMinimumWidth(276);
    connect(m_liveGallery, &LiveGallery::watchRequested, this, &RollbackLobbyDialog::onMatchWatchRequested);
    connect(m_liveGallery, &LiveGallery::joinRequested, this, &RollbackLobbyDialog::onRoomJoinRequested);
    connect(m_liveGallery, &LiveGallery::stopWatchingRequested, this, [this]() {
        m_pendingSpectate = PendingSpectate{};
        emit spectateStreamClosed(QStringLiteral("stopped"));
    });
    connect(m_liveGallery, &LiveGallery::contextMenuRequested, this, &RollbackLobbyDialog::showAdminRoomMenu);
    connect(m_liveGallery, &LiveGallery::playerMenuRequested, this, &RollbackLobbyDialog::showPlayerMenuForName);
    m_browseSplitter->addWidget(m_liveGallery);

    m_roomsColumn = new RoomsColumn(this);
    m_roomsColumn->setMinimumWidth(220);
    connect(m_roomsColumn, &RoomsColumn::joinRequested, this, &RollbackLobbyDialog::onRoomJoinRequested);
    connect(m_roomsColumn, &RoomsColumn::returnRequested, this, &RollbackLobbyDialog::switchToInRoomView);
    // Selecting a room measures the host on demand, so the ping can be an
    // estimate until someone actually cares about that room.
    connect(m_roomsColumn, &RoomsColumn::hostProbeRequested, this, &RollbackLobbyDialog::probeOnDemand);
    connect(m_roomsColumn, &RoomsColumn::contextMenuRequested, this, &RollbackLobbyDialog::showAdminRoomMenu);
    connect(m_roomsColumn, &RoomsColumn::playerMenuRequested, this, &RollbackLobbyDialog::showPlayerMenuForName);
    m_browseSplitter->addWidget(m_roomsColumn);

    // Both panes grow with the window while keeping their relative sizes.
    m_browseSplitter->setStretchFactor(0, 1);
    m_browseSplitter->setStretchFactor(1, 1);
    restoreBrowseSplit();
    lay->addWidget(m_browseSplitter, 1);

    refreshSameGameFilter();

    // Tick the match durations once a second (the room list only refreshes on
    // events, so the timer keeps the elapsed time live).
    m_matchDurationTimer = new QTimer(this);
    m_matchDurationTimer->setInterval(1000);
    connect(m_matchDurationTimer, &QTimer::timeout, this, &RollbackLobbyDialog::updateMatchDurations);
    m_matchDurationTimer->start();

    return page;
}

// ── In-room view ─────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildInRoomView()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(9, 12, 9, 9);
    lay->setSpacing(GAP_STAGE);

    // ── Title bar: the room's game and name, then its state and settings ──
    m_roomBadge = new GameBadge(26, m_roomWindow);
    m_roomWindow->titleLeft()->addWidget(m_roomBadge);
    m_roomTitle = new ElidedLabel(Role::Ink, m_roomWindow);
    m_roomTitle->setFont(LobbyTheme::uiFont(17, QFont::Bold));
    m_roomTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_roomWindow->titleLeft()->addWidget(m_roomTitle, 1);

    m_roomStatePill = new StatePill(m_roomWindow);
    m_roomStatePill->setObjectName("RoomStatePill");
    m_roomWindow->titleRight()->addWidget(m_roomStatePill, 0, Qt::AlignVCenter);
    applyRoomStateBadge(tr("Waiting"), stateHex("waiting", isDarkTheme()));
    m_roomSettingsButton = new IconButton(QStringLiteral("settings-3-line"), 28, m_roomWindow);
    m_roomSettingsButton->setObjectName("RoomSettingsButton");
    m_roomSettingsButton->setToolTip(tr("Room settings: rollback prediction, and who can join"));
    m_roomSettingsButton->setAccessibleName(tr("Room settings"));
    m_roomWindow->titleRight()->addWidget(m_roomSettingsButton);

    // ── Seats: a port card each, sharing the width ──
    auto* seatsBox = new QWidget(page);
    m_seatsBox = seatsBox;
    auto* seatsLay = new QHBoxLayout(seatsBox);
    seatsLay->setContentsMargins(0, 0, 0, 0);
    seatsLay->setSpacing(10);
    for (int i = 0; i < 4; ++i)
    {
        buildSeatRow(m_seats[i], i + 1, seatsBox);
        seatsLay->addWidget(m_seats[i].row, 1);
        renderSeatEmpty(m_seats[i]);
    }
    lay->addWidget(seatsBox, 1);

    // ── Your delay, and this session's switches ──
    auto* net = new LobbyCard(page);
    net->setObjectName("RoomSettings");
    auto* netLay = new QHBoxLayout(net);
    netLay->setContentsMargins(14, 12, 14, 12);
    netLay->setSpacing(16);

    auto* delayCol = new QVBoxLayout;
    delayCol->setSpacing(6);
    auto* delayHeader = new QHBoxLayout;
    delayHeader->setSpacing(10);
    delayHeader->addWidget(makeFieldLabel(tr("Your input delay"), net));
    m_delayAutoCheck = new ToggleSwitch(tr("Auto"), net);
    m_delayAutoCheck->setObjectName("DelayAutoSwitch");
    m_delayAutoCheck->setFixedWidth(m_delayAutoCheck->sizeHint().width());
    m_delayAutoCheck->setChecked(m_delayAuto);
    delayHeader->addWidget(m_delayAutoCheck);
    delayHeader->addStretch(1);
    delayCol->addLayout(delayHeader);
    m_delayStepper = new FrameStepper(0, 9, net);
    m_delayStepper->setObjectName("DelayStepper");
    delayCol->addWidget(m_delayStepper);
    delayCol->addStretch(1);
    netLay->addLayout(delayCol, 4);

    netLay->addWidget(makeSeparator("FieldSeparator", net));

    auto* sessionCol = new QVBoxLayout;
    sessionCol->setSpacing(8);
    sessionCol->addWidget(makeFieldLabel(tr("Session"), net));
    auto* switches = new QGridLayout;
    switches->setHorizontalSpacing(18);
    switches->setVerticalSpacing(8);

    // Per-player local saving, independent of the host's Live Replay choice.
    // Editable before the match and initialized from the cap-aware default.
    m_recordCheck = new ToggleSwitch(tr("Save replay"), net);
    m_recordCheck->setObjectName("RecordSwitch");
    const bool recordingDefault = CoreGetKailleraEffectiveRecordingDefault();
    n02_kaillera_recording_enabled = recordingDefault;
    m_recordLocallyChoice = recordingDefault;
    m_recordCheck->setChecked(recordingDefault);
    connect(m_recordCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_recordLocallyChoice = checked;
        n02_kaillera_recording_enabled = checked;
    });
    switches->addWidget(m_recordCheck, 0, 0);

    // Broadcast replay bytes to the server without requiring a local file.
    m_broadcastCheck = new ToggleSwitch(tr("Allow Spectators"), net);
    m_broadcastCheck->setObjectName("LiveReplaySwitch");
    m_broadcastCheck->setChecked(liveReplayPreference());
    connect(m_broadcastCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_suppressSettingsSignal) return;
        updateRecordingControls();
        if (m_currentRoomId != 0 && m_client && m_currentRoomHostId == m_client->selfUserId())
        {
            QSettings settings("RMG-K", "n02");
            settings.setValue("RollbackLobby/LiveReplayEnabled", checked);
            m_client->updateRoomLiveReplay(checked);
        }
    });
    switches->addWidget(m_broadcastCheck, 1, 0);

    // Off in every new room. The server repeats the host and waiting checks.
    m_lateJoinCheck = new ToggleSwitch(tr("Allow joins after start"), net);
    m_lateJoinCheck->setObjectName("LateJoinSwitch");
    m_lateJoinCheck->setVisible(false);
    connect(m_lateJoinCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_suppressSettingsSignal) return;
        if (m_currentRoomId == 0 || !m_client || m_currentRoomHostId != m_client->selfUserId())
            return;
        m_client->updateRoomLateJoin(checked);
    });
    switches->addWidget(m_lateJoinCheck, 0, 1);

    // Off in every new room: the match starts from a blank save. The server
    // repeats the host and waiting checks.
    m_saveDataCheck = new ToggleSwitch(tr("Sync save data"), net);
    m_saveDataCheck->setObjectName("SaveDataSwitch");
    m_saveDataCheck->setVisible(false);
    connect(m_saveDataCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_suppressSettingsSignal) return;
        if (m_currentRoomId == 0 || !m_client || m_currentRoomHostId != m_client->selfUserId())
            return;
        if (checked && !m_hostSaveChoice && !pickStartingSave())
        {
            m_suppressSettingsSignal = true;
            m_saveDataCheck->setChecked(false);
            m_suppressSettingsSignal = false;
            return;
        }
        if (checked)
            publishHostSaveChoice();
        else
            m_client->updateRoomSaveData(false);
    });
    switches->addWidget(m_saveDataCheck, 1, 1);
    switches->setColumnStretch(2, 1);
    // Game options go in the rows below, once the room's game is known.
    m_sessionSwitches = switches;
    m_gameOptionsResync = new QTimer(this);
    m_gameOptionsResync->setSingleShot(true);
    m_gameOptionsResync->setInterval(3000);
    connect(m_gameOptionsResync, &QTimer::timeout, this, &RollbackLobbyDialog::updateGameOptionControls);
    sessionCol->addLayout(switches);
    sessionCol->addStretch(1);
    netLay->addLayout(sessionCol, 7);

    lay->addWidget(net);

    m_delayStepper->setProperty("originalTip", tr("Frames of input delay added before sending to peer.\n"
        "Higher delay = fewer rollbacks but more input latency.\n"
        "Recommended: 2 for ~80ms RTT, 3-4 for ~150ms RTT.\n"
        "\n"
        "Local setting — each player may choose a different value."));

    // ── Room settings, behind the gear: prediction, and who can join ──
    m_roomSettingsPanel = new DropPanel(m_roomWindow);
    m_roomSettingsPanel->setObjectName("RoomSettingsPanel");
    m_roomSettingsPanel->setFixedWidth(312);
    auto* panelLay = new QVBoxLayout(m_roomSettingsPanel);
    panelLay->setContentsMargins(14, 12, 14, 14);
    panelLay->setSpacing(6);
    panelLay->addWidget(makeFieldLabel(tr("Rollback prediction"), m_roomSettingsPanel));
    m_predictionStepper = new FrameStepper(1, 9, m_roomSettingsPanel);
    m_predictionStepper->setObjectName("PredictionStepper");
    m_predictionStepper->setChipLabels(tr("Default"), tr("Custom"));
    m_predictionStepper->setValue(kDefaultPredictionWindow);
    panelLay->addWidget(m_predictionStepper);
    auto* predictionFoot = new QHBoxLayout;
    predictionFoot->setSpacing(5);
    m_predictionLock = new IconLabel(QStringLiteral("lock-line"), Role::Muted, 12, m_roomSettingsPanel);
    m_predictionLock->setVisible(false);
    predictionFoot->addWidget(m_predictionLock, 0, Qt::AlignTop);
    m_predictionHint = new QLabel(m_roomSettingsPanel);
    m_predictionHint->setFont(LobbyTheme::uiFont(11));
    m_predictionHint->setProperty("role", "muted");
    m_predictionHint->setWordWrap(true);
    predictionFoot->addWidget(m_predictionHint, 1);
    panelLay->addLayout(predictionFoot);
    m_predictionStepper->setProperty("originalTip", tr("Maximum frames the rollback engine may predict ahead.\n"
        "Higher prediction tolerates longer network stalls but can produce larger rollbacks.\n"
        "Default: 7 frames.\n"
        "\n"
        "Room setting — only the host may change this value."));

    // Seats: the host adds or takes some away while the room waits.
    m_roomSizeBox = new QWidget(m_roomSettingsPanel);
    auto* sizeLay = new QVBoxLayout(m_roomSizeBox);
    sizeLay->setContentsMargins(0, 8, 0, 0);
    sizeLay->setSpacing(6);
    auto* sizeRule = new QFrame(m_roomSizeBox);
    sizeRule->setObjectName("FieldRule");
    sizeLay->addWidget(sizeRule);
    sizeLay->addSpacing(4);
    sizeLay->addWidget(makeFieldLabel(tr("Players", "how many seats the room has"), m_roomSizeBox));
    auto* sizeSeg = new QFrame(m_roomSizeBox);
    sizeSeg->setObjectName("Seg");
    auto* sizeSegLay = new QHBoxLayout(sizeSeg);
    sizeSegLay->setContentsMargins(3, 3, 3, 3);
    sizeSegLay->setSpacing(3);
    m_roomSizeGroup = new QButtonGroup(this);
    for (int seats = 2; seats <= 4; ++seats)
    {
        auto* button = new QPushButton(QString::number(seats), sizeSeg);
        button->setObjectName(QString("RoomSizeSeg%1").arg(seats));
        button->setProperty("seg", true);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(LobbyTheme::uiFont(12, QFont::DemiBold));
        m_roomSizeGroup->addButton(button, seats);
        sizeSegLay->addWidget(button);
    }
    sizeLay->addWidget(sizeSeg);
    m_roomSizeHint = new QLabel(m_roomSizeBox);
    m_roomSizeHint->setFont(LobbyTheme::uiFont(11));
    m_roomSizeHint->setProperty("role", "muted");
    m_roomSizeHint->setWordWrap(true);
    sizeLay->addWidget(m_roomSizeHint);
    m_roomSizeBox->setVisible(false);
    panelLay->addWidget(m_roomSizeBox);
    connect(m_roomSizeGroup, &QButtonGroup::idClicked, this, [this](int seats) {
        if (m_currentRoomId == 0 || !m_client || m_currentRoomHostId != m_client->selfUserId())
            return;
        m_client->updateRoomMaxPlayers(seats);
    });

    m_joinLimitsBox = new QWidget(m_roomSettingsPanel);
    auto* limitsLay = new QVBoxLayout(m_joinLimitsBox);
    limitsLay->setContentsMargins(0, 8, 0, 0);
    limitsLay->setSpacing(6);
    auto* limitsRule = new QFrame(m_joinLimitsBox);
    limitsRule->setObjectName("FieldRule");
    limitsLay->addWidget(limitsRule);
    limitsLay->addSpacing(4);
    limitsLay->addWidget(makeFieldLabel(tr("Who can join"), m_joinLimitsBox));
    auto* limitsRow = new QHBoxLayout;
    limitsRow->setSpacing(12);
    m_joinPingCombo = new QComboBox(m_joinLimitsBox);
    m_joinPingCombo->setObjectName("JoinPingCombo");
    CreateRoomDialog::fillPingLimitCombo(m_joinPingCombo);
    m_joinPingCombo->setToolTip(tr("Players whose ping to the host is higher can't join."));
    limitsRow->addWidget(m_joinPingCombo, 1);
    m_wiredOnlyCheck = new ToggleSwitch(tr("Wired only"), m_joinLimitsBox);
    m_wiredOnlyCheck->setObjectName("WiredOnlySwitch");
    m_wiredOnlyCheck->setToolTip(tr("Players on Wi-Fi can't join."));
    limitsRow->addWidget(m_wiredOnlyCheck);
    limitsLay->addLayout(limitsRow);
    m_joinLimitsHint = new QLabel(m_joinLimitsBox);
    m_joinLimitsHint->setFont(LobbyTheme::uiFont(11));
    m_joinLimitsHint->setProperty("role", "muted");
    m_joinLimitsHint->setWordWrap(true);
    limitsLay->addWidget(m_joinLimitsHint);
    m_joinLimitsBox->setVisible(false);
    panelLay->addWidget(m_joinLimitsBox);

    connect(m_roomSettingsButton, &QToolButton::clicked, this, [this]() {
        m_roomSettingsPanel->toggleBelow(m_roomSettingsButton);
    });
    const auto publishJoinLimits = [this]() {
        if (m_suppressSettingsSignal || m_currentRoomId == 0 || !m_client ||
            m_currentRoomHostId != m_client->selfUserId())
            return;
        m_client->updateRoomJoinLimits(m_joinPingCombo->currentData().toInt(), m_wiredOnlyCheck->isChecked());
    };
    connect(m_joinPingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, publishJoinLimits);
    connect(m_wiredOnlyCheck, &QCheckBox::toggled, this, publishJoinLimits);

    // Which save the match starts from; during and after a match, how the
    // game's save stands and the buttons to keep a copy.
    m_saveDataRow = new QWidget(this);
    auto* saveRow = new QHBoxLayout(m_saveDataRow);
    saveRow->setContentsMargins(0, 0, 0, 0);
    saveRow->setSpacing(8);
    m_saveDataNote = new QLabel(m_saveDataRow);
    m_saveDataNote->setObjectName("SaveDataNote");
    m_saveDataNote->setFont(LobbyTheme::uiFont(12));
    m_saveDataNote->setTextFormat(Qt::RichText);
    m_saveDataNote->setWordWrap(true);
    m_saveDataNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    saveRow->addWidget(m_saveDataNote, 1);
    m_saveDataChangeBtn = new LobbyButton(tr("Change..."), LobbyButton::Kind::Secondary, m_saveDataRow);
    m_saveDataChangeBtn->setObjectName("SaveDataChangeBtn");
    m_saveDataChangeBtn->setButtonSize(LobbyButton::Size::Small);
    m_saveDataChangeBtn->setIconName(QStringLiteral("edit-line"));
    m_saveDataChangeBtn->setToolTip(tr("Pick another save for the next match to start from."));
    connect(m_saveDataChangeBtn, &QPushButton::clicked, this, [this]() {
        if (pickStartingSave())
            publishHostSaveChoice();
    });
    saveRow->addWidget(m_saveDataChangeBtn);
    m_saveDataSaveAsBtn = new LobbyButton(tr("Save As..."), LobbyButton::Kind::Secondary, m_saveDataRow);
    m_saveDataSaveAsBtn->setObjectName("SaveDataSaveAsBtn");
    m_saveDataSaveAsBtn->setButtonSize(LobbyButton::Size::Small);
    m_saveDataSaveAsBtn->setIconName(QStringLiteral("file-copy-line"));
    m_saveDataSaveAsBtn->setToolTip(tr("Save the game's save data to a file on your PC."));
    connect(m_saveDataSaveAsBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::saveMatchSaveAs);
    saveRow->addWidget(m_saveDataSaveAsBtn);
    m_saveDataKeepCheck = new ToggleSwitch(tr("Keep updated"), m_saveDataRow);
    m_saveDataKeepCheck->setObjectName("SaveDataKeepSwitch");
    m_saveDataKeepCheck->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_saveDataKeepCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_suppressSettingsSignal) return;
        if (checked && m_keepCopyFile.isEmpty())
        {
            m_keepCopyEnabled = true;
            saveMatchSaveAs();
            if (m_keepCopyFile.isEmpty())
                m_keepCopyEnabled = false;
        }
        else
        {
            m_keepCopyEnabled = checked;
            if (checked && m_matchSave)
                writeMatchSave(m_keepCopyFile);
        }
        updateSaveDataControls();
    });
    saveRow->addWidget(m_saveDataKeepCheck);
    m_saveDataRow->setVisible(false);
    lay->addWidget(m_saveDataRow);
    updateRecordingControls();

    // Delay is local to each player and is published for the seat display.
    const auto delayEdited = [this]() {
        if (m_currentRoomId == 0) return;
        m_delayAuto = m_delayStepper->isAutomatic();
        {
            const QSignalBlocker blocker(m_delayAutoCheck);
            m_delayAutoCheck->setChecked(m_delayAuto);
        }
        if (CoreSettingsGetBoolValue(SettingsID::Rollback_RememberInputDelay))
        {
            QSettings settings("RMG-K", "n02");
            settings.setValue("RollbackLobby/LastInputDelay", m_delayAuto ? -1 : m_delayStepper->value());
        }
        applyLocalFrameDelay(true);
        refreshSettingHints();
    };
    connect(m_delayStepper, &FrameStepper::edited, this, delayEdited);
    connect(m_delayAutoCheck, &QCheckBox::toggled, this, [this, delayEdited](bool automatic) {
        m_delayStepper->setAutomatic(automatic);
        delayEdited();
    });

    // Prediction is authoritative room state. The server repeats the host check
    // so enabling this widget in a modified client cannot grant permission.
    connect(m_predictionStepper, &FrameStepper::edited, this, [this]() {
        if (m_currentRoomId == 0) return;
        m_predictionAuto = m_predictionStepper->isAutomatic();
        if (m_predictionAuto)
            m_predictionStepper->setValue(kDefaultPredictionWindow);
        applyRoomPrediction(true);
    });

    // ── Action bar: leave, why we're waiting, and the one thing to do next ──
    auto* actions = new LobbyCard(this);
    actions->setObjectName("RoomActions");
    auto* actionRow = new QHBoxLayout(actions);
    actionRow->setContentsMargins(12, 10, 12, 10);
    actionRow->setSpacing(GAP_STAGE);

    m_leaveBtn = new LobbyButton(tr("Leave Room"), LobbyButton::Kind::Danger, actions);
    m_leaveBtn->setObjectName("LeaveBtn");
    m_leaveBtn->setButtonSize(LobbyButton::Size::Large);
    m_leaveBtn->setIconName(QStringLiteral("logout-box-r-line"));
    m_leaveBtn->setToolTip(tr("Leave the room and return to the lobby."));
    connect(m_leaveBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onLeaveRoomClicked);
    actionRow->addWidget(m_leaveBtn);

    m_whySpinner = new Spinner(18, actions);
    m_whySpinner->setVisible(false);
    actionRow->addWidget(m_whySpinner);
    m_whyBadge = new IconBadge(20, 10, 13, actions);
    m_whyBadge->setVisible(false);
    actionRow->addWidget(m_whyBadge);
    // One line on why we're waiting; the rest of the story is in its tooltip.
    m_whyTitle = new ElidedLabel(Role::Ink2, actions);
    m_whyTitle->setFont(LobbyTheme::uiFont(12));
    m_whyTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    actionRow->addWidget(m_whyTitle, 1);

    m_lagStatsBtn = new LobbyButton(tr("Lag Stats"), LobbyButton::Kind::Secondary, actions);
    m_lagStatsBtn->setObjectName("LagStatsBtn");
    m_lagStatsBtn->setButtonSize(LobbyButton::Size::Large);
    m_lagStatsBtn->setIconName(QStringLiteral("pulse-line"));
    m_lagStatsBtn->setToolTip(tr("See who is holding the match up: every player's frame rate, frame time, waits and ping. Or type /lagstat in the chat."));
    connect(m_lagStatsBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::showLagStats);
    actionRow->addWidget(m_lagStatsBtn);

    m_dropBtn = new LobbyButton(tr("Drop Game"), LobbyButton::Kind::Danger, actions);
    m_dropBtn->setObjectName("DropBtn");
    m_dropBtn->setEnabled(false);
    m_dropBtn->setVisible(false);
    m_dropBtn->setButtonSize(LobbyButton::Size::Large);
    m_dropBtn->setIconName(QStringLiteral("stop-circle-line"));
    m_dropBtn->setToolTip(tr("Stop playing the current match. If you're the host, the match ends for everyone."));
    connect(m_dropBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onDropGameClicked);
    actionRow->addWidget(m_dropBtn);

    m_startBtn = new LobbyButton(tr("Start Game"), LobbyButton::Kind::Primary, actions);
    m_startBtn->setObjectName("StartGameBtn");
    m_startBtn->setEnabled(false);
    m_startBtn->setButtonSize(LobbyButton::Size::Large);
    m_startBtn->setPixelLabel(true);
    m_startBtn->setIconName(QStringLiteral("play-fill"));
    connect(m_startBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onStartGameClicked);
    actionRow->addWidget(m_startBtn);

    lay->addWidget(actions);

    return page;
}

// ── Port cards ───────────────────────────────────────────────────────

void RollbackLobbyDialog::buildSeatRow(SeatRow& s, int slotIdx, QWidget* parent)
{
    s.slot = slotIdx;
    s.row = new PortCard(slotIdx, parent);

    // Drag grip — only shown to the host while the room is waiting (toggled in
    // onRoomStateChanged). Dragging it onto another seat swaps the two players.
    s.row->dragHandle()->installEventFilter(this);

    connect(s.row->kickButton(), &QToolButton::clicked, this, [this, &s]() {
        if (!m_client || s.userId == 0)
            return;
        if (QMessageBox::question(this, tr("Remove player"),
                QString(tr("Remove %1 from the match?")).arg(s.username.isEmpty() ? tr("this player") : s.username),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        m_client->kickFromRoom(s.userId);
    });

    connect(s.row->admitButton(), &QPushButton::clicked, this, [this, &s]() {
        if (s.userId != 0)
            onAdmitClicked(s.userId);
    });

    s.row->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(s.row, &QWidget::customContextMenuRequested, this, [this, &s](const QPoint& pos) {
        if (s.userId != 0)
            showPlayerMenu(s.userId, s.row->mapToGlobal(pos));
    });
}

void RollbackLobbyDialog::renderSeatEmpty(SeatRow& s)
{
    s.isHost = false;
    s.isSelf = false;
    s.canKick = false;
    s.userId = 0;
    s.peerId = 0;
    s.username.clear();
    s.frameDelay = -1;
    s.iceAttempt = 1;
    s.iceMaxAttempts = 20;

    if (s.row)
    {
        s.row->showOpen(QString());
        s.row->setToolTip(m_currentRoomState == QLatin1String("in_game") && m_roomLateJoin
            ? tr("Anyone who joins can be added")
            : tr("Waiting for a player…"));
    }
}

void RollbackLobbyDialog::renderSeatFilled(SeatRow& s, const QString& username, bool isHost,
                                           bool isSelf, int pingMs, int frameDelay, bool canKick,
                                           const QString& connectionType)
{
    s.isHost = isHost;
    s.isSelf = isSelf;
    s.canKick = canKick;
    s.username = username;
    s.frameDelay = frameDelay;

    s.row->setToolTip(QString());
    s.row->showPlayer(username, isSelf, isHost,
                      m_client && m_client->isLocalPlayer(s.userId) ? tr("This PC") : QString());
    s.row->setConnection(connectionType);
    s.row->setDelay(frameDelay);
    // Self is a local zero-latency endpoint. Remote ping appears after the
    // first reply; until then onRoomStateChanged supplies an ICE status.
    if (isSelf)
        s.row->setLinkLocal();
    else
        s.row->setLinkPing(pingMs);
}

// ── Sidebar ──────────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildSidebar()
{
    auto* wrap = new QWidget(this);
    wrap->setMinimumWidth(220);
    auto* lay = new QVBoxLayout(wrap);
    lay->setContentsMargins(0, MARGIN_STAGE, MARGIN_STAGE, MARGIN_STAGE);
    lay->setSpacing(0);

    m_sideSplitter = new QSplitter(Qt::Vertical, wrap);
    m_sideSplitter->setHandleWidth(GAP_STAGE);
    m_sideSplitter->setChildrenCollapsible(false);
    m_sideSplitter->addWidget(buildPlayersColumn());
    m_sideSplitter->addWidget(buildChatColumn());
    m_sideSplitter->setStretchFactor(0, 0);
    m_sideSplitter->setStretchFactor(1, 1);
    m_sideSplitter->setSizes({ PLAYERS_HEIGHT, 400 });
    {
        QSettings s("RMG-K", "n02");
        const QByteArray splitState = s.value("RollbackLobby/SidebarSplitterState").toByteArray();
        if (!splitState.isEmpty())
            m_sideSplitter->restoreState(splitState);
    }
    lay->addWidget(m_sideSplitter);
    return wrap;
}

QWidget* RollbackLobbyDialog::buildChatColumn()
{
    // One chat surface: Lobby always, Room while seated, Spectators while
    // watching a live replay (chat among that match's viewers, which its
    // players never see).
    auto* card = new LobbyCard(this);
    card->setObjectName("ChatCard");
    card->setMinimumHeight(140);
    card->installEventFilter(this);
    m_chatCard = card;
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(0, 0, 0, 0);
    cardLay->setSpacing(0);

    auto* tabRow = new QFrame(card);
    tabRow->setObjectName("ChatTabRow");
    auto* tabLay = new QHBoxLayout(tabRow);
    tabLay->setContentsMargins(8, 8, 8, 0);
    tabLay->setSpacing(0);
    m_chatTabs = new ChatTabBar(tabRow);
    m_chatTabs->setObjectName("ChatTabs");
    m_chatTabs->setDocumentMode(true);
    m_chatTabs->setDrawBase(false);
    m_chatTabs->setExpanding(false);
    m_chatTabs->setUsesScrollButtons(false);
    m_chatTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_chatTabs->setFocusPolicy(Qt::NoFocus);
    m_chatTabs->setFont(LobbyTheme::pixelFont(10, 0.6));
    m_chatTabs->addTab(tr("Lobby").toUpper());
    m_chatTabs->addTab(tr("Room").toUpper());
    m_chatTabs->addTab(tr("Spectators").toUpper());
    m_chatTabs->addTab(QString());
    m_chatTabs->showTab(kChatTabMail, false);
    m_chatTabs->setTabToolTip(kChatTabMail, tr("Private messages"));
    m_chatTabs->setAccessibleTabName(kChatTabMail, tr("Private messages"));
    m_chatTabs->setTabToolTip(kChatTabLobby, tr("Everyone in the lobby."));
    m_chatTabs->setTabToolTip(kChatTabRoom, tr("Everyone in your room."));
    m_chatTabs->setAccessibleTabName(kChatTabSpectators, tr("Spectators"));
    m_chatTabs->showTab(kChatTabRoom, false);
    m_chatTabs->showTab(kChatTabSpectators, false);
    tabLay->addWidget(m_chatTabs, 1);
    cardLay->addWidget(tabRow);

    // The mail page has its own margins: its header runs edge to edge.
    const auto wrapChatView = [](ChatView* view) {
        auto* wrap = new QWidget;
        auto* wrapLay = new QVBoxLayout(wrap);
        wrapLay->setContentsMargins(10, 6, 4, 2);
        wrapLay->addWidget(view);
        return wrap;
    };
    m_chatStack = new QStackedWidget(card);
    m_chatViewLobby = new ChatView(m_chatStack);
    m_chatViewRoom = new ChatView(m_chatStack);
    m_chatViewSpectate = new ChatView(m_chatStack);
    m_pmPanel = new PrivateMessagesPanel(m_chatStack);
    m_chatStack->insertWidget(kChatTabLobby, wrapChatView(m_chatViewLobby));
    m_chatStack->insertWidget(kChatTabRoom, wrapChatView(m_chatViewRoom));
    auto* spectateWrap = wrapChatView(m_chatViewSpectate);
    m_spectatorStrip = new QWidget(spectateWrap);
    m_spectatorStrip->setObjectName("SpectatorStrip");
    auto* stripLay = new QHBoxLayout(m_spectatorStrip);
    stripLay->setContentsMargins(2, 2, 6, 5);
    stripLay->setSpacing(6);
    m_spectatorEye = new IconLabel(QStringLiteral("eye-line"), Role::Muted, 14, m_spectatorStrip);
    stripLay->addWidget(m_spectatorEye);
    m_spectatorCount = new QLabel(m_spectatorStrip);
    m_spectatorCount->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
    m_spectatorCount->setProperty("role", "muted");
    stripLay->addWidget(m_spectatorCount);
    stripLay->addStretch(1);
    m_spectatorOverlayToggle = new ToggleSwitch(tr("Over the game"), m_spectatorStrip);
    m_spectatorOverlayToggle->setObjectName("SpectatorOverlayToggle");
    m_spectatorOverlayToggle->setFont(LobbyTheme::uiFont(11));
    m_spectatorOverlayToggle->setToolTip(tr("Show spectators' comments over the game while you play."));
    stripLay->addWidget(m_spectatorOverlayToggle);
    m_spectatorStrip->setVisible(false);
    qobject_cast<QVBoxLayout*>(spectateWrap->layout())->insertWidget(0, m_spectatorStrip);
    m_chatStack->insertWidget(kChatTabSpectators, spectateWrap);
    m_chatStack->insertWidget(kChatTabMail, m_pmPanel);
    connectChatView(m_chatViewLobby);
    connectChatView(m_chatViewRoom);
    connectChatView(m_chatViewSpectate);
    cardLay->addWidget(m_chatStack, 1);

    connect(m_chatTabs, &QTabBar::currentChanged, this, [this](int index) {
        m_chatStack->setCurrentIndex(index);
        m_pmPanel->setActive(index == kChatTabMail);
        setChatUnread(index, index == kChatTabMail ? m_pmPanel->unreadTotal() : 0);
        refreshChatInput();
        refreshMentionPopup();
        refreshMentionJump();
    });

    auto* compose = new QFrame(card);
    compose->setObjectName("Compose");
    compose->setFixedHeight(34);
    auto* composeLay = new QHBoxLayout(compose);
    composeLay->setContentsMargins(11, 0, 4, 0);
    composeLay->setSpacing(8);
    m_composeAs = new StatusTag(StatusTag::Kind::Neutral, compose);
    m_composeAs->setVisible(false);
    composeLay->addWidget(m_composeAs);
    m_chatInput = new QLineEdit(compose);
    m_chatInput->setObjectName("ComposeInput");
    m_chatInput->setFont(LobbyTheme::uiFont(12));
    m_chatInput->setEnabled(false);
    m_chatInput->setMaxLength(500); // server truncates at 500; cap client-side too
    m_chatInput->installEventFilter(this);
    connect(m_chatInput, &QLineEdit::returnPressed, this, &RollbackLobbyDialog::onChatSendClicked);
    connect(m_chatInput, &QLineEdit::textChanged, this, [this](const QString&) { refreshMentionPopup(); });
    connect(m_chatInput, &QLineEdit::cursorPositionChanged, this, [this](int, int) { refreshMentionPopup(); });
    composeLay->addWidget(m_chatInput, 1);
    auto* send = new IconButton(QStringLiteral("send-plane-2-line"), 26, compose);
    send->setToolTip(tr("Send"));
    send->setAccessibleName(tr("Send"));
    connect(send, &QToolButton::clicked, this, &RollbackLobbyDialog::onChatSendClicked);
    composeLay->addWidget(send);
    auto* composeWrap = new QVBoxLayout;
    composeWrap->setContentsMargins(8, 5, 8, 8);
    composeWrap->addWidget(compose);
    cardLay->addLayout(composeWrap);

    m_mentionPopup = new MentionPopup(card);
    connect(m_mentionPopup, &MentionPopup::picked, this, &RollbackLobbyDialog::insertMention);
    m_mentionJump = new QPushButton(card);
    m_mentionJump->setObjectName("MentionJump");
    m_mentionJump->setFont(LobbyTheme::uiFont(11, QFont::DemiBold));
    m_mentionJump->setCursor(Qt::PointingHandCursor);
    m_mentionJump->setAutoDefault(false);
    m_mentionJump->setVisible(false);

    connect(m_pmPanel, &PrivateMessagesPanel::currentChanged, this, &RollbackLobbyDialog::refreshChatInput);
    connect(m_pmPanel, &PrivateMessagesPanel::unreadChanged, this, [this]() {
        setChatUnread(kChatTabMail, m_pmPanel->unreadTotal());
    });
    connect(m_pmPanel, &PrivateMessagesPanel::userActivated, this, &RollbackLobbyDialog::showPlayerMenu);
    connect(m_pmPanel, &PrivateMessagesPanel::menuRequested, this, &RollbackLobbyDialog::showPlayerMenu);
    connect(m_pmPanel, &PrivateMessagesPanel::challengeRequested, this, &RollbackLobbyDialog::startChallenge);
    connect(m_pmPanel, &PrivateMessagesPanel::newMessageRequested, this, [this]() {
        m_chatInput->setText(QStringLiteral("/w "));
        m_chatInput->setFocus();
    });

    refreshChatTabIcons();
    refreshChatInput();
    return card;
}

void RollbackLobbyDialog::refreshChatInput()
{
    if (!m_chatInput || !m_chatTabs)
        return;
    const bool connected = m_client && m_client->state() == LobbyClient::ConnectionState::Connected;
    m_chatTabs->showTab(kChatTabMail, connected && m_client->socialSupported());
    const int tab = m_chatTabs->currentIndex();
    if (tab == kChatTabMail)
    {
        const bool open = m_pmPanel->threadOpen();
        const bool asOther = open && !m_pmPanel->openLocalIsOwner() && !m_pmPanel->openRemoteGone();
        m_composeAs->setVisible(asOther);
        if (asOther)
            m_composeAs->setTag(StatusTag::Kind::Neutral, tr("As %1").arg(m_pmPanel->openLocalName()));
        m_chatInput->setPlaceholderText(!open ? tr("Type /w and a name to start a conversation")
            : m_pmPanel->openLocalGone() ? tr("%1 left this PC").arg(m_pmPanel->openLocalName())
            : m_pmPanel->openRemoteGone() ? tr("%1 left the lobby").arg(m_pmPanel->openRemoteName())
            : asOther ? tr("Reply…")
                      : tr("Message %1…").arg(m_pmPanel->openRemoteName()));
        m_chatInput->setEnabled(connected && !m_pmPanel->openRemoteGone());
        return;
    }
    m_composeAs->setVisible(false);
    if (tab == kChatTabSpectators && m_spectatingMatchId == 0)
    {
        m_chatInput->setPlaceholderText(m_playerChatLive ? tr("Message the spectators…") : tr("Closed until your next match"));
        m_chatInput->setEnabled(connected && m_playerChatLive);
        return;
    }
    m_chatInput->setPlaceholderText(tab == kChatTabSpectators
            ? (m_client && m_client->spectatorChatSupported() ? tr("Message the spectators and players…")
                                                              : tr("Message the other spectators…"))
        : tab == kChatTabRoom ? tr("Message the room…")
                              : tr("Message the lobby…"));
    m_chatInput->setEnabled(connected);
}

void RollbackLobbyDialog::setChatUnread(int tab, int count)
{
    if (tab < 0 || tab >= kChatTabCount)
        return;
    m_chatUnread[tab] = count;
    if (count <= 0)
        m_chatMentioned[tab] = false;
    if (count <= 0)
    {
        if (QWidget* old = m_chatTabs->tabButton(tab, QTabBar::RightSide))
        {
            m_chatTabs->setTabButton(tab, QTabBar::RightSide, nullptr);
            old->deleteLater();
        }
        return;
    }

    auto* badge = qobject_cast<QLabel*>(m_chatTabs->tabButton(tab, QTabBar::RightSide));
    const QSize previousSize = badge ? badge->size() : QSize();
    if (badge == nullptr)
    {
        badge = new QLabel(m_chatTabs);
        badge->setObjectName("ChatBadge");
        badge->setFont(LobbyTheme::uiFont(9, QFont::Bold));
        badge->setAlignment(Qt::AlignCenter);
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    badge->setText(m_chatMentioned[tab] ? QStringLiteral("@")
                   : count > 99 ? QStringLiteral("99+") : QString::number(count));
    if (badge->property("mention").toBool() != m_chatMentioned[tab])
    {
        badge->setProperty("mention", m_chatMentioned[tab]);
        badge->style()->unpolish(badge);
        badge->style()->polish(badge);
    }
    badge->adjustSize();
    if (badge->size() != previousSize)
        m_chatTabs->setTabButton(tab, QTabBar::RightSide, badge);
}

void RollbackLobbyDialog::setRoomChatVisible(bool visible)
{
    // Before hiding: Qt would otherwise move on to the next visible tab, not Lobby.
    if (!visible && m_chatTabs->currentIndex() == kChatTabRoom)
        m_chatTabs->setCurrentIndex(kChatTabLobby);
    // First: the word Spectators doesn't fit beside Room.
    refreshSpectatorTab();
    m_chatTabs->showTab(kChatTabRoom, visible);
    setChatUnread(kChatTabRoom, 0);
    m_pendingMentions[kChatTabRoom].clear();
    if (visible)
        m_chatTabs->setCurrentIndex(kChatTabRoom);
    refreshChatInput();
    refreshPlayerSpectatorChat();
}

// ── Players column ───────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildPlayersColumn()
{
    auto* card = new LobbyCard(this);
    card->setObjectName("PlayersCard");
    card->setMinimumHeight(120);
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(0, 0, 0, 0);
    cardLay->setSpacing(0);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(12, 8, 8, 4);
    head->setSpacing(6);
    head->addWidget(new SectionMarker(SectionMarker::Kind::Plain, card));
    head->addWidget(makeSectionLabel(tr("Players"), card));
    m_playersCount = new QLabel(card);
    m_playersCount->setFont(LobbyTheme::uiFont(11, QFont::Bold));
    m_playersCount->setProperty("role", "muted");
    head->addWidget(m_playersCount);
    head->addStretch(1);
    {
        QSettings settings("RMG-K", "n02");
        settings.beginGroup("RollbackLobby/PlayerFilters");
        const int sort = settings.value("Sort", 0).toInt();
        if (sort >= 0 && sort <= static_cast<int>(PlayerSort::Region))
            m_playerSort = static_cast<PlayerSort>(sort);
        m_playerRegionFilter = settings.value("Region").toString();
        m_playerHideWifi = settings.value("HideWifi", false).toBool();
        m_playerMaxPing = std::max(0, settings.value("MaxPing", 0).toInt());
        settings.endGroup();
    }
    m_playersFilterButton = new IconButton(QStringLiteral("filter-3-line"), 26, card);
    m_playersFilterButton->setObjectName("PlayersFilterButton");
    m_playersFilterButton->setToolTip(tr("Sort and filter players"));
    m_playersFilterButton->setAccessibleName(tr("Sort and filter players"));
    connect(m_playersFilterButton, &QToolButton::clicked, this, &RollbackLobbyDialog::showPlayersFilterMenu);
    head->addWidget(m_playersFilterButton);
    auto* search = new IconButton(QStringLiteral("search-line"), 26, card);
    search->setCheckable(true);
    search->setToolTip(tr("Find a player"));
    search->setAccessibleName(tr("Find a player"));
    head->addWidget(search);
    cardLay->addLayout(head);

    m_playersSearch = new QLineEdit(card);
    m_playersSearch->setObjectName("PlayersSearch");
    m_playersSearch->setFont(LobbyTheme::uiFont(12));
    m_playersSearch->setPlaceholderText(tr("Player name…"));
    m_playersSearch->setClearButtonEnabled(true);
    m_playersSearch->setVisible(false);
    auto* searchWrap = new QVBoxLayout;
    searchWrap->setContentsMargins(8, 0, 8, 0);
    searchWrap->addWidget(m_playersSearch);
    cardLay->addLayout(searchWrap);
    connect(m_playersSearch, &QLineEdit::textChanged, this, [this]() { refreshPlayerGroups(); });
    connect(search, &QToolButton::toggled, this, [this, searchWrap](bool on) {
        m_playersSearch->setVisible(on);
        searchWrap->setContentsMargins(8, 0, 8, on ? 4 : 0);
        if (on)
            m_playersSearch->setFocus();
        else
            m_playersSearch->clear();
    });

    const auto makePlayersList = [card]() {
        auto* list = new QTreeWidget(card);
        list->setObjectName("PlayersTree");
        list->setColumnCount(1);
        list->setHeaderHidden(true);
        list->setRootIsDecorated(false);
        list->setIndentation(0);
        list->setItemsExpandable(false);
        list->setFrameShape(QFrame::NoFrame);
        list->setFocusPolicy(Qt::NoFocus);
        list->setMouseTracking(true);
        list->setUniformRowHeights(false);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->viewport()->setAutoFillBackground(false);
        list->setItemDelegate(new PlayersItemDelegate(list));
        return list;
    };
    m_playersTree = makePlayersList();
    auto* rowButtons = static_cast<PlayersItemDelegate*>(m_playersTree->itemDelegate());
    rowButtons->onChallenge = [this](quint64 userId) { startChallenge(userId); };
    rowButtons->onMessage = [this](quint64 userId) { openPrivateMessage(userId); };
    m_playersTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_playersTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_playersTree->itemAt(pos);
        if (item && !item->data(0, kGroupRole).toBool())
            showPlayerMenu(item->data(0, Qt::UserRole).toULongLong(), m_playersTree->viewport()->mapToGlobal(pos));
    });
    connect(m_playersTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->data(0, kSocialRole).toBool())
            openPrivateMessage(item->data(0, Qt::UserRole).toULongLong());
    });

    // Your own row stays in view above the list, with this PC's other players.
    m_selfRowBox = new QFrame(card);
    m_selfRowBox->setObjectName("SelfPlayerRow");
    auto* selfLayout = new QHBoxLayout(m_selfRowBox);
    // The 1 px frame and 2 px margins put the button's 4 px corners
    // 3 px inside the row's 7 px corners.
    selfLayout->setContentsMargins(1, 2, 2, 2);
    selfLayout->setSpacing(0);
    m_selfRow = makePlayersList();
    m_selfRow->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_selfRow->setFixedHeight(24);
    selfLayout->addWidget(m_selfRow, 1);
    m_rosterAdd = new LobbyButton(tr("Add local player"), LobbyButton::Kind::Ghost, m_selfRowBox);
    m_rosterAdd->setObjectName("AddLocalPlayerBtn");
    m_rosterAdd->setButtonSize(LobbyButton::Size::Compact);
    m_rosterAdd->setIconName(QStringLiteral("user-add-line"));
    selfLayout->addWidget(m_rosterAdd);
    connect(m_rosterAdd, &QPushButton::clicked, this, &RollbackLobbyDialog::openRosterEditor);
    m_selfRowBox->hide();

    // One heading per state, in the order people look for them. Each hides
    // itself while empty.
    const QStringList titles = { tr("Searching"), tr("Playing"), tr("Watching"), tr("In rooms"), tr("Online") };
    for (const QString& title : titles)
    {
        auto* group = new QTreeWidgetItem(m_playersTree);
        group->setText(0, title);
        group->setData(0, kGroupRole, true);
        group->setFlags(Qt::ItemIsEnabled);
        group->setHidden(true);
        group->setExpanded(true);
        m_playerGroups.append(group);
    }

    // Selecting a player measures them on demand and refreshes their tooltip.
    connect(m_playersTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item && !item->data(0, kGroupRole).toBool())
                    probeOnDemand(item->data(0, Qt::UserRole).toULongLong());
            });
    // Click in the empty area below the rows to clear the selection (in
    // eventFilter — there's no built-in property for it).
    m_playersTree->viewport()->installEventFilter(this);

    buildLocalRoster();

    auto* treeWrap = new QVBoxLayout;
    treeWrap->setContentsMargins(5, 0, 5, 5);
    treeWrap->setSpacing(0);
    treeWrap->addWidget(m_selfRowBox);
    treeWrap->addWidget(m_rosterBox);
    treeWrap->addWidget(m_playersTree, 1);
    cardLay->addLayout(treeWrap, 1);

    return card;
}

// ── Theme ────────────────────────────────────────────────────────────

void RollbackLobbyDialog::applyStylesheet()
{
    m_appliedDarkTheme = isDarkTheme();
    setStyleSheet(LobbyTheme::styleSheet());
    refreshChatTabIcons();
}

void RollbackLobbyDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // The theme can change in Settings while this dialog is alive. Painted
    // widgets pick the new colours up on their next paint.
    if (event->type() == QEvent::ApplicationPaletteChange && m_appliedDarkTheme != isDarkTheme())
        refreshTheme();
}

void RollbackLobbyDialog::refreshTheme()
{
    applyStylesheet();
    if (m_joinPingCombo)
    {
        // Its signal bar icons were drawn in the old colours.
        const QSignalBlocker blocker(m_joinPingCombo);
        m_joinPingCombo->clear();
        CreateRoomDialog::fillPingLimitCombo(m_joinPingCombo);
        updateJoinLimitControls();
    }
    updateRollbackSettingControls();
    updateStatusIndicator(m_client->state());
    // The role line under the name is coloured as it's set.
    updateUserChip();
    onRoomListChanged();
    update();
}

void RollbackLobbyDialog::setLobbyTheme(LobbyTheme::ThemeChoice choice)
{
    LobbyTheme::setThemeChoice(choice);
    QSettings("RMG-K", "n02").setValue(kLobbyThemeSetting,
        choice == LobbyTheme::ThemeChoice::Dark    ? QStringLiteral("dark")
        : choice == LobbyTheme::ThemeChoice::Light ? QStringLiteral("light")
                                                   : QStringLiteral("app"));
    if (m_appliedDarkTheme != isDarkTheme())
        refreshTheme();
}

// Live beside the open rooms, or above them. Each way keeps its own divider.
void RollbackLobbyDialog::setBrowseStacked(bool stacked)
{
    const Qt::Orientation orientation = stacked ? Qt::Vertical : Qt::Horizontal;
    if (!m_browseSplitter || m_browseSplitter->orientation() == orientation)
        return;
    QSettings s("RMG-K", "n02");
    s.setValue(browseSplitterKey(), m_browseSplitter->saveState());
    m_browseSplitter->setOrientation(orientation);
    restoreBrowseSplit();
    s.setValue(kBrowseLayoutSetting, stacked ? QStringLiteral("stacked") : QStringLiteral("side"));
}

// The divider where the player left it in this layout. A saved state carries
// its own orientation, and the layout wins: one that disagrees is dropped.
void RollbackLobbyDialog::restoreBrowseSplit()
{
    const Qt::Orientation orientation = m_browseSplitter->orientation();
    const int handleWidth = m_browseSplitter->handleWidth();
    const QByteArray state = QSettings("RMG-K", "n02").value(browseSplitterKey()).toByteArray();
    if (state.isEmpty() || !m_browseSplitter->restoreState(state) || m_browseSplitter->orientation() != orientation)
    {
        // A rejected state has already set its own handle width and collapsing.
        m_browseSplitter->setOrientation(orientation);
        m_browseSplitter->setHandleWidth(handleWidth);
        m_browseSplitter->setChildrenCollapsible(false);
        if (orientation == Qt::Horizontal)
            m_browseSplitter->setSizes({ 340, 500 });
        else
            m_browseSplitter->setSizes({ 1, 1 });
    }
}

QString RollbackLobbyDialog::browseSplitterKey() const
{
    // Each layout has its own, as the saved state holds the orientation. Not
    // "BrowseSplitterState": the classic lobby keeps its rooms / matches
    // splitter there, and both read the same settings.
    return m_browseSplitter && m_browseSplitter->orientation() == Qt::Vertical
        ? QStringLiteral("RollbackLobby/LiveRoomsStacked")
        : QStringLiteral("RollbackLobby/LiveRoomsSideBySide");
}

// ──────────────────────────────────────────────────────────────────────
//  ROM library
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::setRomLibrary(const QMap<QString, CoreRomSettings>& roms)
{
    m_roms = roms;
    m_coverPaths.clear();
    populateBrowseRoms();
    // Which rooms and matches this client has the ROM for may have changed.
    onRoomListChanged();
}

void RollbackLobbyDialog::populateBrowseRoms()
{
    if (!m_browseRomCombo) return;

    m_browseRomCombo->hidePopup();
    m_browseRomCombo->blockSignals(true);
    m_browseRomCombo->clear();

    if (m_roms.isEmpty())
    {
        m_browseRomCombo->addItem(tr("No ROMs in your library — add some first"));
        m_browseRomCombo->setEnabled(false);
        m_browseRomCombo->blockSignals(false);
        refreshSameGameFilter();
        return;
    }
    m_browseRomCombo->setEnabled(true);

    // Sort by display name (file path as tiebreaker), matching CreateRoomDialog
    // so the names line up and the Create Room pre-select resolves.
    QMap<QString, QString> sortedByName; // "<name>\x01<file>" → file
    for (auto it = m_roms.constBegin(); it != m_roms.constEnd(); ++it)
    {
        const QString display = CreateRoomDialog::displayGameName(
            QString::fromStdString(it.value().GoodName), it.key());
        sortedByName.insert(display + "\x01" + it.key(), it.key());
    }
    for (auto it = sortedByName.constBegin(); it != sortedByName.constEnd(); ++it)
    {
        const QString file = it.value();
        const QString display = CreateRoomDialog::displayGameName(
            QString::fromStdString(m_roms.value(file).GoodName), file);
        // Store the ROM's name + MD5 as item data so Quick Match can scope the
        // search to the selected game (matches the shape Create Room sends).
        QVariantMap romData;
        romData["name"] = display;
        romData["md5"]  = QString::fromStdString(m_roms.value(file).MD5);
        romData["file"] = file;
        m_browseRomCombo->addItem(display, romData);
        // Index the title and filename as separate strings within the filter role.
        m_browseRomCombo->setItemData(m_browseRomCombo->count() - 1,
            browseGameSearchText(display) + u'\n' + browseGameSearchText(QFileInfo(file).fileName()),
            kBrowseGameSearchRole);
    }

    // Restore the last-used selection (shared with Create Room via QSettings).
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    const QString preferred = s.value("Rom").toString();
    s.endGroup();
    if (!preferred.isEmpty())
    {
        const int idx = m_browseRomCombo->findText(preferred);
        if (idx >= 0) m_browseRomCombo->setCurrentIndex(idx);
    }

    m_browseRomCombo->blockSignals(false);
    refreshSameGameFilter();
}

QVariantMap RollbackLobbyDialog::selectedBrowseRom() const
{
    if (!m_browseRomCombo || !m_browseRomCombo->isEnabled())
        return {};

    return m_browseRomCombo->currentData().toMap();
}

void RollbackLobbyDialog::refreshSameGameFilter()
{
    const QVariantMap rom = selectedBrowseRom();
    if (m_browseRomCombo)
    {
        const QString name = m_browseRomCombo->currentText();
        m_pickerName->setText(name);
        m_pickerName->setRole(rom.isEmpty() ? Role::Muted : Role::Ink);
        m_browseRomCombo->setToolTip(name);
        m_pickerTile->setGame(name, coverPathForMd5(rom.value("md5").toString()));
    }
    if (!m_liveGallery || !m_roomsColumn)
        return;
    m_liveGallery->setThisGameAvailable(!rom.isEmpty());
    m_roomsColumn->setThisGameAvailable(!rom.isEmpty());
    onRoomListChanged();
}

// Local cover art for a game in the library, found the way the ROM browser
// finds it: by file name, MD5 or good name, in the user's Covers directory.
QString RollbackLobbyDialog::coverPathForMd5(const QString& md5)
{
    if (md5.isEmpty())
        return QString();
    const auto known = m_coverPaths.constFind(md5);
    if (known != m_coverPaths.constEnd())
        return known.value();

    QString found;
    const QString file = localRomPathForMd5(md5);
    if (!file.isEmpty())
    {
        const QString directory = QString::fromStdString(CoreGetUserDataDirectory().string())
            + CORE_DIR_SEPERATOR_STR + QStringLiteral("Covers");
        const QStringList names = { QFileInfo(file).completeBaseName(), md5,
                                    QString::fromStdString(m_roms.value(file).GoodName) };
        for (QString name : names)
        {
            for (const QChar c : QString(":<>\"/\\|?*"))
                name.replace(c, "_");
            if (name.isEmpty())
                continue;
            for (const char* ext : { ".png", ".jpg", ".jpeg" })
            {
                const QString path = directory + CORE_DIR_SEPERATOR_STR + name + QLatin1String(ext);
                if (QFile::exists(path))
                {
                    found = path;
                    break;
                }
            }
            if (!found.isEmpty())
                break;
        }
    }
    m_coverPaths.insert(md5, found);
    return found;
}

// ──────────────────────────────────────────────────────────────────────
//  Show / close
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!matchTransportInProgress() &&
        (m_client->state() == LobbyClient::ConnectionState::Disconnected ||
         m_client->state() == LobbyClient::ConnectionState::Failed))
    {
        QTimer::singleShot(0, this, [this]() {
            if (!isVisible() || m_connectPromptOpen ||
                m_client->state() == LobbyClient::ConnectionState::Connected)
                return;
            const QString message = m_connectPromptMessage;
            m_connectPromptMessage.clear();
            promptForUsername(message);
        });
    }
}

void RollbackLobbyDialog::closeEvent(QCloseEvent* event)
{
    if (m_client)
        m_client->disconnectFromServer();
    QDialog::closeEvent(event);
}

bool RollbackLobbyDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (socialEventFilter(watched, event))
        return true;

    // Enter and Escape in the roster's name box stop here: the dialog would press
    // its default button for one and close the lobby for the other.
    if (m_rosterName && watched == m_rosterName && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
        {
            submitRosterEditor();
            return true;
        }
        if (key == Qt::Key_Escape)
        {
            closeRosterEditor();
            return true;
        }
    }

    if (m_playersTree && watched == m_playersTree->viewport())
    {
        // A click in the players-list viewport that doesn't land on a row clears
        // the selection, so a highlighted name can be dismissed by clicking off it.
        if (event->type() == QEvent::MouseButtonPress)
        {
            auto* me = static_cast<QMouseEvent*>(event);
            if (!m_playersTree->itemAt(me->position().toPoint()))
                m_playersTree->clearSelection();
        }
        return QDialog::eventFilter(watched, event);
    }

    // ── Seat reorder: start a drag from a seat's grip handle ──
    if (m_canReorderSeats)
    {
        for (auto& s : m_seats)
        {
            if (!s.row || watched != s.row->dragHandle())
                continue;
            if (event->type() == QEvent::MouseButtonPress)
            {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton && s.userId != 0)
                {
                    m_seatDragStartPos = me->globalPosition().toPoint();
                    m_seatDragSlot     = s.slot;
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease)
            {
                m_seatDragSlot = 0;
                return true;
            }
            if (event->type() == QEvent::MouseMove)
            {
                auto* me = static_cast<QMouseEvent*>(event);
                if ((me->buttons() & Qt::LeftButton) && m_seatDragSlot != 0 &&
                    (me->globalPosition().toPoint() - m_seatDragStartPos).manhattanLength()
                        >= QApplication::startDragDistance())
                {
                    const int slot = m_seatDragSlot;
                    m_seatDragSlot = 0;
                    beginSeatDrag(slot, s.row, m_seatDragStartPos);
                    moveSeatDrag(me->globalPosition().toPoint());
                }
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, event);
}

void RollbackLobbyDialog::beginSeatDrag(int slot, PortCard* card, const QPoint& globalPos)
{
    if (slot < 1 || !card || m_seatDraggingSlot != 0)
        return;

    // The card a little smaller, so the ring on the seat under it shows around
    // it. It shrinks toward the grab point, which stays under the pointer.
    constexpr qreal kScale = 0.94;
    const QPixmap shot = card->grab();
    const qreal dpr = shot.devicePixelRatio();
    QPixmap lifted((QSizeF(shot.size()) * kScale).toSize());
    lifted.setDevicePixelRatio(dpr);
    lifted.fill(Qt::transparent);
    {
        QPainter painter(&lifted);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(QRectF(QPointF(), QSizeF(lifted.size()) / dpr), shot, QRectF(shot.rect()));
    }
    m_seatGhost = new QLabel(m_roomWindow);
    m_seatGhost->setObjectName("SeatDragGhost");
    m_seatGhost->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_seatGhost->setPixmap(lifted);
    m_seatGhost->resize((QSizeF(lifted.size()) / dpr).toSize());
    auto* shadow = new QGraphicsDropShadowEffect(m_seatGhost);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(0, 0, 0, isDarkTheme() ? 150 : 70));
    m_seatGhost->setGraphicsEffect(shadow);
    m_seatGhostGrab = (QPointF(card->mapFromGlobal(globalPos)) * kScale).toPoint();

    m_seatDraggingSlot = slot;
    card->setLifted(true);
    m_seatDragTracker = new SeatDragTracker(
        [this](const QPoint& pos) { moveSeatDrag(pos); },
        [this](const QPoint& pos, bool drop) { endSeatDrag(pos, drop); }, this);
    qApp->installEventFilter(m_seatDragTracker);
    QApplication::setOverrideCursor(Qt::ClosedHandCursor);
    moveSeatDrag(globalPos);
    m_seatGhost->show();
    m_seatGhost->raise();
}

void RollbackLobbyDialog::moveSeatDrag(const QPoint& globalPos)
{
    if (!m_seatGhost)
        return;
    m_seatGhost->move(m_roomWindow->mapFromGlobal(globalPos) - m_seatGhostGrab);
    int target = seatSlotAtPos(m_seatsBox->mapFromGlobal(globalPos));
    if (target == m_seatDraggingSlot)
        target = 0;
    if (target == m_seatDropSlot)
        return;
    m_seatDropSlot = target;
    for (auto& s : m_seats)
    {
        if (s.row)
            s.row->setDropTarget(s.slot == target);
    }
}

void RollbackLobbyDialog::endSeatDrag(const QPoint& globalPos, bool drop)
{
    if (m_seatDraggingSlot == 0)
        return;
    const int from = m_seatDraggingSlot;
    const int to = drop ? seatSlotAtPos(m_seatsBox->mapFromGlobal(globalPos)) : 0;
    m_seatDraggingSlot = 0;
    m_seatDropSlot = 0;

    // This can run inside the tracker's own filter, so it's deleted later.
    qApp->removeEventFilter(m_seatDragTracker);
    m_seatDragTracker->deleteLater();
    m_seatDragTracker = nullptr;
    QApplication::restoreOverrideCursor();
    m_seatGhost->hide();
    m_seatGhost->deleteLater();
    m_seatGhost = nullptr;
    for (auto& s : m_seats)
    {
        if (!s.row)
            continue;
        s.row->setLifted(false);
        s.row->setDropTarget(false);
    }

    if (m_client && m_canReorderSeats && to >= 1 && to != from)
        m_client->swapSeats(from, to);
}

int RollbackLobbyDialog::seatSlotAtPos(const QPoint& pos) const
{
    // pos is in m_seatsBox coordinates (the drop target). Seat cards are its
    // direct children, so their geometry() is in the same space.
    for (const auto& s : m_seats)
    {
        if (s.row && s.row->isVisible() && s.row->geometry().contains(pos))
            return s.slot;
    }
    return 0;
}

QString RollbackLobbyDialog::localRomPathForMd5(const QString& md5) const
{
    if (md5.isEmpty())
        return QString();
    for (auto it = m_roms.constBegin(); it != m_roms.constEnd(); ++it)
    {
        if (QString::fromStdString(it.value().MD5).compare(md5, Qt::CaseInsensitive) == 0)
            return it.key();
    }
    return QString();
}

QString RollbackLobbyDialog::prefillUsername() const
{
    // Prefer a previously saved lobby name, then the Kaillera username, then the
    // OS username. Conform whatever we pick to the lobby's allowed charset/length.
    QSettings s("RMG-K", "n02");
    QString name = s.value("Lobby/Username").toString().trimmed();

    if (name.isEmpty())
    {
        const QString kaillera = QString::fromStdString(
            CoreSettingsGetStringValue(SettingsID::Kaillera_Username)).trimmed();
        if (!kaillera.isEmpty() && kaillera != "Player")
        {
            name = kaillera;
        }
    }

    if (name.isEmpty())
    {
        QByteArray envUser = qgetenv("USER");
        if (envUser.isEmpty())
        {
            envUser = qgetenv("USERNAME");
        }
        name = QString::fromUtf8(envUser).trimmed();
    }

    return LobbyConnectDialog::fitUsername(name);
}

void RollbackLobbyDialog::promptForUsername(const QString& statusMessage)
{
    if (m_connectPromptOpen || !isVisible() || matchTransportInProgress() ||
        m_client->state() == LobbyClient::ConnectionState::Connected)
        return;

    m_connectPromptOpen = true;
    LobbyConnectDialog prompt(this);
    prompt.setUsername(prefillUsername());
    prompt.setStatusMessage(statusMessage);
    const int result = prompt.exec();
    m_connectPromptOpen = false;

    if (result != QDialog::Accepted)
    {
        // A new name the lobby refused: cancelling goes back to the old one.
        if (!m_renameFallbackUsername.isEmpty())
        {
            const QString previous = m_renameFallbackUsername;
            m_renameFallbackUsername.clear();
            QSettings("RMG-K", "n02").setValue("Lobby/Username", previous);
            connectAs(previous, m_serverUrl);
            return;
        }
        close();
        return;
    }
    connectAs(prompt.username(), prompt.serverUrl());
}

void RollbackLobbyDialog::changeUsername()
{
    if (matchTransportInProgress() || m_client->state() != LobbyClient::ConnectionState::Connected)
        return;
    LobbyConnectDialog prompt(this);
    prompt.setRenameMode();
    prompt.setUsername(m_username);
    if (m_currentRoomId != 0)
        prompt.setStatusMessage(tr("Changing your name reconnects you, so you'll leave your room."));
    m_connectPromptOpen = true;
    const int result = prompt.exec();
    m_connectPromptOpen = false;
    // The connection dropped while the prompt was open, so the usual prompt
    // never came: this one stands in for it.
    const LobbyClient::ConnectionState state = m_client->state();
    if (state == LobbyClient::ConnectionState::Disconnected || state == LobbyClient::ConnectionState::Failed)
    {
        if (result == QDialog::Accepted)
            connectAs(prompt.username(), m_serverUrl);
        else
            promptForUsername(m_connectPromptMessage);
        return;
    }
    // Cancel, or the same name: nothing changes, and you stay connected.
    if (result != QDialog::Accepted || prompt.username() == m_username ||
        state != LobbyClient::ConnectionState::Connected)
        return;
    m_renameUsername = prompt.username();
    m_renameFallbackUsername = m_username;
    m_client->disconnectFromServer(); // onClientStateChanged reconnects under the new name
}

void RollbackLobbyDialog::connectAs(const QString& username, const QString& serverUrl)
{
    m_username  = username;
    m_serverUrl = serverUrl;

    // A new WebSocket session has no claim on a room from the previous server
    // session. This is normally already cleared by the disconnect handler, but
    // doing it here as well makes every reconnect start from a clean lobby.
    clearServerRoomSnapshot();
    updateServerMeta();
    // The server compares libraries so a challenge can offer games both players have.
    // The lobby reads no more than this, and a much longer HELLO wouldn't fit in a message.
    constexpr int maxRomHashes = 20'000;
    QStringList romHashes;
    for (const CoreRomSettings& rom : m_roms)
    {
        if (romHashes.size() == maxRomHashes)
            break;
        romHashes << QString::fromStdString(rom.MD5);
    }
    m_client->connectToServer(m_serverUrl, m_username, romHashes, QString());
}

bool RollbackLobbyDialog::matchTransportInProgress() const
{
    return m_emulationActive || m_awaitingEmulationStart ||
           (m_client && m_client->matchTransportActive());
}

void RollbackLobbyDialog::showMatchConnectionLostNotice()
{
    const QString notice = tr("Error: lost connection to server. You can continue the game but without chat.");
    if (m_connectPromptMessage.isEmpty())
        m_connectPromptMessage = notice;

    if (!m_matchConnectionLostNoticeShown)
    {
        appendChatSystemLine(CHANNEL_ROOM, notice);
        emit matchServerConnectionLost(notice);
        m_matchConnectionLostNoticeShown = true;
    }

    applyRoomStateBadge(tr("Lobby offline — game continues"),
                        QString(statusColors().fail));

    // Broadcast and spectator traffic use the WebSocket, unlike gameplay.
    // Detach the recording tee and stop attempting uploads while offline.
    stopBroadcast();
}

// ──────────────────────────────────────────────────────────────────────
//  Connection events
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onClientStateChanged(LobbyClient::ConnectionState s)
{
    updateStatusIndicator(s);

    const bool connected = (s == LobbyClient::ConnectionState::Connected);
    if (connected)
        m_matchConnectionLostNoticeShown = false;
    // Ping probing runs for the whole connection lifetime — it feeds the
    // players-list hover tooltips in the lobby, not just seated peers.
    if (m_pingProbeTimer)
    {
        if (connected && !m_pingProbeTimer->isActive())
            m_pingProbeTimer->start();
        else if (!connected)
            m_pingProbeTimer->stop();
    }
    refreshChatInput();
    refreshLocalRoster();
    if (m_quickMatchBtn)
        m_quickMatchBtn->setEnabled(connected && m_currentRoomId == 0);
    if (m_createRoomBtn)
        m_createRoomBtn->setEnabled(connected && m_currentRoomId == 0);

    if (s == LobbyClient::ConnectionState::Disconnected ||
        s == LobbyClient::ConnectionState::Failed)
    {
        emit liveReplayViewerCountCleared();
        // A live replay arrives over this connection, so watching can't outlive
        // it. End the watch (closing the lobby window lands here too) instead of
        // leaving playback frozen, waiting for data that will never come. The
        // server drops a disconnected viewer itself.
        if (m_spectatingMatchId != 0)
        {
            m_spectatingMatchId = 0;
            m_spectateStreamArmed = false;
            setSpectatorChatVisible(false);
            emit spectateStreamClosed(QStringLiteral("disconnected"));
        }
        clearPlayerRows();
        m_quickMatchActive = false;
        resetSocialState();

        if (m_chatViewLobby) m_chatViewLobby->clear();
        if (matchTransportInProgress())
        {
            // Only the active match and its established ICE agents survive a
            // lobby outage. The room identity and roster are server-owned and
            // are already stale, so remove them from the UI immediately.
            showMatchConnectionLostNotice();
            clearServerRoomSnapshot();
        }
        else
        {
            m_currentMatchId = 0;
            m_awaitingEmulationStart = false;
            m_emulationActive = false;
            endLiveRollbackSettings();
            clearServerRoomSnapshot();
        }
    }
    updateServerMeta();
    updateUserChip();
    m_liveGallery->setOffline(!connected);
    m_roomsColumn->setOffline(!connected);
    onRoomListChanged();

    if (connected)
        m_renameFallbackUsername.clear();

    // Changing your name: straight back in under the new one.
    if (s == LobbyClient::ConnectionState::Disconnected && !m_renameUsername.isEmpty())
    {
        const QString name = m_renameUsername;
        m_renameUsername.clear();
        QTimer::singleShot(0, this, [this, name]() { connectAs(name, m_serverUrl); });
        return;
    }

    // A normal disconnect returns to the username prompt. Failed connections
    // are reopened by the error slots below so the server's message is retained.
    if (s == LobbyClient::ConnectionState::Disconnected && isVisible() &&
        !matchTransportInProgress() &&
        !m_connectPromptOpen)
    {
        QTimer::singleShot(0, this, [this]() {
            if (!isVisible() || m_connectPromptOpen || matchTransportInProgress() ||
                m_client->state() == LobbyClient::ConnectionState::Connected)
                return;
            const QString message = m_connectPromptMessage;
            m_connectPromptMessage.clear();
            promptForUsername(message);
        });
    }
}

void RollbackLobbyDialog::onHelloFailed(const QString& reason)
{
    QString human = reason;
    if (reason == "username_taken")    human = tr("That username is already in use, or looks too much like one that is.");
    else if (reason == "invalid_name") human = tr("That name can't be used. Names are 2 to 20 letters, numbers, or _ - . "
                                                  "in one alphabet, and at most 10 Japanese, Chinese or Korean characters.");
    else if (reason == "invalid_hello") human = tr("Server rejected the connection handshake.");
    else if (reason == "invalid_payload") human = tr("That username isn't allowed.");
    else if (reason == "version_mismatch") human =
        tr("Your Dale64 client is out of date. This lobby requires version 0.9.13 "
        "or vdev-2367 or newer.");
    else if (reason == "server_full") human = tr("The lobby is currently full.");

    m_connectPromptMessage = human;
    if (matchTransportInProgress())
    {
        showMatchConnectionLostNotice();
        return;
    }
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible() || m_connectPromptOpen || matchTransportInProgress())
            return;
        const QString message = m_connectPromptMessage;
        m_connectPromptMessage.clear();
        promptForUsername(message);
    });
}

void RollbackLobbyDialog::onConnectError(const QString& msg)
{
    m_connectPromptMessage = tr("Couldn't reach the lobby: %1").arg(msg);
    if (matchTransportInProgress())
    {
        showMatchConnectionLostNotice();
        return;
    }
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible() || m_connectPromptOpen || matchTransportInProgress())
            return;
        const QString message = m_connectPromptMessage;
        m_connectPromptMessage.clear();
        promptForUsername(message);
    });
}

void RollbackLobbyDialog::updateStatusIndicator(LobbyClient::ConnectionState s)
{
    const QString sc = statusColor(s);
    if (m_statusLed)
        m_statusLed->setColor(QColor(sc));
    if (m_statusText)
    {
        m_statusText->setText(humanState(s).toUpper());
        m_statusText->setStyleSheet(QString("color: %1;").arg(sc));
    }
}

void RollbackLobbyDialog::applyRoomStateBadge(const QString& text, const QString& colorHex)
{
    m_roomBadgeText = text;
    m_roomBadgeColor = colorHex;
    refreshRoomBadge();
}

// A running match's pill also says how long it's been going.
void RollbackLobbyDialog::refreshRoomBadge()
{
    if (!m_roomStatePill)
        return;
    QString text = m_roomBadgeText;
    if (text == roomStateLabel(QStringLiteral("in_game")) && m_client)
    {
        const auto room = m_client->rooms().constFind(m_currentRoomId);
        if (room != m_client->rooms().constEnd() && room->startedAtMs > 0)
            text += QStringLiteral(" · ") + formatMatchDuration(room->startedAtMs);
    }
    m_roomStatePill->setState(text, QColor(m_roomBadgeColor));
}

// The play bar holds one height through every state: choosing a game, searching,
// watching, or seated in a room while browsing. Nothing below it ever moves.
void RollbackLobbyDialog::refreshPlayBar()
{
    if (!m_playBar || !m_client)
        return;

    const Tokens& t = LobbyTheme::tokens();
    const auto setValues = [this](const QString& firstValue, const QString& firstCaption,
                                  const QString& secondValue, const QString& secondCaption) {
        m_ticketValues[0]->setText(firstValue);
        m_ticketCaptions[0]->setText(firstCaption.toUpper());
        m_ticketValues[1]->setText(secondValue);
        m_ticketCaptions[1]->setText(secondCaption.toUpper());
        m_ticketValueBoxes[0]->setVisible(!firstValue.isEmpty());
        m_ticketValueBoxes[1]->setVisible(!secondValue.isEmpty());
    };
    const auto setTicket = [&](const QString& title, const QColor& color, const QString& lead, const QString& rest) {
        m_playBarStack->setCurrentIndex(1);
        m_ticketTitle->setText(title.toUpper());
        // This runs every second while a ticket shows; a stylesheet isn't free.
        const QString titleStyle = QString("color: %1;").arg(color.name());
        if (m_ticketTitle->styleSheet() != titleStyle)
            m_ticketTitle->setStyleSheet(titleStyle);
        m_ticketSub->setLead(lead);
        m_ticketSub->setText(rest);
        m_ticketSub->setToolTip(lead + rest);
        m_playBar->setBorderColor(LobbyTheme::mix(color, t.line, 0.45));
    };

    m_ticketLeave->setVisible(m_currentRoomId != 0);
    if (m_currentRoomId != 0)
    {
        QString name = tr("Your room");
        QString seats;
        const auto room = m_client->rooms().constFind(m_currentRoomId);
        if (room != m_client->rooms().constEnd())
        {
            if (!room->name.isEmpty())
                name = room->name;
            if (room->maxPlayers > 0)
                seats = tr(" · %1 of %2 seats").arg(room->players).arg(room->maxPlayers);
        }
        setTicket(tr("You're in a room"), t.ok, name, seats);
        setValues(QString(), QString(), QString(), QString());
        m_ticketSpinner->setVisible(false);
        m_ticketBadge->setVisible(true);
        m_ticketBadge->setIconAndColors(QStringLiteral("home-4-line"), t.ok, t.okTint);
        m_ticketAction->setText(tr("Return to room"));
        m_ticketAction->setKind(LobbyButton::Kind::Primary);
        m_ticketAction->setIconName(QStringLiteral("arrow-right-line"));
    }
    else if (m_outgoingChallenge.id != 0)
    {
        setTicket(tr("Challenge sent"), t.accentInk, tr("Waiting for %1").arg(m_outgoingChallenge.toUsername),
                  m_outgoingChallenge.romName.isEmpty() ? QString()
                      : QStringLiteral(" · ") + m_outgoingChallenge.romName);
        const qint64 left = m_outgoingChallenge.expiresAtMs - QDateTime::currentMSecsSinceEpoch();
        setValues(QString::number(std::max<qint64>(0, (left + 999) / 1000)), tr("seconds"), QString(), QString());
        m_ticketBadge->setIconAndColors(QString(), t.accentInk, t.accentTint);
        m_ticketBadge->setVisible(true);
        m_ticketSpinner->setColors(t.accentTint, t.accentInk);
        m_ticketSpinner->setVisible(true);
        m_ticketAction->setText(tr("Cancel"));
        m_ticketAction->setKind(LobbyButton::Kind::Secondary);
        m_ticketAction->setIconName(QStringLiteral("close-line"));
    }
    else if (!m_challengeOutcome.title.isEmpty())
    {
        setTicket(m_challengeOutcome.title, m_challengeOutcome.color, QString(), m_challengeOutcome.detail);
        setValues(QString(), QString(), QString(), QString());
        m_ticketSpinner->setVisible(false);
        m_ticketBadge->setVisible(true);
        m_ticketBadge->setIconAndColors(m_challengeOutcome.icon, m_challengeOutcome.color,
                                        LobbyTheme::mix(m_challengeOutcome.color, t.tint, 0.13));
        m_ticketAction->setText(tr("OK"));
        m_ticketAction->setKind(LobbyButton::Kind::Secondary);
        m_ticketAction->setIconName(QString());
    }
    else if (m_quickMatchActive)
    {
        setTicket(tr("Searching…"), t.warn, m_searchingGame, QString());
        setValues(formatMatchDuration(m_searchStartedMs), tr("elapsed"),
                  QString::number(std::max(1, m_searchQueueSize)), tr("in queue"));
        m_ticketBadge->setIconAndColors(QString(), t.warn, t.warnTint);
        m_ticketBadge->setVisible(true);
        m_ticketSpinner->setColors(t.warnTint, t.warn);
        m_ticketSpinner->setVisible(true);
        m_ticketAction->setText(tr("Cancel"));
        m_ticketAction->setKind(LobbyButton::Kind::Secondary);
        m_ticketAction->setIconName(QStringLiteral("close-line"));
    }
    else if (m_spectatingMatchId != 0)
    {
        QString lead;
        QString game;
        qint64 startedAt = 0;
        int viewers = 0;
        for (const auto& room : m_client->rooms())
        {
            if (room.matchId != m_spectatingMatchId)
                continue;
            lead = room.playerNames.isEmpty() ? room.name : room.playerNames.join(tr(" vs "));
            game = room.romName;
            startedAt = room.startedAtMs;
            viewers = room.viewerCount;
            break;
        }
        setTicket(tr("Watching"), t.stateWatch, lead,
                  game.isEmpty() ? QString() : QStringLiteral(" · ") + game);
        setValues(formatMatchDuration(startedAt), tr("live"), QString::number(viewers), tr("watching"));
        m_ticketSpinner->setVisible(false);
        m_ticketBadge->setVisible(true);
        m_ticketBadge->setIconAndColors(QStringLiteral("eye-line"), t.stateWatch, t.watchTint);
        m_ticketAction->setText(tr("Stop watching"));
        m_ticketAction->setKind(LobbyButton::Kind::Secondary);
        m_ticketAction->setIconName(QStringLiteral("stop-fill"));
    }
    else
    {
        m_playBarStack->setCurrentIndex(0);
        m_playBar->setBorderColor(QColor());
        m_ticketSpinner->setVisible(false);
    }
}

void RollbackLobbyDialog::onPlayBarActionClicked()
{
    if (m_currentRoomId != 0)
    {
        switchToInRoomView();
    }
    else if (m_outgoingChallenge.id != 0)
    {
        // Not waiting for the reply: there's none if the challenge has already ended.
        m_client->cancelChallenge(m_outgoingChallenge.id);
        m_outgoingChallenge = LobbyClient::Challenge{};
        refreshPlayBar();
    }
    else if (!m_challengeOutcome.title.isEmpty())
    {
        m_challengeOutcome.title.clear();
        refreshPlayBar();
    }
    else if (m_quickMatchActive)
    {
        m_client->quickMatchCancel();
    }
    else if (m_spectatingMatchId != 0)
    {
        m_pendingSpectate = PendingSpectate{};
        emit spectateStreamClosed(QStringLiteral("stopped"));
    }
}

// How much is going on for the game selected in the picker.
void RollbackLobbyDialog::refreshPickerInfo()
{
    if (!m_pickerInfo || !m_client)
        return;
    const QVariantMap rom = selectedBrowseRom();
    if (rom.isEmpty() || m_client->state() != LobbyClient::ConnectionState::Connected)
    {
        m_pickerInfo->setText(rom.isEmpty() && !m_roms.isEmpty() ? tr("Pick a game from your library") : QString());
        return;
    }
    const QString md5 = rom.value("md5").toString();
    int rooms = 0;
    int live = 0;
    for (const auto& room : m_client->rooms())
    {
        if (!md5.isEmpty() && md5.compare(room.romMd5, Qt::CaseInsensitive) != 0)
            continue;
        if (room.state == QLatin1String("in_game"))
            ++live;
        else
            ++rooms;
    }
    const QString strong = LobbyTheme::tokens().ink2.name();
    m_pickerInfo->setText(QString("<b style='color:%1;'>%2</b> %3 · <b style='color:%1;'>%4</b> %5")
        .arg(strong).arg(rooms).arg(rooms == 1 ? tr("room") : tr("rooms")).arg(live).arg(tr("live")));
}

void RollbackLobbyDialog::updateServerMeta()
{
    if (!m_serverMeta) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
    {
        m_serverMeta->setText(tr("Not connected"));
        if (m_playersCount)
            m_playersCount->clear();
        return;
    }
    const int players = m_client->users().size();
    const int rooms = m_client->rooms().size();
    // Server host/IP intentionally omitted from the header — just show the
    // live population.
    const QString strong = LobbyTheme::tokens().ink2.name();
    m_serverMeta->setText(QString("<b style='color:%1; font-weight:600;'>%2</b> %3"
                                  " &nbsp;·&nbsp; <b style='color:%1; font-weight:600;'>%4</b> %5")
        .arg(strong).arg(players).arg(players == 1 ? tr("player") : tr("players"))
        .arg(rooms).arg(rooms == 1 ? tr("room") : tr("rooms")));
    if (m_playersCount)
        m_playersCount->setText(QString::number(players));
}

// ──────────────────────────────────────────────────────────────────────
//  Presence
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onPresenceFull()
{
    clearPlayerRows();
    for (auto it = m_client->users().constBegin(); it != m_client->users().constEnd(); ++it)
    {
        auto* row = new QTreeWidgetItem();
        m_userItems.insert(it.key(), row);
        refreshPlayerRow(row, it.value());
    }
    refreshPlayerGroups();
    updateServerMeta();
    updateUserChip();
}

void RollbackLobbyDialog::onUserAdded(quint64 userId)
{
    const auto& users = m_client->users();
    auto it = users.constFind(userId);
    if (it == users.constEnd()) return;

    auto* row = new QTreeWidgetItem();
    m_userItems.insert(userId, row);
    refreshPlayerRow(row, it.value());
    refreshPlayerGroups();
    updateServerMeta();
}

void RollbackLobbyDialog::onUserRemoved(quint64 userId)
{
    m_pmPanel->markGone(userId);
    auto it = m_userItems.find(userId);
    if (it == m_userItems.end()) return;
    delete it.value();
    m_userItems.erase(it);
    refreshPlayerGroups();
    updateServerMeta();
}

void RollbackLobbyDialog::onUserUpdated(quint64 userId)
{
    auto it = m_userItems.find(userId);
    if (it == m_userItems.end()) return;
    const auto& users = m_client->users();
    auto userIt = users.constFind(userId);
    if (userIt == users.constEnd()) return;
    refreshPlayerRow(it.value(), userIt.value());
    refreshPlayerGroups();
    refreshPrivateMessageStatus(userId);
    if (userId == m_client->selfUserId())
        updateUserChip();
    for (auto& seat : m_seats)
    {
        if (seat.userId == userId && seat.row)
        {
            seat.row->setConnection(userIt->connection);
            const QString country = userIt->anonymous ? QString() : userIt->country;
            seat.row->setCountry(country, countryTooltip(country));
            break;
        }
    }
}

void RollbackLobbyDialog::clearPlayerRows()
{
    qDeleteAll(m_userItems);
    m_userItems.clear();
    if (m_selfRowBox)
        m_selfRowBox->hide();
    refreshPlayerGroups();
}

// Which heading a presence state lists under.
QTreeWidgetItem* RollbackLobbyDialog::playerGroupForState(const QString& state) const
{
    enum { Searching, Playing, Watching, InRooms, Online };
    if (m_playerGroups.size() <= Online)
        return nullptr;
    if (state == QLatin1String("searching"))  return m_playerGroups[Searching];
    if (state == QLatin1String("playing"))    return m_playerGroups[Playing];
    if (state == QLatin1String("spectating")) return m_playerGroups[Watching];
    if (state == QLatin1String("hosting") || state == QLatin1String("in_room"))
        return m_playerGroups[InRooms];
    return m_playerGroups[Online];
}

void RollbackLobbyDialog::placePlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u)
{
    // This PC's other players are listed in the roster instead.
    if (m_client->isLocalPlayer(u.id))
    {
        if (QTreeWidgetItem* parent = item->parent())
            parent->removeChild(item);
        else if (m_playersTree->indexOfTopLevelItem(item) >= 0)
            return;
        m_playersTree->addTopLevelItem(item);
        item->setHidden(true);
        return;
    }

    // This client's own row has its own place above the list.
    if (u.id == m_client->selfUserId())
    {
        if (m_selfRow->indexOfTopLevelItem(item) >= 0)
            return;
        if (QTreeWidgetItem* parent = item->parent())
            parent->removeChild(item);
        else if (const int index = m_playersTree->indexOfTopLevelItem(item); index >= 0)
            m_playersTree->takeTopLevelItem(index);
        m_selfRow->addTopLevelItem(item);
        m_selfRowBox->show();
        return;
    }

    QTreeWidgetItem* group = playerGroupForState(u.state);
    if (group == nullptr)
        return;
    // Already under the right heading: it only moves if the sort says so, say
    // because a ping landed.
    if (item->parent() == group)
    {
        const int index = group->indexOfChild(item);
        const bool inOrder = (index == 0 || !playerRowBefore(item, group->child(index - 1))) &&
                             (index == group->childCount() - 1 || !playerRowBefore(group->child(index + 1), item));
        if (inOrder)
            return;
    }
    const bool selected = item->isSelected();
    if (QTreeWidgetItem* parent = item->parent())
        parent->removeChild(item);
    else if (const int index = m_playersTree->indexOfTopLevelItem(item); index >= 0)
        m_playersTree->takeTopLevelItem(index);

    int position = 0;
    while (position < group->childCount() && playerRowBefore(group->child(position), item))
        ++position;
    group->insertChild(position, item);
    group->setExpanded(true);
    item->setSelected(selected);
}

bool RollbackLobbyDialog::playerRowBefore(const QTreeWidgetItem* a, const QTreeWidgetItem* b) const
{
    if (m_playerSort == PlayerSort::Ping)
    {
        // Unknown pings go last.
        const int pingA = a->data(0, kPingMsRole).toInt();
        const int pingB = b->data(0, kPingMsRole).toInt();
        const int keyA = pingA < 0 ? std::numeric_limits<int>::max() : pingA;
        const int keyB = pingB < 0 ? std::numeric_limits<int>::max() : pingB;
        if (keyA != keyB)
            return keyA < keyB;
    }
    else if (m_playerSort == PlayerSort::Region)
    {
        // In the regions' own order, then by country; unknown regions last.
        const int regionA = LobbyRegions::regionIndex(a->data(0, kRegionRole).toString());
        const int regionB = LobbyRegions::regionIndex(b->data(0, kRegionRole).toString());
        const int keyA = regionA < 0 ? std::numeric_limits<int>::max() : regionA;
        const int keyB = regionB < 0 ? std::numeric_limits<int>::max() : regionB;
        if (keyA != keyB)
            return keyA < keyB;
        const int country = a->data(0, kCountryRole).toString().compare(b->data(0, kCountryRole).toString());
        if (country != 0)
            return country < 0;
    }
    return a->text(0).compare(b->text(0), Qt::CaseInsensitive) < 0;
}

void RollbackLobbyDialog::resortPlayers()
{
    for (QTreeWidgetItem* group : m_playerGroups)
    {
        QList<QTreeWidgetItem*> rows = group->takeChildren();
        QSet<QTreeWidgetItem*> selected;
        for (QTreeWidgetItem* row : rows)
        {
            if (row->isSelected())
                selected.insert(row);
        }
        std::stable_sort(rows.begin(), rows.end(), [this](const QTreeWidgetItem* a, const QTreeWidgetItem* b) {
            return playerRowBefore(a, b);
        });
        group->addChildren(rows);
        for (QTreeWidgetItem* row : selected)
            row->setSelected(true);
    }
    refreshPlayerGroups();
}

bool RollbackLobbyDialog::playersFiltered() const
{
    return !m_playerRegionFilter.isEmpty() || m_playerHideWifi || m_playerMaxPing > 0;
}

// Every choice applies as it's ticked, with the menu still open.
void RollbackLobbyDialog::showPlayersFilterMenu()
{
    LobbyMenu menu(this);
    const auto save = [this]() {
        QSettings settings("RMG-K", "n02");
        settings.beginGroup("RollbackLobby/PlayerFilters");
        settings.setValue("Sort", static_cast<int>(m_playerSort));
        settings.setValue("Region", m_playerRegionFilter);
        settings.setValue("HideWifi", m_playerHideWifi);
        settings.setValue("MaxPing", m_playerMaxPing);
        settings.endGroup();
    };

    menu.addHeading(QString(), tr("Sort each list by"), false);
    auto* sorts = new QActionGroup(&menu);
    const auto addSort = [&](const QString& label, PlayerSort sort) {
        QAction* action = menu.addCheckItem(QString(), label, m_playerSort == sort, sorts);
        connect(action, &QAction::toggled, this, [this, sort, save](bool on) {
            if (!on)
                return;
            m_playerSort = sort;
            save();
            resortPlayers();
        });
    };
    addSort(tr("Name"), PlayerSort::Name);
    addSort(tr("Ping"), PlayerSort::Ping);
    addSort(tr("Region"), PlayerSort::Region);

    menu.addHeading(QString(), tr("Hide"), false);
    QAction* wifi = menu.addCheckItem(QStringLiteral("wifi-line"), tr("Players on Wi-Fi"), m_playerHideWifi);
    connect(wifi, &QAction::toggled, this, [this, save](bool on) {
        m_playerHideWifi = on;
        save();
        refreshPlayerGroups();
    });

    menu.addHeading(QString(), tr("Your ping to them"), false);
    auto* pings = new QActionGroup(&menu);
    for (const int limit : CreateRoomDialog::pingLimits())
    {
        QAction* action = menu.addPingItem(limit, CreateRoomDialog::pingLimitText(limit), m_playerMaxPing == limit, pings);
        connect(action, &QAction::toggled, this, [this, limit, save](bool on) {
            if (!on)
                return;
            m_playerMaxPing = limit;
            save();
            refreshPlayerGroups();
        });
    }

    // Only the regions someone is in, with how many.
    QMap<int, int> regions;
    for (const auto& user : m_client->users())
    {
        const int region = LobbyRegions::regionIndex(user.region);
        if (region >= 0)
            ++regions[region];
    }
    menu.addHeading(QString(), tr("Region"), false);
    auto* regionGroup = new QActionGroup(&menu);
    const auto addRegion = [&](const QString& label, const QString& code) {
        QAction* action = menu.addCheckItem(QString(), label, m_playerRegionFilter == code, regionGroup);
        connect(action, &QAction::toggled, this, [this, code, save](bool on) {
            if (!on)
                return;
            m_playerRegionFilter = code;
            save();
            refreshPlayerGroups();
        });
    };
    addRegion(tr("Every region"), QString());
    for (auto it = regions.constBegin(); it != regions.constEnd(); ++it)
    {
        const auto& region = LobbyRegions::kRegions[it.key()];
        const QString code = QString::fromUtf8(region.code.data(), int(region.code.size()));
        addRegion(QStringLiteral("%1 (%2)").arg(LobbyRegions::labelFor(code)).arg(it.value()), code);
    }
    // A region nobody's in any more stays listed while it's the filter.
    if (!m_playerRegionFilter.isEmpty() && !regions.contains(LobbyRegions::regionIndex(m_playerRegionFilter)))
        addRegion(QStringLiteral("%1 (0)").arg(LobbyRegions::labelFor(m_playerRegionFilter)), m_playerRegionFilter);

    menu.exec(m_playersFilterButton->mapToGlobal(QPoint(0, m_playersFilterButton->height())));
}

// Each heading shows how many players match the search and filters, and hides
// when none do.
void RollbackLobbyDialog::refreshPlayerGroups()
{
    if (!m_playersSearch)
        return;
    const QString search = m_playersSearch->text().trimmed();
    if (m_playersFilterButton)
        m_playersFilterButton->setIconColorOverride(playersFiltered() ? LobbyTheme::tokens().accentInk : QColor());
    for (QTreeWidgetItem* group : m_playerGroups)
    {
        int shown = 0;
        for (int i = 0; i < group->childCount(); ++i)
        {
            QTreeWidgetItem* child = group->child(i);
            const int ping = child->data(0, kPingMsRole).toInt();
            const bool match = (search.isEmpty() || child->text(0).contains(search, Qt::CaseInsensitive)) &&
                (m_playerRegionFilter.isEmpty() || child->data(0, kRegionRole).toString() == m_playerRegionFilter) &&
                (!m_playerHideWifi || !isWireless(child->data(0, kConnectionTypeRole).toString())) &&
                (m_playerMaxPing <= 0 || (ping >= 0 && ping <= m_playerMaxPing));
            child->setHidden(!match);
            shown += match ? 1 : 0;
        }
        group->setData(0, kGroupCountRole, shown);
        group->setHidden(shown == 0);
    }
}

void RollbackLobbyDialog::refreshPlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u)
{
    const bool isSelf = (u.id == m_client->selfUserId());
    item->setText(0, u.username);
    item->setData(0, Qt::UserRole, QVariant::fromValue(u.id));
    item->setData(0, kSelfRole, isSelf);
    item->setData(0, kStateRole, u.state);

    // The server geolocates the ISO country code; the delegate paints the
    // bundled flag. No flag for anonymous users — and deliberately no
    // region-badge fallback for them either, that's the coarse location leak
    // the option exists to prevent. Old servers that send no country just show
    // no flag.
    const QString flagResource = QString(":/flags/%1.svg").arg(u.country.toLower());
    QString countryName;
    if (!u.anonymous && !u.country.isEmpty() && QFile::exists(flagResource))
    {
        item->setData(0, kCountryRole, u.country);
        const QLocale::Territory territory = QLocale::codeToTerritory(u.country);
        countryName = (territory != QLocale::AnyTerritory)
            ? QLocale::territoryToString(territory)
            : u.country;
    }
    else
    {
        item->setData(0, kCountryRole, QString());
    }

    // Signal bars: a real measurement wins; the region estimate stands in
    // translucently until one lands; -1 draws the empty skeleton. Self shows no
    // bars. Anonymity does not hide this match-quality signal.
    const int measured = isSelf ? -1 : m_client->measuredPingMs(u.id);
    const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), u.region);
    const int effective = isSelf ? -1
        : measured >= 0 ? measured
        : estimate > 0  ? estimate
                        : -1;
    item->setData(0, kPingMsRole, effective);
    item->setData(0, kPingEstimateRole, measured < 0);
    item->setData(0, kConnectionTypeRole, u.connection);
    item->setData(0, kRegionRole, u.region);
    item->setData(0, kSocialRole, !isSelf && u.social && m_client->socialSupported());
    item->setData(0, kNoChallengesRole, u.noChallenges);

    // Built as lines so an absent field just doesn't contribute, rather than
    // leaving a stray separator. The region is deliberately not among them: it's
    // a coarse server-side bucket that can disagree with the geolocated country,
    // so the country is the only location we state.
    QStringList tipLines;
    // While searching, the tip shows which ROM they're queued for.
    if (u.state == "searching" && !u.searchingRom.isEmpty())
        tipLines << QString(tr("Searching for: %1")).arg(u.searchingRom);
    else
        tipLines << stateGlyph(u.state);
    if (!countryName.isEmpty())
        tipLines << QString(tr("Country: %1")).arg(countryName);
    // Self-reported transport medium ("wifi"/"lan"/...) and build, when the
    // peer's client and the server are new enough to relay them.
    const QString connLabel = ConnectionGlyph::displayName(u.connection);
    if (!connLabel.isEmpty())
        tipLines << QString(tr("Connection: %1")).arg(connLabel);
    if (!u.clientVersion.isEmpty())
        tipLines << QString(tr("Build: %1")).arg(u.clientVersion);
    if (!isSelf && u.noChallenges)
        tipLines << tr("Not taking challenges");
    if (!isSelf)
    {
        // The bars give the tier; the tooltip gives the number. An estimate is
        // hedged and only stands in until a real UDP probe lands — say what
        // makes it real rather than implying a measurement is on its way.
        if (measured >= 0)
            tipLines << QString(tr("Ping: %1 ms")).arg(measured);
        else
            tipLines << (estimate > 0
                ? QString(tr("Ping: ~%1 ms (estimate — click to measure)")).arg(estimate)
                : tr("Ping: click to measure"));
    }
    item->setToolTip(0, tipLines.join(QChar('\n')));

    placePlayerRow(item, u);
}

QString RollbackLobbyDialog::stateGlyph(const QString& state) const
{
    if (state == "idle")       return tr("Online");
    if (state == "browsing")   return tr("Browsing");
    if (state == "hosting")    return tr("Hosting");
    if (state == "in_room")    return tr("In Room");
    if (state == "searching")  return tr("Searching");
    if (state == "playing")    return tr("In Game");
    if (state == "spectating") return tr("Watching");
    if (state == "away")       return tr("Away");
    return state;
}

// ──────────────────────────────────────────────────────────────────────
//  Rooms list
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onRoomListChanged()
{
    refreshPlayerSpectatorChat();
    const bool connected = m_client->state() == LobbyClient::ConnectionState::Connected;
    const QVariantMap selectedRom = selectedBrowseRom();
    const QString selectedMd5 = selectedRom.value("md5").toString().trimmed();
    const QString selectedName = selectedRom.value("name").toString().trimmed();

    // Rows are matched to players by name, the only key playerNames offers.
    QHash<QString, QString> countries;
    QHash<QString, QString> connections;
    QSet<quint64> roomsWithWifi;
    if (connected)
    {
        for (const auto& user : m_client->users())
        {
            if (!user.anonymous && !user.country.isEmpty())
                countries.insert(user.username, user.country);
            if (!user.connection.isEmpty())
                connections.insert(user.username, user.connection);
            if (user.currentRoomId != 0 && isWireless(user.connection))
                roomsWithWifi.insert(user.currentRoomId);
        }
    }
    // Everyone on this PC shares its connection.
    const bool selfWireless = connected && isWireless(m_client->selfConnection());

    QList<LiveMatchView> matches;
    QList<RoomRowView> rooms;
    for (auto it = m_client->rooms().constBegin(); connected && it != m_client->rooms().constEnd(); ++it)
    {
        const LobbyClient::LobbyRoomSummary& r = it.value();

        const bool sameMd5 =
            !selectedMd5.isEmpty() && !r.romMd5.isEmpty() &&
            selectedMd5.compare(r.romMd5, Qt::CaseInsensitive) == 0;
        const bool sameName =
            (selectedMd5.isEmpty() || r.romMd5.isEmpty()) &&
            !selectedName.isEmpty() &&
            selectedName.compare(r.romName, Qt::CaseInsensitive) == 0;
        const bool sameGame = sameMd5 || sameName;
        const bool hasRom = r.romMd5.isEmpty() || !localRomPathForMd5(r.romMd5).isEmpty();

        // A room that's playing is a live match — show it in the gallery. It
        // stays in the joinable list only while a late joiner can take a seat.
        if (r.state == "in_game")
        {
            LiveMatchView match;
            match.roomId = r.id;
            match.matchId = r.broadcasting ? r.matchId : 0;
            match.game = r.romName;
            match.roomName = r.name;
            match.host = r.hostName;
            match.coverPath = coverPathForMd5(r.romMd5);
            match.startedAtMs = r.startedAtMs;
            match.viewers = r.viewerCount;
            match.openSeats = r.lateJoin ? std::max(0, r.maxPlayers - r.players) : 0;
            match.seatsNeeded = m_client->seatsOnThisPc();
            match.hasRom = hasRom;
            match.sameGame = sameGame;
            match.watching = m_spectatingMatchId != 0 && m_spectatingMatchId == r.matchId;
            match.watchingOther = m_spectatingMatchId != 0 && !match.watching;
            match.inRoom = m_currentRoomId != 0;
            for (const QString& name : r.playerNames)
                match.players.append({ name, countries.value(name), connections.value(name) });
            match.gameOptions = gameOptionLabels(r.romMd5, r.gameOptions);
            match.gameOptionsTip = gameOptionsTip(r.romMd5, r.gameOptions);
            matches.append(match);
            if (!r.lateJoin)
                continue;
        }
        // Made for a challenge, so there's nothing here for anyone else to join.
        if (r.privateRoom)
            continue;

        RoomRowView room;
        room.roomId = r.id;
        room.hostId = r.hostId;
        room.name = r.name;
        room.host = r.hostName;
        room.game = r.romName;
        room.players = r.players;
        room.maxPlayers = r.maxPlayers;
        room.hasPassword = r.hasPassword;
        room.inGame = r.state == "in_game";
        room.mine = r.id == m_currentRoomId;
        room.seatsNeeded = m_client->seatsOnThisPc();
        room.hasRom = hasRom;
        room.sameGame = sameGame;
        room.hostCountry = countries.value(r.hostName);
        room.anyWifi = roomsWithWifi.contains(r.id);
        room.maxPing = r.maxPing;
        room.wiredOnly = r.wiredOnly;
        room.gameOptions = gameOptionLabels(r.romMd5, r.gameOptions);
        room.gameOptionsTip = gameOptionsTip(r.romMd5, r.gameOptions);

        // Your ping to the host — the real measurement once one exists, the
        // region estimate until then — so a room can be judged before joining.
        // Nothing is probed in the background; selecting the row measures it.
        if (r.hostId != 0 && r.hostId != m_client->selfUserId())
        {
            room.pingMs = m_client->measuredPingMs(r.hostId);
            room.pingMeasured = room.pingMs >= 0;
            if (!room.pingMeasured)
            {
                const auto hostIt = m_client->users().constFind(r.hostId);
                if (hostIt != m_client->users().constEnd())
                    room.pingMs = LobbyRegions::estimatedRttMs(m_client->selfRegion(), hostIt->region);
                if (room.pingMs == 0)
                    room.pingMs = -1;
            }
        }
        if (!room.mine && room.wiredOnly && selfWireless)
            room.refusal = RoomRowView::Refusal::WiredOnly;
        else if (!room.mine && room.maxPing > 0 && room.pingMs > room.maxPing)
            room.refusal = RoomRowView::Refusal::PingTooHigh;
        rooms.append(room);
    }

    m_liveGallery->setMatches(matches);
    m_roomsColumn->setRooms(rooms);
    refreshPickerInfo();
    updateServerMeta();
    refreshPlayBar();   // seat counts may have changed in our own room
}

void RollbackLobbyDialog::updateMatchDurations()
{
    if (m_liveGallery)
        m_liveGallery->updateDurations();
    refreshRoomBadge();
    refreshPlayBar();
}

void RollbackLobbyDialog::switchToRoomsView()
{
    m_roomMinimized = true;
    layoutStage();
}

void RollbackLobbyDialog::switchToInRoomView()
{
    m_roomMinimized = false;
    layoutStage();
}

bool RollbackLobbyDialog::roomWindowOpen() const
{
    return m_roomWindow && m_roomWindow->isVisible();
}

void RollbackLobbyDialog::layoutStage()
{
    if (!m_stage || !m_roomWindow)
        return;
    const bool open = m_currentRoomId != 0 && !m_roomMinimized;
    if (!open && m_roomSettingsPanel)
        m_roomSettingsPanel->hide();
    if (!open)
        endSeatDrag(QPoint(), false);
    m_scrim->setGeometry(m_stage->rect());
    m_roomWindow->placeIn(m_stage->rect().adjusted(MARGIN_STAGE, MARGIN_STAGE, -MARGIN_STAGE_INNER, -MARGIN_STAGE));
    m_scrim->setWindowRect(m_roomWindow->geometry());
    m_scrim->setVisible(open);
    m_roomWindow->setVisible(open);
    m_scrim->raise();
    m_roomWindow->raise();
    m_challengeCard->raise();
    m_toast->raise();
    positionStageOverlays();
}

// The room's subtitle went: its game, seats and host are in the title's tooltip.
void RollbackLobbyDialog::refreshRoomTitleTip()
{
    if (!m_roomTitle)
        return;
    QString tip = m_roomSubText;
    if (m_client)
    {
        const auto room = m_client->rooms().constFind(m_currentRoomId);
        if (room != m_client->rooms().constEnd() && room->state == QLatin1String("in_game") && room->viewerCount > 0)
            tip += QStringLiteral(" · ") + tr("%1 watching").arg(room->viewerCount);
    }
    m_roomTitle->setToolTip(tip.isEmpty() ? m_roomTitle->text() : m_roomTitle->text() + QStringLiteral("\n") + tip);
    if (m_roomBadge)
        m_roomBadge->setToolTip(m_currentRoomGame);
}

void RollbackLobbyDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && roomWindowOpen())
    {
        switchToRoomsView();
        return;
    }
    QDialog::keyPressEvent(event);
}

void RollbackLobbyDialog::clearServerRoomSnapshot()
{
    // None of this state owns gameplay transport. Once the lobby connection is
    // gone, retaining it can only display a room/roster that may no longer exist
    // (the server keeps rooms in memory and loses them across a restart).
    m_canReorderSeats = false;
    endSeatDrag(QPoint(), false);

    m_currentRoomId = 0;
    m_currentRoomGame.clear();
    m_currentRoomMd5.clear();
    m_currentRoomRegion.clear();
    m_currentRoomState.clear();
    m_currentRoomDelay = 2;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    m_currentRoomPacing = 0;
    m_currentRoomHostId = 0;
    m_currentRoomHostName.clear();
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;
    m_localDelayPublished = false;
    m_probeFailureAnnounced.clear();
    updateRollbackSettingControls();

    for (auto& seat : m_seats)
    {
        renderSeatEmpty(seat);
        if (seat.row)
            seat.row->setVisible(true);
    }

    if (m_roomTitle) m_roomTitle->setText(QStringLiteral("—"));
    m_roomSubText.clear();
    m_roomHasJoinLimits = false;
    m_roomMaxPing = 0;
    m_roomWiredOnly = false;
    m_roomResizable = false;
    m_roomMaxPlayers = 0;
    m_roomSeatedCount = 0;
    if (m_chatViewRoom) m_chatViewRoom->clear();
    setRoomChatVisible(false);
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn) m_dropBtn->setEnabled(false);
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    switchToRoomsView();
    refreshPlayBar();
}

void RollbackLobbyDialog::showAdminRoomMenu(quint64 roomId, const QPoint& globalPos)
{
    // Right-click close is a moderator tool — everyone else gets no menu.
    if (!m_client || !m_client->isModerator() || roomId == 0)
        return;
    const auto room = m_client->rooms().constFind(roomId);
    if (room == m_client->rooms().constEnd())
        return;

    LobbyMenu menu(this);
    QAction* closeAction = menu.addDangerItem(QStringLiteral("close-circle-line"), room->state == QLatin1String("in_game")
        ? tr("End match && close room  (moderator)")
        : tr("Close room  (moderator)"));
    if (menu.exec(globalPos) != closeAction)
        return;

    if (QMessageBox::question(this, tr("Close room"),
            tr("Close \"%1\" for everyone?").arg(room->name)) != QMessageBox::Yes)
        return;

    m_client->sendModAction(QStringLiteral("closeroom"),
                            QString::number(roomId), QString(), QString());
}

void RollbackLobbyDialog::onCreateRoomClicked()
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
        return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("Already in a room"),
            tr("Leave your current room before creating a new one."));
        return;
    }
    if (m_createRoomDialog)
    {
        m_createRoomDialog->raise();
        m_createRoomDialog->activateWindow();
        return;
    }
    // The room's game is whatever is selected in the lobby's shared picker — the
    // same source Quick Match uses. Require a real pick before opening the form.
    const QVariantMap romData = selectedBrowseRom();
    const QString romName = romData.value("name").toString();
    const QString romMd5  = romData.value("md5").toString();
    if (romMd5.isEmpty())
    {
        QMessageBox::information(this, tr("Select a game"),
            tr("Pick a game from the dropdown before creating a room."));
        return;
    }

    m_createRoomDialog = new CreateRoomDialog(
        m_username, m_lastRoomName, romName, romMd5, this);
    m_createRoomDialog->setCoverPath(coverPathForMd5(romMd5));
    m_createRoomDialog->setMinimumPlayers(m_client->seatsOnThisPc());
    connect(m_createRoomDialog, &CreateRoomDialog::createRequested,
            this, &RollbackLobbyDialog::onRoomCreateRequested);
    connect(m_createRoomDialog, &QDialog::finished, this, [this](int) {
        m_createRoomDialog->deleteLater();
        m_createRoomDialog = nullptr;
    });
    m_createRoomDialog->show();
}

void RollbackLobbyDialog::onRoomCreateRequested()
{
    if (!m_createRoomDialog) return;
    // Pacing is fixed to Smooth (the engine hardwires it); the room field only
    // survives for protocol compatibility.
    const int pacing = 1;
    m_client->createRoom(
        m_createRoomDialog->name(),
        m_createRoomDialog->romName(),
        m_createRoomDialog->romMd5(),
        m_createRoomDialog->romRegion(),
        m_createRoomDialog->maxPlayers(),
        m_createRoomDialog->delay(),
        m_createRoomDialog->prediction(),
        pacing,
        m_createRoomDialog->password(),
        m_createRoomDialog->maxPing(),
        m_createRoomDialog->wiredOnly());
}

void RollbackLobbyDialog::onRoomCreated(quint64 roomId)
{
    if (m_createRoomDialog)
    {
        m_lastRoomName = m_createRoomDialog->name();
        m_createRoomDialog->accept();
    }
    enterRoom(roomId,
        QString("<i>%1</i>").arg(tr("Room created — waiting for players")));
}

void RollbackLobbyDialog::onRoomJoinOk(quint64 roomId)
{
    QString roomName;
    auto it = m_client->rooms().constFind(roomId);
    if (it != m_client->rooms().constEnd())
        roomName = it.value().name;
    enterRoom(roomId,
        roomName.isEmpty()
            ? tr("<i>You joined the room</i>")
            : QString(tr("<i>You joined &quot;%1&quot;</i>")).arg(roomName.toHtmlEscaped()));
}

void RollbackLobbyDialog::onRoomJoinFailed(const QString& reason)
{
    QString human = reason;
    if (reason == "wrong_password")    human = tr("Wrong password.");
    else if (reason == "full")         human = tr("Room is full.");
    else if (reason == "already_started") human = tr("That game has already started.");
    else if (reason == "already_in_room") human = tr("You're already in a room.");
    else if (reason == "room_not_found")  human = tr("That room no longer exists.");
    else if (reason == "kicked_from_room") human =
        tr("You were removed from this room and cannot rejoin.");
    else if (reason == "kicked_recently") human =
        tr("You were recently removed from this room. Try again in a moment.");
    else if (reason == "not_enough_seats") human =
        tr("That room doesn't have a free seat for everyone on this PC.");
    else if (reason == "private_room") human =
        tr("That room is private.");
    else if (reason == "wired_only") human =
        tr("That room is wired only, and you're on Wi-Fi.");
    else if (reason == "ping_too_high") human =
        tr("Your ping to that room's host is higher than the room allows.");
    else if (reason == "needs_update") human =
        tr("That room needs a newer version of Dale64.");
    else if (reason == "peer_needs_update") human =
        tr("Someone in that room is on an older Dale64 that can't play with several players on one PC. "
           "Remove the other players on this PC to join.");
    QMessageBox::warning(this, tr("Couldn't join room"), human);
}

void RollbackLobbyDialog::enterRoom(quint64 roomId, const QString& greetingChatLine)
{
    m_currentRoomId = roomId;
    m_liveReplayPreferencePublished = false;

    // The dialog survives reconnects and room changes. Recheck storage and
    // reset the recording choice on entry, then preserve it for this room.
    CoreRefreshKailleraRecordingStorageStatus();
    const bool recordingDefault = CoreGetKailleraEffectiveRecordingDefault();
    n02_kaillera_recording_enabled = recordingDefault;
    m_recordLocallyChoice = recordingDefault;
    m_roomHasLateJoinOption = false;
    m_roomLateJoinEnabled = false;
    m_lateJoinEnabledSeen = false;
    if (m_recordCheck)
        m_recordCheck->setChecked(recordingDefault);

    // Being in a room is mutually exclusive with searching. A successful Quick
    // Match drops us straight into an auto-created room without the server ever
    // sending QUICK_MATCH_STATUS{searching:false}, so clear the toggle state
    // here. Otherwise m_quickMatchActive stays true through the game and onClick
    // sends quickMatchCancel() (a no-op once we're out of the queue) instead of
    // quickMatchJoin() — leaving the button unable to start a new search.
    m_quickMatchActive = false;

    // Restore only our selected mode/value, never an Auto calculation or a
    // remote player's delay. With remembering disabled, each room uses Auto.
    int preferredDelay = -1;
    if (CoreSettingsGetBoolValue(SettingsID::Rollback_RememberInputDelay))
    {
        QSettings settings("RMG-K", "n02");
        bool valid = false;
        const int savedDelay = settings.value("RollbackLobby/LastInputDelay", -1).toInt(&valid);
        if (valid && savedDelay >= 0 && savedDelay <= 9)
            preferredDelay = savedDelay;
    }
    m_delayAuto = preferredDelay == -1;
    m_predictionAuto = true;
    m_localDelayPublished = false;
    m_currentRoomDelay = m_delayAuto ? autoFrameDelayForPing(-1) : preferredDelay;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    if (m_delayStepper && m_predictionStepper)
    {
        m_delayStepper->setAutomatic(m_delayAuto);
        const QSignalBlocker blocker(m_delayAutoCheck);
        m_delayAutoCheck->setChecked(m_delayAuto);
        m_delayStepper->setValue(m_currentRoomDelay);
        m_predictionStepper->setAutomatic(true);
        m_predictionStepper->setValue(kDefaultPredictionWindow);
    }

    // Fresh room — don't chime for the seats already present in the first
    // ROOM_STATE; only for players who join after we've settled in.
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;

    // Fresh room, fresh chat — never carry the previous room's messages in.
    if (m_chatViewRoom) m_chatViewRoom->clear();
    setRoomChatVisible(true);
    m_roomHasJoinLimits = false;
    m_roomMaxPing = 0;
    m_roomWiredOnly = false;
    updateJoinLimitControls();
    m_roomResizable = false;
    updateRoomSizeControls();
    switchToInRoomView();

    if (m_createRoomBtn) m_createRoomBtn->setEnabled(false);
    if (m_quickMatchBtn) m_quickMatchBtn->setEnabled(false);

    refreshPlayBar();
    onRoomListChanged();

    if (!greetingChatLine.isEmpty())
        appendChatLine(CHANNEL_ROOM, greetingChatLine);

    refreshLocalRoster();

    // Backstop in case ROOM_STATE seats arrive in a shape that didn't trigger a
    // per-peer probe — kick one sweep shortly after entry so every client starts
    // filling the room-wide ping matrix promptly. Harmless in an empty room:
    // onPingProbeTick skips self / unseated slots.
    QTimer::singleShot(600, this, &RollbackLobbyDialog::onPingProbeTick);
}

void RollbackLobbyDialog::onRoomJoinRequested(quint64 roomId)
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected) return;
    if (roomId == m_currentRoomId)
    {
        switchToInRoomView();
        return;
    }
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("Already in a room"),
            tr("Leave your current room before joining another."));
        return;
    }

    auto it = m_client->rooms().constFind(roomId);
    if (it == m_client->rooms().constEnd()) return;
    const auto& summary = it.value();

    if ((summary.state == "in_game" || summary.state == "starting") && !summary.lateJoin)
    {
        if (summary.broadcasting && summary.matchId != 0)
        {
            beginSpectate(summary.matchId, summary.romName, summary.romMd5);
        }
        else
        {
            QMessageBox::information(this, tr("Match in progress"),
                tr("That room is already playing — try another or wait for it to finish."));
        }
        return;
    }

    // Don't join a room whose ROM we don't have. Otherwise we'd commit, make the
    // other players wait, then fail pre-match sync — which used to hang the whole
    // client. Matched by MD5, the same check onMatchBegin uses, so passing here
    // guarantees the ROM resolves at match start too.
    if (!summary.romMd5.isEmpty() && localRomPathForMd5(summary.romMd5).isEmpty())
    {
        QMessageBox::warning(this, tr("ROM not found"),
            QString(tr("You don't have the ROM for \"%1\" (%2).\n\n"
                    "Add it to your ROM directory and refresh the list, then try again."))
                .arg(summary.name, summary.romName));
        return;
    }

    // The room's limits, checked here so nobody types a password for nothing;
    // the server checks them again. Ping is measured if it can be, otherwise
    // estimated from regions, like the room list shows it.
    int pingMs = summary.hostId != 0 ? m_client->measuredPingMs(summary.hostId) : -1;
    if (pingMs < 0 && summary.hostId != 0)
    {
        const auto host = m_client->users().constFind(summary.hostId);
        if (host != m_client->users().constEnd())
            pingMs = LobbyRegions::estimatedRttMs(m_client->selfRegion(), host->region);
    }
    if (summary.wiredOnly && isWireless(m_client->selfConnection()))
    {
        QMessageBox::information(this, tr("Wired only"),
            tr("\"%1\" doesn't take players on Wi-Fi. Connect with a cable to join it.").arg(summary.name));
        return;
    }
    if (summary.maxPing > 0 && pingMs > summary.maxPing)
    {
        QMessageBox::information(this, tr("Too far"),
            tr("\"%1\" takes players within %2 ms of the host. Your ping to them is about %3 ms.")
                .arg(summary.name).arg(summary.maxPing).arg(pingMs));
        return;
    }

    QString password;
    if (summary.hasPassword)
    {
        bool ok = false;
        password = QInputDialog::getText(this, tr("Password required"),
            QString(tr("Enter password for \"%1\":")).arg(summary.name),
            QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }
    m_client->joinRoom(roomId, password, std::max(0, pingMs));
}

void RollbackLobbyDialog::onMatchWatchRequested(quint64 roomId)
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected) return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("In a room"),
            tr("Leave your room before watching a match."));
        return;
    }
    const auto room = m_client->rooms().constFind(roomId);
    if (room == m_client->rooms().constEnd())
        return;
    if (!room->broadcasting || room->matchId == 0)
    {
        QMessageBox::information(this, tr("Not live"),
            tr("That match isn't streaming a live replay, so there's nothing to watch."));
        return;
    }
    beginSpectate(room->matchId, room->romName, room->romMd5);
}

void RollbackLobbyDialog::onRoomCreateFailed(const QString& reason)
{
    if (m_createRoomDialog)
    {
        m_createRoomDialog->showCreateFailure(reason);
        return;
    }
    QMessageBox::warning(this, tr("Couldn't create room"), reason);
}

namespace
{
    // The game options packs' translations follow the language the UI was
    // started in.
    const std::string& gameOptionLanguage()
    {
        static const std::string language = Translation::ResolvedLanguage().toStdString();
        return language;
    }

    // This client's own pack's entry for one of a game's options, if it has one.
    std::optional<CoreGameOptionInfo> ownGameOption(const QString& romMd5, const QString& id)
    {
        for (CoreGameOptionInfo& info : CoreGetGameOptions(romMd5.toStdString(), gameOptionLanguage()))
        {
            if (QString::fromStdString(info.Id) == id)
                return std::move(info);
        }
        return std::nullopt;
    }
}

void RollbackLobbyDialog::onRoomStateChanged(const QJsonObject& roomState)
{
    const quint64 roomId = static_cast<quint64>(roomState.value("id").toDouble());
    if (roomId == 0 || roomId != m_currentRoomId)
        return;

    const QString name = roomState.value("name").toString();
    const QJsonObject rom = roomState.value("rom").toObject();
    const QString romName = rom.value("name").toString();
    const QString romMd5 = rom.value("md5").toString();
    const QString romRegion = rom.value("region").toString();
    const int roomPrediction = std::clamp(
        roomState.value("prediction").toInt(kDefaultPredictionWindow), 1, 9);
    const bool roomPredictionAuto = roomState.value("predictionAuto").toBool(true);
    const int pacing = roomState.value("pacing").toInt() == 1 ? 1 : 0;
    const int maxPlayers = roomState.value("maxPlayers").toInt();
    const QString state = roomState.value("state").toString();
    const quint64 hostId = static_cast<quint64>(roomState.value("hostId").toDouble());
    const bool iAmHost = (hostId == m_client->selfUserId());

    // New servers publish the host's choice. With an older server, preserve the
    // host's local checkbox so an unrelated ROOM_STATE cannot silently disarm it;
    // non-hosts have no authoritative value to display and therefore show off.
    bool liveReplayEnabled = roomState.contains("liveReplayEnabled")
        ? roomState.value("liveReplayEnabled").toBool()
        : (iAmHost && m_broadcastCheck && m_broadcastCheck->isChecked());

    // Initialize each room we host from our own saved choice, including Quick
    // Match rooms. Remote hosts' settings must never replace that preference.
    if (iAmHost && state == "waiting" && !m_liveReplayPreferencePublished)
    {
        m_liveReplayPreferencePublished = true;
        const bool preferred = liveReplayPreference();
        if (roomState.contains("liveReplayEnabled") && liveReplayEnabled != preferred)
            m_client->updateRoomLiveReplay(preferred);
        liveReplayEnabled = preferred;
    }

    m_currentRoomGame       = romName;
    m_currentRoomMd5        = romMd5;
    m_currentRoomRegion     = romRegion;
    m_currentRoomPrediction = roomPrediction;
    m_predictionAuto        = roomPredictionAuto;
    m_currentRoomPacing     = pacing;
    m_currentRoomHostId     = hostId;
    m_serverAcceptsInGameSettings = roomState.value("inGameSettings").toBool(false);
    m_roomLateJoin = roomState.value("lateJoin").toBool(false);
    m_canStartAlone = roomState.value("canStartAlone").toBool(false);

    // Tell the room when the host changes late join, and notify new guests if it's on.
    m_roomHasLateJoinOption = roomState.contains("lateJoinEnabled");
    {
        const bool lateJoinEnabled = roomState.value("lateJoinEnabled").toBool(false);
        if (!m_lateJoinEnabledSeen)
        {
            if (lateJoinEnabled && !iAmHost)
                appendChatSystemLine(CHANNEL_ROOM,
                    tr("Late join is on."));
        }
        else if (lateJoinEnabled != m_roomLateJoinEnabled)
        {
            if (iAmHost)
                appendChatSystemLine(CHANNEL_ROOM, lateJoinEnabled
                    ? tr("Late join is on.")
                    : tr("Late join is off."));
            else
                appendChatSystemLine(CHANNEL_ROOM, lateJoinEnabled
                    ? tr("The host turned on late join.")
                    : tr("The host turned off late join."));
        }
        m_lateJoinEnabledSeen = true;
        m_roomLateJoinEnabled = lateJoinEnabled;
    }
    if (m_lateJoinCheck)
    {
        m_suppressSettingsSignal = true;
        m_lateJoinCheck->setChecked(m_roomLateJoinEnabled);
        m_suppressSettingsSignal = false;
        m_lateJoinCheck->setVisible(m_roomHasLateJoinOption);
    }

    // Save data is the host's choice per room too. Everyone hears which save
    // the next match starts from, and when it changes.
    m_roomHasSaveDataOption = roomState.contains("saveData");
    {
        const QJsonObject saveData = roomState.value("saveData").toObject();
        const bool enabled = saveData.value("enabled").toBool(false);
        const QString label = saveData.value("label").toString();
        const QString hash = saveData.value("hash").toString();
        QStringList kinds;
        for (const auto& kind : saveData.value("kinds").toArray())
            kinds << kind.toString();
        const bool changed = enabled != m_roomSaveDataEnabled || (enabled && hash != m_roomSaveDataHash);
        // A blank save holds nothing yet: the match makes the game's first save.
        const bool blank = enabled && kinds.isEmpty();
        if (m_roomHasSaveDataOption && changed && (m_saveDataSeen || enabled))
        {
            const QString what = QStringLiteral("%1 (%2)").arg(label, kinds.join(QStringLiteral(" + ")));
            if (!enabled)
                appendChatSystemLine(CHANNEL_ROOM, iAmHost
                    ? tr("Save data is off. The next match starts from a blank save.")
                    : tr("The host turned off save data. The next match starts from a blank save."));
            else if (blank)
                appendChatSystemLine(CHANNEL_ROOM, iAmHost
                    ? tr("Save data is on. The next match starts from a blank save.")
                    : !m_saveDataSeen ? tr("This room uses save data. The next match starts from a blank save.")
                                      : tr("The host picked a blank save. The next match starts from it."));
            else if (iAmHost)
                appendChatSystemLine(CHANNEL_ROOM, tr("The next match starts from your save: %1.").arg(what));
            else if (!m_saveDataSeen)
                appendChatSystemLine(CHANNEL_ROOM, tr("This room uses save data. Matches start from the host's save: %1.").arg(what));
            else
                appendChatSystemLine(CHANNEL_ROOM, tr("The host picked save data: %1. The next match starts from it.").arg(what));
        }
        m_saveDataSeen = m_roomHasSaveDataOption;
        m_roomSaveDataEnabled = enabled;
        m_roomSaveDataLabel = label;
        m_roomSaveDataKinds = kinds;
        m_roomSaveDataBytes = saveData.value("bytes").toInt();
        m_roomSaveDataHash = hash;
    }
    if (m_saveDataCheck)
    {
        m_suppressSettingsSignal = true;
        m_saveDataCheck->setChecked(m_roomSaveDataEnabled);
        m_suppressSettingsSignal = false;
        m_saveDataCheck->setVisible(m_roomHasSaveDataOption);
    }
    // Game options are the host's choice per room too, and the server keeps
    // the list. Everyone hears when one goes on or off, and whoever enters a
    // room with some on hears which: they decide what can be played.
    m_roomGameOptionsOffered = roomState.value("gameOptionsOffered").toBool();
    {
        const QList<LobbyClient::LobbyGameOption> options = LobbyClient::parseGameOptions(roomState.value("gameOptions"));
        const auto has = [](const QList<LobbyClient::LobbyGameOption>& list, const QString& id) {
            return std::any_of(list.cbegin(), list.cend(), [&id](const auto& option) { return option.id == id; });
        };
        // With this client's pack, a line says what the option does. (Japanese
        // sentences follow one another without a space.)
        const auto withDescription = [this](const QString& line, const LobbyClient::LobbyGameOption& option) {
            const std::optional<CoreGameOptionInfo> own = ownGameOption(m_currentRoomMd5, option.id);
            if (!own || own->Description.empty())
                return line;
            const QString gap = line.endsWith(QChar(0x3002)) ? QString() : QStringLiteral(" ");
            return line + gap + QString::fromStdString(own->Description).toHtmlEscaped();
        };
        if (!m_gameOptionsSeen)
        {
            for (const auto& option : options)
            {
                if (iAmHost)
                    break;
                const QString label = gameOptionLabels(m_currentRoomMd5, { option }).value(0).toHtmlEscaped();
                appendChatSystemLine(CHANNEL_ROOM, withDescription(tr("%1 is on in this room.").arg(label), option));
            }
        }
        else
        {
            for (const auto& option : options)
            {
                if (has(m_roomGameOptions, option.id))
                    continue;
                const QString label = gameOptionLabels(m_currentRoomMd5, { option }).value(0).toHtmlEscaped();
                appendChatSystemLine(CHANNEL_ROOM, withDescription(
                    iAmHost ? tr("%1 is on.").arg(label) : tr("The host turned on %1.").arg(label), option));
            }
            for (const auto& option : m_roomGameOptions)
            {
                if (has(options, option.id))
                    continue;
                const QString label = gameOptionLabels(m_currentRoomMd5, { option }).value(0).toHtmlEscaped();
                appendChatSystemLine(CHANNEL_ROOM, iAmHost ? tr("%1 is off.").arg(label)
                                                           : tr("The host turned off %1.").arg(label));
            }
        }
        m_gameOptionsSeen = true;
        m_roomGameOptions = options;
        m_hostGameOptionIds.clear();
        if (iAmHost)
        {
            for (const auto& option : options)
                m_hostGameOptionIds.push_back(option.id.toStdString());
        }
    }
    // Join limits are the host's too, and can change at any time.
    m_roomHasJoinLimits = roomState.contains("joinLimits");
    {
        const QJsonObject limits = roomState.value("joinLimits").toObject();
        m_roomMaxPing = std::max(0, limits.value("maxPing").toInt());
        m_roomWiredOnly = limits.value("wiredOnly").toBool();
    }
    updateJoinLimitControls();

    if (m_publishSaveChoiceWhenWaiting && iAmHost && state == "waiting")
    {
        m_publishSaveChoiceWhenWaiting = false;
        if (m_roomSaveDataEnabled)
            publishHostSaveChoice();
    }

    // Pacing is host-authoritative and applies to every seat. Mirror it into the
    // engine setting now (host included) so the match starts on the room's model;
    // reset_gekko_log() reads Rollback_PacingMode when the session begins.
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, pacing);

    // Auto-stop emulation if the room transitioned out of in_game while we
    // still had a live local match (host dropped it for everyone).
    const QString prevState = m_currentRoomState;
    m_currentRoomState = state;
    // So is the room's size, while it waits.
    m_roomResizable = roomState.value("resizable").toBool();
    m_roomMaxPlayers = maxPlayers;
    m_roomSeatedCount = int(roomState.value("players").toArray().size());
    updateRoomSizeControls();
    if (prevState == "in_game" && state != "in_game" &&
        (m_emulationActive || m_awaitingEmulationStart))
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("Host ended the match — stopping game."));
        emit closeMatchRequested();
    }

    if (m_dropBtn)
    {
        m_dropBtn->setEnabled(state == "in_game" &&
            (m_emulationActive || m_awaitingEmulationStart));
        m_dropBtn->setToolTip(
            iAmHost
                ? tr("Stop the match for everyone in the room.")
                : tr("Drop out of the match. Other players keep playing."));
    }
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    // ── Title bar ──
    const QString title = name.isEmpty() ? romName
        : romName.isEmpty() || name == romName ? name
        : QStringLiteral("%1 — %2").arg(name, romName);
    m_roomTitle->setText(title);
    m_roomBadge->setGame(romName, coverPathForMd5(romMd5));

    applyRoomStateBadge(roomStateLabel(state), stateHex(state, isDarkTheme()));

    const QJsonArray players = roomState.value("players").toArray();
    m_currentRoomHostName.clear();
    for (const auto& v : players)
    {
        const auto p = v.toObject();
        if (static_cast<quint64>(p.value("userId").toDouble()) == hostId)
            m_currentRoomHostName = p.value("username").toString();
    }
    {
        QStringList parts;
        if (!romRegion.isEmpty())
            parts << romRegion;
        if (roomState.value("private").toBool())
            parts << tr("Private");
        if (!iAmHost)
        {
            parts << (state == "in_game" ? tr("in game")
                                         : tr("%1 of %2 seats").arg(players.size()).arg(maxPlayers));
            parts << tr("host %1").arg(m_currentRoomHostName);
        }
        m_roomSubText = parts.join(QStringLiteral(" · "));
        refreshRoomTitleTip();
        refreshPlayerSpectatorChat();
    }

    // Delay remains local to every player. Prediction mirrors authoritative
    // room state; updateRollbackSettingControls() decides who may edit them.
    if (m_delayStepper && m_predictionStepper)
    {
        if (m_delayAuto)
            m_delayStepper->setValue(m_currentRoomDelay);
        m_predictionStepper->setAutomatic(roomPredictionAuto);
        m_predictionStepper->setValue(roomPrediction);
    }
    updateRollbackSettingControls();

    // The host's prediction reaches every seat through ROOM_STATE, the host's
    // own included, so each player's running match switches to it on receipt.
    // Players may switch a few frames apart: the window only limits how far
    // each client predicts ahead, so the match stays in sync either way.
    if (inLiveMatch() && roomPrediction != m_livePrediction)
    {
        m_livePrediction = roomPrediction;
        rmgk_gekko::set_live_prediction_window(roomPrediction);
        appendChatSystemLine(CHANNEL_ROOM, tr("Prediction is now %1 f.").arg(roomPrediction),
                             QStringLiteral("settings-3-line"));
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            QString("Rollback lobby live prediction=%1").arg(roomPrediction).toUtf8().constData());
    }

    // The host owns the room-wide live-replay choice, but every seated player
    // sees its authoritative value. Non-hosts get a disabled checkbox instead
    // of a hidden option. Local saving remains each player's separate choice.
    if (m_broadcastCheck)
    {
        m_suppressSettingsSignal = true;
        m_broadcastCheck->setChecked(liveReplayEnabled);
        m_suppressSettingsSignal = false;
        m_broadcastCheck->setVisible(true);
    }
    updateRecordingControls();

    // ── Seats ──
    QVector<bool> filled(4, false);
    for (const auto& v : players)
    {
        const auto p = v.toObject();
        const int slot = p.value("slot").toInt();
        if (slot < 1 || slot > 4) continue;
        const QString user = p.value("username").toString();
        const quint64 uid = static_cast<quint64>(p.value("userId").toDouble());
        const quint64 peerId = LobbyClient::seatPeerId(p);
        const bool slotIsHost = (uid == hostId);
        // Every seat on this PC: no connection to measure, and the same delay.
        const bool slotIsSelf = (peerId == m_client->selfUserId());
        const int pingMs = slotIsSelf ? 0 : m_client->measuredPingMs(peerId);
        const int frameDelay = slotIsSelf
            ? m_currentRoomDelay
            : p.value("frameDelay").toInt(-1);
        QString connectionType;
        QString country;
        bool anonymous = false;
        const auto userIt = m_client->users().constFind(uid);
        if (userIt != m_client->users().constEnd())
        {
            connectionType = userIt->connection;
            country = userIt->country;
            anonymous = userIt->anonymous;
        }
        // The host can remove any seated player except the ones on their own PC.
        const bool canKick = iAmHost && !slotIsSelf;
        if (m_seats[slot - 1].userId != uid)
        {
            m_seats[slot - 1].iceAttempt = 1;
            m_seats[slot - 1].iceMaxAttempts = 20;
        }
        m_seats[slot - 1].userId = uid;
        m_seats[slot - 1].peerId = peerId;
        renderSeatFilled(m_seats[slot - 1], user, slotIsHost, slotIsSelf,
                         pingMs, frameDelay, canKick, connectionType);
        const QString seatCountry = anonymous ? QString() : country;
        m_seats[slot - 1].row->setCountry(seatCountry, countryTooltip(seatCountry));
        if (!slotIsSelf && pingMs < 0)
            m_seats[slot - 1].row->setLinkConnecting(m_seats[slot - 1].iceAttempt,
                                                     m_seats[slot - 1].iceMaxAttempts);
        // Older servers don't send inMatch; everyone seated then plays.
        m_seats[slot - 1].row->setWaitingToJoin(state == "in_game" && !p.value("inMatch").toBool(true));
        filled[slot - 1] = true;
    }
    for (int i = 0; i < 4; ++i)
    {
        // A seat past the room's size may be one the host just took away.
        if (!filled[i])
            renderSeatEmpty(m_seats[i]);
        m_seats[i].row->setVisible(i < maxPlayers);
    }

    // Seat reorder is host-only and pre-match. Show the drag grip on occupied,
    // visible seats so the host can drag one onto another to swap them.
    m_canReorderSeats = iAmHost && (state == "waiting");
    for (int i = 0; i < 4; ++i)
    {
        m_seats[i].row->setHostTools(
            m_canReorderSeats && i < maxPlayers && m_seats[i].userId != 0,
            m_seats[i].canKick && m_seats[i].userId != 0);
    }
    if (m_seatDraggingSlot != 0 && (!m_canReorderSeats || m_seats[m_seatDraggingSlot - 1].userId == 0))
        endSeatDrag(QPoint(), false);

    // Late join: track who sits outside the running match. Players in the
    // match hear about a newcomer waiting to join; the newcomer learns what
    // happens next.
    {
        const quint64 selfId = m_client->selfUserId();
        QSet<quint64> outOfMatch;
        m_lateJoinCapableUsers.clear();
        m_saveDataCapableUsers.clear();
        m_gameOptionsCapableUsers.clear();
        bool selfOutOfMatch = false;
        for (const auto& v : players)
        {
            const auto p = v.toObject();
            const quint64 uid = static_cast<quint64>(p.value("userId").toDouble());
            if (p.value("lateJoin").toBool())
                m_lateJoinCapableUsers.insert(uid);
            if (p.value("saveData").toBool())
                m_saveDataCapableUsers.insert(uid);
            if (p.value("gameOptions").toBool())
                m_gameOptionsCapableUsers.insert(uid);
            if (state == "in_game" && !p.value("inMatch").toBool(true))
            {
                outOfMatch.insert(uid);
                if (uid == selfId)
                    selfOutOfMatch = true;
            }
        }
        const bool selfPlaying = m_emulationActive || m_awaitingEmulationStart;
        if (m_roomSeatsSeen && selfPlaying && m_roomLateJoin)
        {
            for (const quint64 uid : outOfMatch)
            {
                if (m_client->isOnThisPc(uid) || m_knownSeatedUsers.contains(uid))
                    continue;
                const QString who = lobbyUserName(uid);
                appendChatSystemLine(CHANNEL_ROOM, iAmHost
                    ? tr("%1 is waiting to join. Click Add to Game on their seat to bring them in.").arg(who)
                    : tr("%1 is waiting to join. The host can add them to the game.").arg(who),
                    QStringLiteral("user-add-line"), Role::Warn);
                constexpr qint64 kWaitingNoticeGapMs = 30000;
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                const auto last = m_waitingNoticeMs.constFind(uid);
                if (last != m_waitingNoticeMs.constEnd() && nowMs - *last < kWaitingNoticeGapMs)
                    continue;
                m_waitingNoticeMs.insert(uid, nowMs);
                emit overlayMessage(iAmHost
                    ? tr("%1 is waiting to join (Add to Game in the lobby)").arg(who)
                    : tr("%1 is waiting to join").arg(who));
            }
        }
        if (selfOutOfMatch && m_roomLateJoin && m_lateJoin.role == LateJoinRole::None && !selfPlaying)
        {
            applyRoomStateBadge(tr("Waiting to join"), stateHex("connecting", isDarkTheme()));
            if (!m_announcedWaitingSelf)
            {
                m_announcedWaitingSelf = true;
                appendChatSystemLine(CHANNEL_ROOM,
                    tr("A match is in progress. The host can add you to the game; it starts on its own when they do."),
                    QStringLiteral("time-line"));
            }
        }
        else if (!selfOutOfMatch)
        {
            m_announcedWaitingSelf = false;
        }
        m_outOfMatchUsers = outOfMatch;
        // Joining mid-game means no replay of this one.
        updateRecordingControls();
    }

    // Flash + chime when a *new* player takes a seat. Compare this ROOM_STATE's
    // seated set against the last one; any id that's newly present (and isn't us)
    // is a join. The first ROOM_STATE after entering a room just seeds the set —
    // its occupants were already there, so they don't count as joins.
    {
        QSet<quint64> currentSeated;
        for (const auto& v : players)
        {
            const quint64 uid = static_cast<quint64>(v.toObject().value("userId").toDouble());
            if (uid != 0) currentSeated.insert(uid);
        }
        bool sawNewPeer = false;
        for (const auto& s : m_seats)
        {
            if (s.userId == 0 || m_client->isOnThisPc(s.userId) || m_knownSeatedUsers.contains(s.userId))
                continue;
            // Probe a newly-seated peer immediately so their ping appears within
            // an RTT instead of waiting on the next probe tick — and so this
            // player's Auto delay resolves from real ping before the match begins.
            m_client->requestPingProbe(s.peerId);
            sawNewPeer = true;
        }
        // Chime only once we've settled in: the first ROOM_STATE's occupants were
        // already here, so they don't count as joins.
        if (m_roomSeatsSeen && sawNewPeer)
            notifyPlayerJoined();
        m_knownSeatedUsers = currentSeated;
        m_roomSeatsSeen = true;
    }

    // Start button gating (host-only): waiting, 2+ seated, every pair measured.
    // Re-resolve local Auto after seat changes too: a join/leave can change this
    // player's worst relevant path even before another probe result arrives.
    // Publish a restored fixed choice once the initial room state has arrived.
    if (m_delayAuto || !m_localDelayPublished)
        applyLocalFrameDelay(false);
    refreshStartButton();
    refreshAdmitButtons();
}

void RollbackLobbyDialog::updateRecordingControls()
{
    updateSaveDataControls();
    updateGameOptionControls();
    if (!m_recordCheck || !m_broadcastCheck)
        return;

    const bool iAmHost = m_client && m_currentRoomId != 0 &&
        m_currentRoomHostId == m_client->selfUserId();
    const bool liveReplayEnabled = m_broadcastCheck->isChecked();
    const bool editable = m_currentRoomState == "waiting" && !matchTransportInProgress();

    // Everyone in the room when a game starts can record it. A player who joins
    // mid-game can't: their replay would start part way through, with nothing
    // to start from. The box shows off for them meanwhile and goes back to
    // their own choice afterwards.
    const bool joiningMidGame = m_client && m_outOfMatchUsers.contains(m_client->selfUserId()) &&
        m_currentRoomState == QLatin1String("in_game") && !matchTransportInProgress();
    {
        const QSignalBlocker blocker(m_recordCheck);
        m_recordCheck->setChecked(!joiningMidGame && m_recordLocallyChoice);
    }
    m_recordCheck->setEnabled(editable);
    if (joiningMidGame)
    {
        m_recordCheck->setToolTip(
            tr("You joined while a game is running, so you won't get a replay of this one."));
    }
    else if (!editable)
    {
        m_recordCheck->setToolTip(
            tr("Recording can't be changed during a match."));
    }
    else
    {
        m_recordCheck->setToolTip(
            tr("Record this match to a .krec file on your PC.\n"
                           "Independent of Live Replay — each player chooses whether to save a copy."));
    }

    m_broadcastCheck->setEnabled(iAmHost && editable);
    if (!iAmHost)
    {
        m_broadcastCheck->setToolTip(liveReplayEnabled
            ? tr("The room host will stream this match as a live replay.")
            : tr("The room host has live replay streaming turned off."));
    }
    else if (!editable)
    {
        m_broadcastCheck->setToolTip(
            tr("The live replay choice is locked while a match is running."));
    }
    else
    {
        m_broadcastCheck->setToolTip(
            tr("Let others in the lobby watch this match live.\n"
                           "A local replay is saved only if Save replay is on."));
    }

    if (!m_lateJoinCheck)
        return;
    m_lateJoinCheck->setEnabled(iAmHost && editable);
    if (!iAmHost)
    {
        m_lateJoinCheck->setToolTip(m_roomLateJoinEnabled
            ? tr("The host turned on late join: players can join while a game is running.")
            : tr("The host has late join turned off."));
    }
    else if (!editable)
    {
        m_lateJoinCheck->setToolTip(
            tr("Late join can't be changed during a match."));
    }
    else
    {
        m_lateJoinCheck->setToolTip(
            tr("Let players join while a game is running. You add them with Add to Game."));
    }
}

void RollbackLobbyDialog::updateJoinLimitControls()
{
    if (!m_joinLimitsBox)
        return;
    const bool iAmHost = m_client && m_currentRoomId != 0 && m_currentRoomHostId == m_client->selfUserId();
    m_joinLimitsBox->setVisible(m_roomHasJoinLimits);
    const bool wasSuppressed = m_suppressSettingsSignal;
    m_suppressSettingsSignal = true;
    const int index = m_joinPingCombo->findData(m_roomMaxPing);
    if (index >= 0)
    {
        m_joinPingCombo->setCurrentIndex(index);
    }
    else
    {
        // A limit this client doesn't offer, set by some other client.
        m_joinPingCombo->addItem(CreateRoomDialog::pingLimitText(m_roomMaxPing), m_roomMaxPing);
        m_joinPingCombo->setCurrentIndex(m_joinPingCombo->count() - 1);
    }
    m_wiredOnlyCheck->setChecked(m_roomWiredOnly);
    m_suppressSettingsSignal = wasSuppressed;
    m_joinPingCombo->setEnabled(iAmHost);
    m_wiredOnlyCheck->setEnabled(iAmHost);
    m_joinLimitsHint->setVisible(!iAmHost);
    if (!iAmHost)
        m_joinLimitsHint->setText(m_currentRoomHostName.isEmpty()
            ? tr("Set by the host") : tr("Set by the host (%1)").arg(m_currentRoomHostName));
    if (m_roomSettingsButton)
    {
        // A tinted gear says something under it isn't the default.
        const bool custom = !m_predictionAuto || m_roomMaxPing > 0 || m_roomWiredOnly;
        m_roomSettingsButton->setIconColorOverride(custom ? LobbyTheme::tokens().accentInk : QColor());
    }
}

void RollbackLobbyDialog::updateRoomSizeControls()
{
    if (!m_roomSizeBox)
        return;
    const bool iAmHost = m_client && m_currentRoomId != 0 && m_currentRoomHostId == m_client->selfUserId();
    const bool waiting = m_currentRoomState == QLatin1String("waiting") && !matchTransportInProgress();
    m_roomSizeBox->setVisible(m_roomResizable);
    for (QAbstractButton* button : m_roomSizeGroup->buttons())
    {
        const int seats = m_roomSizeGroup->id(button);
        if (seats == m_roomMaxPlayers)
            button->setChecked(true);
        // Fewer seats than players seated would leave someone without one.
        button->setEnabled(iAmHost && waiting && seats >= m_roomSeatedCount);
    }
    if (!iAmHost)
        m_roomSizeHint->setText(m_currentRoomHostName.isEmpty()
            ? tr("Set by the host")
            : tr("Set by the host (%1)").arg(m_currentRoomHostName));
    else if (!waiting)
        m_roomSizeHint->setText(tr("Can't be changed during a match."));
    else if (m_roomSeatedCount > 2)
        m_roomSizeHint->setText(tr("Not fewer than the %1 players seated.").arg(m_roomSeatedCount));
    else
        m_roomSizeHint->setText(tr("Anyone in a seat you take away moves to a free one."));
}

// ── Save data ─────────────────────────────────────────────────────────
//
// With the room's Save data option on, the host picks the save the next
// match starts from. It reaches every player in the pre-match manifest, and
// during the match the game saves to memory only (the core's save sandbox),
// so no player's own save files are read or changed. Rollbacks restore the
// saves with the rest of the state. Once every peer has confirmed the frames
// after a change, the engine publishes the new save; any player can write it
// to a file, and keep that file updated as the game saves.

void RollbackLobbyDialog::updateSaveDataControls()
{
    if (!m_saveDataRow || !m_saveDataCheck)
        return;

    const bool iAmHost = m_client && m_currentRoomId != 0 &&
        m_currentRoomHostId == m_client->selfUserId();
    const bool editable = m_currentRoomState == "waiting" && !matchTransportInProgress();
    const bool inRoom = m_currentRoomId != 0;

    m_saveDataCheck->setEnabled(iAmHost && editable);
    m_saveDataCheck->setToolTip(!iAmHost
        ? (m_roomSaveDataEnabled ? tr("Matches in this room start from the host's save data.")
                                 : tr("Matches in this room start from a blank save."))
        : !editable ? tr("Save data can't be changed during a match.")
                    : tr("Start the match from a save you pick, for games that need one (unlocks, settings, progress).\n"
                         "The game saves to memory only during the match: nobody's own save files are touched."));

    const bool haveMatchSave = m_matchSave.has_value() && (m_matchSaveActive || m_matchSaveChanged || m_keepCopyEnabled);
    const bool showRow = (inRoom && m_roomSaveDataEnabled) || haveMatchSave;
    m_saveDataRow->setVisible(showRow);
    if (!showRow)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    auto ago = [nowMs](qint64 ms) {
        const qint64 seconds = std::max<qint64>(0, (nowMs - ms) / 1000);
        if (seconds < 60)
            return tr("%n s ago", nullptr, int(seconds));
        return tr("%n min ago", nullptr, int(seconds / 60));
    };

    QStringList parts;
    // The host's label is in the host's language; a blank save reads in ours.
    const QString label = m_roomSaveDataEnabled && m_roomSaveDataKinds.isEmpty() ? tr("Blank save")
        : m_roomSaveDataLabel.isEmpty() ? tr("the host's save") : m_roomSaveDataLabel;
    if (m_matchSaveActive)
    {
        parts << tr("<b>Save data:</b> %1").arg(label.toHtmlEscaped());
        parts << (m_matchSaveChanged ? tr("the game saved %1").arg(ago(m_matchSaveChangedMs))
                                     : tr("not changed yet"));
    }
    else if (m_matchSaveChanged && m_matchSave)
    {
        parts << tr("<b>Save data:</b> the last match changed the save");
        if (iAmHost && m_hostSaveChoice && m_roomSaveDataEnabled)
            parts << (m_hostSaveChoice->ContinueAfterMatch ? tr("the next match continues from it")
                                                           : tr("the next match starts from %1 again").arg(label.toHtmlEscaped()));
    }
    else
    {
        parts << tr("<b>Starting save:</b> %1").arg(label.toHtmlEscaped());
        if (!m_roomSaveDataKinds.isEmpty())
            parts << QStringLiteral("%1 · %2").arg(m_roomSaveDataKinds.join(QStringLiteral(" + ")),
                                                   tr("%1 KB").arg((m_roomSaveDataBytes + 1023) / 1024));
        if (!iAmHost)
            parts << tr("picked by the host");
    }
    if (m_keepCopyEnabled && !m_keepCopyFile.isEmpty())
    {
        const QString name = QFileInfo(m_keepCopyFile).completeBaseName().toHtmlEscaped();
        parts << (m_keepCopyWrittenMs > 0 ? tr("kept in %1 (%2)").arg(name, ago(m_keepCopyWrittenMs))
                                          : tr("kept in %1").arg(name));
    }
    m_saveDataNote->setText(parts.join(QStringLiteral(" · ")));

    m_saveDataChangeBtn->setVisible(iAmHost && editable && m_roomSaveDataEnabled);
    m_saveDataSaveAsBtn->setVisible(m_matchSave.has_value() && (m_matchSaveActive || m_matchSaveChanged));
    m_saveDataKeepCheck->setVisible(m_matchSave.has_value() && m_matchSaveActive);
    {
        const QSignalBlocker blocker(m_saveDataKeepCheck);
        m_saveDataKeepCheck->setChecked(m_keepCopyEnabled);
    }
    m_saveDataKeepCheck->setToolTip(m_keepCopyFile.isEmpty()
        ? tr("Pick a file, then rewrite it each time the game saves.")
        : tr("Rewrite %1 each time the game saves.").arg(QDir::toNativeSeparators(m_keepCopyFile)));
}

bool RollbackLobbyDialog::pickStartingSave()
{
    const QString romFile = localRomPathForMd5(m_currentRoomMd5);
    std::optional<CoreNetplaySaveData> lastMatch;
    if (m_matchSaveChanged && m_matchSave)
        lastMatch = m_matchSave;

    StartingSaveDialog dialog(romFile, m_currentRoomMd5, m_currentRoomGame, m_hostSaveChoice, lastMatch, this);
    dialog.setCoverPath(coverPathForMd5(m_currentRoomMd5));
    if (dialog.exec() != QDialog::Accepted)
        return false;
    // The room may have moved on while the dialog was open.
    if (m_currentRoomId == 0 || !m_client || m_currentRoomHostId != m_client->selfUserId() ||
        m_currentRoomState != "waiting")
        return false;

    m_hostSaveChoice = dialog.choice();
    return true;
}

void RollbackLobbyDialog::publishHostSaveChoice()
{
    if (!m_client || !m_hostSaveChoice)
        return;
    const CoreNetplaySaveData& data = m_hostSaveChoice->Data;
    m_client->updateRoomSaveData(true, m_hostSaveChoice->Label, netplaySaveKindLabels(data),
                                 int(data.TotalBytes()), netplaySaveHash(data));
}

void RollbackLobbyDialog::beginMatchSave()
{
    // The manifest just applied staged the starting save (blank without one).
    m_matchSaveActive = m_roomSaveDataEnabled || !CoreGetNetplaySaveData().IsEmpty();
    if (!m_matchSaveActive)
    {
        updateSaveDataControls();
        return;
    }

    m_matchSave = CoreGetNetplaySaveData();
    m_matchSaveChanged = false;
    m_matchSaveChangedMs = 0;
    m_keepCopyWrittenMs = 0;
    if (!m_saveDataTimer)
    {
        m_saveDataTimer = new QTimer(this);
        m_saveDataTimer->setInterval(500);
        connect(m_saveDataTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onSaveDataTick);
    }
    m_saveDataTimer->start();
    updateSaveDataControls();
}

void RollbackLobbyDialog::onSaveDataTick()
{
    CoreNetplaySaveData data;
    uint32_t generation = 0;
    if (CoreTakeConfirmedSaveData(data, generation))
    {
        m_matchSave = std::move(data);
        m_matchSaveChanged = true;
        m_matchSaveChangedMs = QDateTime::currentMSecsSinceEpoch();
        if (m_keepCopyEnabled && !m_keepCopyFile.isEmpty())
        {
            QString error;
            if (!writeMatchSave(m_keepCopyFile, &error))
            {
                m_keepCopyEnabled = false;
                appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't update %1 (%2). Keep updated is off.")
                    .arg(QDir::toNativeSeparators(m_keepCopyFile), error));
            }
        }
    }
    updateSaveDataControls();
}

void RollbackLobbyDialog::finishMatchSave()
{
    if (!m_matchSaveActive)
        return;
    onSaveDataTick();
    if (m_saveDataTimer)
        m_saveDataTimer->stop();
    m_matchSaveActive = false;
    // Nothing after this match may boot with its save by accident.
    CoreClearNetplaySaveData();

    const bool iAmHost = m_client && m_currentRoomId != 0 &&
        m_currentRoomHostId == m_client->selfUserId();
    if (m_matchSaveChanged && m_matchSave)
    {
        if (m_keepCopyEnabled && !m_keepCopyFile.isEmpty())
            appendChatSystemLine(CHANNEL_ROOM, tr("The game's save is kept in %1.")
                .arg(QDir::toNativeSeparators(m_keepCopyFile)));
        else
            appendChatSystemLine(CHANNEL_ROOM, tr("The game saved during this match. Use Save As to keep a copy."));

        // The next match picks up where this one left off.
        if (iAmHost && m_hostSaveChoice && m_hostSaveChoice->ContinueAfterMatch && m_roomSaveDataEnabled)
        {
            QString label = m_hostSaveChoice->Label;
            const QString suffix = tr(" (continued)");
            if (!label.endsWith(suffix))
                label += suffix;
            m_hostSaveChoice->Data = *m_matchSave;
            m_hostSaveChoice->Label = label;
            m_hostSaveChoice->File.clear();
            // The room may already be waiting again: our game can close after
            // the server's ROOM_STATE says so.
            if (m_currentRoomState == "waiting")
                publishHostSaveChoice();
            else
                m_publishSaveChoiceWhenWaiting = true;
        }
    }
    updateSaveDataControls();
}

void RollbackLobbyDialog::saveMatchSaveAs()
{
    if (!m_matchSave)
        return;
    SaveNetplaySaveDialog dialog(m_currentRoomGame, m_keepCopyFile, m_keepCopyEnabled || m_keepCopyFile.isEmpty(), this);
    dialog.setCoverPath(coverPathForMd5(m_currentRoomMd5));
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString file = dialog.filePath();
    QString error;
    if (!writeMatchSave(file, &error))
    {
        QMessageBox::warning(this, tr("Save Game Save"), tr("Couldn't save %1: %2")
            .arg(QDir::toNativeSeparators(file), error));
        return;
    }
    m_keepCopyFile = file;
    m_keepCopyEnabled = dialog.keepUpdated() && m_matchSaveActive;
    appendChatSystemLine(CHANNEL_ROOM, m_keepCopyEnabled
        ? tr("Saved to %1. It's kept updated while you play.").arg(QDir::toNativeSeparators(file))
        : tr("Saved to %1.").arg(QDir::toNativeSeparators(file)));
    updateSaveDataControls();
}

bool RollbackLobbyDialog::writeMatchSave(const QString& file, QString* error)
{
    if (!m_matchSave)
        return false;

    CoreNetplaySaveInfo info;
    info.Name = QFileInfo(file).completeBaseName().toStdString();
    info.GoodName = m_currentRoomGame.toStdString();
    info.RomMD5 = m_currentRoomMd5.toStdString();
    info.CreatedUnix = QDateTime::currentSecsSinceEpoch();
    for (const auto& seat : m_seats)
    {
        if (seat.userId != 0)
            info.Players.push_back(lobbyUserName(seat.userId).toStdString());
    }
    if (!CoreWriteNetplaySaveFile(file.toStdU32String(), *m_matchSave, info))
    {
        if (error)
            *error = QString::fromStdString(CoreGetError());
        return false;
    }
    m_keepCopyWrittenMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

// ── Game options ──────────────────────────────────────────────────────
//
// The host turns on options the game options packs offer for the room's
// game (say, Vanilla only for Smash Remix). The server only keeps the list,
// for the room and the room list. At match start the host's client resolves
// them into ROM patches for the manifest, and every player, late joiner, live
// viewer and replay patches its game the same way, so only the host needs
// the pack. Everyone else sees the host's labels, or their own pack's.

QStringList RollbackLobbyDialog::gameOptionLabels(const QString& romMd5,
                                                  const QList<LobbyClient::LobbyGameOption>& options) const
{
    QStringList labels;
    for (const auto& option : options)
    {
        const std::optional<CoreGameOptionInfo> own = ownGameOption(romMd5, option.id);
        labels << (own ? QString::fromStdString(own->Label) : option.label);
    }
    return labels;
}

QString RollbackLobbyDialog::gameOptionsTip(const QString& romMd5,
                                            const QList<LobbyClient::LobbyGameOption>& options) const
{
    QStringList lines;
    for (const auto& option : options)
    {
        const std::optional<CoreGameOptionInfo> own = ownGameOption(romMd5, option.id);
        if (own && !own->Description.empty())
            lines << QStringLiteral("%1: %2").arg(QString::fromStdString(own->Label), QString::fromStdString(own->Description));
        else
            lines << (own ? QString::fromStdString(own->Label) : option.label);
    }
    return lines.join(QLatin1Char('\n'));
}

void RollbackLobbyDialog::updateGameOptionControls()
{
    if (!m_sessionSwitches)
        return;
    const bool iAmHost = m_client && m_currentRoomId != 0 && m_currentRoomHostId == m_client->selfUserId();
    const bool editable = m_currentRoomState == "waiting" && !matchTransportInProgress();

    // This client's pack's options for the room's game, then any the host
    // turned on that it doesn't know. Quick match and older servers get none.
    struct Shown
    {
        QString id, label, description;
        bool on = false;
    };
    QList<Shown> shown;
    const auto isOn = [this](const QString& id) {
        return std::any_of(m_roomGameOptions.cbegin(), m_roomGameOptions.cend(),
                           [&id](const auto& option) { return option.id == id; });
    };
    if (m_currentRoomId != 0 && m_roomGameOptionsOffered)
    {
        for (const CoreGameOptionInfo& info : CoreGetGameOptions(m_currentRoomMd5.toStdString(), gameOptionLanguage()))
        {
            const QString id = QString::fromStdString(info.Id);
            shown.append({ id, QString::fromStdString(info.Label), QString::fromStdString(info.Description), isOn(id) });
        }
        for (const auto& option : m_roomGameOptions)
        {
            if (std::none_of(shown.cbegin(), shown.cend(), [&option](const Shown& s) { return s.id == option.id; }))
                shown.append({ option.id, option.label, QString(), true });
        }
    }

    // A switch each, in the rows below Late join and Save data.
    bool same = shown.size() == m_gameOptionChecks.size();
    for (int i = 0; same && i < shown.size(); ++i)
        same = m_gameOptionChecks.at(i)->property("gameOptionId").toString() == shown.at(i).id;
    if (!same)
    {
        for (ToggleSwitch* check : std::as_const(m_gameOptionChecks))
        {
            m_sessionSwitches->removeWidget(check);
            check->deleteLater();
        }
        m_gameOptionChecks.clear();
        QWidget* parent = m_saveDataCheck ? m_saveDataCheck->parentWidget() : nullptr;
        for (int i = 0; i < shown.size(); ++i)
        {
            auto* check = new ToggleSwitch(shown.at(i).label, parent);
            check->setObjectName(QStringLiteral("GameOptionSwitch"));
            check->setProperty("gameOptionId", shown.at(i).id);
            const QString id = shown.at(i).id;
            connect(check, &QCheckBox::toggled, this, [this, id](bool checked) { onGameOptionToggled(id, checked); });
            m_sessionSwitches->addWidget(check, 2 + i / 2, i % 2);
            m_gameOptionChecks.append(check);
        }
    }

    for (int i = 0; i < shown.size(); ++i)
    {
        const Shown& s = shown.at(i);
        ToggleSwitch* check = m_gameOptionChecks.at(i);
        {
            const QSignalBlocker blocker(check);
            check->setChecked(s.on);
        }
        check->setText(s.label);
        check->setEnabled(iAmHost && editable);
        // What it does (from this client's pack), then who decides.
        const QString who = !iAmHost ? (s.on ? tr("The host turned on %1.").arg(s.label)
                                             : tr("The host has %1 turned off.").arg(s.label))
                          : !editable ? tr("Game options can't be changed during a match.")
                                      : tr("Every player's game is changed for the match, and so is its replay.");
        check->setToolTip(s.description.isEmpty() ? who : s.description + QLatin1Char('\n') + who);
        check->setVisible(true);
    }
}

void RollbackLobbyDialog::onGameOptionToggled(const QString& id, bool checked)
{
    if (m_suppressSettingsSignal || m_currentRoomId == 0 || !m_client ||
        m_currentRoomHostId != m_client->selfUserId() || m_currentRoomState != "waiting")
        return;

    // The room's list with this one added or taken out, in pack order, with
    // the English labels: other clients show their own pack's when they have it.
    QList<LobbyClient::LobbyGameOption> list;
    std::vector<std::string> ids;
    QString label;
    for (const CoreGameOptionInfo& info : CoreGetGameOptions(m_currentRoomMd5.toStdString(), gameOptionLanguage()))
    {
        const QString infoId = QString::fromStdString(info.Id);
        if (infoId == id)
            label = QString::fromStdString(info.Label);
        const bool on = infoId == id ? checked
            : std::any_of(m_roomGameOptions.cbegin(), m_roomGameOptions.cend(),
                          [&infoId](const auto& option) { return option.id == infoId; });
        if (!on)
            continue;
        // The server takes labels of up to 48 characters.
        QString english = QString::fromStdString(info.EnglishLabel);
        if (english.size() > 48)
            english.truncate(english.at(47).isHighSurrogate() ? 47 : 48);
        list.append({ infoId, english });
        ids.push_back(info.Id);
    }

    // Checked against the ROM now, so a ROM the pack doesn't fit says so
    // before anyone joins expecting it; the match start checks again.
    if (checked)
    {
        CoreNetplayGameOptions resolved;
        if (!CoreResolveGameOptions(localRomPathForMd5(m_currentRoomMd5).toStdU32String(),
                                    m_currentRoomMd5.toStdString(), ids, resolved))
        {
            const QString why = QString::fromStdString(CoreGetError());
            updateGameOptionControls();
            QMessageBox box(QMessageBox::Warning, tr("Game Options"),
                            tr("Couldn't turn on %1.").arg(label), QMessageBox::Ok, this);
            box.setInformativeText(why);
            box.exec();
            return;
        }
    }
    m_client->updateRoomGameOptions(list);
    // The switches follow the room's ROOM_STATE. The server refuses a change
    // without a word, so they go back to what the room has if none comes.
    m_gameOptionsResync->start();
}

void RollbackLobbyDialog::announceMatchGameOptions(const QString& channel, const QString& romMd5)
{
    QList<LobbyClient::LobbyGameOption> options;
    for (const CoreNetplayGameOption& option : CoreGetNetplayGameOptions().Options)
        options.append({ QString::fromStdString(option.Id), QString::fromStdString(option.Label) });
    if (!options.isEmpty())
        appendChatSystemLine(channel, tr("This match is played with %1.").arg(
            gameOptionLabels(romMd5, options).join(QStringLiteral(", ")).toHtmlEscaped()));
}

void RollbackLobbyDialog::refreshStartButton()
{
    if (!m_startBtn)
        return;

    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    const bool iAmHost = (m_client && m_currentRoomHostId == selfId);
    const bool waiting = (m_currentRoomState == "waiting");
    const bool inGame = (m_currentRoomState == "in_game");
    const bool playing = m_emulationActive || m_awaitingEmulationStart;

    // Seats may be sparse — two players in P1 and P3 is a legitimate 3-port
    // match with P2 empty, so this counts occupants without requiring them to
    // run contiguously from P1. The core sizes the GekkoNet session by the
    // highest occupied seat and feeds empty ports zeroed input.
    int seated = 0;
    bool otherPcSeated = false;
    QList<quint64> seatedPeerIds;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0)
            continue;
        ++seated;
        otherPcSeated = otherPcSeated || s.peerId != selfId;
        seatedPeerIds.append(s.peerId);
    }

    // With nobody on another PC there are no paths to measure. Players on this
    // PC are enough for a match; one player alone is only allowed when whoever
    // joins later can be added to the running match.
    const bool alone = seated >= 1 && !otherPcSeated && m_canStartAlone;
    const bool enoughPlayers = otherPcSeated || seated >= 2 || alone;
    bool pingsReady = !otherPcSeated;
    if (m_client && otherPcSeated)
        m_client->worstRoomPingMs(seatedPeerIds, &pingsReady);
    // Every player's client must take the save, or the match can't start.
    QStringList needSaveData;
    if (m_roomSaveDataEnabled)
    {
        for (const auto& s : m_seats)
        {
            if (s.userId != 0 && !m_saveDataCapableUsers.contains(s.userId))
                needSaveData << lobbyUserName(s.userId);
        }
    }
    // Likewise the game options: their game has to be changed the same way.
    QStringList needGameOptions;
    const QString gameOptionNames = gameOptionLabels(m_currentRoomMd5, m_roomGameOptions).join(QStringLiteral(", "));
    if (!m_roomGameOptions.isEmpty())
    {
        for (const auto& s : m_seats)
        {
            if (s.userId != 0 && !m_gameOptionsCapableUsers.contains(s.userId))
                needGameOptions << lobbyUserName(s.userId);
        }
    }
    const bool canStart      = iAmHost && waiting && enoughPlayers && pingsReady && needSaveData.isEmpty() &&
                               needGameOptions.isEmpty();

    m_startBtn->setEnabled(canStart);
    m_startBtn->setText(alone && seated == 1 ? tr("Start alone") : tr("Start Game"));
    m_startBtn->setVisible(iAmHost && waiting);
    m_startBtn->setToolTip(
        !iAmHost         ? tr("Only the host can start the game.")
        : !waiting       ? tr("Already in a match.")
        : !needSaveData.isEmpty() ? tr("This room uses save data, which %1 can't play with yet. "
                                       "They need a newer Dale64, or turn off Sync save data.")
                                       .arg(needSaveData.join(QStringLiteral(", ")))
        : !needGameOptions.isEmpty() ? tr("This room uses %1, which %2 can't play with yet. "
                                          "They need a newer Dale64, or turn %1 off.")
                                          .arg(gameOptionNames, needGameOptions.join(QStringLiteral(", ")))
        : !enoughPlayers ? (seated == 1 && m_roomHasLateJoinOption && !m_roomLateJoinEnabled
                              ? tr("Need at least 2 players to start. To start alone, turn on Allow joins after start.")
                              : tr("Need at least 2 players to start."))
        : !pingsReady    ? tr("Measuring every player-to-player path…")
        : alone          ? tr("Start playing now. You can add anyone who joins the room to the game.")
                         : QString());

    // Once this client is in the match, Drop is the call to action and leaving
    // steps back; before that, leaving is the only thing here that costs you.
    m_dropBtn->setVisible(inGame && playing);
    // Lag stats stay up after the match, for a look back at what happened.
    m_lagStatsBtn->setVisible((inGame && playing) || (m_currentRoomId != 0 && m_lagStatsRoomId == m_currentRoomId));
    m_leaveBtn->setKind(inGame && playing ? LobbyButton::Kind::Ghost : LobbyButton::Kind::Danger);

    const Tokens& t = LobbyTheme::tokens();
    const auto showWhy = [&](const QString& icon, const QColor& ink, const QColor& fill,
                             const QString& title, const QString& detail) {
        m_whySpinner->setVisible(icon.isEmpty());
        m_whyBadge->setVisible(!icon.isEmpty());
        if (!icon.isEmpty())
            m_whyBadge->setIconAndColors(icon, ink, fill);
        else
            m_whySpinner->setColors(fill, ink);
        m_whyTitle->setText(title);
        m_whyTitle->setToolTip(detail.isEmpty() ? title : title + QStringLiteral("\n") + detail);
    };

    if (inGame)
    {
        quint64 waitingUser = 0;
        for (const auto& s : m_seats)
        {
            if (s.userId != 0 && m_outOfMatchUsers.contains(s.userId) && m_lateJoinCapableUsers.contains(s.userId))
            {
                waitingUser = s.userId;
                break;
            }
        }
        const bool selfWaiting = m_outOfMatchUsers.contains(selfId);
        if (selfWaiting && m_roomLateJoin)
            showWhy(QString(), t.warn, t.warnTint, tr("A match is in progress"),
                    tr("The host can add you to the game. It starts on its own when they do."));
        else if (iAmHost && playing && waitingUser != 0 && m_roomLateJoin)
            showWhy(QStringLiteral("user-add-line"), t.warn, t.warnTint,
                    tr("%1 is waiting to join").arg(lobbyUserName(waitingUser)),
                    tr("Add them and everyone pauses for a moment while they load in."));
        else if (playing)
            showWhy(QStringLiteral("gamepad-line"), t.live, t.liveTint, tr("You're in the match"),
                    iAmHost ? tr("Dropping ends the match for everyone in the room.")
                            : tr("Dropping takes you out. The other players keep playing."));
        else
            showWhy(QStringLiteral("gamepad-line"), t.live, t.liveTint, tr("A match is in progress"),
                    tr("It's playing without you. You'll be back here when it ends."));
    }
    else if (m_currentRoomState == "starting")
    {
        showWhy(QString(), t.warn, t.warnTint, tr("Starting…"), tr("Getting everyone connected."));
    }
    else if (iAmHost && waiting)
    {
        if (!needSaveData.isEmpty())
        {
            showWhy(QStringLiteral("error-warning-line"), t.warn, t.warnTint,
                    tr("%1 can't play with save data").arg(needSaveData.join(QStringLiteral(", "))),
                    tr("They need a newer Dale64, or turn off Sync save data."));
        }
        else if (!needGameOptions.isEmpty())
        {
            showWhy(QStringLiteral("error-warning-line"), t.warn, t.warnTint,
                    tr("%1 can't play with %2").arg(needGameOptions.join(QStringLiteral(", ")), gameOptionNames),
                    tr("They need a newer Dale64, or turn %1 off.").arg(gameOptionNames));
        }
        else if (!enoughPlayers)
        {
            showWhy(QStringLiteral("time-line"), t.muted, t.tint, tr("Waiting for players"),
                    tr("Start unlocks when someone joins."));
        }
        else if (!pingsReady)
        {
            QString slowest;
            for (const auto& s : m_seats)
            {
                if (s.userId != 0 && s.peerId != selfId && m_client->measuredPingMs(s.peerId) < 0)
                {
                    slowest = s.username;
                    break;
                }
            }
            showWhy(QString(), t.warn, t.warnTint,
                    slowest.isEmpty() ? tr("Measuring connections") : tr("Waiting for %1's connection").arg(slowest),
                    tr("Start unlocks when every player-to-player path has a ping."));
        }
        else
        {
            m_whySpinner->hide();
            m_whyBadge->hide();
            m_whyTitle->setText(QString());
            m_whyTitle->setToolTip(QString());
        }
    }
    else
    {
        showWhy(QStringLiteral("time-line"), t.muted, t.tint, tr("Waiting for the match to start"),
                tr("It launches by itself when it does."));
    }
}

void RollbackLobbyDialog::onDropGameClicked()
{
    if (m_currentRoomState != "in_game")
        return;

    // No local "you dropped" line — the server announces the drop as a chat from
    // this player (see broadcastMatchPeerLeft) and echoes it back to us, so every
    // seat (including ours) shows the same "<name>: I dropped from the game." line.
    emit closeMatchRequested();
    if (m_currentMatchId != 0)
        m_client->reportMatchFinished(m_currentMatchId);
}

void RollbackLobbyDialog::onLeaveRoomClicked()
{
    if (m_emulationActive || m_awaitingEmulationStart)
    {
        emit closeMatchRequested();
        if (m_currentMatchId != 0)
            m_client->reportMatchFinished(m_currentMatchId);
    }
    m_client->leaveRoom();
}

void RollbackLobbyDialog::notifyEmulationStarted()
{
    m_localGameOpen = true;
    if (!m_awaitingEmulationStart) return;
    m_awaitingEmulationStart = false;
    m_emulationActive = true;
    updateRollbackSettingControls();
    refreshStartButton();

    // Emulation is live now, so retire the "Connecting…" transient that
    // onMatchBegin painted over the badge. The room is in_game on the server;
    // mirror that authoritative state (don't assume — read m_currentRoomState).
    applyRoomStateBadge(roomStateLabel(m_currentRoomState),
                        stateHex(m_currentRoomState, isDarkTheme()));

    if (m_currentMatchId == 0) return;

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Lobby→notifyEmulationStarted matchId=%llu",
            (unsigned long long)m_currentMatchId);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }
    if (!m_matchWatchTimer)
    {
        m_matchWatchTimer = new QTimer(this);
        m_matchWatchTimer->setInterval(1000);
        connect(m_matchWatchTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onMatchWatchTick);
    }
    m_matchWatchEpoch = -1;
    m_matchWatchSeenSlots.clear();
    m_matchWatchTimer->start();
    refreshAdmitButtons();
    if (m_lateJoin.role == LateJoinRole::Joiner)
        return;
    m_client->reportMatchConnected(m_currentMatchId, 0);
    appendChatSystemLine(CHANNEL_ROOM, tr("Match started — playing now."));
}

void RollbackLobbyDialog::notifyEmulationFinished()
{
    m_localGameOpen = false;
    // Always close out a broadcast if one is running, even on paths that fall
    // through the early-return below (it flushes the tail + sends BROADCAST_END).
    stopBroadcast();

    const bool ownedMatchTransport = m_client && m_client->matchTransportActive();
    if (!m_emulationActive && !m_awaitingEmulationStart && !ownedMatchTransport) return;
    const quint64 matchId = m_currentMatchId;
    m_emulationActive = false;
    m_awaitingEmulationStart = false;
    if (m_matchWatchTimer)
        m_matchWatchTimer->stop();
    endLiveRollbackSettings();
    finishMatchSave();
    // Nothing after this match may boot with its game options by accident.
    CoreClearNetplayGameOptions();
    if (m_client)
        m_client->setMatchTransportActive(false);
    updateRecordingControls();

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Lobby→notifyEmulationFinished matchId=%llu",
            (unsigned long long)matchId);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }
    if (matchId != 0)
        m_client->reportMatchFinished(matchId);
    m_currentMatchId = 0;
    emit liveReplayViewerCountCleared();
    if (m_dropBtn) m_dropBtn->setEnabled(false);
    // A hand-off can't outlive our game. A joiner's game starts at launch;
    // before that, the game that closed is the one it replaces.
    if (m_lateJoin.role != LateJoinRole::None &&
        (m_lateJoin.role != LateJoinRole::Joiner || m_lateJoin.launched))
        abortLateJoin(QStringLiteral("game_closed"), true);
    refreshAdmitButtons();

    // A disconnect prompt suppressed during play becomes safe once the game no
    // longer owns ICE. Offer reconnection now without ever stealing focus from
    // active emulation.
    if (m_client && isVisible() && !m_connectPromptOpen &&
        (m_client->state() == LobbyClient::ConnectionState::Disconnected ||
         m_client->state() == LobbyClient::ConnectionState::Failed))
    {
        QTimer::singleShot(0, this, [this]() {
            if (!isVisible() || m_connectPromptOpen || matchTransportInProgress() ||
                m_client->state() == LobbyClient::ConnectionState::Connected)
                return;
            const QString message = m_connectPromptMessage;
            m_connectPromptMessage.clear();
            promptForUsername(message);
        });
    }

}

// A game that has lost its match plays on alone, and while it runs it holds
// on to the lobby: the player can't reconnect, or be added back. End it once
// that has happened:
//  - Off the lobby, the server has dropped us from the match, and the other
//    players stop sending us input, so the game stands still. (When the lobby
//    itself restarts, everyone keeps playing.)
//  - Every other player has gone, after GekkoNet's long silence timeout,
//    without the lobby saying so. The host can play on and add players.
void RollbackLobbyDialog::onMatchWatchTick()
{
    if (!m_emulationActive)
    {
        m_matchWatchTimer->stop();
        return;
    }
    if (m_lateJoin.role != LateJoinRole::None)
        return;

    RollbackLagStats stats;
    rmgk_gekko::lag_stats(stats);
    if (!stats.valid || !stats.active || stats.peers.empty())
        return;

    // Peers start out unconnected in every session, including the one a late
    // join moves the match to.
    const int epoch = rmgk_gekko::session_epoch();
    if (epoch != m_matchWatchEpoch)
    {
        m_matchWatchEpoch = epoch;
        m_matchWatchSeenSlots.clear();
    }
    bool anyConnected = false;
    for (const auto& peer : stats.peers)
    {
        if (!peer.connected)
            continue;
        anyConnected = true;
        m_matchWatchSeenSlots.insert(peer.slot);
    }
    const bool everyoneGone = !anyConnected && !m_matchWatchSeenSlots.isEmpty();

    constexpr int kOfflineDroppedStallMs = 10000;
    const bool lobbyUp = m_client && m_client->state() == LobbyClient::ConnectionState::Connected;
    const bool iAmHost = m_client && m_currentRoomHostId == m_client->selfUserId();
    if (lobbyUp && !(everyoneGone && !iAmHost))
        return;
    if (!lobbyUp && !everyoneGone && stats.stalledMs < kOfflineDroppedStallMs)
        return;

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Lobby→match watch: closing the game lobby=%d everyone_gone=%d stalled_ms=%d",
            int(lobbyUp), int(everyoneGone), stats.stalledMs);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }
    m_matchWatchTimer->stop();
    if (lobbyUp)
    {
        appendChatSystemLine(CHANNEL_ROOM,
            tr("Lost the connection to every other player, so your game was closed. The host can add you back."));
        if (m_currentMatchId != 0)
            m_client->reportMatchFinished(m_currentMatchId);
    }
    else
    {
        // Shown by the reconnect prompt once the game has closed.
        m_connectPromptMessage =
            tr("You were dropped from the match when the connection to the lobby was lost. "
               "Reconnect and the host can add you back.");
    }
    emit closeMatchRequested();
}

void RollbackLobbyDialog::onMatchPeerLeft(quint64 matchId, quint64 userId, int slot, const QString& reason)
{
    // A late notice from an earlier match must not drop whoever holds that
    // slot in this one. Members waiting to join hear about every drop in the
    // room; only the match we play in, or are being added to, matters. A
    // joiner has no current match until it launches.
    const quint64 ourMatchId = m_currentMatchId != 0 ? m_currentMatchId : m_lateJoin.matchId;
    if (matchId == 0 || matchId != ourMatchId)
        return;

    // A player leaving mid hand-off is left out of the new session.
    if (m_lateJoin.role != LateJoinRole::None)
    {
        m_lateJoin.left.insert(userId);
        m_lateJoin.awaitingCut.remove(userId);
    }

    // Lobby matches leave player drops to this notice (GekkoNet's own silence
    // timeout is only a 30 s fallback): tell the rollback engine to substitute
    // idle input for them. Every seat gets the same notice, so all peers drop
    // the same player. The request is queued and applied on the emulation
    // thread at the next frame.
    if (slot > 0)
        rmgk_gekko::request_disconnect_player(slot);

    // The port is empty now. The replay's header has already been written, so
    // the replay and its live viewers learn it from a port change record.
    if (slot >= 1 && slot <= 4)
    {
        n02::recordingQueuePortChange(slot, recording_player_names[slot - 1], false);
        recording_player_names[slot - 1][0] = '\0';
        emit portLabelsChanged();
    }

    QString who = QString::number(userId);
    if (m_client)
    {
        const auto& users = m_client->users();
        const auto it = users.find(userId);
        if (it != users.end() && !it->username.isEmpty())
            who = it->username;
    }
    const QString suffix = reason.isEmpty() ? QString(tr("left")) : reason;
    const QString line = slot > 0
        ? QString(tr("%1 (P%2) dropped (%3) — controller %2 will go idle."))
              .arg(who).arg(slot).arg(suffix)
        : QString(tr("%1 dropped (%2).")).arg(who, suffix);
    appendChatSystemLine(CHANNEL_ROOM, line);
}

void RollbackLobbyDialog::onStartGameClicked()
{
    // TODO: server-side ROOM_START handler not yet implemented; this currently
    // sends the request but won't do anything until we wire MATCH_BEGIN.
    m_client->startRoom();
}

void RollbackLobbyDialog::onPingProbeTick()
{
    if (!m_client || m_client->state() != LobbyClient::ConnectionState::Connected)
        return;

    const quint64 selfId = m_client->selfUserId();

    // Seated peers refresh every tick — Start gating and the Auto frame delay
    // hang off these measurements.
    if (m_currentRoomId != 0)
    {
        for (const auto& s : m_seats)
        {
            if (s.userId == 0 || s.peerId == selfId)
                continue;
            m_client->requestPingProbe(s.peerId);
        }
    }

    // Everyone outside the room is measured on demand — see probeOnDemand.
    // The old slow round-robin probed a few lobby users per tick so a hover
    // might find a real number, but in a busy lobby most rows still showed the
    // region estimate and the whole lobby traded probe traffic continuously to
    // populate values nobody looked at.
}

// Measure a specific peer because the user asked to see their ping, then let
// onPingMeasured repaint whatever row is showing it. Throttled per user: the
// server drops requests over its own flood budget, and a double-click or a
// click-drag across rows would otherwise burn that budget on one peer and
// starve the seat refresh that Start gating depends on.
void RollbackLobbyDialog::probeOnDemand(quint64 userId)
{
    if (!m_client || userId == 0 || userId == m_client->selfUserId())
        return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
        return;

    constexpr qint64 kOnDemandProbeCooldownMs = 3'000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_lastOnDemandProbe.constFind(userId);
    if (it != m_lastOnDemandProbe.constEnd() && now - it.value() < kOnDemandProbeCooldownMs)
        return;
    m_lastOnDemandProbe[userId] = now;

    m_client->requestPingProbe(userId);
}

// Back to what the seat is known to have: its ping, or its connection attempt.
void RollbackLobbyDialog::refreshSeatLink(quint64 userId)
{
    for (auto& s : m_seats)
    {
        if (s.peerId != userId || !s.row)
            continue;
        const bool isSelf = m_client && userId == m_client->selfUserId();
        const int pingMs = m_client ? m_client->measuredPingMs(userId) : -1;
        if (isSelf)
            s.row->setLinkLocal();
        else if (pingMs >= 0)
            s.row->setLinkPing(pingMs);
        else
            s.row->setLinkConnecting(s.iceAttempt, s.iceMaxAttempts);
    }
}

void RollbackLobbyDialog::setSeatLinkStatus(quint64 userId, const QString& text, Role role)
{
    for (auto& s : m_seats)
    {
        if (s.peerId == userId && s.row)
            s.row->setLinkStatus(text, role);
    }
}

void RollbackLobbyDialog::onIcePeerConnectionChanged(quint64 userId, bool connected, bool failed)
{
    if (!m_client || userId == m_client->selfUserId())
        return;

    // Keep connection progress visible until the first RTT replaces it, but
    // do not leave a peer looking busy after ICE has exhausted every pair.
    // Failure supersedes a cached RTT because that old measurement no longer
    // represents a usable direct connection.
    if (failed)
        setSeatLinkStatus(userId, QCoreApplication::translate("RollbackLobbyDialog", "connection failed"), Role::Bad);
    // Once ICE connects, probe immediately rather than waiting up to three
    // seconds for the periodic refresh timer.
    else if (m_client->measuredPingMs(userId) < 0)
        refreshSeatLink(userId);
    if (connected)
        m_client->requestPingProbe(userId);
}

void RollbackLobbyDialog::onIcePeerConnectionAttemptChanged(
    quint64 userId, int attempt, int maxAttempts)
{
    if (!m_client || userId == m_client->selfUserId())
        return;
    for (auto& seat : m_seats)
    {
        if (seat.peerId != userId)
            continue;
        seat.iceMaxAttempts = std::max(1, maxAttempts);
        seat.iceAttempt = std::clamp(attempt, 1, seat.iceMaxAttempts);
        // A fresh ICE generation invalidates the usefulness of any cached ping
        // until this new path connects and is measured.
        if (seat.row)
            seat.row->setLinkConnecting(seat.iceAttempt, seat.iceMaxAttempts);
    }
}

void RollbackLobbyDialog::onRoomPingMeasurementsChanged()
{
    // Only the host receives non-host path reports. They complete the matrix
    // required to enable Start, but no longer affect anyone's local delay.
    refreshStartButton();
}

void RollbackLobbyDialog::onPingProbeRetrying(quint64 userId, int attempt, int maxAttempts)
{
    // Retry progress is first-contact feedback. Once a seat has a number the
    // number *is* the display — a routine re-check that needs a second burst
    // (typical right after a match, while the peer reclaims their socket)
    // shouldn't churn it through amber every time.
    if (m_client && m_client->measuredPingMs(userId) >= 0)
        return;
    setSeatLinkStatus(userId,
        QCoreApplication::translate("RollbackLobbyDialog", "retrying %2/%3…").arg(attempt).arg(maxAttempts),
        Role::Warn);
}

void RollbackLobbyDialog::onPingProbeSoftFailed(quint64 userId)
{
    // Clear any retry text; with no status the seat renders the cached
    // measurement. The client escalates to onPingProbeFailed if the next
    // series also dies, so a real outage still surfaces within two cycles.
    refreshSeatLink(userId);
}

void RollbackLobbyDialog::onPingProbeFailed(quint64 userId)
{
    setSeatLinkStatus(userId, QCoreApplication::translate("RollbackLobbyDialog", "unreachable"), Role::Bad);

    // Only announce for a seated player, and only once per room visit — the
    // seat itself carries the ongoing state, and Start stays disabled anyway.
    // Without this the host just sees a Start button that never lights up.
    bool seated = false;
    for (const auto& s : m_seats)
        seated = seated || (s.userId == userId);
    if (!seated || m_probeFailureAnnounced.contains(userId))
        return;
    m_probeFailureAnnounced.insert(userId);

    QString name = QString::number(userId);
    if (m_client)
    {
        const auto& users = m_client->users();
        const auto it = users.constFind(userId);
        if (it != users.constEnd())
            name = it->username;
    }
    appendChatSystemLine(CHANNEL_ROOM,
        QString(tr("Couldn't reach %1 to measure ping — their connection may not allow "
                "a direct link. Start stays disabled until it succeeds.")).arg(name),
        QStringLiteral("error-warning-line"), Role::Warn);
}

void RollbackLobbyDialog::onPingMeasured(quint64 userId, int rttMs)
{
    // A measurement clears any retry/unreachable text and re-arms the notice
    // so a later failure is announced again.
    m_probeFailureAnnounced.remove(userId);

    // Only redraw the seat that just got a measurement — avoids churning the
    // whole room view on every probe response.
    for (auto& s : m_seats)
    {
        if (s.peerId == userId && s.row)
            s.row->setLinkPing(rttMs);
    }

    // Auto delay follows this player's own direct peer RTTs. Every seated client
    // runs this path independently, so their resolved delays may differ.
    if (m_delayAuto)
        applyLocalFrameDelay(false);
    refreshSettingHints();

    // A peer's first ping may be what unblocks the host's Start button.
    refreshStartButton();

    // Refresh the lobby rows so the hover tooltip shows the fresh measurement:
    // this user's, and any others seated on the same connection.
    const auto refreshRow = [this](quint64 id) {
        const auto rowIt = m_userItems.constFind(id);
        const auto userIt = m_client->users().constFind(id);
        if (rowIt != m_userItems.constEnd() && userIt != m_client->users().constEnd())
            refreshPlayerRow(rowIt.value(), userIt.value());
    };
    refreshRow(userId);
    for (const auto& s : m_seats)
    {
        if (s.peerId == userId && s.userId != userId)
            refreshRow(s.userId);
    }
    // A ping filter may now show or hide them.
    if (m_playerMaxPing > 0)
        refreshPlayerGroups();

    // Rooms hosted by this user show your ping in the rooms column.
    for (const auto& room : m_client->rooms())
    {
        if (room.hostId == userId)
        {
            onRoomListChanged();
            break;
        }
    }
}

int RollbackLobbyDialog::worstLocalPeerPingMs() const
{
    if (!m_client) return -1;
    const quint64 selfId = m_client->selfUserId();
    int worst = -1;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0 || s.peerId == selfId)
            continue;
        const int ping = m_client->measuredPingMs(s.peerId);
        if (ping >= 0)
            worst = std::max(worst, ping);
    }
    return worst;
}

// Auto's delay right now. Alone there is nobody's input to wait for; it settles
// on the real ping when someone is added.
int RollbackLobbyDialog::autoFrameDelay() const
{
    return m_playingAlone ? 0 : autoFrameDelayForPing(worstLocalPeerPingMs());
}

void RollbackLobbyDialog::applyLocalFrameDelay(bool force)
{
    const bool liveMatch = inLiveMatch();
    if (m_currentRoomId == 0 || !m_delayStepper ||
        (m_currentRoomState != "waiting" && !liveMatch))
        return;
    // Auto settles the delay before the match. Mid-match it resolves only when
    // the player picks it, so a ping change never moves the delay under them.
    if (liveMatch && !force)
        return;

    const int delay = m_delayAuto
        ? autoFrameDelay()
        : m_delayStepper->value();
    if (m_delayAuto)
        m_delayStepper->setValue(delay);
    if (!force && delay == m_currentRoomDelay && m_localDelayPublished)
        return;

    if (liveMatch && delay != m_currentRoomDelay)
    {
        rmgk_gekko::set_live_local_delay(delay);
        appendChatSystemLine(CHANNEL_ROOM, tr("Your frame delay is now %1 f.").arg(delay),
                             QStringLiteral("settings-3-line"));
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            QString("Rollback lobby live delay=%1 auto=%2").arg(delay).arg(m_delayAuto ? 1 : 0)
                .toUtf8().constData());
    }
    m_currentRoomDelay = delay;
    if (m_client)
    {
        const quint64 selfId = m_client->selfUserId();
        for (auto& seat : m_seats)
        {
            if (seat.peerId != selfId)
                continue;
            seat.frameDelay = delay;
            if (seat.row)
                seat.row->setDelay(delay);
        }
    }

    // Persist the concrete local value (never the Auto sentinel) as the legacy
    // create-room seed. The optional remembered choice is stored separately.
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    s.setValue("Delay", delay);
    s.endGroup();

    m_localDelayPublished = true;
    if (m_client)
        m_client->updateLocalFrameDelay(delay, m_delayAuto);
}

void RollbackLobbyDialog::applyRoomPrediction(bool force)
{
    // Mid-match the host's change only takes effect when the server relays it:
    // every player, the host included, applies it from the ROOM_STATE echo.
    const bool liveChange = inLiveMatch() && m_serverAcceptsInGameSettings;
    if (m_currentRoomId == 0 || (m_currentRoomState != "waiting" && !liveChange) ||
        !m_predictionStepper || !m_client ||
        m_currentRoomHostId != m_client->selfUserId())
        return;

    const int prediction = m_predictionAuto
        ? kDefaultPredictionWindow
        : m_predictionStepper->value();
    if (!force && prediction == m_currentRoomPrediction)
        return;

    m_currentRoomPrediction = prediction;

    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    s.setValue("Prediction", prediction);
    s.endGroup();

    m_client->updateRoomPrediction(prediction, m_predictionAuto);
}

// This client is playing (or starting) the room's current match.
bool RollbackLobbyDialog::inLiveMatch() const
{
    return m_currentRoomState == "in_game" && (m_emulationActive || m_awaitingEmulationStart);
}

void RollbackLobbyDialog::updateRollbackSettingControls()
{
    if (!m_delayStepper || !m_predictionStepper || !m_client)
        return;

    static const QString tipDisabledMidMatch = tr("Can't change settings during a match.");
    static const QString tipHostControlled = tr("Only the room host can change the prediction window.");

    // The running session switches to a new delay or prediction without
    // desyncing, so both stay editable while this client plays the match.
    // Prediction reaches the other players through the server, so mid-match it
    // also needs a server that accepts it once the room is in game.
    const bool iAmHost = m_currentRoomHostId == m_client->selfUserId();
    const bool waiting = m_currentRoomState == "waiting";
    const bool liveMatch = inLiveMatch();
    const bool delayEditable = waiting || liveMatch;
    const bool predictionOpen = waiting || (liveMatch && m_serverAcceptsInGameSettings);
    m_delayStepper->setEnabled(delayEditable);
    m_delayAutoCheck->setEnabled(delayEditable);
    m_predictionStepper->setEnabled(predictionOpen && iAmHost);

    m_delayStepper->setProperty("baseTip", delayEditable
        ? m_delayStepper->property("originalTip").toString()
        : tipDisabledMidMatch);
    m_predictionStepper->setToolTip(!predictionOpen
        ? tipDisabledMidMatch
        : (iAmHost ? m_predictionStepper->property("originalTip").toString()
                   : tipHostControlled));
    refreshSettingHints();
}

// Says where each value came from: the delay's in its tooltip, prediction's in
// the line under it in the room settings panel.
void RollbackLobbyDialog::refreshSettingHints()
{
    if (!m_delayStepper || !m_predictionHint || !m_client)
        return;

    const quint64 selfId = m_client->selfUserId();
    const bool liveMatch = inLiveMatch();
    QString delayHint;
    if (!m_delayAuto)
    {
        delayHint = liveMatch ? tr("You changed this mid-match. Auto re-reads your ping if you pick it.")
                              : tr("Your pick. Only affects you.");
    }
    else if (m_playingAlone)
    {
        delayHint = m_client->matchSeats() > 0
            ? tr("Auto stays at 0 while you're alone, then reads real pings when someone's added.")
            : tr("Auto stays at 0: everyone in this match plays on this PC.");
    }
    else
    {
        int worst = -1;
        QString worstName;
        for (const auto& s : m_seats)
        {
            if (s.userId == 0 || s.peerId == selfId)
                continue;
            const int ping = m_client->measuredPingMs(s.peerId);
            if (ping > worst)
            {
                worst = ping;
                worstName = s.username;
            }
        }
        delayHint = worst >= 0
            ? tr("From your worst ping, %1 ms to %2. Only affects you.").arg(worst).arg(worstName)
            : tr("Auto reads your ping once someone joins. Only affects you.");
    }
    // Where the delay came from goes under the explanation, in its tooltip.
    const QString delayTip = m_delayStepper->property("baseTip").toString() + QStringLiteral("\n\n") + delayHint;
    m_delayStepper->setToolTip(delayTip);
    m_delayAutoCheck->setToolTip(delayTip);

    const bool iAmHost = m_currentRoomId != 0 && m_currentRoomHostId == selfId;
    m_predictionLock->setVisible(!iAmHost);
    m_predictionHint->setVisible(!iAmHost);
    if (!iAmHost)
        m_predictionHint->setText(m_currentRoomHostName.isEmpty()
            ? tr("Set by the host")
            : tr("Set by the host (%1)").arg(m_currentRoomHostName));
    updateJoinLimitControls();
    updateRoomSizeControls();
}

// The match is over for this client. Stop steering the engine so the next
// session, lobby or not, runs the values it is started with.
void RollbackLobbyDialog::endLiveRollbackSettings()
{
    m_playingAlone = false;
    rmgk_gekko::set_live_local_delay(-1);
    rmgk_gekko::set_live_prediction_window(-1);
    updateRollbackSettingControls();
}

void RollbackLobbyDialog::onRoomLeft(const QString& reason)
{
    if (m_lateJoin.role != LateJoinRole::None)
        abortLateJoin(QStringLiteral("left_room"), false);
    m_roomLateJoin = false;
    m_canStartAlone = false;
    m_roomHasLateJoinOption = false;
    m_roomLateJoinEnabled = false;
    m_lateJoinEnabledSeen = false;
    if (m_lateJoinCheck) m_lateJoinCheck->setVisible(false);
    m_outOfMatchUsers.clear();
    m_waitingNoticeMs.clear();
    m_lateJoinCapableUsers.clear();
    m_admitRequestedUserId = 0;
    m_announcedWaitingSelf = false;
    m_roomHasSaveDataOption = false;
    m_roomSaveDataEnabled = false;
    m_saveDataSeen = false;
    m_roomSaveDataHash.clear();
    m_saveDataCapableUsers.clear();
    m_hostSaveChoice.reset();
    m_hostGameOptionIds.clear();
    m_roomGameOptionsOffered = false;
    m_gameOptionsSeen = false;
    m_roomGameOptions.clear();
    m_gameOptionsCapableUsers.clear();
    m_gameOptionsResync->stop();
    m_publishSaveChoiceWhenWaiting = false;
    if (!m_matchSaveActive)
    {
        m_matchSave.reset();
        m_matchSaveChanged = false;
        m_keepCopyFile.clear();
        m_keepCopyEnabled = false;
    }
    if (m_saveDataCheck) m_saveDataCheck->setVisible(false);
    updateSaveDataControls();
    if (m_emulationActive || m_awaitingEmulationStart)
        emit closeMatchRequested();

    for (auto& s : m_seats)
    {
        s.userId = 0;
        s.peerId = 0;
    }

    m_currentRoomId = 0;
    m_currentRoomGame.clear();
    m_currentRoomRegion.clear();
    m_currentRoomState.clear();
    m_currentRoomDelay = 2;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    m_currentRoomPacing = 0;
    m_currentRoomHostId = 0;
    m_currentRoomHostName.clear();
    m_currentMatchId = 0;
    emit liveReplayViewerCountCleared();
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;
    m_awaitingEmulationStart = false;
    m_emulationActive = false;
    endLiveRollbackSettings();
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;
    m_localDelayPublished = false;
    updateRecordingControls();

    if (m_chatViewRoom) m_chatViewRoom->clear();
    setRoomChatVisible(false);

    refreshLocalRoster();
    switchToRoomsView();
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn)  m_dropBtn->setEnabled(false);
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    const bool connected = (m_client->state() == LobbyClient::ConnectionState::Connected);
    if (m_createRoomBtn) m_createRoomBtn->setEnabled(connected);
    if (m_quickMatchBtn) m_quickMatchBtn->setEnabled(connected);

    onRoomListChanged();

    // Tell the user why they left, if it wasn't their own doing.
    if (reason == QLatin1String("kicked"))
        QMessageBox::information(this, tr("Removed from match"),
            tr("You were removed from the lobby by the host."));
    else if (reason == QLatin1String("host_left"))
        QMessageBox::information(this, tr("Room closed"),
            tr("The host closed the room."));
    else if (reason == QLatin1String("admin_closed"))
        QMessageBox::information(this, tr("Room closed"),
            tr("The room was closed by a moderator."));
}

// ──────────────────────────────────────────────────────────────────────
//  Chat
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onChatSendClicked()
{
    const QString text = m_chatInput->text().trimmed();
    if (text.isEmpty()) return;
    const int tab = m_chatTabs->currentIndex();
    const QString channel = tab == kChatTabSpectators && (m_spectatingMatchId != 0 || m_playerChatLive) ? CHANNEL_SPECTATE
        : tab == kChatTabRoom && m_currentRoomId != 0 ? CHANNEL_ROOM
                                                      : CHANNEL_LOBBY;
    if (handleSlashCommand(channel, text)) { m_chatInput->clear(); return; }
    if (tab == kChatTabMail)
    {
        // With no thread open there's nobody to send it to; keep what they typed.
        if (m_pmPanel->threadOpen())
        {
            sendCurrentPrivateMessage(text);
            m_chatInput->clear();
        }
        return;
    }
    if (channel == QLatin1String(CHANNEL_SPECTATE))
        sendSpectatorChat(text);
    else if (channel == QLatin1String(CHANNEL_ROOM))
        sendRoomChat(text);
    else
        m_client->sendChat(CHANNEL_LOBBY, text);
    m_chatInput->clear();
}

void RollbackLobbyDialog::onChatMessageReceived(const LobbyClient::ChatMessage& msg)
{
    if (msg.channel == QLatin1String("pm"))
    {
        onPrivateMessage(msg);
        return;
    }

    // A moderator always gets through, so a warning can't be ignored away.
    if (!msg.fromAdmin && !m_client->isOnThisPc(msg.fromUserId) && isIgnored(msg.fromUsername))
        return;

    bool mentionsMe = false;
    const auto lineFor = [&](const QString& message, bool mine = false) {
        const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
        QString line = chatMessageHtml(ts, msg.fromUserId, msg.fromUsername, chatBodyHtml(message, &mentionsMe), msg.fromAdmin, mine, msg.fromPlayer);
        mentionsMe = mentionsMe && msg.fromUserId != 0 && !m_client->isOnThisPc(msg.fromUserId);
        return mentionsMe ? chatMentionLineHtml(line) : line;
    };

    if (msg.channel == CHANNEL_SPECTATE)
    {
        // A live replay's chat, shared by those watching it and the match's
        // players, shown in the Spectators tab. Lines that arrive after this client
        // left it are dropped.
        const bool watching = m_spectatingMatchId != 0;
        if (!watching && !m_playerChatLive)
            return;
        appendChatLine(CHANNEL_SPECTATE, lineFor(msg.message, m_client->isOnThisPc(msg.fromUserId)));
        if (mentionsMe)
            noteMention(CHANNEL_SPECTATE, msg);
        if (watching)
            emit spectatorChatReceived(msg.fromUsername, msg.message);
        else
            showSpectatorCommentOverGame(msg);
        return;
    }

    // Discord-style @notify ping: a moderator's live message containing
    // "@notify" is highlighted, plays the system alert sound, and flashes the
    // lobby window. Trust comes from the server's admin stamp on the message,
    // so a regular user typing "@notify" can't ping the lobby. History replays
    // arrive via chatHistoryReceived, not here, so reconnecting never re-pings.
    const bool notifyPing = msg.fromAdmin && msg.message.contains(QStringLiteral("@notify"));

    // The "@notify" token is a trigger, not content — hide it from the
    // rendered line. A bare "@notify" still needs something to show on the bar.
    QString displayMessage = msg.message;
    if (msg.channel == CHANNEL_ROOM && msg.message == QStringLiteral("I dropped from the game."))
        displayMessage = tr("I dropped from the game.");
    if (notifyPing)
    {
        displayMessage.remove(QStringLiteral("@notify"));
        displayMessage = displayMessage.simplified();
        if (displayMessage.isEmpty())
            displayMessage = tr("📣 Attention!");
    }

    QString line = lineFor(displayMessage);
    if (notifyPing)
    {
        // Full-width bar via a one-cell table (QTextEdit's rich-text subset
        // doesn't background-fill plain blocks). Fixed black-on-light-yellow
        // so the highlight reads the same in light and dark themes.
        line = QString("<table width=\"100%\" cellpadding=\"3\" style=\"background-color:#fff3b0;\">"
                       "<tr><td style=\"color:#000000;\">%1</td></tr></table>").arg(line);
    }
    appendChatLine(msg.channel, line);
    if (mentionsMe)
        noteMention(msg.channel, msg);

    if (notifyPing && m_client && msg.fromUserId != m_client->selfUserId())
    {
        QApplication::beep();
        QApplication::alert(this);
    }

    // Mirror room chat into the in-game overlay. The server relays room chat to
    // every member (including the sender), so forwarding all room messages —
    // own and remote — makes both show on the OSD whether they were typed in
    // the dialog or via the in-game chat key.
    if (msg.channel == CHANNEL_ROOM)
    {
        emit roomChatReceived(msg.fromUsername, displayMessage);
    }
    else if (notifyPing)
    {
        // Admin @notify announcements are forced onto the in-game overlay even
        // from the lobby channel, so mid-match players see them. MainWindow
        // drops the line unless emulation is running.
        emit roomChatReceived(msg.fromUsername, displayMessage);
    }
}

void RollbackLobbyDialog::sendRoomChat(const QString& message)
{
    if (!m_client)
        return;

    const QString text = message.trimmed();
    if (text.isEmpty())
        return;

    m_client->sendChat(CHANNEL_ROOM, text);
}

void RollbackLobbyDialog::sendSpectatorChat(const QString& message)
{
    if (!m_client || (m_spectatingMatchId == 0 && !m_playerChatLive))
        return;

    const QString text = message.trimmed();
    if (text.isEmpty())
        return;

    m_client->sendChat(CHANNEL_SPECTATE, text);
}

LagReport RollbackLobbyDialog::lagReport(bool wholeMatch) const
{
    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    // Seats are named by who sits in them: a PC with several players has one
    // connection, so its user id names only the first of them.
    const QList<LagPlayer> players = m_lagStats->players(selfId, [this](int slot, quint64 userId) {
        if (slot >= 1 && slot <= 4 && !m_seats[slot - 1].username.isEmpty())
            return m_seats[slot - 1].username;
        return lobbyUserName(userId);
    });
    return LagStats::build(players, wholeMatch);
}

void RollbackLobbyDialog::appendLagReport(const LagReport& report, const QString& channel)
{
    if (report.severity == LagReport::Severity::None)
        return;
    // Shown to this player only, as a system line with the verdict's colour.
    const Tokens& t = LobbyTheme::tokens();
    const auto ts = QDateTime::currentDateTime().toString("hh:mm");
    appendChatLine(channel.isEmpty() ? QString(CHANNEL_ROOM) : channel,
                   QString("<span style='color:%1; font-size:9px;'>%2</span>&nbsp; %3&nbsp;%4")
                       .arg(t.muted.name(), ts,
                            chatIconHtml(QStringLiteral("pulse-line"), LagStats::severityColor(report.severity), 13),
                            LagStats::chatHtml(report)));
}

void RollbackLobbyDialog::showLagStats()
{
    if (m_lagStatsDialog == nullptr)
        m_lagStatsDialog = new LagStatsDialog([this](bool wholeMatch) { return lagReport(wholeMatch); }, this);
    m_lagStatsDialog->show();
    m_lagStatsDialog->raise();
    m_lagStatsDialog->activateWindow();
}

QString RollbackLobbyDialog::chatMessageHtml(const QString& time, quint64 userId, const QString& who,
                                             const QString& bodyHtml, bool fromAdmin, bool mine, bool fromPlayer) const
{
    const Tokens& t = LobbyTheme::tokens();
    const QString color = mine ? t.accentInk.name() : t.ink.name();
    QString name = QString("<b style='color:%1;'>%2</b>").arg(color, who.toHtmlEscaped());
    // Someone else's name opens their menu. Server notices have no user behind them.
    if (userId != 0 && !m_client->isOnThisPc(userId))
        name = QString("<a href='%1' style='color:%2; text-decoration:none;'>%3</a>")
            .arg(ChatView::userHref(userId), color, name);
    QString shield = fromAdmin
        ? chatIconHtml(QStringLiteral("shield-star-fill"), t.accentInk, 12) + QStringLiteral("&nbsp;")
        : QString();
    // In spectator chat, which of the people talking are in the match.
    if (fromPlayer)
        shield += chatIconHtml(QStringLiteral("gamepad-line"), t.ink2, 12) + QStringLiteral("&nbsp;");
    return QString("<span style='color:%1; font-size:9px;'>%2</span>&nbsp; %3%4 %5")
        .arg(t.muted.name(), time, shield, name, bodyHtml);
}

void RollbackLobbyDialog::appendChatLine(const QString& channel, const QString& text)
{
    ChatView* target = m_chatViewLobby;
    int tab = kChatTabLobby;
    if (channel == CHANNEL_ROOM && m_chatViewRoom)
    {
        target = m_chatViewRoom;
        tab = kChatTabRoom;
    }
    else if (channel == CHANNEL_SPECTATE && m_chatViewSpectate)
    {
        target = m_chatViewSpectate;
        tab = kChatTabSpectators;
    }
    if (!target)
        return;
    target->addLine(text);

    if (m_chatTabs && tab != m_chatTabs->currentIndex() && m_chatTabs->isTabVisible(tab))
        setChatUnread(tab, m_chatUnread[tab] + 1);
}

void RollbackLobbyDialog::appendChatSystemLine(const QString& channel, const QString& text,
                                               const QString& icon, Role role)
{
    const Tokens& t = LobbyTheme::tokens();
    const auto ts = QDateTime::currentDateTime().toString("hh:mm");
    QString mark;
    if (!icon.isEmpty())
    {
        const QColor color = role == Role::Muted ? t.muted : LobbyTheme::roleColor(role);
        mark = chatIconHtml(icon, color, 13) + QStringLiteral("&nbsp;");
    }
    appendChatLine(channel, QString("<span style='color:%1; font-size:9px;'>%2</span>&nbsp; %3"
                                    "<i style='color:%1;'>%4</i>")
        .arg(t.muted.name(), ts, mark, text));
}

bool RollbackLobbyDialog::handleSlashCommand(const QString& channel, const QString& text)
{
    if (!m_client || !text.startsWith('/'))
        return false;

    const QStringList parts = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    const QString cmd = parts[0].toLower();

    auto sysline = [&](const QString& s) { appendChatSystemLine(channel, s); };

    if ((cmd == "/w" || cmd == "/pm" || cmd == "/r") && m_client->socialSupported())
    {
        const bool reply = cmd == "/r";
        if (reply && m_lastPmRemote == 0)
        {
            sysline(tr("There's nobody to reply to yet."));
            return true;
        }
        if (!reply && parts.size() < 2)
        {
            sysline(tr("Usage: /w &lt;player&gt; [message]"));
            return true;
        }
        const quint64 target = reply ? m_lastPmRemote : userIdForName(parts[1]);
        const auto user = m_client->users().constFind(target);
        if (target == 0 || user == m_client->users().constEnd())
        {
            sysline(reply ? tr("They've left the lobby.")
                          : tr("Nobody called %1 is here.").arg(parts[1].toHtmlEscaped()));
            return true;
        }
        if (m_client->isOnThisPc(target))
        {
            sysline(tr("They're on this PC."));
            return true;
        }
        if (reply && !m_client->isOnThisPc(m_lastPmLocal))
        {
            sysline(tr("Whoever they wrote to has left this PC."));
            return true;
        }
        if (reply)
            m_pmPanel->openThread(m_lastPmLocal, target);
        else
            openPrivateMessage(target);
        m_chatTabs->setCurrentIndex(kChatTabMail);
        const QStringList words = parts.mid(reply ? 1 : 2);
        if (!words.isEmpty())
            sendCurrentPrivateMessage(words.join(QLatin1Char(' ')));
        return true;
    }

    // /login is always intercepted (even with trailing text) so a password typed
    // inline is never sent to chat. The password is collected via a masked modal.
    if (cmd == "/login")
    {
        bool ok = false;
        const QString pw = QInputDialog::getText(this, tr("Moderator Login"),
            tr("Password:"), QLineEdit::Password, QString(), &ok);
        if (ok && !pw.isEmpty())
        {
            m_client->sendAdminAuth(pw);
            sysline(tr("Authenticating…"));
        }
        return true;
    }

    bool lagWholeMatch = false;
    if (LagStats::parseCommand(text, &lagWholeMatch))
    {
        const LagReport report = lagReport(lagWholeMatch);
        if (report.severity == LagReport::Severity::None)
            sysline(tr("Lag stats need a rollback match: try /lagstat while one is running, or right after it."));
        else
            appendLagReport(report, channel);
        return true;
    }

    if (cmd == "/modhelp")
    {
        sysline("Moderator commands: /login, /kick &lt;user&gt; [reason], "
                "/mute &lt;user&gt; [dur] [reason], /timeout &lt;user&gt; &lt;dur&gt; [reason], "
                "/ban &lt;user&gt; [reason], /unban &lt;ip&gt;, /unmute &lt;ip&gt;, /modlist. "
                "Durations: 10m, 1h, 2d (blank = permanent).");
        return true;
    }

    static const QStringList modCmds = {"/kick", "/mute", "/timeout", "/ban", "/unban", "/unmute", "/modlist"};
    if (!modCmds.contains(cmd))
        return false; // unknown slash command → fall through and send as normal chat

    if (!m_client->isModerator())
    {
        sysline(tr("You are not a moderator. Use /login first."));
        return true;
    }

    const QString action = cmd.mid(1); // drop leading '/'

    if (action == "modlist")
    {
        m_client->sendModAction("list", QString());
        return true;
    }

    if (action == "unban" || action == "unmute")
    {
        if (parts.size() < 2) { sysline(tr("Usage: /%1 &lt;ip&gt;").arg(action)); return true; }
        m_client->sendModAction(action, parts[1]);
        return true;
    }

    // kick / ban / mute / timeout target a username.
    if (parts.size() < 2)
    {
        sysline(tr("Usage: /%1 &lt;username&gt; …").arg(action));
        return true;
    }
    const QString target = parts[1];

    if (action == "kick" || action == "ban")
    {
        const QString reason = parts.size() > 2 ? parts.mid(2).join(' ') : QString();
        m_client->sendModAction(action, target, QString(), reason);
        return true;
    }

    if (action == "mute")
    {
        // Optional duration (blank = permanent), optional reason after it.
        QString duration, reason;
        if (parts.size() >= 3) duration = parts[2];
        if (parts.size() >= 4) reason = parts.mid(3).join(' ');
        m_client->sendModAction("mute", target, duration, reason);
        return true;
    }

    // timeout requires an explicit duration.
    if (parts.size() < 3)
    {
        sysline(tr("Usage: /timeout &lt;username&gt; &lt;duration&gt; [reason]"));
        return true;
    }
    const QString reason = parts.size() > 3 ? parts.mid(3).join(' ') : QString();
    m_client->sendModAction("timeout", target, parts[2], reason);
    return true;
}

void RollbackLobbyDialog::onAdminAuthResult(bool ok, const QString& nameOrReason)
{
    if (ok)
    {
        m_moderatorName = nameOrReason;
        updateUserChip();
        const QString line = tr("✓ Moderator access granted (%1). Right-click a player to moderate, or type /modhelp.")
            .arg(nameOrReason.toHtmlEscaped());
        appendChatSystemLine(CHANNEL_LOBBY, line);
        appendChatSystemLine(CHANNEL_ROOM, line);
    }
    else
    {
        appendChatSystemLine(CHANNEL_LOBBY, tr("Admin login failed: %1").arg(nameOrReason.toHtmlEscaped()));
    }
}

void RollbackLobbyDialog::onModNotice(const QString& severity, const QString& text)
{
    Q_UNUSED(severity);
    const QString line = text.toHtmlEscaped();
    appendChatSystemLine(CHANNEL_LOBBY, line);
    appendChatSystemLine(CHANNEL_ROOM, line);
}

void RollbackLobbyDialog::onModListReceived(const QJsonArray& bans, const QJsonArray& mutes)
{
    if (m_moderationDialog && m_moderationDialog->isVisible())
    {
        m_moderationDialog->setSanctions(bans, mutes);
        return;
    }

    auto fmt = [](const QJsonArray& arr, const QString& kind) -> QStringList {
        QStringList lines;
        for (const auto& v : arr)
        {
            const auto o = v.toObject();
            const QString ip = o.value("ip").toString();
            const QString by = o.value("by").toString().toHtmlEscaped();
            const QString reason = o.value("reason").toString().toHtmlEscaped();
            const qint64 until = static_cast<qint64>(o.value("until").toDouble());
            const QString when = (until == 0)
                ? QStringLiteral("permanent")
                : QDateTime::fromMSecsSinceEpoch(until).toString("yyyy-MM-dd hh:mm");
            QString line = QString("&nbsp;&nbsp;%1 %2 (by %3, %4)").arg(kind, ip, by, when);
            if (!reason.isEmpty()) line += " — " + reason;
            lines << line;
        }
        return lines;
    };

    appendChatSystemLine(CHANNEL_LOBBY, tr("Active sanctions:"));
    const QStringList all = fmt(bans, "BAN") + fmt(mutes, "MUTE");
    if (all.isEmpty())
        appendChatLine(CHANNEL_LOBBY, tr("&nbsp;&nbsp;(none)"));
    for (const auto& l : all)
        appendChatLine(CHANNEL_LOBBY, l);
}

void RollbackLobbyDialog::notifyPlayerJoined()
{
    // Always chime on a join — including when the lobby is in the background —
    // so the user hears it while doing something else. The taskbar flash
    // self-gates: QApplication::alert only flashes when we're not the active
    // window (on Windows it's FlashWindowEx; a no-op where there's no taskbar).
    QApplication::alert(this);
    // System notification sound — MessageBeep(MB_OK) on Windows (the "bing"),
    // the platform bell elsewhere. Plays regardless of focus.
    QApplication::beep();
}

// ──────────────────────────────────────────────────────────────────────
//  Match handoff / quick match
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onQuickMatchClicked()
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
        return;

    if (m_quickMatchActive)
    {
        m_client->quickMatchCancel();
    }
    else
    {
        if (m_currentRoomId != 0)
        {
            QMessageBox::information(this, tr("Already in a room"),
                tr("Leave your current room before searching for a match."));
            return;
        }
        if (m_client->seatsOnThisPc() > 1)
        {
            onQuickMatchRefused(QStringLiteral("local_players"));
            return;
        }

        const QVariantMap romData = selectedBrowseRom();
        const QString romName = romData.value("name").toString();
        const QString romMd5  = romData.value("md5").toString();
        if (romMd5.isEmpty())
        {
            QMessageBox::information(this, tr("Select a game"),
                tr("Pick a game from the dropdown before searching — Quick Match "
                "only pairs you with players searching for that same ROM."));
            return;
        }
        qInfo() << "lobby: quick match search" << "game" << romName << "md5" << romMd5;
        m_client->quickMatchJoin(romName, romMd5);
    }
}

void RollbackLobbyDialog::onQuickMatchStatusChanged(bool searching, int queueSize)
{
    if (searching && !m_quickMatchActive)
    {
        m_searchStartedMs = QDateTime::currentMSecsSinceEpoch();
        m_searchingGame = selectedBrowseRom().value("name").toString();
    }
    m_quickMatchActive = searching;
    m_searchQueueSize = queueSize;
    if (m_createRoomBtn)
        m_createRoomBtn->setEnabled(!searching &&
                                    m_client->state() == LobbyClient::ConnectionState::Connected &&
                                    m_currentRoomId == 0);
    refreshPlayBar();
    refreshLocalRoster();
}

void RollbackLobbyDialog::onQuickMatchRefused(const QString& reason)
{
    QMessageBox::information(this, tr("Quick Match"),
        reason == QStringLiteral("local_players")
            ? tr("Quick Match pairs two players, one on each PC. "
                 "Remove the other players on this PC, or create a room to play together.")
            : reason);
}

// ── Other players on this PC ──────────────────────────────────────────
//
// They sit under this client's own row. Everything that happens to one of them
// happens to all: they take seats together when this client creates or joins a
// room, and leave, get kicked, drop and join a running match together. So the
// roster only changes out here in the lobby.

namespace
{
    constexpr int kMaxPlayersOnPc = 4; // the lobby's limit too
    constexpr const char* kLocalNamesSetting = "Lobby/LocalPlayers/Names";

    // Whether the player has turned this controller on. Only RMG-Input has an
    // on switch per controller; other input plugins decide for themselves.
    bool localControllerOn(int controller)
    {
        if (CoreSettingsGetStringValue(SettingsID::Core_INPUT_Plugin).find("RMG-Input") == std::string::npos)
            return true;
        const std::string section = "Rosalie's Mupen GUI - Input Plugin Profile " + std::to_string(controller);
        return CoreSettingsSectionExists(section) &&
               CoreSettingsGetBoolValue(SettingsID::Input_PluggedIn, section);
    }
}

void RollbackLobbyDialog::buildLocalRoster()
{
    m_rosterBox = new QWidget(m_playersTree->parentWidget());
    m_rosterBox->setObjectName("LocalRoster");
    auto* lay = new QVBoxLayout(m_rosterBox);
    lay->setContentsMargins(4, 3, 4, 5);
    lay->setSpacing(3);

    m_rosterRows = new QVBoxLayout;
    m_rosterRows->setContentsMargins(0, 0, 0, 0);
    m_rosterRows->setSpacing(2);
    lay->addLayout(m_rosterRows);

    m_rosterEditor = new QWidget(m_rosterBox);
    auto* editorLay = new QHBoxLayout(m_rosterEditor);
    editorLay->setContentsMargins(0, 0, 0, 0);
    editorLay->setSpacing(4);
    m_rosterName = new QLineEdit(m_rosterEditor);
    m_rosterName->setObjectName("LocalPlayerName");
    m_rosterName->setFont(LobbyTheme::uiFont(12));
    m_rosterName->setPlaceholderText(tr("Name"));
    m_rosterName->setMaxLength(20);
    m_rosterName->setValidator(new QRegularExpressionValidator(LobbyConnectDialog::usernameCharacters(), m_rosterName));
    m_rosterController = new QComboBox(m_rosterEditor);
    m_rosterController->setObjectName("LocalPlayerController");
    m_rosterController->setFont(LobbyTheme::uiFont(11));
    m_rosterController->setToolTip(tr("The controller they play on. Set controllers up in Input settings."));
    auto* confirm = new IconButton(QStringLiteral("check-line"), 24, m_rosterEditor);
    confirm->setObjectName("LocalPlayerConfirm");
    confirm->setToolTip(tr("Add"));
    confirm->setAccessibleName(tr("Add"));
    auto* cancel = new IconButton(QStringLiteral("close-line"), 24, m_rosterEditor);
    cancel->setObjectName("LocalPlayerCancel");
    cancel->setToolTip(tr("Cancel"));
    cancel->setAccessibleName(tr("Cancel"));
    editorLay->addWidget(m_rosterName, 1);
    editorLay->addWidget(m_rosterController);
    editorLay->addWidget(confirm);
    editorLay->addWidget(cancel);
    m_rosterEditor->setVisible(false);
    lay->addWidget(m_rosterEditor);

    m_rosterError = new QLabel(m_rosterBox);
    m_rosterError->setFont(LobbyTheme::uiFont(11));
    m_rosterError->setProperty("role", "bad");
    m_rosterError->setWordWrap(true);
    m_rosterError->setVisible(false);
    lay->addWidget(m_rosterError);

    m_rosterName->installEventFilter(this);
    connect(confirm, &QToolButton::clicked, this, &RollbackLobbyDialog::submitRosterEditor);
    connect(cancel, &QToolButton::clicked, this, &RollbackLobbyDialog::closeRosterEditor);

    refreshLocalRoster();
}

// Controllers 2-4 that no other local player has. The first is the owner's.
void RollbackLobbyDialog::fillControllerCombo(QComboBox* combo, int current, quint64 exceptUserId) const
{
    QSet<int> taken;
    for (const auto& player : m_client->localPlayers())
    {
        if (player.userId != exceptUserId)
            taken.insert(player.controller);
    }
    const QSignalBlocker blocker(combo);
    combo->clear();
    for (int controller = 1; controller < kMaxPlayersOnPc; ++controller)
    {
        if (taken.contains(controller))
            continue;
        combo->addItem(localControllerOn(controller)
                           ? tr("Controller %1").arg(controller + 1)
                           : tr("Controller %1 (off)").arg(controller + 1),
                       controller);
    }
    combo->setCurrentIndex(std::max(0, combo->findData(current)));
}

// The next controller that's turned on, or else the next one free.
int RollbackLobbyDialog::nextFreeController() const
{
    QSet<int> taken;
    for (const auto& player : m_client->localPlayers())
        taken.insert(player.controller);
    int firstFree = -1;
    for (int controller = 1; controller < kMaxPlayersOnPc; ++controller)
    {
        if (taken.contains(controller))
            continue;
        if (localControllerOn(controller))
            return controller;
        if (firstFree < 0)
            firstFree = controller;
    }
    return firstFree;
}

QString RollbackLobbyDialog::nextRememberedLocalName() const
{
    QSettings s("RMG-K", "n02");
    for (const QString& name : s.value(kLocalNamesSetting).toStringList())
    {
        if (!m_client->users().isEmpty() && std::none_of(m_client->users().cbegin(), m_client->users().cend(),
                [&name](const LobbyClient::LobbyUser& u) { return u.username.compare(name, Qt::CaseInsensitive) == 0; }))
            return name;
    }
    return QString();
}

void RollbackLobbyDialog::openRosterEditor()
{
    m_rosterError->setVisible(false);
    m_rosterName->setText(nextRememberedLocalName());
    fillControllerCombo(m_rosterController, nextFreeController(), 0);
    m_rosterEditor->setVisible(true);
    m_rosterName->setFocus();
    m_rosterName->selectAll();
    refreshLocalRoster();
}

void RollbackLobbyDialog::closeRosterEditor()
{
    m_rosterSubmittedName.clear();
    m_rosterEditor->setVisible(false);
    m_rosterError->setVisible(false);
    refreshLocalRoster();
}

void RollbackLobbyDialog::submitRosterEditor()
{
    const QString name = m_rosterName->text().trimmed();
    if (name.size() < 2 || !LobbyConnectDialog::usernameFits(name))
    {
        onLocalPlayerFailed(name, QStringLiteral("invalid_name"));
        return;
    }
    m_rosterSubmittedName = name;
    m_client->addLocalPlayer(name, m_rosterController->currentData().toInt());
}

void RollbackLobbyDialog::onLocalPlayerFailed(const QString& name, const QString& reason)
{
    Q_UNUSED(name);
    m_rosterSubmittedName.clear();
    QString why;
    if (reason == QStringLiteral("invalid_name"))
        why = tr("Names are 2 to 20 letters, numbers, or _ - . in one alphabet, at most 10 Japanese, "
                 "Chinese or Korean characters, and can't be offensive.");
    else if (reason == QStringLiteral("name_taken"))
        why = tr("Someone in the lobby already has that name.");
    else if (reason == QStringLiteral("too_many"))
        why = tr("One PC can have up to %1 players.").arg(kMaxPlayersOnPc);
    else if (reason == QStringLiteral("in_room"))
        why = tr("Leave the room to change who's playing on this PC.");
    else if (reason == QStringLiteral("searching"))
        why = tr("Stop searching to change who's playing on this PC.");
    else if (reason == QStringLiteral("too_fast"))
        why = tr("That's a lot of changes at once. Try again in a moment.");
    else
        why = tr("The lobby refused that (%1).").arg(reason);
    m_rosterError->setText(why);
    m_rosterError->setVisible(true);
    refreshLocalRoster();
}

void RollbackLobbyDialog::refreshLocalRoster()
{
    if (!m_rosterBox || !m_client)
        return;

    const bool available = m_client->state() == LobbyClient::ConnectionState::Connected &&
        m_client->localPlayersSupported();
    const bool locked = m_currentRoomId != 0 || m_quickMatchActive;
    const QString lockedWhy = m_currentRoomId != 0
        ? tr("Leave the room to change who's playing on this PC.")
        : tr("Stop searching to change who's playing on this PC.");
    const auto& players = m_client->localPlayers();

    // The player the editor asked for has arrived: remember their name for next
    // time. The roster itself lasts only as long as the connection.
    if (!m_rosterSubmittedName.isEmpty() &&
        std::any_of(players.cbegin(), players.cend(), [this](const LobbyClient::LocalPlayer& player) {
            return player.name == m_rosterSubmittedName;
        }))
    {
        QSettings s("RMG-K", "n02");
        QStringList names = s.value(kLocalNamesSetting).toStringList();
        names.removeAll(m_rosterSubmittedName);
        names.prepend(m_rosterSubmittedName);
        s.setValue(kLocalNamesSetting, names.mid(0, 8));
        m_rosterSubmittedName.clear();
        m_rosterEditor->setVisible(false);
        m_rosterError->setVisible(false);
    }
    if (locked || !available)
        m_rosterEditor->setVisible(false);

    while (QLayoutItem* item = m_rosterRows->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    for (const auto& player : players)
    {
        auto* row = new QWidget(m_rosterBox);
        auto* rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(3, 0, 0, 0);
        rowLay->setSpacing(4);

        auto* name = new ElidedLabel(LobbyTheme::Role::Ink, row);
        name->setFont(LobbyTheme::uiFont(12));
        name->setText(player.name);
        rowLay->addWidget(name, 1);

        auto* combo = new QComboBox(row);
        combo->setFont(LobbyTheme::uiFont(11));
        fillControllerCombo(combo, player.controller, player.userId);
        combo->setEnabled(!locked);
        combo->setToolTip(locked ? lockedWhy
            : localControllerOn(player.controller)
                ? tr("The controller %1 plays on.").arg(player.name)
                : tr("This controller is off. Turn it on in Input settings before playing."));
        const quint64 userId = player.userId;
        connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, combo, userId](int) {
            m_client->setLocalPlayerController(userId, combo->currentData().toInt());
        });
        rowLay->addWidget(combo);

        auto* remove = new IconButton(QStringLiteral("close-line"), 22, row);
        remove->setEnabled(!locked);
        remove->setToolTip(locked ? lockedWhy : tr("Remove %1 from this PC").arg(player.name));
        remove->setAccessibleName(tr("Remove %1 from this PC").arg(player.name));
        connect(remove, &QToolButton::clicked, this, [this, userId]() {
            m_client->removeLocalPlayer(userId);
        });
        rowLay->addWidget(remove);

        m_rosterRows->addWidget(row);
    }

    m_rosterAdd->setVisible(available && !m_rosterEditor->isVisibleTo(m_rosterBox) &&
                            m_client->seatsOnThisPc() < kMaxPlayersOnPc);
    m_rosterAdd->setEnabled(!locked);
    m_rosterAdd->setToolTip(locked ? lockedWhy
        : tr("Play with someone else on this PC. They join every room with you, on their own controller."));

    m_rosterBox->setVisible(available);

    // Whether a room fits depends on how many are on this PC.
    if (m_rosterSeatsShown != m_client->seatsOnThisPc())
    {
        m_rosterSeatsShown = m_client->seatsOnThisPc();
        onRoomListChanged();
    }
}

void RollbackLobbyDialog::warnAboutLocalControllers()
{
    for (const auto& player : m_client->localPlayers())
    {
        if (!localControllerOn(player.controller))
            appendChatSystemLine(CHANNEL_ROOM,
                tr("%1 can't play yet: controller %2 is off. Turn it on in Input settings.")
                    .arg(player.name).arg(player.controller + 1),
                QStringLiteral("gamepad-line"), Role::Warn);
    }
}

// ── Broadcaster ───────────────────────────────────────────────────────

void RollbackLobbyDialog::startBroadcast(quint64 matchId)
{
    if (m_broadcasting) return;
    if (!m_client->sendBroadcastBegin(matchId))
    {
        appendChatSystemLine(CHANNEL_ROOM,
            tr("Live Replay unavailable: the match settings are missing or too large to sync."));
        return;
    }
    m_broadcasting = true;
    m_broadcastMatchId = matchId;
    m_lastKeyframeRequestMs = 0; // request the first keyframe on the next drain tick
    {
        QMutexLocker lock(&m_broadcastMutex);
        m_broadcastBuf.clear();
    }
    // The sink runs on the emulation thread; it only stages bytes (no Qt calls).
    n02::setRecordingStreamSink([this](const void* data, int len) {
        this->feedBroadcastBytes(data, len);
    });
    m_broadcastDrainTimer->start();
    emit liveReplayViewerCountChanged(0, false);
    appendChatSystemLine(CHANNEL_ROOM, tr("Live Replay on — others can watch this match."));
}

void RollbackLobbyDialog::stopBroadcast()
{
    if (!m_broadcasting) return;
    m_broadcasting = false;
    // Stop new bytes, flush the tail, then tell the server.
    n02::setRecordingStreamSink(nullptr);
    if (m_broadcastDrainTimer)
        m_broadcastDrainTimer->stop();
    onBroadcastDrainTick(); // final flush of whatever the close-out wrote
    if (m_broadcastMatchId != 0)
        m_client->sendBroadcastEnd(m_broadcastMatchId);
    m_broadcastMatchId = 0;
    emit liveReplayViewerCountCleared();
    QMutexLocker lock(&m_broadcastMutex);
    m_broadcastBuf.clear();
}

void RollbackLobbyDialog::feedBroadcastBytes(const void* data, int len)
{
    // Emulation thread: stage bytes only, no Qt/network calls here.
    if (data == nullptr || len <= 0) return;
    QMutexLocker lock(&m_broadcastMutex);
    m_broadcastBuf.append(reinterpret_cast<const char*>(data), len);
}

void RollbackLobbyDialog::onBroadcastDrainTick()
{
    QByteArray chunk;
    {
        QMutexLocker lock(&m_broadcastMutex);
        chunk = m_broadcastBuf;   // COW share
        m_broadcastBuf.clear();   // detaches m_broadcastBuf, leaving chunk intact
    }
    if (m_broadcastMatchId == 0)
        return;

    if (!chunk.isEmpty())
    {
        // Stamp the broadcaster's live krec frame so spectators know the live
        // edge and can fast-forward to it. The drained chunk carries records up
        // to ~this frame, so it's the right "after this chunk" live position.
        m_client->sendBroadcastData(m_broadcastMatchId, chunk, n02::recordingFrameCount());
    }

    // Keep a recent, rollback-confirmed keyframe on the server so late viewers
    // start from a short tail instead of replaying from frame zero. The lobby
    // picks the interval (30 s by default), trading recurring upload cost against
    // a late viewer's headless fast-forward. Runs even when no krec chunk drained
    // this tick.
    const qint64 keyframeIntervalMs = m_client->broadcastKeyframeIntervalMs();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool keyframeUploadBusy = m_client->broadcastKeyframeUploadBusy();
    if (!keyframeUploadBusy &&
        (m_lastKeyframeRequestMs == 0 || nowMs - m_lastKeyframeRequestMs >= keyframeIntervalMs))
    {
        rmgk_gekko::request_keyframe();
        m_lastKeyframeRequestMs = nowMs;
    }
    std::vector<unsigned char> kf;
    int kfFrame = -1;
    if (!keyframeUploadBusy && rmgk_gekko::take_keyframe(kf, kfFrame) &&
        !kf.empty() && kfFrame >= 0)
    {
        m_client->sendBroadcastKeyframe(m_broadcastMatchId,
            QByteArray(reinterpret_cast<const char*>(kf.data()), static_cast<int>(kf.size())),
            kfFrame);
    }
}

// ── Spectator ─────────────────────────────────────────────────────────

void RollbackLobbyDialog::beginSpectate(quint64 matchId, const QString& gameName, const QString& romMd5)
{
    if (m_spectatingMatchId == matchId) return; // already watching this one

    // Watch with the exact ROM the players use, matched by MD5 like joining a
    // room. Another build of the game (e.g. a different Smash Remix version) would
    // reject the match's keyframe and play its inputs on different game data,
    // which shows up as the wrong stage.
    QString romPath;
    if (!romMd5.isEmpty())
    {
        romPath = localRomPathForMd5(romMd5);
        if (romPath.isEmpty())
        {
            QMessageBox::warning(this, tr("ROM not found"),
                QString(tr("You don't have the ROM this match is using (%1).\n\n"
                        "Add it to your ROM directory and refresh the list, then try again."))
                    .arg(gameName));
            return;
        }
    }

    // Switching matches: the current playback has to shut down first, which
    // takes the emulator a moment. MainWindow says when it's done, and the new
    // watch starts from notifySpectateSessionClosed.
    if (m_spectatingMatchId != 0)
    {
        m_pendingSpectate = PendingSpectate{ matchId, gameName, romMd5 };
        emit spectateStreamClosed(QStringLiteral("switching"));
        return;
    }

    m_pendingSpectate = PendingSpectate{};
    m_spectatingMatchId = matchId;
    m_spectateStreamArmed = false; // wait for this subscribe's SPECTATE_BEGIN before accepting data
    m_client->startSpectate(matchId);
    m_chatViewSpectate->clear();
    setSpectatorChatVisible(true);
    emit spectateLaunch(matchId, gameName, romPath);
    appendChatSystemLine(CHANNEL_SPECTATE, tr("Connecting to live replay…"), QStringLiteral("eye-line"));
    onRoomListChanged();
}

void RollbackLobbyDialog::notifySpectateSessionClosed()
{
    if (m_pendingSpectate.matchId == 0)
        return;
    const PendingSpectate next = m_pendingSpectate;
    m_pendingSpectate = PendingSpectate{};
    QTimer::singleShot(0, this, [this, next]() {
        // The match may have ended while the old session shut down.
        for (const auto& room : m_client->rooms())
        {
            if (room.matchId == next.matchId && room.broadcasting && m_currentRoomId == 0)
            {
                beginSpectate(next.matchId, next.gameName, next.romMd5);
                break;
            }
        }
    });
}

void RollbackLobbyDialog::stopSpectating()
{
    if (m_spectatingMatchId == 0) return;
    m_client->stopSpectate(m_spectatingMatchId);
    m_spectatingMatchId = 0;
    m_spectateStreamArmed = false;
    setSpectatorChatVisible(false);
    emit liveReplayViewerCountCleared();
    onRoomListChanged();
}

void RollbackLobbyDialog::setSpectatorChatVisible(bool visible)
{
    // Before hiding: Qt would otherwise move on to the next visible tab, not Lobby.
    if (!visible && m_chatTabs->currentIndex() == kChatTabSpectators)
        m_chatTabs->setCurrentIndex(kChatTabLobby);
    m_chatTabs->showTab(kChatTabSpectators, visible);
    setChatUnread(kChatTabSpectators, 0);
    m_pendingMentions[kChatTabSpectators].clear();
    if (visible)
        m_chatTabs->setCurrentIndex(kChatTabSpectators);
    refreshSpectatorTab();
    refreshChatInput();
}

void RollbackLobbyDialog::onSpectateBegan(quint64 matchId, const QByteArray& manifest)
{
    if (matchId != m_spectatingMatchId) return;
    if (m_spectateStreamArmed) return;
    // Use the same parser and session-only cheat set as players, before any
    // replay input or keyframe can start emulation. An empty cheat set is valid;
    // a missing manifest is not safe to replay with local or leftover cheats.
    if (!m_client->applySpectateManifest(manifest))
    {
        m_client->stopSpectate(matchId);
        onSpectateFailed(matchId, QStringLiteral("invalid_manifest"));
        return;
    }
    // The session boundary: everything on the wire before this is leftover from a
    // prior watch of this same match. Arm the stream now; keyframe + tail follow.
    m_spectateStreamArmed = true;
    appendChatSystemLine(CHANNEL_SPECTATE, tr("Watching — buffering the match…"));
    QString romMd5;
    for (const auto& room : m_client->rooms())
    {
        if (room.matchId == matchId)
            romMd5 = room.romMd5;
    }
    announceMatchGameOptions(CHANNEL_SPECTATE, romMd5);
    if (m_client->spectatorChatSupported())
        appendChatSystemLine(CHANNEL_SPECTATE, tr("The players can read this chat too."), QStringLiteral("eye-line"));
}

void RollbackLobbyDialog::onSpectateData(quint64 matchId, const QByteArray& bytes, int liveFrame, qint64 offset)
{
    if (matchId != m_spectatingMatchId) return;
    if (!m_spectateStreamArmed) return; // stale chunk from a previous watch — drop it
    emit spectateStreamData(bytes, liveFrame, offset);
}

void RollbackLobbyDialog::onSpectateKeyframe(quint64 matchId, int frame, const QByteArray& savestate)
{
    if (matchId != m_spectatingMatchId) return;
    if (!m_spectateStreamArmed) return; // stale keyframe from a previous watch — drop it
    emit spectateStreamKeyframe(frame, savestate);
}

void RollbackLobbyDialog::onSpectateEnded(quint64 matchId, const QString& reason)
{
    if (matchId != m_spectatingMatchId) return;
    m_spectatingMatchId = 0; // server already ended it; don't echo SPECTATE_STOP
    setSpectatorChatVisible(false);
    emit liveReplayViewerCountCleared();
    emit spectateStreamClosed(reason);
    onRoomListChanged();
}

void RollbackLobbyDialog::onSpectateFailed(quint64 matchId, const QString& reason)
{
    if (matchId != m_spectatingMatchId) return;
    m_spectatingMatchId = 0;
    setSpectatorChatVisible(false);
    emit liveReplayViewerCountCleared();
    const QString human =
        reason == "not_broadcasting" ? tr("That live replay isn't available anymore.") :
        reason == "ended"            ? tr("That live replay just ended.") :
        reason == "invalid_manifest" ? tr("The live replay's cheat settings are missing or invalid. The host and lobby server need to support spectator cheat sync.") :
                                       tr("Couldn't watch: %1").arg(reason);
    QMessageBox::information(this, tr("Live Replay"), human);
    emit spectateStreamClosed(reason);
    onRoomListChanged();
}

void RollbackLobbyDialog::onBroadcastViewerCount(quint64 matchId, int viewerCount)
{
    // Cards update in place, so audience changes keep the scroll position.
    onRoomListChanged();
    if (m_broadcasting && matchId == m_broadcastMatchId)
    {
        emit liveReplayViewerCountChanged(viewerCount, false);
    }
    else if (matchId == m_spectatingMatchId)
    {
        emit liveReplayViewerCountChanged(viewerCount, true);
    }
    else if (matchId == m_currentMatchId)
    {
        // Non-host players in the broadcast match should see the same audience
        // count as the broadcaster. They are playing, rather than spectating.
        emit liveReplayViewerCountChanged(viewerCount, false);
    }
}

void RollbackLobbyDialog::abortMatchStart(const QString& reason)
{
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;
    if (!reason.isEmpty())
    {
        appendChatSystemLine(CHANNEL_ROOM, reason);
        CoreAddCallbackMessage(CoreDebugMessageType::Error, reason.toUtf8().constData());
    }
    applyRoomStateBadge(tr("Pre-match start failed"), QString(statusColors().fail));

    const quint64 matchId = m_currentMatchId;
    m_awaitingEmulationStart = false;
    m_currentMatchId = 0;
    endLiveRollbackSettings();
    if (m_client)
        m_client->setMatchTransportActive(false);
    emit liveReplayViewerCountCleared();

    // Cancel any launch the MainWindow may already be spinning up, and tell the
    // server the match is over so the room flips back to "waiting" for everyone
    // (a follow-up ROOM_STATE re-enables Start and clears the badge).
    emit closeMatchRequested();
    if (matchId != 0 && m_client)
        m_client->reportMatchFinished(matchId);
    updateRecordingControls();

    if (m_dropBtn) m_dropBtn->setEnabled(false);
    refreshStartButton();
}

void RollbackLobbyDialog::onMatchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers)
{
    QString iceError;
    if (!m_client->allIcePeersConnected(peers, &iceError))
    {
        const bool unsupported = std::any_of(peers.cbegin(), peers.cend(), [this](const auto& peer) {
            return peer.peerId != m_client->selfUserId() && !peer.iceSupported;
        });
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_iceWaitingMatchId != matchId)
        {
            m_iceWaitingMatchId = matchId;
            m_iceMatchDeadlineMs = nowMs + 15'000;
            appendChatSystemLine(CHANNEL_ROOM, tr("Negotiating direct ICE connections..."));
        }
        if (unsupported || nowMs >= m_iceMatchDeadlineMs)
        {
            m_currentMatchId = matchId;
            m_awaitingEmulationStart = true;
            m_iceWaitingMatchId = 0;
            m_iceMatchDeadlineMs = 0;
            abortMatchStart(unsupported
                ? tr("Match start failed: %1").arg(iceError)
                : tr("Match start failed: direct ICE negotiation timed out. No TURN relay is configured."));
            return;
        }
        applyRoomStateBadge(tr("Negotiating ICE…"), stateHex("connecting", isDarkTheme()));
        QTimer::singleShot(100, this, [this, matchId, peers]() {
            if (m_iceWaitingMatchId == matchId)
                onMatchBegin(matchId, peers);
        });
        return;
    }
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;

    const QString line = QString(tr("Match #%1 starting with %2 player(s)")).arg(matchId).arg(peers.size());
    appendChatSystemLine(CHANNEL_LOBBY, line);
    appendChatSystemLine(CHANNEL_ROOM,  line);

    m_playingAlone = std::all_of(peers.cbegin(), peers.cend(), [this](const auto& peer) {
        return peer.peerId == m_client->selfUserId();
    });
    if (m_playingAlone && m_delayAuto && m_currentRoomDelay != 0)
    {
        m_currentRoomDelay = 0;
        m_delayStepper->setValue(0);
        m_client->updateLocalFrameDelay(0, true);
    }

    // Delay is local to this player; prediction is the host-controlled room value.
    {
        QString delayMsg;
        if (m_delayAuto)
        {
            const int worst = worstLocalPeerPingMs();
            delayMsg = worst >= 0
                ? tr("Your frame delay: %1 (auto, from %2 ms ping) · prediction %3")
                      .arg(m_currentRoomDelay).arg(worst).arg(m_currentRoomPrediction)
                : tr("Your frame delay: %1 (auto) · prediction %2")
                      .arg(m_currentRoomDelay).arg(m_currentRoomPrediction);
        }
        else
        {
            delayMsg = tr("Your frame delay: %1 · prediction %2")
                           .arg(m_currentRoomDelay).arg(m_currentRoomPrediction);
        }
        appendChatSystemLine(CHANNEL_ROOM, delayMsg);
    }
    if (m_playingAlone && m_client->matchSeats() > 0)
        appendChatSystemLine(CHANNEL_ROOM, peers.size() > 1
            ? tr("Only this PC is playing for now. When someone joins the room, click Add to Game on their seat.")
            : tr("You're playing alone for now. When someone joins the room, click Add to Game on their seat."));
    warnAboutLocalControllers();

    m_currentMatchId = matchId;
    m_awaitingEmulationStart = true;
    m_lagStats->reset();
    m_lagStatsRoomId = m_currentRoomId;
    updateRecordingControls();

    // The engine follows these live values for the whole match. Seeding them
    // with the start values replaces anything left from an earlier match, and
    // a change made while the session is still starting applies once it runs.
    m_livePrediction = m_currentRoomPrediction;
    rmgk_gekko::set_live_local_delay(m_currentRoomDelay);
    rmgk_gekko::set_live_prediction_window(m_currentRoomPrediction);
    updateRollbackSettingControls();

    // Disable start; enable Drop now — MATCH_BEGIN arrives *after* the
    // ROOM_STATE that flipped us to in_game, so onRoomStateChanged ran before
    // the awaiting flag was set and left Drop disabled.
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn)  m_dropBtn->setEnabled(true);
    refreshStartButton();
    applyRoomStateBadge(tr("Connecting…"), stateHex("connecting", isDarkTheme()));

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        QString("Rollback lobby MATCH_BEGIN: match=%1 peers=%2 self=%3 roomGame='%4' delay=%5 prediction=%6 transport=ice")
            .arg(matchId)
            .arg(peers.size())
            .arg(m_client->selfUserId())
            .arg(m_currentRoomGame)
            .arg(m_currentRoomDelay)
            .arg(m_currentRoomPrediction)
            .toUtf8().constData());

    const quint64 selfId = m_client->selfUserId();
    LobbyClient::LobbyMatchPeer local{};
    bool foundLocal = false;
    QStringList remotePeers;
    for (const auto& p : peers)
    {
        const QString peerLine = QString("Rollback lobby MATCH_BEGIN peer: user=%1 name='%2' slot=%3 public=%4:%5 local=%6 self=%7")
            .arg(p.userId)
            .arg(p.username)
            .arg(p.slot)
            .arg(p.publicIp)
            .arg(p.publicPort)
            .arg(p.localIp)
            .arg(p.userId == selfId ? 1 : 0);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, peerLine.toUtf8().constData());

        if (p.userId == selfId)
        {
            local = p;
            foundLocal = true;
        }
    }
    if (!foundLocal)
    {
        abortMatchStart(tr("Match start failed: missing local peer."));
        return;
    }

    // Make this room's "Record locally" checkbox authoritative for the match, in
    // case another netplay dialog touched the shared flag since the box was built.
    if (m_recordCheck)
    {
        n02_kaillera_recording_enabled = m_recordCheck->isChecked();
    }
    // Everyone who starts the match can record it, late join or not. A late
    // joiner never gets here: MainWindow opens no replay for them.

    // Capture seated player names (slot-indexed) for the .krec header, the same
    // way the p2p / kaillera paths fill recording_player_names before a recording
    // starts. MainWindow begins replay capture when it launches the match,
    // saving locally and/or streaming according to the two independent choices.
    {
        std::memset(recording_player_names, 0, sizeof(recording_player_names));
        for (const auto& p : peers)
        {
            if (p.slot >= 1 && p.slot <= 4)
            {
                n02::recordingSetPlayerName(p.slot - 1, p.username.toUtf8().constData());
            }
        }
    }

    const quint16 localPort = 0;

    QStringList localSeats;
    for (const auto& p : peers)
    {
        if (p.peerId == selfId)
        {
            if (p.userId != selfId)
                localSeats << QString("local=%1:%2").arg(p.slot).arg(m_client->localPlayerController(p.userId));
            continue;
        }

        const QString selectedLine = QString("Rollback lobby selected ICE peer: peerUser=%1 slot=%2")
            .arg(p.peerId).arg(p.slot);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, selectedLine.toUtf8().constData());
        remotePeers << QString("%1,ice,%2").arg(p.slot).arg(p.peerId);
    }

    // Nobody on another PC is fine with other players on this one, or in a
    // match someone can join later.
    if (remotePeers.isEmpty() && localSeats.isEmpty() && m_client->matchSeats() == 0)
    {
        abortMatchStart(tr("Match start failed: missing remote peer."));
        return;
    }
    // A match open to late joiners emulates every seat the room has, so a
    // newcomer takes over an idle controller instead of changing the machine.
    if (m_client->matchSeats() > 0)
        remotePeers << QString("seats=%1").arg(m_client->matchSeats());
    remotePeers << localSeats;

    const QString localRomFile = localRomPathForMd5(m_currentRoomMd5);
    if (localRomFile.isEmpty())
    {
        abortMatchStart(QString(tr("Match start failed: you don't have the ROM for %1.")).arg(m_currentRoomGame));
        return;
    }

    appendChatSystemLine(CHANNEL_ROOM, tr("Synchronizing pre-match settings..."));
    if (m_client->selfUserId() == m_currentRoomHostId)
    {
        m_client->setPrematchSaveData(m_roomSaveDataEnabled && m_hostSaveChoice
            ? std::optional<CoreNetplaySaveData>(m_hostSaveChoice->Data) : std::nullopt);
        // Checked against the ROM file about to boot, so an option that no
        // longer fits it stops the match here rather than on every player.
        CoreNetplayGameOptions gameOptions;
        if (!CoreResolveGameOptions(localRomFile.toStdU32String(), m_currentRoomMd5.toStdString(),
                                    m_hostGameOptionIds, gameOptions))
        {
            abortMatchStart(tr("Match start failed: %1").arg(QString::fromStdString(CoreGetError())));
            return;
        }
        m_client->setPrematchGameOptions(std::move(gameOptions));
    }
    QString prematchError;
    if (!m_client->syncPrematchManifest(LobbyClient::connectionPeers(peers), local.slot, m_currentRoomHostId,
                                        localRomFile, prematchError))
    {
        abortMatchStart(prematchError.isEmpty() ? tr("Pre-match sync failed.") : prematchError);
        return;
    }
    appendChatSystemLine(CHANNEL_ROOM, tr("Pre-match sync complete."));
    announceMatchGameOptions(CHANNEL_ROOM, m_currentRoomMd5);
    beginMatchSave();

    // Share the exact settings just synced to the players. Arm the stream sink
    // before matchReady begins capture, leaving local recording independent.
    const bool iAmHostForBroadcast =
        (m_client && m_currentRoomHostId == m_client->selfUserId());
    if (iAmHostForBroadcast && m_broadcastCheck && m_broadcastCheck->isChecked())
    {
        startBroadcast(matchId);
    }

    rmgk_gekko::clear_external_socket();

    {
        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "Lobby→matchReady: game='%s' peers=%d (%s) transport=ice slot=%d delay=%d pred=%d",
            m_currentRoomGame.toUtf8().constData(),
            int(remotePeers.size()),
            remotePeers.join("; ").toUtf8().constData(),
            local.slot,
            m_currentRoomDelay,
            m_currentRoomPrediction);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }

    // Pacing isn't threaded through matchReady — the engine reads it from the
    // Rollback_PacingMode setting at session start. Set it from the room's value
    // here so the match definitively runs the host-selected model.
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, m_currentRoomPacing);

    // From this point until notifyEmulationFinished, the GekkoNet adapter owns
    // these established agents. Lobby disconnect/room cleanup is deferred so
    // signaling loss cannot tear down the peer-to-peer data plane.
    m_client->setMatchTransportActive(true);
    emit matchReady(m_currentRoomGame, localRomFile, remotePeers, int(localPort),
                    local.slot, m_currentRoomDelay, m_currentRoomPrediction);
}

#endif // NETPLAY

// ── Late join ─────────────────────────────────────────────────────────
//
// A room member outside the running match (they joined mid-match, or dropped
// out) can be added by the host. The hand-off moves every player to a new
// rollback session that includes them, without restarting anyone's game:
//
//   host    Add to Game → MATCH_ADMIT; the server starts the hand-off
//   host    sends the joiner the match's pre-match settings
//   joiner  boots the ROM, holds before its first real frame, connects to
//           every player, then reports ready
//   host    picks a cut frame ~1 s ahead and tells the players; everyone
//           stops there, where every earlier frame is final
//   host    sends the joiner its state at the cut, then go
//   all     continue from the cut in a session with the joiner
//
// Nobody's emulator restarts: the players freeze for as long as the state
// takes to reach the joiner, with a banner saying who is joining.

namespace
{
    // How far ahead of the host the cut lands. It must reach every player
    // through the lobby before they get there; a player already past it
    // aborts the join.
    constexpr int kLateJoinCutLeadFrames = 60;
    constexpr qint64 kLateJoinReadyTimeoutMs = 60'000;
    constexpr qint64 kLateJoinCutTimeoutMs = 15'000;
    constexpr qint64 kLateJoinStateTimeoutMs = 45'000;
    constexpr qint64 kLateJoinResumeTimeoutMs = 45'000;
}

QString RollbackLobbyDialog::lobbyUserName(quint64 userId) const
{
    if (m_client)
    {
        const auto it = m_client->users().constFind(userId);
        if (it != m_client->users().constEnd() && !it->username.isEmpty())
            return it->username;
    }
    for (const auto& peer : m_lateJoin.peers)
    {
        if (peer.userId == userId && !peer.username.isEmpty())
            return peer.username;
    }
    return QString::number(userId);
}

QString RollbackLobbyDialog::lateJoinNames() const
{
    QStringList names;
    for (const auto& joiner : m_lateJoin.joiners)
        names << joiner.username;
    if (names.size() < 2)
        return m_lateJoin.joiner.username;
    const QString last = names.takeLast();
    return tr("%1 and %2").arg(names.join(QStringLiteral(", ")), last);
}

QString RollbackLobbyDialog::lateJoinBanner() const
{
    return m_lateJoin.joiners.size() > 1
        ? tr("%1 are joining the game").arg(lateJoinNames())
        : tr("%1 is joining the game").arg(lateJoinNames());
}

void RollbackLobbyDialog::onAdmitClicked(quint64 userId)
{
    if (!m_client || m_currentMatchId == 0 || !m_emulationActive ||
        m_lateJoin.role != LateJoinRole::None)
        return;
    m_admitRequestedUserId = userId;
    m_client->sendMatchAdmit(m_currentMatchId, userId);
    appendChatSystemLine(CHANNEL_ROOM, tr("Adding %1 to the game…").arg(lobbyUserName(userId)));
    refreshAdmitButtons();
}

void RollbackLobbyDialog::onMatchAdmitFailed(quint64 matchId, quint64 userId, const QString& reason)
{
    Q_UNUSED(matchId);
    m_admitRequestedUserId = 0;
    const QString name = lobbyUserName(userId);
    QString why;
    if (reason == QStringLiteral("version_mismatch"))
        why = tr("they're on a different Dale64 version");
    else if (reason == QStringLiteral("admit_busy"))
        why = tr("another player is joining right now");
    else if (reason == QStringLiteral("late_join_unsupported"))
        why = tr("this match or their Dale64 can't add players mid-game");
    else if (reason == QStringLiteral("not_waiting"))
        why = tr("they're not waiting to join any more");
    else
        why = tr("something went wrong (%1)").arg(reason);
    appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't add %1: %2.").arg(name, why));
    refreshAdmitButtons();
}

void RollbackLobbyDialog::onMatchAdmitBegin(quint64 matchId, quint64 admitId, int epoch, int seats,
                                            quint64 hostId, const LobbyClient::LobbyMatchPeer& joiner,
                                            const QList<LobbyClient::LobbyMatchPeer>& joiners,
                                            const QList<LobbyClient::LobbyMatchPeer>& peers)
{
    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    if (selfId == 0 || m_lateJoin.role != LateJoinRole::None)
        return;

    m_lateJoin = LateJoin{};
    m_lateJoin.matchId = matchId;
    m_lateJoin.admitId = admitId;
    m_lateJoin.epoch = epoch;
    m_lateJoin.seats = seats;
    m_lateJoin.hostId = hostId;
    m_lateJoin.joiner = joiner;
    m_lateJoin.joiners = joiners;
    m_lateJoin.peers = peers;
    m_lateJoin.startedMs = QDateTime::currentMSecsSinceEpoch();
    m_admitRequestedUserId = 0;

    if (joiner.userId == selfId)
    {
        m_lateJoin.role = LateJoinRole::Joiner;
        // The game is about to take over the window. Whatever is open there
        // (say, a live replay of this match) closes first: its shutdown
        // clears the match settings the host sends, so they wait until it's gone.
        if (m_spectatingMatchId != 0)
        {
            stopSpectating();
            emit spectateStreamClosed(QStringLiteral("late_join"));
        }
        else if (m_localGameOpen)
        {
            emit closeMatchRequested();
        }
        for (const auto& peer : peers)
            m_client->ensureIcePeer(peer.peerId);
        appendChatSystemLine(CHANNEL_ROOM, tr("The host is adding you to the game…"));
        applyRoomStateBadge(tr("Joining the game…"), stateHex("connecting", isDarkTheme()));
    }
    else if (matchId == m_currentMatchId && m_emulationActive)
    {
        m_lateJoin.role = hostId == selfId ? LateJoinRole::Host : LateJoinRole::Player;
        m_client->ensureIcePeer(joiner.userId);
        if (m_lateJoin.role == LateJoinRole::Host)
        {
            // The joiner applies the same pre-match settings every player
            // started with before booting.
            if (m_client->prematchManifest().isEmpty())
            {
                abortLateJoin(QStringLiteral("manifest_missing"), true);
                return;
            }
            m_client->sendAdmitBlob(matchId, admitId, QStringLiteral("manifest"),
                                    m_client->prematchManifest());
        }
        else
        {
            appendChatSystemLine(CHANNEL_ROOM, tr("The host is adding %1 to the game…").arg(lateJoinNames()));
        }
        // The overlay font has no "…" glyph.
        emit overlayMessage(lateJoinBanner() + QStringLiteral("..."));
    }
    else
    {
        return;
    }

    if (!m_lateJoinTimer)
    {
        m_lateJoinTimer = new QTimer(this);
        m_lateJoinTimer->setInterval(16);
        connect(m_lateJoinTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onLateJoinTick);
    }
    m_lateJoinTimer->start();
    refreshAdmitButtons();
    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        QString("Rollback late join begin: match=%1 admit=%2 epoch=%3 seats=%4 joiner=%5 slot=%6 role=%7")
            .arg(matchId).arg(admitId).arg(epoch).arg(seats).arg(joiner.userId).arg(joiner.slot)
            .arg(static_cast<int>(m_lateJoin.role)).toUtf8().constData());
}

void RollbackLobbyDialog::onMatchAdmitStep(quint64 matchId, quint64 admitId, quint64 fromUserId,
                                           const QString& step, int frame, bool ok, const QString& reason)
{
    if (m_lateJoin.role == LateJoinRole::None || matchId != m_lateJoin.matchId ||
        admitId != m_lateJoin.admitId)
        return;

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        QString("Rollback late join step: %1 from=%2 frame=%3 ok=%4 reason=%5")
            .arg(step).arg(fromUserId).arg(frame).arg(ok ? 1 : 0).arg(reason).toUtf8().constData());

    if (step == QStringLiteral("abort"))
    {
        abortLateJoin(reason, false);
        return;
    }

    switch (m_lateJoin.role)
    {
    case LateJoinRole::Host:
        if (step == QStringLiteral("ready") && fromUserId == m_lateJoin.joiner.userId)
        {
            m_lateJoin.readyMs = QDateTime::currentMSecsSinceEpoch();
        }
        else if (step == QStringLiteral("cut_ready"))
        {
            if (!ok)
            {
                abortLateJoin(QStringLiteral("cut_passed"), true);
                return;
            }
            m_lateJoin.awaitingCut.remove(fromUserId);
        }
        break;

    case LateJoinRole::Player:
        if (step == QStringLiteral("cut") && frame > 0 && m_lateJoin.cutFrame < 0)
        {
            if (m_delayAuto)
                applyLocalFrameDelay(true);
            m_lateJoin.cutFrame = frame;
            m_lateJoin.cutSentMs = QDateTime::currentMSecsSinceEpoch();
            rmgk_gekko::arm_admit_cut(frame, false);
            emit overlayCenterMessage(lateJoinBanner());
        }
        else if (step == QStringLiteral("go") && m_lateJoin.cutFrame > 0)
        {
            rmgk_gekko::release_admit_cut(lateJoinRemotes(), m_lateJoin.epoch);
            m_lateJoin.released = true;
            m_lateJoin.releasedMs = QDateTime::currentMSecsSinceEpoch();
            applyLateJoinRoster();
        }
        break;

    case LateJoinRole::Joiner:
        if (step == QStringLiteral("go"))
        {
            m_lateJoin.released = true;
            m_lateJoin.releasedMs = QDateTime::currentMSecsSinceEpoch();
        }
        break;

    default:
        break;
    }
}

void RollbackLobbyDialog::onMatchAdmitBlob(quint64 matchId, quint64 admitId, const QString& step,
                                           const QByteArray& raw)
{
    if (m_lateJoin.role != LateJoinRole::Joiner || matchId != m_lateJoin.matchId ||
        admitId != m_lateJoin.admitId)
        return;

    if (step == QStringLiteral("manifest") && !m_lateJoin.launched)
    {
        if (raw.isEmpty())
        {
            abortLateJoin(QStringLiteral("manifest_failed"), true);
            return;
        }
        // The tick launches once the game that was open has closed.
        m_lateJoin.manifest = raw;
        if (!m_localGameOpen)
            launchAsLateJoiner();
    }
    else if (step == QStringLiteral("state") && m_lateJoin.launched)
    {
        if (raw.isEmpty())
        {
            abortLateJoin(QStringLiteral("state_failed"), true);
            return;
        }
        m_lateJoin.stateReceived = true;
        rmgk_gekko::provide_join_state(std::vector<unsigned char>(raw.cbegin(), raw.cend()));
    }
}

void RollbackLobbyDialog::launchAsLateJoiner()
{
    if (!m_client->applySpectateManifest(m_lateJoin.manifest))
    {
        abortLateJoin(QStringLiteral("manifest_failed"), true);
        return;
    }
    announceMatchGameOptions(CHANNEL_ROOM, m_currentRoomMd5);
    beginMatchSave();

    const quint64 selfId = m_client->selfUserId();
    const QString romFile = localRomPathForMd5(m_currentRoomMd5);
    if (romFile.isEmpty())
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("You don't have the ROM for %1.").arg(m_currentRoomGame));
        abortLateJoin(QStringLiteral("rom_missing"), true);
        return;
    }

    // The same session description a MATCH_BEGIN produces, plus the seat
    // count and the session this join opens.
    QStringList remotePeers;
    std::memset(recording_player_names, 0, sizeof(recording_player_names));
    for (const auto& peer : m_lateJoin.peers)
    {
        if (peer.slot >= 1 && peer.slot <= 4 && !m_lateJoin.left.contains(peer.userId))
        {
            n02::recordingSetPlayerName(peer.slot - 1, peer.username.toUtf8().constData());
        }
        if (peer.peerId != selfId)
            remotePeers << QString("%1,ice,%2").arg(peer.slot).arg(peer.peerId);
        else if (peer.userId != selfId)
            remotePeers << QString("local=%1:%2").arg(peer.slot).arg(m_client->localPlayerController(peer.userId));
    }
    remotePeers << QString("seats=%1").arg(m_lateJoin.seats)
                << QString("join=%1").arg(m_lateJoin.epoch);
    warnAboutLocalControllers();

    m_currentMatchId = m_lateJoin.matchId;
    m_awaitingEmulationStart = true;
    m_lateJoin.launched = true;
    // Auto doesn't settle while waiting beside a running match; it does now.
    if (m_delayAuto)
        applyLocalFrameDelay(true);
    m_livePrediction = m_currentRoomPrediction;
    rmgk_gekko::set_live_local_delay(m_currentRoomDelay);
    rmgk_gekko::set_live_prediction_window(m_currentRoomPrediction);
    updateRollbackSettingControls();
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, m_currentRoomPacing);
    rmgk_gekko::clear_external_socket();
    if (m_dropBtn) m_dropBtn->setEnabled(true);
    refreshStartButton();

    emit overlayCenterMessage(tr("Joining the game..."));
    emit matchReady(m_currentRoomGame, romFile, remotePeers, 0,
                    m_lateJoin.joiner.slot, m_currentRoomDelay, m_currentRoomPrediction);
}

std::vector<LobbyRemotePeer> RollbackLobbyDialog::lateJoinRemotes() const
{
    std::vector<LobbyRemotePeer> remotes;
    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    for (const auto& peer : m_lateJoin.peers)
    {
        if (peer.peerId == selfId || m_lateJoin.left.contains(peer.userId))
            continue;
        LobbyRemotePeer remote;
        remote.slot = peer.slot;
        remote.userId = peer.peerId;
        remotes.push_back(remote);
    }
    return remotes;
}

void RollbackLobbyDialog::armLateJoinCut()
{
    const int current = rmgk_gekko::current_frame();
    if (current < 0)
    {
        abortLateJoin(QStringLiteral("host_not_running"), true);
        return;
    }
    // A new player changes who the worst connection is, so Auto settles
    // again. The joiner is connected by now, so their ping is known.
    m_playingAlone = false;
    if (m_delayAuto)
        applyLocalFrameDelay(true);
    // Everyone still playing stops at the cut; the joiner was never
    // in this session.
    m_lateJoin.cutFrame = current + kLateJoinCutLeadFrames;
    m_lateJoin.cutSentMs = QDateTime::currentMSecsSinceEpoch();
    m_lateJoin.awaitingCut.clear();
    for (const auto& peer : m_lateJoin.peers)
    {
        if (peer.peerId != m_client->selfUserId() && peer.peerId != m_lateJoin.joiner.userId &&
            !m_lateJoin.left.contains(peer.userId))
            m_lateJoin.awaitingCut.insert(peer.peerId);
    }
    rmgk_gekko::arm_admit_cut(m_lateJoin.cutFrame, true);
    m_client->sendAdmitStep(m_lateJoin.matchId, m_lateJoin.admitId, QStringLiteral("cut"), m_lateJoin.cutFrame);
    emit overlayCenterMessage(lateJoinBanner());
}

// The joiner checks its side before saying it's ready, but a new session that
// starts with either side unconnected holds everyone until someone gives up.
bool RollbackLobbyDialog::connectedToLateJoiner() const
{
    return m_client && m_client->allIcePeersConnected({ m_lateJoin.joiner });
}

void RollbackLobbyDialog::applyLateJoinRoster()
{
    // Label the joiners' ports in the overlay. The replay header was written
    // when the match began, so the replay and its live viewers learn the
    // names from port change records.
    for (const auto& joiner : m_lateJoin.joiners)
    {
        const int slot = joiner.slot;
        if (slot >= 1 && slot <= 4)
        {
            n02::recordingSetPlayerName(slot - 1, joiner.username.toUtf8().constData());
            n02::recordingQueuePortChange(slot, recording_player_names[slot - 1], true);
        }
    }
    emit portLabelsChanged();
}

void RollbackLobbyDialog::onLateJoinTick()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    LateJoin& lj = m_lateJoin;
    switch (lj.role)
    {
    case LateJoinRole::Host:
    {
        if (lj.cutFrame < 0)
        {
            if (lj.readyMs != 0)
            {
                if (connectedToLateJoiner())
                    armLateJoinCut();
                else if (nowMs - lj.readyMs > kLateJoinCutTimeoutMs)
                    abortLateJoin(QStringLiteral("ice_timeout"), true);
            }
            // Later than the joiner's own timeout, which knows why it isn't ready.
            else if (nowMs - lj.startedMs > kLateJoinReadyTimeoutMs + 5'000)
            {
                abortLateJoin(QStringLiteral("joiner_timeout"), true);
            }
            return;
        }
        if (lj.released)
        {
            // The new session runs once the joiner has loaded the state and
            // connected; until then everyone holds on the banner.
            if (rmgk_gekko::session_epoch() == lj.epoch && rmgk_gekko::current_frame() >= 1)
                finishLateJoin(true);
            else if (nowMs - lj.releasedMs > kLateJoinResumeTimeoutMs)
                finishLateJoin(false);
            return;
        }
        const AdmitCutStatus status = rmgk_gekko::admit_cut_status();
        if (status == AdmitCutStatus::Passed)
        {
            abortLateJoin(QStringLiteral("cut_passed"), true);
            return;
        }
        if (status == AdmitCutStatus::Reached && !lj.stateSent)
        {
            std::vector<unsigned char> state;
            if (!rmgk_gekko::take_admit_state(state))
            {
                abortLateJoin(QStringLiteral("state_capture_failed"), true);
                return;
            }
            m_client->sendAdmitBlob(lj.matchId, lj.admitId, QStringLiteral("state"),
                QByteArray(reinterpret_cast<const char*>(state.data()), qsizetype(state.size())));
            lj.stateSent = true;
        }
        if (status == AdmitCutStatus::Reached && lj.stateSent && lj.awaitingCut.isEmpty())
        {
            m_client->sendAdmitStep(lj.matchId, lj.admitId, QStringLiteral("go"));
            rmgk_gekko::release_admit_cut(lateJoinRemotes(), lj.epoch);
            lj.released = true;
            lj.releasedMs = nowMs;
            applyLateJoinRoster();
            return;
        }
        if (nowMs - lj.cutSentMs > kLateJoinCutTimeoutMs)
            abortLateJoin(QStringLiteral("cut_timeout"), true);
        return;
    }

    case LateJoinRole::Player:
    {
        if (lj.released)
        {
            if (rmgk_gekko::session_epoch() == lj.epoch && rmgk_gekko::current_frame() >= 1)
                finishLateJoin(true);
            else if (nowMs - lj.releasedMs > kLateJoinResumeTimeoutMs)
                finishLateJoin(false);
            return;
        }
        if (lj.cutFrame < 0)
        {
            if (nowMs - lj.startedMs > kLateJoinReadyTimeoutMs + kLateJoinCutTimeoutMs)
                abortLateJoin(QStringLiteral("cut_timeout"), true);
            return;
        }
        if (!lj.cutReported)
        {
            const AdmitCutStatus status = rmgk_gekko::admit_cut_status();
            // Not connected yet: hold at the cut. The host gives up on its timeout.
            if ((status == AdmitCutStatus::Reached && connectedToLateJoiner()) ||
                status == AdmitCutStatus::Passed)
            {
                lj.cutReported = true;
                m_client->sendAdmitStep(lj.matchId, lj.admitId, QStringLiteral("cut_ready"),
                                        lj.cutFrame, status == AdmitCutStatus::Reached);
            }
        }
        // The host gives up after its own timeout and aborts; this one only
        // covers losing the host's messages.
        if (nowMs - lj.cutSentMs > kLateJoinCutTimeoutMs + kLateJoinStateTimeoutMs)
            abortLateJoin(QStringLiteral("go_timeout"), true);
        return;
    }

    case LateJoinRole::Joiner:
    {
        if (!lj.launched)
        {
            if (!lj.manifest.isEmpty() && !m_localGameOpen)
                launchAsLateJoiner();
            else if (nowMs - lj.startedMs > kLateJoinReadyTimeoutMs)
                abortLateJoin(QStringLiteral("manifest_timeout"), true);
            return;
        }
        if (!lj.readySent)
        {
            QList<LobbyClient::LobbyMatchPeer> others;
            for (const auto& peer : lj.peers)
            {
                if (peer.peerId != m_client->selfUserId() && !lj.left.contains(peer.userId))
                    others.append(peer);
            }
            const bool connected = m_client->allIcePeersConnected(others);
            if (rmgk_gekko::join_holding() && connected)
            {
                lj.readySent = true;
                lj.readyMs = nowMs;
                // Until now a connection that failed could still restart.
                m_client->setMatchTransportActive(true);
                m_client->sendAdmitStep(lj.matchId, lj.admitId, QStringLiteral("ready"));
            }
            else if (nowMs - lj.startedMs > kLateJoinReadyTimeoutMs)
            {
                abortLateJoin(connected ? QStringLiteral("joiner_timeout") : QStringLiteral("ice_timeout"), true);
            }
            return;
        }
        if (rmgk_gekko::session_epoch() == lj.epoch && rmgk_gekko::current_frame() >= 1)
        {
            finishLateJoin(true);
            return;
        }
        if (!lj.stateReceived && nowMs - lj.readyMs > kLateJoinCutTimeoutMs + kLateJoinStateTimeoutMs)
            abortLateJoin(QStringLiteral("state_timeout"), true);
        else if (lj.stateReceived && nowMs - lj.readyMs > kLateJoinCutTimeoutMs + kLateJoinStateTimeoutMs +
                 kLateJoinResumeTimeoutMs)
            abortLateJoin(QStringLiteral("resume_timeout"), true);
        return;
    }

    default:
        if (m_lateJoinTimer)
            m_lateJoinTimer->stop();
        return;
    }
}

void RollbackLobbyDialog::finishLateJoin(bool resumed)
{
    const LateJoinRole role = m_lateJoin.role;
    const QString names = lateJoinNames();
    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    const QList<LobbyClient::LobbyMatchPeer> joiners = m_lateJoin.joiners;
    m_lateJoin = LateJoin{};
    if (m_lateJoinTimer)
        m_lateJoinTimer->stop();
    emit overlayCenterMessage(QString());

    if (role == LateJoinRole::Joiner || resumed)
    {
        for (const auto& joiner : joiners)
        {
            if (joiner.userId == selfId)
            {
                appendChatSystemLine(CHANNEL_ROOM, tr("You joined the game as P%1.").arg(joiner.slot));
                emit overlayMessage(tr("You joined the game as P%1").arg(joiner.slot));
            }
            else
            {
                appendChatSystemLine(CHANNEL_ROOM, tr("%1 joined the game as P%2.").arg(joiner.username).arg(joiner.slot));
                emit overlayMessage(tr("%1 joined the game as P%2").arg(joiner.username).arg(joiner.slot));
            }
        }
    }
    else
    {
        // The new session is still waiting for the joiner; if they never
        // connect, the lobby drops them and play continues without them.
        appendChatSystemLine(CHANNEL_ROOM, tr("Still waiting for %1 to connect…").arg(names));
    }
    refreshAdmitButtons();
}

void RollbackLobbyDialog::abortLateJoin(const QString& reason, bool tellOthers)
{
    const LateJoin lj = m_lateJoin;
    if (lj.role == LateJoinRole::None)
        return;
    const QString names = lateJoinNames();
    m_lateJoin = LateJoin{};
    if (m_lateJoinTimer)
        m_lateJoinTimer->stop();

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        QString("Rollback late join abort: reason=%1 role=%2 tell=%3")
            .arg(reason).arg(static_cast<int>(lj.role)).arg(tellOthers ? 1 : 0).toUtf8().constData());
    if (tellOthers && m_client)
        m_client->sendAdmitStep(lj.matchId, lj.admitId, QStringLiteral("abort"), 0, false, reason);
    emit overlayCenterMessage(QString());

    // The joiner reads these about themselves; everyone else about the joiner.
    const bool self = lj.role == LateJoinRole::Joiner;
    QString why;
    if (reason == QStringLiteral("joiner_left"))
        why = tr("they left the room");
    else if (reason == QStringLiteral("joiner_timeout") || reason == QStringLiteral("manifest_timeout"))
        why = self ? tr("your game didn't get ready in time") : tr("their game didn't get ready in time");
    else if (reason == QStringLiteral("ice_timeout"))
        why = self ? tr("you couldn't connect to every player") : tr("they couldn't connect to every player");
    else if (reason == QStringLiteral("cut_passed"))
        why = self ? tr("a player was too far ahead to stop in time")
                   : tr("a player was too far ahead to stop in time — try again");
    else if (reason == QStringLiteral("rom_missing"))
        why = self ? tr("you don't have the ROM") : tr("they don't have the ROM");
    else if (reason == QStringLiteral("match_over"))
        why = tr("the match ended");
    else if (reason.endsWith(QStringLiteral("_timeout")) || reason == QStringLiteral("timeout"))
        why = tr("a player stopped responding");
    else if (reason.startsWith(QStringLiteral("manifest")) || reason.startsWith(QStringLiteral("state")))
        why = self ? tr("the game couldn't be sent to you") : tr("the game couldn't be sent to them");
    else if (reason == QStringLiteral("game_closed") || reason == QStringLiteral("left_room") ||
             reason == QStringLiteral("host_not_running"))
        why = tr("the game was closed");
    else
        why = tr("something went wrong (%1)").arg(reason);

    if (lj.role == LateJoinRole::Joiner)
    {
        // Our held game never joined; close it and go back to waiting.
        if (lj.launched)
        {
            emit closeMatchRequested();
            m_currentMatchId = 0;
            m_awaitingEmulationStart = false;
        }
        appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't join the game: %1.").arg(why));
        applyRoomStateBadge(tr("Waiting to join"), stateHex("connecting", isDarkTheme()));
    }
    else
    {
        // A cut that was never released is dropped and play goes on.
        if (!lj.released)
            rmgk_gekko::cancel_admit_cut();
        appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't add %1: %2.").arg(names, why));
        emit overlayMessage(tr("%1 couldn't join").arg(names));
    }
    refreshAdmitButtons();
}

void RollbackLobbyDialog::refreshAdmitButtons()
{
    const bool iAmHost = m_client && m_currentRoomHostId == m_client->selfUserId();
    const bool canAdmit = iAmHost && m_roomLateJoin && m_currentRoomState == "in_game" &&
        m_emulationActive && m_currentMatchId != 0;
    const bool busy = m_lateJoin.role != LateJoinRole::None || m_admitRequestedUserId != 0;
    // Adding one player on a PC adds them all.
    quint64 admitRequestedPeer = 0;
    for (const auto& seat : m_seats)
    {
        if (m_admitRequestedUserId != 0 && seat.userId == m_admitRequestedUserId)
            admitRequestedPeer = seat.peerId;
    }
    for (auto& seat : m_seats)
    {
        if (!seat.row)
            continue;
        const bool waiting = seat.userId != 0 && m_outOfMatchUsers.contains(seat.userId) &&
            m_lateJoinCapableUsers.contains(seat.userId);
        const bool joiningThis = (admitRequestedPeer != 0 && admitRequestedPeer == seat.peerId) ||
            (m_lateJoin.role != LateJoinRole::None && m_lateJoin.joiner.userId == seat.peerId);
        LobbyButton* admit = seat.row->admitButton();
        seat.row->setAdmitVisible(canAdmit && waiting);
        admit->setEnabled(!busy);
        admit->setText(joiningThis ? tr("Adding…") : tr("Add to Game"));
        admit->setToolTip(busy && !joiningThis
            ? tr("Another player is joining right now.")
            : tr("Add this player to the running game. Everyone pauses for a moment while they load in."));
    }
    refreshStartButton();
}

