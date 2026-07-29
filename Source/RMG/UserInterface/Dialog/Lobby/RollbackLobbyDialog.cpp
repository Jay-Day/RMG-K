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
#include "CreateRoomDialog.hpp"
#include "LobbyRegions.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/rmgk_gekko.hpp> // request_disconnect_player (instant peer drop)

#include "n02_client.h" // setRecordingStreamSink (broadcast tee)

#include <QRegularExpression>
#include <QRegularExpressionValidator>

#include <QApplication>
#include <QPalette>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTextEdit>
#include <QLineEdit>
#include <QJsonArray>
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
// Ping < 0 (unknown) falls back to the default. Never returns 0 — the dropdown
// hides zero delay (zero-delay rollback has open bugs; see project memory).
int autoFrameDelayForPing(int ping)
{
    if (ping < 0)    return 2;   // default when ping is unknown
    if (ping <= 50)  return 1;
    if (ping <= 100) return 2;
    if (ping <= 150) return 3;
    if (ping <= 220) return 4;
    return 5;
}

// Auto prediction is a fixed value so users don't have to think about it.
constexpr int kAutoPredictionWindow = 7;

// Show the resolved value on the "Auto" entry, e.g. "Auto (3 f)".
void setAutoComboLabel(QComboBox* combo, int resolved)
{
    if (!combo) return;
    const int idx = combo->findData(0);
    if (idx >= 0) combo->setItemText(idx, QStringLiteral("Auto (%1 f)").arg(resolved));
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
            case LobbyClient::ConnectionState::Disconnected:   return "Offline";
            case LobbyClient::ConnectionState::Connecting:     return "Connecting";
            case LobbyClient::ConnectionState::Authenticating: return "Authenticating";
            case LobbyClient::ConnectionState::Connected:      return "Online";
            case LobbyClient::ConnectionState::Failed:         return "Connection failed";
        }
        return "Unknown";
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
    // Distinct seat colors so P1-P4 read apart at a glance (P1 blue,
    // P2 red, P3 green, P4 amber — a Smash/Mario-Kart-style set). Light
    // and dark variants keep contrast on both themes; brightness mirrors
    // the way statusColors() picks Material 500 on dark / 800 on light.
    QString playerAccentHex(int slot, bool dark)
    {
        switch (slot)
        {
            case 1: return dark ? "#4aa3ff" : "#0078D7"; // blue
            case 2: return dark ? "#ff6b66" : "#d83a34"; // red
            case 3: return dark ? "#4caf50" : "#2e9e3f"; // green
            case 4: return dark ? "#ffca45" : "#b5790a"; // amber
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
            case 1: c = dark ? RGB{74, 163, 255} : RGB{0, 120, 215};  break;
            case 2: c = dark ? RGB{255, 107, 102} : RGB{216, 58, 52}; break;
            case 3: c = dark ? RGB{76, 175, 80} : RGB{46, 158, 63};   break;
            case 4: c = dark ? RGB{255, 202, 69} : RGB{181, 121, 10}; break;
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
            case 1: c = dark ? RGB{74, 163, 255} : RGB{0, 120, 215};  break;
            case 2: c = dark ? RGB{255, 107, 102} : RGB{216, 58, 52}; break;
            case 3: c = dark ? RGB{76, 175, 80} : RGB{46, 158, 63};   break;
            case 4: c = dark ? RGB{255, 202, 69} : RGB{181, 121, 10}; break;
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

    // Friendly label for an authoritative *room* state. Distinct from
    // stateGlyph(), which maps per-user presence states ("playing", "in_room",
    // …); the room state machine uses a different vocabulary ("waiting",
    // "starting", "in_game", "finished") that stateGlyph would pass through raw.
    QString roomStateLabel(const QString& state)
    {
        if (state == "waiting")  return "Waiting";
        if (state == "starting") return "Starting";
        if (state == "in_game")  return "In Game";
        if (state == "finished") return "Finished";
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

    // Right-aligned seat meta as rich text: an accent-colored HOST badge and a
    // ping-tier-colored "N ms". Shared by renderSeatFilled and the live ping
    // refresh in onPingMeasured so the two never drift. pingMs < 0 hides ping.
    QString seatMetaHtml(int slot, bool isHost, int pingMs, bool dark)
    {
        QStringList parts;
        if (isHost)
            parts << QString("<span style='color:%1; font-weight:700;'>HOST</span>")
                         .arg(playerAccentHex(slot, dark));
        if (pingMs >= 0)
            parts << QString("<span style='color:%1; font-weight:600;'>%2 ms</span>")
                         .arg(pingHex(pingMs)).arg(pingMs);
        return parts.join(QStringLiteral("&nbsp;·&nbsp;"));
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
    setWindowTitle("RMG-K Lobby");
    setWindowModality(Qt::NonModal);
    resize(1180, 720);
    setObjectName("RollbackLobbyDialog");

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
    connect(m_client, &LobbyClient::chatMessageReceived,  this, &RollbackLobbyDialog::onChatMessageReceived);
    connect(m_client, &LobbyClient::adminAuthResult,      this, &RollbackLobbyDialog::onAdminAuthResult);
    connect(m_client, &LobbyClient::modNotice,            this, &RollbackLobbyDialog::onModNotice);
    connect(m_client, &LobbyClient::modListReceived,      this, &RollbackLobbyDialog::onModListReceived);
    connect(m_client, &LobbyClient::matchBegin,           this, &RollbackLobbyDialog::onMatchBegin);
    connect(m_client, &LobbyClient::matchPeerLeft,        this, &RollbackLobbyDialog::onMatchPeerLeft);
    connect(m_client, &LobbyClient::pingProbeMeasured,    this, &RollbackLobbyDialog::onPingMeasured);
    connect(m_client, &LobbyClient::spectateBegan,        this, &RollbackLobbyDialog::onSpectateBegan);
    connect(m_client, &LobbyClient::spectateData,         this, &RollbackLobbyDialog::onSpectateData);
    connect(m_client, &LobbyClient::spectateKeyframe,     this, &RollbackLobbyDialog::onSpectateKeyframe);
    connect(m_client, &LobbyClient::spectateEnded,        this, &RollbackLobbyDialog::onSpectateEnded);
    connect(m_client, &LobbyClient::spectateFailed,       this, &RollbackLobbyDialog::onSpectateFailed);

    // Drains staged krec bytes to the WebSocket while broadcasting. ~80 ms keeps
    // WS frame overhead low without adding meaningful latency to spectators.
    m_broadcastDrainTimer = new QTimer(this);
    m_broadcastDrainTimer->setInterval(80);
    connect(m_broadcastDrainTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onBroadcastDrainTick);

    // 3 s cadence is a balance: fast enough that the displayed ping tracks real
    // conditions, slow enough that we're not flooding peers' anchor sockets. The
    // *first* ping for a peer no longer waits on this tick — onRoomStateChanged
    // fires an immediate probe the moment a peer is seated. Stays stopped until a
    // room is entered.
    m_pingProbeTimer = new QTimer(this);
    m_pingProbeTimer->setInterval(3'000);
    connect(m_pingProbeTimer, &QTimer::timeout, this, &RollbackLobbyDialog::onPingProbeTick);
}

RollbackLobbyDialog::~RollbackLobbyDialog()
{
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

    // Top-level stack: the inline connect screen (0) transforms into the live
    // lobby (1) once connected, instead of a separate modal prompt.
    m_topStack = new QStackedWidget(this);
    m_topStack->addWidget(buildConnectView());   // index 0
    m_topStack->addWidget(buildLobbyView());     // index 1
    root->addWidget(m_topStack);
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
    m_splitter->setSizes({600, 360, 280});

    lay->addWidget(m_splitter, 1);
    return container;
}

QWidget* RollbackLobbyDialog::buildConnectView()
{
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(32, 32, 32, 32);
    outer->addStretch(1);

    auto* card = new QWidget(page);
    card->setObjectName("LobbyConnectCard");
    card->setMaximumWidth(460);
    auto* lay = new QVBoxLayout(card);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(14);

    auto* title = new QLabel("RMG-K Rollback Netplay", card);
    title->setAlignment(Qt::AlignHCenter);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    lay->addWidget(title);

    auto* intro = new QLabel(
        "Rollback netplay uses GGPO-style rollback for smooth, low-latency online "
        "play. Connect to the lobby to see who's online, create or join a room, and "
        "start a match.\n\nPick a username other players will see — you can change "
        "it later.", card);
    intro->setWordWrap(true);
    intro->setAlignment(Qt::AlignHCenter);
    lay->addWidget(intro);

    m_connectUsernameEdit = new QLineEdit(card);
    m_connectUsernameEdit->setMaxLength(16);
    m_connectUsernameEdit->setPlaceholderText("Username");
    m_connectUsernameEdit->setAlignment(Qt::AlignHCenter);
    auto* validator = new QRegularExpressionValidator(
        QRegularExpression(R"([A-Za-z0-9_\-\.]{1,16})"), this);
    m_connectUsernameEdit->setValidator(validator);
    // Half-width, centered (side stretch 1 : field 2 : side stretch 1 = 50%).
    auto* userRow = new QHBoxLayout();
    userRow->setContentsMargins(0, 0, 0, 0);
    userRow->addStretch(1);
    userRow->addWidget(m_connectUsernameEdit, 2);
    userRow->addStretch(1);
    lay->addLayout(userRow);

    m_connectStatusLabel = new QLabel(card);
    m_connectStatusLabel->setWordWrap(true);
    m_connectStatusLabel->setAlignment(Qt::AlignHCenter);
    lay->addWidget(m_connectStatusLabel);

    m_connectButton = new QPushButton("Connect", card);
    m_connectButton->setDefault(true);
    m_connectButton->setMinimumHeight(38);
    m_connectButton->setCursor(Qt::PointingHandCursor);
    m_connectButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Modern, rounded, primary-blue (matches the launcher's accent buttons).
    m_connectButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #0078D7;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 19px;"
        "  padding: 8px 22px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background-color: #1c88dc; }"
        "QPushButton:pressed { background-color: #005a9e; }"
        "QPushButton:disabled { background-color: palette(mid); color: palette(midlight); }");
    // Half-width, centered to match the username field.
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addStretch(1);
    btnRow->addWidget(m_connectButton, 2);
    btnRow->addStretch(1);
    lay->addLayout(btnRow);

    connect(m_connectButton, &QPushButton::clicked,
            this, &RollbackLobbyDialog::onConnectClicked);
    connect(m_connectUsernameEdit, &QLineEdit::returnPressed,
            this, &RollbackLobbyDialog::onConnectClicked);
    connect(m_connectUsernameEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        if (m_connectStatusLabel) m_connectStatusLabel->clear();
    });

    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(2);
    return page;
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

    m_brandLabel = new QLabel("RMG-K · Rollback Netplay", this);
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

    m_statusText = new QLabel("Offline", m_statusPill);
    m_statusText->setStyleSheet(QString("color: %1; font-weight: 600;").arg(statusColors().idle));
    pillLay->addWidget(m_statusText);
    h->addWidget(m_statusPill);

    h->addSpacing(SPACING_DEFAULT * 2);

    m_serverMeta = new QLabel("Not connected", this);
    // PlaceholderText is the right role for "secondary body text" — Mid is
    // a border-tone and disappears on Fusion Dark. PlaceholderText derives
    // from palette(text) with reduced alpha, so it reads on every theme.
    m_serverMeta->setForegroundRole(QPalette::PlaceholderText);
    h->addWidget(m_serverMeta);

    h->addStretch(1);

    m_userLabel = new QLabel("User: —", this);
    m_userLabel->setObjectName("UserLabel");
    h->addWidget(m_userLabel);

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

    m_bannerText = new QLabel("You're in a room", this);
    bumpFont(m_bannerText, 0, /*bold=*/true);
    bannerLay->addWidget(m_bannerText, 1);

    m_bannerReturn = new QPushButton(QStringLiteral("Return to Room  →"), this);
    m_bannerReturn->setObjectName("BannerReturnBtn");
    m_bannerReturn->setMinimumHeight(BUTTON_MIN_HEIGHT);
    m_bannerReturn->setCursor(Qt::PointingHandCursor);
    m_bannerReturn->setAutoDefault(false);
    m_bannerReturn->setDefault(false);
    connect(m_bannerReturn, &QPushButton::clicked, this, &RollbackLobbyDialog::switchToInRoomView);
    bannerLay->addWidget(m_bannerReturn);

    m_inRoomBanner->setVisible(false);
    lay->addWidget(m_inRoomBanner);

    // ── Hero row: Quick Match (primary blue) + Create Room (secondary) ──
    auto* heroRow = new QHBoxLayout;
    heroRow->setSpacing(SPACING_DEFAULT);

    m_quickMatchBtn = new QPushButton("⚡  Quick Match", this);
    m_quickMatchBtn->setObjectName("QuickMatchBtn");
    m_quickMatchBtn->setEnabled(false);
    m_quickMatchBtn->setAutoDefault(false);
    m_quickMatchBtn->setDefault(false);
    m_quickMatchBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_quickMatchBtn->setCursor(Qt::PointingHandCursor);
    m_quickMatchBtn->setToolTip(
        "Auto-match with another player searching for the selected game.\n"
        "Defaults to delay 2 / prediction 7.");

    m_createRoomBtn = new QPushButton("Create Room…", this);
    m_createRoomBtn->setObjectName("CreateRoomBtn");
    m_createRoomBtn->setAutoDefault(false);
    m_createRoomBtn->setDefault(false);
    m_createRoomBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_createRoomBtn->setCursor(Qt::PointingHandCursor);

    connect(m_quickMatchBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onQuickMatchClicked);
    connect(m_createRoomBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onCreateRoomClicked);

    heroRow->addWidget(m_quickMatchBtn, 1);
    heroRow->addWidget(m_createRoomBtn, 0);
    lay->addLayout(heroRow);

    // ── Game picker — pulls from the RMG-K library. Both Create Room and
    //    Quick Match key off the selected game's MD5. ──
    auto* gameRow = new QHBoxLayout;
    gameRow->setSpacing(SPACING_DEFAULT);
    auto* gameLbl = new QLabel("Game:", this);
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
    gameRow->addWidget(gameLbl, 0);
    gameRow->addWidget(m_browseRomCombo, 1);
    lay->addLayout(gameRow);
    populateBrowseRoms(); // fill from whatever library we have so far

    // ── Active Rooms ──
    auto* roomsHeader = new QLabel("ACTIVE ROOMS", this);
    roomsHeader->setProperty("class", "SectionHeader");
    lay->addWidget(roomsHeader);

    m_roomsTree = new QTreeWidget(this);
    m_roomsTree->setObjectName("RoomsTree");
    m_roomsTree->setHeaderLabels({ "Name", "Host", "ROM", "Seats", "State" });
    m_roomsTree->setRootIsDecorated(false);
    m_roomsTree->setSortingEnabled(true);
    m_roomsTree->setAlternatingRowColors(true);
    m_roomsTree->setFrameShape(QFrame::NoFrame);
    m_roomsTree->setUniformRowHeights(true);
    m_roomsTree->header()->setStretchLastSection(false);
    m_roomsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_roomsTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_roomsTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_roomsTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_roomsTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    connect(m_roomsTree, &QTreeWidget::itemDoubleClicked,
            this, &RollbackLobbyDialog::onRoomDoubleClicked);
    lay->addWidget(m_roomsTree, 1);

    // ── Ongoing Matches ──
    auto* matchesHeader = new QLabel("ONGOING MATCHES", this);
    matchesHeader->setProperty("class", "SectionHeader");
    lay->addWidget(matchesHeader);

    m_matchesTree = new QTreeWidget(this);
    m_matchesTree->setObjectName("MatchesTree");
    m_matchesTree->setHeaderLabels({ "Players", "Duration", "ROM" });
    m_matchesTree->setRootIsDecorated(false);
    m_matchesTree->setAlternatingRowColors(true);
    m_matchesTree->setFrameShape(QFrame::NoFrame);
    m_matchesTree->setFixedHeight(120);
    m_matchesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_matchesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_matchesTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(m_matchesTree, &QTreeWidget::itemDoubleClicked,
            this, &RollbackLobbyDialog::onMatchDoubleClicked);
    lay->addWidget(m_matchesTree);

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
    auto* breadcrumb = new QPushButton("←  Browse Rooms", this);
    breadcrumb->setFlat(true);
    breadcrumb->setCursor(Qt::PointingHandCursor);
    breadcrumb->setAutoDefault(false);
    breadcrumb->setToolTip("Switch to the room list. You stay in your seat — use Leave Room to actually exit.");
    connect(breadcrumb, &QPushButton::clicked, this, &RollbackLobbyDialog::switchToRoomsView);
    topRow->addWidget(breadcrumb);
    topRow->addStretch(1);

    m_roomStateLabel = new QLabel("Waiting", this);
    topRow->addWidget(m_roomStateLabel);
    applyRoomStateBadge("Waiting", stateHex("waiting", isDarkTheme()));
    lay->addLayout(topRow);

    // ROM title (large). Word-wrap so a long ROM name doesn't pin a wide
    // minimum width on the whole left column (it's shared via the stacked
    // widget with the browse view).
    m_roomTitle = new QLabel("—", this);
    bumpFont(m_roomTitle, 4, /*bold=*/true);
    m_roomTitle->setWordWrap(true);
    lay->addWidget(m_roomTitle);

    // Subtitle (host / max players) — default WindowText since this is
    // important info; the bold title above gives the hierarchy.
    m_roomSubtitle = new QLabel("—", this);
    m_roomSubtitle->setWordWrap(true);
    lay->addWidget(m_roomSubtitle);

    // Meta line (Seats / Region) — plain rich text. Delay and prediction
    // live in the editable settings row below instead of read-only chips.
    m_roomMetaLabel = new QLabel("—", this);
    m_roomMetaLabel->setTextFormat(Qt::RichText);
    m_roomMetaLabel->setContentsMargins(0, SPACING_TIGHT, 0, 0);
    lay->addWidget(m_roomMetaLabel);

    // ── Rollback settings row (host-editable) ──
    auto* settingsRow = new QHBoxLayout;
    settingsRow->setContentsMargins(0, SPACING_TIGHT, 0, 0);
    settingsRow->setSpacing(SPACING_DEFAULT);

    const QString delayTip = QStringLiteral(
        "Frames of input delay added before sending to peer.\n"
        "Higher delay = fewer rollbacks but more input latency.\n"
        "Recommended: 2 for ~80ms RTT, 3-4 for ~150ms RTT.\n"
        "\n"
        "Host-only. All players will use the same value.");
    const QString predictionTip = QStringLiteral(
        "Maximum frames the rollback engine may predict ahead.\n"
        "Higher prediction = more network tolerance.\n"
        "Recommended: 7 (matches Slippi default).\n"
        "\n"
        "Host-only. All players will use the same value.");

    // Frame values exposed in the dropdown. 0 is intentionally omitted —
    // GekkoNet's zero-delay path still has open bugs (see project memory:
    // 0-delay host crash with non-zero analog drift), so we hide it from
    // users until that's resolved. Editing this list updates the UI; the
    // server's clamp range governs what values it'll actually accept.
    static const QList<int> FRAME_OPTIONS = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    // The auto entry is data 0 (a value the numeric list never uses), inserted
    // at the top of each combo. Delay's auto resolves from ping (labelled
    // "Auto"); prediction's is a fixed 7 (labelled "Default").
    auto fillFrameCombo = [](QComboBox* combo, const QString& autoLabel) {
        combo->addItem(autoLabel, 0);
        for (int v : FRAME_OPTIONS)
            combo->addItem(QString("%1 f").arg(v), v);
        combo->setCurrentIndex(0); // default to the auto/default entry
    };

    auto* delayLbl = new QLabel("Frame delay:", this);
    m_delayCombo = new QComboBox(this);
    m_delayCombo->setObjectName("LobbyCombo");
    fillFrameCombo(m_delayCombo, "Auto");
    m_delayCombo->setToolTip(delayTip);
    // Stash the explainer so onRoomStateChanged can restore it after a
    // disabled stint (host became host again, or match ended).
    m_delayCombo->setProperty("originalTip", delayTip);
    settingsRow->addWidget(delayLbl);
    settingsRow->addWidget(m_delayCombo);

    settingsRow->addSpacing(SPACING_DEFAULT * 2);

    auto* predLbl = new QLabel("Prediction:", this);
    m_predictionCombo = new QComboBox(this);
    m_predictionCombo->setObjectName("LobbyCombo");
    fillFrameCombo(m_predictionCombo, "Default");
    m_predictionCombo->setToolTip(predictionTip);
    m_predictionCombo->setProperty("originalTip", predictionTip);
    settingsRow->addWidget(predLbl);
    settingsRow->addWidget(m_predictionCombo);

    // Pacing is no longer user-selectable — the engine hardwires the Smooth
    // (asymmetric) model. The combo survives hidden and pinned to Smooth
    // because the room-settings sync paths require all three combos non-null;
    // it just always reports 1.
    m_pacingCombo = new QComboBox(this);
    m_pacingCombo->addItem("Smooth", 1);
    m_pacingCombo->hide();

    settingsRow->addStretch(1);
    lay->addLayout(settingsRow);

    // Recording toggles live on their own row beneath the combos. Keeping them
    // off the combo row keeps that row's minimum width small, so the left
    // column can be dragged narrower (more room for chat / players).
    auto* toggleRow = new QHBoxLayout;
    toggleRow->setContentsMargins(0, 0, 0, 0);
    toggleRow->setSpacing(SPACING_DEFAULT);

    // Per-player local recording toggle. Stays enabled for everyone — each
    // player decides whether to save their own .krec. Initialized from the
    // cap-aware default and kept in the shared n02 recording flag, exactly like
    // the p2p / kaillera lobbies.
    m_recordCheck = new QCheckBox("Record game", this);
    m_recordCheck->setToolTip(
        "Record this match to a .krec file on your PC.\n"
        "Local setting — each player records their own copy.");
    const bool recordingDefault = CoreGetKailleraEffectiveRecordingDefault();
    n02_kaillera_recording_enabled = recordingDefault;
    m_recordCheck->setChecked(recordingDefault);
    connect(m_recordCheck, &QCheckBox::toggled, this, [](bool checked) {
        n02_kaillera_recording_enabled = checked;
    });
    toggleRow->addWidget(m_recordCheck);

    // Broadcast: stream this match's krec to the server so others can spectate.
    // Broadcasting implies recording (the stream is the krec), so ticking it
    // also forces "Record game" on.
    m_broadcastCheck = new QCheckBox("Live Replay", this);
    m_broadcastCheck->setToolTip(
        "Let others in the lobby watch this match live.\n"
        "Implies Record game (the live replay is the .krec). Only one player\n"
        "per match streams it — whoever enables it first.");
    connect(m_broadcastCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_recordCheck)
            m_recordCheck->setChecked(true); // broadcasting needs the krec written
    });
    toggleRow->addWidget(m_broadcastCheck);
    toggleRow->addStretch(1);

    lay->addLayout(toggleRow);

    // Both combos share the same change handler. Skip emits while we're
    // applying values from a ROOM_STATE refresh — otherwise we'd ping-pong
    // a settings update back to the server on every server-driven refresh.
    auto pushSettings = [this]() {
        if (m_suppressSettingsSignal) return;
        if (m_currentRoomId == 0) return;
        if (!m_client) return;
        // A combo's "Auto" entry has data 0; track each mode, then resolve and
        // push the concrete values (applyHostRoomSettings never sends 0).
        m_delayAuto      = (m_delayCombo->currentData().toInt() == 0);
        m_predictionAuto = (m_predictionCombo->currentData().toInt() == 0);
        applyHostRoomSettings(true);
    };
    connect(m_delayCombo,      QOverload<int>::of(&QComboBox::currentIndexChanged), this, pushSettings);
    connect(m_predictionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, pushSettings);

    // ── Seats — bold section header + vertical player-list rows ──
    auto* seatsHeader = new QLabel("SEATS", this);
    seatsHeader->setProperty("class", "SectionHeader");
    seatsHeader->setContentsMargins(0, SPACING_DEFAULT, 0, 0);
    lay->addWidget(seatsHeader);

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
    auto* roomChatHeader = new QLabel("ROOM CHAT", this);
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
    roomChatLay->addWidget(m_chatViewRoom, 1);

    m_roomChatInput = new QLineEdit(roomChatCard);
    m_roomChatInput->setPlaceholderText("Message the room…");
    m_roomChatInput->setEnabled(false);
    m_roomChatInput->setMaxLength(500); // server truncates at 500; cap client-side too
    m_roomChatInput->setMinimumHeight(BUTTON_MIN_HEIGHT);
    connect(m_roomChatInput, &QLineEdit::returnPressed, this, &RollbackLobbyDialog::onRoomChatSendClicked);
    roomChatLay->addWidget(m_roomChatInput);

    lay->addWidget(roomChatCard, 1);   // expands to fill between seats and actions

    // ── Action bar ──
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(SPACING_DEFAULT);

    m_startBtn = new QPushButton("Start Game", this);
    m_startBtn->setObjectName("StartGameBtn");
    m_startBtn->setEnabled(false);
    m_startBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_startBtn->setCursor(Qt::PointingHandCursor);
    m_startBtn->setAutoDefault(false);
    m_startBtn->setDefault(false);
    connect(m_startBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onStartGameClicked);

    m_dropBtn = new QPushButton("Drop Game", this);
    m_dropBtn->setObjectName("DropBtn");
    m_dropBtn->setEnabled(false);
    m_dropBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_dropBtn->setAutoDefault(false);
    m_dropBtn->setDefault(false);
    m_dropBtn->setCursor(Qt::PointingHandCursor);
    m_dropBtn->setToolTip("Stop playing the current match. If you're the host, the match ends for everyone.");
    connect(m_dropBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onDropGameClicked);

    m_leaveBtn = new QPushButton("Leave Room", this);
    m_leaveBtn->setObjectName("LeaveBtn");
    m_leaveBtn->setMinimumHeight(HERO_BUTTON_HEIGHT);
    m_leaveBtn->setAutoDefault(false);
    m_leaveBtn->setDefault(false);
    m_leaveBtn->setCursor(Qt::PointingHandCursor);
    m_leaveBtn->setToolTip("Leave the room and return to the lobby.");
    connect(m_leaveBtn, &QPushButton::clicked, this, &RollbackLobbyDialog::onLeaveRoomClicked);

    actionRow->addWidget(m_startBtn, 1);
    actionRow->addStretch(0);
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
    s.dragHandle->setToolTip(QStringLiteral("Drag onto another seat to swap players"));
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

    s.nameLabel = new QLabel(QStringLiteral("Waiting…"), s.row);
    lay->addWidget(s.nameLabel);

    lay->addStretch(1);

    // Right-aligned meta (HOST badge · ping). Rich text so the badge and the
    // ping value can carry their own colors.
    s.metaLabel = new QLabel(QString(), s.row);
    s.metaLabel->setTextFormat(Qt::RichText);
    s.metaLabel->setForegroundRole(QPalette::PlaceholderText);
    lay->addWidget(s.metaLabel);

    // Host-only kick button. Hidden by default; shown on non-host seats only
    // when the local user is the host (set in renderSeatFilled).
    s.kickButton = new QPushButton(QStringLiteral("✕"), s.row);
    s.kickButton->setObjectName("SeatKickButton");
    s.kickButton->setFixedSize(20, 20);
    s.kickButton->setCursor(Qt::PointingHandCursor);
    s.kickButton->setFocusPolicy(Qt::NoFocus);
    s.kickButton->setToolTip(QStringLiteral("Remove this player from the match"));
    s.kickButton->setVisible(false);
    connect(s.kickButton, &QPushButton::clicked, this, [this, &s]() {
        if (!m_client || s.userId == 0)
            return;
        const QString name = s.nameLabel ? s.nameLabel->text() : QString();
        if (QMessageBox::question(this, "Remove player",
                QString("Remove %1 from the match?").arg(name.isEmpty() ? QStringLiteral("this player") : name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        m_client->kickFromRoom(s.userId);
    });
    lay->addWidget(s.kickButton);
}

void RollbackLobbyDialog::renderSeatEmpty(SeatRow& s)
{
    s.isHost = false;
    s.userId = 0;

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
        s.nameLabel->setText(QStringLiteral("Waiting…"));
        s.nameLabel->setStyleSheet(QString());
        s.nameLabel->setForegroundRole(QPalette::PlaceholderText);
        QFont f = s.nameLabel->font();
        f.setBold(false);
        s.nameLabel->setFont(f);
    }
    if (s.metaLabel) s.metaLabel->setText(QString());
    if (s.kickButton) s.kickButton->setVisible(false);
}

void RollbackLobbyDialog::renderSeatFilled(SeatRow& s, const QString& username, bool isHost,
                                           bool isSelf, int pingMs, bool canKick)
{
    s.isHost = isHost;
    if (s.kickButton) s.kickButton->setVisible(canKick);

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
        if (isSelf) name += QStringLiteral("  (you)");
        s.nameLabel->setText(name);
        s.nameLabel->setStyleSheet(QString());
        s.nameLabel->setForegroundRole(QPalette::WindowText);
        QFont f = s.nameLabel->font();
        f.setBold(isSelf);
        s.nameLabel->setFont(f);
    }
    if (s.metaLabel)
    {
        // HOST badge + ping. Self never shows a ping (pingMs == -1 also means
        // "no measurement yet" for peers we haven't probed). Ping shows up
        // after the first PROBE_REPLY arrives, refreshed on each tick.
        s.metaLabel->setText(seatMetaHtml(s.slot, isHost, isSelf ? -1 : pingMs, dark));
    }
}

// ── Chat column ──────────────────────────────────────────────────────

QWidget* RollbackLobbyDialog::buildChatColumn()
{
    // Lobby chat only — room chat lives below the seats in the in-room view.
    // A bold header labels the column (it used to be a tab) and the chat sits
    // in the same rounded card as the players list.
    auto* col = new QWidget(this);
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER, MARGIN_OUTER);
    lay->setSpacing(SPACING_DEFAULT);

    auto* header = new QLabel("LOBBY CHAT", this);
    header->setProperty("class", "SectionHeader");
    lay->addWidget(header);

    auto* card = new QFrame(col);
    card->setObjectName("LobbyCard");
    auto* cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT, SPACING_DEFAULT);
    cardLay->setSpacing(SPACING_DEFAULT);

    m_chatViewLobby = new QTextEdit(card);
    m_chatViewLobby->setReadOnly(true);
    m_chatViewLobby->setLineWrapMode(QTextEdit::WidgetWidth);
    m_chatViewLobby->setFrameShape(QFrame::NoFrame);
    cardLay->addWidget(m_chatViewLobby, 1);

    m_chatInput = new QLineEdit(card);
    m_chatInput->setPlaceholderText("Message the lobby…");
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
    // The tree's column header ("Player / State / Ping") already labels the
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
    m_playersTree->setHeaderLabels({ "Player", "State", "Est. Ping" });
    m_playersTree->setRootIsDecorated(false);
    m_playersTree->setSortingEnabled(true);
    m_playersTree->setAlternatingRowColors(true);
    m_playersTree->setFrameShape(QFrame::NoFrame);
    m_playersTree->setUniformRowHeights(true);
    m_playersTree->header()->setStretchLastSection(false);
    m_playersTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_playersTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_playersTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // Click in the empty area below the rows to clear the selection (handled in
    // eventFilter — there's no built-in property for this).
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

    // Brand accent (P1 blue) for the header text, and a faint accent wash for
    // the marquee strip so it reads as a header band rather than blank window.
    const QString brandAccent = playerAccentHex(1, dark);
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

        // Secondary action buttons — Create Room / Drop / Leave read as a set,
        // matching the P2P hosting screen's secondary buttons.
        "QPushButton#CreateRoomBtn, QPushButton#DropBtn, QPushButton#LeaveBtn {"
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
        m_browseRomCombo->addItem("No ROMs in your library — add some first");
        m_browseRomCombo->setEnabled(false);
        m_browseRomCombo->blockSignals(false);
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
}

