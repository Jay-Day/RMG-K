/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ReplayMemory.hpp"
#include "m64p/Api.hpp"

#include <cmath>
#include <cstring>

namespace
{
constexpr uint32_t ADDR_CURRENT_SCREEN   = 0x800A4AD0;
constexpr uint32_t ADDR_MATCH_RESET_FLAG = 0x800A4AE2;
constexpr uint32_t ADDR_MATCH_INFO_PTR   = 0x800A50E8;

constexpr uint32_t MI_GAME_MODE          = 0x00;
constexpr uint32_t MI_STAGE_ID           = 0x01;
constexpr uint32_t MI_TEAMS_ENABLED      = 0x02;
constexpr uint32_t MI_GAME_TYPE          = 0x03;
constexpr uint32_t MI_TIME_LIMIT         = 0x06;
constexpr uint32_t MI_STOCK_COUNT        = 0x07;
constexpr uint32_t MI_HANDICAP_MODE      = 0x08;
constexpr uint32_t MI_DAMAGE_RATIO       = 0x0B;
constexpr uint32_t MI_GAME_STATUS        = 0x11;
constexpr uint32_t MI_ITEM_FREQUENCY     = 0x1C;
constexpr uint32_t MI_PORT_STRUCT_BASE   = 0x20;
constexpr uint32_t MI_PORT_STRUCT_STRIDE = 0x74;

constexpr uint32_t PORT_SLOT_TYPE        = 0x02;
constexpr uint32_t PORT_CHARACTER_ID     = 0x03;
constexpr uint32_t PORT_COSTUME_ID       = 0x06;
constexpr uint32_t PORT_TEAM_COLOR       = 0x07;
constexpr uint32_t PORT_STOCKS_REMAINING = 0x0B;
constexpr uint32_t PORT_COMBO_DAMAGE     = 0x50;
constexpr uint32_t PORT_COMBO_HIT_COUNT  = 0x54;
constexpr uint32_t PORT_PLAYER_OBJECT    = 0x58;

constexpr uint32_t PLAYER_OBJECT_TO_STRUCT = 0x84;

constexpr uint32_t PS_ACTION_FRAME_COUNTER = 0x1C;
constexpr uint32_t PS_ACTION_STATE_ID      = 0x24;
constexpr uint32_t PS_DAMAGE_PERCENT       = 0x2C;
constexpr uint32_t PS_FACING_DIRECTION     = 0x44;
constexpr uint32_t PS_VELOCITY_X           = 0x48;
constexpr uint32_t PS_VELOCITY_Y           = 0x4C;
constexpr uint32_t PS_POSITION_PTR         = 0x78;
// A single byte (u8), NOT a u32 - confirmed via the decomp, where the very
// next byte (+0x149) is independently named `unk_ft_0x149`, a naming
// convention that literally encodes its own byte offset. +0x14A-0x14B are
// padding; +0x14C (PS_KINETIC_STATE, "ga"/Ground-Air bool below) is the
// next real field. An earlier version of this code read this as a u32,
// which - on a word-swapped emulator, where a *byte* read needs its
// address XORed with 3 to land correctly - explains the earlier symptom
// exactly (a wrong-width/wrong-swizzle read landing on the 0x14A-0x14B
// padding instead, reading a constant 0 for an entire match, both ports).
// Resets to 0 on landing, so reading 0 through most of a grounded match is
// completely normal, not a sign this is still broken.
constexpr uint32_t PS_JUMPS_USED           = 0x148;
// FTAttributes* - per-character static move data, cached once per port at
// match start (jumps_max is per-character, not universal) rather than
// re-chased every frame. See PS_ATTRIBUTES_PTR's own read site.
constexpr uint32_t PS_ATTRIBUTES_PTR       = 0x9C8;
// s32, within FTAttributes - confirmed by two independent sources
// agreeing: counting FTAttributes' fields in the decomp (all 4-byte, no
// bitfields/padding) lands on +0x64, and Smash Remix's own ASM
// independently comments this exact read ("t0 = max jumps").
constexpr uint32_t FT_ATTR_MAX_JUMPS       = 0x64;
constexpr uint32_t PS_KINETIC_STATE        = 0x14C;
constexpr uint32_t PS_PROCESSED_BUTTONS    = 0x1BC;
constexpr uint32_t PS_STICK_X              = 0x1C2;
constexpr uint32_t PS_STICK_Y              = 0x1C3;
constexpr uint32_t PS_HURTBOX_STATE        = 0x5BB;
constexpr uint32_t PS_HITSTUN_COUNTER      = 0xB1A;
constexpr uint32_t PS_TEAM                 = 0x0C;
constexpr uint32_t PS_HANDICAP             = 0x12;
constexpr uint32_t PS_CPU_LEVEL            = 0x13;

// GObj (universal engine object) linked lists - independent of MatchInfo,
// fixed global head pointers. See smashremix docs/ram-map.md section 10.4 -
// confirmed against the real SSB64 decompilation (VetriTheRetri/ssb-decomp-re)
// after an earlier version of that doc misdocumented GOBJ_LINK_ID as a
// 32-bit "item ID" (it's actually 4 packed single bytes; only the first,
// link_id, matters here).
//
// gGCCommonLinks is a fixed 33-entry array of *per-link_id* list heads, not
// one shared list filtered by link_id - an earlier version of this function
// walked only gGCCommonLinks[4] (the Item list) and relied on a link_id
// filter to also pick out Weapons, but structurally that list can only ever
// contain Items; Weapons (fireballs, boomerang, charge shot, PK Fire/
// Thunder, ...) live on the separate list at gGCCommonLinks[5] and were
// never actually reachable that way. See ram-map.md section 10.4 (updated).
constexpr uint32_t ADDR_GC_COMMON_LINKS = 0x800466F0; // gGCCommonLinks[0]; list head for link_id N is this + N*4
constexpr uint32_t GOBJ_NEXT            = 0x04;
constexpr uint32_t GOBJ_LINK_ID         = 0x0C; // u8: 3 = Fighter, 4 = Item, 5 = Weapon
constexpr uint32_t GOBJ_OBJ_PTR         = 0x74; // -> DObj (position/scale)
constexpr uint32_t GOBJ_USER_DATA_PTR   = 0x84; // -> ITStruct* (Item) or WPStruct* (Weapon)
constexpr uint8_t  GOBJ_LINK_ID_ITEM    = 4;
constexpr uint8_t  GOBJ_LINK_ID_WEAPON  = 5;
// Both ITStruct and WPStruct happen to place `kind` at the same sub-offset
// (a coincidence of parallel struct design per the ram-map, not a rule).
constexpr uint32_t IT_OR_WP_STRUCT_KIND = 0x0C; // s32: ITKind or WPKind, per GOBJ_LINK_ID
constexpr uint32_t DOBJ_POSITION_X = 0x1C;
constexpr uint32_t DOBJ_POSITION_Y = 0x20;
constexpr uint32_t DOBJ_POSITION_Z = 0x24;

// ITStruct+0x08 (owner_gobj) is NOT usable to detect "currently held" -
// confirmed against the decomp's itMainSetFighterRelease(): owner_gobj is
// deliberately RETAINED across the throw/drop (needed later for damage/KO
// attribution) and is only cleared by the separate, not-always-called
// itMainClearOwnerStats(). It's non-NULL for essentially an item's entire
// lifetime, held or not - an earlier version of this code used
// `owner_gobj != NULL` as a "currently held" proxy, which silently
// swallowed every thrown/dropped item for its whole flight (see git
// history for the investigation). The real per-instance flag
// (ITStruct.is_hold, a single bit inside a packed bitfield run well past
// the embedded MPCollData) doesn't have a pinned-down offset yet.
//
// What IS available today without new offset work: while held, the engine
// re-parents the item's DObj onto the holding fighter's hand-bone joint
// (lbCommonEjectTreeDObj() undoes this exactly once, in the same release
// function, writing the item's real world coordinates back), so
// GOBJ_OBJ_PTR's position reads as a small/near-zero local offset instead
// of a world coordinate for as long as it's held. IsHeldItemPosition()
// below treats "still reads as ~(0,0,0)" as the proxy for "still held" -
// imperfect (a genuinely free item passing through world-origin on all
// three axes simultaneously would be misclassified for that one frame),
// but unlike the owner_gobj proxy this one actually flips at the right
// moment. See ram-map.md section 10.4.2.
constexpr float HELD_ITEM_POSITION_EPSILON = 10.0f;

bool IsHeldItemPosition(float x, float y, float z)
{
    return std::fabs(x) < HELD_ITEM_POSITION_EPSILON &&
        std::fabs(y) < HELD_ITEM_POSITION_EPSILON &&
        std::fabs(z) < HELD_ITEM_POSITION_EPSILON;
}

// Slippi caps its own per-frame item event count at 15 (see
// rmgk-replay-file-agent-prompt.md section 4.4); reused here as a sane
// per-frame budget, mainly to bound a corrupt/cyclic list to a fixed number
// of reads rather than looping until something crashes.
constexpr int ITEM_LIST_MAX_OBJECTS = 32;

// Stage hazards. See smashremix docs/ram-map.md section 10.3 (is Whispy
// blowing) and 10.3.1 (which direction) - Dream Land's live hazard state
// lives in a fixed global that is a *union* shared by every "common
// ground" stage; these exact offsets are only valid when the current
// stage is actually Dream Land.
constexpr uint8_t  STAGE_ID_DREAM_LAND       = 0x06;
constexpr uint32_t ADDR_PUPUPU_WHISPY_STATUS = 0x80131416; // gGRCommonStruct (0x801313F0) + 0x26, Dream Land's union view
constexpr uint8_t  WHISPY_STATUS_BLOW        = 4;          // grPupupuWhispyWindStatus::Blow
constexpr uint32_t ADDR_PUPUPU_WHISPY_LR     = 0x8013141A; // gGRCommonStruct + 0x2A - lr_players: 0 = blowing left, 1 = blowing right

// KSEG0, 8MB expansion-pak RDRAM window. A value outside this range means a
// pointer chase hit garbage - treat as "not currently available", not a crash.
bool IsValidRdramPointer(uint32_t ptr)
{
    return ptr >= 0x80000000u && ptr < 0x80800000u;
}

float ReadFloat(uint32_t address)
{
    uint32_t bits = m64p::Core.DebugMemRead32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Walks one gGCCommonLinks[linkId] list (Items or Weapons, never mixed - see
// ADDR_GC_COMMON_LINKS's doc comment), invoking `visitor(objectAddress,
// userData)` for every live entry whose own link_id byte actually matches
// (defensively skipping anything else, per that list's documented
// guarantee). `userData` is that GObj's ITStruct*/WPStruct* pointer
// (GOBJ_USER_DATA_PTR), NOT validated as a real RDRAM pointer here - callers
// that dereference it must check IsValidRdramPointer() themselves.
template <typename Visitor>
void WalkGObjLinkList(uint8_t linkId, Visitor&& visitor)
{
    const uint32_t listHead = ADDR_GC_COMMON_LINKS + static_cast<uint32_t>(linkId) * 4;
    uint32_t current = m64p::Core.DebugMemRead32(listHead);
    for (int i = 0; i < ITEM_LIST_MAX_OBJECTS && IsValidRdramPointer(current); i++)
    {
        const uint32_t next = m64p::Core.DebugMemRead32(current + GOBJ_NEXT);

        const uint8_t actualLinkId = m64p::Core.DebugMemRead8(current + GOBJ_LINK_ID);
        if (actualLinkId == linkId)
        {
            const uint32_t userData = m64p::Core.DebugMemRead32(current + GOBJ_USER_DATA_PTR);
            visitor(current, userData);
        }
        // else: this list is documented to only ever contain `linkId`
        // entries - skip defensively rather than trust unexpected data.

        current = next;
    }
}

// Chases matchStruct+0x58 -> playerObject+0x84 -> playerStruct - the same
// two-hop pattern ReadPortPlayerState() needs. Returns 0 if the first hop
// (matchStruct+0x58) isn't a valid pointer; otherwise returns the second
// hop's raw value as-is, WITHOUT validating it - the caller already calls
// IsValidRdramPointer() on the result immediately, so re-checking here
// would just be redundant.
uint32_t ResolvePlayerStruct(uint32_t matchInfoPtr, int port)
{
    const uint32_t base = matchInfoPtr + MI_PORT_STRUCT_BASE +
        static_cast<uint32_t>(port) * MI_PORT_STRUCT_STRIDE;

    const uint32_t playerObject = m64p::Core.DebugMemRead32(base + PORT_PLAYER_OBJECT);
    if (!IsValidRdramPointer(playerObject))
    {
        return 0;
    }

    return m64p::Core.DebugMemRead32(playerObject + PLAYER_OBJECT_TO_STRUCT);
}
} // namespace

namespace ReplayMemory
{
bool IsInVsMatchScreen(void)
{
    return m64p::Core.DebugMemRead8(ADDR_CURRENT_SCREEN) == 0x16;
}

MatchInfo ReadMatchInfo(void)
{
    MatchInfo info{};
    info.valid = false;

    uint32_t matchInfoPtr = m64p::Core.DebugMemRead32(ADDR_MATCH_INFO_PTR);
    if (!IsValidRdramPointer(matchInfoPtr))
    {
        return info;
    }

    info.valid             = true;
    info.matchInfoPtr      = matchInfoPtr;
    info.gameMode          = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_MODE);
    info.stageId            = m64p::Core.DebugMemRead8(matchInfoPtr + MI_STAGE_ID);
    info.gameType            = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_TYPE);
    info.timeLimitMinutes    = m64p::Core.DebugMemRead8(matchInfoPtr + MI_TIME_LIMIT);
    info.stockCountSetting   = m64p::Core.DebugMemRead8(matchInfoPtr + MI_STOCK_COUNT);
    info.damageRatio         = m64p::Core.DebugMemRead8(matchInfoPtr + MI_DAMAGE_RATIO);
    info.itemFrequency       = m64p::Core.DebugMemRead8(matchInfoPtr + MI_ITEM_FREQUENCY);
    info.gameStatus          = m64p::Core.DebugMemRead8(matchInfoPtr + MI_GAME_STATUS);
    info.matchWasReset       = m64p::Core.DebugMemRead8(ADDR_MATCH_RESET_FLAG) != 0;
    info.teamsEnabled        = m64p::Core.DebugMemRead8(matchInfoPtr + MI_TEAMS_ENABLED) != 0;
    info.handicapMode        = m64p::Core.DebugMemRead8(matchInfoPtr + MI_HANDICAP_MODE);
    return info;
}

PortMatchInfo ReadPortMatchInfo(uint32_t matchInfoPtr, int port)
{
    PortMatchInfo info{};
    uint32_t base = matchInfoPtr + MI_PORT_STRUCT_BASE +
        static_cast<uint32_t>(port) * MI_PORT_STRUCT_STRIDE;

    uint8_t slotType = m64p::Core.DebugMemRead8(base + PORT_SLOT_TYPE);
    info.slotType          = slotType;
    info.seated             = slotType != 2;
    info.isCpu               = slotType == 1;
    info.characterId          = m64p::Core.DebugMemRead8(base + PORT_CHARACTER_ID);
    info.costumeId             = m64p::Core.DebugMemRead8(base + PORT_COSTUME_ID);
    info.teamColor              = m64p::Core.DebugMemRead8(base + PORT_TEAM_COLOR);
    info.stocksRemaining         = static_cast<int8_t>(m64p::Core.DebugMemRead8(base + PORT_STOCKS_REMAINING));
    info.comboDamage              = m64p::Core.DebugMemRead32(base + PORT_COMBO_DAMAGE);
    info.comboHitCount             = m64p::Core.DebugMemRead32(base + PORT_COMBO_HIT_COUNT);
    return info;
}

PortPlayerState ReadPortPlayerState(uint32_t matchInfoPtr, int port)
{
    PortPlayerState state{};
    state.valid = false;

    uint32_t playerStruct = ResolvePlayerStruct(matchInfoPtr, port);
    if (!IsValidRdramPointer(playerStruct))
    {
        return state;
    }

    state.valid               = true;
    state.actionStateId         = static_cast<uint16_t>(m64p::Core.DebugMemRead32(playerStruct + PS_ACTION_STATE_ID));
    state.actionFrameCounter    = m64p::Core.DebugMemRead32(playerStruct + PS_ACTION_FRAME_COUNTER);
    state.facingDirection        = static_cast<int32_t>(m64p::Core.DebugMemRead32(playerStruct + PS_FACING_DIRECTION));
    state.velocityX               = ReadFloat(playerStruct + PS_VELOCITY_X);
    state.velocityY                = ReadFloat(playerStruct + PS_VELOCITY_Y);
    state.groundedState              = static_cast<uint8_t>(m64p::Core.DebugMemRead32(playerStruct + PS_KINETIC_STATE));
    state.processedButtons            = m64p::Core.DebugMemRead16(playerStruct + PS_PROCESSED_BUTTONS);
    state.stickX                       = static_cast<int8_t>(m64p::Core.DebugMemRead8(playerStruct + PS_STICK_X));
    state.stickY                        = static_cast<int8_t>(m64p::Core.DebugMemRead8(playerStruct + PS_STICK_Y));
    state.hurtboxState                   = m64p::Core.DebugMemRead8(playerStruct + PS_HURTBOX_STATE);
    state.hitstunCounter                  = m64p::Core.DebugMemRead16(playerStruct + PS_HITSTUN_COUNTER);
    state.damagePercent                    = m64p::Core.DebugMemRead32(playerStruct + PS_DAMAGE_PERCENT);
    state.team                              = m64p::Core.DebugMemRead8(playerStruct + PS_TEAM);
    state.handicap                           = m64p::Core.DebugMemRead8(playerStruct + PS_HANDICAP);
    state.cpuLevel                            = m64p::Core.DebugMemRead8(playerStruct + PS_CPU_LEVEL);

    uint32_t positionPtr = m64p::Core.DebugMemRead32(playerStruct + PS_POSITION_PTR);
    if (IsValidRdramPointer(positionPtr))
    {
        state.positionX = ReadFloat(positionPtr + 0x00);
        state.positionY = ReadFloat(positionPtr + 0x04);
    }

    // jumps_used resets to 0 on landing and is a per-instance counter, not
    // per-character, so it's cheap to just read fresh every frame. jumps_max
    // IS per-character (FTAttributes is shared static move data), so chase
    // it here rather than hardcoding 2 - Remix also writes jumps_used ==
    // jumps_max in some places to deliberately exhaust jumps (e.g. certain
    // up-specials), so jumpsRemaining can legitimately hit 0 without that
    // many real jump inputs.
    const uint8_t jumpsUsed = m64p::Core.DebugMemRead8(playerStruct + PS_JUMPS_USED);
    const uint32_t attributesPtr = m64p::Core.DebugMemRead32(playerStruct + PS_ATTRIBUTES_PTR);
    if (IsValidRdramPointer(attributesPtr))
    {
        const int32_t jumpsMax = static_cast<int32_t>(m64p::Core.DebugMemRead32(attributesPtr + FT_ATTR_MAX_JUMPS));
        state.jumpsRemaining = jumpsMax - static_cast<int32_t>(jumpsUsed);
    }

    return state;
}

std::vector<ItemObject> ReadItemObjects(void)
{
    std::vector<ItemObject> objects;

    auto collectNonHeld = [&](uint8_t linkId)
    {
        WalkGObjLinkList(linkId, [&](uint32_t current, uint32_t userData)
        {
            int32_t kind = 0;
            if (IsValidRdramPointer(userData))
            {
                kind = static_cast<int32_t>(m64p::Core.DebugMemRead32(userData + IT_OR_WP_STRUCT_KIND));
            }

            // Position has to be read before the held-item check can even
            // run (see IsHeldItemPosition's doc comment) - an invalid DObj
            // pointer means there's no reliable position either way, so
            // skip the object entirely rather than recording a
            // meaningless (0,0,0).
            const uint32_t dObj = m64p::Core.DebugMemRead32(current + GOBJ_OBJ_PTR);
            if (!IsValidRdramPointer(dObj))
            {
                return;
            }

            const float posX = ReadFloat(dObj + DOBJ_POSITION_X);
            const float posY = ReadFloat(dObj + DOBJ_POSITION_Y);
            const float posZ = ReadFloat(dObj + DOBJ_POSITION_Z);

            if (linkId == GOBJ_LINK_ID_ITEM && IsHeldItemPosition(posX, posY, posZ))
            {
                return;
            }

            ItemObject object{};
            object.objectAddress = current;
            object.linkId        = linkId;
            object.kind          = kind;
            object.positionX     = posX;
            object.positionY     = posY;
            object.positionZ     = posZ;
            objects.push_back(object);
        });
    };

    collectNonHeld(GOBJ_LINK_ID_ITEM);
    collectNonHeld(GOBJ_LINK_ID_WEAPON);

    return objects;
}

StageHazards ReadStageHazards(uint8_t stageId)
{
    StageHazards hazards{};
    if (stageId == STAGE_ID_DREAM_LAND)
    {
        hazards.whispyBlowing =
            m64p::Core.DebugMemRead8(ADDR_PUPUPU_WHISPY_STATUS) == WHISPY_STATUS_BLOW;
        // Read unconditionally (cheap, single byte) even when not currently
        // blowing - the engine holds the last-decided direction steady
        // between blows too (see ram-map.md section 10.3.1), harmless to
        // read regardless; only meaningful to a caller when whispyBlowing
        // is true.
        hazards.whispyBlowingRight =
            m64p::Core.DebugMemRead8(ADDR_PUPUPU_WHISPY_LR) != 0;
    }
    return hazards;
}
} // namespace ReplayMemory
