/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 *
 * Visual approach: palette-based colors so the dialog inherits the active
 * system theme (Modern / Fusion / Fusion Dark), mirroring the Kaillera
 * launcher in Source/RMG/UserInterface/Dialog/Kaillera/KailleraNetplayDialog.cpp.
 * The QSS in applyStylesheet() is intentionally minimal — primary buttons
 * pick up the same #0078D7 Windows blue used by KailleraPrimaryButton.
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │ RMG-K · Rollback Netplay   ● Online   server · 3p · 1r  JD │ ← marquee
 *   ├────────────────────────────┬──────────────┬────────────────┤
 *   │ [Quick Match]  [Create…]   │  Lobby Room  │  Players       │
 *   │ ┌Active Rooms─────────┐    │  ┌────────┐  │ ┌─────────┐    │
 *   │ │ room rows...        │    │  │ chat   │  │ │ rows..  │    │
 *   │ └─────────────────────┘    │  │ stream │  │ └─────────┘    │
 *   │ ┌Ongoing Matches──────┐    │  └────────┘  │                │
 *   │ │ rows...             │    │  [input]     │                │
 *   │ └─────────────────────┘    │              │                │
 *   └────────────────────────────┴──────────────┴────────────────┘
 */
#ifdef NETPLAY

#include "RollbackLobbyDialog.hpp"
#include "LobbyConnectDialog.hpp"
#include "AccountEmailDialog.hpp"
#include "Utilities/DiscordLogin.hpp"
#include "CreateRoomDialog.hpp"
#include "QuickRankedMatchDialog.hpp"
#include "LobbyRegions.hpp"
#include "PlayerRatings.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Ranked.hpp>
#include <RMG-Core/Error.hpp>
#include <RMG-Core/rmgk_gekko.hpp> // request_disconnect_player (instant peer drop)

#include "n02_client.h" // setRecordingStreamSink (broadcast tee)
#include "Utilities/KailleraExport/KrecParser.hpp" // ranked replay metadata

#include <QRegularExpression>

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTextEdit>
#include <QScrollBar>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFile>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QSettings>
#include <QMessageBox>
#include <QInputDialog>
#include <QDateTime>
#include <QShowEvent>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QPixmap>
#include <QTimer>
#include <QFont>
#include <QUrl>
#include <QDebug>
#include <QVariant>
#include <QIcon>
#include <QFile>
#include <QFontMetrics>
#include <QLocale>
#include <QMenu>
#include <QToolButton>
#include <QToolTip>
#include <QHelpEvent>
#include <algorithm>
#include <memory>
#include <vector>
#include <cstdio>
#include <cstring>

using namespace UserInterface::Dialog;

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

// Show the resolved value on the delay combo's Auto entry (data -1),
// e.g. "Auto (3 f)".
void setAutoComboLabel(QComboBox* combo, int resolved)
{
    if (!combo) return;
    const int idx = combo->findData(-1);
    if (idx >= 0) combo->setItemText(idx, QCoreApplication::translate("RollbackLobbyDialog", "Auto (%1 f)").arg(resolved));
}

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

    // Chat column tabs (the bar is only shown while watching a live replay).
    constexpr int kChatTabLobby      = 0;
    constexpr int kChatTabSpectators = 1;

    // MIME type carrying the dragged seat's slot during a host reorder drag.
    const char* const kSeatMime = "application/x-rmgk-seat-slot";

    constexpr int MARGIN_OUTER       = 8;   // dialog/column outside padding
    constexpr int MARGIN_GROUP       = 10;  // banner / accent-frame content inset
    constexpr int SPACING_DEFAULT    = 8;
    constexpr int SPACING_TIGHT      = 4;

    constexpr int MARQUEE_HEIGHT     = 44;
    constexpr int BUTTON_MIN_HEIGHT  = 28;
    constexpr int HERO_BUTTON_HEIGHT = 36;
    constexpr int STATUS_DOT_PX      = 10;

    // Theme-aware status indicator colors. Material 500 on dark backgrounds
    // (readable against deep greys), Material 800 on light (readable against
    // white). Detection mirrors Kaillera's approach at
    // KailleraNetplayDialog.cpp:1303.
    bool isDarkTheme()
    {
        return QApplication::palette().window().color().value() < 128;
    }

    struct StatusColors { const char* ok; const char* wait; const char* fail; const char* idle; };
    StatusColors statusColors()
    {
        return isDarkTheme()
            ? StatusColors{ "#4CAF50", "#FFCA28", "#EF5350", "#BDBDBD" }
            : StatusColors{ "#2E7D32", "#F9A825", "#C62828", "#9E9E9E" };
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

    // ── Per-player / per-state / per-ping accents ──────────────────────
    // Distinct seat colors so P1-P4 read apart at a glance (P1 red,
    // P2 blue, P3 yellow, P4 green — a Smash/Mario-Kart-style set). Light
    // and dark variants keep contrast on both themes; brightness mirrors
    // the way statusColors() picks Material 500 on dark / 800 on light.
    // P3's light variant is a deep goldenrod rather than a true yellow,
    // which would be illegible on a light background.
    QString playerAccentHex(int slot, bool dark)
    {
        switch (slot)
        {
            case 1: return dark ? "#ff6b66" : "#d83a34"; // red
            case 2: return dark ? "#4aa3ff" : "#0078D7"; // blue
            case 3: return dark ? "#ffca45" : "#b5790a"; // yellow
            case 4: return dark ? "#4caf50" : "#2e9e3f"; // green
            default: return dark ? "#bdbdbd" : "#9e9e9e";
        }
    }

    // Soft accent-tinted fill for the seat card. rgba so it sits gently over
    // palette(base) on either theme — heavier on dark where a faint wash
    // would vanish.
    QString playerCardBg(int slot, bool dark)
    {
        struct RGB { int r, g, b; };
        RGB c;
        switch (slot)
        {
            case 1: c = dark ? RGB{255, 107, 102} : RGB{216, 58, 52}; break;
            case 2: c = dark ? RGB{74, 163, 255} : RGB{0, 120, 215};  break;
            case 3: c = dark ? RGB{255, 202, 69} : RGB{181, 121, 10}; break;
            case 4: c = dark ? RGB{76, 175, 80} : RGB{46, 158, 63};   break;
            default: c = RGB{128, 128, 128}; break;
        }
        return QString("rgba(%1, %2, %3, %4)")
            .arg(c.r).arg(c.g).arg(c.b)
            .arg(dark ? "0.14" : "0.08");
    }

    // Same accent at a stronger alpha for the card's hairline border.
    QString playerBorderRgba(int slot, bool dark)
    {
        struct RGB { int r, g, b; };
        RGB c;
        switch (slot)
        {
            case 1: c = dark ? RGB{255, 107, 102} : RGB{216, 58, 52}; break;
            case 2: c = dark ? RGB{74, 163, 255} : RGB{0, 120, 215};  break;
            case 3: c = dark ? RGB{255, 202, 69} : RGB{181, 121, 10}; break;
            case 4: c = dark ? RGB{76, 175, 80} : RGB{46, 158, 63};   break;
            default: c = RGB{128, 128, 128}; break;
        }
        return QString("rgba(%1, %2, %3, 0.55)").arg(c.r).arg(c.g).arg(c.b);
    }

    // Ping quality tiers: green good, amber playable, red rough, grey unknown.
    // (statusColors() already resolves the active theme.)
    QString pingHex(int ms)
    {
        const auto c = statusColors();
        if (ms < 0)   return c.idle; // no measurement yet
        if (ms <= 60) return c.ok;
        if (ms <= 120) return c.wait;
        return c.fail;
    }

    bool liveReplayPreference()
    {
        QSettings settings("RMG-K", "n02");
        return settings.value("RollbackLobby/LiveReplayEnabled", true).toBool();
    }

    QString matchViewersLabel(const LobbyClient::LobbyRoomSummary& room)
    {
        if (!room.broadcasting || room.matchId == 0)
            return {};
        return QStringLiteral("%1 🔴").arg(room.viewerCount);
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
    QString stateHex(const QString& state, bool dark)
    {
        const auto c = statusColors();
        if (state == "idle" || state == "waiting")          return c.ok;
        if (state == "hosting" || state == "in_room")       return dark ? "#4aa3ff" : "#0078D7";
        if (state == "searching" || state == "starting" ||
            state == "connecting")                          return c.wait;
        if (state == "playing" || state == "in_game")       return dark ? "#b78cff" : "#7a44c9"; // purple
        if (state == "spectating")                          return dark ? "#4aa3ff" : "#0078D7";
        if (state == "browsing")                            return c.idle;
        return c.idle;
    }

    // Right-aligned seat meta as rich text: an accent-colored HOST badge, the
    // player's local frame delay, then the ping-tier-colored "N ms".
    // Shared by renderSeatFilled and live refreshes so the render paths never
    // drift. A negative delay hides it; pingMs < 0 hides ping.
    // `status`, when set, replaces the ping cell — used to show a punch being
    // retried or given up on, so a peer we can't reach reads as a stated
    // outcome instead of a number that never appears.
    QString seatMetaHtml(int slot, bool isHost, int pingMs, int frameDelay,
                         bool dark, const QString& status = QString())
    {
        QStringList parts;
        if (isHost)
            parts << QString("<span style='color:%1; font-weight:700;'>%2</span>")
                         .arg(playerAccentHex(slot, dark), QCoreApplication::translate("RollbackLobbyDialog", "HOST"));
        if (frameDelay >= 0)
            parts << QString("<span style='font-weight:600;'>%1</span>")
                         .arg(QCoreApplication::translate("RollbackLobbyDialog", "Frame delay: %1f").arg(frameDelay));
        if (!status.isEmpty())
            parts << status;
        else if (pingMs >= 0)
            parts << QString("<span style='color:%1; font-weight:600;'>%2 ms</span>")
                         .arg(pingHex(pingMs)).arg(pingMs);
        return parts.join(QStringLiteral("&nbsp;·&nbsp;"));
    }

    // Amber while we're still trying, red once we've stopped.
    QString seatProbeStatusHtml(int attempt, int maxAttempts, bool failed, bool dark)
    {
        const auto c = statusColors();
        if (failed)
            return QString("<span style='color:%1; font-weight:600;'>%2</span>").arg(c.fail,QCoreApplication::translate("RollbackLobbyDialog","unreachable"));
        return QString("<span style='color:%1; font-weight:600;'>%2</span>").arg(c.wait,QCoreApplication::translate("RollbackLobbyDialog","retrying %2/%3…").arg(attempt).arg(maxAttempts));
    }

    // Shown before the first ping can be sent. ICE negotiation may take a few
    // seconds on restrictive NATs, so leaving this cell blank makes a healthy
    // connection look stalled.
    QString seatIceStatusHtml(int attempt = 1, int maxAttempts = 20)
    {
        const auto c = statusColors();
        return QString("<span style='color:%1; font-weight:600;'>%2</span>").arg(c.wait,QCoreApplication::translate("RollbackLobbyDialog","Connection attempt %2/%3…").arg(attempt).arg(maxAttempts));
    }

    QString seatIceFailedStatusHtml()
    {
        const auto c = statusColors();
        return QString("<span style='color:%1; font-weight:600;'>%2</span>").arg(c.fail,QCoreApplication::translate("RollbackLobbyDialog","connection failed"));
    }

    // A translucent fill derived from a solid hex — used for the soft pill /
    // badge backgrounds so an accent reads gently over palette(window/base).
    QString tintRgba(const QString& hex, double alpha)
    {
        const QColor c(hex);
        return QString("rgba(%1, %2, %3, %4)")
            .arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    // Apply a +N point delta to the widget's font. Used to bump section
    // titles / hero text consistently without hand-rolled QSS font sizes.
    void bumpFont(QWidget* w, int pointDelta, bool bold = false)
    {
        if (!w) return;
        QFont f = w->font();
        f.setPointSize(f.pointSize() + pointDelta);
        if (bold) f.setBold(true);
        w->setFont(f);
    }

    // QTextEdit::append only follows new lines while the view is exactly at
    // the bottom. When the text grows without an append, the reader is left
    // a few lines up and every later append stays parked there. That happens
    // when the splitter narrows the chat column and lines rewrap, e.g. when
    // the room column's minimum width grows on entering a room or at a match
    // start or end. Remember whether the reader was at the bottom and keep
    // them there when the range grows; scrolling up still holds the view.
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

    // Column-0 role carrying "this player is searching for a match" for the
    // sort override below. Qt::UserRole itself already carries the user id.
    constexpr int kSearchingSortRole = Qt::UserRole + 1;

    // Roles backing the painted Ping and Type columns (see PlayersItemDelegate).
    constexpr int kPingMsRole         = Qt::UserRole + 2; // int; -1 = unknown
    constexpr int kPingEstimateRole   = Qt::UserRole + 3; // value is a region estimate
    constexpr int kConnectionTypeRole = Qt::UserRole + 4; // "wifi", "lan", ...
    constexpr int kShowPingRole       = Qt::UserRole + 5; // false for the self row

    QString connectionTypeName(const QString& connection)
    {
        if (connection == QLatin1String("wifi"))      return QCoreApplication::translate("RollbackLobbyDialog", "Wi-Fi");
        if (connection == QLatin1String("lan"))       return QCoreApplication::translate("RollbackLobbyDialog", "Ethernet");
        if (connection == QLatin1String("cellular"))  return QCoreApplication::translate("RollbackLobbyDialog", "Cellular");
        if (connection == QLatin1String("bluetooth")) return QCoreApplication::translate("RollbackLobbyDialog", "Bluetooth");
        return QString();
    }

    void paintConnectionTypeSymbol(QPainter* painter, const QRectF& rect,
                                   const QString& connection, const QColor& color,
                                   const QFont& font)
    {
        const QPointF center = rect.center();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setFont(font);
        painter->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);

        if (connection == QLatin1String("wifi"))
        {
            // Two arcs and a dot form a crisp Wi-Fi mark without relying on a
            // platform font having the corresponding Unicode glyph.
            const qreal baseY = center.y() + 5.0;
            painter->drawArc(QRectF(center.x() - 9.0, baseY - 9.0, 18.0, 18.0),
                             45 * 16, 90 * 16);
            painter->drawArc(QRectF(center.x() - 5.5, baseY - 5.5, 11.0, 11.0),
                             45 * 16, 90 * 16);
            painter->setBrush(color);
            painter->setPen(Qt::NoPen);
            painter->drawEllipse(QPointF(center.x(), baseY), 1.6, 1.6);
        }
        else if (connection == QLatin1String("lan"))
        {
            // RJ45-style plug: connector body, cable stem, and four pins.
            QPainterPath plug;
            plug.moveTo(center.x() - 7.0, center.y() - 7.0);
            plug.lineTo(center.x() + 7.0, center.y() - 7.0);
            plug.lineTo(center.x() + 7.0, center.y() + 2.0);
            plug.lineTo(center.x() + 3.0, center.y() + 2.0);
            plug.lineTo(center.x() + 3.0, center.y() + 7.0);
            plug.lineTo(center.x() - 3.0, center.y() + 7.0);
            plug.lineTo(center.x() - 3.0, center.y() + 2.0);
            plug.lineTo(center.x() - 7.0, center.y() + 2.0);
            plug.closeSubpath();
            painter->drawPath(plug);
            for (int i = -3; i <= 3; i += 2)
                painter->drawLine(QPointF(center.x() + i, center.y() - 5.0),
                                  QPointF(center.x() + i, center.y() - 2.0));
        }
        else
        {
            const QString compactLabel = connection == QLatin1String("cellular")
                ? QStringLiteral("Cell")
                : connection == QLatin1String("bluetooth")
                    ? QStringLiteral("BT")
                    : QStringLiteral("?");
            painter->setPen(color);
            painter->drawText(rect, Qt::AlignCenter, compactLabel);
        }
        painter->restore();
    }

    class ConnectionTypeLabel final : public QLabel
    {
    public:
        using QLabel::QLabel;

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            paintConnectionTypeSymbol(&painter, rect(),
                                      property("connectionType").toString(),
                                      palette().color(QPalette::WindowText), font());
        }
    };

    void setConnectionTypeLabel(QLabel* label, const QString& connection)
    {
        if (!label)
            return;
        const QString name = connectionTypeName(connection);
        label->setProperty("connectionType", connection);
        label->setToolTip(name.isEmpty() ? QCoreApplication::translate("RollbackLobbyDialog", "Connection type unknown") : name);
        label->update();
    }

    void setCountryFlagLabel(QLabel* label, const QString& country, bool anonymous)
    {
        if (!label)
            return;

        const QString flagResource = QString(":/flags/%1.svg").arg(country.toLower());
        if (anonymous || country.isEmpty() || !QFile::exists(flagResource))
        {
            label->clear();
            label->setToolTip(QString());
            label->setVisible(false);
            return;
        }

        label->setPixmap(QIcon(flagResource).pixmap(18, 12));
        const QLocale::Territory territory = QLocale::codeToTerritory(country);
        const QString countryName = territory != QLocale::AnyTerritory
            ? QLocale::territoryToString(territory)
            : country;
        label->setToolTip(QString(QCoreApplication::translate("RollbackLobbyDialog", "Country: %1")).arg(countryName));
        label->setVisible(true);
    }

    // Player-list row that pins searching players to the top of the list no
    // matter which column or direction the user sorts by. Qt places items
    // that compare "less" first in ascending order and last in descending,
    // so the priority comparison pre-inverts on descending to land searching
    // rows on top either way. Ties (both searching, or neither) fall through
    // to the normal column comparison, so the user's chosen sort still
    // orders each group internally.
    class PlayerListItem : public QTreeWidgetItem
    {
    public:
        using QTreeWidgetItem::QTreeWidgetItem;

        bool operator<(const QTreeWidgetItem& other) const override
        {
            const bool a = data(0, kSearchingSortRole).toBool();
            const bool b = other.data(0, kSearchingSortRole).toBool();
            if (a != b)
            {
                const Qt::SortOrder order = treeWidget()
                    ? treeWidget()->header()->sortIndicatorOrder()
                    : Qt::AscendingOrder;
                return order == Qt::AscendingOrder ? a : b;
            }
            // The Ping column has no cell text (the delegate paints bars), so
            // sort it numerically via the backing role. Unknown (-1) sinks
            // below any real value in ascending order.
            if (treeWidget() && treeWidget()->sortColumn() == 2)
            {
                const int msA = data(2, kPingMsRole).toInt();
                const int msB = other.data(2, kPingMsRole).toInt();
                const int kA = msA < 0 ? (1 << 30) : msA;
                const int kB = msB < 0 ? (1 << 30) : msB;
                if (kA != kB)
                    return kA < kB;
            }
            return QTreeWidgetItem::operator<(other);
        }
    };

    // Paints the players list: column 0 gets its decoration (the country
    // flag) moved to the RIGHT edge of the name cell, column 2 is drawn as
    // Fightcade-style signal bars, and column 3 is a compact connection-type
    // icon. Keeping connection type separate prevents it obscuring the ping
    // bars while still making Wi-Fi versus Ethernet visible at a glance.
    class PlayersItemDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

    protected:
        void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
        {
            QStyledItemDelegate::initStyleOption(option, index);
            if (index.column() == 0)
                option->decorationPosition = QStyleOptionViewItem::Right;
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
        {
            if (index.column() != 2 && index.column() != 3)
            {
                QStyledItemDelegate::paint(painter, option, index);
                return;
            }

            // Draw the item chrome (selection, hover, alternate stripe) with
            // no content, then paint the bars over it.
            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);
            opt.text.clear();
            opt.icon = QIcon();
            QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

            const QRect r = opt.rect;
            if (index.column() == 2)
            {
                if (!index.data(kShowPingRole).toBool())
                    return;

                const int  ping     = index.data(kPingMsRole).toInt();
                const bool estimate = index.data(kPingEstimateRole).toBool();
                const int  level    = ping < 0 ? 0 : ping <= 60 ? 3 : ping <= 120 ? 2 : 1;

                QColor lit(pingHex(ping));
                if (estimate)
                    lit.setAlpha(150);
                QColor unlit = opt.palette.text().color();
                unlit.setAlpha(56);

                constexpr int barW = 4, gap = 2, maxH = 14;
                const int baseY = r.center().y() + maxH / 2;
                const int x0 = r.x() + 8;

                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, false);
                painter->setPen(Qt::NoPen);
                for (int i = 0; i < 3; i++)
                {
                    const int h = maxH * (i + 1) / 3;
                    painter->setBrush(i < level ? lit : unlit);
                    painter->drawRect(x0 + i * (barW + gap), baseY - h, barW, h);
                }
                painter->restore();
                return;
            }

            const QColor iconColor = (opt.state & QStyle::State_Selected)
                ? opt.palette.highlightedText().color()
                : opt.palette.text().color();
            paintConnectionTypeSymbol(painter, r,
                                      index.data(kConnectionTypeRole).toString(),
                                      iconColor, opt.font);
        }
    };
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
    setWindowTitle(tr("RMG-K Lobby"));
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
    connect(m_client, &LobbyClient::quickMatchRematchOffered, this, &RollbackLobbyDialog::onQuickMatchRematchOffered);
    // Report measured pings to room members so the lobby can hold ranked sets
    // to their max ping (only on a change of 10 ms or more).
    connect(m_client, &LobbyClient::pingProbeMeasured, this, [this](quint64 userId, int rttMs) {
        if (m_currentRoomId == 0 || rttMs < 0)
            return;
        bool seated = false;
        for (const auto& seat : m_seats)
            seated |= seat.userId == userId;
        if (!seated)
            return;
        const auto last = m_reportedPeerPings.constFind(userId);
        if (last != m_reportedPeerPings.constEnd() && qAbs(last.value() - rttMs) < 10)
            return;
        m_reportedPeerPings[userId] = rttMs;
        m_client->reportRoomPeerPing(userId, rttMs);
    });
    connect(m_client, &LobbyClient::chatMessageReceived,  this, &RollbackLobbyDialog::onChatMessageReceived);
    connect(m_client, &LobbyClient::adminAuthResult,      this, &RollbackLobbyDialog::onAdminAuthResult);
    connect(m_client, &LobbyClient::modNotice,            this, &RollbackLobbyDialog::onModNotice);
    connect(m_client, &LobbyClient::modListReceived,      this, &RollbackLobbyDialog::onModListReceived);
    connect(m_client, &LobbyClient::matchBegin,           this, &RollbackLobbyDialog::onMatchBegin);
    connect(m_client, &LobbyClient::accountUpdated,       this, &RollbackLobbyDialog::onAccountUpdated);
    connect(m_client, &LobbyClient::rankedResult,         this, &RollbackLobbyDialog::onRankedResult);
    connect(m_client, &LobbyClient::passwordChangeResult, this, &RollbackLobbyDialog::onPasswordChangeResult);
    connect(m_client, &LobbyClient::emailUpdated, this, &RollbackLobbyDialog::onEmailUpdated);
    connect(m_client, &LobbyClient::discordLinkResult,    this, &RollbackLobbyDialog::onDiscordLinkResult);
    connect(m_client, &LobbyClient::replayAccepted,       this, &RollbackLobbyDialog::onReplayAccepted);
    connect(m_client, &LobbyClient::replayDeclined,       this, &RollbackLobbyDialog::onReplayDeclined);
    connect(m_client, &LobbyClient::replayStored,         this, &RollbackLobbyDialog::onReplayStored);
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
    connect(m_client, &LobbyClient::spectateJournal,      this, &RollbackLobbyDialog::onSpectateJournal);
    connect(m_client, &LobbyClient::spectateEnded,        this, &RollbackLobbyDialog::onSpectateEnded);
    connect(m_client, &LobbyClient::spectateFailed,       this, &RollbackLobbyDialog::onSpectateFailed);
    connect(m_client, &LobbyClient::broadcastViewerCount, this, &RollbackLobbyDialog::onBroadcastViewerCount);

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
    // Saved here rather than closeEvent because Esc rejects the dialog
    // without a close event.
    QSettings s("RMG-K", "n02");
    s.setValue("RollbackLobby/WindowGeometry", saveGeometry());
    if (m_playersTree)
        s.setValue(QString("RollbackLobby/PlayersHeaderState.v4.c%1").arg(m_playersTree->columnCount()),
                   m_playersTree->header()->saveState());
    if (m_roomsTree)
        s.setValue(QString("RollbackLobby/RoomsHeaderState.c%1").arg(m_roomsTree->columnCount()),
                   m_roomsTree->header()->saveState());
    if (m_matchesTree)
        s.setValue(QString("RollbackLobby/MatchesHeaderState.v3.c%1").arg(m_matchesTree->columnCount()),
                   m_matchesTree->header()->saveState());
    if (m_browseSplitter)
        s.setValue("RollbackLobby/BrowseSplitterState", m_browseSplitter->saveState());

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
    auto* container = new QWidget(this);
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    lay->addWidget(buildMarquee());

    m_splitter = new QSplitter(Qt::Horizontal, container);
    m_splitter->setHandleWidth(1);
    m_splitter->setChildrenCollapsible(false);

    auto* leftCol = new QWidget(container);
    auto* leftLay = new QVBoxLayout(leftCol);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(0);

    m_roomsStack = new QStackedWidget(container);
    m_roomsStack->addWidget(buildBrowseView());   // index 0
    m_roomsStack->addWidget(buildInRoomView());   // index 1
    leftLay->addWidget(m_roomsStack, 1);
    m_splitter->addWidget(leftCol);

    m_splitter->addWidget(buildChatColumn());
    m_splitter->addWidget(buildPlayersColumn());

    m_splitter->setStretchFactor(0, 5);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setStretchFactor(2, 2);
    // Give the player pane enough first-run width for Player, State, Ping,
    // and the compact connection Type column without squeezing names.
    m_splitter->setSizes({560, 340, 320});

    lay->addWidget(m_splitter, 1);
    return container;
}