QVariantMap RollbackLobbyDialog::selectedBrowseRom() const
{
    if (!m_browseRomCombo || !m_browseRomCombo->isEnabled())
        return {};
    QVariantMap data = m_browseRomCombo->currentData().toMap();
    // Editable combo: if the user typed an exact name but didn't commit it (no
    // index change), currentData() is stale — fall back to matching the text.
    if (data.value("md5").toString().isEmpty())
    {
        const int idx = m_browseRomCombo->findText(m_browseRomCombo->currentText());
        if (idx >= 0)
            data = m_browseRomCombo->itemData(idx).toMap();
    }
    return data;
}

// ──────────────────────────────────────────────────────────────────────
//  Show / close
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Show the inline connect screen unless we're already in a live session.
    if (m_client->state() == LobbyClient::ConnectionState::Connected)
    {
        showLobbyView();
    }
    else
    {
        showConnectView();
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
    // A click in the players-list viewport that doesn't land on a row clears
    // the selection, so a highlighted name can be dismissed by clicking off it.
    if (m_playersTree && watched == m_playersTree->viewport() &&
        event->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(event);
        if (!m_playersTree->itemAt(me->position().toPoint()))
            m_playersTree->clearSelection();
        return QDialog::eventFilter(watched, event);
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
    return name.left(16);
}

void RollbackLobbyDialog::showConnectView(const QString& statusMessage)
{
    if (!m_topStack)
    {
        return;
    }

    if (m_connectUsernameEdit)
    {
        if (m_connectUsernameEdit->text().trimmed().isEmpty())
        {
            m_connectUsernameEdit->setText(prefillUsername());
        }
        m_connectUsernameEdit->setFocus();
        m_connectUsernameEdit->selectAll();
    }
    if (m_connectButton)
    {
        m_connectButton->setEnabled(true);
    }
    if (m_connectStatusLabel)
    {
        m_connectStatusLabel->setStyleSheet(statusMessage.isEmpty() ? QString() : "color: #c0392b;");
        m_connectStatusLabel->setText(statusMessage);
    }

    m_topStack->setCurrentIndex(0);
}

void RollbackLobbyDialog::showLobbyView()
{
    if (m_topStack)
    {
        m_topStack->setCurrentIndex(1);
    }
}

void RollbackLobbyDialog::onConnectClicked()
{
    const QString username = m_connectUsernameEdit
        ? m_connectUsernameEdit->text().trimmed()
        : QString();

    if (username.length() < 3)
    {
        if (m_connectStatusLabel)
        {
            m_connectStatusLabel->setStyleSheet("color: #c0392b;");
            m_connectStatusLabel->setText("Username must be at least 3 characters.");
        }
        if (m_connectUsernameEdit) m_connectUsernameEdit->setFocus();
        return;
    }

    m_username  = username;
    m_serverUrl = LobbyConnectDialog::defaultServerUrl();
    if (m_userLabel) m_userLabel->setText(QString("User: %1").arg(m_username));

    // Remember it so the field pre-fills next time.
    QSettings s("RMG-K", "n02");
    s.setValue("Lobby/Username", m_username);

    if (m_connectButton) m_connectButton->setEnabled(false);
    if (m_connectStatusLabel)
    {
        m_connectStatusLabel->setStyleSheet(QString());
        m_connectStatusLabel->setText("Connecting…");
    }

    updateServerMeta();
    m_client->connectToServer(m_serverUrl, m_username, {}, QString());

    // Transform straight into the lobby; the marquee shows live connection state.
    showLobbyView();
}

// ──────────────────────────────────────────────────────────────────────
//  Connection events
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onClientStateChanged(LobbyClient::ConnectionState s)
{
    updateStatusIndicator(s);

    const bool connected = (s == LobbyClient::ConnectionState::Connected);
    m_chatInput->setEnabled(connected);
    if (m_quickMatchBtn)
        m_quickMatchBtn->setEnabled(connected && m_currentRoomId == 0);
    if (m_createRoomBtn)
        m_createRoomBtn->setEnabled(connected && m_currentRoomId == 0);

    if (s == LobbyClient::ConnectionState::Disconnected)
    {
        m_playersTree->clear();
        m_roomsTree->clear();
        m_userItems.clear();
        m_roomItems.clear();
        m_currentRoomId = 0;
        m_currentRoomState.clear();
        m_currentMatchId = 0;
        m_awaitingEmulationStart = false;
        m_emulationActive = false;
        m_quickMatchActive = false;
        if (m_quickMatchBtn) m_quickMatchBtn->setText("⚡  Quick Match");

        if (m_chatViewRoom) m_chatViewRoom->clear();
        if (m_roomChatInput) m_roomChatInput->setEnabled(false);
        switchToRoomsView();
    }
    updateServerMeta();
    updateInRoomBanner();

    // Drive the top-level connect ↔ lobby swap. Failed is handled by the error
    // slots below (onConnectError/onHelloFailed), which fire just before it and
    // show the connect screen with a message.
    if (s == LobbyClient::ConnectionState::Connected)
    {
        showLobbyView();
    }
    else if (s == LobbyClient::ConnectionState::Disconnected)
    {
        showConnectView();
    }
}

void RollbackLobbyDialog::onHelloFailed(const QString& reason)
{
    QString human = reason;
    if (reason == "username_taken")    human = "That username is already in use.";
    else if (reason == "invalid_hello") human = "Server rejected the connection handshake.";
    else if (reason == "version_mismatch") human = "Client version is incompatible with this server.";

    showConnectView(human);
}

void RollbackLobbyDialog::onConnectError(const QString& msg)
{
    showConnectView("Couldn't reach the lobby: " + msg);
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
        name = QStringLiteral("Your room");

    // Also include seat count when we have it cached from ROOM_STATE.
    QString seats;
    if (it != rooms.constEnd() && it->maxPlayers > 0)
        seats = QString("  ·  %1/%2 seats").arg(it->players).arg(it->maxPlayers);

    if (m_bannerText)
        m_bannerText->setText(QString("You're in: <b>%1</b>%2").arg(name, seats));
}

void RollbackLobbyDialog::updateServerMeta()
{
    if (!m_serverMeta) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
    {
        m_serverMeta->setText("Not connected");
        return;
    }
    const int players = m_client->users().size();
    const int rooms = m_client->rooms().size();
    // Server host/IP intentionally omitted from the header — just show the
    // live population.
    m_serverMeta->setText(QString("%1 player%2  ·  %3 room%4")
                              .arg(players).arg(players == 1 ? "" : "s")
                              .arg(rooms).arg(rooms == 1 ? "" : "s"));
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
        auto* row = new QTreeWidgetItem(m_playersTree);
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

    auto* row = new QTreeWidgetItem(m_playersTree);
    refreshPlayerRow(row, it.value());
    m_userItems.insert(userId, row);
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
    refreshPlayerRow(it.value(), userIt.value());
}

void RollbackLobbyDialog::refreshPlayerRow(QTreeWidgetItem* item, const LobbyClient::LobbyUser& u)
{
    const bool dark = isDarkTheme();
    item->setText(0, u.username);
    // Self gets bold and an accent tint; everyone else uses default palette text.
    if (u.id == m_client->selfUserId())
    {
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setForeground(0, QColor(playerAccentHex(1, dark)));
    }
    item->setText(1, stateGlyph(u.state));
    item->setForeground(1, QColor(stateHex(u.state, dark)));

    // While searching, hovering the state shows which ROM they're queued for.
    if (u.state == "searching" && !u.searchingRom.isEmpty())
        item->setToolTip(1, QString("Searching for: %1").arg(u.searchingRom));
    else
        item->setToolTip(1, QString());

    // Ping column shows an estimated round-trip between *this* client and the
    // peer, based on region buckets the server assigned via IP geolocation.
    // It's intentionally rough — same-region pairs read ~30 ms, transatlantic
    // ~90, transpacific ~200. Self-row always shows "—".
    // U+2014 EM DASH — using QChar avoids any file-encoding ambiguity in
    // the literal.
    const QString dash = QString(QChar(0x2014));
    if (u.id == m_client->selfUserId())
    {
        item->setText(2, dash);
        item->setForeground(2, QColor(statusColors().idle));
    }
    else
    {
        const int rtt = UserInterface::Dialog::LobbyRegions::estimatedRttMs(
            m_client->selfRegion(), u.region);
        item->setText(2, rtt > 0 ? QString("~%1 ms").arg(rtt) : dash);
        // Tint by the same ping tiers as the seat cards (grey when unknown).
        item->setForeground(2, QColor(pingHex(rtt > 0 ? rtt : -1)));
    }

    const QString regionLabel = UserInterface::Dialog::LobbyRegions::labelFor(u.region);
    const QString regionTip = QString("Region: %1").arg(regionLabel.isEmpty() ? "unknown" : regionLabel);
    item->setToolTip(0, regionTip);
    item->setToolTip(2, regionTip); // hovering the ping shows the peer's region
    item->setData(0, Qt::UserRole, QVariant::fromValue(u.id));
}

QString RollbackLobbyDialog::stateGlyph(const QString& state) const
{
    if (state == "idle")       return "Online";
    if (state == "browsing")   return "Browsing";
    if (state == "hosting")    return "Hosting";
    if (state == "in_room")    return "In Room";
    if (state == "searching")  return "Searching";
    if (state == "playing")    return "In Game";
    if (state == "spectating") return "Watching";
    if (state == "away")       return "Away";
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

void RollbackLobbyDialog::onRoomListChanged()
{
    m_roomsTree->clear();
    m_matchesTree->clear();
    m_roomItems.clear();
    for (auto it = m_client->rooms().constBegin(); it != m_client->rooms().constEnd(); ++it)
    {
        const LobbyClient::LobbyRoomSummary& r = it.value();

        // A room that's playing is a live match — show it under Ongoing Matches
        // and drop it from the joinable Active Rooms list.
        if (r.state == "in_game")
        {
            auto* matchRow = new QTreeWidgetItem(m_matchesTree);
            matchRow->setText(0, r.playerNames.isEmpty() ? r.name : r.playerNames.join(" vs "));
            matchRow->setText(1, formatMatchDuration(r.startedAtMs));
            matchRow->setText(2, r.romName);
            matchRow->setData(0, Qt::UserRole, QVariant::fromValue(r.id));
            matchRow->setData(0, Qt::UserRole + 1, QVariant::fromValue(r.matchId)); // 0 unless broadcast
            matchRow->setData(1, Qt::UserRole, r.startedAtMs); // for the duration ticker

            // A live-replay match turns green with a watch hint; double-clicking
            // it watches live.
            if (r.broadcasting && r.matchId != 0)
            {
                const QColor live(0x2e, 0xa0, 0x43); // green, readable on light + dark
                const QString tip = QStringLiteral("🔴 Live — double-click to watch");
                matchRow->setText(0, QStringLiteral("🔴  ") + matchRow->text(0));
                for (int col = 0; col < 3; ++col)
                {
                    matchRow->setForeground(col, live);
                    matchRow->setToolTip(col, tip);
                }
            }
            continue;
        }

        auto* row = new QTreeWidgetItem(m_roomsTree);
        refreshRoomRow(row, r);
        m_roomItems.insert(it.key(), row);
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
        item->setText(1, formatMatchDuration(item->data(1, Qt::UserRole).toLongLong()));
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

void RollbackLobbyDialog::refreshRoomRow(QTreeWidgetItem* item, const LobbyClient::LobbyRoomSummary& r)
{
    const bool mine = (r.id == m_currentRoomId);
    QString nameCell = r.name;
    if (r.hasPassword) nameCell = QStringLiteral("🔒  ") + nameCell;
    if (mine)          nameCell = QStringLiteral("★  ") + nameCell;

    const bool dark = isDarkTheme();
    item->setText(0, nameCell);
    item->setText(1, r.hostName);
    item->setText(2, r.romName);
    item->setText(3, QString("%1/%2").arg(r.players).arg(r.maxPlayers));
    item->setText(4, stateGlyph(r.state));
    item->setData(0, Qt::UserRole, QVariant::fromValue(r.id));

    // Seats: green when there's room to join, red when full.
    const bool full = (r.maxPlayers > 0 && r.players >= r.maxPlayers);
    const auto sc = statusColors();
    item->setForeground(3, QColor(full ? sc.fail : sc.ok));
    // State: same color language as presence / seats.
    item->setForeground(4, QColor(stateHex(r.state, dark)));

    // Bold the user's own room and tint its name with the P1 accent.
    QFont f = item->font(0);
    f.setBold(mine);
    item->setFont(0, f);
    if (mine)
        item->setForeground(0, QColor(playerAccentHex(1, dark)));
}

void RollbackLobbyDialog::onCreateRoomClicked()
{
    if (m_client->state() != LobbyClient::ConnectionState::Connected)
        return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, "Already in a room",
            "Leave your current room before creating a new one.");
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
        QMessageBox::information(this, "Select a game",
            "Pick a game from the dropdown before creating a room.");
        return;
    }

    m_createRoomDialog = new CreateRoomDialog(m_username, romName, romMd5, this);
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
        m_createRoomDialog->password());
}

void RollbackLobbyDialog::onRoomCreated(quint64 roomId)
{
    if (m_createRoomDialog)
        m_createRoomDialog->accept();
    enterRoom(roomId,
        QStringLiteral("<i>Room created — waiting for players</i>"));
}

void RollbackLobbyDialog::onRoomJoinOk(quint64 roomId)
{
    QString roomName;
    auto it = m_client->rooms().constFind(roomId);
    if (it != m_client->rooms().constEnd())
        roomName = it.value().name;
    enterRoom(roomId,
        roomName.isEmpty()
            ? QStringLiteral("<i>You joined the room</i>")
            : QString("<i>You joined &quot;%1&quot;</i>").arg(roomName.toHtmlEscaped()));
}

void RollbackLobbyDialog::onRoomJoinFailed(const QString& reason)
{
    QString human = reason;
    if (reason == "wrong_password")    human = "Wrong password.";
    else if (reason == "full")         human = "Room is full.";
    else if (reason == "already_started") human = "That game has already started.";
    else if (reason == "already_in_room") human = "You're already in a room.";
    else if (reason == "room_not_found")  human = "That room no longer exists.";
    else if (reason == "kicked_recently") human = "You were recently removed from this room. Try again in a moment.";
    QMessageBox::warning(this, "Couldn't join room", human);
}

void RollbackLobbyDialog::enterRoom(quint64 roomId, const QString& greetingChatLine)
{
    m_currentRoomId = roomId;

    // Being in a room is mutually exclusive with searching. A successful Quick
    // Match drops us straight into an auto-created room without the server ever
    // sending QUICK_MATCH_STATUS{searching:false}, so clear the toggle state
    // here. Otherwise m_quickMatchActive stays true through the game and onClick
    // sends quickMatchCancel() (a no-op once we're out of the queue) instead of
    // quickMatchJoin() — leaving the button unable to start a new search.
    m_quickMatchActive = false;
    if (m_quickMatchBtn) m_quickMatchBtn->setText("⚡  Quick Match");

    // Every freshly-entered room starts in Auto delay/prediction (the in-room
    // combos default to "Auto", and Create Room no longer surfaces these). This
    // is what makes a Quick Match host resolve delay from the measured peer ping
    // during the warmup; the host can still switch to a manual value in-room.
    m_delayAuto      = true;
    m_predictionAuto = true;

    // Fresh room — don't chime for the seats already present in the first
    // ROOM_STATE; only for players who join after we've settled in.
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;

    // Fresh room, fresh chat — never carry the previous room's messages in.
    // The room chat widget lives in the in-room view (below the seats) and
    // persists; just clear it and enable its input.
    if (m_chatViewRoom) m_chatViewRoom->clear();
    if (m_roomChatInput) m_roomChatInput->setEnabled(true);
    switchToInRoomView();

    if (m_createRoomBtn) m_createRoomBtn->setEnabled(false);
    if (m_quickMatchBtn) m_quickMatchBtn->setEnabled(false);

    updateInRoomBanner();

    if (!greetingChatLine.isEmpty())
        appendChatLine(CHANNEL_ROOM, greetingChatLine);

    // Start measuring actual round-trip to seated peers. Seat assignments
    // populate via the follow-up ROOM_STATE message, which fires its own
    // immediate probe per newly-seated peer; this timer just refreshes them.
    if (m_pingProbeTimer && !m_pingProbeTimer->isActive())
        m_pingProbeTimer->start();

    // Backstop in case ROOM_STATE seats arrive in a shape that didn't trigger a
    // per-peer probe — kick one sweep shortly after entry so ping shows promptly
    // and the host's Auto delay resolves before the match begins. Harmless in an
    // empty room: onPingProbeTick skips self / unseated slots.
    QTimer::singleShot(600, this, &RollbackLobbyDialog::onPingProbeTick);
}

void RollbackLobbyDialog::onRoomDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    if (m_client->state() != LobbyClient::ConnectionState::Connected) return;
    if (m_currentRoomId != 0)
    {
        QMessageBox::information(this, "Already in a room",
            "Leave your current room before joining another.");
        return;
    }

    const quint64 roomId = item->data(0, Qt::UserRole).toULongLong();
    if (roomId == 0) return;

    auto it = m_client->rooms().constFind(roomId);
    if (it == m_client->rooms().constEnd()) return;
    const auto& summary = it.value();

    if (summary.state == "in_game" || summary.state == "starting")
    {
        if (summary.broadcasting && summary.matchId != 0)
        {
            beginSpectate(summary.matchId, summary.romName);
        }
        else
        {
            QMessageBox::information(this, "Match in progress",
                "That room is already playing — try another or wait for it to finish.");
        }
        return;
    }

    // Don't join a room whose ROM we don't have. Otherwise we'd commit, make the
    // other players wait, then fail pre-match sync — which used to hang the whole
    // client. Matched by MD5, the same check onMatchBegin uses, so passing here
    // guarantees the ROM resolves at match start too.
    if (!summary.romMd5.isEmpty() && localRomPathForMd5(summary.romMd5).isEmpty())
    {
        QMessageBox::warning(this, "ROM not found",
            QString("You don't have the ROM for \"%1\" (%2).\n\n"
                    "Add it to your ROM directory and refresh the list, then try again.")
                .arg(summary.name, summary.romName));
        return;
    }

    QString password;
    if (summary.hasPassword)
    {
        bool ok = false;
        password = QInputDialog::getText(this, "Password required",
            QString("Enter password for \"%1\":").arg(summary.name),
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
        QMessageBox::information(this, "In a room",
            "Leave your room before watching a match.");
        return;
    }
    const quint64 matchId = item->data(0, Qt::UserRole + 1).toULongLong();
    if (matchId == 0)
    {
        QMessageBox::information(this, "Not live",
            "That match isn't streaming a live replay, so there's nothing to watch.");
        return;
    }
    beginSpectate(matchId, item->text(2));
}

void RollbackLobbyDialog::onRoomCreateFailed(const QString& reason)
{
    QMessageBox::warning(this, "Couldn't create room", reason);
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
    const int delay = roomState.value("delay").toInt();
    const int prediction = roomState.value("prediction").toInt();
    const int pacing = roomState.value("pacing").toInt() == 1 ? 1 : 0;
    // Whether the host left each value on "Auto" — lets non-hosts mirror the
    // host's "Auto (N f)" / "Default" labels instead of a bare number.
    const bool roomDelayAuto = roomState.value("delayAuto").toBool();
    const bool roomPredictionAuto = roomState.value("predictionAuto").toBool();
    const int maxPlayers = roomState.value("maxPlayers").toInt();
    const QString state = roomState.value("state").toString();
    const quint64 hostId = static_cast<quint64>(roomState.value("hostId").toDouble());
    const bool iAmHost = (hostId == m_client->selfUserId());

    m_currentRoomGame       = romName;
    m_currentRoomMd5        = romMd5;
    m_currentRoomRegion     = romRegion;
    m_currentRoomDelay      = delay;
    m_currentRoomPrediction = prediction;
    m_currentRoomPacing     = pacing;
    m_currentRoomHostId     = hostId;

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
        appendChatSystemLine(CHANNEL_ROOM, "Host ended the match — stopping game.");
        emit closeMatchRequested();
    }

    if (m_dropBtn)
    {
        m_dropBtn->setEnabled(state == "in_game" &&
            (m_emulationActive || m_awaitingEmulationStart));
        m_dropBtn->setToolTip(
            iAmHost
                ? "Stop the match for everyone in the room."
                : "Drop out of the match. Other players keep playing.");
    }
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    // ── Header card ──
    QString title = romName.isEmpty() ? name : romName;
    if (!name.isEmpty() && name != romName && !romName.isEmpty())
        title = QString("%1 — %2").arg(name, romName);
    m_roomTitle->setText(title);

    // Resolve host display name: prefer hostName, fall back to users() map,
    // then to our own username if we're the host.
    QString hostName = roomState.value("hostName").toString();
    if (hostName.isEmpty())
    {
        const auto& users = m_client->users();
        const auto it = users.constFind(hostId);
        if (it != users.constEnd()) hostName = it->username;
    }
    if (hostName.isEmpty() && iAmHost) hostName = m_username;
    if (hostName.isEmpty()) hostName = QStringLiteral("—");

    m_roomSubtitle->setText(QString("Hosted by %1  ·  %2 players max")
                                .arg(hostName).arg(maxPlayers));

    applyRoomStateBadge(roomStateLabel(state), stateHex(state, isDarkTheme()));

    const QJsonArray players = roomState.value("players").toArray();
    QStringList metaParts;
    metaParts << QString("<b>Seats:</b> %1/%2").arg(players.size()).arg(maxPlayers);
    if (!romRegion.isEmpty())
        metaParts << QString("<b>Region:</b> %1").arg(romRegion);
    m_roomMetaLabel->setText(metaParts.join("  ·  "));

    // Sync the delay / prediction combos with the authoritative room state.
    // Suppress the currentIndexChanged signal during the assignment so the
    // combo doesn't immediately re-send an UPDATE for the same value we
    // just received from the server.
    if (m_delayCombo && m_predictionCombo && m_pacingCombo)
    {
        static const QString tipDisabledNotHost = QStringLiteral(
            "Only the host can change rollback settings.");
        static const QString tipDisabledMidMatch = QStringLiteral(
            "Can't change settings during a match.");

        const bool editable = iAmHost && state == "waiting";
        m_suppressSettingsSignal = true;
        // Look up the index for the server-supplied value via findData
        // since the combo's index no longer maps 1:1 to the value (0/Auto is
        // not a real value). Falls back to the floor entry if the server
        // somehow handed us a value we don't expose.
        auto pickIndex = [](QComboBox* combo, int value) {
            const int idx = combo->findData(value);
            return idx >= 0 ? idx : 0;
        };
        // Show the "Auto"/"Default" entry whenever the value is auto-driven, so
        // every seat displays the same thing. The host trusts their own local
        // m_*Auto (the server echo of the resolved number must not knock their
        // combo off Auto); non-hosts trust the room's auto flags. When not auto,
        // everyone shows the concrete resolved value.
        const bool showDelayAuto = editable ? m_delayAuto : roomDelayAuto;
        const bool showPredAuto  = editable ? m_predictionAuto : roomPredictionAuto;
        if (showDelayAuto)
            m_delayCombo->setCurrentIndex(m_delayCombo->findData(0));
        else
            m_delayCombo->setCurrentIndex(pickIndex(m_delayCombo, delay));
        if (showPredAuto)
            m_predictionCombo->setCurrentIndex(m_predictionCombo->findData(0));
        else
            m_predictionCombo->setCurrentIndex(pickIndex(m_predictionCombo, prediction));
        // Pacing is a plain 2-choice value (no Auto): mirror the room's value on
        // every seat.
        m_pacingCombo->setCurrentIndex(pickIndex(m_pacingCombo, pacing));
        // Keep the delay "Auto" entry's label showing the live resolved value.
        // (Prediction's "Default" entry is a plain, fixed label.)
        setAutoComboLabel(m_delayCombo, delay);
        m_delayCombo->setEnabled(editable);
        m_predictionCombo->setEnabled(editable);
        m_pacingCombo->setEnabled(editable);

        const QString delayTip = editable
            ? m_delayCombo->property("originalTip").toString()
            : (iAmHost ? tipDisabledMidMatch : tipDisabledNotHost);
        const QString predTip = editable
            ? m_predictionCombo->property("originalTip").toString()
            : (iAmHost ? tipDisabledMidMatch : tipDisabledNotHost);
        const QString pacingTip = editable
            ? m_pacingCombo->property("originalTip").toString()
            : (iAmHost ? tipDisabledMidMatch : tipDisabledNotHost);
        m_delayCombo->setToolTip(delayTip);
        m_predictionCombo->setToolTip(predTip);
        m_pacingCombo->setToolTip(pacingTip);
        m_suppressSettingsSignal = false;
    }

    // Broadcasting is host-only — one authoritative stream per match. Hide the
    // option for non-hosts (and clear any stale check) so only the host can arm
    // it; the server enforces the same rule on BROADCAST_BEGIN. "Record game"
    // stays available to everyone (each player saves their own local .krec).
    if (m_broadcastCheck)
    {
        m_broadcastCheck->setVisible(iAmHost);
        if (!iAmHost && m_broadcastCheck->isChecked())
            m_broadcastCheck->setChecked(false);
    }

    // ── Seats ──
    QVector<bool> filled(4, false);
    for (const auto& v : players)
    {
        const auto p = v.toObject();
        const int slot = p.value("slot").toInt();
        if (slot < 1 || slot > 4) continue;
        const QString user = p.value("username").toString();
        const quint64 uid = static_cast<quint64>(p.value("userId").toDouble());
        const bool slotIsHost = (uid == hostId);
        const bool slotIsSelf = (uid == m_client->selfUserId());
        const int pingMs = slotIsSelf ? -1 : m_client->measuredPingMs(uid);
        // The host can remove any seated player except themselves (the host seat).
        const bool canKick = iAmHost && !slotIsHost;
        m_seats[slot - 1].userId = uid;
        renderSeatFilled(m_seats[slot - 1], user, slotIsHost, slotIsSelf, pingMs, canKick);
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
            // an RTT instead of waiting on the next probe tick — and so the host's
            // Auto delay resolves from real ping before the match begins.
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

    // Start button gating (host-only): waiting, 2+ seated, ping from everyone.
    refreshStartButton();
}

void RollbackLobbyDialog::refreshStartButton()
{
    if (!m_startBtn)
        return;

    const quint64 selfId = m_client ? m_client->selfUserId() : 0;
    const bool iAmHost = (m_client && m_currentRoomHostId == selfId);
    const bool waiting = (m_currentRoomState == "waiting");

    int seated = 0;
    int peersAwaitingPing = 0;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0)
            continue;
        ++seated;
        // We never ping ourselves; every *other* seated player needs a measured
        // ping before the host may start, so the match opens on real latency
        // (and the Auto frame-delay has resolved from it).
        if (s.userId != selfId && m_client && m_client->measuredPingMs(s.userId) < 0)
            ++peersAwaitingPing;
    }

    const bool enoughPlayers = seated >= 2;
    const bool pingsReady    = (peersAwaitingPing == 0);
    const bool canStart      = iAmHost && waiting && enoughPlayers && pingsReady;

    m_startBtn->setEnabled(canStart);
    m_startBtn->setToolTip(
        !iAmHost         ? QStringLiteral("Only the host can start the game.")
        : !waiting       ? QStringLiteral("Already in a match.")
        : !enoughPlayers ? QStringLiteral("Need at least 2 players to start.")
        : !pingsReady    ? QStringLiteral("Measuring ping to all players…")
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
    m_client->reportMatchConnected(m_currentMatchId, 0);
    appendChatSystemLine(CHANNEL_ROOM, "Match started — playing now.");
}

