/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "NetplayModule.hpp"
#include "../RMG-Core/Kaillera.hpp"
#include "../RMG-Core/rmgk_gekko.hpp"

#include <quickjs.h>

namespace RMGScript {

// ─── helpers ─────────────────────────────────────────────────────────────────

// Returns true if a Kaillera server game is currently running.
static bool KailleraGameActive()
{
    return CoreGetKailleraPlayerNumber() > 0;
}

// Returns true if a GekkoNet rollback P2P session is active.
static bool RollbackActive()
{
    return rmgk_gekko::is_netplay_session_active();
}

// ─── netplay.isActive() ──────────────────────────────────────────────────────

static JSValue Netplay_IsActive(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewBool(ctx, KailleraGameActive() || RollbackActive());
}

// ─── netplay.getMode() ───────────────────────────────────────────────────────

// Returns "kaillera", "rollback", or "none".
static JSValue Netplay_GetMode(JSContext* ctx, JSValue, int, JSValue*)
{
    if (RollbackActive())
        return JS_NewString(ctx, "rollback");
    if (KailleraGameActive())
        return JS_NewString(ctx, "kaillera");
    return JS_NewString(ctx, "none");
}

// ─── netplay.getPlayerNumber() ───────────────────────────────────────────────

static JSValue Netplay_GetPlayerNumber(JSContext* ctx, JSValue, int, JSValue*)
{
    if (RollbackActive())
        return JS_NewInt32(ctx, rmgk_gekko::get_local_player());
    return JS_NewInt32(ctx, CoreGetKailleraPlayerNumber());
}

// ─── netplay.getNumPlayers() ─────────────────────────────────────────────────

static JSValue Netplay_GetNumPlayers(JSContext* ctx, JSValue, int, JSValue*)
{
    if (RollbackActive())
        return JS_NewInt32(ctx, rmgk_gekko::get_num_players());
    return JS_NewInt32(ctx, CoreGetKailleraNumPlayers());
}

// ─── netplay.getFrameDelay() ─────────────────────────────────────────────────

static JSValue Netplay_GetFrameDelay(JSContext* ctx, JSValue, int, JSValue*)
{
    if (RollbackActive())
        return JS_NewInt32(ctx, rmgk_gekko::get_local_delay());
    return JS_NewInt32(ctx, CoreGetKailleraFrameDelay());
}

// ─── netplay.getPredictionWindow() ───────────────────────────────────────────

static JSValue Netplay_GetPredictionWindow(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewInt32(ctx, rmgk_gekko::get_prediction_window());
}

// ─── netplay.isRollingBack() ─────────────────────────────────────────────────

static JSValue Netplay_IsRollingBack(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewBool(ctx, rmgk_gekko::is_rolling_back());
}

// ─── netplay.getFramesAhead() ────────────────────────────────────────────────

// GekkoNet speed-pacing factor — how many frames the local client is running
// ahead of the remote. Positive = running fast, negative = running slow.
// Always 0 for Kaillera server sessions.
static JSValue Netplay_GetFramesAhead(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewFloat64(ctx, static_cast<double>(rmgk_gekko::get_frames_ahead()));
}

// ─── netplay.getPing() / getAvgPing() / getJitter() ──────────────────────────

// getPing() returns the transport-layer RTT — the same value shown in the
// netplay window. getAvgPing()/getJitter() use GekkoNet's protocol-level stats.
static JSValue Netplay_GetPing(JSContext* ctx, JSValue, int, JSValue*)
{
    return JS_NewInt32(ctx, rmgk_gekko::get_transport_ping());
}

static JSValue Netplay_GetAvgPing(JSContext* ctx, JSValue, int, JSValue*)
{
    float avg = 0, jitter = 0;
    rmgk_gekko::get_network_stats(avg, jitter);
    return JS_NewFloat64(ctx, static_cast<double>(avg));
}

static JSValue Netplay_GetJitter(JSContext* ctx, JSValue, int, JSValue*)
{
    float avg = 0, jitter = 0;
    rmgk_gekko::get_network_stats(avg, jitter);
    return JS_NewFloat64(ctx, static_cast<double>(jitter));
}

// ─── netplay.getPlayerName(n) ────────────────────────────────────────────────

static JSValue Netplay_GetPlayerName(JSContext* ctx, JSValue, int argc, JSValue* argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "getPlayerName(playerNumber): expected a number");
    int32_t player;
    if (JS_ToInt32(ctx, &player, argv[0]) != 0) return JS_EXCEPTION;
    std::string name = CoreNetplayGetPlayerName(player);
    return JS_NewStringLen(ctx, name.c_str(), name.size());
}

// ─── Registration ─────────────────────────────────────────────────────────────

void RegisterNetplayModule(JSContext* ctx)
{
    JSValue netplay = JS_NewObject(ctx);

#define SET(name, fn, nargs) \
    JS_SetPropertyStr(ctx, netplay, name, JS_NewCFunction(ctx, fn, name, nargs))

    SET("isActive",       Netplay_IsActive,       0);
    SET("getMode",        Netplay_GetMode,         0);
    SET("getPlayerNumber",Netplay_GetPlayerNumber, 0);
    SET("getNumPlayers",  Netplay_GetNumPlayers,   0);
    SET("getFrameDelay",       Netplay_GetFrameDelay,       0);
    SET("getPredictionWindow", Netplay_GetPredictionWindow, 0);
    SET("isRollingBack",       Netplay_IsRollingBack,       0);
    SET("getFramesAhead", Netplay_GetFramesAhead,  0);
    SET("getPing",        Netplay_GetPing,         0);
    SET("getAvgPing",     Netplay_GetAvgPing,      0);
    SET("getJitter",      Netplay_GetJitter,       0);
    SET("getPlayerName",  Netplay_GetPlayerName,   1);

#undef SET

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "netplay", netplay);
    JS_FreeValue(ctx, global);
}

} // namespace RMGScript