// ── Marquee ──────────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildMarquee()
{
    m_marquee = new QFrame(this);
    m_marquee->setObjectName("LobbyMarquee");
    m_marquee->setFixedHeight(MARQUEE_HEIGHT);

    auto* h = new QHBoxLayout(m_marquee);
    h->setContentsMargins(MARGIN_OUTER * 2, 0, MARGIN_OUTER * 2, 0);
    h->setSpacing(SPACING_DEFAULT * 2);

    m_brandLabel = new QLabel(tr("RMG-K · Rollback Netplay"), this);
    m_brandLabel->setObjectName("LobbyBrand");
    bumpFont(m_brandLabel, 1, /*bold=*/true);
    h->addWidget(m_brandLabel);

    h->addSpacing(SPACING_DEFAULT);

    // Connection status: a colored LED dot + status-colored label. No filled
    // pill background — just the dot and text follow the connection state
    // (set in updateStatusIndicator).
    m_statusPill = new QFrame(this);
    m_statusPill->setObjectName("StatusPill");
    auto* pillLay = new QHBoxLayout(m_statusPill);
    pillLay->setContentsMargins(0, 0, 0, 0);
    pillLay->setSpacing(SPACING_TIGHT + 2);

    m_statusLed = new QLabel(m_statusPill);
    m_statusLed->setFixedSize(STATUS_DOT_PX, STATUS_DOT_PX);
    m_statusLed->setStyleSheet(
        QString("background-color: %1; border-radius: %2px;")
            .arg(statusColors().idle).arg(STATUS_DOT_PX / 2));
    pillLay->addWidget(m_statusLed);

    m_statusText = new QLabel(tr("Offline"), m_statusPill);
    m_statusText->setStyleSheet(QString("color: %1; font-weight: 600;").arg(statusColors().idle));
    pillLay->addWidget(m_statusText);
    h->addWidget(m_statusPill);

    h->addSpacing(SPACING_DEFAULT * 2);

    m_serverMeta = new QLabel(tr("Not connected"), this);
    // PlaceholderText is the right role for "secondary body text" — Mid is
    // a border-tone and disappears on Fusion Dark. PlaceholderText derives
    // from palette(text) with reduced alpha, so it reads on every theme.
    m_serverMeta->setForegroundRole(QPalette::PlaceholderText);
    h->addWidget(m_serverMeta);

    h->addStretch(1);

    // Our name. Hovering shows our ranked standings; clicking opens the
    // account menu (logged-in accounts only).
    m_userButton = new QToolButton(this);
    m_userButton->setObjectName("UserButton");
    m_userButton->setText(tr("User: —"));
    m_userButton->setAutoRaise(true);
    m_userButton->setPopupMode(QToolButton::InstantPopup);
    m_userButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_userButton->installEventFilter(this);
    auto* accountMenu = new QMenu(m_userButton);
    accountMenu->setToolTipsVisible(true);
    m_changePasswordAction = accountMenu->addAction(tr("Change password…"));
    connect(m_changePasswordAction, &QAction::triggered, this, &RollbackLobbyDialog::onChangePassword);
    m_emailAction = accountMenu->addAction(tr("Add email address…"));
    m_emailAction->setVisible(false);
    connect(m_emailAction, &QAction::triggered, this, &RollbackLobbyDialog::onEmailAction);
    m_discordAction = accountMenu->addAction(tr("Link Discord…"));
    m_discordAction->setVisible(false);
    connect(m_discordAction, &QAction::triggered, this, &RollbackLobbyDialog::onDiscordButton);
    accountMenu->addSeparator();
    m_logOutAction = accountMenu->addAction(tr("Log out"));
    m_logOutAction->setToolTip(tr("Forget the saved login on this computer and return to the login screen."));
    connect(m_logOutAction, &QAction::triggered, this, &RollbackLobbyDialog::onLogOut);
    h->addWidget(m_userButton);

    m_ratings = new PlayerRatings(this);
    connect(m_ratings, &PlayerRatings::updated, this, [this](const QString& name) {
        // refresh a card that's still showing
        if (QToolTip::isVisible() && m_ratingsTipWidget &&
            name.compare(m_ratingsTipName, Qt::CaseInsensitive) == 0)
            showRatingsTip(m_ratingsTipName, m_ratingsTipBase, m_ratingsTipPos, m_ratingsTipWidget);
    });

    m_discordLogin = new Utilities::DiscordLogin(this);
    connect(m_discordLogin, &Utilities::DiscordLogin::availabilityKnown, this, [this](bool available) {
        m_discordAvailable = available;
        updateDiscordButton();
    });
    connect(m_discordLogin, &Utilities::DiscordLogin::codeReceived, this,
            [this](const QString& code, const QString& verifier, const QString& redirectUri) {
        updateDiscordButton();
        if (m_client && m_client->isVerified())
            m_client->linkDiscord(code, verifier, redirectUri);
    });
    connect(m_discordLogin, &Utilities::DiscordLogin::failed, this, [this](const QString& message) {
        updateDiscordButton();
        QMessageBox::warning(this, tr("Link Discord"), message);
    });

    return m_marquee;
}

// ── Browse view ──────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildBrowseView()
{
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER);
    lay->setSpacing(SPACING_DEFAULT);

    // ── "You're in a room" banner — shown only when m_currentRoomId != 0
    //    so the user can pop back into their seat after browsing other rooms. ──
    m_inRoomBanner = new QFrame(this);
    m_inRoomBanner->setObjectName("InRoomBanner");
    auto* bannerLay = new QHBoxLayout(m_inRoomBanner);
    bannerLay->setContentsMargins(MARGIN_GROUP, SPACING_TIGHT, MARGIN_GROUP, SPACING_TIGHT);
    bannerLay->setSpacing(SPACING_DEFAULT);

    auto* bannerIcon = new QLabel(QStringLiteral("★"), this);
    bumpFont(bannerIcon, 2, /*bold=*/true);
    bannerLay->addWidget(bannerIcon);

    m_bannerText = new QLabel(tr("You're in a room"), this);
    bumpFont(m_bannerText, 0, /*bold=*/true);
    bannerLay->addWidget(m_bannerText, 1);

    m_bannerReturn = new QPushButton(tr("Return to Room  →"), this);
    m_bannerReturn->setObjectName("BannerReturnBtn");
    m_bannerReturn->setMinimumHeight(BUTTON_MIN_HEIGHT);
    m_bannerReturn->setCursor(Qt::PointingHandCursor);
    m_bannerReturn->setAutoDefault(false);
    m_bannerReturn->setDefault(false);
    connect(m_bannerReturn, &QPushButton::clicked, this, &RollbackLobbyDialog::switchToInRoomView);
    bannerLay->addWidget(m_bannerReturn);

    m_inRoomBanner->setVisible(false);
    lay->addWidget(m_inRoomBanner);

    // ── Hero row: Quick Match (primary blue), Quick Ranked Match (gold) + Create Room (secondary) ──
    auto* heroRow = new QHBoxLayout;
    heroRow->setSpacing(SPACING_DEFAULT);

    m_quickMatchBtn = new QPushButton(QStringLiteral("⚡  ") + tr("Quick Match"), this);
    m_quickMatchBtn->setObjectName("QuickMatchBtn");
    m_quickMatchBtn->setEnabled(false);
    m_quickMatchBtn->setAutoDefault(false);
    m_quickMatchBtn->setDefault(false);
    m_quickMatchBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_quickMatchBtn->setCursor(Qt::PointingHandCursor);
    m_quickMatchBtn->setToolTip(
        tr("Auto-match with another player searching for the selected game.\n"
           "After each match it searches again, with a new opponent, until you stop the session.\n"
           "Uses Auto frame delay unless remembering your last choice is enabled.\n"
           "Your frame delay can be changed during the warmup. Prediction defaults to 7 frames."));

    m_quickRankedBtn = new QPushButton(QStringLiteral("🏆  ") + tr("Quick Ranked Match"), this);
    m_quickRankedBtn->setObjectName("QuickRankedBtn");
    m_quickRankedBtn->setEnabled(false);
    m_quickRankedBtn->setAutoDefault(false);
    m_quickRankedBtn->setDefault(false);
    m_quickRankedBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_quickRankedBtn->setCursor(Qt::PointingHandCursor);
    m_quickRankedBtn->setToolTip(tr(
        "Search for a ranked set on the selected game against anyone looking for the same\n"
        "format and roster. After each set it searches again, with a new opponent, until you\n"
        "stop the session. Needs a logged-in account."));

    m_createRoomBtn = new QPushButton(tr("Create Room…"), this);
    m_createRoomBtn->setObjectName("CreateRoomBtn");
    m_createRoomBtn->setAutoDefault(false);
    m_createRoomBtn->setDefault(false);
    m_createRoomBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_createRoomBtn->setCursor(Qt::PointingHandCursor);

    connect(m_quickMatchBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onQuickMatchClicked);
    connect(m_quickRankedBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onQuickRankedMatchClicked);
    connect(m_createRoomBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onCreateRoomClicked);

    heroRow->addWidget(m_quickMatchBtn, 1);
    heroRow->addWidget(m_quickRankedBtn, 1);
    heroRow->addWidget(m_createRoomBtn, 0);
    lay->addLayout(heroRow);

    m_sessionLabel = new QLabel(this);
    m_sessionLabel->setObjectName("RankedSessionLabel");
    m_sessionLabel->setVisible(false);
    lay->addWidget(m_sessionLabel);

    // ── Game picker — pulls from the RMG-K library. Both Create Room and
    //    Quick Match key off the selected game's MD5. ──
    auto* gameRow = new QHBoxLayout;
    gameRow->setSpacing(SPACING_DEFAULT);
    auto* gameLbl = new QLabel(tr("Game:"), this);
    m_browseRomCombo = new QComboBox(this);
    m_browseRomCombo->setObjectName("BrowseRomCombo");
    m_browseRomCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    // Searchable: type to filter (substring, case-insensitive) or scroll the
    // full list. NoInsert keeps typed text from being added as a bogus entry.
    m_browseRomCombo->setEditable(true);
    m_browseRomCombo->setInsertPolicy(QComboBox::NoInsert);
    if (auto* comp = m_browseRomCombo->completer())
    {
        comp->setCompletionMode(QCompleter::PopupCompletion);
        comp->setFilterMode(Qt::MatchContains);
        comp->setCaseSensitivity(Qt::CaseInsensitive);
    }
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
            });
    connect(m_browseRomCombo, &QComboBox::currentTextChanged,
            this, [this](const QString&) {
                refreshSameGameFilter();
                updateHeroButtons(); // Quick Ranked Match only for ranked games
            });
    gameRow->addWidget(gameLbl, 0);
    gameRow->addWidget(m_browseRomCombo, 1);
    lay->addLayout(gameRow);
    populateBrowseRoms(); // fill from whatever library we have so far

    // ── Active Rooms / Ongoing Matches — panes of a vertical splitter so
    //    the matches list height is user-adjustable ──
    m_browseSplitter = new QSplitter(Qt::Vertical, this);
    m_browseSplitter->setHandleWidth(1);
    m_browseSplitter->setChildrenCollapsible(false);

    auto* roomsPane = new QWidget(this);
    auto* roomsLay = new QVBoxLayout(roomsPane);
    roomsLay->setContentsMargins(0, 0, 0, 0);
    roomsLay->setSpacing(SPACING_DEFAULT);

    auto* roomsHeader = new QLabel(tr("ACTIVE ROOMS"), this);
    roomsHeader->setProperty("class", "SectionHeader");
    auto* roomsHeaderRow = new QHBoxLayout;
    roomsHeaderRow->setContentsMargins(0, 0, 0, 0);
    roomsHeaderRow->setSpacing(SPACING_DEFAULT);
    roomsHeaderRow->addWidget(roomsHeader, 1);

    m_sameGameFilterCheck = new QCheckBox(tr("Same game only"), this);
    m_sameGameFilterCheck->setToolTip(
        tr("Only show rooms and matches for the selected game."));
    m_sameGameFilterCheck->setEnabled(!selectedBrowseRom().isEmpty());
    connect(m_sameGameFilterCheck, &QCheckBox::toggled,
            this, [this](bool) { onRoomListChanged(); });
    roomsHeaderRow->addWidget(m_sameGameFilterCheck);
    roomsLay->addLayout(roomsHeaderRow);

    m_roomsTree = new QTreeWidget(this);
    m_roomsTree->setObjectName("RoomsTree");
    m_roomsTree->setHeaderLabels({ tr("Name"), tr("Host"), tr("Ping"), tr("ROM"), tr("Seats") });
    m_roomsTree->setRootIsDecorated(false);
    m_roomsTree->setSortingEnabled(true);
    m_roomsTree->setAlternatingRowColors(true);
    m_roomsTree->setFrameShape(QFrame::NoFrame);
    m_roomsTree->setUniformRowHeights(true);
    // Columns are clamped to the viewport like the players list
    // (clampTreeColumns): no Qt Stretch section, Name is the fill column, and
    // a horizontal scrollbar can never be needed.
    m_roomsTree->header()->setStretchLastSection(false);
    m_roomsTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_roomsTree->setColumnWidth(0, 180);
    m_roomsTree->setColumnWidth(1, 110);
    m_roomsTree->setColumnWidth(2, 70);
    m_roomsTree->setColumnWidth(3, 160);
    m_roomsTree->setColumnWidth(4, 60);
    {
        // The column count is part of the key: restoring a state saved for a
        // different column layout half-applies and breaks the header (missing
        // section dividers, wrong stretch section).
        QSettings s("RMG-K", "n02");
        const QString key = QString("RollbackLobby/RoomsHeaderState.c%1").arg(m_roomsTree->columnCount());
        const QByteArray headerState = s.value(key).toByteArray();
        if (!headerState.isEmpty())
            m_roomsTree->header()->restoreState(headerState);
        // restoreState can resurrect a stretch section and per-section modes
        // (older saves have stretch-last baked in); re-pin ours AFTER it. Any
        // overflowing restored widths are pulled back in by the first viewport
        // Resize.
        m_roomsTree->header()->setStretchLastSection(false);
        m_roomsTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    }
    m_roomsTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_roomsTree->header(), &QHeaderView::sectionResized, this,
            [this](int index, int, int) { clampTreeColumns(m_roomsTree, index); });
    m_roomsTree->viewport()->installEventFilter(this);
    connect(m_roomsTree, &QTreeWidget::itemDoubleClicked,
            this, &RollbackLobbyDialog::onRoomDoubleClicked);
    // Selecting a room measures the host on demand, so the ping column can be
    // an estimate until someone actually cares about that room.
    connect(m_roomsTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item)
                    probeOnDemand(item->data(0, Qt::UserRole + 1).toULongLong());
            });
    // Moderators get a right-click "close room" action; a no-op for everyone else.
    m_roomsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_roomsTree, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) { showAdminRoomMenu(m_roomsTree, pos); });
    roomsLay->addWidget(m_roomsTree, 1);
    m_browseSplitter->addWidget(roomsPane);

    // ── Ongoing Matches ──
    auto* matchesPane = new QWidget(this);
    auto* matchesLay = new QVBoxLayout(matchesPane);
    matchesLay->setContentsMargins(0, 0, 0, 0);
    matchesLay->setSpacing(SPACING_DEFAULT);

    auto* matchesHeader = new QLabel(tr("ONGOING MATCHES"), this);
    matchesHeader->setProperty("class", "SectionHeader");
    matchesLay->addWidget(matchesHeader);

    m_matchesTree = new QTreeWidget(this);
    m_matchesTree->setObjectName("MatchesTree");
    // Keep the wide text columns first and the compact match stats at the right.
    m_matchesTree->setHeaderLabels({ tr("Players"), tr("ROM"), tr("Duration"), tr("Viewers") });
    m_matchesTree->setRootIsDecorated(false);
    m_matchesTree->setSortingEnabled(true);
    m_matchesTree->sortItems(2, Qt::AscendingOrder);
    m_matchesTree->setAlternatingRowColors(true);
    m_matchesTree->setFrameShape(QFrame::NoFrame);
    m_matchesTree->setMinimumHeight(70);
    // Clamped to the viewport like the rooms tree above; Players is the fill.
    m_matchesTree->header()->setStretchLastSection(false);
    m_matchesTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_matchesTree->setColumnWidth(0, 220);
    m_matchesTree->setColumnWidth(1, 160);
    m_matchesTree->setColumnWidth(2, 80);
    m_matchesTree->setColumnWidth(3, 64);
    {
        // v3 keeps the stats compact while preserving saved text column widths.
        QSettings s("RMG-K", "n02");
        const QString key = QString("RollbackLobby/MatchesHeaderState.v3.c%1").arg(m_matchesTree->columnCount());
        const QByteArray headerState = s.value(key).toByteArray();
        if (!headerState.isEmpty())
            m_matchesTree->header()->restoreState(headerState);
        else
        {
            const QString oldKey = QString("RollbackLobby/MatchesHeaderState.v2.c%1").arg(m_matchesTree->columnCount());
            const QByteArray oldState = s.value(oldKey).toByteArray();
            if (!oldState.isEmpty())
                m_matchesTree->header()->restoreState(oldState);
            m_matchesTree->setColumnWidth(2, 80);
            m_matchesTree->setColumnWidth(3, 64);
        }
        m_matchesTree->header()->setStretchLastSection(false);
        m_matchesTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    }
    m_matchesTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_matchesTree->header(), &QHeaderView::sectionResized, this,
            [this](int index, int, int) { clampTreeColumns(m_matchesTree, index); });
    m_matchesTree->viewport()->installEventFilter(this);
    connect(m_matchesTree, &QTreeWidget::itemDoubleClicked,
            this, &RollbackLobbyDialog::onMatchDoubleClicked);
    m_matchesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_matchesTree, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) { showAdminRoomMenu(m_matchesTree, pos); });
    matchesLay->addWidget(m_matchesTree, 1);
    m_browseSplitter->addWidget(matchesPane);

    m_browseSplitter->setStretchFactor(0, 1);
    m_browseSplitter->setStretchFactor(1, 0);
    m_browseSplitter->setSizes({420, 150});
    {
        QSettings s("RMG-K", "n02");
        const QByteArray splitState = s.value("RollbackLobby/BrowseSplitterState").toByteArray();
        if (!splitState.isEmpty())
            m_browseSplitter->restoreState(splitState);
    }
    lay->addWidget(m_browseSplitter, 1);

    // Tick the ongoing-match durations once a second (the room list only
    // refreshes on events, so the timer keeps the elapsed time live).
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
    lay->setContentsMargins(MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER);
    lay->setSpacing(SPACING_DEFAULT);

    // ── Header (room info) — no group-box wrapper; the hero title carries
    //    its own visual weight, and the SEATS section header below provides
    //    the only divider this column needs. ──

    // Top row: breadcrumb + state label
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(SPACING_DEFAULT);
    auto* breadcrumb = new QPushButton(tr("←  Browse Rooms"), this);
    breadcrumb->setFlat(true);
    breadcrumb->setCursor(Qt::PointingHandCursor);
    breadcrumb->setAutoDefault(false);
    breadcrumb->setToolTip(tr("Switch to the room list. You stay in your seat — use Leave Room to actually exit."));
    connect(breadcrumb, &QPushButton::clicked, this, &RollbackLobbyDialog::switchToRoomsView);
    topRow->addWidget(breadcrumb);
    topRow->addStretch(1);

    m_roomStateLabel = new QLabel(tr("Waiting"), this);
    topRow->addWidget(m_roomStateLabel);
    applyRoomStateBadge(tr("Waiting"), stateHex("waiting", isDarkTheme()));
    lay->addLayout(topRow);

    // ROM title (large). Word-wrap so a long ROM name doesn't pin a wide
    // minimum width on the whole left column (it's shared via the stacked
    // widget with the browse view).
    m_roomTitle = new QLabel("—", this);
    bumpFont(m_roomTitle, 4, /*bold=*/true);
    m_roomTitle->setWordWrap(true);
    lay->addWidget(m_roomTitle);

    m_roomRegionLabel = new QLabel(this);
    m_roomRegionLabel->setTextFormat(Qt::RichText);
    m_roomRegionLabel->setContentsMargins(0, SPACING_TIGHT, 0, 0);
    m_roomRegionLabel->setVisible(false);
    lay->addWidget(m_roomRegionLabel);

    // ── Rollback settings row ──
    auto* settingsRow = new QHBoxLayout;
    settingsRow->setContentsMargins(0, SPACING_TIGHT, 0, 0);
    settingsRow->setSpacing(SPACING_DEFAULT);

    const QString delayTip = tr("Frames of input delay added before sending to peer.\n"
        "Higher delay = fewer rollbacks but more input latency.\n"
        "Recommended: 2 for ~80ms RTT, 3-4 for ~150ms RTT.\n"
        "\n"
        "Local setting — each player may choose a different value.");
    const QString predictionTip = tr("Maximum frames the rollback engine may predict ahead.\n"
        "Higher prediction tolerates longer network stalls but can produce larger rollbacks.\n"
        "Default: 7 frames.\n"
        "\n"
        "Room setting — only the host may change this value.");

    static const QList<int> DELAY_OPTIONS = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const QList<int> PREDICTION_OPTIONS = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    // Delay uses -1 as its Auto sentinel so numeric zero remains selectable.
    // Prediction uses zero as its Default sentinel since its range begins at 1.
    auto fillFrameCombo = [](QComboBox* combo, const QString& autoLabel,
                             int sentinel, const QList<int>& options) {
        combo->addItem(autoLabel, sentinel);
        for (int v : options)
            combo->addItem(QString(tr("%1 f")).arg(v), v);
        combo->setCurrentIndex(0); // default to Auto
    };

    auto* delayLbl = new QLabel(tr("Your frame delay:"), this);
    m_delayCombo = new QComboBox(this);
    m_delayCombo->setObjectName("LobbyCombo");
    // Re-measure when the Auto entry grows into "Auto (2 f)" so the resolved
    // value isn't clipped.
    m_delayCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    fillFrameCombo(m_delayCombo, tr("Auto"), -1, DELAY_OPTIONS);
    m_delayCombo->setToolTip(delayTip);
    // Stash the explainer so onRoomStateChanged can restore it after a
    // disabled stint (host became host again, or match ended).
    m_delayCombo->setProperty("originalTip", delayTip);
    settingsRow->addWidget(delayLbl);
    settingsRow->addWidget(m_delayCombo);

    settingsRow->addSpacing(SPACING_DEFAULT * 2);

    auto* predictionLbl = new QLabel(tr("Prediction:"), this);
    m_predictionCombo = new QComboBox(this);
    m_predictionCombo->setObjectName("LobbyCombo");
    m_predictionCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    fillFrameCombo(m_predictionCombo,
                   tr("Default (%1 f)").arg(kDefaultPredictionWindow),
                   0, PREDICTION_OPTIONS);
    m_predictionCombo->setToolTip(predictionTip);
    m_predictionCombo->setProperty("originalTip", predictionTip);
    settingsRow->addWidget(predictionLbl);
    settingsRow->addWidget(m_predictionCombo);

    settingsRow->addStretch(1);
    lay->addLayout(settingsRow);

    // Recording toggles live on their own row beneath the combos. Keeping them
    // off the combo row keeps that row's minimum width small, so the left
    // column can be dragged narrower (more room for chat / players).
    auto* toggleRow = new QHBoxLayout;
    toggleRow->setContentsMargins(0, 0, 0, 0);
    toggleRow->setSpacing(SPACING_DEFAULT);

    // Per-player local saving, independent of the host's Live Replay choice.
    // Editable before the match and initialized from the cap-aware default.
    m_recordCheck = new QCheckBox(tr("Record locally"), this);
    const bool recordingDefault = CoreGetKailleraEffectiveRecordingDefault();
    n02_kaillera_recording_enabled = recordingDefault;
    m_recordCheck->setChecked(recordingDefault);
    connect(m_recordCheck, &QCheckBox::toggled, this, [](bool checked) {
        n02_kaillera_recording_enabled = checked;
    });
    toggleRow->addWidget(m_recordCheck);

    // Broadcast replay bytes to the server without requiring a local file.
    m_broadcastCheck = new QCheckBox(tr("Live Replay"), this);
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
    toggleRow->addWidget(m_broadcastCheck);

    // Ranked: host-only, between games. The server validates the ladder and
    // the seated players; ROOM_STATE echoes the result.
    m_rankedCheck = new QCheckBox(tr("Ranked"), this);
    m_rankedCheck->setVisible(false);
    connect(m_rankedCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_suppressSettingsSignal || m_currentRoomId == 0 || !m_client) return;
        QJsonObject ranked;
        ranked["enabled"] = checked;
        // Keep the room's previous format and roster when switching back on.
        if (checked && m_currentRoomRanked.contains("ladder"))
            ranked["ladderId"] = m_currentRoomRanked.value("ladder").toString();
        if (checked && m_currentRoomRanked.contains("rulesetId"))
            ranked["rulesetId"] = m_currentRoomRanked.value("rulesetId").toString();
        if (checked && m_currentRoomRanked.contains("maxPing"))
            ranked["maxPing"] = m_currentRoomRanked.value("maxPing").toInt();
        m_client->updateRoomRanked(ranked);
    });
    toggleRow->addWidget(m_rankedCheck);
    toggleRow->addStretch(1);
    updateRecordingControls();

    lay->addLayout(toggleRow);

    // Delay is local to each player and is published for the seat display.
    auto applyDelay = [this]() {
        if (m_suppressSettingsSignal) return;
        if (m_currentRoomId == 0) return;
        m_delayAuto = (m_delayCombo->currentData().toInt() == -1);
        if (CoreSettingsGetBoolValue(SettingsID::Rollback_RememberInputDelay))
        {
            QSettings settings("RMG-K", "n02");
            settings.setValue("RollbackLobby/LastInputDelay", m_delayCombo->currentData().toInt());
        }
        applyLocalFrameDelay(true);
    };
    connect(m_delayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, applyDelay);

    // Prediction is authoritative room state. The server repeats the host check
    // so enabling this widget in a modified client cannot grant permission.
    auto applyPrediction = [this]() {
        if (m_suppressSettingsSignal) return;
        if (m_currentRoomId == 0) return;
        m_predictionAuto = (m_predictionCombo->currentData().toInt() == 0);
        applyRoomPrediction(true);
    };
    connect(m_predictionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, applyPrediction);

    // ── Seats — bold section header + vertical player-list rows ──
    auto* seatsHeaderRow = new QHBoxLayout;
    seatsHeaderRow->setContentsMargins(0, SPACING_DEFAULT, 0, 0);
    seatsHeaderRow->setSpacing(SPACING_TIGHT);
    auto* seatsHeader = new QLabel(tr("SEATS"), this);
    seatsHeader->setProperty("class", "SectionHeader");
    seatsHeaderRow->addWidget(seatsHeader, 0, Qt::AlignBaseline);
    m_seatCountLabel = new QLabel(this);
    bumpFont(m_seatCountLabel, -1);
    seatsHeaderRow->addWidget(m_seatCountLabel, 0, Qt::AlignBaseline);
    seatsHeaderRow->addStretch(1);
    lay->addLayout(seatsHeaderRow);

    auto* seatsBox = new QWidget(this);
    m_seatsBox = seatsBox;
    seatsBox->setAcceptDrops(true);      // host reorder: drop a seat onto another
    seatsBox->installEventFilter(this);  // handles DragEnter/Move/Drop
    auto* seatsLay = new QVBoxLayout(seatsBox);
    seatsLay->setContentsMargins(0, SPACING_TIGHT, 0, 0);
    seatsLay->setSpacing(SPACING_TIGHT);
    for (int i = 0; i < 4; ++i)
    {
        buildSeatRow(m_seats[i], i + 1, seatsBox);
        seatsLay->addWidget(m_seats[i].row);
        renderSeatEmpty(m_seats[i]);
    }
    lay->addWidget(seatsBox);

    // ── Room chat — sits directly below the seats, with its own input. The
    //    lobby chat stays in the dedicated middle column. ──
    auto* roomChatHeader = new QLabel(tr("ROOM CHAT"), this);
    roomChatHeader->setProperty("class", "SectionHeader");
    roomChatHeader->setContentsMargins(0, SPACING_DEFAULT, 0, 0);
    lay->addWidget(roomChatHeader);

    auto* roomChatCard = new QFrame(this);
    roomChatCard->setObjectName("LobbyCard");
    auto* roomChatLay = new QVBoxLayout(roomChatCard);
    roomChatLay->setContentsMargins(SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT);
    roomChatLay->setSpacing(SPACING_DEFAULT);

    m_chatViewRoom = new QTextEdit(roomChatCard);
    m_chatViewRoom->setReadOnly(true);
    m_chatViewRoom->setLineWrapMode(QTextEdit::WidgetWidth);
    m_chatViewRoom->setFrameShape(QFrame::NoFrame);
    keepChatAtBottom(m_chatViewRoom);
    roomChatLay->addWidget(m_chatViewRoom, 1);

    m_roomChatInput = new QLineEdit(roomChatCard);
    m_roomChatInput->setPlaceholderText(tr("Message the room…"));
    m_roomChatInput->setEnabled(false);
    m_roomChatInput->setMaxLength(500); // server truncates at 500; cap client-side too
    m_roomChatInput->setMinimumHeight(BUTTON_MIN_HEIGHT);
    connect(m_roomChatInput, &QLineEdit::returnPressed, this, &RollbackLobbyDialog::onRoomChatSendClicked);
    roomChatLay->addWidget(m_roomChatInput);

    lay->addWidget(roomChatCard, 1);   // expands to fill between seats and actions

    // ── Action bar ──
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(SPACING_DEFAULT);

    m_startBtn = new QPushButton(tr("Start Game"), this);
    m_startBtn->setObjectName("StartGameBtn");
    m_startBtn->setEnabled(false);
    m_startBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_startBtn->setCursor(Qt::PointingHandCursor);
    m_startBtn->setAutoDefault(false);
    m_startBtn->setDefault(false);
    connect(m_startBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onStartGameClicked);

    m_dropBtn = new QPushButton(tr("Drop Game"), this);
    m_dropBtn->setObjectName("DropBtn");
    m_dropBtn->setEnabled(false);
    m_dropBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_dropBtn->setAutoDefault(false);
    m_dropBtn->setDefault(false);
    m_dropBtn->setCursor(Qt::PointingHandCursor);
    m_dropBtn->setToolTip(tr("Stop playing the current match. If you're the host, the match ends for everyone."));
    connect(m_dropBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onDropGameClicked);

    m_leaveBtn = new QPushButton(tr("Leave Room"), this);
    m_leaveBtn->setObjectName("LeaveBtn");
    m_leaveBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_leaveBtn->setAutoDefault(false);
    m_leaveBtn->setDefault(false);
    m_leaveBtn->setCursor(Qt::PointingHandCursor);
    m_leaveBtn->setToolTip(tr("Leave the room and return to the lobby."));
    connect(m_leaveBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onLeaveRoomClicked);

    m_stopSessionBtn = new QPushButton(tr("Stop After This Set"), this);
    m_stopSessionBtn->setObjectName("StopSessionBtn");
    m_stopSessionBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_stopSessionBtn->setAutoDefault(false);
    m_stopSessionBtn->setDefault(false);
    m_stopSessionBtn->setCursor(Qt::PointingHandCursor);
    m_stopSessionBtn->setToolTip(tr("End your ranked session: finish this set, then return to the lobby "
                                    "instead of searching for the next match."));
    m_stopSessionBtn->setVisible(false);
    connect(m_stopSessionBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onStopSessionClicked);

    m_keepTeammateCheck = new QCheckBox(tr("Keep Teammate"), this);
    m_keepTeammateCheck->setObjectName("KeepTeammateCheck");
    m_keepTeammateCheck->setToolTip(tr("When this set ends and your teammate keeps you too, the two of you search "
                                       "for the next set as a team."));
    m_keepTeammateCheck->setVisible(false);
    connect(m_keepTeammateCheck, &QCheckBox::toggled, this, &RollbackLobbyDialog::onKeepTeammateToggled);

    actionRow->addWidget(m_startBtn, 1);
    actionRow->addStretch(0);
    actionRow->addWidget(m_keepTeammateCheck);
    actionRow->addWidget(m_stopSessionBtn);
    actionRow->addWidget(m_dropBtn);
    actionRow->addWidget(m_leaveBtn);
    lay->addLayout(actionRow);

    return page;
}