void RollbackLobbyDialog::notifyEmulationFinished()
{
    // Always close out a broadcast if one is running, even on paths that fall
    // through the early-return below (it flushes the tail + sends BROADCAST_END).
    stopBroadcast();

    if (!m_emulationActive && !m_awaitingEmulationStart) return;
    const quint64 matchId = m_currentMatchId;
    m_emulationActive = false;
    m_awaitingEmulationStart = false;

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Lobby→notifyEmulationFinished matchId=%llu",
            (unsigned long long)matchId);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }
    if (matchId != 0)
        m_client->reportMatchFinished(matchId);
    m_currentMatchId = 0;
    if (m_dropBtn) m_dropBtn->setEnabled(false);

    // Re-open the UDP anchor so the next MATCH_BEGIN can hand us a fresh
    // local port. Without this, localUdpPort() returns 0 (socket is aborted
    // after onMatchBegin's releaseUdpAnchor), GekkoNet binds to a random
    // kernel-picked port for match #2, and peers send to the stale port from
    // match #1 — black screen on every match after the first.
    if (m_client) m_client->reopenUdpAnchor();
}

void RollbackLobbyDialog::onMatchPeerLeft(quint64 matchId, quint64 userId, int slot, const QString& reason)
{
    Q_UNUSED(matchId);

    // The lobby server already knows this peer is gone, so tell the rollback
    // engine to drop their actor immediately. Without this, GekkoNet only sees
    // the peer stop sending UDP input and stalls the match ~5 s waiting out its
    // disconnect timeout before substituting idle input. The request is queued
    // and applied on the emulation thread at the next frame.
    if (slot > 0)
        rmgk_gekko::request_disconnect_player(slot);

    QString who = QString::number(userId);
    if (m_client)
    {
        const auto& users = m_client->users();
        const auto it = users.find(userId);
        if (it != users.end() && !it->username.isEmpty())
            who = it->username;
    }
    const QString suffix = reason.isEmpty() ? QString("left") : reason;
    const QString line = slot > 0
        ? QString("%1 (P%2) dropped (%3) — controller %2 will go idle.")
              .arg(who).arg(slot).arg(suffix)
        : QString("%1 dropped (%2).").arg(who, suffix);
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
    if (!m_client || m_currentRoomId == 0)
        return;

    const quint64 selfId = m_client->selfUserId();
    for (const auto& s : m_seats)
    {
        if (s.userId == 0 || s.userId == selfId)
            continue;
        m_client->requestPingProbe(s.userId);
    }
}

