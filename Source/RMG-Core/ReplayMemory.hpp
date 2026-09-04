/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef REPLAY_MEMORY_HPP
#define REPLAY_MEMORY_HPP

#include <cstdint>
#include <vector>

// Smash Remix (N64) memory-layout reader for the replay-file feature.
// Every address/offset here comes from the hand-derived RAM map in
// rmgk-replay-file-agent-prompt.md section 6 - do not re-derive.
namespace ReplayMemory
{
struct PortMatchInfo
{
    uint8_t  slotType;   // 0 human, 1 CPU, 2 empty (raw value read from memory)
    bool     seated;     // slotType != 2
    bool     isCpu;      // slotType == 1
    uint8_t  characterId;
    uint8_t  costumeId;
    uint8_t  teamColor;
    int8_t   stocksRemaining;
    // Native engine combo tracking, not mod-added - tracked with the combo
    // meter display toggle off too. Belongs to the victim (this port), not
    // the attacker: how many hits *this port* has taken in its current
    // unbroken chain. 0 = no active chain, 1 = a single hit (not yet a
    // "combo" by convention), 2+ = an actual combo. Both zero the instant
    // the chain breaks. See docs/RMGR_SPEC.md section 7.5 / smashremix
    // docs/ram-map.md section 13.
    uint32_t comboHitCount;
    uint32_t comboDamage;
};

struct PortPlayerState
{
    bool     valid;     // false if the player-object/player-struct pointer
                         // chase failed (port not currently in a live match)
    // No characterId here - it's the exact same base+PORT_CHARACTER_ID
    // read as PortMatchInfo::characterId; callers that need both should
    // read PortMatchInfo, not duplicate the read.
    uint16_t actionStateId;
    uint32_t actionFrameCounter;
    int32_t  facingDirection; // 1 = right, -1 = left
    float    velocityX;
    float    velocityY;
    float    positionX;
    float    positionY;
    // jumpsMax (per-character, from FTAttributes) minus jumps_used
    // (playerStruct+0x148, a u8 that resets to 0 on landing) - see
    // ReadPortPlayerState()'s own comments for the read-width bug this
    // fixed and the two behavioral caveats (0 through most of a grounded
    // match is normal; Remix can force this to 0 without that many real
    // jump inputs, e.g. certain up-specials).
    int32_t  jumpsRemaining;
    uint8_t  groundedState;    // 0 grounded, 1 airborne
    uint16_t processedButtons; // works for human AND CPU ports
    int8_t   stickX;
    int8_t   stickY;
    uint8_t  hurtboxState;
    uint16_t hitstunCounter;
    uint32_t damagePercent;
    uint8_t  team;      // team number this port is assigned to
    uint8_t  handicap;  // this port's handicap value (meaningful when MatchInfo::handicapMode != 0)
    uint8_t  cpuLevel;  // CPU difficulty; meaningless for human ports
};

// One live entry from the Item or Weapon GObj list (see ReadItemObjects()
// below) - Items and Weapons live on two separate lists (gGCCommonLinks[4]
// and gGCCommonLinks[5] respectively), not one shared list filtered by
// link_id. "Weapon" is the engine's own term for a free-flying character
// special-move projectile (boomerang, fireball, charge shot, ...); "Item"
// covers thrown/spawned items and hazard objects (bananas, Poké Balls,
// Waddle Dees, and some fighter-held things like Link's pulled bomb). See
// smashremix docs/ram-map.md section 10.4. `objectAddress` is that GObj's
// own RDRAM address, the closest thing to a stable per-object identity
// available - stable for as long as the object is alive, but not a
// semantic "spawn ID" the engine itself assigns.
//
// A held Item (e.g. Link's bomb while still in his hand) is never returned
// here at all: while held, its position isn't a world coordinate (see
// ReadItemObjects()'s doc comment), so there's nothing meaningful to report
// for it until it's thrown/dropped.
struct ItemObject
{
    uint32_t objectAddress;
    // GObjLinkID: 4 = Item (kind is an ITKind value), 5 = Weapon (kind is a
    // WPKind value) - which enum `kind` means depends on this.
    uint8_t  linkId;
    // ITKind (linkId == 4) or WPKind (linkId == 5) - see docs/RMGR_SPEC.md
    // section 7.6 for both enums.
    int32_t  kind;
    float    positionX;
    float    positionY;
    float    positionZ; // confirmed exactly via the decomp - see ram-map.md section 10.4.1
};

// Live stage-hazard state. Currently just Whispy Woods' wind on Dream Land
// - see ReadStageHazards() below. More hazards (Zebes' acid, Duel Zone's
// platforms, ...) can be added here later the same way, per smashremix
// docs/ram-map.md section 10.5.
struct StageHazards
{
    bool whispyBlowing;      // Dream Land only; always false on any other stage
    bool whispyBlowingRight; // Only meaningful when whispyBlowing is true - see
                              // smashremix docs/ram-map.md section 10.3.1. true =
                              // blowing right (pushes players right of Whispy
                              // further right), false = blowing left.
};

struct MatchInfo
{
    bool     valid;
    uint32_t matchInfoPtr;
    uint8_t  gameMode;
    uint8_t  stageId;
    uint8_t  gameType;         // 1 time, 2 stock, 3 both
    uint8_t  timeLimitMinutes; // 100 = infinite
    uint8_t  stockCountSetting;
    uint8_t  damageRatio;      // 50 = 50%, 200 = 200%
    uint8_t  itemFrequency;
    uint8_t  gameStatus;       // 0 pre-match countdown, 1 ongoing, 2 paused, 5 ended
    bool     matchWasReset;
    bool     teamsEnabled;
    uint8_t  handicapMode;     // 0 off, 1 on, 2 auto
};

// current_screen == 0x16 ("in a VS match")
bool IsInVsMatchScreen(void);

// Reads the match info block at *0x800A50E8. MatchInfo::valid is false if
// the pointer isn't in the valid KSEG0 RDRAM window (0x80000000-0x80800000).
MatchInfo ReadMatchInfo(void);

// Cheap per-port fields, no further pointer chasing.
// port is 0-3. matchInfoPtr must come from a valid ReadMatchInfo() result.
PortMatchInfo ReadPortMatchInfo(uint32_t matchInfoPtr, int port);

// Chases matchStruct+0x58 -> playerObject+0x84 -> playerStruct. Returns
// PortPlayerState::valid == false (not a crash) if that port isn't
// currently seated in a live match.
PortPlayerState ReadPortPlayerState(uint32_t matchInfoPtr, int port);

// Walks the Item list (gGCCommonLinks[4]) and the Weapon list
// (gGCCommonLinks[5]) - two separate fixed-address GObj linked lists,
// independent of MatchInfo - and returns every live, non-held entry found
// across both. See smashremix docs/ram-map.md section 10.4. Each list is
// documented to only ever contain GObjs of its own link_id; an unexpected
// link_id found while walking one is skipped defensively rather than
// trusted. A held Item (e.g. Link's bomb while still in his hand) is also
// skipped: the engine re-parents a held item's DObj onto the holding
// fighter's hand-bone joint, so its position stops being a world coordinate
// and reads as a meaningless local offset near (0,0,0) instead -
// IsHeldItemPosition() in the .cpp uses "position still reads near
// (0,0,0)" as the proxy for "currently held" (NOT ITStruct::owner_gobj,
// which decomp confirms stays non-NULL for essentially an item's whole
// lifetime regardless of held/thrown - see that function's doc comment for
// the full story). Weapons are never held, so this check doesn't apply to
// them. Empty if both lists are empty or their head pointers are invalid.
// Each list's walk is capped at a fixed number of iterations
// (ITEM_LIST_MAX_OBJECTS in the .cpp) so a corrupt or unexpectedly-cyclic
// list can never hang recording - it doesn't specifically detect/dedupe a
// cycle, just guarantees termination.
std::vector<ItemObject> ReadItemObjects(void);

// Reads live stage-hazard state for the given stage. `stageId` comes from
// MatchInfo::stageId - the live per-frame hazard fields live in a
// stage-common union whose byte layout is different per stage (smashremix
// docs/ram-map.md section 10.3's caveat), so the caller's stageId must be
// checked before any hazard-specific offset is trusted; this function does
// that internally and returns all-false for a stage it doesn't know yet.
StageHazards ReadStageHazards(uint8_t stageId);
} // namespace ReplayMemory

#endif // REPLAY_MEMORY_HPP