// ── Seat row ─────────────────────────────────────────────────────────

void RollbackLobbyDialog::buildSeatRow(SeatRow& s, int slotIdx, QWidget* parent)
{
    s.slot = slotIdx;
    // A QFrame (not bare QWidget) so the per-player card background / border
    // set via setStyleSheet in the render helpers paints reliably.
    auto* card = new QFrame(parent);
    card->setObjectName("SeatCard");
    s.row = card;
    auto* lay = new QHBoxLayout(s.row);
    lay->setContentsMargins(MARGIN_GROUP, SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT);
    lay->setSpacing(SPACING_DEFAULT);

    // Drag grip — only shown to the host while the room is waiting (toggled in
    // onRoomStateChanged). Dragging it onto another seat swaps the two players.
    // A hidden widget reclaims its layout space, so non-host seats stay flush.
    s.dragHandle = new QLabel(QStringLiteral("⋮⋮"), s.row);
    s.dragHandle->setObjectName("SeatDragHandle");
    s.dragHandle->setFixedWidth(14);
    s.dragHandle->setAlignment(Qt::AlignCenter);
    s.dragHandle->setForegroundRole(QPalette::PlaceholderText); // subtle grip
    s.dragHandle->setCursor(Qt::OpenHandCursor);
    s.dragHandle->setToolTip(tr("Drag onto another seat to swap players"));
    s.dragHandle->setVisible(false);
    s.dragHandle->installEventFilter(this);
    lay->addWidget(s.dragHandle);

    // Filled/empty dot — single glyph, tinted with the player's accent when
    // filled (set in renderSeatFilled), muted grey when empty.
    s.dotLabel = new QLabel(QStringLiteral("○"), s.row);
    s.dotLabel->setFixedWidth(14);
    lay->addWidget(s.dotLabel);

    // Slot tag — a colored "P1" chip; shape + color applied per-player in the
    // render helpers. Fixed min size so names line up across rows.
    s.slotLabel = new QLabel(QString("P%1").arg(slotIdx), s.row);
    s.slotLabel->setObjectName("SeatChip");
    s.slotLabel->setAlignment(Qt::AlignCenter);
    s.slotLabel->setMinimumWidth(30);
    lay->addWidget(s.slotLabel);

    s.nameLabel = new QLabel(tr("Waiting…"), s.row);
    s.nameLabel->installEventFilter(this); // hover card
    lay->addWidget(s.nameLabel);

    s.countryLabel = new QLabel(s.row);
    s.countryLabel->setFixedSize(20, 14);
    s.countryLabel->setAlignment(Qt::AlignCenter);
    s.countryLabel->setVisible(false);
    lay->addWidget(s.countryLabel);

    // A member seated beside a running match they aren't in yet.
    s.statusLabel = new QLabel(s.row);
    s.statusLabel->setVisible(false);
    lay->addWidget(s.statusLabel);

    lay->addStretch(1);

    // Right-aligned meta (HOST badge · ping). Rich text lets each value carry
    // its own color; the connection symbol immediately after it is a separate
    // widget so it remains crisp and aligned for every status string.
    s.metaLabel = new QLabel(QString(), s.row);
    s.metaLabel->setTextFormat(Qt::RichText);
    s.metaLabel->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(s.metaLabel);

    s.connectionTypeLabel = new ConnectionTypeLabel(s.row);
    s.connectionTypeLabel->setFixedSize(24, 20);
    s.connectionTypeLabel->setVisible(false);
    lay->addWidget(s.connectionTypeLabel);

    // Host-only kick button. Hidden by default; shown on non-host seats only
    // when the local user is the host (set in renderSeatFilled).
    s.kickButton = new QPushButton(QStringLiteral("✕"), s.row);
    s.kickButton->setObjectName("SeatKickButton");
    s.kickButton->setFixedSize(20, 20);
    s.kickButton->setCursor(Qt::PointingHandCursor);
    s.kickButton->setFocusPolicy(Qt::NoFocus);
    s.kickButton->setToolTip(tr("Remove this player from the match"));
    s.kickButton->setVisible(false);
    connect(s.kickButton, &QPushButton::clicked, this, [this, &s]() {
        if (!m_client || s.userId == 0)
            return;
        const QString name = s.nameLabel ? s.nameLabel->text() : QString();
        if (QMessageBox::question(this, tr("Remove player"),
                QString(tr("Remove %1 from the match?")).arg(name.isEmpty() ? tr("this player") : name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        m_client->kickFromRoom(s.userId);
    });
    lay->addWidget(s.kickButton);

    s.admitButton = new QPushButton(tr("Add to Game"), s.row);
    s.admitButton->setObjectName("SeatAdmitButton");
    s.admitButton->setCursor(Qt::PointingHandCursor);
    s.admitButton->setFocusPolicy(Qt::NoFocus);
    s.admitButton->setVisible(false);
    connect(s.admitButton, &QPushButton::clicked, this, [this, &s]() {
        if (s.userId != 0)
            onAdmitClicked(s.userId);
    });
    lay->insertWidget(lay->indexOf(s.kickButton), s.admitButton);
}

void RollbackLobbyDialog::renderSeatEmpty(SeatRow& s)
{
    s.isHost = false;
    s.userId = 0;
    s.frameDelay = -1;
    s.iceAttempt = 1;
    s.iceMaxAttempts = 20;

    // Muted, dashed card — clearly "open seat" without competing with the
    // filled, color-tinted player cards.
    if (s.row)
        s.row->setStyleSheet(
            "QFrame#SeatCard {"
            "  background-color: transparent;"
            "  border: 1px dashed palette(mid);"
            "  border-radius: 8px;"
            "}");
    if (s.dotLabel)
    {
        s.dotLabel->setText(QStringLiteral("○"));
        s.dotLabel->setStyleSheet(QString("color: %1;").arg(statusColors().idle));
    }
    if (s.slotLabel)
        s.slotLabel->setStyleSheet(
            QString("QLabel#SeatChip {"
                    "  background-color: rgba(128,128,128,0.18);"
                    "  color: %1;"
                    "  border-radius: 7px;"
                    "  padding: 1px 7px;"
                    "  font-weight: 700;"
                    "}").arg(statusColors().idle));
    if (s.nameLabel)
    {
        s.nameLabel->setText(tr("Waiting…"));
        s.nameLabel->setStyleSheet(QString());
        s.nameLabel->setForegroundRole(QPalette::PlaceholderText);
        QFont f = s.nameLabel->font();
        f.setBold(false);
        s.nameLabel->setFont(f);
    }
    if (s.countryLabel) s.countryLabel->setVisible(false);
    if (s.statusLabel) s.statusLabel->setVisible(false);
    if (s.metaLabel) s.metaLabel->setText(QString());
    if (s.connectionTypeLabel) s.connectionTypeLabel->setVisible(false);
    if (s.kickButton)
    {
        QSizePolicy policy = s.kickButton->sizePolicy();
        policy.setRetainSizeWhenHidden(false);
        s.kickButton->setSizePolicy(policy);
        s.kickButton->setVisible(false);
    }
    if (s.admitButton) s.admitButton->setVisible(false);
}

void RollbackLobbyDialog::renderSeatFilled(SeatRow& s, const QString& username, bool isHost,
                                           bool isSelf, int pingMs, int frameDelay, bool canKick,
                                           const QString& connectionType)
{
    s.isHost = isHost;
    s.frameDelay = frameDelay;
    if (s.kickButton)
    {
        // When the local user is host, every occupied seat reserves the same
        // fixed kick-button slot. The host's own hidden X therefore takes the
        // same layout width as each visible X, keeping all connection icons on
        // one vertical line. Non-hosts do not retain the unused space.
        const bool localUserIsHost = m_client &&
            m_currentRoomHostId == m_client->selfUserId();
        QSizePolicy policy = s.kickButton->sizePolicy();
        policy.setRetainSizeWhenHidden(localUserIsHost);
        s.kickButton->setSizePolicy(policy);
        s.kickButton->setVisible(canKick);
    }

    const bool dark = isDarkTheme();
    const QString accent = playerAccentHex(s.slot, dark);

    // Player-colored, gently tinted card. Self stands out with a more vivid
    // (fully-opaque) accent border in the same color — kept at 1px because a
    // 2px rounded border renders with uneven/notchy corners.
    if (s.row)
        s.row->setStyleSheet(QString(
            "QFrame#SeatCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}")
            .arg(playerCardBg(s.slot, dark),
                 isSelf ? accent : playerBorderRgba(s.slot, dark)));

    if (s.dotLabel)
    {
        s.dotLabel->setText(QStringLiteral("●"));
        s.dotLabel->setStyleSheet(QString("color: %1;").arg(accent));
    }
    if (s.slotLabel)
        s.slotLabel->setStyleSheet(QString(
            "QLabel#SeatChip {"
            "  background-color: %1;"
            "  color: white;"
            "  border-radius: 7px;"
            "  padding: 1px 7px;"
            "  font-weight: 700;"
            "}").arg(accent));
    if (s.nameLabel)
    {
        QString name = username;
        if (isSelf) name += tr("  (you)");
        s.nameLabel->setText(name);
        s.nameLabel->setStyleSheet(QString());
        s.nameLabel->setForegroundRole(QPalette::WindowText);
        QFont f = s.nameLabel->font();
        f.setBold(isSelf);
        s.nameLabel->setFont(f);
    }
    if (s.metaLabel)
    {
        // Self is a local zero-latency endpoint. Remote ping appears after the
        // first reply; until then onRoomStateChanged supplies an ICE status.
        s.metaLabel->setText(
            seatMetaHtml(s.slot, isHost, isSelf ? 0 : pingMs, frameDelay, dark));
    }
    if (s.connectionTypeLabel)
    {
        setConnectionTypeLabel(s.connectionTypeLabel, connectionType);
        s.connectionTypeLabel->setVisible(true);
    }
}

// ── Chat column ──────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildChatColumn()
{
    // Lobby chat — room chat lives below the seats in the in-room view.
    // A bold header labels the column and the chat sits in the same rounded
    // card as the players list. While watching a live replay a tab bar adds a
    // Spectators tab: chat among that match's viewers, which its players never
    // see.
    auto* col = new QWidget(this);
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER);
    lay->setSpacing(SPACING_DEFAULT);

    m_chatHeader = new QLabel(tr("LOBBY CHAT"), this);
    m_chatHeader->setProperty("class", "SectionHeader");
    lay->addWidget(m_chatHeader);

    auto* card = new QFrame(col);
    card->setObjectName("LobbyCard");
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT);
    cardLay->setSpacing(SPACING_DEFAULT);

    m_chatTabs = new QTabBar(card);
    m_chatTabs->setObjectName("ChatTabs");
    m_chatTabs->setDocumentMode(true);
    m_chatTabs->setDrawBase(false);
    m_chatTabs->setExpanding(false);
    m_chatTabs->addTab(tr("Lobby"));
    m_chatTabs->addTab(tr("Spectators"));
    m_chatTabs->setTabToolTip(kChatTabLobby, tr("Everyone in the lobby."));
    m_chatTabs->setTabToolTip(kChatTabSpectators,
        tr("Everyone watching this match. The players can't see it."));
    m_chatTabs->setVisible(false);
    cardLay->addWidget(m_chatTabs);

    auto makeChatView = [](QWidget* parent) {
        auto* view = new QTextEdit(parent);
        view->setReadOnly(true);
        view->setLineWrapMode(QTextEdit::WidgetWidth);
        view->setFrameShape(QFrame::NoFrame);
        keepChatAtBottom(view);
        return view;
    };
    m_chatStack = new QStackedWidget(card);
    m_chatViewLobby = makeChatView(m_chatStack);
    m_chatViewSpectate = makeChatView(m_chatStack);
    m_chatStack->insertWidget(kChatTabLobby, m_chatViewLobby);
    m_chatStack->insertWidget(kChatTabSpectators, m_chatViewSpectate);
    cardLay->addWidget(m_chatStack, 1);

    connect(m_chatTabs, &QTabBar::currentChanged, this, [this](int index) {
        m_chatStack->setCurrentIndex(index);
        if (index == kChatTabSpectators)
        {
            m_chatTabs->setTabText(kChatTabSpectators, tr("Spectators")); // clear the unread mark
            m_chatInput->setPlaceholderText(tr("Message the other spectators…"));
        }
        else
        {
            m_chatInput->setPlaceholderText(tr("Message the lobby…"));
        }
    });

    m_chatInput = new QLineEdit(card);
    m_chatInput->setPlaceholderText(tr("Message the lobby…"));
    m_chatInput->setEnabled(false);
    m_chatInput->setMaxLength(500); // server truncates at 500; cap client-side too
    m_chatInput->setMinimumHeight(BUTTON_MIN_HEIGHT);
    connect(m_chatInput, &QLineEdit::returnPressed, this, &RollbackLobbyDialog::onChatSendClicked);
    cardLay->addWidget(m_chatInput);

    lay->addWidget(card, 1);

    return col;
}

// ── Players column ───────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildPlayersColumn()
{
    // The tree's column header ("Player / State / Ping / Type") already labels the
    // content; a borderless rounded card frame gives it the polished, P2P-
    // style container without a heavy titled group box.
    auto* col = new QWidget(this);
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER);
    lay->setSpacing(SPACING_DEFAULT);

    auto* card = new QFrame(col);
    card->setObjectName("LobbyCard");
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(SPACING_TIGHT, SPACING_TIGHT, SPACING_TIGHT, SPACING_TIGHT);

    m_playersTree = new QTreeWidget(card);
    m_playersTree->setObjectName("PlayersTree");
    m_playersTree->setHeaderLabels({ tr("Player"), tr("State"), tr("Ping"), tr("Type") });
    m_playersTree->setRootIsDecorated(false);
    m_playersTree->setSortingEnabled(true);
	m_playersTree->sortItems(0, Qt::AscendingOrder);
    m_playersTree->setAlternatingRowColors(true);
    m_playersTree->setFrameShape(QFrame::NoFrame);
    m_playersTree->setUniformRowHeights(true);
    // Right-edge flags in the name column + painted Ping and Type icons.
    m_playersTree->setItemDelegate(new PlayersItemDelegate(m_playersTree));
    // Columns are clamped to the viewport (clampTreeColumns, shared with the
    // browse trees) rather than using a Qt Stretch section: a Stretch section
    // locks its own divider, whereas the clamp keeps every divider live —
    // each trades with the column on its right, so even the last column is
    // resizable via the divider on its left — while the header always fills
    // the card.
    m_playersTree->header()->setStretchLastSection(false);
    m_playersTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    {
        // Size State, Ping, and Type to their widest expected content once, up
        // front: the tree is empty at construction, so ResizeToContents here
        // would measure the header labels alone and clip "Spectating" later.
        const QFontMetrics fm(m_playersTree->header()->font());
        const int pad = 20; // section margins
        const int stateWidth  = fm.horizontalAdvance(QStringLiteral("Spectating")) + pad;
        // Wide enough for the painted bars and header label. The sort indicator
        // renders above the label in this header style, so it needs no extra room.
        const int pingWidth = std::max(fm.horizontalAdvance(tr("Ping")) + pad - 4, 40);
        const int typeWidth = std::max(fm.horizontalAdvance(tr("Type")) + pad - 4, 44);
        m_playersTree->setColumnWidth(1, stateWidth);
        m_playersTree->setColumnWidth(2, pingWidth);
        m_playersTree->setColumnWidth(3, typeWidth);

        // The column count is part of the key: restoring a state saved for a
        // different column layout half-applies and breaks the header (missing
        // section dividers, wrong stretch section). v4 adds the compact Type
        // column and keeps the Ping column solely for its signal bars, so old
        // three-column states must remain orphaned.
        QSettings s("RMG-K", "n02");
        const QString key = QString("RollbackLobby/PlayersHeaderState.v4.c%1").arg(m_playersTree->columnCount());
        const QByteArray headerState = s.value(key).toByteArray();
        if (!headerState.isEmpty())
            m_playersTree->header()->restoreState(headerState);
        // restoreState can resurrect per-section modes (older saves have Ping
        // pinned Fixed); re-pin ours AFTER it. The computed widths above are
        // only first-run defaults — saved widths win, and Player is refit by
        // the first viewport Resize regardless.
        m_playersTree->header()->setStretchLastSection(false);
        m_playersTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    }
    // The columns always span the viewport exactly, so a horizontal scrollbar
    // can never be needed — turning it off also stops Qt reserving space for
    // one mid-drag and reflowing the row underneath.
    m_playersTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_playersTree->header(), &QHeaderView::sectionResized, this,
            [this](int index, int, int) { clampTreeColumns(m_playersTree, index); });
    // Selecting a player measures them on demand and refreshes their tooltip.
    connect(m_playersTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item)
                    probeOnDemand(item->data(0, Qt::UserRole).toULongLong());
            });
    // Click in the empty area below the rows to clear the selection, and catch
    // viewport resizes to pull the columns back in (both in eventFilter —
    // there's no built-in property for either).
    m_playersTree->viewport()->installEventFilter(this);
    cardLay->addWidget(m_playersTree);
    lay->addWidget(card);

    return col;
}

// ── QSS (minimal, palette-based) ─────────────────────────────────────