void RollbackLobbyDialog::onPingMeasured(quint64 userId, int rttMs)
{
    // Only redraw the seat that just got a measurement — avoids churning the
    // whole room view on every probe response.
    for (auto& s : m_seats)
    {
        if (s.userId != userId || !s.metaLabel)
            continue;
        s.metaLabel->setText(seatMetaHtml(s.slot, s.isHost, rttMs, isDarkTheme()));
        break;
    }

    // Auto delay follows ping — re-resolve when a fresh RTT lands. The helper
    // only acts when we're the host of a waiting room with Auto selected.
    if (m_delayAuto)
        applyHostRoomSettings(false);

    // A peer's first ping may be what unblocks the host's Start button.
    refreshStartButton();
}

int RollbackLobbyDialog::worstSeatPingMs() const
{
    if (!m_client) return -1;
    const quint64 selfId = m_client->selfUserId();
    int worst = -1;
    for (const auto& s : m_seats)
    {
        if (s.userId == 0 || s.userId == selfId)
            continue;
        const int p = m_client->measuredPingMs(s.userId);
        if (p > worst) worst = p; // tune against the worst host↔peer link
    }
    return worst;
}

void RollbackLobbyDialog::applyHostRoomSettings(bool force)
{
    if (m_currentRoomId == 0 || !m_client || !m_delayCombo || !m_predictionCombo || !m_pacingCombo)
        return;
    // The delay combo is enabled only for the host of a waiting room, so its
    // enabled state doubles as the "I'm allowed to push settings now" gate.
    if (!m_delayCombo->isEnabled())
        return;

    const int effDelay = m_delayAuto
        ? autoFrameDelayForPing(worstSeatPingMs())
        : m_delayCombo->currentData().toInt();
    const int effPred = m_predictionAuto
        ? kAutoPredictionWindow
        : m_predictionCombo->currentData().toInt();
    const int effPacing = m_pacingCombo->currentData().toInt() == 1 ? 1 : 0;

    // Delay's "Auto" entry shows the resolved value, e.g. "Auto (4 f)".
    // Prediction's "Default" entry stays a plain label (always 7, not ping-based).
    if (m_delayAuto) setAutoComboLabel(m_delayCombo, effDelay);

    if (!force && effDelay == m_currentRoomDelay && effPred == m_currentRoomPrediction &&
        effPacing == m_currentRoomPacing)
        return;

    m_client->updateRoomSettings(effDelay, effPred, effPacing, m_delayAuto, m_predictionAuto);

    // Persist the concrete resolved values (never the Auto sentinel 0) so
    // CreateRoomDialog seeds a valid delay/prediction for the next room. Pacing
    // also rides the engine setting so a freshly created room defaults to it.
    QSettings s("RMG-K", "n02");
    s.beginGroup("Lobby/CreateRoom");
    s.setValue("Delay",      effDelay);
    s.setValue("Prediction", effPred);
    s.endGroup();
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, effPacing);
}

void RollbackLobbyDialog::onRoomLeft(const QString& reason)
{
    if (m_emulationActive || m_awaitingEmulationStart)
        emit closeMatchRequested();

    if (m_pingProbeTimer) m_pingProbeTimer->stop();
    for (auto& s : m_seats)
        s.userId = 0;

    m_currentRoomId = 0;
    m_currentRoomGame.clear();
    m_currentRoomRegion.clear();
    m_currentRoomState.clear();
    m_currentRoomDelay = 2;
    m_currentRoomPrediction = 7;
    m_currentRoomPacing = 0;
    m_currentRoomHostId = 0;
    m_currentMatchId = 0;
    m_awaitingEmulationStart = false;
    m_emulationActive = false;
    m_knownSeatedUsers.clear();
    m_roomSeatsSeen = false;

    if (m_client) m_client->reopenUdpAnchor();

    if (m_chatViewRoom) m_chatViewRoom->clear();
    if (m_roomChatInput) m_roomChatInput->setEnabled(false);

    switchToRoomsView();
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn)  m_dropBtn->setEnabled(false);
    if (m_leaveBtn) m_leaveBtn->setEnabled(true);

    const bool connected = (m_client->state() == LobbyClient::ConnectionState::Connected);
    if (m_createRoomBtn) m_createRoomBtn->setEnabled(connected);
    if (m_quickMatchBtn) m_quickMatchBtn->setEnabled(connected);

    updateInRoomBanner();
    onRoomListChanged();

    // Tell the user why they left, if it wasn't their own doing.
    if (reason == QLatin1String("kicked"))
        QMessageBox::information(this, "Removed from match",
            "You were removed from the lobby by the host.");
    else if (reason == QLatin1String("host_left"))
        QMessageBox::information(this, "Room closed",
            "The host closed the room.");
}