void RollbackLobbyDialog::applyStylesheet()
{
    // Adapts to the system theme via palette() functions. Only the primary
    // CTA buttons (Quick Match / Start Game) are explicitly colored, matching
    // the Kaillera launcher's KailleraPrimaryButton (#0078D7 Windows blue).
    const QPalette pal = QApplication::palette();
    const bool dark = pal.window().color().value() < 128;
    const QString border = dark ? QStringLiteral("rgba(255,255,255,0.18)")
                                : QStringLiteral("palette(mid)");

    // Disabled controls need a *readable* grey on dark themes — palette(mid) is
    // near-invisible against the dark base (you can't see a non-host's combos or
    // the "No ROMs" picker). Light mode keeps palette(mid), which already reads.
    const QString disabledText = dark ? QStringLiteral("rgba(255,255,255,0.45)")
                                      : QStringLiteral("palette(mid)");

    // Selected-row highlight for the lobby tables. Styling ::item makes Qt fall
    // back to a near-white selection that's unreadable on dark themes, so set an
    // explicit blue wash + readable text for each theme.
    const QString selBg   = dark ? QStringLiteral("rgba(0,120,215,0.55)")
                                 : QStringLiteral("rgba(0,120,215,0.22)");
    const QString selText = dark ? QStringLiteral("#ffffff")
                                 : QStringLiteral("palette(text)");

    // Banner accent — soft blue tint that reads on both light and dark themes.
    const QString bannerBg = dark
        ? QStringLiteral("rgba(0, 120, 215, 0.18)")
        : QStringLiteral("rgba(0, 120, 215, 0.10)");
    const QString bannerBorder = QStringLiteral("rgba(0, 120, 215, 0.45)");

    // Brand accent for the header text, and a faint accent wash for the marquee
    // strip so it reads as a header band rather than blank window. Blue is the
    // dialog's own identity, independent of the seat palette — it must not read
    // as the caution red that marks destructive actions below.
    const QString brandAccent = dark ? QStringLiteral("#4aa3ff") : QStringLiteral("#0078D7");
    const QString marqueeBg   = tintRgba(brandAccent, dark ? 0.16 : 0.07);

    // Themed chevron for the styled combos (same asset the P2P dialog uses), so
    // the drop-down arrow doesn't fall back to Qt's default that clashes with
    // the rounded border.
    const QString comboArrow = QString(":/icons/%1/svg/arrow-down-s-line.svg")
        .arg(dark ? "white" : "black");

    // Drop Game is destructive (it ends the match), so it gets a caution-red
    // secondary treatment — same size as the others, set apart by color, not
    // size. Leave Room stays neutral; Start Game is the filled-blue primary.
    const QString cautionRed  = dark ? QStringLiteral("#ef6b68") : QStringLiteral("#c0392b");
    const QString cautionSoft = tintRgba(cautionRed, dark ? 0.18 : 0.10);

    QString qss = QString(
        // Marquee — faint accent header band with a hairline divider.
        "QFrame#LobbyMarquee {"
        "  background-color: %5;"
        "  border-bottom: 1px solid %1;"
        "}"
        "QLabel#LobbyBrand { color: %4; }"

        // Rounded content cards (players list, chat) — the P2P-style container
        // without a heavy titled group box.
        "QFrame#LobbyCard {"
        "  border: 1px solid %1;"
        "  border-radius: 10px;"
        "  background-color: palette(base);"
        "}"

        // Active Rooms / Ongoing Matches — styled as tables: square 1px outline,
        // a distinct header row, and light column / row separators.
        "QTreeWidget#RoomsTree, QTreeWidget#MatchesTree {"
        "  border: 1px solid %1;"
        "  background-color: palette(base);"
        "}"
        "QTreeWidget#RoomsTree::item, QTreeWidget#MatchesTree::item {"
        "  padding: 3px 4px;"
        "  border-bottom: 1px solid rgba(127, 127, 127, 0.12);"
        "}"
        "QTreeWidget#RoomsTree QHeaderView::section,"
        " QTreeWidget#MatchesTree QHeaderView::section {"
        "  background-color: rgba(127, 127, 127, 0.12);"
        "  color: palette(text);"
        "  border: none;"
        "  border-right: 1px solid %1;"
        "  border-bottom: 1px solid %1;"
        "  padding: 4px 6px;"
        "  font-weight: 600;"
        "}"
        "QTreeWidget#RoomsTree QHeaderView::section:last,"
        " QTreeWidget#MatchesTree QHeaderView::section:last {"
        "  border-right: none;"
        "}"

        // Section headers — bold uppercase label with a hairline divider
        // below. Replaces the QGroupBox wrappers we used to have around
        // every region; reads as a flatter, list-style hierarchy.
        "QLabel[class=\"SectionHeader\"] {"
        "  font-weight: 700;"
        "  color: palette(text);"
        "  padding-top: 6px;"
        "  padding-bottom: 4px;"
        "  border-bottom: 1px solid %1;"
        "}"

        // In-room banner — visible in browse view when the user is seated.
        // Kept framed since it's a CTA accent, not a content region.
        "QFrame#InRoomBanner {"
        "  background-color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 6px;"
        "}"
        "QFrame#InRoomBanner QLabel {"
        "  color: palette(text);"
        "}"

        // Primary CTAs — same blue as KailleraPrimaryButton.
        "QPushButton#QuickMatchBtn, QPushButton#StartGameBtn,"
        " QPushButton#BannerReturnBtn {"
        "  border: 1px solid #0066b4;"
        "  border-radius: 7px;"
        "  padding: 4px 16px;"
        "  font-weight: 700;"
        "  color: white;"
        "  background-color: #0078D7;"
        "}"
        "QPushButton#QuickMatchBtn:hover, QPushButton#StartGameBtn:hover,"
        " QPushButton#BannerReturnBtn:hover {"
        "  background-color: #1584dd;"
        "}"
        "QPushButton#QuickMatchBtn:pressed, QPushButton#StartGameBtn:pressed,"
        " QPushButton#BannerReturnBtn:pressed {"
        "  background-color: #0063b1;"
        "}"
        "QPushButton#QuickMatchBtn:disabled, QPushButton#StartGameBtn:disabled {"
        "  background-color: palette(button);"
        "  color: palette(mid);"
        "  border-color: %1;"
        "}"

        // Quick Ranked Match — gold, to read as ranked next to the blue CTA.
        "QPushButton#QuickRankedBtn {"
        "  border: 1px solid #a8750c;"
        "  border-radius: 7px;"
        "  padding: 4px 16px;"
        "  font-weight: 700;"
        "  color: white;"
        "  background-color: #c48a12;"
        "}"
        "QPushButton#QuickRankedBtn:hover { background-color: #d29a22; }"
        "QPushButton#QuickRankedBtn:pressed { background-color: #a8750c; }"
        "QPushButton#QuickRankedBtn:disabled {"
        "  background-color: palette(button);"
        "  color: palette(mid);"
        "  border-color: %1;"
        "}"

        // Secondary action buttons — Create Room / Drop / Leave read as a set,
        // matching the P2P hosting screen's secondary buttons.
        "QPushButton#CreateRoomBtn, QPushButton#DropBtn, QPushButton#LeaveBtn, QPushButton#StopSessionBtn {"
        "  border: 1px solid %1;"
        "  border-radius: 7px;"
        "  padding: 4px 14px;"
        "  font-weight: 600;"
        "  background-color: palette(button);"
        "}"
        "QPushButton#CreateRoomBtn:hover, QPushButton#DropBtn:hover,"
        " QPushButton#LeaveBtn:hover {"
        "  border-color: palette(dark);"
        "  background-color: palette(light);"
        "}"
        "QPushButton#CreateRoomBtn:pressed, QPushButton#DropBtn:pressed,"
        " QPushButton#LeaveBtn:pressed {"
        "  background-color: palette(midlight);"
        "}"
        "QPushButton#CreateRoomBtn:disabled, QPushButton#DropBtn:disabled {"
        "  color: palette(mid);"
        "}"

        // Drop Game — caution variant of the secondary style (destructive).
        "QPushButton#DropBtn {"
        "  color: %7;"
        "  border-color: %7;"
        "}"
        "QPushButton#DropBtn:hover {"
        "  color: %7;"
        "  border-color: %7;"
        "  background-color: %8;"
        "}"
        "QPushButton#DropBtn:pressed {"
        "  background-color: %8;"
        "}"
        "QPushButton#DropBtn:disabled {"
        "  color: palette(mid);"
        "  border-color: %1;"
        "}"

        // Session-setting combos — rounded, themed border (P2P style).
        "QComboBox#LobbyCombo, QComboBox#BrowseRomCombo {"
        "  border: 1px solid %1;"
        "  border-radius: 7px;"
        "  background-color: palette(base);"
        "  padding: 3px 8px;"
        "  min-height: 22px;"
        "}"
        // The browse combo is editable (searchable); strip the embedded line
        // edit's own frame/background so it sits flush inside the combo border.
        "QComboBox#BrowseRomCombo QLineEdit {"
        "  border: none;"
        "  background: transparent;"
        "  padding: 0px;"
        "  color: palette(text);"
        "  selection-background-color: palette(highlight);"
        "}"
        "QComboBox#LobbyCombo:focus, QComboBox#BrowseRomCombo:focus {"
        "  border-color: palette(highlight);"
        "}"
        "QComboBox#LobbyCombo:disabled, QComboBox#BrowseRomCombo:disabled {"
        "  color: %9;"
        "}"
        // Editable browse combo: the embedded line edit's own color rule would
        // otherwise win, so dim it explicitly when disabled too.
        "QComboBox#BrowseRomCombo QLineEdit:disabled {"
        "  color: %9;"
        "}"
        "QComboBox#LobbyCombo::drop-down, QComboBox#BrowseRomCombo::drop-down {"
        "  subcontrol-origin: padding;"
        "  subcontrol-position: top right;"
        "  width: 20px;"
        "  border: none;"
        "  border-left: 1px solid %1;"
        "  border-top-right-radius: 7px;"
        "  border-bottom-right-radius: 7px;"
        "  background-color: transparent;"
        "  margin: 1px;"
        "}"
        "QComboBox#LobbyCombo::down-arrow, QComboBox#BrowseRomCombo::down-arrow {"
        "  image: url(%6);"
        "  width: 12px;"
        "  height: 12px;"
        "}"

        // Seat kick button — red accent, transparent until hovered (P2P style).
        "QPushButton#SeatKickButton {"
        "  border: 1px solid transparent;"
        "  border-radius: 6px;"
        "  padding: 0px;"
        "  color: #c03a3a;"
        "  background-color: transparent;"
        "  font-weight: 800;"
        "}"
        "QPushButton#SeatKickButton:hover {"
        "  border-color: #d77a7a;"
        "  background-color: rgba(208, 72, 72, 0.12);"
        "}"
        "QPushButton#SeatKickButton:pressed {"
        "  background-color: rgba(208, 72, 72, 0.20);"
        "}"
    ).arg(border, bannerBg, bannerBorder, brandAccent, marqueeBg, comboArrow,
          cautionRed, cautionSoft, disabledText);

    // Appended separately: the block above already uses the 9-arg arg() limit.
    // A bare :selected covers both focused and unfocused selection, so the row
    // stays readable after focus moves (e.g. right after a double-click to join).
    qss += QString(
        "QTreeWidget#RoomsTree::item:selected,"
        " QTreeWidget#MatchesTree::item:selected,"
        " QTreeWidget#PlayersTree::item:selected {"
        "  background-color: %1;"
        "  color: %2;"
        "}"
    ).arg(selBg, selText);

    // our name in the banner: room for the account menu's arrow
    qss += QStringLiteral(
        "QToolButton#UserButton { padding: 3px 18px 3px 6px; }"
        "QToolButton#UserButton::menu-indicator { subcontrol-origin: padding;"
        "  subcontrol-position: right center; right: 4px; }");

    setStyleSheet(qss);
}

// ──────────────────────────────────────────────────────────────────────
//  ROM library
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::setRomLibrary(const QMap<QString, CoreRomSettings>& roms)
{
    m_roms = roms;
    populateBrowseRoms();
}

void RollbackLobbyDialog::populateBrowseRoms()
{
    if (!m_browseRomCombo) return;

    m_browseRomCombo->blockSignals(true);
    m_browseRomCombo->clear();

    if (m_roms.isEmpty())
    {
        m_browseRomCombo->addItem(tr("No ROMs in your library — add some first"));
        m_browseRomCombo->setEnabled(false);
        m_browseRomCombo->blockSignals(false);
        refreshSameGameFilter();
        updateHeroButtons();
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
    updateHeroButtons();
}

QVariantMap RollbackLobbyDialog::selectedBrowseRom() const
{
    if (!m_browseRomCombo || !m_browseRomCombo->isEnabled())
        return {};

    // An editable combo can retain the previous currentData while its line edit
    // contains different text. Only accept data for the text actually shown.
    const QString currentText = m_browseRomCombo->currentText();
    int idx = m_browseRomCombo->currentIndex();
    if (idx < 0 || m_browseRomCombo->itemText(idx) != currentText)
        idx = m_browseRomCombo->findText(currentText, Qt::MatchFixedString);
    return idx >= 0 ? m_browseRomCombo->itemData(idx).toMap() : QVariantMap{};
}

void RollbackLobbyDialog::refreshSameGameFilter()
{
    if (!m_sameGameFilterCheck)
        return;

    m_sameGameFilterCheck->setEnabled(!selectedBrowseRom().isEmpty());
    if (m_sameGameFilterCheck->isChecked() && m_roomsTree && m_matchesTree)
        onRoomListChanged();
}

// ──────────────────────────────────────────────────────────────────────
//  Show / close
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!event->spontaneous())
        m_useSavedLogin = true;
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
    // Hover cards: a player's ranked standings under their usual tooltip.
    if (event->type() == QEvent::ToolTip && m_client)
    {
        auto* help = static_cast<QHelpEvent*>(event);
        if (m_playersTree && watched == m_playersTree->viewport())
        {
            QTreeWidgetItem* item = m_playersTree->itemAt(help->pos());
            const int column = m_playersTree->columnAt(help->pos().x());
            if (item && (column == 0 || column == 1))
            {
                const quint64 id = item->data(0, Qt::UserRole).toULongLong();
                const auto it = m_client->users().constFind(id);
                if (it != m_client->users().constEnd())
                {
                    // the State column's own tip (what they search for or watch) leads
                    const QString stateTip = column == 1 ? item->toolTip(1) : QString();
                    const QString base = stateTip.isEmpty() ? item->toolTip(0)
                                                            : stateTip + QStringLiteral("\n\n") + item->toolTip(0);
                    showRatingsTip(it->verified ? it->username : QString(), base,
                                   help->globalPos(), m_playersTree->viewport());
                    return true;
                }
            }
        }
        if (watched == m_userButton)
        {
            showRatingsTip(m_client->isVerified() ? m_username : QString(),
                           m_client->isVerified() ? QString() : tr("Guest: log in to an account to play ranked."),
                           help->globalPos(), m_userButton);
            return true;
        }
        for (const SeatRow& seat : m_seats)
        {
            if (seat.nameLabel && watched == seat.nameLabel && seat.userId != 0)
            {
                const auto it = m_client->users().constFind(seat.userId);
                if (it != m_client->users().constEnd())
                {
                    showRatingsTip(it->verified ? it->username : QString(), seat.nameLabel->toolTip(),
                                   help->globalPos(), seat.nameLabel);
                    return true;
                }
            }
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
        // Narrowing the dialog shrinks the viewport out from under columns that
        // were already sized to it, which is the one way they'd overflow without
        // anyone touching a divider.
        else if (event->type() == QEvent::Resize)
        {
            clampTreeColumns(m_playersTree, -1);
        }
        return QDialog::eventFilter(watched, event);
    }

    // The browse trees are clamped the same way: a viewport resize (window
    // resize, vertical scrollbar appearing) is the one way their columns can
    // overflow without anyone touching a divider.
    if (event->type() == QEvent::Resize)
    {
        if (m_roomsTree && watched == m_roomsTree->viewport())
        {
            clampTreeColumns(m_roomsTree, -1);
            return QDialog::eventFilter(watched, event);
        }
        if (m_matchesTree && watched == m_matchesTree->viewport())
        {
            clampTreeColumns(m_matchesTree, -1);
            return QDialog::eventFilter(watched, event);
        }
    }

    // ── Seat reorder: start a drag from a seat's grip handle ──
    if (m_canReorderSeats)
    {
        for (auto& s : m_seats)
        {
            if (!s.dragHandle || watched != s.dragHandle)
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
                    startSeatDrag(slot, s.row);
                }
                return true;
            }
        }
    }

    // ── Seat reorder: accept the drop and ask the server to swap ──
    if (m_seatsBox && watched == m_seatsBox)
    {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)
        {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData() && de->mimeData()->hasFormat(kSeatMime))
            {
                de->acceptProposedAction();
                return true;
            }
        }
        else if (event->type() == QEvent::Drop)
        {
            auto* de = static_cast<QDropEvent*>(event);
            if (de->mimeData() && de->mimeData()->hasFormat(kSeatMime))
            {
                const int from = de->mimeData()->data(kSeatMime).toInt();
                const int to   = seatSlotAtPos(de->position().toPoint());
                if (m_client && m_canReorderSeats && from >= 1 && to >= 1 && from != to)
                    m_client->swapSeats(from, to);
                de->acceptProposedAction();
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, event);
}

void RollbackLobbyDialog::startSeatDrag(int slot, QWidget* card)
{
    if (slot < 1 || !card)
        return;
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData();
    mime->setData(kSeatMime, QByteArray::number(slot));
    drag->setMimeData(mime);
    // Carry a snapshot of the seat card under the cursor for feedback.
    const QPixmap pm = card->grab();
    if (!pm.isNull())
    {
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 8, pm.height() / 2));
    }
    drag->exec(Qt::MoveAction);
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

void RollbackLobbyDialog::clampTreeColumns(QTreeWidget* tree, int resizedIndex)
{
    // Shared column clamp for the players, rooms, and matches trees.
    // Re-entrancy: every setColumnWidth below emits sectionResized, which is
    // the signal that called us.
    if (tree == nullptr || m_clampingTreeColumns)
        return;

    const int count     = tree->columnCount();
    const int available = tree->viewport()->width();
    // Width is 0 until the dialog is first laid out; the viewport Resize that
    // follows re-runs this with a real number.
    if (count < 2 || available <= 0)
        return;

    // Column 0 is the text-heavy fill in every tree; the others only need to
    // keep a squeezed divider grabbable.
    const auto minFor = [](int index) { return index == 0 ? 80 : 36; };

    std::vector<int> widths(count);
    for (int i = 0; i < count; ++i)
        widths[i] = tree->columnWidth(i);
    // Floor everything first: Qt's own minimum section size is font-derived
    // and a drag can push a column below ours.
    for (int i = 0; i < count; ++i)
        widths[i] = std::max(widths[i], minFor(i));

    // Qt resizes a column via the divider on its RIGHT edge, so giving the
    // change to the next column keeps the boundary where the user put it and
    // makes every column resizable — the last one via the divider on its
    // left. Viewport resizes (index -1) — and direct resizes of the last
    // column, whose own right edge is the viewport border — are absorbed by
    // the fill column instead.
    int absorber = (resizedIndex >= 0 && resizedIndex < count - 1)
        ? resizedIndex + 1 : 0;
    // Match stats stay compact: trade spare space between Players and ROM
    // instead of expanding Duration when the ROM column is narrowed.
    if (tree == m_matchesTree)
        absorber = resizedIndex == 0 ? 1 : 0;

    const auto fitToViewport = [&](int target) {
        int others = 0;
        for (int i = 0; i < count; ++i)
        {
            if (i != target)
                others += widths[i];
        }
        widths[target] = available - others;
    };

    fitToViewport(absorber);
    if (widths[absorber] < minFor(absorber))
    {
        // The drag squeezed the absorber to its floor: pin it there and push
        // the shortfall back into the dragged column, then — if that bottoms
        // out too — into the remaining columns, right to left.
        widths[absorber] = minFor(absorber);
        int deficit = -available;
        for (int i = 0; i < count; ++i)
            deficit += widths[i];

        const int dragged = (resizedIndex >= 0 && resizedIndex < count &&
                             resizedIndex != absorber) ? resizedIndex : -1;
        std::vector<int> order;
        if (tree == m_matchesTree)
            order.push_back(absorber == 0 ? 1 : 0);
        if (dragged >= 0)
        {
            if (std::find(order.begin(), order.end(), dragged) == order.end())
                order.push_back(dragged);
        }
        for (int i = count - 1; i >= 0; --i)
        {
            if (i != absorber && std::find(order.begin(), order.end(), i) == order.end())
                order.push_back(i);
        }
        for (int i : order)
        {
            if (deficit <= 0)
                break;
            const int give = std::min(deficit, widths[i] - minFor(i));
            if (give > 0)
            {
                widths[i] -= give;
                deficit   -= give;
            }
        }
    }

    m_clampingTreeColumns = true;
    for (int i = 0; i < count; ++i)
    {
        if (widths[i] != tree->columnWidth(i))
            tree->setColumnWidth(i, widths[i]);
    }
    m_clampingTreeColumns = false;
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

    name.remove(QRegularExpression(R"([^A-Za-z0-9_\-\.])"));
    return name.left(20);
}

void RollbackLobbyDialog::promptForUsername(const QString& statusMessage)
{
    if (m_connectPromptOpen || !isVisible() || matchTransportInProgress() ||
        m_client->state() == LobbyClient::ConnectionState::Connected)
        return;

    // Already logged in: connect with the saved login instead of asking. If
    // it's rejected (expired) or the lobby can't be reached, the prompt
    // opens with the reason.
    const bool useSavedLogin = m_useSavedLogin;
    m_useSavedLogin = false;
    const QString savedUser = LobbyConnectDialog::savedLoginUser();
    if (useSavedLogin && statusMessage.isEmpty() && m_discordLinkTicket.isEmpty() && !savedUser.isEmpty())
    {
        m_username      = savedUser;
        m_serverUrl     = LobbyConnectDialog::defaultServerUrl();
        m_rememberLogin = true;
        LobbyClient::LobbyAuth auth;
        auth.mode  = "token";
        auth.token = LobbyConnectDialog::savedSessionToken(savedUser);
        connectToLobby(auth);
        return;
    }

    m_connectPromptOpen = true;
    LobbyConnectDialog prompt(this);
    // after a failed attempt, reopen where it was made (Create account, reset...)
    if (!statusMessage.isEmpty() && m_authAttempted)
        prompt.setMode(LobbyConnectDialog::modeForAuth(m_lastAuthMode));
    prompt.setUsername(prefillUsername());
    prompt.setStatusMessage(statusMessage);
    if (!m_discordLinkTicket.isEmpty())
        prompt.setDiscordLink(m_discordLinkTicket, m_discordLinkName);
    const QString linkName = m_discordLinkName;
    m_discordLinkTicket.clear();
    m_discordLinkName.clear();
    const int result = prompt.exec();
    m_connectPromptOpen = false;

    if (result != QDialog::Accepted)
    {
        close();
        return;
    }

    m_username  = prompt.username();
    m_serverUrl = prompt.serverUrl();
    m_rememberLogin = prompt.rememberLogin();
    connectToLobby(prompt.auth(), linkName);
}

void RollbackLobbyDialog::connectToLobby(const LobbyClient::LobbyAuth& auth, const QString& discordLinkName)
{
    m_lastAuthMode = auth.mode;
    m_authAttempted = true;
    m_welcomeLine.clear();
    m_linkRetryTicket = auth.ticket;
    m_linkRetryName   = auth.ticket.isEmpty() ? QString() : discordLinkName;
    if (auth.mode == "register")
        m_welcomeLine = tr("Account created. Welcome, %1! You can play ranked now.").arg(m_username);
    else if (auth.mode == "reset")
        m_welcomeLine = tr("Password reset. You're logged in, and other saved logins were signed out.");
    else if (auth.mode == "discord_link")
        m_welcomeLine = tr("Discord linked: next time, Log in with Discord is enough.");
    else if (auth.mode == "discord_register")
        m_welcomeLine = tr("Account created and Discord linked. Welcome, %1! Next time, Log in with Discord is enough.")
                            .arg(m_username);
    if (m_userButton)
        m_userButton->setText(tr("User: %1").arg(m_username));

    // A new WebSocket session has no claim on a room from the previous server
    // session. This is normally already cleared by the disconnect handler, but
    // doing it here as well makes every reconnect start from a clean lobby.
    clearServerRoomSnapshot();
    updateServerMeta();
    m_client->connectToServer(m_serverUrl, m_username, {}, QString(), auth);
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

    if (m_roomChatInput)
        m_roomChatInput->setEnabled(false);
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
    m_chatInput->setEnabled(connected);
    updateHeroButtons();

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
        m_playersTree->clear();
        m_roomsTree->clear();
        m_matchesTree->clear();
        m_userItems.clear();
        m_roomItems.clear();
        resetQuickMatchButtons();
        closeRematchPrompt();
        m_rankedSession = false;
        m_casualSession = false;
        updateHeroButtons();
        updateSessionUi();

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
    updateInRoomBanner();

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
    if (reason == "username_taken")    human = tr("That username is already in use.");
    else if (reason == "invalid_hello") human = tr("Server rejected the connection handshake.");
    else if (reason == "invalid_payload") human = tr("That username isn't allowed.");
    else if (reason == "version_mismatch") human =
        tr("Your RMG-K client is out of date. This lobby requires RMG-K 0.9.13 "
        "or vdev-2367 or newer.");
    else if (reason == "server_full") human = tr("The lobby is currently full.");

    m_welcomeLine.clear();
    const QString failReason = m_client->lastHelloFailReason();
    // A rejected remember-me token has expired or was revoked; forget it so
    // the prompt asks for the password instead of retrying it forever.
    if (m_lastAuthMode == "token" && failReason == "auth_failed")
    {
        LobbyConnectDialog::clearSessionToken();
        human = tr("Your saved login expired. Enter your password to log in again.");
    }
    else if (m_lastAuthMode == "login" && failReason == "auth_failed")
    {
        human = tr("That username and password don't match. Check Caps Lock, or use Forgot password? "
                   "if you need a reset code.");
    }
    else if (m_lastAuthMode == "discord_link" && failReason == "auth_failed")
    {
        // the link form has no Forgot password? link
        human = tr("That username and password don't match. Check Caps Lock and try again.");
    }
    else if (failReason == "too_many_attempts" && m_client->lastHelloRetryAfter() > 0)
    {
        const int minutes = (m_client->lastHelloRetryAfter() + 59) / 60;
        human = minutes == 1 ? tr("Too many failed logins from your network. Try again in 1 minute.")
                             : tr("Too many failed logins from your network. Try again in %1 minutes.").arg(minutes);
    }

    // Discord sign-in found no linked account: the prompt asks for the
    // password once to link it.
    if (m_client->lastHelloFailReason() == "discord_unlinked")
    {
        m_discordLinkTicket = m_client->discordLinkTicket();
        m_discordLinkName   = m_client->discordLinkName();
        human.clear();
    }
    else if ((m_lastAuthMode == "discord_link" || m_lastAuthMode == "discord_register") &&
             failReason != "ticket_expired" && failReason != "discord_in_use")
    {
        // the ticket survives a wrong password or a taken name: reopen the
        // link form to try again
        m_discordLinkTicket = m_linkRetryTicket;
        m_discordLinkName   = m_linkRetryName;
    }
    else if (m_lastAuthMode == "discord" && m_client->lastHelloFailReason() == "discord_failed")
    {
        human = tr("Discord sign-in didn't work. Try again, or log in with your password.");
    }

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
    {
        m_statusLed->setStyleSheet(
            QString("background-color: %1; border-radius: %2px;")
                .arg(sc).arg(STATUS_DOT_PX / 2));
    }
    if (m_statusText)
    {
        m_statusText->setText(humanState(s));
        m_statusText->setStyleSheet(QString("color: %1; font-weight: 600;").arg(sc));
    }
}