// ──────────────────────────────────────────────────────────────────────
//  Chat
// ──────────────────────────────────────────────────────────────────────

void RollbackLobbyDialog::onChatSendClicked()
{
    const QString text = m_chatInput->text().trimmed();
    if (text.isEmpty()) return;
    if (handleSlashCommand(CHANNEL_LOBBY, text)) { m_chatInput->clear(); return; }
    m_client->sendChat(CHANNEL_LOBBY, text);
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
    const auto ts = QDateTime::fromMSecsSinceEpoch(msg.serverTimeMs).toString("hh:mm");
    const QString line = QString("[%1] <b>%2:</b> %3")
        .arg(ts, msg.fromUsername.toHtmlEscaped(), msg.message.toHtmlEscaped());
    appendChatLine(msg.channel, line);

    // Mirror room chat into the in-game overlay. The server relays room chat to
    // every member (including the sender), so forwarding all room messages —
    // own and remote — makes both show on the OSD whether they were typed in
    // the dialog or via the in-game chat key.
    if (msg.channel == CHANNEL_ROOM)
    {
        emit roomChatReceived(msg.fromUsername, msg.message);
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

void RollbackLobbyDialog::appendChatLine(const QString& channel, const QString& text)
{
    QTextEdit* target = m_chatViewLobby;
    if (channel == CHANNEL_ROOM && m_chatViewRoom)
        target = m_chatViewRoom;
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
            QMessageBox::information(this, "Already in a room",
                "Leave your current room before searching for a match.");
            return;
        }

        const QVariantMap romData = selectedBrowseRom();
        const QString romName = romData.value("name").toString();
        const QString romMd5  = romData.value("md5").toString();
        if (romMd5.isEmpty())
        {
            QMessageBox::information(this, "Select a game",
                "Pick a game from the dropdown before searching — Quick Match "
                "only pairs you with players searching for that same ROM.");
            return;
        }
        qInfo() << "lobby: quick match search" << "game" << romName << "md5" << romMd5;
        m_client->quickMatchJoin(romName, romMd5);
    }
}