void RollbackLobbyDialog::applyRoomStateBadge(const QString& text, const QString& colorHex)
{
    if (!m_roomStateLabel) return;
    m_roomStateLabel->setText(text);
    m_roomStateLabel->setStyleSheet(QString(
        "QLabel {"
        "  color: %1;"
        "  background-color: %2;"
        "  border-radius: 9px;"
        "  padding: 2px 10px;"
        "  font-weight: 700;"
        "}").arg(colorHex, tintRgba(colorHex, isDarkTheme() ? 0.22 : 0.14)));
}

void RollbackLobbyDialog::updateInRoomBanner()
{
    if (!m_inRoomBanner) return;
    const bool inRoom = (m_currentRoomId != 0);
    m_inRoomBanner->setVisible(inRoom);
    if (!inRoom) return;

    // Prefer the live room name from the rooms map; fall back to a neutral label.
    QString name;
    const auto& rooms = m_client->rooms();
    const auto it = rooms.constFind(m_currentRoomId);
    if (it != rooms.constEnd() && !it->name.isEmpty())
        name = it->name;
    else
        name = tr("Your room");

    // Also include seat count when we have it cached from ROOM_STATE.
    QString seats;
    if (it != rooms.constEnd() && it->maxPlayers > 0)
        seats = QString(tr("  ·  %1/%2 seats")).arg(it->players).arg(it->maxPlayers);

    if (m_bannerText)
        m_bannerText->setText(QString(tr("You're in: <b>%1</b>%2")).arg(name, seats));
}

void RollbackLobbyDialog::updateServerMeta()
{
    if (!m_serverMeta) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
    {
        m_serverMeta->setText(tr("Not connected"));
        return;
    }
    const int players = m_client->users().size();
    const int rooms = m_client->rooms().size();
    // Server host/IP intentionally omitted from the header — just show the
    // live population.
    const char* populationSource = "%1 player%3  ·  %2 room%4";
    const QString populationText = tr("%1 player%3  ·  %2 room%4");
    const bool translatedPopulation = populationText != QLatin1String(populationSource);
    m_serverMeta->setText(populationText.arg(players).arg(rooms)
                              .arg(translatedPopulation || players == 1 ? "" : "s")
                              .arg(translatedPopulation || rooms == 1 ? "" : "s"));
}

// ──────────────────────────────────────────────────────────────────────
//  Presence
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onPresenceFull()
{
    m_playersTree->clear();
    m_userItems.clear();
    for (auto it = m_client->users().constBegin(); it != m_client->users().constEnd(); ++it)
    {
        auto* row = new PlayerListItem(m_playersTree);
        refreshPlayerRow(row, it.value());
        m_userItems.insert(it.key(), row);
    }
    updateServerMeta();
}

void RollbackLobbyDialog::onUserAdded(quint64 userId)
{
    const auto& users = m_client->users();
    auto it = users.constFind(userId);
    if (it == users.constEnd()) return;

    auto* row = new PlayerListItem(m_playersTree);
    refreshPlayerRow(row, it.value());
    m_userItems.insert(userId, row);
    // A user can join already searching (quick-match from the launcher);
    // make sure the priority placement applies from their first paint.
    if (it.value().state == QLatin1String("searching"))
        m_playersTree->sortItems(m_playersTree->header()->sortIndicatorSection(),
                                 m_playersTree->header()->sortIndicatorOrder());
    updateServerMeta();
}

void RollbackLobbyDialog::onUserRemoved(quint64 userId)
{
    auto it = m_userItems.find(userId);
    if (it == m_userItems.end()) return;
    delete it.value();
    m_userItems.erase(it);
    updateServerMeta();
}

void RollbackLobbyDialog::onUserUpdated(quint64 userId)
{
    auto it = m_userItems.find(userId);
    if (it == m_userItems.end()) return;
    const auto& users = m_client->users();
    auto userIt = users.constFind(userId);
    if (userIt == users.constEnd()) return;
    const bool wasSearching = it.value()->data(0, kSearchingSortRole).toBool();
    refreshPlayerRow(it.value(), userIt.value());
    for (auto& seat : m_seats)
    {
        if (seat.userId == userId && seat.connectionTypeLabel)
        {
            setConnectionTypeLabel(seat.connectionTypeLabel, userIt->connection);
            setCountryFlagLabel(seat.countryLabel, userIt->country, userIt->anonymous);
            break;
        }
    }
    // Qt only re-sorts when data in the current sort column changes; a state
    // flip doesn't touch the Region column, so a viewer sorted by Region
    // would never see the row move. Force the pass on the transition.
    if (it.value()->data(0, kSearchingSortRole).toBool() != wasSearching)
        m_playersTree->sortItems(m_playersTree->header()->sortIndicatorSection(),
                                 m_playersTree->header()->sortIndicatorOrder());
}

// The match a spectator watches, named like the Ongoing Matches list does.
static QString watchingTip(const LobbyClient* client, quint64 matchId)
{
    for (const LobbyClient::LobbyRoomSummary& r : client->rooms())
    {
        if (matchId != 0 && r.matchId == matchId)
        {
            const QString match = r.playerNames.isEmpty() ? r.name : r.playerNames.join(" vs ");
            return r.romName.isEmpty() ? QString("Watching: %1").arg(match)
                                       : QString("Watching: %1 (%2)").arg(match, r.romName);
        }
    }
    return QStringLiteral("Watching a live replay");
}

void RollbackLobbyDialog::refreshPlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u)
{
    const bool dark = isDarkTheme();
    item->setText(0, u.username);
    // Self gets bold and the brand accent; everyone else uses default palette
    // text. Blue, not a seat color — this list is lobby-wide, not per-seat.
    if (u.id == m_client->selfUserId())
    {
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setForeground(0, QColor(dark ? QStringLiteral("#4aa3ff")
                                           : QStringLiteral("#0078D7")));
    }
    item->setData(0, kSearchingSortRole, u.state == QLatin1String("searching"));
    item->setText(1, stateGlyph(u.state));
    item->setForeground(1, QColor(stateHex(u.state, dark)));

    // While searching, hovering the state shows which ROM they're queued for;
    // while spectating, which match they're watching.
    if (u.state == "searching" && !u.searchingRom.isEmpty())
        item->setToolTip(1, QString(tr("Searching for: %1")).arg(u.searchingRom));
    else if (u.state == "spectating")
        item->setToolTip(1, watchingTip(m_client, u.watchingMatchId));
    else
        item->setToolTip(1, QString());

    // Country flag rides the right edge of the NAME cell (the delegate flips
    // column 0's decorationPosition), Fightcade-style. The server geolocates
    // the ISO country code; we render the bundled SVG. No flag for anonymous
    // users — and deliberately no region-badge fallback for them either,
    // that's the coarse location leak the option exists to prevent. Old
    // servers that send no country just show no flag.
    const QString flagResource = QString(":/flags/%1.svg").arg(u.country.toLower());
    QString countryName;
    if (!u.anonymous && !u.country.isEmpty() && QFile::exists(flagResource))
    {
        item->setIcon(0, QIcon(flagResource));
        const QLocale::Territory territory = QLocale::codeToTerritory(u.country);
        countryName = (territory != QLocale::AnyTerritory)
            ? QLocale::territoryToString(territory)
            : u.country;
    }
    else
    {
        item->setIcon(0, QIcon());
    }

    // Ping column: roles only — PlayersItemDelegate paints the signal bars.
    // A real measurement wins; the region estimate stands in translucently
    // until one lands; -1 draws the empty skeleton. Self shows no bars. The
    // Connection type gets its own compact icon column so it cannot obscure
    // the quality bars. Anonymity does not hide this match-quality signal.
    const bool isSelf = (u.id == m_client->selfUserId());
    const int measured = isSelf ? -1 : m_client->measuredPingMs(u.id);
    const int estimate = LobbyRegions::estimatedRttMs(m_client->selfRegion(), u.region);
    const int effective = isSelf ? -1
        : measured >= 0 ? measured
        : estimate > 0  ? estimate
                        : -1;
    item->setText(2, QString());
    item->setIcon(2, QIcon());
    item->setData(2, kShowPingRole, !isSelf);
    item->setData(2, kPingMsRole, effective);
    item->setData(2, kPingEstimateRole, measured < 0);
    // Built as lines so an absent field just doesn't contribute, rather than
    // leaving a stray separator. The region is deliberately not among them: it's
    // a coarse server-side bucket that can disagree with the geolocated country,
    // so the country is the only location we state.
    QStringList tipLines;
    if (!countryName.isEmpty())
        tipLines << QString(tr("Country: %1")).arg(countryName);
    // Self-reported transport medium ("wifi"/"lan"/...) and build, when the
    // peer's client and the server are new enough to relay them.
    const QString connLabel = connectionTypeName(u.connection);
    item->setText(3, connLabel);
    item->setData(3, kConnectionTypeRole, u.connection);
    if (!connLabel.isEmpty())
        tipLines << QString(tr("Connection: %1")).arg(connLabel);
    if (!u.clientVersion.isEmpty())
        tipLines << QString(tr("Build: %1")).arg(u.clientVersion);
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
    const QString tip = tipLines.join(QChar('\n'));
    item->setToolTip(0, tip);
    item->setToolTip(2, tip);
    item->setToolTip(3, connLabel.isEmpty() ? QStringLiteral("Connection type unknown") : connLabel);
    item->setData(0, Qt::UserRole, QVariant::fromValue(u.id));
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

static QString formatMatchDuration(qint64 startedAtMs)
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

namespace
{
// Sorts the Duration column (2) by match start time instead of the displayed
// "m:ss" text, which orders wrong once a duration passes 10 minutes and
// changes under the once-a-second ticker.
class MatchTreeItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const QTreeWidget* tree = treeWidget();
        if (tree && tree->sortColumn() == 2)
        {
            // A later start means a shorter duration.
            return data(2, Qt::UserRole).toLongLong() > other.data(2, Qt::UserRole).toLongLong();
        }
        if (tree && tree->sortColumn() == 3)
            return data(3, Qt::UserRole).toInt() < other.data(3, Qt::UserRole).toInt();
        return QTreeWidgetItem::operator<(other);
    }
};

// Sorts the rooms list's Ping column by the numeric RTT stashed in the cell's
// UserRole data — text like "~105 ms" orders wrong as a string. Unknown pings
// (-1) sort to the bottom in ascending order.
class RoomTreeItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const QTreeWidget* tree = treeWidget();
        if (tree && tree->sortColumn() == 2)
        {
            const int a = data(2, Qt::UserRole).toInt();
            const int b = other.data(2, Qt::UserRole).toInt();
            return (a < 0 ? 1'000'000 : a) < (b < 0 ? 1'000'000 : b);
        }
        return QTreeWidgetItem::operator<(other);
    }
};
} // namespace

void RollbackLobbyDialog::onRoomListChanged()
{
    m_roomsTree->clear();
    m_matchesTree->clear();
    m_roomItems.clear();

    const bool sameGameOnly =
        m_sameGameFilterCheck && m_sameGameFilterCheck->isChecked() &&
        m_sameGameFilterCheck->isEnabled();
    const QVariantMap selectedRom = sameGameOnly ? selectedBrowseRom() : QVariantMap{};
    const QString selectedMd5 = selectedRom.value("md5").toString().trimmed();
    const QString selectedName = selectedRom.value("name").toString().trimmed();

    for (auto it = m_client->rooms().constBegin(); it != m_client->rooms().constEnd(); ++it)
    {
        const LobbyClient::LobbyRoomSummary& r = it.value();

        if (sameGameOnly)
        {
            const bool sameMd5 =
                !selectedMd5.isEmpty() && !r.romMd5.isEmpty() &&
                selectedMd5.compare(r.romMd5, Qt::CaseInsensitive) == 0;
            const bool sameName =
                (selectedMd5.isEmpty() || r.romMd5.isEmpty()) &&
                !selectedName.isEmpty() &&
                selectedName.compare(r.romName, Qt::CaseInsensitive) == 0;
            if (!sameMd5 && !sameName)
                continue;
        }

        // A room that's playing is a live match — show it under Ongoing Matches
        // and drop it from the joinable Active Rooms list.
        if (r.state == "in_game")
        {
            auto* matchRow = new MatchTreeItem(m_matchesTree);
            matchRow->setText(0, r.playerNames.isEmpty() ? r.name : r.playerNames.join(tr(" vs ")));
            matchRow->setText(1, r.romName);
            matchRow->setText(2, formatMatchDuration(r.startedAtMs));
            matchRow->setText(3, matchViewersLabel(r));
            matchRow->setData(3, Qt::UserRole, r.broadcasting && r.matchId != 0 ? r.viewerCount : -1);
            matchRow->setData(0, Qt::UserRole, QVariant::fromValue(r.id));
            matchRow->setData(0, Qt::UserRole + 1, QVariant::fromValue(r.matchId)); // 0 unless broadcast
            matchRow->setData(2, Qt::UserRole, r.startedAtMs); // for the duration ticker

            // A live-replay match turns green with a watch hint; double-clicking
            // it watches live.
            if (r.broadcasting && r.matchId != 0)
            {
                const QColor live(0x2e, 0xa0, 0x43); // green, readable on light + dark
                const QString tip = tr("🔴 Live — double-click to watch");
                for (int col = 0; col < m_matchesTree->columnCount(); ++col)
                {
                    matchRow->setForeground(col, live);
                    matchRow->setToolTip(col, tip);
                }
            }
            if (!r.lateJoin)
                continue;
        }

        auto* row = new RoomTreeItem(m_roomsTree);
        refreshRoomRow(row, r);
        m_roomItems.insert(it.key(), row);
    }

    // spectators' state tooltips name their match from this list
    for (auto it = m_userItems.constBegin(); it != m_userItems.constEnd(); ++it)
    {
        const auto userIt = m_client->users().constFind(it.key());
        if (userIt != m_client->users().constEnd() && userIt->state == "spectating")
            it.value()->setToolTip(1, watchingTip(m_client, userIt->watchingMatchId));
    }
    updateServerMeta();
    updateInRoomBanner();   // seat counts may have changed in our own room
}

void RollbackLobbyDialog::updateMatchDurations()
{
    if (!m_matchesTree)
        return;
    for (int i = 0; i < m_matchesTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = m_matchesTree->topLevelItem(i);
        item->setText(2, formatMatchDuration(item->data(2, Qt::UserRole).toLongLong()));
    }
}

void RollbackLobbyDialog::switchToRoomsView()
{
    if (m_roomsStack) m_roomsStack->setCurrentIndex(0);
}

void RollbackLobbyDialog::switchToInRoomView()
{
    if (m_roomsStack) m_roomsStack->setCurrentIndex(1);
}

void RollbackLobbyDialog::clearServerRoomSnapshot()
{
    // None of this state owns gameplay transport. Once the lobby connection is
    // gone, retaining it can only display a room/roster that may no longer exist
    // (the server keeps rooms in memory and loses them across a restart).
    for (auto& seat : m_seats)
    {
        renderSeatEmpty(seat);
        if (seat.dragHandle)
            seat.dragHandle->setVisible(false);
    }
    m_canReorderSeats = false;

    m_currentRoomId = 0;
    m_currentRoomGame.clear();
    m_currentRoomMd5.clear();
    m_currentRoomRegion.clear();
    m_currentRoomState.clear();
    m_currentRoomDelay = 2;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    m_currentRoomPacing = 0;
    m_currentRoomHostId = 0;
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;
    m_localDelayPublished = false;
    m_probeFailureAnnounced.clear();
    updateRollbackSettingControls();

    if (m_roomTitle) m_roomTitle->setText(QStringLiteral("—"));
    if (m_roomRegionLabel) m_roomRegionLabel->setVisible(false);
    if (m_seatCountLabel) m_seatCountLabel->clear();
    if (m_chatViewRoom) m_chatViewRoom->clear();
    if (m_roomChatInput)
    {
        m_roomChatInput->clear();
        m_roomChatInput->setEnabled(false);
    }
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn) m_dropBtn->setEnabled(false);
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    switchToRoomsView();
    updateInRoomBanner();
}

void RollbackLobbyDialog::refreshRoomRow(QTreeWidgetItem* item, const LobbyClient::LobbyRoomSummary& r)
{
    const bool mine = (r.id == m_currentRoomId);
    QString nameCell = r.name;
    if (r.hasPassword) nameCell = QStringLiteral("🔒  ") + nameCell;
    if (r.ranked)      nameCell = tr("[Ranked]") + QStringLiteral("  ") + nameCell;
    if (mine)          nameCell = QStringLiteral("★  ") + nameCell;

    const bool dark = isDarkTheme();
    item->setText(0, nameCell);
    item->setText(1, r.hostName);
    item->setText(3, r.romName);
    item->setText(4, QString("%1/%2").arg(r.players).arg(r.maxPlayers));
    item->setData(0, Qt::UserRole, QVariant::fromValue(r.id));
    // Host id rides along so selecting the row can measure the host on demand.
    item->setData(0, Qt::UserRole + 1, QVariant::fromValue(r.hostId));

    // Your ping to the host — the real measurement once one exists, the region
    // estimate until then — so a room can be judged before joining. Nothing is
    // probed in the background any more; selecting the row measures it. Own
    // room shows a dash.
    const QString dash = QString(QChar(0x2014));
    int pingMs = -1;
    bool pingMeasured = false;
    if (m_client && r.hostId != 0 && r.hostId != m_client->selfUserId())
    {
        pingMs = m_client->measuredPingMs(r.hostId);
        pingMeasured = (pingMs >= 0);
        if (!pingMeasured)
        {
            const auto& users = m_client->users();
            const auto hostIt = users.constFind(r.hostId);
            if (hostIt != users.constEnd())
                pingMs = LobbyRegions::estimatedRttMs(m_client->selfRegion(), hostIt->region);
            if (pingMs == 0)
                pingMs = -1;
        }
    }
    item->setText(2, pingMs >= 0 ? QString(pingMeasured ? "%1 ms" : "~%1 ms").arg(pingMs) : dash);
    item->setForeground(2, QColor(pingHex(pingMs)));
    item->setData(2, Qt::UserRole, pingMs);
    item->setToolTip(2,
        pingMeasured  ? tr("Your measured ping to the host")
        : pingMs >= 0 ? tr("Estimated from regions — click the row to measure")
                      : tr("Click the row to measure your ping to the host"));

    // Seats: green when there's room to join, red when full.
    const bool full = (r.maxPlayers > 0 && r.players >= r.maxPlayers);
    const auto sc = statusColors();
    item->setForeground(4, QColor(full ? sc.fail : sc.ok));

    // A running match with a free seat: joining seats you beside it, and the
    // host adds you to the game.
    if (r.state == "in_game")
    {
        item->setText(0, nameCell + QStringLiteral("  ") + tr("(playing)"));
        const QString tip = tr("Match in progress. Join, and the host can add you to the game.");
        for (int col = 0; col < item->columnCount(); ++col)
            item->setToolTip(col, tip);
    }

    // Bold the user's own room and tint its name with the brand accent. Blue,
    // not a seat color — "mine" is unrelated to which seat you end up in.
    QFont f = item->font(0);
    f.setBold(mine);
    item->setFont(0, f);
    if (mine)
        item->setForeground(0, QColor(dark ? QStringLiteral("#4aa3ff")
                                           : QStringLiteral("#0078D7")));
}

void RollbackLobbyDialog::showAdminRoomMenu(QTreeWidget* tree, const QPoint& pos)
{
    // Right-click close is a moderator tool — everyone else gets no menu.
    if (!m_client || !m_client->isModerator())
        return;

    QTreeWidgetItem* item = tree->itemAt(pos);
    if (!item)
        return;
    const quint64 roomId = item->data(0, Qt::UserRole).toULongLong();
    if (roomId == 0)
        return;

    QMenu menu(tree);
    QAction* closeAction = menu.addAction(tree == m_matchesTree
        ? tr("End match && close room  (moderator)")
        : tr("Close room  (moderator)"));
    if (menu.exec(tree->viewport()->mapToGlobal(pos)) != closeAction)
        return;

    if (QMessageBox::question(this, tr("Close room"),
            tr("Close \"%1\" for everyone?").arg(item->text(0))) != QMessageBox::Yes)
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

    const CreateRoomRankedOptions rankedOptions = rankedOptionsFor(romMd5);

    m_createRoomDialog = new CreateRoomDialog(
        m_username, m_lastRoomName, romName, romMd5, rankedOptions, this);
    connect(m_createRoomDialog, &CreateRoomDialog::createRequested,
            this, &RollbackLobbyDialog::onRoomCreateRequested);
    connect(m_createRoomDialog, &QDialog::finished, this, [this](int) {
        m_createRoomDialog->deleteLater();
        m_createRoomDialog = nullptr;
    });
    m_createRoomDialog->show();
}

CreateRoomRankedOptions RollbackLobbyDialog::rankedOptionsFor(const QString& romMd5) const
{
    CreateRoomRankedOptions rankedOptions;
    if (m_client->ladders().isEmpty())
        return rankedOptions;
    // one format per ladder the game is on (1v1, 2v2)
    const QList<const LobbyClient::LadderInfo*> ladders = m_client->laddersForRomMd5(romMd5);
    const LobbyClient::LadderInfo* ladder = ladders.isEmpty() ? nullptr : ladders.first();
    bool moduleInstalled = false;
    if (ladder)
    {
        for (const CoreRankedModuleInfo& module : CoreGetRankedModules())
            moduleInstalled |= QString::fromStdString(module.Id) == ladder->moduleId;
    }
    if (!ladder)
        return rankedOptions; // not a ranked game: Create Room shows no ranked option
    if (!m_client->isVerified())
        rankedOptions.unavailableReason = tr("log into an account to play ranked");
    else if (!moduleInstalled)
        rankedOptions.unavailableReason = tr("the %1 ranked module isn't installed").arg(ladder->moduleId);
    else
    {
        rankedOptions.available = true;
        for (const LobbyClient::LadderInfo* l : ladders)
        {
            CreateRoomRankedOptions::Format format;
            format.ladderId   = l->id;
            format.ladderName = l->name;
            format.teamSize   = l->teamSize;
            format.bestOf     = l->bestOf;
            for (const QString& id : l->rulesets)
            {
                format.rulesets.append({id, l->rulesetNames.value(id, id)});
                if (const auto* standing = m_client->standing(l->id, id))
                    format.standings.insert(id, standingText(*standing));
            }
            rankedOptions.formats.append(format);
        }
    }
    return rankedOptions;
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
        [this]() {
            QJsonObject ranked;
            if (!m_createRoomDialog->ranked())
                return ranked;
            ranked["enabled"]          = true;
            if (!m_createRoomDialog->ladderId().isEmpty())
                ranked["ladderId"]     = m_createRoomDialog->ladderId();
            ranked["rulesetId"]        = m_createRoomDialog->rulesetId();
            ranked["maxPing"]          = m_createRoomDialog->maxPing();
            return ranked;
        }());
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
    else if (reason == "not_verified") human =
        tr("This is a ranked room. Log into an account to join it.");
    else if (reason == "skill_mismatch") human =
        tr("Your rating is outside this ranked room's skill range.");
    else if (reason == "too_far") human =
        tr("You're too far from this ranked room's host for its max ping.");
    else if (reason == "ranked_unavailable") human =
        tr("Ranked play is temporarily unavailable.");
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
    if (m_recordCheck)
        m_recordCheck->setChecked(recordingDefault);

    // Being in a room is mutually exclusive with searching. A successful Quick
    // Match drops us straight into an auto-created room without the server ever
    // sending QUICK_MATCH_STATUS{searching:false}, so clear the toggle state
    // here. Otherwise m_quickMatchActive stays true through the game and onClick
    // sends quickMatchCancel() (a no-op once we're out of the queue) instead of
    // quickMatchJoin() — leaving the button unable to start a new search.
    resetQuickMatchButtons();
    closeRematchPrompt();

    // Restore only our selected mode/value, never an Auto calculation or a
    // remote player's delay. With remembering disabled, each room uses Auto.
    int preferredDelay = -1;
    if (CoreSettingsGetBoolValue(SettingsID::Rollback_RememberInputDelay))
    {
        QSettings settings("RMG-K", "n02");
        bool valid = false;
        const int savedDelay = settings.value("RollbackLobby/LastInputDelay", -1).toInt(&valid);
        if (valid && m_delayCombo && m_delayCombo->findData(savedDelay) >= 0)
            preferredDelay = savedDelay;
    }
    m_delayAuto = preferredDelay == -1;
    m_predictionAuto = true;
    m_localDelayPublished = false;
    m_currentRoomDelay = m_delayAuto ? autoFrameDelayForPing(-1) : preferredDelay;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    if (m_delayCombo && m_predictionCombo)
    {
        m_suppressSettingsSignal = true;
        m_delayCombo->setCurrentIndex(m_delayCombo->findData(preferredDelay));
        setAutoComboLabel(m_delayCombo, m_currentRoomDelay);
        m_predictionCombo->setCurrentIndex(m_predictionCombo->findData(0));
        m_suppressSettingsSignal = false;
    }

    // Fresh room — don't chime for the seats already present in the first
    // ROOM_STATE; only for players who join after we've settled in.
    m_knownSeatedUsers.clear();
    m_reportedPeerPings.clear();
    m_roomSeatsSeen = false;

    // Fresh room, fresh chat — never carry the previous room's messages in.
    // The room chat widget lives in the in-room view (below the seats) and
    // persists; just clear it and enable its input.
    if (m_chatViewRoom) m_chatViewRoom->clear();
    if (m_roomChatInput) m_roomChatInput->setEnabled(true);
    switchToInRoomView();

    updateHeroButtons();
    updateSessionUi();

    updateInRoomBanner();

    if (!greetingChatLine.isEmpty())
        appendChatLine(CHANNEL_ROOM, greetingChatLine);

    // Backstop in case ROOM_STATE seats arrive in a shape that didn't trigger a
    // per-peer probe — kick one sweep shortly after entry so every client starts
    // filling the room-wide ping matrix promptly. Harmless in an empty room:
    // onPingProbeTick skips self / unseated slots.
    QTimer::singleShot(600, this, &RollbackLobbyDialog::onPingProbeTick);
}

void RollbackLobbyDialog::onRoomDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected) return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("Already in a room"),
            tr("Leave your current room before joining another."));
        return;
    }

    const quint64 roomId = item->data(0, Qt::UserRole).toULongLong();
    if (roomId == 0) return;

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

    QString password;
    if (summary.hasPassword)
    {
        bool ok = false;
        password = QInputDialog::getText(this, tr("Password required"),
            QString(tr("Enter password for \"%1\":")).arg(summary.name),
            QLineEdit::Password, QString(), &ok);
        if (!ok) return;
    }
    m_client->joinRoom(roomId, password);
}

void RollbackLobbyDialog::onMatchDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected) return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("In a room"),
            tr("Leave your room before watching a match."));
        return;
    }
    const quint64 matchId = item->data(0, Qt::UserRole + 1).toULongLong();
    if (matchId == 0)
    {
        QMessageBox::information(this, tr("Not live"),
            tr("That match isn't streaming a live replay, so there's nothing to watch."));
        return;
    }
    QString romMd5;
    for (const auto& summary : m_client->rooms())
    {
        if (summary.matchId == matchId)
        {
            romMd5 = summary.romMd5;
            break;
        }
    }
    beginSpectate(matchId, item->text(1), romMd5); // ROM name column
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

    // Pacing is host-authoritative and applies to every seat. Mirror it into the
    // engine setting now (host included) so the match starts on the room's model;
    // reset_gekko_log() reads Rollback_PacingMode when the session begins.
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, pacing);

    // Auto-stop emulation if the room transitioned out of in_game while we
    // still had a live local match (host dropped it for everyone).
    const QString prevState = m_currentRoomState;
    m_currentRoomState = state;
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

    // ── Header card ──
    QString title = romName.isEmpty() ? name : romName;
    if (!name.isEmpty() && name != romName && !romName.isEmpty())
        title = QString("%1 — %2").arg(name, romName);
    m_roomTitle->setText(title);

    applyRoomStateBadge(roomStateLabel(state), stateHex(state, isDarkTheme()));

    const QJsonArray players = roomState.value("players").toArray();
    m_seatCountLabel->setText(QString("(%1/%2)").arg(players.size()).arg(maxPlayers));
    m_currentRoomRanked = roomState.value("ranked").toObject();
    const bool roomRanked = !m_currentRoomRanked.isEmpty();
    QStringList metaParts;
    if (roomRanked)
    {
        QString rankedPart = tr("<b>Ranked:</b> Bo%1 · %2")
            .arg(m_currentRoomRanked.value("bestOf").toInt())
            .arg(m_currentRoomRanked.value("ladderName").toString().toHtmlEscaped());
        const QString rulesetName = m_currentRoomRanked.value("rulesetName").toString();
        if (!rulesetName.isEmpty())
            rankedPart += QString(" · %1").arg(rulesetName.toHtmlEscaped());
        if (const int maxPing = m_currentRoomRanked.value("maxPing").toInt(); maxPing > 0)
            rankedPart += tr(" · max %1 ms").arg(maxPing);
        metaParts << rankedPart;
    }
    if (!romRegion.isEmpty())
        metaParts << QString("<b>%1:</b> %2").arg(tr("Region"), romRegion.toHtmlEscaped());
    m_roomRegionLabel->setText(metaParts.join("  ·  "));
    m_roomRegionLabel->setVisible(!metaParts.isEmpty());
    // "Keep Teammate" shows once the room is known to be a 2v2 set
    if (m_keepTeammateCheck)
        m_keepTeammateCheck->setVisible(keepTeammateApplies());

    // Delay remains local to every player. Prediction mirrors authoritative
    // room state; updateRollbackSettingControls() decides who may edit them.
    if (m_delayCombo && m_predictionCombo)
    {
        setAutoComboLabel(m_delayCombo, m_currentRoomDelay);

        m_suppressSettingsSignal = true;
        const int predictionData = roomPredictionAuto ? 0 : roomPrediction;
        const int predictionIndex = m_predictionCombo->findData(predictionData);
        if (predictionIndex >= 0)
            m_predictionCombo->setCurrentIndex(predictionIndex);
        m_suppressSettingsSignal = false;
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
        appendChatSystemLine(CHANNEL_ROOM, tr("Prediction is now %1 f.").arg(roomPrediction));
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

    if (m_rankedCheck)
    {
        // Only offered when the room's game is on one of the server's ladders.
        const bool hasLadder = m_client->ladderForRomMd5(romMd5) != nullptr;
        m_suppressSettingsSignal = true;
        m_rankedCheck->setChecked(roomRanked);
        m_suppressSettingsSignal = false;
        m_rankedCheck->setVisible(hasLadder || roomRanked);
        m_rankedCheck->setEnabled(iAmHost && state == "waiting" && m_client->isVerified());
        if (!iAmHost)
            m_rankedCheck->setToolTip(roomRanked
                ? tr("This room plays ranked sets.")
                : tr("The host can make this room ranked."));
        else if (!m_client->isVerified())
            m_rankedCheck->setToolTip(tr("Log into an account to play ranked."));
        else if (state != "waiting")
            m_rankedCheck->setToolTip(tr("Ranked can only be changed between games."));
        else
            m_rankedCheck->setToolTip(tr("Sets that count toward ratings. Every player needs an account."));
    }

    // ── Seats ──
    QVector<bool> filled(4, false);
    for (const auto& v : players)
    {
        const auto p = v.toObject();
        const int slot = p.value("slot").toInt();
        if (slot < 1 || slot > 4) continue;
        QString user = p.value("username").toString();
        if (p.contains("standing"))
            user += QStringLiteral("  ") + standingText(LobbyClient::parseStanding(p.value("standing").toObject()));
        else if (p.value("verified").toBool())
            user += QStringLiteral("  ✓");
        const quint64 uid = static_cast<quint64>(p.value("userId").toDouble());
        const bool slotIsHost = (uid == hostId);
        const bool slotIsSelf = (uid == m_client->selfUserId());
        const int pingMs = slotIsSelf ? 0 : m_client->measuredPingMs(uid);
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
        // The host can remove any seated player except themselves (the host seat).
        const bool canKick = iAmHost && !slotIsHost;
        if (m_seats[slot - 1].userId != uid)
        {
            m_seats[slot - 1].iceAttempt = 1;
            m_seats[slot - 1].iceMaxAttempts = 20;
        }
        m_seats[slot - 1].userId = uid;
        renderSeatFilled(m_seats[slot - 1], user, slotIsHost, slotIsSelf,
                         pingMs, frameDelay, canKick, connectionType);
        setCountryFlagLabel(m_seats[slot - 1].countryLabel, country, anonymous);
        if (!slotIsSelf && pingMs < 0 && m_seats[slot - 1].metaLabel)
            m_seats[slot - 1].metaLabel->setText(
                seatMetaHtml(slot, slotIsHost, pingMs, frameDelay, isDarkTheme(),
                             seatIceStatusHtml(m_seats[slot - 1].iceAttempt,
                                               m_seats[slot - 1].iceMaxAttempts)));
        // Older servers don't send inMatch; everyone seated then plays.
        if (m_seats[slot - 1].statusLabel)
        {
            const bool waiting = state == "in_game" && !p.value("inMatch").toBool(true);
            m_seats[slot - 1].statusLabel->setText(
                QString("<span style='color:%1; font-weight:600;'>%2</span>")
                    .arg(statusColors().wait, tr("waiting to join")));
            m_seats[slot - 1].statusLabel->setVisible(waiting);
        }
        filled[slot - 1] = true;
    }
    for (int i = 0; i < 4; ++i)
    {
        if (i >= maxPlayers) m_seats[i].row->setVisible(false);
        else
        {
            m_seats[i].row->setVisible(true);
            if (!filled[i])
            {
                m_seats[i].userId = 0;
                renderSeatEmpty(m_seats[i]);
            }
        }
    }

    // Seat reorder is host-only and pre-match. Show the drag grip on occupied,
    // visible seats so the host can drag one onto another to swap them.
    m_canReorderSeats = iAmHost && (state == "waiting");
    for (int i = 0; i < 4; ++i)
    {
        if (m_seats[i].dragHandle)
            m_seats[i].dragHandle->setVisible(
                m_canReorderSeats && i < maxPlayers && m_seats[i].userId != 0);
    }

    // Late join: track who sits outside the running match. Players in the
    // match hear about a newcomer waiting to join; the newcomer learns what
    // happens next.
    {
        const quint64 selfId = m_client->selfUserId();
        QSet<quint64> outOfMatch;
        m_lateJoinCapableUsers.clear();
        bool selfOutOfMatch = false;
        for (const auto& v : players)
        {
            const auto p = v.toObject();
            const quint64 uid = static_cast<quint64>(p.value("userId").toDouble());
            if (p.value("lateJoin").toBool())
                m_lateJoinCapableUsers.insert(uid);
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
                if (uid == selfId || m_knownSeatedUsers.contains(uid))
                    continue;
                const QString who = lobbyUserName(uid);
                appendChatSystemLine(CHANNEL_ROOM, iAmHost
                    ? tr("%1 is waiting to join. Click Add to Game on their seat to bring them in.").arg(who)
                    : tr("%1 is waiting to join. The host can add them to the game.").arg(who));
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
                    tr("A match is in progress. The host can add you to the game; it starts on its own when they do."));
            }
        }
        else if (!selfOutOfMatch)
        {
            m_announcedWaitingSelf = false;
        }
        m_outOfMatchUsers = outOfMatch;
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
        const quint64 selfId = m_client->selfUserId();
        bool sawNewPeer = false;
        for (const quint64 uid : currentSeated)
        {
            if (uid == selfId || m_knownSeatedUsers.contains(uid))
                continue;
            // Probe a newly-seated peer immediately so their ping appears within
            // an RTT instead of waiting on the next probe tick — and so this
            // player's Auto delay resolves from real ping before the match begins.
            if (m_client) m_client->requestPingProbe(uid);
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
    if (!m_recordCheck || !m_broadcastCheck)
        return;

    const bool iAmHost = m_client && m_currentRoomId != 0 &&
        m_currentRoomHostId == m_client->selfUserId();
    const bool liveReplayEnabled = m_broadcastCheck->isChecked();
    const bool editable = m_currentRoomState == "waiting" && !matchTransportInProgress();

    m_recordCheck->setEnabled(editable);
    if (!editable)
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
                           "A local replay is saved only if Record locally is checked."));
    }
}

void RollbackLobbyDialog::refreshStartButton()
{
    if (!m_startBtn)
        return;

    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    const bool iAmHost = (m_client && m_currentRoomHostId == selfId);
    const bool waiting = (m_currentRoomState == "waiting");

    // Seats may be sparse — two players in P1 and P3 is a legitimate 3-port
    // match with P2 empty, so this counts occupants without requiring them to
    // run contiguously from P1. The core sizes the GekkoNet session by the
    // highest occupied seat and feeds empty ports zeroed input.
    int seated = 0;
    QList<quint64> seatedUserIds;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0)
            continue;
        ++seated;
        seatedUserIds.append(s.userId);
    }

    // Alone there are no paths to measure. The server only allows it when
    // whoever joins later can be added to the running match.
    const bool alone = seated == 1 && m_canStartAlone;
    const bool enoughPlayers = seated >= 2 || alone;
    bool pingsReady = alone;
    if (m_client && !alone)
        m_client->worstRoomPingMs(seatedUserIds, &pingsReady);
    const bool canStart      = iAmHost && waiting && enoughPlayers && pingsReady;

    m_startBtn->setEnabled(canStart);
    m_startBtn->setToolTip(
        !iAmHost         ? tr("Only the host can start the game.")
        : !waiting       ? tr("Already in a match.")
        : !enoughPlayers ? tr("Need at least 2 players to start.")
        : !pingsReady    ? tr("Measuring every player-to-player path…")
        : alone          ? tr("Start playing now. You can add anyone who joins the room to the game.")
                         : QString());
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
    if (!m_awaitingEmulationStart) return;
    m_awaitingEmulationStart = false;
    m_emulationActive = true;
    updateRollbackSettingControls();
    if (m_casualSession)
    {
        m_sessionMatches++;
        updateSessionUi();
    }

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
    refreshAdmitButtons();
    if (m_lateJoin.role == LateJoinRole::Joiner)
        return;
    m_client->reportMatchConnected(m_currentMatchId, 0);
    appendChatSystemLine(CHANNEL_ROOM, tr("Match started — playing now."));
}

void RollbackLobbyDialog::notifyEmulationFinished()
{
    // Always close out a broadcast if one is running, even on paths that fall
    // through the early-return below (it flushes the tail + sends BROADCAST_END).
    stopBroadcast();

    const bool ownedMatchTransport = m_client && m_client->matchTransportActive();
    if (!m_emulationActive && !m_awaitingEmulationStart && !ownedMatchTransport)
    {
        // The room closed while the ranked set's game was still running (a
        // ranked session moving on to its next match): keep its replay.
        if (m_rankedMatchId != 0)
        {
            CoreRankedClearStagedSession();
            captureRankedReplay();
            m_rankedMatchId = 0;
        }
        return;
    }
    const quint64 matchId = m_currentMatchId;
    m_emulationActive = false;
    m_awaitingEmulationStart = false;
    endLiveRollbackSettings();
    if (m_client)
        m_client->setMatchTransportActive(false);
    updateRecordingControls();
    // A staged session that never started must not leak into the next game.
    CoreRankedClearStagedSession();
    if (m_rankedMatchId != 0)
        captureRankedReplay();
    m_rankedMatchId = 0;

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
    // A hand-off can't outlive our game.
    if (m_lateJoin.role != LateJoinRole::None)
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

QString RollbackLobbyDialog::tierName(const QString& tier)
{
    // the server's tier bands (ranked.json); other names pass through
    if (tier == "Bronze")      return tr("Bronze");
    if (tier == "Silver")      return tr("Silver");
    if (tier == "Gold")        return tr("Gold");
    if (tier == "Platinum")    return tr("Platinum");
    if (tier == "Diamond")     return tr("Diamond");
    if (tier == "Master")      return tr("Master");
    if (tier == "Grandmaster") return tr("Grandmaster");
    return tier;
}

QString RollbackLobbyDialog::standingText(const LobbyClient::LadderStanding& standing)
{
    // placement takes about 10-15 sets in 1v1 and 15-20 in 2v2, so count sets
    if (standing.provisional)
        return standing.setsPlayed == 1
            ? tr("✓ %1 (in placement · 1 set)").arg(standing.rating)
            : tr("✓ %1 (in placement · %2 sets)").arg(standing.rating).arg(standing.setsPlayed);
    return QString("✓ %1 %2").arg(tierName(standing.tier)).arg(standing.rating);
}

void RollbackLobbyDialog::onAccountUpdated()
{
    if (!m_client)
        return;

    // Adopt the server's spelling of the name (logins are case-insensitive).
    // Our standings are in the name's hover card.
    m_username = m_client->selfUsername();
    if (m_client->isVerified())
    {
        if (m_rememberLogin && !m_client->sessionToken().isEmpty())
            LobbyConnectDialog::saveSessionToken(m_username, m_client->sessionToken());
        else
            LobbyConnectDialog::clearSessionToken();
    }
    if (m_ratings)
    {
        m_ratings->setServer(m_serverUrl);
        m_ratings->invalidate(m_username); // standings change after a rated set
    }
    if (m_userButton)
    {
        m_userButton->setText(m_client->isVerified() ? tr("User: %1").arg(m_username)
                                                     : tr("User: %1").arg(m_username) + tr(" (guest)"));
        // the account menu is for accounts
        if (auto* menu = m_userButton->findChild<QMenu*>())
            m_userButton->setMenu(m_client->isVerified() ? menu : nullptr);
        m_userButton->setCursor(m_client->isVerified() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    if (m_changePasswordAction)
        m_changePasswordAction->setVisible(m_client->isVerified());
    updateEmailAction();
    // confirms a new account, a reset or a Discord link once
    if (m_client->isVerified() && !m_welcomeLine.isEmpty())
    {
        appendChatSystemLine(CHANNEL_LOBBY, m_welcomeLine);
        m_welcomeLine.clear();
    }
    updateHeroButtons(); // the ladders (which games are ranked) arrive with HELLO_OK
    if (m_client->state() == LobbyClient::ConnectionState::Connected)
        LobbyConnectDialog::saveConnectedMode(m_client->isVerified());

    // a Discord sign-in sent no name; remember the account's for next time
    if (m_client->isVerified())
    {
        QSettings s("RMG-K", "n02");
        s.setValue("Lobby/Username", m_username);
        if (m_discordCheckedUrl != m_serverUrl && m_discordLogin)
        {
            m_discordCheckedUrl = m_serverUrl;
            m_discordAvailable  = false;
            m_discordLogin->checkAvailable(m_serverUrl);
        }
    }
    updateDiscordButton();
}

void RollbackLobbyDialog::updateDiscordButton()
{
    if (!m_discordAction || !m_client)
        return;
    const LobbyClient::DiscordLink& link = m_client->discordLink();
    m_discordAction->setVisible(m_client->isVerified() && m_discordAvailable);
    if (m_discordLogin && m_discordLogin->isRunning())
    {
        m_discordAction->setText(tr("Waiting for Discord… (Cancel)"));
        m_discordAction->setToolTip(tr("Finish signing in to Discord in your browser."));
    }
    else if (link.linked)
    {
        m_discordAction->setText(tr("Unlink Discord"));
        m_discordAction->setToolTip(tr("You can log in with the Discord account %1. Only you and moderators see this.")
                                     .arg(link.username));
    }
    else
    {
        m_discordAction->setText(tr("Link Discord…"));
        m_discordAction->setToolTip(tr("Link a Discord account to log in without typing your password."));
    }
}

void RollbackLobbyDialog::showRatingsTip(const QString& username, const QString& base,
                                         const QPoint& globalPos, QWidget* widget)
{
    m_ratingsTipName   = username;
    m_ratingsTipBase   = base;
    m_ratingsTipPos    = globalPos;
    m_ratingsTipWidget = widget;
    QStringList parts;
    if (!base.isEmpty())
        parts << base;
    if (!username.isEmpty())
    {
        const QString ratings = ratingsText(username);
        if (!ratings.isEmpty())
            parts << ratings;
    }
    if (parts.isEmpty())
    {
        QToolTip::hideText();
        return;
    }
    QToolTip::showText(globalPos, parts.join(QStringLiteral("\n\n")), widget);
}

QString RollbackLobbyDialog::ratingsText(const QString& username)
{
    if (!m_ratings || !m_client)
        return QString();
    m_ratings->setServer(m_serverUrl);
    QJsonArray standings;
    QString region;
    const PlayerRatings::Status status = m_ratings->lookup(username, standings, region);
    if (status == PlayerRatings::Status::Loading)
        return tr("Ranked: loading…");
    if (status == PlayerRatings::Status::NotFound)
        return QString();
    if (standings.isEmpty())
        return tr("Ranked: no rated sets yet");

    static const QHash<QString, QString> regionNames = {
        { "na", "NA" }, { "sa", "SA" }, { "eu", "EU" }, { "asia", "Asia" }, { "oc", "OCE" },
    };
    const QString regionName = regionNames.value(region, region.toUpper());

    // 1v1 before 2v2, in the lobby's ladder order
    struct Line { int teamSize; int order; QString text; };
    QList<Line> lines;
    int order = 0;
    for (const QJsonValue& value : standings)
    {
        const QJsonObject s = value.toObject();
        const QString ladderId = s.value("ladder").toString();
        const QString ruleset  = s.value("ruleset").toString();
        int teamSize = 1;
        QString roster = s.value("rulesetName").toString();
        for (const LobbyClient::LadderInfo& ladder : m_client->ladders())
        {
            if (ladder.id == ladderId)
            {
                teamSize = ladder.teamSize;
                roster = ladder.rulesetNames.value(ruleset, roster);
            }
        }
        roster.remove(QRegularExpression("\\s+roster$", QRegularExpression::CaseInsensitiveOption));
        const QString board = QString("%1v%1 %2").arg(teamSize).arg(roster).trimmed();
        const int rating = s.value("rating").toInt();
        const int sets   = s.value("sets").toInt();
        QString text;
        if (s.value("provisional").toBool())
        {
            text = sets == 1 ? tr("%1: %2, in placement (1 set)").arg(board).arg(rating)
                             : tr("%1: %2, in placement (%3 sets)").arg(board).arg(rating).arg(sets);
        }
        else
        {
            text = tr("%1: %2 %3 · #%4").arg(board, tierName(s.value("tier").toString())).arg(rating)
                       .arg(s.value("rank").toInt());
            if (s.value("regionRank").toInt() > 0 && !regionName.isEmpty())
                text += tr(" (%1 #%2)").arg(regionName).arg(s.value("regionRank").toInt());
            text += tr(" · %1–%2").arg(s.value("wins").toInt()).arg(s.value("losses").toInt());
        }
        lines.append({ teamSize, order++, text });
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.teamSize < b.teamSize; });
    QStringList out;
    out << tr("Ranked");
    for (const Line& line : lines)
        out << line.text;
    return out.join('\n');
}

void RollbackLobbyDialog::onDiscordButton()
{
    if (!m_client || !m_client->isVerified() || !m_discordLogin)
        return;
    if (m_discordLogin->isRunning())
    {
        m_discordLogin->cancel();
        updateDiscordButton();
        return;
    }
    const LobbyClient::DiscordLink& link = m_client->discordLink();
    if (link.linked)
    {
        if (QMessageBox::question(this, tr("Unlink Discord"),
                tr("Stop logging in with the Discord account %1? Your password keeps working.").arg(link.username))
            != QMessageBox::Yes)
            return;
        m_client->unlinkDiscord();
        return;
    }
    m_discordLogin->start(m_serverUrl);
    updateDiscordButton();
}

void RollbackLobbyDialog::onDiscordLinkResult(bool ok, const QString& message)
{
    updateDiscordButton();
    if (ok)
    {
        const LobbyClient::DiscordLink& link = m_client->discordLink();
        appendChatSystemLine(CHANNEL_LOBBY, link.linked
            ? tr("Discord linked: you can now use Log in with Discord.")
            : tr("Discord unlinked. Use your password next time."));
        return;
    }
    QMessageBox::warning(this, tr("Link Discord"),
        message.isEmpty() ? tr("Discord couldn't be linked.") : message);
}

void RollbackLobbyDialog::onChangePassword()
{
    if (!m_client || !m_client->isVerified())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Change Password"));
    auto* form = new QFormLayout(&dialog);
    auto makeField = [&dialog]() {
        auto* edit = new QLineEdit(&dialog);
        edit->setEchoMode(QLineEdit::Password);
        edit->setMaxLength(128);
        return edit;
    };
    QLineEdit* current = makeField();
    QLineEdit* next    = makeField();
    QLineEdit* confirm = makeField();
    form->addRow(tr("Current password:"), current);
    form->addRow(tr("New password:"), next);
    form->addRow(tr("Confirm new password:"), confirm);
    auto* note = new QLabel(tr("You'll stay logged in here; other saved logins are signed out."), &dialog);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (next->text().length() < 8)
            note->setText(tr("Choose a new password of at least 8 characters."));
        else if (next->text() != confirm->text())
            note->setText(tr("The new passwords don't match."));
        else
            dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    m_client->changePassword(current->text(), next->text());
}

void RollbackLobbyDialog::onLogOut()
{
    if (!m_client || !m_client->isVerified())
        return;
    if (matchTransportInProgress())
    {
        QMessageBox::information(this, tr("Log out"), tr("Finish the match before logging out."));
        return;
    }
    // The lobby revokes the saved login; disconnecting returns to the login
    // screen, which now asks for the password.
    m_client->logout();
    LobbyConnectDialog::clearSessionToken();
    m_connectPromptMessage.clear();
    m_useSavedLogin = false;
    m_client->disconnectFromServer();
}

void RollbackLobbyDialog::updateEmailAction()
{
    if (!m_emailAction)
        return;
    // only when the lobby can send email
    m_emailAction->setVisible(m_client->isVerified() && m_client->emailAvailable());
    const QString email = m_client->accountEmail();
    m_emailAction->setText(email.isEmpty() ? tr("Add email address…") : tr("Email address…"));
    m_emailAction->setToolTip(email.isEmpty()
        ? tr("Lets you email yourself a reset code if you forget your password.")
        : tr("Reset codes go to %1.").arg(email));
}

void RollbackLobbyDialog::onEmailAction()
{
    if (!m_client || !m_client->isVerified())
        return;
    AccountEmailDialog dialog(m_client, this);
    dialog.exec();
}

void RollbackLobbyDialog::onEmailUpdated(const QString& address)
{
    updateEmailAction();
    if (address.isEmpty())
        appendChatSystemLine(CHANNEL_LOBBY, tr("Email address removed."));
    else
        appendChatSystemLine(CHANNEL_LOBBY, tr("Email address confirmed: %1. If you forget your password, "
                                               "you can email yourself a reset code.").arg(address.toHtmlEscaped()));
}

void RollbackLobbyDialog::onPasswordChangeResult(bool ok, const QString& message)
{
    if (ok)
    {
        appendChatSystemLine(CHANNEL_LOBBY, tr("Password changed. Other saved logins were signed out."));
        return;
    }
    QMessageBox::warning(this, tr("Change Password"),
        message.isEmpty() ? tr("Your password couldn't be changed.") : message);
}

void RollbackLobbyDialog::reportRankedSetEnded()
{
    if (!m_client || m_rankedMatchId == 0 || m_rankedResultSent)
        return;

    // The set result the ranked host recorded (Ranked.cpp build_result_json);
    // controller ports are the lobby's seat slots.
    const QJsonObject record = QJsonDocument::fromJson(
        QByteArray::fromStdString(CoreRankedGetLastResultJson())).object();
    if (record.isEmpty())
        return;

    QJsonArray games;
    for (const QJsonValue& value : record.value("games").toArray())
    {
        const QJsonObject game = value.toObject();
        QJsonArray players;
        for (const QJsonValue& p : game.value("players").toArray())
        {
            const QJsonObject player = p.toObject();
            QJsonObject out;
            out["slot"]       = player.value("port").toInt();
            out["character"]  = player.value("character").toInt();
            out["stocksLeft"] = player.value("stocksLeft").toInt();
            players.append(out);
        }
        QJsonObject out;
        out["game"]       = game.value("game").toInt();
        out["winnerSlot"] = game.value("winnerPort").toInt();
        if (game.value("winnerTeam").toInt() != 0)
            out["winnerTeam"] = game.value("winnerTeam").toInt();
        out["stage"]      = game.value("stage").toInt();
        out["players"]    = players;
        games.append(out);
    }

    QJsonObject result;
    result["matchId"]     = QJsonValue(qint64(m_rankedMatchId));
    result["winnerSlot"]  = record.value("winnerPort").toInt();
    result["score"]       = record.value("score").toArray();
    // 2v2: each slot's team and the winning team
    if (record.value("winnerTeam").toInt() != 0)
    {
        result["teams"]      = record.value("teams").toArray();
        result["winnerTeam"] = record.value("winnerTeam").toInt();
    }
    result["games"]       = games;
    result["voidedGames"] = record.value("voidedGames").toInt();
    result["rulesetHash"] = record.value("ruleset").toObject().value("hash").toString();
    result["integrity"]   = record.value("integrity").toString();
    // what the set was played with, so its replay can be played back later
    result["rulesetId"]     = record.value("ruleset").toObject().value("id").toString();
    result["moduleVersion"] = record.value("module").toObject().value("version").toString();
    result["romMd5"]        = record.value("romMd5").toString();
    m_client->reportMatchResult(result);
    m_rankedResultSent = true;
    appendChatSystemLine(CHANNEL_ROOM, tr("Ranked set finished — result sent."));
}

// the counted games (controller ports are the lobby's seat slots)
static QJsonArray rankedGamesForLobby(const QJsonArray& games)
{
    QJsonArray out;
    for (const QJsonValue& value : games)
    {
        const QJsonObject game = value.toObject();
        QJsonArray players;
        for (const QJsonValue& p : game.value("players").toArray())
        {
            const QJsonObject player = p.toObject();
            QJsonObject entry;
            entry["slot"]       = player.value("port").toInt();
            entry["character"]  = player.value("character").toInt();
            entry["stocksLeft"] = player.value("stocksLeft").toInt();
            players.append(entry);
        }
        QJsonObject entry;
        entry["game"]       = game.value("game").toInt();
        entry["winnerSlot"] = game.value("winnerPort").toInt();
        if (game.value("winnerTeam").toInt() != 0)
            entry["winnerTeam"] = game.value("winnerTeam").toInt();
        entry["stage"]   = game.value("stage").toInt();
        entry["players"] = players;
        out.append(entry);
    }
    return out;
}

void RollbackLobbyDialog::reportRankedProgress()
{
    if (!m_client || m_rankedMatchId == 0 || m_rankedResultSent)
        return;
    const QJsonObject progress = QJsonDocument::fromJson(
        QByteArray::fromStdString(CoreRankedGetProgressJson())).object();
    if (progress.isEmpty())
        return;
    QJsonObject report;
    report["matchId"] = QJsonValue(qint64(m_rankedMatchId));
    report["games"]   = rankedGamesForLobby(progress.value("games").toArray());
    m_client->reportMatchProgress(report);
}

void RollbackLobbyDialog::reportRankedPlayerLeft()
{
    if (!m_client || m_rankedMatchId == 0 || m_rankedResultSent)
        return;
    // the set as of the frame the player left (Ranked.cpp build_left_json)
    const QJsonObject left = QJsonDocument::fromJson(
        QByteArray::fromStdString(CoreRankedGetLeftJson())).object();
    if (left.isEmpty())
        return;

    QJsonObject report;
    report["matchId"]       = QJsonValue(qint64(m_rankedMatchId));
    report["leftSlot"]      = left.value("leftPort").toInt();
    report["games"]         = rankedGamesForLobby(left.value("games").toArray());
    report["voidedGames"]   = left.value("voidedGames").toInt();
    report["integrity"]     = left.value("integrity").toString();
    report["rulesetHash"]   = left.value("ruleset").toObject().value("hash").toString();
    report["rulesetId"]     = left.value("ruleset").toObject().value("id").toString();
    report["moduleVersion"] = left.value("module").toObject().value("version").toString();
    report["romMd5"]        = left.value("romMd5").toString();
    const QJsonObject live = left.value("live").toObject();
    if (!live.isEmpty())
    {
        QJsonArray players;
        for (const QJsonValue& value : live.value("players").toArray())
        {
            const QJsonObject player = value.toObject();
            QJsonObject entry;
            entry["slot"]    = player.value("port").toInt();
            entry["stocks"]  = player.value("stocksLeft").toInt();
            entry["percent"] = player.value("percent").toInt();
            players.append(entry);
        }
        QJsonObject game;
        game["game"]    = live.value("game").toInt();
        game["inGame"]  = live.value("inGame").toBool();
        game["teams"]   = live.value("teams").toArray();
        game["players"] = players;
        report["live"] = game;
    }
    m_client->reportMatchAbandoned(report);
    m_rankedResultSent = true;
    appendChatSystemLine(CHANNEL_ROOM, tr("A player left the ranked set; the lobby decides the result from the "
                                          "score when they left."));
    finishRankedLeftWait();
}

void RollbackLobbyDialog::finishRankedLeftWait()
{
    if (!m_rankedLeftPending)
        return;
    m_rankedLeftPending = false;
    if (m_closeMatchAfterLeftReport)
    {
        m_closeMatchAfterLeftReport = false;
        emit closeMatchRequested();
    }
}

void RollbackLobbyDialog::onRankedResult(const QJsonObject& result)
{
    const QString status = result.value("status").toString();
    QString line;
    if (status == "rated")
    {
        QStringList changes;
        for (const QJsonValue& value : result.value("players").toArray())
        {
            const QJsonObject player = value.toObject();
            const auto before = LobbyClient::parseStanding(player.value("before").toObject());
            const auto after  = LobbyClient::parseStanding(player.value("after").toObject());
            const int delta = after.rating - before.rating;
            QString change = QString("%1 %2 → %3 (%4%5)")
                .arg(player.value("username").toString())
                .arg(before.rating).arg(after.rating)
                .arg(delta >= 0 ? "+" : "").arg(delta);
            if (!after.provisional && after.tier != before.tier)
                change += tr(", now %1").arg(tierName(after.tier));
            changes << change;
        }
        const QString leaver = result.value("leftUsername").toString();
        line = result.value("reason").toString() == "forfeit" && !leaver.isEmpty()
            ? tr("Ranked set rated: %1 left and forfeits. %2").arg(leaver, changes.join(" · "))
            : tr("Ranked set rated: %1").arg(changes.join(" · "));
    }
    else if (status == "disputed")
    {
        line = tr("Ranked set disputed (%1) — ratings unchanged, flagged for review.")
            .arg(result.value("reason").toString());
    }
    else
    {
        const QString reason = result.value("reason").toString();
        QString why = reason;
        if (reason == "player_left")         why = tr("a player left before the set finished");
        else if (reason == "left_in_game_1") why = tr("a player left before game 1 was over");
        else if (reason == "leader_left")    why = tr("the player who left was ahead");
        else if (reason == "result_timeout") why = tr("a result report never arrived");
        else if (reason == "server_error")   why = tr("a server error");
        line = tr("Ranked set voided: %1. Ratings unchanged.").arg(why);
    }
    appendChatSystemLine(CHANNEL_ROOM, line);
    appendChatSystemLine(CHANNEL_LOBBY, line);

    if (m_rankedSession)
    {
        m_sessionSets++;
        const int winnerSlot = result.value("winnerSlot").toInt();
        const int winnerTeam = result.value("winnerTeam").toInt();
        for (const QJsonValue& value : result.value("players").toArray())
        {
            const QJsonObject player = value.toObject();
            if (player.value("username").toString().compare(m_username, Qt::CaseInsensitive) != 0)
                continue;
            if (status == "rated")
            {
                const bool won = winnerTeam != 0 ? player.value("team").toInt() == winnerTeam
                                                 : player.value("slot").toInt() == winnerSlot;
                (won ? m_sessionWins : m_sessionLosses)++;
                const auto before = LobbyClient::parseStanding(player.value("before").toObject());
                const auto after  = LobbyClient::parseStanding(player.value("after").toObject());
                m_sessionRatingDelta += after.rating - before.rating;
                m_sessionStanding = standingText(after);
            }
        }
        updateSessionUi();
    }

    // Rated and disputed sets keep their replay (voided ones aren't worth it).
    const qint64 setId = result.value("setId").toInteger();
    if (setId > 0 && status != "voided" &&
        static_cast<quint64>(result.value("matchId").toInteger()) == m_replayMatchId)
    {
        m_replaySetId = setId;
        tryOfferRankedReplay();
    }
}

// Removes chat (0x08) records from a .krec: lobby chat is embedded in local
// recordings but must not end up in a public replay. Input (0x12) and drop
// (0x14) records and the ranked metadata trailer are kept byte for byte.
// Returns empty on a malformed file.
static QByteArray stripKrecChat(const QByteArray& krec)
{
    constexpr int headerSize = 400;
    if (krec.size() < headerSize || !krec.startsWith("KRC1"))
        return {};

    size_t trailerStart = 0;
    const bool hasTrailer = KailleraExport::FindKrecMetadata(krec.toStdString(), trailerStart, nullptr);
    QByteArray out = krec.left(headerSize);
    const char* data = krec.constData();
    const int size = hasTrailer ? static_cast<int>(trailerStart) : krec.size();
    int pos = headerSize;
    auto skipString = [&](int& p) {
        while (p < size && data[p] != 0)
            p++;
        if (p >= size)
            return false;
        p++;
        return true;
    };
    while (pos < size)
    {
        const int start = pos;
        const uint8_t type = static_cast<uint8_t>(data[pos++]);
        if (type == 0x12)
        {
            if (pos + 2 > size)
                return {};
            uint16_t length = 0;
            std::memcpy(&length, data + pos, sizeof(length));
            pos += 2 + length;
            if (pos > size)
                return {};
            out.append(data + start, pos - start);
        }
        else if (type == 0x14)
        {
            if (!skipString(pos) || pos + 4 > size)
                return {};
            pos += 4;
            out.append(data + start, pos - start);
        }
        else if (type == 0x08)
        {
            if (!skipString(pos) || !skipString(pos))
                return {};
        }
        else
        {
            return {};
        }
    }
    if (hasTrailer)
        out.append(krec.mid(size));
    return out;
}

void RollbackLobbyDialog::captureRankedReplay()
{
    const QString krecPath = QString::fromStdString(n02::recordingLastPath());
    const std::string metaJson = CoreRankedGetLastReplayJson();
    if (krecPath.isEmpty() || metaJson.empty() || !QFile::exists(krecPath))
        return;
    const QByteArray meta = QByteArray::fromStdString(metaJson);

    // The metadata (bookmarks + the module's write journal) goes inside the
    // .krec itself, so the file plays back on its own, here or downloaded.
    std::string error;
    if (!KailleraExport::AppendKrecMetadata(std::filesystem::path(krecPath.toStdU32String()), metaJson, &error))
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            ("Ranked: failed to add replay data to " + krecPath + ": " +
             QString::fromStdString(error)).toUtf8().constData());

    m_replayKrecPath  = krecPath;
    m_replayBookmarks = QJsonDocument::fromJson(meta).object().value("bookmarks").toArray();
    tryOfferRankedReplay();
}

void RollbackLobbyDialog::tryOfferRankedReplay()
{
    if (!m_client || m_replaySetId <= 0 || m_replayKrecPath.isEmpty())
        return;
    const qint64 setId = m_replaySetId;
    const QString path = m_replayKrecPath;
    m_replaySetId = 0;
    m_replayKrecPath.clear();
    m_replayMatchId = 0;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = stripKrecChat(file.readAll());
    if (bytes.isEmpty())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            ("Ranked: not uploading unreadable replay " + path).toUtf8().constData());
        return;
    }
    m_replayUploadSetId = setId;
    m_replayUploadBytes = bytes;
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    m_client->offerReplay(setId, bytes.size(), sha, m_replayBookmarks);
}