void RollbackLobbyDialog::onQuickMatchStatusChanged(bool searching, int queueSize)
{
    m_quickMatchActive = searching;
    if (!m_quickMatchBtn) return;

    if (searching)
    {
        const QString suffix = queueSize > 1
            ? QString(" (%1 in queue)").arg(queueSize)
            : QString(" (searching…)");
        m_quickMatchBtn->setText("✕  Cancel Quick Match" + suffix);
        if (m_createRoomBtn) m_createRoomBtn->setEnabled(false);
    }
    else
    {
        m_quickMatchBtn->setText("⚡  Quick Match");
        if (m_createRoomBtn)
            m_createRoomBtn->setEnabled(m_client->state() == LobbyClient::ConnectionState::Connected
                                        && m_currentRoomId == 0);
    }
}

// ── Broadcaster ───────────────────────────────────────────────────────

void RollbackLobbyDialog::startBroadcast(quint64 matchId)
{
    if (m_broadcasting) return;
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
    m_client->sendBroadcastBegin(matchId);
    m_broadcastDrainTimer->start();
    appendChatSystemLine(CHANNEL_ROOM, "Live Replay on — others can watch this match.");
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

    // Periodic savestate keyframe: ask the engine for one at a fixed interval (it
    // snapshots + holds it until confirmed against rollback), then poll for the
    // result and upload it so late spectators can jump near the live edge instead
    // of replaying from frame 0. Runs even when no krec chunk drained this tick.
    //
    // EXPERIMENT (2026-06-29): keyframes disabled to measure pure replay-from-frame-0.
    // With no keyframe uploaded, the server falls back to streaming the full spool from
    // frame 0 (broadcast.go spectateStart), so the spectator replays the whole match
    // (deterministic — boot-replay RNG is correct) and fast-forwards. Tests whether
    // video-on catch-up is fast enough to make keyframes unnecessary. Flip to re-enable.
    constexpr bool kSpectateKeyframesEnabled = false;
    if (kSpectateKeyframesEnabled)
    {
        const qint64 kKeyframeIntervalMs = 60000; // ~once a minute (knob)
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastKeyframeRequestMs == 0 || nowMs - m_lastKeyframeRequestMs >= kKeyframeIntervalMs)
        {
            rmgk_gekko::request_keyframe();
            m_lastKeyframeRequestMs = nowMs;
        }
        std::vector<unsigned char> kf;
        int kfFrame = -1;
        if (rmgk_gekko::take_keyframe(kf, kfFrame) && !kf.empty() && kfFrame >= 0)
        {
            m_client->sendBroadcastKeyframe(m_broadcastMatchId,
                QByteArray(reinterpret_cast<const char*>(kf.data()), static_cast<int>(kf.size())),
                kfFrame);
        }
    }
}

// ── Spectator ─────────────────────────────────────────────────────────