void RollbackLobbyDialog::onReplayAccepted(qint64 setId, int chunkSize)
{
    if (!m_client || setId != m_replayUploadSetId || chunkSize <= 0)
        return;
    const int count = (m_replayUploadBytes.size() + chunkSize - 1) / chunkSize;
    for (int i = 0; i < count; i++)
        m_client->sendReplayChunk(setId, i, count, m_replayUploadBytes.mid(i * chunkSize, chunkSize));
    m_replayUploadBytes.clear();
}

void RollbackLobbyDialog::onReplayDeclined(qint64 setId, const QString& reason)
{
    if (setId != m_replayUploadSetId)
        return;
    m_replayUploadSetId = 0;
    m_replayUploadBytes.clear();
    // "exists" / "in_progress": the opponent's copy is already uploading
    if (reason != "exists" && reason != "in_progress")
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            ("Ranked: replay upload declined: " + reason).toUtf8().constData());
}

void RollbackLobbyDialog::onReplayStored(qint64 setId)
{
    if (setId != m_replayUploadSetId)
        return;
    m_replayUploadSetId = 0;
    appendChatSystemLine(CHANNEL_ROOM, tr("Replay of set #%1 uploaded to the rankings site.").arg(setId));
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

    // The port is empty now. The replay's header has already been written.
    if (slot >= 1 && slot <= 4)
    {
        recording_player_names[slot - 1][0] = '\0';
        emit portLabelsChanged();
    }

    // A ranked set: the core keeps it as of this player leaving (about half a
    // second from now, once that frame is confirmed) and reportRankedPlayerLeft
    // sends it. Closing the match (the room may be closing) waits for that.
    if (slot > 0 && m_rankedMatchId != 0 && !m_rankedResultSent && !m_rankedLeftPending)
    {
        m_rankedLeftPending = true;
        const quint64 rankedMatchId = m_rankedMatchId;
        QTimer::singleShot(3000, this, [this, rankedMatchId]() {
            if (m_rankedMatchId == rankedMatchId)
                finishRankedLeftWait();
        });
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
            if (s.userId == 0 || s.userId == selfId)
                continue;
            m_client->requestPingProbe(s.userId);
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

void RollbackLobbyDialog::refreshSeatMeta(quint64 userId, const QString& statusHtml)
{
    for (auto& s : m_seats)
    {
        if (s.userId != userId || !s.metaLabel)
            continue;
        const bool isSelf = m_client && userId == m_client->selfUserId();
        const int pingMs = isSelf ? 0 : (m_client ? m_client->measuredPingMs(userId) : -1);
        s.metaLabel->setText(
            seatMetaHtml(s.slot, s.isHost, pingMs, s.frameDelay,
                         isDarkTheme(), statusHtml));
        break;
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
        refreshSeatMeta(userId, seatIceFailedStatusHtml());
    // Once ICE connects, probe immediately rather than waiting up to three
    // seconds for the periodic refresh timer.
    else if (m_client->measuredPingMs(userId) < 0)
    {
        for (const auto& seat : m_seats)
        {
            if (seat.userId != userId)
                continue;
            refreshSeatMeta(userId,
                seatIceStatusHtml(seat.iceAttempt, seat.iceMaxAttempts));
            break;
        }
    }
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
        if (seat.userId != userId)
            continue;
        seat.iceMaxAttempts = std::max(1, maxAttempts);
        seat.iceAttempt = std::clamp(attempt, 1, seat.iceMaxAttempts);
        // A fresh ICE generation invalidates the usefulness of any cached ping
        // until this new path connects and is measured.
        refreshSeatMeta(userId,
            seatIceStatusHtml(seat.iceAttempt, seat.iceMaxAttempts));
        break;
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
    refreshSeatMeta(userId, seatProbeStatusHtml(attempt, maxAttempts, false, isDarkTheme()));
}

void RollbackLobbyDialog::onPingProbeSoftFailed(quint64 userId)
{
    // Clear any retry text; with no status the seat renders the cached
    // measurement. The client escalates to onPingProbeFailed if the next
    // series also dies, so a real outage still surfaces within two cycles.
    refreshSeatMeta(userId, QString());
}

void RollbackLobbyDialog::onPingProbeFailed(quint64 userId)
{
    refreshSeatMeta(userId, seatProbeStatusHtml(0, 0, true, isDarkTheme()));

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
                "a direct link. Start stays disabled until it succeeds.")).arg(name));
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
        if (s.userId != userId || !s.metaLabel)
            continue;
        s.metaLabel->setText(
            seatMetaHtml(s.slot, s.isHost, rttMs, s.frameDelay, isDarkTheme()));
        break;
    }

    // Auto delay follows this player's own direct peer RTTs. Every seated client
    // runs this path independently, so their resolved delays may differ.
    if (m_delayAuto)
        applyLocalFrameDelay(false);

    // A peer's first ping may be what unblocks the host's Start button.
    refreshStartButton();

    // Refresh the lobby row so the hover tooltip shows the fresh measurement.
    const auto rowIt = m_userItems.constFind(userId);
    if (rowIt != m_userItems.constEnd())
    {
        const auto& users = m_client->users();
        const auto userIt = users.constFind(userId);
        if (userIt != users.constEnd())
            refreshPlayerRow(rowIt.value(), userIt.value());
    }

    // Rooms hosted by this user show your ping in the Active Rooms list.
    const auto& rooms = m_client->rooms();
    for (auto it = rooms.constBegin(); it != rooms.constEnd(); ++it)
    {
        if (it.value().hostId != userId)
            continue;
        const auto roomRowIt = m_roomItems.constFind(it.key());
        if (roomRowIt != m_roomItems.constEnd())
            refreshRoomRow(roomRowIt.value(), it.value());
    }
}

int RollbackLobbyDialog::worstLocalPeerPingMs() const
{
    if (!m_client) return -1;
    const quint64 selfId = m_client->selfUserId();
    int worst = -1;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0 || s.userId == selfId)
            continue;
        const int ping = m_client->measuredPingMs(s.userId);
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
    if (m_currentRoomId == 0 || !m_delayCombo ||
        (m_currentRoomState != "waiting" && !liveMatch))
        return;
    // Auto settles the delay before the match. Mid-match it resolves only when
    // the player picks it, so a ping change never moves the delay under them.
    if (liveMatch && !force)
        return;

    const int delay = m_delayAuto
        ? autoFrameDelay()
        : m_delayCombo->currentData().toInt();
    if (m_delayAuto)
        setAutoComboLabel(m_delayCombo, delay);
    if (!force && delay == m_currentRoomDelay && m_localDelayPublished)
        return;

    if (liveMatch && delay != m_currentRoomDelay)
    {
        rmgk_gekko::set_live_local_delay(delay);
        appendChatSystemLine(CHANNEL_ROOM, tr("Your frame delay is now %1 f.").arg(delay));
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
            if (seat.userId != selfId)
                continue;
            seat.frameDelay = delay;
            refreshSeatMeta(selfId, QString());
            break;
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
        !m_predictionCombo || !m_client ||
        m_currentRoomHostId != m_client->selfUserId())
        return;

    const int prediction = m_predictionAuto
        ? kDefaultPredictionWindow
        : m_predictionCombo->currentData().toInt();
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
    if (!m_delayCombo || !m_predictionCombo || !m_client)
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
    m_delayCombo->setEnabled(delayEditable);
    m_predictionCombo->setEnabled(predictionOpen && iAmHost);

    m_delayCombo->setToolTip(delayEditable
        ? m_delayCombo->property("originalTip").toString()
        : tipDisabledMidMatch);
    m_predictionCombo->setToolTip(!predictionOpen
        ? tipDisabledMidMatch
        : (iAmHost ? m_predictionCombo->property("originalTip").toString()
                   : tipHostControlled));
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
    m_outOfMatchUsers.clear();
    m_lateJoinCapableUsers.clear();
    m_admitRequestedUserId = 0;
    m_announcedWaitingSelf = false;
    if (m_emulationActive || m_awaitingEmulationStart)
    {
        if (m_rankedLeftPending)
            m_closeMatchAfterLeftReport = true; // after the left report
        else
            emit closeMatchRequested();
    }

    for (auto& s : m_seats)
        s.userId = 0;

    m_currentRoomId = 0;
    m_currentRoomGame.clear();
    m_currentRoomRegion.clear();
    m_currentRoomState.clear();
    m_currentRoomDelay = 2;
    m_currentRoomPrediction = kDefaultPredictionWindow;
    m_currentRoomPacing = 0;
    m_currentRoomHostId = 0;
    m_currentMatchId = 0;
    emit liveReplayViewerCountCleared();
    m_iceWaitingMatchId = 0;
    m_iceMatchDeadlineMs = 0;
    m_awaitingEmulationStart = false;
    m_emulationActive = false;
    endLiveRollbackSettings();
    m_knownSeatedUsers.clear();
    m_reportedPeerPings.clear();
    m_roomSeatsSeen = false;
    m_localDelayPublished = false;

    if (m_chatViewRoom) m_chatViewRoom->clear();
    if (m_roomChatInput) m_roomChatInput->setEnabled(false);

    switchToRoomsView();
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn)  m_dropBtn->setEnabled(false);
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    updateHeroButtons();

    updateInRoomBanner();
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
    else if (reason == QLatin1String("quick_match_next"))
        appendChatSystemLine(CHANNEL_LOBBY, m_rankedSession
            ? tr("Set over — searching for your next ranked match.")
            : m_casualSession ? tr("Match over — searching for your next match.")
                              : tr("The match is over and the room closed."));
    // "quick_match_cancelled": a player left the warmup; the lobby says so and
    // searches again.
    updateSessionUi();
}

// ──────────────────────────────────────────────────────────────────────
//  Chat
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onChatSendClicked()
{
    const QString text = m_chatInput->text().trimmed();
    if (text.isEmpty()) return;
    const bool toSpectators = m_spectatingMatchId != 0 &&
                              m_chatTabs->currentIndex() == kChatTabSpectators;
    const char* const channel = toSpectators ? CHANNEL_SPECTATE : CHANNEL_LOBBY;
    if (handleSlashCommand(channel, text)) { m_chatInput->clear(); return; }
    m_client->sendChat(channel, text);
    m_chatInput->clear();
}

void RollbackLobbyDialog::onRoomChatSendClicked()
{
    if (!m_roomChatInput) return;
    const QString text = m_roomChatInput->text().trimmed();
    if (text.isEmpty()) return;
    if (handleSlashCommand(CHANNEL_ROOM, text)) { m_roomChatInput->clear(); return; }
    m_client->sendChat(CHANNEL_ROOM, text);
    m_roomChatInput->clear();
}

void RollbackLobbyDialog::onChatMessageReceived(const LobbyClient::ChatMessage& msg)
{
    if (msg.channel == CHANNEL_SPECTATE)
    {
        // Chat among the viewers of the live replay we're watching, shown in
        // the Spectators tab. Lines that arrive after we stopped watching are
        // dropped.
        if (m_spectatingMatchId == 0)
            return;
        const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
        appendChatLine(CHANNEL_SPECTATE, QString("[%1] <b>%2:</b> %3")
            .arg(ts, msg.fromUsername.toHtmlEscaped(), msg.message.toHtmlEscaped()));
        if (m_chatTabs->currentIndex() != kChatTabSpectators)
            m_chatTabs->setTabText(kChatTabSpectators, tr("Spectators •"));
        emit spectatorChatReceived(msg.fromUsername, msg.message);
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

    const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
    QString line = QString("[%1] <b>%2:</b> %3")
        .arg(ts, msg.fromUsername.toHtmlEscaped(), displayMessage.toHtmlEscaped());
    if (notifyPing)
    {
        // Full-width bar via a one-cell table (QTextEdit's rich-text subset
        // doesn't background-fill plain blocks). Fixed black-on-light-yellow
        // so the highlight reads the same in light and dark themes.
        line = QString("<table width=\"100%\" cellpadding=\"3\" style=\"background-color:#fff3b0;\">"
                       "<tr><td style=\"color:#000000;\">%1</td></tr></table>").arg(line);
    }
    appendChatLine(msg.channel, line);

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
    if (!m_client || m_spectatingMatchId == 0)
        return;

    const QString text = message.trimmed();
    if (text.isEmpty())
        return;

    m_client->sendChat(CHANNEL_SPECTATE, text);
}

void RollbackLobbyDialog::appendChatLine(const QString& channel, const QString& text)
{
    QTextEdit* target = m_chatViewLobby;
    if (channel == CHANNEL_ROOM && m_chatViewRoom)
        target = m_chatViewRoom;
    else if (channel == CHANNEL_SPECTATE && m_chatViewSpectate)
        target = m_chatViewSpectate;
    if (target) target->append(text);
}

void RollbackLobbyDialog::appendChatSystemLine(const QString& channel, const QString& text)
{
    const auto ts = QDateTime::currentDateTime().toString("hh:mm");
    appendChatLine(channel, QString("[%1] <i>%2</i>").arg(ts, text));
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

    if (cmd == "/modhelp")
    {
        sysline("Moderator commands: /login, /kick &lt;user&gt; [reason], "
                "/mute &lt;user&gt; [dur] [reason], /timeout &lt;user&gt; &lt;dur&gt; [reason], "
                "/ban &lt;user&gt; [reason], /unban &lt;ip&gt;, /unmute &lt;ip&gt;, /modlist. "
                "Durations: 10m, 1h, 2d (blank = permanent). "
                "Ranked: /disputes, /resolve &lt;setId&gt; &lt;winner|void&gt; [note], "
                "/pwreset &lt;user&gt; (one-time code to set a new password).");
        return true;
    }

    static const QStringList modCmds = {"/kick", "/mute", "/timeout", "/ban", "/unban", "/unmute", "/modlist",
                                        "/disputes", "/resolve", "/pwreset"};
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

    // ranked sets and accounts
    if (action == "disputes")
    {
        m_client->sendModAction("disputes", QString());
        return true;
    }
    if (action == "resolve")
    {
        if (parts.size() < 3)
        {
            sysline(tr("Usage: /resolve &lt;setId&gt; &lt;winner username|void&gt; [note]"));
            return true;
        }
        m_client->sendModAction("resolve", parts[1] + " " + parts[2], QString(),
            parts.size() > 3 ? parts.mid(3).join(' ') : QString());
        return true;
    }
    if (action == "pwreset")
    {
        if (parts.size() < 2) { sysline(tr("Usage: /pwreset &lt;username&gt;")); return true; }
        m_client->sendModAction("pwreset", parts[1]);
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
        const QString line = tr("✓ Moderator access granted (%1). Type /modhelp for commands.")
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

void RollbackLobbyDialog::onQuickRankedMatchClicked()
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
        return;

    if (m_quickMatchActive)
    {
        m_client->quickMatchCancel();
        return;
    }
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, tr("Already in a room"),
            tr("Leave your current room before searching for a match."));
        return;
    }

    const QVariantMap romData = selectedBrowseRom();
    const QString romName = romData.value("name").toString();
    const QString romMd5  = romData.value("md5").toString();
    if (romMd5.isEmpty())
    {
        QMessageBox::information(this, tr("Select a game"),
            tr("Pick a game from the dropdown before searching for a ranked match."));
        return;
    }
    const CreateRoomRankedOptions options = rankedOptionsFor(romMd5);
    if (!options.available || options.formats.isEmpty())
    {
        const QString reason = options.unavailableReason.isEmpty()
            ? tr("this server doesn't run ranked play") : options.unavailableReason;
        QMessageBox::information(this, tr("Ranked unavailable"),
            tr("Can't search for a ranked match: %1.").arg(reason));
        return;
    }

    auto* dialog = new QuickRankedMatchDialog(romName, options, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QStringList players;
    for (const LobbyClient::LobbyUser& user : m_client->users())
    {
        if (user.verified && user.id != m_client->selfUserId())
            players.append(user.username);
    }
    players.sort(Qt::CaseInsensitive);
    dialog->setTeammateSuggestions(players);
    connect(dialog, &QDialog::accepted, this, [this, dialog, romName, romMd5]() {
        // the lobby may have changed while the dialog was open
        if (m_client->state() != LobbyClient::ConnectionState::Connected || m_currentRoomId != 0 || m_quickMatchActive)
            return;
        QJsonObject ranked;
        ranked["enabled"]          = true;
        ranked["ladderId"]         = dialog->ladderId();
        ranked["rulesetId"]        = dialog->rulesetId();
        ranked["maxPing"]          = dialog->maxPing();
        qInfo() << "lobby: ranked quick match search" << "game" << romName << "ladder" << dialog->ladderId()
                << "ruleset" << dialog->rulesetId() << "maxPing" << dialog->maxPing();
        m_client->quickMatchJoin(romName, romMd5, ranked, dialog->keepTeammate(), dialog->teammate());
    });
    dialog->open();
}

void RollbackLobbyDialog::resetQuickMatchButtons()
{
    m_quickMatchActive = false;
    m_quickMatchRanked = false;
    if (m_quickMatchBtn)  m_quickMatchBtn->setText(QStringLiteral("⚡  ") + tr("Quick Match"));
    if (m_quickRankedBtn) m_quickRankedBtn->setText(QStringLiteral("🏆  ") + tr("Quick Ranked Match"));
}

void RollbackLobbyDialog::updateHeroButtons()
{
    if (!m_client)
        return;
    const bool idle = m_client->state() == LobbyClient::ConnectionState::Connected && m_currentRoomId == 0;
    // while searching only that search's button (now its cancel) stays usable
    if (m_quickMatchBtn)  m_quickMatchBtn->setEnabled(idle && (!m_quickMatchActive || !m_quickMatchRanked));
    if (m_quickRankedBtn) m_quickRankedBtn->setEnabled(idle && (!m_quickMatchActive || m_quickMatchRanked));
    if (m_createRoomBtn)  m_createRoomBtn->setEnabled(idle && !m_quickMatchActive);

    // Ranked UI only shows for a game with a ranked ladder; Quick Match takes
    // the row otherwise. A running ranked search keeps its (cancel) button.
    if (m_quickRankedBtn)
    {
        const QString md5 = selectedBrowseRom().value("md5").toString();
        const bool rankedGame = !md5.isEmpty() && !m_client->laddersForRomMd5(md5).isEmpty();
        m_quickRankedBtn->setVisible(rankedGame || (m_quickMatchActive && m_quickMatchRanked));
    }
}

void RollbackLobbyDialog::onQuickMatchStatusChanged(bool searching, int queueSize, bool ranked, bool continuous,
                                                    bool keepTeammate, const QString& teammate,
                                                    const QString& waitingFor)
{
    resetQuickMatchButtons();
    m_quickMatchActive = searching;
    m_quickMatchRanked = searching && ranked;
    if (ranked)
    {
        const bool session = continuous;
        if (session && !m_rankedSession)
        {
            m_sessionSets = m_sessionWins = m_sessionLosses = m_sessionRatingDelta = 0;
            m_sessionStanding.clear();
        }
        if (!session && m_rankedSession && m_currentRoomId != 0)
            appendChatSystemLine(CHANNEL_ROOM, tr("Your ranked session ends after this set."));
        m_rankedSession = session;
        if (session)
            m_sessionCasual = false;
        m_sessionKeepTeammate = session && keepTeammate;
        m_sessionTeammate     = session ? teammate : QString();
        m_sessionWaitingFor   = searching ? waitingFor : QString();
    }
    else
    {
        const bool session = continuous;
        if (session && !m_casualSession)
        {
            m_sessionMatches = 0;
            m_sessionCasual = true;
        }
        if (!session && m_casualSession && m_currentRoomId != 0)
            appendChatSystemLine(CHANNEL_ROOM, tr("Your quick match session ends after this match."));
        m_casualSession = session;
    }
    if (!searching)
        closeRematchPrompt();

    if (searching)
    {
        QPushButton* button = ranked ? m_quickRankedBtn : m_quickMatchBtn;
        const QString suffix = !waitingFor.isEmpty() ? tr(" (waiting for %1)").arg(waitingFor)
            : queueSize > 1 ? tr(" (%1 in queue)").arg(queueSize)
            : tr(" (searching…)");
        QString label = QStringLiteral("✕  ") + (ranked ? tr("Cancel Ranked Search") : tr("Cancel Quick Match"));
        if (continuous)
            label = QStringLiteral("✕  ") + (ranked ? tr("Stop Ranked Session") : tr("Stop Quick Match Session"));
        if (button)
            button->setText(label + suffix);
    }
    updateHeroButtons();
    updateSessionUi();
}

void RollbackLobbyDialog::onQuickMatchRematchOffered(const QStringList& opponents)
{
    closeRematchPrompt();
    const QString names = opponents.join(", ");
    auto* box = new QMessageBox(QMessageBox::Question, tr("Rematch?"),
        opponents.size() == 1
            ? tr("No other players except your previous opponent, %1, are available.").arg(names)
            : tr("No other players except your previous opponents, %1, are available.").arg(names),
        QMessageBox::NoButton, this);
    box->setObjectName("RematchPrompt");
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* play = box->addButton(tr("Play %1").arg(names), QMessageBox::AcceptRole);
    play->setObjectName("RematchPlayBtn");
    QPushButton* keep = box->addButton(tr("Keep Searching"), QMessageBox::RejectRole);
    keep->setObjectName("RematchKeepSearchingBtn");
    box->setDefaultButton(keep);
    connect(box, &QMessageBox::buttonClicked, this, [this, play](QAbstractButton* button) {
        m_client->quickMatchRematch(button == play);
    });
    m_rematchPrompt = box;
    box->open();
    QApplication::alert(this);
}

void RollbackLobbyDialog::closeRematchPrompt()
{
    if (!m_rematchPrompt)
        return;
    // closing without an answer sends nothing; the lobby asks again later
    m_rematchPrompt->disconnect(this);
    m_rematchPrompt->close();
    m_rematchPrompt = nullptr;
}

void RollbackLobbyDialog::onStopSessionClicked()
{
    if (!m_rankedSession && !m_casualSession)
        return;
    m_client->quickMatchSetContinuous(false);
    if (m_stopSessionBtn)
        m_stopSessionBtn->setEnabled(false);
}

void RollbackLobbyDialog::onKeepTeammateToggled(bool keep)
{
    if (!m_rankedSession)
        return;
    m_sessionKeepTeammate = keep;
    m_client->quickMatchSetKeepTeammate(keep);
}

void RollbackLobbyDialog::updateSessionUi()
{
    if (m_sessionLabel)
    {
        QString text;
        if (m_sessionCasual && (m_casualSession || m_sessionMatches > 0))
        {
            text = m_casualSession ? tr("Quick match session") : tr("Last quick match session");
            text += m_sessionMatches == 0 ? tr(" · searching for your first match")
                  : m_sessionMatches == 1 ? tr(" · 1 match")
                                          : tr(" · %1 matches").arg(m_sessionMatches);
        }
        else if (!m_sessionCasual && (m_rankedSession || m_sessionSets > 0))
        {
            text = m_rankedSession ? tr("Ranked session") : tr("Last ranked session");
            if (m_rankedSession && !m_sessionTeammate.isEmpty())
                text += tr(" with %1").arg(m_sessionTeammate);
            else if (m_rankedSession && !m_sessionWaitingFor.isEmpty())
                text += tr(" · waiting for %1 to pick you as their teammate").arg(m_sessionWaitingFor);
            if (m_sessionSets > 0)
            {
                text += (m_sessionSets == 1 ? tr(" · 1 set") : tr(" · %1 sets").arg(m_sessionSets))
                      + QString(" · %1–%2").arg(m_sessionWins).arg(m_sessionLosses);
                if (!m_sessionStanding.isEmpty())
                    text += QString(" · %1 (%2%3)").arg(m_sessionStanding)
                                .arg(m_sessionRatingDelta >= 0 ? "+" : "").arg(m_sessionRatingDelta);
            }
            else
            {
                text += tr(" · searching for your first set");
            }
        }
        m_sessionLabel->setText(text);
        m_sessionLabel->setVisible(!text.isEmpty());
    }
    if (m_stopSessionBtn)
    {
        const bool show = (m_rankedSession || m_casualSession) && m_currentRoomId != 0;
        m_stopSessionBtn->setText(m_casualSession ? tr("Stop After This Match") : tr("Stop After This Set"));
        m_stopSessionBtn->setVisible(show);
        if (show)
            m_stopSessionBtn->setEnabled(true);
    }
    if (m_keepTeammateCheck)
    {
        const QSignalBlocker blocker(m_keepTeammateCheck);
        m_keepTeammateCheck->setChecked(m_sessionKeepTeammate);
        m_keepTeammateCheck->setVisible(keepTeammateApplies());
    }
}

bool RollbackLobbyDialog::keepTeammateApplies() const
{
    return m_rankedSession && m_currentRoomId != 0 && m_currentRoomRanked.value("teamSize").toInt() > 1;
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

    // Ranked: the module's write journal goes ahead of the inputs it belongs
    // with, spectators can't stay in sync without it.
    std::string journal;
    while (CoreRankedTakeJournalUpdate(journal, 256 * 1024))
    {
        m_client->sendBroadcastJournal(m_broadcastMatchId, QByteArray::fromStdString(journal));
    }

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
    if (m_spectatingMatchId != 0) return; // already spectating

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

    m_spectatingMatchId = matchId;
    m_spectateStreamArmed = false; // wait for this subscribe's SPECTATE_BEGIN before accepting data
    m_client->startSpectate(matchId);
    m_chatViewSpectate->clear();
    setSpectatorChatVisible(true);
    emit spectateLaunch(matchId, gameName, romPath);
    appendChatSystemLine(CHANNEL_SPECTATE, tr("Connecting to live replay…"));
}

void RollbackLobbyDialog::stopSpectating()
{
    if (m_spectatingMatchId == 0) return;
    m_client->stopSpectate(m_spectatingMatchId);
    m_spectatingMatchId = 0;
    m_spectateStreamArmed = false;
    setSpectatorChatVisible(false);
    emit liveReplayViewerCountCleared();
}

void RollbackLobbyDialog::setSpectatorChatVisible(bool visible)
{
    m_chatHeader->setText(visible ? tr("CHAT") : tr("LOBBY CHAT"));
    m_chatTabs->setTabText(kChatTabSpectators, tr("Spectators"));
    m_chatTabs->setCurrentIndex(visible ? kChatTabSpectators : kChatTabLobby);
    m_chatTabs->setVisible(visible);
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

void RollbackLobbyDialog::onSpectateJournal(quint64 matchId, int seq, const QByteArray& journal)
{
    if (matchId != m_spectatingMatchId) return;
    if (!m_spectateStreamArmed) return; // stale chunk from a previous watch
    emit spectateStreamJournal(seq, journal);
}

void RollbackLobbyDialog::onSpectateEnded(quint64 matchId, const QString& reason)
{
    if (matchId != m_spectatingMatchId) return;
    m_spectatingMatchId = 0; // server already ended it; don't echo SPECTATE_STOP
    setSpectatorChatVisible(false);
    emit liveReplayViewerCountCleared();
    emit spectateStreamClosed(reason);
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
}

void RollbackLobbyDialog::onBroadcastViewerCount(quint64 matchId, int viewerCount)
{
    // Refresh only the affected label so audience changes preserve selection.
    for (int i = 0; i < m_matchesTree->topLevelItemCount(); ++i)
    {
        auto* row = m_matchesTree->topLevelItem(i);
        if (row->data(0, Qt::UserRole + 1).toULongLong() != matchId) continue;
        const auto room = m_client->rooms().constFind(row->data(0, Qt::UserRole).toULongLong());
        if (room != m_client->rooms().constEnd())
        {
            row->setText(3, matchViewersLabel(room.value()));
            row->setData(3, Qt::UserRole, room->broadcasting && room->matchId != 0 ? room->viewerCount : -1);
        }
        break;
    }
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

}

void RollbackLobbyDialog::onMatchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers)
{
    QString iceError;
    if (!m_client->allIcePeersConnected(peers, &iceError))
    {
        const bool unsupported = std::any_of(peers.cbegin(), peers.cend(), [this](const auto& peer) {
            return peer.userId != m_client->selfUserId() && !peer.iceSupported;
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

    m_playingAlone = peers.size() == 1;
    if (m_playingAlone && m_delayAuto && m_currentRoomDelay != 0)
    {
        m_currentRoomDelay = 0;
        setAutoComboLabel(m_delayCombo, 0);
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
    if (m_playingAlone)
        appendChatSystemLine(CHANNEL_ROOM,
            tr("You're playing alone for now. When someone joins the room, click Add to Game on their seat."));

    m_currentMatchId = matchId;
    m_awaitingEmulationStart = true;
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
                const QByteArray nameBytes = p.username.toUtf8();
                std::strncpy(recording_player_names[p.slot - 1], nameBytes.constData(), 31);
                recording_player_names[p.slot - 1][31] = '\0';
            }
        }
    }

    const quint16 localPort = 0;

    for (const auto& p : peers)
    {
        if (p.userId == selfId)
            continue;

        const QString selectedLine = QString("Rollback lobby selected ICE peer: peerUser=%1 slot=%2")
            .arg(p.userId).arg(p.slot);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, selectedLine.toUtf8().constData());
        remotePeers << QString("%1,ice,%2").arg(p.slot).arg(p.userId);
    }

    // Nobody else is fine only in a match someone can join later.
    if (remotePeers.isEmpty() && m_client->matchSeats() == 0)
    {
        abortMatchStart(tr("Match start failed: missing remote peer."));
        return;
    }
    // A match open to late joiners emulates every seat the room has, so a
    // newcomer takes over an idle controller instead of changing the machine.
    if (m_client->matchSeats() > 0)
        remotePeers << QString("seats=%1").arg(m_client->matchSeats());

    const QString localRomFile = localRomPathForMd5(m_currentRoomMd5);
    if (localRomFile.isEmpty())
    {
        abortMatchStart(QString(tr("Match start failed: you don't have the ROM for %1.")).arg(m_currentRoomGame));
        return;
    }

    appendChatSystemLine(CHANNEL_ROOM, tr("Synchronizing pre-match settings..."));
    QString prematchError;
    if (!m_client->syncPrematchManifest(peers, local.slot, m_currentRoomHostId,
                                        localRomFile, prematchError))
    {
        abortMatchStart(prematchError.isEmpty() ? tr("Pre-match sync failed.") : prematchError);
        return;
    }
    appendChatSystemLine(CHANNEL_ROOM, tr("Pre-match sync complete."));

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

    // Ranked sets: start the game module with the server's settings. Every
    // peer stages the same module, ruleset and set length from MATCH_BEGIN.
    m_rankedMatchId = 0;
    m_rankedResultSent = false;
    m_rankedLeftPending = false;
    m_closeMatchAfterLeftReport = false;
    const QJsonObject ranked = m_client->matchRankedInfo(matchId);
    if (!ranked.isEmpty())
    {
        CoreRankedSessionSettings settings;
        settings.ModuleId  = ranked.value("moduleId").toString().toStdString();
        settings.RulesetId = ranked.value("rulesetId").toString().toStdString();
        settings.BestOf    = ranked.value("bestOf").toInt();
        settings.PlayerNames.assign(4, std::string());
        settings.Teams.assign(4, 0);
        for (const QJsonValue& value : ranked.value("players").toArray())
        {
            const QJsonObject player = value.toObject();
            const int slot = player.value("slot").toInt();
            if (slot < 1 || slot > 4)
                continue;
            settings.Ports.push_back(slot);
            settings.PlayerNames[slot - 1] = player.value("username").toString().toStdString();
            // a chosen or kept team: the module holds the set to it
            settings.Teams[slot - 1] = player.value("team").toInt();
        }
        if (!CoreRankedStageSession(settings))
        {
            abortMatchStart(tr("Match start failed: %1")
                .arg(QString::fromStdString(CoreGetError())));
            return;
        }
        m_rankedMatchId = matchId;
        // ranked sets are always recorded: the .krec becomes the set's replay
        n02_kaillera_recording_enabled = true;
        m_replayMatchId = matchId;
        m_replaySetId = 0;
        m_replayKrecPath.clear();
        QString rulesetName = ranked.value("rulesetId").toString();
        if (const auto* ladder = m_client->ladderForRomMd5(m_currentRoomMd5))
            rulesetName = ladder->rulesetNames.value(rulesetName, rulesetName);
        appendChatSystemLine(CHANNEL_ROOM, tr("Ranked set: best of %1, %2.")
            .arg(settings.BestOf).arg(rulesetName));
    }

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
        why = tr("they're on a different RMG-K version");
    else if (reason == QStringLiteral("admit_busy"))
        why = tr("another player is joining right now");
    else if (reason == QStringLiteral("late_join_unsupported"))
        why = tr("this match or their RMG-K can't add players mid-game");
    else if (reason == QStringLiteral("not_waiting"))
        why = tr("they're not waiting to join any more");
    else
        why = tr("something went wrong (%1)").arg(reason);
    appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't add %1: %2.").arg(name, why));
    refreshAdmitButtons();
}

void RollbackLobbyDialog::onMatchAdmitBegin(quint64 matchId, quint64 admitId, int epoch, int seats,
                                            quint64 hostId, const LobbyClient::LobbyMatchPeer& joiner,
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
    m_lateJoin.peers = peers;
    m_lateJoin.startedMs = QDateTime::currentMSecsSinceEpoch();
    m_admitRequestedUserId = 0;

    if (joiner.userId == selfId)
    {
        m_lateJoin.role = LateJoinRole::Joiner;
        // Watching the match? The game is about to take over the window.
        if (m_spectatingMatchId != 0)
            stopSpectating();
        for (const auto& peer : peers)
            m_client->ensureIcePeer(peer.userId);
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
            appendChatSystemLine(CHANNEL_ROOM, tr("The host is adding %1 to the game…").arg(joiner.username));
        }
        emit overlayMessage(tr("%1 is joining the game…").arg(joiner.username));
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
            emit overlayCenterMessage(tr("%1 is joining the game").arg(m_lateJoin.joiner.username));
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
        if (raw.isEmpty() || !m_client->applySpectateManifest(raw))
        {
            abortLateJoin(QStringLiteral("manifest_failed"), true);
            return;
        }
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
            const QByteArray nameBytes = peer.username.toUtf8();
            std::strncpy(recording_player_names[peer.slot - 1], nameBytes.constData(), 31);
            recording_player_names[peer.slot - 1][31] = '\0';
        }
        if (peer.userId != selfId)
            remotePeers << QString("%1,ice,%2").arg(peer.slot).arg(peer.userId);
    }
    remotePeers << QString("seats=%1").arg(m_lateJoin.seats)
                << QString("join=%1").arg(m_lateJoin.epoch);

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
        if (peer.userId == selfId || m_lateJoin.left.contains(peer.userId))
            continue;
        LobbyRemotePeer remote;
        remote.slot = peer.slot;
        remote.userId = peer.userId;
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
        if (peer.userId != m_client->selfUserId() && peer.userId != m_lateJoin.joiner.userId &&
            !m_lateJoin.left.contains(peer.userId))
            m_lateJoin.awaitingCut.insert(peer.userId);
    }
    rmgk_gekko::arm_admit_cut(m_lateJoin.cutFrame, true);
    m_client->sendAdmitStep(m_lateJoin.matchId, m_lateJoin.admitId, QStringLiteral("cut"), m_lateJoin.cutFrame);
    emit overlayCenterMessage(tr("%1 is joining the game").arg(m_lateJoin.joiner.username));
}

// The joiner checks its side before saying it's ready, but a new session that
// starts with either side unconnected holds everyone until someone gives up.
bool RollbackLobbyDialog::connectedToLateJoiner() const
{
    return m_client && m_client->allIcePeersConnected({ m_lateJoin.joiner });
}

void RollbackLobbyDialog::applyLateJoinRoster()
{
    // Label the joiner's port in the overlay (and, on the host, in nothing
    // else: the replay header was written when the match began).
    const int slot = m_lateJoin.joiner.slot;
    if (slot >= 1 && slot <= 4)
    {
        const QByteArray nameBytes = m_lateJoin.joiner.username.toUtf8();
        std::strncpy(recording_player_names[slot - 1], nameBytes.constData(), 31);
        recording_player_names[slot - 1][31] = '\0';
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
            if (nowMs - lj.startedMs > kLateJoinReadyTimeoutMs)
                abortLateJoin(QStringLiteral("manifest_timeout"), true);
            return;
        }
        if (!lj.readySent)
        {
            QList<LobbyClient::LobbyMatchPeer> others;
            for (const auto& peer : lj.peers)
            {
                if (peer.userId != m_client->selfUserId() && !lj.left.contains(peer.userId))
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
    const QString name = m_lateJoin.joiner.username;
    const int slot = m_lateJoin.joiner.slot;
    m_lateJoin = LateJoin{};
    if (m_lateJoinTimer)
        m_lateJoinTimer->stop();
    emit overlayCenterMessage(QString());

    if (role == LateJoinRole::Joiner)
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("You joined the game as P%1.").arg(slot));
        emit overlayMessage(tr("You joined the game as P%1").arg(slot));
    }
    else if (resumed)
    {
        appendChatSystemLine(CHANNEL_ROOM, tr("%1 joined the game as P%2.").arg(name).arg(slot));
        emit overlayMessage(tr("%1 joined the game as P%2").arg(name).arg(slot));
    }
    else
    {
        // The new session is still waiting for the joiner; if they never
        // connect, the lobby drops them and play continues without them.
        appendChatSystemLine(CHANNEL_ROOM, tr("Still waiting for %1 to connect…").arg(name));
    }
    refreshAdmitButtons();
}

void RollbackLobbyDialog::abortLateJoin(const QString& reason, bool tellOthers)
{
    const LateJoin lj = m_lateJoin;
    if (lj.role == LateJoinRole::None)
        return;
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
        appendChatSystemLine(CHANNEL_ROOM, tr("Couldn't add %1: %2.").arg(lj.joiner.username, why));
        emit overlayMessage(tr("%1 couldn't join").arg(lj.joiner.username));
    }
    refreshAdmitButtons();
}

void RollbackLobbyDialog::refreshAdmitButtons()
{
    const bool iAmHost = m_client && m_currentRoomHostId == m_client->selfUserId();
    const bool canAdmit = iAmHost && m_roomLateJoin && m_currentRoomState == "in_game" &&
        m_emulationActive && m_currentMatchId != 0;
    const bool busy = m_lateJoin.role != LateJoinRole::None || m_admitRequestedUserId != 0;
    for (auto& seat : m_seats)
    {
        if (!seat.admitButton)
            continue;
        const bool waiting = seat.userId != 0 && m_outOfMatchUsers.contains(seat.userId) &&
            m_lateJoinCapableUsers.contains(seat.userId);
        const bool joiningThis = m_admitRequestedUserId == seat.userId ||
            (m_lateJoin.role != LateJoinRole::None && m_lateJoin.joiner.userId == seat.userId);
        seat.admitButton->setVisible(canAdmit && waiting);
        seat.admitButton->setEnabled(!busy);
        seat.admitButton->setText(joiningThis ? tr("Adding…") : tr("Add to Game"));
        seat.admitButton->setToolTip(busy && !joiningThis
            ? tr("Another player is joining right now.")
            : tr("Add this player to the running game. Everyone pauses for a moment while they load in."));
    }
}