void RollbackLobbyDialog::beginSpectate(quint64 matchId, const QString& gameName)
{
    if (m_spectatingMatchId != 0) return; // already spectating
    m_spectatingMatchId = matchId;
    m_spectateStreamArmed = false; // wait for this subscribe's SPECTATE_BEGIN before accepting data
    m_client->startSpectate(matchId);
    emit spectateLaunch(matchId, gameName);
    appendChatSystemLine(CHANNEL_LOBBY, "Connecting to live replay…");
}

void RollbackLobbyDialog::stopSpectating()
{
    if (m_spectatingMatchId == 0) return;
    m_client->stopSpectate(m_spectatingMatchId);
    m_spectatingMatchId = 0;
    m_spectateStreamArmed = false;
}

void RollbackLobbyDialog::onSpectateBegan(quint64 matchId)
{
    if (matchId != m_spectatingMatchId) return;
    // The session boundary: everything on the wire before this is leftover from a
    // prior watch of this same match. Arm the stream now; keyframe + tail follow.
    m_spectateStreamArmed = true;
    appendChatSystemLine(CHANNEL_LOBBY, "Watching — buffering the match…");
}

void RollbackLobbyDialog::onSpectateData(quint64 matchId, const QByteArray& bytes, int liveFrame)
{
    if (matchId != m_spectatingMatchId) return;
    if (!m_spectateStreamArmed) return; // stale chunk from a previous watch — drop it
    emit spectateStreamData(bytes, liveFrame);
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
    emit spectateStreamClosed(reason);
}

void RollbackLobbyDialog::onSpectateFailed(quint64 matchId, const QString& reason)
{
    if (matchId != m_spectatingMatchId) return;
    m_spectatingMatchId = 0;
    const QString human =
        reason == "not_broadcasting" ? QStringLiteral("That live replay isn't available anymore.") :
        reason == "ended"            ? QStringLiteral("That live replay just ended.") :
                                       QStringLiteral("Couldn't watch: %1").arg(reason);
    QMessageBox::information(this, "Live Replay", human);
    emit spectateStreamClosed(reason);
}

void RollbackLobbyDialog::abortMatchStart(const QString& reason)
{
    if (!reason.isEmpty())
    {
        appendChatSystemLine(CHANNEL_ROOM, reason);
        CoreAddCallbackMessage(CoreDebugMessageType::Error, reason.toUtf8().constData());
    }
    applyRoomStateBadge("Pre-match start failed", QString(statusColors().fail));

    const quint64 matchId = m_currentMatchId;
    m_awaitingEmulationStart = false;
    m_currentMatchId = 0;

    // Cancel any launch the MainWindow may already be spinning up, and tell the
    // server the match is over so the room flips back to "waiting" for everyone
    // (a follow-up ROOM_STATE re-enables Start and clears the badge).
    emit closeMatchRequested();
    if (matchId != 0 && m_client)
        m_client->reportMatchFinished(matchId);

    if (m_dropBtn) m_dropBtn->setEnabled(false);

    // No-op while the anchor is still open (these failures all happen before
    // releaseUdpAnchor); guards the rare path where it was already torn down so
    // ping probing keeps working without a relaunch.
    if (m_client) m_client->reopenUdpAnchor();
}

void RollbackLobbyDialog::onMatchBegin(quint64 matchId, const QList<LobbyClient::LobbyMatchPeer>& peers)
{
    const QString line = QString("Match #%1 starting with %2 player(s)").arg(matchId).arg(peers.size());
    appendChatSystemLine(CHANNEL_LOBBY, line);
    appendChatSystemLine(CHANNEL_ROOM,  line);

    // The host announces the rollback delay the room is about to play on, so
    // everyone sees it in chat (and on the in-game overlay) — especially useful
    // when Auto resolved it from ping. One authoritative message, gated to the
    // host so a 4-player match doesn't print it four times.
    if (m_client && m_currentRoomHostId == m_client->selfUserId())
    {
        QString delayMsg;
        if (m_delayAuto)
        {
            const int worst = worstSeatPingMs();
            delayMsg = worst >= 0
                ? QStringLiteral("Frame delay: %1 (auto, from %2 ms ping) · prediction %3")
                      .arg(m_currentRoomDelay).arg(worst).arg(m_currentRoomPrediction)
                : QStringLiteral("Frame delay: %1 (auto) · prediction %2")
                      .arg(m_currentRoomDelay).arg(m_currentRoomPrediction);
        }
        else
        {
            delayMsg = QStringLiteral("Frame delay: %1 · prediction %2")
                           .arg(m_currentRoomDelay).arg(m_currentRoomPrediction);
        }
        m_client->sendChat(CHANNEL_ROOM, delayMsg);
    }

    m_currentMatchId = matchId;
    m_awaitingEmulationStart = true;

    // Disable start; enable Drop now — MATCH_BEGIN arrives *after* the
    // ROOM_STATE that flipped us to in_game, so onRoomStateChanged ran before
    // the awaiting flag was set and left Drop disabled.
    if (m_startBtn) m_startBtn->setEnabled(false);
    if (m_dropBtn)  m_dropBtn->setEnabled(true);
    applyRoomStateBadge("Connecting…", stateHex("connecting", isDarkTheme()));

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        QString("Rollback lobby MATCH_BEGIN: match=%1 peers=%2 self=%3 roomGame='%4' delay=%5 prediction=%6 anchorPort=%7")
            .arg(matchId)
            .arg(peers.size())
            .arg(m_client->selfUserId())
            .arg(m_currentRoomGame)
            .arg(m_currentRoomDelay)
            .arg(m_currentRoomPrediction)
            .arg(m_client->localUdpPort())
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
        abortMatchStart("Match start failed: missing local peer.");
        return;
    }

    // Make this room's "Record game" checkbox authoritative for the match, in
    // case another netplay dialog touched the shared flag since the box was built.
    if (m_recordCheck)
    {
        n02_kaillera_recording_enabled = m_recordCheck->isChecked();
    }

    // If broadcasting, arm the krec tee and announce to the server now — before
    // MainWindow calls recordingOpen, so the .krec header is captured too. Host-
    // only: the broadcast checkbox is hidden for non-hosts, and the server also
    // rejects a non-host BROADCAST_BEGIN — this is the matching client guard.
    const bool iAmHostForBroadcast =
        (m_client && m_currentRoomHostId == m_client->selfUserId());
    if (iAmHostForBroadcast && m_broadcastCheck && m_broadcastCheck->isChecked())
    {
        n02_kaillera_recording_enabled = true; // broadcasting requires the krec
        startBroadcast(matchId);
    }

    // Capture seated player names (slot-indexed) for the .krec header, the same
    // way the p2p / kaillera paths fill recording_player_names before a recording
    // opens. MainWindow opens the file when it launches the match; this is
    // harmless when "Record game" is off (the open self-gates on the flag).
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

    const quint16 localPort = m_client->localUdpPort();

    for (const auto& p : peers)
    {
        if (p.userId == selfId)
            continue;

        QString endpointIp = p.publicIp;
        QString endpointKind = "public";

        if (!local.publicIp.isEmpty() &&
            !p.publicIp.isEmpty() &&
            local.publicIp == p.publicIp &&
            !p.localIp.isEmpty())
        {
            endpointIp = p.localIp;
            endpointKind = "local";
        }

        const QString selectedLine = QString("Rollback lobby selected endpoint: peerUser=%1 slot=%2 kind=%3 endpoint=%4:%5 localPublic=%6 peerPublic=%7 peerLocal=%8")
            .arg(p.userId)
            .arg(p.slot)
            .arg(endpointKind)
            .arg(endpointIp)
            .arg(p.publicPort)
            .arg(local.publicIp)
            .arg(p.publicIp)
            .arg(p.localIp);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, selectedLine.toUtf8().constData());

        remotePeers << QString("%1,%2,%3").arg(p.slot).arg(endpointIp).arg(p.publicPort);
    }

    if (remotePeers.isEmpty())
    {
        abortMatchStart("Match start failed: missing remote peer.");
        return;
    }

    // Punch peer NATs from the anchor socket before handing the port to
    // GekkoNet. Both peers receive MATCH_BEGIN within ~RTT of each other, so
    // both fire while the other's anchor is still open — opens the NAT
    // mapping so GekkoNet's first frame doesn't have to eat the handshake.
    m_client->punchPeerEndpoints(peers);

    const QString localRomFile = localRomPathForMd5(m_currentRoomMd5);
    if (localRomFile.isEmpty())
    {
        abortMatchStart(QString("Match start failed: you don't have the ROM for %1.").arg(m_currentRoomGame));
        return;
    }

    appendChatSystemLine(CHANNEL_ROOM, "Synchronizing pre-match settings...");
    QString prematchError;
    if (!m_client->syncPrematchManifest(peers, local.slot, localRomFile, prematchError))
    {
        abortMatchStart(prematchError.isEmpty() ? QStringLiteral("Pre-match sync failed.") : prematchError);
        return;
    }
    appendChatSystemLine(CHANNEL_ROOM, "Pre-match sync complete.");

    m_client->releaseUdpAnchor();

    {
        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "Lobby→matchReady (deferred 100ms): game='%s' peers=%d (%s) localPort=%u slot=%d delay=%d pred=%d",
            m_currentRoomGame.toUtf8().constData(),
            int(remotePeers.size()),
            remotePeers.join("; ").toUtf8().constData(),
            unsigned(localPort),
            local.slot,
            m_currentRoomDelay,
            m_currentRoomPrediction);
        CoreAddCallbackMessage(CoreDebugMessageType::Info, buf);
    }

    // Pacing isn't threaded through matchReady — the engine reads it from the
    // Rollback_PacingMode setting at session start. Set it from the room's value
    // here so the match definitively runs the host-selected model.
    CoreSettingsSetValue(SettingsID::Rollback_PacingMode, m_currentRoomPacing);

    // Defer the emit so the OS finishes releasing the anchor port before
    // GekkoNet attempts to bind it. 100ms is plenty on Windows for an UDP
    // socket teardown to complete; without this delay the bind races.
    const QString gameName  = m_currentRoomGame;
    const QString romFile   = localRomFile; // resolved above by MD5 — robust for off-database ROMs
    const int localPortInt  = int(localPort);
    const int slot          = local.slot;
    const int delay         = m_currentRoomDelay;
    const int prediction    = m_currentRoomPrediction;
    QTimer::singleShot(100, this, [this, gameName, romFile, remotePeers, localPortInt, slot, delay, prediction]() {
        emit matchReady(gameName, romFile, remotePeers, localPortInt, slot, delay, prediction);
    });
}

#endif // NETPLAY
