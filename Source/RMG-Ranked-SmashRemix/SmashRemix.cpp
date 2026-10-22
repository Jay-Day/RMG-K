/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Ranked module for Smash Remix.
 *
 * Enforcement (OnFrame, deterministic). The game's ways of changing the
 * rules are removed with code patches; the values themselves are only
 * checked each frame and written when they differ (which the patches should
 * make rare: any correction is logged once, pointing at a missed path).
 *  - patches: the hold-L settings shortcut and Remix's stock/time picker
 *    return immediately; on the character select screen (its overlay loads
 *    fresh each time) the stock/time arrows, the team toggle and Back report
 *    "not under the cursor", and the back action returns immediately
 *  - checked values: the Tournament toggle profile with the ruleset's
 *    overrides (skip results, no salty runback, only the allowed stages as
 *    random stages); normal VS mode, no per-port input delay, default stock
 *    mode; Remix's saved VS menu settings (VsRemixMenu.global_*) and the VS
 *    rules outside of battle (4 stock, 8 minutes, no items, teams off - or
 *    on with team attack in 2v2), which the CSS sets its own copy of the
 *    rules up from
 *  - stages (kVanillaStages, kRemixStages): with one allowed stage, stage
 *    select is off and every game is on it. With more, stage select is on
 *    and uses Remix's tournament layout (no variants, no hazard toggle),
 *    whose table is rewritten to show exactly the allowed stages and Random
 *    (which picks among them). Each stage's hazard mode is written to
 *    Remix's tournament hazard table, which hovering a stage and Random
 *    apply; Remix's exceptions to that table (Dream Land, Yoshi's Island DL
 *    and Saffron City DL get another mode on some pages) are patched out
 *  - boot: the N64 logo's next-scene instruction is patched to the VS
 *    character select, skipping Remix's splash and the title
 *  - as a safety net, every menu screen (title, mode select, options, ...)
 *    is sent back to the VS character select screen
 *
 * Tracking (OnConfirmedFrame):
 *  - a game starts during the pre-GO countdown (fresh timer, status Wait),
 *    so its on-screen message shows before players can move
 *  - it ends when the battle scene is left; the winner is the only ranked
 *    port with stocks left (or the one with more stocks on a timeout)
 *  - 2v2 rulesets: team battle with team attack, all four ports human in two
 *    teams of two (any ports, team colours don't matter). The pairing is
 *    fixed by game 1, or from the start when the lobby matched teammates
 *    (RankedSessionConfig.teams; character select then opens with each
 *    team on one colour, P1's red); the winning team is the one with stocks
 *    left (more stocks between its two players on a timeout)
 *  - resets, CPUs, extra players, a stage that isn't allowed (or its hazard
 *    mode differs), wrong rules, a character outside the ruleset's roster,
 *    or (2v2) other partners void the game
 *
 * Remix code lives at addresses that move with every build, so the module
 * resolves them from a symbol file generated from bass' logfile.log:
 *   <dataDirectory>/<ROM MD5>.sym   ("80462ca4 Toggles.entry_foo" per line)
 * See Source/Script/GenerateRankedRemixSymbols.sh.
 */

#include "Ranked/RankedModuleApi.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{

//
// Vanilla SSB64 (US) addresses, stable across Remix builds
//

constexpr uint32_t kSceneCurrent       = 0x800A4AD0; // u8 gSCManagerSceneData.scene_curr
constexpr uint32_t kScenePrevious      = 0x800A4AD1; // u8 scene_prev
constexpr uint32_t kSceneIsReset       = 0x800A4AE2; // u8 A+B+Z+R reset during battle
constexpr uint32_t kSceneInterrupt     = 0x800465D0; // u32 write 1 to load scene_curr
constexpr uint32_t kTransferBattle     = 0x800A4D08; // SCBattleState used by VS mode
constexpr uint32_t kSuddenDeathBattle  = 0x800A4EF8; // SCBattleState copy for sudden death
constexpr uint32_t kActiveBattlePtr    = 0x800A50E8; // SCBattleState* in use

// VS character select screen (overlay at 0x80131B00, only valid while loaded)
constexpr uint32_t kCssSignatureA      = 0x80131B40; // unpatched instruction words that
constexpr uint32_t kCssSignatureAValue = 0xAC780004; // identify the CSS overlay (same in
constexpr uint32_t kCssSignatureB      = 0x80138000; // vanilla and Remix)
constexpr uint32_t kCssSignatureBValue = 0x000E7100;
constexpr uint32_t kCssBackToVsMode    = 0x801380F4; // mnPlayersVSBackToVSMode
constexpr uint32_t kCssBackFirstWord   = 0x3C02800A; // lui v0, 0x800A (unpatched)
constexpr uint32_t kMipsJrRa           = 0x03E00008;
constexpr uint32_t kMipsNop            = 0x00000000;
constexpr uint32_t kMipsClearV0        = 0x00001025; // or v0, r0, r0
constexpr uint32_t kMipsClearA0        = 0x00002021; // addu a0, r0, r0
constexpr uint32_t kMipsLoadT0         = 0x34080000; // ori t0, r0, imm
// VS CSS overlay functions that check whether a button is under a cursor
// (ssb64 decomp, mnplayersvs.c): a click on these buttons does nothing when
// they return FALSE
constexpr uint32_t kCssCheckTimeArrowR = 0x80134F64; // stock/time + arrow
constexpr uint32_t kCssCheckTimeArrowL = 0x8013502C; // stock/time - arrow
constexpr uint32_t kCssCheckGameMode   = 0x80135270; // Free-for-all/Team Battle
constexpr uint32_t kCssCheckBack       = 0x80138218; // Back
// N64 logo overlay: the instruction that picks the next scene when the logo
// ends (Remix: its splash, scene 0x37; vanilla: the opening, 0x1C)
constexpr uint32_t kLogoNextScene      = 0x80131C94;
constexpr uint32_t kLogoToRemixSplash  = 0x34090037; // ori t1, r0, 0x37
constexpr uint32_t kLogoToOpening      = 0x3409001C; // ori t1, r0, 0x1C
constexpr uint32_t kLogoToVsCss        = 0x34090010; // ori t1, r0, 0x10

// SCBattleState offsets
constexpr uint32_t kBattleGkind        = 0x01;
constexpr uint32_t kBattleIsTeam       = 0x02;
constexpr uint32_t kBattleRules        = 0x03; // 1 = time, 2 = stock
constexpr uint32_t kBattleTimeLimit    = 0x06; // minutes, 100 = infinite
constexpr uint32_t kBattleStocks       = 0x07; // 0-based
constexpr uint32_t kBattleHandicap     = 0x08;
constexpr uint32_t kBattleTeamAttack   = 0x09;
constexpr uint32_t kBattleStageSelect  = 0x0A;
constexpr uint32_t kBattleDamageRatio  = 0x0B;
constexpr uint32_t kBattleItemToggles  = 0x0C; // u32
constexpr uint32_t kBattleResetPlayers = 0x10; // character select starts every slot from its defaults
constexpr uint32_t kBattleGameStatus   = 0x11;
constexpr uint32_t kBattleTimeRemain   = 0x14; // u32 tics, reset to the full limit at battle setup
constexpr uint32_t kBattleTimePassed   = 0x18; // u32 tics, 0 until GO
constexpr uint32_t kBattleItemRate     = 0x1C;
constexpr uint32_t kBattlePlayers      = 0x20;
constexpr uint32_t kPlayerStride       = 0x74;

// SCPlayerData offsets
constexpr uint32_t kPlayerPkind        = 0x02; // 0 = human, 1 = CPU, 2 = none
constexpr uint32_t kPlayerFkind        = 0x03;
constexpr uint32_t kPlayerTeam         = 0x04; // team colour: 0 red, 1 blue, 2 green
constexpr uint32_t kPlayerStockCount   = 0x0B; // s8, -1 = out
constexpr uint32_t kPlayerPlace        = 0x0D;

constexpr uint8_t kSceneTitle          = 0x01;
constexpr uint8_t kSceneN64Logo        = 0x1B;
constexpr uint8_t kSceneModeSelect     = 0x07;
constexpr uint8_t kSceneVsMenu         = 0x09;
constexpr uint8_t kSceneVsOptions      = 0x0A;
constexpr uint8_t kSceneItemSwitch     = 0x0B;
constexpr uint8_t kSceneVsCss          = 0x10;
constexpr uint8_t kSceneStageSelect    = 0x15;
constexpr uint8_t kSceneVsBattle       = 0x16;
constexpr uint8_t kSceneVsResults      = 0x18;
constexpr uint8_t kSceneRemixOptions   = 0x39;
constexpr uint8_t kSceneRemixModes     = 0x77;

constexpr uint8_t kStatusWait          = 0;
constexpr uint8_t kStatusGo            = 1;
constexpr uint8_t kStatusEnd           = 5;
constexpr uint8_t kStatusSet           = 7; // after GAME SET (and on resets)

constexpr uint8_t kPkindHuman          = 0;
constexpr uint8_t kPkindNone           = 2;

constexpr uint8_t kTeamRed             = 0;
constexpr uint8_t kTeamBlue            = 1;

// Remix Menu.entry layout
constexpr uint32_t kEntryType          = 0x00;
constexpr uint32_t kEntryValue         = 0x04;
constexpr uint32_t kEntryCopyAddress   = 0x18;
constexpr uint32_t kEntryNext          = 0x1C;
constexpr uint32_t kEntryToggleNumber  = 0x24; // Toggles: 1-based index into profile_defaults_*
constexpr uint32_t kEntryTitle         = 0x28; // inline string
constexpr uint32_t kEntryTypeTitle     = 0x00;
constexpr uint32_t kEntryTypeInput     = 0x0B;
constexpr int      kMaxEntriesPerHead  = 4096;
constexpr int      kMaxHeads           = 32;

// Remix stage select (Stages.asm): 18 icons per page, Random is the last
constexpr uint32_t kSssIconsPerPage    = 18;
constexpr uint32_t kTournamentPages    = 3;  // stage_table_tournament's size
constexpr uint32_t kSssLayoutTournament = 1; // Toggles.entry_sss_layout
constexpr uint8_t  kStageRandom        = 0xDE;
constexpr uint8_t  kNoStage            = 0xFF; // never a hovered stage
// Stages.swap_stage_ builds the random stage list with a run of
//   lui a0, hi / ori a0, a0, lo / jal add_stage_to_random_list_ / ori a1, r0, id
// (one per stage toggle); the run starts within this many bytes
constexpr uint32_t kRandomListSearchBytes = 0x100;
// Stages.get_stage_id_ (the hovered stage) ends within this many bytes
constexpr uint32_t kHoveredStageSearchBytes = 0x400;

constexpr const char* kModuleVersion   = "0.9.0";

//
// Rulesets
//

struct ToggleOverride
{
    const char* symbol;
    uint32_t value;
};

// Remix stage ids (Stages.id). Remix appends new stages, so ids are stable
// across builds; verified against v2.0.1. These are the stages on Remix's
// tournament stage select layout (its competitive list), with the hazard mode
// that layout uses where it isn't "hazards on, movement on". The stage list
// below can give any stage any mode.
// Page 1 - main stages
constexpr uint8_t kStageDreamLand         = 0x06; // also on page 3 with hazards off
constexpr uint8_t kStageFraysStage        = 0x37;
constexpr uint8_t kStageFraysStageNight   = 0x6E;
constexpr uint8_t kStageFirstDestination  = 0x2A;
constexpr uint8_t kStagePokemonStadium    = 0x2D;
constexpr uint8_t kStagePokemonStadium2   = 0xA4;
constexpr uint8_t kStageSmashville        = 0x4C;
constexpr uint8_t kStageGoombaRoad        = 0x6F;
constexpr uint8_t kStageGymLeaderCastle   = 0x2C;
constexpr uint8_t kStageSaffronDl         = 0x72; // also on page 3 with movement off
constexpr uint8_t kStageGanonsTower       = 0x2B;
constexpr uint8_t kStageGlacialRemix      = 0xAB;
constexpr uint8_t kStageDrMario           = 0x33;
constexpr uint8_t kStageTalTal            = 0x2E; // hazards off, movement off
constexpr uint8_t kStageMelrode           = 0xBB;
constexpr uint8_t kStageYoshisStory       = 0x60;
constexpr uint8_t kStageBattlefield       = 0x31;
// Page 2 - viable stages
constexpr uint8_t kStageGerudoValley      = 0x63;
constexpr uint8_t kStageWarioWare         = 0x30;
constexpr uint8_t kStageDelfinoPlaza      = 0x45;
constexpr uint8_t kStageCastleSiege       = 0x8D;
constexpr uint8_t kStageSpiralMountain    = 0x3D;
constexpr uint8_t kStageSmashvilleRemix   = 0xC4;
constexpr uint8_t kStageYoshisIslandDl    = 0x73; // also on page 3 with hazards off
constexpr uint8_t kStageYoshisIslandII    = 0x8E;
constexpr uint8_t kStagePlanetClancer     = 0x85;
constexpr uint8_t kStageFountainOfDreams  = 0x39;
constexpr uint8_t kStageFinalDestination  = 0x10;
constexpr uint8_t kStageGlacialRiver      = 0x2F;
constexpr uint8_t kStageBigBoosHaunt      = 0xC0;
constexpr uint8_t kStageGreenHillZone     = 0x9C;
constexpr uint8_t kStageNewPorkCity       = 0x58;
constexpr uint8_t kStageBowsersKeep       = 0x7C;
constexpr uint8_t kStageDataDyneCentral   = 0x84; // hazards on, movement off
// Page 3 - Dream Land clones
constexpr uint8_t kStagePeachsCastleDl    = 0x6B; // hazards off
constexpr uint8_t kStageCongoJungleDl     = 0x69; // hazards off
constexpr uint8_t kStageHyruleCastleDl    = 0x67; // hazards off
constexpr uint8_t kStagePlanetZebesDl     = 0x74; // hazards off
constexpr uint8_t kStageMushroomKingdomDl = 0x41; // hazards off
constexpr uint8_t kStageDuelZoneDl        = 0x92;
constexpr uint8_t kStageSectorZDl         = 0x71; // hazards off
constexpr uint8_t kStageFinalDestinationDl = 0x8F;
constexpr uint8_t kStageMetaCrystalDl     = 0x94;
constexpr uint8_t kStageMuteCityDl        = 0x3F;
constexpr uint8_t kStageWinterDl          = 0xA9; // hazards off
constexpr uint8_t kStageDekuTreeDl        = 0xB5;
constexpr uint8_t kStageCrateriaDl        = 0xB6;
constexpr uint8_t kStageBattlefieldDl     = 0x50;

// Toggles.entry_hazard_mode (Hazards.mode)
constexpr uint8_t kHazardsOnMovementOn    = 0;
constexpr uint8_t kHazardsOffMovementOn   = 1;
constexpr uint8_t kHazardsOnMovementOff   = 2;
constexpr uint8_t kHazardsOffMovementOff  = 3;

struct RankedStage
{
    uint8_t id;
    uint8_t hazardMode;
};

// The stages a ruleset's games may be played on, in stage select order.
//  - one stage: stage select stays off and every game is on it
//  - more: players pick on the stage select screen, which shows only these
//    (and Random, which picks among them), at most 51 (3 pages of 17)
// Each game gets its stage's hazard mode. A list is part of the ruleset hash,
// so peers must run the same one: changing it needs a new module version
// (kModuleVersion) and matching rulesetStages in the lobby's ranked.json.
// Vanilla roster: Dream Land.
const std::vector<RankedStage> kVanillaStages =
{
    {kStageDreamLand, kHazardsOnMovementOn},
};
// Remix roster: the community's ranked stages.
const std::vector<RankedStage> kRemixStages =
{
    {kStageDreamLand, kHazardsOnMovementOn},
    {kStageGoombaRoad, kHazardsOnMovementOn},
    {kStagePokemonStadium2, kHazardsOnMovementOn},
    {kStagePokemonStadium, kHazardsOnMovementOn},
};

struct Ruleset
{
    const char* id;
    const char* rosterName;
    // legal character ids (Remix's Character.id values)
    std::vector<uint8_t> roster;
    // Toggles.profile_defaults_<profile>
    const char* profile;
    uint8_t stocks;       // 0-based
    uint8_t timeMinutes;
    // the allowed stages, see kVanillaStages; the first is the default
    std::vector<RankedStage> stages;
    std::vector<ToggleOverride> overrides;
    // players per team: 1 (1v1) or 2 (2v2 team battle, team attack on)
    int teamSize;
};

// Rosters. Remix assigns character ids in definition order and appends new
// characters, so ids are stable across builds; verified against the built
// ROM's CharacterSelect.id_table and Character.variant_original.table.
// Vanilla: the original 12.
const std::vector<uint8_t> kVanillaRoster =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, // Mario, Fox, DK, Samus, Luigi, Link
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, // Yoshi, Falcon, Kirby, Pikachu, Jigglypuff, Ness
};
// Remix: everyone on the main character select grid, plus Dark Samus and
// Lucas. No polygons, J/E regional variants, bosses or other bonus fighters.
const std::vector<uint8_t> kRemixRoster =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x1D, // Falco
    0x1E, // Ganondorf
    0x1F, // Young Link
    0x20, // Dr. Mario
    0x21, // Wario
    0x22, // Dark Samus
    0x26, // Lucas
    0x34, // Bowser
    0x37, // Wolf
    0x38, // Conker
    0x39, // Mewtwo
    0x3A, // Marth
    0x3B, // Sonic
    0x3E, // Sheik
    0x3F, // Marina
    0x40, // Dedede
    0x41, // Goemon
    0x44, // Banjo & Kazooie
    0x48, // Crash
    0x49, // Peach
};

Ruleset make_ruleset(const char* id, const char* rosterName, const std::vector<uint8_t>& roster,
    const std::vector<RankedStage>& stages, int teamSize = 1)
{
    return {
        id,
        rosterName,
        roster,
        "TE",
        3,    // 4 stock
        8,    // 8 minutes
        stages,
        {
            {"Toggles.entry_skip_results_screen", 1}, // straight back to CSS
            {"Toggles.entry_salty_runback", 0},       // no rematch without CSS
        },
        teamSize,
    };
}

// what's wrong with a stage list, empty when it's usable
std::string check_stages(const std::vector<RankedStage>& stages)
{
    if (stages.empty())
    {
        return "no stages";
    }
    if (stages.size() > kTournamentPages * (kSssIconsPerPage - 1))
    {
        return "more stages than stage select can show (" +
            std::to_string(kTournamentPages * (kSssIconsPerPage - 1)) + ")";
    }
    std::set<uint8_t> seen;
    for (const RankedStage& stage : stages)
    {
        if (stage.id == kStageRandom || stage.hazardMode > kHazardsOffMovementOff || !seen.insert(stage.id).second)
        {
            return "bad stage entry " + std::to_string(stage.id);
        }
    }
    return std::string();
}

// the first ruleset is the default
const std::vector<Ruleset>& get_rulesets()
{
    static const std::vector<Ruleset> rulesets =
    {
        make_ruleset("remix-1v1-vanilla", "Vanilla", kVanillaRoster, kVanillaStages),
        make_ruleset("remix-1v1-remix", "Remix", kRemixRoster, kRemixStages),
        make_ruleset("remix-2v2-vanilla", "Vanilla", kVanillaRoster, kVanillaStages, 2),
        make_ruleset("remix-2v2-remix", "Remix", kRemixRoster, kRemixStages, 2),
    };
    return rulesets;
}

//
// Observation (fits RANKED_MAX_OBSERVATION_SIZE)
//

#pragma pack(push, 1)
struct PlayerObservation
{
    uint8_t pkind;
    uint8_t fkind;
    int8_t  stockCount;
    uint8_t place;
    uint8_t team; // team colour
};

struct Observation
{
    uint8_t ready;
    uint8_t scene;
    uint8_t isReset;
    uint8_t battleGkind;
    uint8_t status;
    uint8_t isSuddenDeath;
    uint8_t gameRules;
    uint8_t stocks;
    uint8_t isTeam;
    uint8_t itemRate;
    uint8_t vsModeFlag;
    uint8_t hazardMode;
    uint8_t inCountdown; // this battle is set up and counting down to GO
    uint8_t saltyRunback; // GameEnd.is_salty_runback: restarted from the results screen
    uint8_t teamAttack;
    uint32_t itemToggles;
    PlayerObservation players[RANKED_MAX_PLAYERS];
};
#pragma pack(pop)
static_assert(sizeof(Observation) <= RANKED_MAX_OBSERVATION_SIZE, "observation too large");

//
// Session
//

struct Instance
{
    RankedHostApi host{};
    const Ruleset* ruleset = nullptr;
    int bestOf = 3;
    std::vector<int> ports;
    std::string romMd5;

    // resolved addresses
    uint32_t profileHeadTable = 0;
    uint32_t headRemixSettings = 0;
    uint32_t profileDefaults = 0;
    uint32_t vsModeFlag = 0;
    uint32_t inputDelayTable = 0;
    uint32_t stockModeTable = 0;
    uint32_t hazardModeEntry = 0;
    uint32_t settingsShortcut = 0; // Toggles.settings_shortcut (hold L on the CSS)
    uint32_t pickerPress = 0;      // CharacterSelect.check_picker_press_ (stock/time picker)
    uint32_t saltyRunbackFlag = 0; // GameEnd.is_salty_runback (word), 0 = unknown
    // VsRemixMenu.global_* (words)
    uint32_t globalGameMode = 0;
    uint32_t globalTime = 0;
    uint32_t globalStocks = 0;
    uint32_t globalTeams = 0;
    uint32_t globalStageSelect = 0;
    uint32_t globalStockModeTable = 0;
    std::vector<std::pair<uint32_t, uint32_t>> overrides; // entry address, value
    // stages
    bool stageSelect = false;          // more than one allowed stage
    uint32_t tournamentDefaults = 0;   // Toggles.profile_defaults_TE (Random's tournament filter)
    uint32_t randomStageList = 0;      // Stages.swap_stage_
    uint32_t addRandomStage = 0;       // Stages.add_stage_to_random_list_
    uint32_t dreamLandRandomEntry = 0; // Toggles.entry_random_stage_dream_land, checks the decoding
    uint32_t sssLayoutEntry = 0;       // Toggles.entry_sss_layout
    uint32_t stageTable = 0;           // Stages.stage_table (a table pointer per layout)
    uint32_t stageTablePages = 0;      // Stages.stage_table_pages (pages per layout)
    uint32_t tournamentTable = 0;      // Stages.stage_table_tournament
    uint32_t tournamentHazards = 0;    // Stages.tournament_hazard_mode_table (per stage id)
    uint32_t hoveredStage = 0;         // Stages.get_stage_id_
    // its `lli t0, <stage id>` instructions that pick out the stages whose
    // hazard mode Remix sets by page instead of from the table
    std::vector<uint32_t> pageHazardChecks;

    // The values the ruleset fixes, built once Remix is loaded (from constant
    // Remix data, so every peer and every resimulated frame gets the same).
    // Code patches remove the game's ways of changing them; the module only
    // checks them each frame and corrects drift, which it reports once.
    struct Pin
    {
        uint32_t address;
        uint8_t  size; // 1 or 4
        uint32_t value;
    };
    std::vector<Pin> pins;
    bool pinsBuilt = false;
    // logging only: values set while booting are summarized when character
    // select is first reached, later corrections are reported once each
    bool booted = false;
    int bootCorrections = 0;
    std::set<uint32_t> reportedDrift;

    // confirmed tracking state
    std::mutex stateMutex;
    Observation previous{};
    Observation lastInBattle{};
    bool inBattle = false;
    bool gameStarted = false;
    bool gameReachedEnd = false; // status hit End during this battle scene
    int gameStage = -1;          // stage id of the current (or last) game
    std::string invalidReason;
    int score[RANKED_MAX_PLAYERS] = {0, 0, 0, 0}; // a team set's members share their team's
    // 2v2: each port's team (1 or 2, team 1 holding the lowest ranked port),
    // fixed by the first game; all 0 until then and in 1v1
    int pairing[RANKED_MAX_PLAYERS] = {0, 0, 0, 0};
    // the lobby matched teammates: pairing is set from the start
    bool lobbyPairing = false;
    int gamesCounted = 0;
    int gamesVoided = 0;
    bool setOver = false;
    int setWinner = 0;
    int setWinnerTeam = 0;
};

// memory helpers

bool has_write_code(const RankedHostApi& host)
{
    return host.size >= offsetof(RankedHostApi, WriteCode32) + sizeof(host.WriteCode32) &&
           host.WriteCode32 != nullptr;
}

uint8_t  read8(Instance* i, uint32_t a)  { return i->host.Read8(i->host.context, a); }
uint32_t read32(Instance* i, uint32_t a) { return i->host.Read32(i->host.context, a); }
void write8(Instance* i, uint32_t a, uint8_t v)   { i->host.Write8(i->host.context, a, v); }
void write32(Instance* i, uint32_t a, uint32_t v) { i->host.Write32(i->host.context, a, v); }

bool is_ram_pointer(uint32_t pointer)
{
    return pointer >= 0x80000000 && pointer < 0x80800000 && (pointer & 3) == 0;
}

void module_log(Instance* i, RankedLogLevel level, const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    i->host.Log(i->host.context, level, buffer);
}

void copy_string(char* out, uint32_t outSize, const std::string& value)
{
    if (out == nullptr || outSize == 0)
    {
        return;
    }
    std::snprintf(out, outSize, "%s", value.c_str());
}

std::string to_upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

// symbol file: "<hex address> <name>" per line, '#' comments
bool load_symbols(const std::string& path, std::map<std::string, uint32_t>& symbols)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::istringstream stream(line);
        std::string address, name;
        if (!(stream >> address >> name))
        {
            continue;
        }
        try
        {
            symbols[name] = static_cast<uint32_t>(std::stoul(address, nullptr, 16));
        }
        catch (...)
        {
            continue;
        }
    }
    return true;
}

//
// Enforcement
//

// the VS CSS overlay is resident (scene_curr alone changes before the old
// scene unloads, so it can't tell)
bool css_loaded(Instance* i)
{
    return read32(i, kCssSignatureA) == kCssSignatureAValue &&
           read32(i, kCssSignatureB) == kCssSignatureBValue;
}

// checks a fixed value, writing it only when it differs
void pin(Instance* i, uint32_t address, uint8_t size, uint32_t value)
{
    const uint32_t current = size == 1 ? read8(i, address) : read32(i, address);
    if (current == value)
    {
        return;
    }
    if (size == 1)
    {
        write8(i, address, static_cast<uint8_t>(value));
    }
    else
    {
        write32(i, address, value);
    }
    if (!i->booted)
    {
        i->bootCorrections++;
    }
    else if (i->reportedDrift.insert(address).second)
    {
        module_log(i, RankedLog_Warning, "corrected %08X (%X, ruleset %X): a way to change it isn't blocked",
            address, current, value);
    }
}

// patches a function's first two instructions, once per load of its code
void patch_code(Instance* i, uint32_t address, uint32_t first, uint32_t second, const char* what)
{
    if (address == 0 || !has_write_code(i->host) ||
        (read32(i, address) == first && read32(i, address + 4) == second))
    {
        return;
    }
    i->host.WriteCode32(i->host.context, address + 4, second);
    i->host.WriteCode32(i->host.context, address, first);
    module_log(i, RankedLog_Verbose, "%s disabled", what);
}

// patches one instruction, once per load of its code
void patch_instruction(Instance* i, uint32_t address, uint32_t value, const char* what)
{
    if (address == 0 || !has_write_code(i->host) || read32(i, address) == value)
    {
        return;
    }
    i->host.WriteCode32(i->host.context, address, value);
    module_log(i, RankedLog_Verbose, "%s disabled", what);
}

const RankedStage* find_stage(Instance* i, uint8_t id)
{
    for (const RankedStage& stage : i->ruleset->stages)
    {
        if (stage.id == id)
        {
            return &stage;
        }
    }
    return nullptr;
}

// the hazard mode a game on this stage gets (the default stage's for others)
uint32_t stage_hazard_mode(Instance* i, uint8_t id)
{
    const RankedStage* stage = find_stage(i, id);
    return stage != nullptr ? stage->hazardMode : i->ruleset->stages.front().hazardMode;
}

// a Toggles entry's title, for logs
std::string entry_title(Instance* i, uint32_t entry)
{
    std::string title;
    for (uint32_t n = 0; n < 48; n++)
    {
        const uint8_t c = read8(i, entry + kEntryTitle + n);
        if (c == 0)
        {
            break;
        }
        title += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return title;
}

// Each stage Random can pick, with its random toggle entry, read from the code
// of Remix's random stage list (see kRandomListSearchBytes). Custom stages'
// toggles are named by their sort order, not their id, so this is the only
// map from a stage id to its toggle.
std::vector<std::pair<uint8_t, uint32_t>> random_stage_entries(Instance* i)
{
    const uint32_t jal = 0x0C000000 | ((i->addRandomStage >> 2) & 0x03FFFFFF);
    auto decode = [&](uint32_t at, std::pair<uint8_t, uint32_t>& out)
    {
        const uint32_t lui = read32(i, at);     // lui a0, hi
        const uint32_t ori = read32(i, at + 4); // ori a0, a0, lo
        const uint32_t id  = read32(i, at + 12); // ori a1, r0, id (jal's delay slot)
        if ((lui >> 16) != 0x3C04 || (ori >> 16) != 0x3484 || read32(i, at + 8) != jal ||
            (id & 0xFFFFFF00) != 0x34050000)
        {
            return false;
        }
        out = {static_cast<uint8_t>(id & 0xFF), ((lui & 0xFFFF) << 16) | (ori & 0xFFFF)};
        return true;
    };

    std::vector<std::pair<uint8_t, uint32_t>> entries;
    std::pair<uint8_t, uint32_t> entry;
    uint32_t at = i->randomStageList;
    while (at < i->randomStageList + kRandomListSearchBytes && !decode(at, entry))
    {
        at += 4;
    }
    while (decode(at, entry))
    {
        entries.push_back(entry);
        at += 16;
    }
    return entries;
}

// Remix's tournament layout lists Dream Land, Yoshi's Island DL and Saffron
// City DL twice with different hazard modes, so the hovered stage function
// (get_stage_id_) sets theirs by page instead of from the hazard table: it
// compares the stage against each with `lli t0, <stage id>`. Finds those
// instructions, which enforce makes compare against no stage, leaving every
// stage's hazard mode to the table (the stage list).
std::vector<uint32_t> find_page_hazard_checks(Instance* i)
{
    std::vector<uint32_t> checks;
    for (uint32_t at = i->hoveredStage; at < i->hoveredStage + kHoveredStageSearchBytes; at += 4)
    {
        const uint32_t word = read32(i, at);
        if (word == kMipsJrRa)
        {
            break;
        }
        if (word == (kMipsLoadT0 | kStageDreamLand) || word == (kMipsLoadT0 | kStageYoshisIslandDl) ||
            word == (kMipsLoadT0 | kStageSaffronDl))
        {
            checks.push_back(at);
        }
    }
    return checks;
}

// Remix's tournament stage select layout shows exactly the allowed stages, 17
// per page with Random last (or, if Random can't pick any of them, the stages
// again), and its hazard table holds their hazard modes, which hovering a
// stage (or Random picking it) applies.
void build_stage_select(Instance* i, bool random)
{
    const std::vector<RankedStage>& stages = i->ruleset->stages;
    if (read32(i, i->stageTable + kSssLayoutTournament * 4) != i->tournamentTable)
    {
        module_log(i, RankedLog_Error, "Remix's tournament stage table isn't at %08X: stage select shows "
            "every tournament stage", i->tournamentTable);
        return;
    }
    i->pageHazardChecks = find_page_hazard_checks(i);
    if (i->pageHazardChecks.size() != 3)
    {
        module_log(i, RankedLog_Error, "found %zu of Remix's 3 per-page hazard modes: stage select may show "
            "another hazard mode for Dream Land, Yoshi's Island DL or Saffron City DL",
            i->pageHazardChecks.size());
    }

    const uint32_t perPage = kSssIconsPerPage - 1;
    const uint32_t pages = static_cast<uint32_t>((stages.size() + perPage - 1) / perPage);
    size_t next = 0;
    for (uint32_t slot = 0; slot < pages * kSssIconsPerPage; slot++)
    {
        uint8_t id;
        if (slot % kSssIconsPerPage != perPage && next < stages.size())
        {
            id = stages[next++].id;
        }
        else if (random)
        {
            id = kStageRandom;
        }
        else
        {
            id = stages[slot % stages.size()].id;
        }
        i->pins.push_back({i->tournamentTable + slot, 1, id});
    }
    i->pins.push_back({i->stageTablePages + kSssLayoutTournament * 4, 4, pages});
    for (const RankedStage& stage : stages)
    {
        i->pins.push_back({i->tournamentHazards + stage.id, 1, stage.hazardMode});
    }
    module_log(i, RankedLog_Verbose, "stage select: %zu stages on %u page(s)%s", stages.size(), pages,
        random ? "" : ", no Random");
}

// The ruleset's toggles (Toggles.load_profile_ for the profile, then the
// overrides), the stages and the VS settings Remix keeps outside the CSS.
void build_pins(Instance* i)
{
    std::map<uint32_t, uint32_t> overrides(i->overrides.begin(), i->overrides.end());

    // Random picks only the allowed stages: their random toggles on, every
    // other one off. On the tournament layout Random also skips stages the
    // Tournament profile doesn't default on, so the allowed ones are marked.
    std::vector<std::pair<uint8_t, uint32_t>> randomStages = random_stage_entries(i);
    bool decoded = !randomStages.empty();
    for (const auto& [id, entry] : randomStages)
    {
        if (id == kStageDreamLand && i->dreamLandRandomEntry != 0 && entry != i->dreamLandRandomEntry)
        {
            decoded = false;
        }
    }
    bool random = false;
    if (decoded)
    {
        std::string names;
        for (const auto& [id, entry] : randomStages)
        {
            const bool allowed = find_stage(i, id) != nullptr;
            overrides[entry] = allowed ? 1 : 0;
            if (!allowed)
            {
                continue;
            }
            random = true;
            names += (names.empty() ? "" : ", ") + entry_title(i, entry);
            const uint32_t toggle = read32(i, entry + kEntryToggleNumber);
            if (toggle >= 1 && toggle <= kMaxEntriesPerHead)
            {
                i->pins.push_back({i->tournamentDefaults + (toggle - 1) * 4, 4, 1});
            }
        }
        if (random)
        {
            module_log(i, RankedLog_Info, "Random picks from: %s", names.c_str());
        }
        else if (!i->stageSelect)
        {
            module_log(i, RankedLog_Error, "Random can't pick the ruleset's stage, so games may be on any stage");
        }
        else
        {
            module_log(i, RankedLog_Info, "Random can't pick any allowed stage: stage select has no Random");
        }
    }
    else
    {
        module_log(i, RankedLog_Error, "couldn't read Remix's random stage list (%zu stages): Random isn't "
            "limited to the allowed stages", randomStages.size());
    }

    uint32_t defaults = i->profileDefaults;
    for (int head = 0; head < kMaxHeads; head++)
    {
        uint32_t entry = read32(i, i->profileHeadTable + static_cast<uint32_t>(head) * 4);
        if (entry == 0)
        {
            break;
        }
        for (int count = 0; count < kMaxEntriesPerHead && is_ram_pointer(entry); count++)
        {
            uint32_t type = read32(i, entry + kEntryType);
            // with stage select the hazard mode follows the chosen stage (enforce)
            if (i->stageSelect && entry == i->hazardModeEntry)
            {
                defaults += 4;
            }
            else if (type != kEntryTypeTitle && type != kEntryTypeInput)
            {
                auto it = overrides.find(entry);
                uint32_t value = it != overrides.end() ? it->second : read32(i, defaults);
                i->pins.push_back({entry + kEntryValue, 4, value});
                // Menu's "copy curr val": an entry may shadow its value elsewhere
                uint32_t copyAddress = read32(i, entry + kEntryCopyAddress);
                if (it != overrides.end() && is_ram_pointer(copyAddress))
                {
                    i->pins.push_back({copyAddress + 3, 1, value & 0xFF});
                }
                defaults += 4;
            }
            entry = read32(i, entry + kEntryNext);
        }
    }

    const Ruleset* rules = i->ruleset;
    i->pins.push_back({i->vsModeFlag, 4, 0});
    for (uint32_t port = 0; port < RANKED_MAX_PLAYERS; port++)
    {
        if (i->inputDelayTable != 0)
        {
            i->pins.push_back({i->inputDelayTable + port * 4, 4, 0});
        }
        if (i->stockModeTable != 0)
        {
            i->pins.push_back({i->stockModeTable + port * 4, 4, 0});
        }
        if (i->globalStockModeTable != 0)
        {
            i->pins.push_back({i->globalStockModeTable + port * 4, 4, 0});
        }
    }
    // Remix's saved VS menu settings, restored into the CSS when it opens
    i->pins.push_back({i->globalGameMode, 4, 3});
    i->pins.push_back({i->globalTime, 4, rules->timeMinutes});
    i->pins.push_back({i->globalStocks, 4, rules->stocks});
    i->pins.push_back({i->globalTeams, 4, rules->teamSize > 1 ? 1u : 0u});
    i->pins.push_back({i->globalStageSelect, 4, i->stageSelect ? 1u : 0u});
    if (i->stageSelect)
    {
        build_stage_select(i, random);
    }
    i->pinsBuilt = true;
    module_log(i, RankedLog_Verbose, "%zu fixed values", i->pins.size());
}

void enforce(Instance* i)
{
    if (!i->pinsBuilt)
    {
        build_pins(i);
    }
    for (const Instance::Pin& p : i->pins)
    {
        pin(i, p.address, p.size, p.value);
    }

    // the settings shortcut (hold L) and Remix's stock/time picker
    patch_code(i, i->settingsShortcut, kMipsJrRa, kMipsClearA0, "settings shortcut");
    patch_code(i, i->pickerPress, kMipsJrRa, kMipsNop, "stock/time picker");
    // stage select's per-page hazard modes (see find_page_hazard_checks)
    for (uint32_t address : i->pageHazardChecks)
    {
        patch_instruction(i, address, kMipsLoadT0 | kNoStage, "stage select per-page hazard mode");
    }

    uint8_t scene = read8(i, kSceneCurrent);
    const Ruleset* r = i->ruleset;

    // boot: when the N64 logo ends, go straight to VS character select
    // instead of the splash and the title
    if (scene == kSceneN64Logo && has_write_code(i->host))
    {
        const uint32_t next = read32(i, kLogoNextScene);
        if (next == kLogoToRemixSplash || next == kLogoToOpening)
        {
            i->host.WriteCode32(i->host.context, kLogoNextScene, kLogoToVsCss);
            module_log(i, RankedLog_Verbose, "boot goes straight to character select");
        }
    }

    // teammates the lobby matched start on one team colour, P1's team red
    // and the other blue. Character select, first loaded when the logo ends,
    // keeps the slots a battle left (here: these colours, every port human,
    // no character yet) instead of its P1+P2 vs P3+P4 defaults. Players may
    // change colours afterwards; only the pairing counts.
    if (scene == kSceneN64Logo && i->lobbyPairing)
    {
        pin(i, kTransferBattle + kBattleResetPlayers, 1, 0);
        for (int port : i->ports)
        {
            const uint32_t player = kTransferBattle + kBattlePlayers + static_cast<uint32_t>(port - 1) * kPlayerStride;
            pin(i, player + kPlayerPkind, 1, kPkindHuman);
            pin(i, player + kPlayerTeam, 1, i->pairing[port - 1] == 1 ? kTeamRed : kTeamBlue);
        }
    }

    if (scene == kSceneVsCss && css_loaded(i))
    {
        // the CSS overlay is loaded fresh each time: its rule buttons and
        // Back do nothing, so its copy of the rules (set up from the battle
        // state below and Remix's VS globals) can't change
        patch_code(i, kCssCheckTimeArrowR, kMipsJrRa, kMipsClearV0, "character select stock/time + arrow");
        patch_code(i, kCssCheckTimeArrowL, kMipsJrRa, kMipsClearV0, "character select stock/time - arrow");
        patch_code(i, kCssCheckGameMode, kMipsJrRa, kMipsClearV0, "character select team toggle");
        patch_code(i, kCssCheckBack, kMipsJrRa, kMipsClearV0, "character select back");
        if (read32(i, kCssBackToVsMode) == kCssBackFirstWord)
        {
            patch_code(i, kCssBackToVsMode, kMipsJrRa, kMipsNop, "character select back action");
        }

        if (!i->booted)
        {
            i->booted = true;
            module_log(i, RankedLog_Verbose, "set %d of %zu ruleset values while booting",
                i->bootCorrections, i->pins.size());
        }
    }

    // VS rules are read when a battle starts, never touch them mid battle
    if (scene != kSceneVsBattle)
    {
        pin(i, kTransferBattle + kBattleIsTeam, 1, r->teamSize > 1 ? 1 : 0);
        if (r->teamSize > 1)
        {
            pin(i, kTransferBattle + kBattleTeamAttack, 1, 1);
        }
        pin(i, kTransferBattle + kBattleRules, 1, 3);
        pin(i, kTransferBattle + kBattleTimeLimit, 1, r->timeMinutes);
        pin(i, kTransferBattle + kBattleStocks, 1, r->stocks);
        pin(i, kTransferBattle + kBattleHandicap, 1, 0);
        pin(i, kTransferBattle + kBattleStageSelect, 1, i->stageSelect ? 1 : 0);
        pin(i, kTransferBattle + kBattleDamageRatio, 1, 100);
        pin(i, kTransferBattle + kBattleItemToggles, 4, 0);
        pin(i, kTransferBattle + kBattleItemRate, 1, 0);
    }

    // With stage select the hazard mode follows the stage: the stage select
    // screen sets it for the hovered stage (and Random for the stage it
    // picks), every other screen keeps the chosen stage's, which the battle
    // sets its hazards up from. Leaving stage select with B keeps the last
    // hovered stage's, so it's written back here.
    if (i->stageSelect && scene != kSceneStageSelect)
    {
        const uint32_t address = i->hazardModeEntry + kEntryValue;
        const uint32_t mode = stage_hazard_mode(i, read8(i, kTransferBattle + kBattleGkind));
        if (read32(i, address) != mode)
        {
            write32(i, address, mode);
        }
    }

    // only character select, (with stage select) stage select, battle and
    // results are reachable: the title and every menu goes back to
    // character select, and so does any other screen entered from them.
    // Boot screens (and the uninitialized scene byte before the first scene)
    // are left alone.
    auto is_ranked_scene = [i](uint8_t s)
    {
        return s == kSceneVsCss || s == kSceneVsBattle || s == kSceneVsResults ||
               (i->stageSelect && s == kSceneStageSelect);
    };
    uint8_t previous = read8(i, kScenePrevious);
    bool ranked = is_ranked_scene(scene);
    bool menu = scene == kSceneTitle || scene == kSceneModeSelect || scene == kSceneVsMenu ||
                scene == kSceneVsOptions || scene == kSceneItemSwitch || scene == kSceneStageSelect ||
                scene == kSceneRemixOptions || scene == kSceneRemixModes;
    bool fromRanked = is_ranked_scene(previous);
    if (!ranked && (menu || fromRanked))
    {
        // the patches above keep the game out of the menus; this is the net
        write8(i, kScenePrevious, scene);
        write8(i, kSceneCurrent, kSceneVsCss);
        write32(i, kSceneInterrupt, 1);
        if (i->reportedDrift.insert(0x100u | scene).second)
        {
            module_log(i, i->booted ? RankedLog_Warning : RankedLog_Verbose,
                "sent scene 0x%02X back to character select%s", scene,
                i->booted ? ": a way into the menus isn't blocked" : "");
        }
    }
}

// Remix code is DMA'd in at boot; until then its symbols point at garbage
bool remix_ready(Instance* i)
{
    return read32(i, i->profileHeadTable) == i->headRemixSettings;
}

void observe(Instance* i, Observation& o)
{
    o.ready = 1;
    o.scene = read8(i, kSceneCurrent);
    o.isReset = read8(i, kSceneIsReset);

    uint32_t battle = read32(i, kActiveBattlePtr);
    if (battle != kTransferBattle && battle != kSuddenDeathBattle)
    {
        battle = kTransferBattle;
    }
    o.isSuddenDeath = battle == kSuddenDeathBattle ? 1 : 0;
    o.battleGkind = read8(i, battle + kBattleGkind);
    o.status = read8(i, battle + kBattleGameStatus);
    o.gameRules = read8(i, battle + kBattleRules);
    o.stocks = read8(i, battle + kBattleStocks);
    o.isTeam = read8(i, battle + kBattleIsTeam);
    o.teamAttack = read8(i, battle + kBattleTeamAttack);
    o.itemRate = read8(i, battle + kBattleItemRate);
    o.itemToggles = read32(i, battle + kBattleItemToggles);
    // a battle that was just set up has a full timer and nothing elapsed; a
    // previous game's leftover state (ran down) or boot state (0) never does
    uint8_t timeLimit = read8(i, battle + kBattleTimeLimit);
    o.inCountdown = o.status == kStatusWait && read32(i, battle + kBattleTimePassed) == 0 &&
                    read32(i, battle + kBattleTimeRemain) == static_cast<uint32_t>(timeLimit) * 60 * 60 &&
                    timeLimit != 0 ? 1 : 0;
    o.vsModeFlag = static_cast<uint8_t>(read32(i, i->vsModeFlag));
    o.saltyRunback = i->saltyRunbackFlag != 0 && read32(i, i->saltyRunbackFlag) != 0 ? 1 : 0;
    o.hazardMode = static_cast<uint8_t>(read32(i, i->hazardModeEntry + kEntryValue));

    for (uint32_t port = 0; port < RANKED_MAX_PLAYERS; port++)
    {
        uint32_t player = battle + kBattlePlayers + port * kPlayerStride;
        o.players[port].pkind = read8(i, player + kPlayerPkind);
        o.players[port].fkind = read8(i, player + kPlayerFkind);
        o.players[port].stockCount = static_cast<int8_t>(read8(i, player + kPlayerStockCount));
        o.players[port].place = read8(i, player + kPlayerPlace);
        o.players[port].team = read8(i, player + kPlayerTeam);
    }
}

//
// Tracking
//

bool is_ranked_port(Instance* i, int port)
{
    return std::find(i->ports.begin(), i->ports.end(), port) != i->ports.end();
}

bool team_set(Instance* i)
{
    return i->ruleset->teamSize > 1;
}

// A 2v2 game's pairing from the team colours: each ranked port's team, 1 for
// the team holding the lowest ranked port, 2 for the other. Empty when the
// ports aren't two teams of teamSize.
std::vector<int> pairing_of(Instance* i, const Observation& o)
{
    std::vector<int> teams(RANKED_MAX_PLAYERS, 0);
    int lowest = *std::min_element(i->ports.begin(), i->ports.end());
    uint8_t first = o.players[lowest - 1].team;
    int count[3] = {0, 0, 0};
    uint8_t second = 0xFF;
    for (int port : i->ports)
    {
        uint8_t colour = o.players[port - 1].team;
        if (colour == first)
        {
            teams[port - 1] = 1;
        }
        else if (second == 0xFF || colour == second)
        {
            second = colour;
            teams[port - 1] = 2;
        }
        else
        {
            return {}; // a third team
        }
        count[teams[port - 1]]++;
    }
    if (count[1] != i->ruleset->teamSize || count[2] != i->ruleset->teamSize)
    {
        return {};
    }
    return teams;
}

// "P1+P3 vs P2+P4", empty before the pairing is known
std::string pairing_text(Instance* i)
{
    std::string sides[3];
    for (int port = 1; port <= RANKED_MAX_PLAYERS; port++)
    {
        int team = i->pairing[port - 1];
        if (team == 1 || team == 2)
        {
            sides[team] += (sides[team].empty() ? "P" : "+P") + std::to_string(port);
        }
    }
    if (sides[1].empty() || sides[2].empty())
    {
        return std::string();
    }
    return sides[1] + " vs " + sides[2];
}

// Sets the pairing the lobby requires (RankedSessionConfig.teams, numbered
// like pairing_of) for every game of the set. All 0 leaves it to game 1;
// anything but two teams of teamSize among the ranked ports is an error.
std::string lobby_pairing(Instance* i, const int32_t* teams)
{
    if (std::all_of(teams, teams + RANKED_MAX_PLAYERS, [](int32_t team) { return team == 0; }))
    {
        return std::string();
    }
    const int lowest = *std::min_element(i->ports.begin(), i->ports.end());
    int pairing[RANKED_MAX_PLAYERS] = {0, 0, 0, 0};
    int count[3] = {0, 0, 0};
    for (int port = 1; port <= RANKED_MAX_PLAYERS; port++)
    {
        const int32_t team = teams[port - 1];
        if (!is_ranked_port(i, port))
        {
            if (team != 0)
            {
                return "the lobby's teams include P" + std::to_string(port) + ", who isn't in the set";
            }
            continue;
        }
        if (team != 1 && team != 2)
        {
            return "the lobby's teams leave P" + std::to_string(port) + " without a team";
        }
        pairing[port - 1] = team == teams[lowest - 1] ? 1 : 2;
        count[pairing[port - 1]]++;
    }
    if (count[1] != i->ruleset->teamSize || count[2] != i->ruleset->teamSize)
    {
        return "the lobby's teams aren't two teams of " + std::to_string(i->ruleset->teamSize);
    }
    std::copy(pairing, pairing + RANKED_MAX_PLAYERS, i->pairing);
    i->lobbyPairing = true;
    module_log(i, RankedLog_Info, "teams for the set (matched by the lobby): %s", pairing_text(i).c_str());
    return std::string();
}

std::string validate_game(Instance* i, const Observation& o)
{
    char reason[128];
    if (o.vsModeFlag != 0)
    {
        std::snprintf(reason, sizeof(reason), "not normal VS mode (%u)", o.vsModeFlag);
        return reason;
    }
    for (int port = 1; port <= RANKED_MAX_PLAYERS; port++)
    {
        uint8_t pkind = o.players[port - 1].pkind;
        if (is_ranked_port(i, port) ? pkind != kPkindHuman : pkind != kPkindNone)
        {
            std::snprintf(reason, sizeof(reason), "port %d has player kind %u", port, pkind);
            return reason;
        }
    }
    for (int port : i->ports)
    {
        const std::vector<uint8_t>& roster = i->ruleset->roster;
        uint8_t fkind = o.players[port - 1].fkind;
        if (std::find(roster.begin(), roster.end(), fkind) == roster.end())
        {
            std::snprintf(reason, sizeof(reason), "P%d's character (id 0x%02X) isn't in the %s roster",
                port, fkind, i->ruleset->rosterName);
            return reason;
        }
    }
    const RankedStage* stage = find_stage(i, o.battleGkind);
    if (stage == nullptr)
    {
        std::snprintf(reason, sizeof(reason), "stage 0x%02X isn't allowed", o.battleGkind);
        return reason;
    }
    uint8_t wantTeam = team_set(i) ? 1 : 0;
    if (!(o.gameRules & 2) || o.stocks != i->ruleset->stocks || o.isTeam != wantTeam ||
        o.itemRate != 0 || o.itemToggles != 0 || (team_set(i) && o.teamAttack != 1))
    {
        std::snprintf(reason, sizeof(reason),
            "rules differ: rules=%u stocks=%u team=%u teamAttack=%u itemRate=%u items=%08X",
            o.gameRules, o.stocks, o.isTeam, o.teamAttack, o.itemRate, o.itemToggles);
        return reason;
    }
    if (team_set(i))
    {
        std::vector<int> teams = pairing_of(i, o);
        if (teams.empty())
        {
            return "not two teams of two";
        }
        if (i->pairing[0] + i->pairing[1] + i->pairing[2] + i->pairing[3] != 0 &&
            !std::equal(teams.begin(), teams.end(), i->pairing))
        {
            if (i->lobbyPairing)
            {
                return "the teams must be " + pairing_text(i) + " (the teammates the lobby matched)";
            }
            return "the teams changed partners (the set keeps game 1's teams)";
        }
    }
    if (o.hazardMode != stage->hazardMode)
    {
        std::snprintf(reason, sizeof(reason), "hazard mode %u, the stage's is %u", o.hazardMode, stage->hazardMode);
        return reason;
    }
    // the Salty Runback toggle is forced off; a runback skips the character
    // screen (no counterpicks), so it never counts even if one slips through
    if (o.saltyRunback != 0)
    {
        return "Salty Runback restart";
    }
    return std::string();
}

void fill_event(Instance* i, RankedEvent& event, RankedEventType type, uint32_t frame)
{
    std::memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    event.type = type;
    event.frame = frame;
    event.gameNumber = i->gamesCounted + 1;
    event.stage = i->gameStage;
    for (int port = 0; port < RANKED_MAX_PLAYERS; port++)
    {
        event.score[port] = i->score[port];
        event.players[port] = {-1, -1, -1};
        event.teams[port] = i->pairing[port];
    }
}

void emit(Instance* i, const RankedEvent& event)
{
    i->host.EmitEvent(i->host.context, &event);
}

void void_game(Instance* i, uint32_t frame, const std::string& reason)
{
    RankedEvent event;
    fill_event(i, event, RankedEvent_GameVoided, frame);
    copy_string(event.detail, sizeof(event.detail), reason);
    i->gamesVoided++;
    emit(i, event);
}

// 2v2: the team with stocks left wins (more stocks between its players on a
// timeout). Both members share the team's score.
void finish_team_game(Instance* i, uint32_t frame, const Observation& o)
{
    std::vector<int> teams = pairing_of(i, o);
    if (teams.empty())
    {
        void_game(i, frame, "not two teams of two");
        return;
    }
    int stocks[3] = {0, 0, 0};
    for (int port : i->ports)
    {
        stocks[teams[port - 1]] += std::max(0, o.players[port - 1].stockCount + 1);
    }
    if (stocks[1] == stocks[2])
    {
        void_game(i, frame, "no single winner");
        return;
    }
    int winnerTeam = stocks[1] > stocks[2] ? 1 : 2;
    int winnerPort = 0;
    for (int port : i->ports)
    {
        if (teams[port - 1] == winnerTeam)
        {
            i->score[port - 1]++;
            if (winnerPort == 0 || port < winnerPort)
            {
                winnerPort = port;
            }
        }
    }

    RankedEvent event;
    fill_event(i, event, RankedEvent_GameEnded, frame);
    event.winnerPort = winnerPort;
    event.winnerTeam = winnerTeam;
    for (int port : i->ports)
    {
        const PlayerObservation& player = o.players[port - 1];
        event.players[port - 1] = {player.fkind, player.stockCount, -1};
    }
    i->gamesCounted++;
    emit(i, event);

    if (i->score[winnerPort - 1] >= i->bestOf / 2 + 1)
    {
        i->setOver = true;
        i->setWinner = winnerPort;
        i->setWinnerTeam = winnerTeam;

        RankedEvent setEvent;
        fill_event(i, setEvent, RankedEvent_SetEnded, frame);
        setEvent.gameNumber = i->gamesCounted;
        setEvent.winnerPort = winnerPort;
        setEvent.winnerTeam = winnerTeam;
        emit(i, setEvent);
    }
}

void finish_game(Instance* i, uint32_t frame)
{
    const Observation& o = i->lastInBattle;

    if (!i->invalidReason.empty())
    {
        void_game(i, frame, i->invalidReason);
        return;
    }
    if (o.isReset)
    {
        void_game(i, frame, "match was reset");
        return;
    }
    // a normal finish passes End (5) and leaves the scene in End or Set (7);
    // resets are told apart by the reset flag above
    if (!i->gameReachedEnd && o.status != kStatusEnd && o.status != kStatusSet)
    {
        char reason[64];
        std::snprintf(reason, sizeof(reason), "match left with status %u", o.status);
        void_game(i, frame, reason);
        return;
    }

    if (team_set(i))
    {
        finish_team_game(i, frame, o);
        return;
    }

    // winner: most stocks left; a tie is only possible without sudden death
    int winner = 0;
    int best = -2;
    bool tie = false;
    for (int port : i->ports)
    {
        int stocks = o.players[port - 1].stockCount;
        if (stocks > best)
        {
            best = stocks;
            winner = port;
            tie = false;
        }
        else if (stocks == best)
        {
            tie = true;
        }
    }
    if (tie || winner == 0)
    {
        void_game(i, frame, "no single winner");
        return;
    }

    i->score[winner - 1]++;

    RankedEvent event;
    fill_event(i, event, RankedEvent_GameEnded, frame);
    event.winnerPort = winner;
    for (int port : i->ports)
    {
        const PlayerObservation& player = o.players[port - 1];
        event.players[port - 1] = {player.fkind, player.stockCount, -1};
    }
    i->gamesCounted++;
    emit(i, event);

    int needed = i->bestOf / 2 + 1;
    if (i->score[winner - 1] >= needed)
    {
        i->setOver = true;
        i->setWinner = winner;

        RankedEvent setEvent;
        fill_event(i, setEvent, RankedEvent_SetEnded, frame);
        setEvent.gameNumber = i->gamesCounted;
        setEvent.winnerPort = winner;
        emit(i, setEvent);
    }
}

void track(Instance* i, uint32_t frame, const Observation& o)
{
    if (!o.ready || i->setOver)
    {
        i->previous = o;
        return;
    }

    if (!i->previous.ready || i->previous.scene != o.scene)
    {
        module_log(i, RankedLog_Verbose, "frame %u: scene 0x%02X -> 0x%02X",
            frame, i->previous.ready ? i->previous.scene : 0xFF, o.scene);
    }

    bool wasBattle = i->previous.ready && i->previous.scene == kSceneVsBattle;
    bool isBattle = o.scene == kSceneVsBattle;

    if (isBattle && !wasBattle)
    {
        i->inBattle = true;
        i->gameStarted = false;
        i->gameReachedEnd = false;
        i->invalidReason.clear();
    }

    if (i->inBattle && isBattle)
    {
        if (!i->gameStarted && (o.inCountdown || o.status == kStatusGo))
        {
            i->gameStarted = true;
            i->gameStage = o.battleGkind;
            i->invalidReason = validate_game(i, o);
            if (team_set(i) && i->invalidReason.empty() &&
                i->pairing[0] + i->pairing[1] + i->pairing[2] + i->pairing[3] == 0)
            {
                std::vector<int> teams = pairing_of(i, o);
                std::copy(teams.begin(), teams.end(), i->pairing);
                module_log(i, RankedLog_Info, "teams for the set: %s", pairing_text(i).c_str());
            }

            RankedEvent event;
            fill_event(i, event, RankedEvent_GameStarted, frame);
            copy_string(event.detail, sizeof(event.detail), i->invalidReason);
            emit(i, event);
        }
        if (i->gameStarted && o.status != kStatusWait)
        {
            i->lastInBattle = o;
            if (o.status == kStatusEnd)
            {
                i->gameReachedEnd = true;
            }
        }
    }

    if (i->inBattle && wasBattle && !isBattle)
    {
        i->inBattle = false;
        if (i->gameStarted)
        {
            finish_game(i, frame);
        }
    }

    i->previous = o;
}

} // namespace

//
// Exported Functions
//

MODULE_EXPORT int RankedModule_GetInfo(RankedModuleInfo* info)
{
    if (info == nullptr || info->size < sizeof(RankedModuleInfo))
    {
        return 0;
    }
    info->apiVersion = RANKED_MODULE_API_VERSION;
    info->id = "smashremix";
    info->name = "Smash Remix";
    info->version = kModuleVersion;
    return 1;
}

MODULE_EXPORT RankedModuleInstance* RankedModule_Create(const RankedHostApi* host,
    const RankedSessionConfig* config, char* error, uint32_t errorSize)
{
    if (host == nullptr || config == nullptr || host->size < sizeof(RankedHostApi) ||
        config->size < RANKED_SESSION_CONFIG_SIZE_V1)
    {
        copy_string(error, errorSize, "incompatible host");
        return nullptr;
    }

    const Ruleset* ruleset = nullptr;
    std::string rulesetId = (config->rulesetId && config->rulesetId[0]) ? config->rulesetId : "";
    for (const Ruleset& candidate : get_rulesets())
    {
        if (rulesetId.empty() || rulesetId == candidate.id)
        {
            ruleset = &candidate;
            break;
        }
    }
    if (ruleset == nullptr)
    {
        copy_string(error, errorSize, "unknown ruleset '" + rulesetId + "'");
        return nullptr;
    }

    if (config->numPlayers != 2 * ruleset->teamSize)
    {
        copy_string(error, errorSize, std::string("ruleset '") + ruleset->id + "' needs " +
            std::to_string(2 * ruleset->teamSize) + " players");
        return nullptr;
    }

    std::string stageError = check_stages(ruleset->stages);
    if (!stageError.empty())
    {
        copy_string(error, errorSize, std::string("ruleset '") + ruleset->id + "': " + stageError);
        return nullptr;
    }

    std::string romMd5 = to_upper(config->romMd5 ? config->romMd5 : "");
    std::string dataDirectory = config->dataDirectory ? config->dataDirectory : "";
    std::string symbolPath = dataDirectory + "/" + romMd5 + ".sym";

    std::map<std::string, uint32_t> symbols;
    if (!load_symbols(symbolPath, symbols))
    {
        copy_string(error, errorSize, "this ROM (MD5 " + romMd5 +
            ") is not a supported Smash Remix build: no symbol file '" + symbolPath + "'");
        return nullptr;
    }

    auto instance = new Instance();
    instance->host = *host;
    instance->ruleset = ruleset;
    instance->bestOf = config->bestOf;
    instance->romMd5 = romMd5;
    for (int32_t n = 0; n < config->numPlayers; n++)
    {
        instance->ports.push_back(config->ports[n]);
    }

    // teammates the lobby matched (hosts that predate the field send none)
    if (team_set(instance) && config->size >= offsetof(RankedSessionConfig, teams) + sizeof(config->teams))
    {
        std::string pairingError = lobby_pairing(instance, config->teams);
        if (!pairingError.empty())
        {
            copy_string(error, errorSize, pairingError);
            delete instance;
            return nullptr;
        }
    }

    std::string missing;
    auto resolve = [&](const std::string& name, bool required) -> uint32_t
    {
        auto it = symbols.find(name);
        if (it == symbols.end())
        {
            if (required)
            {
                missing += (missing.empty() ? "" : ", ") + name;
            }
            return 0;
        }
        return it->second;
    };

    instance->profileHeadTable  = resolve("Toggles.profile_head_table", true);
    instance->headRemixSettings = resolve("Toggles.head_remix_settings", true);
    instance->profileDefaults   = resolve(std::string("Toggles.profile_defaults_") + ruleset->profile, true);
    instance->vsModeFlag        = resolve("VsRemixMenu.vs_mode_flag", true);
    instance->hazardModeEntry   = resolve("Toggles.entry_hazard_mode", true);
    instance->inputDelayTable   = resolve("InputDelay.delay_table", false);
    instance->stockModeTable    = resolve("StockMode.stockmode_table", false);
    instance->settingsShortcut  = resolve("Toggles.settings_shortcut", false);
    instance->pickerPress       = resolve("CharacterSelect.check_picker_press_", false);
    instance->saltyRunbackFlag  = resolve("GameEnd.is_salty_runback", false);
    instance->globalGameMode    = resolve("VsRemixMenu.global_game_mode", true);
    instance->globalTime        = resolve("VsRemixMenu.global_time", true);
    instance->globalStocks      = resolve("VsRemixMenu.global_stocks", true);
    instance->globalTeams       = resolve("VsRemixMenu.global_teams", true);
    instance->globalStageSelect = resolve("VsRemixMenu.global_stage_select", true);
    instance->globalStockModeTable = resolve("VsRemixMenu.global_stockmode_table", false);

    // stages
    instance->stageSelect          = ruleset->stages.size() > 1;
    instance->tournamentDefaults   = resolve("Toggles.profile_defaults_TE", true);
    instance->randomStageList      = resolve("Stages.swap_stage_", true);
    instance->addRandomStage       = resolve("Stages.add_stage_to_random_list_", true);
    instance->dreamLandRandomEntry = resolve("Toggles.entry_random_stage_dream_land", false);
    if (instance->stageSelect)
    {
        instance->sssLayoutEntry    = resolve("Toggles.entry_sss_layout", true);
        instance->stageTable        = resolve("Stages.stage_table", true);
        instance->stageTablePages   = resolve("Stages.stage_table_pages", true);
        instance->tournamentTable   = resolve("Stages.stage_table_tournament", true);
        instance->tournamentHazards = resolve("Stages.tournament_hazard_mode_table", true);
        instance->hoveredStage      = resolve("Stages.get_stage_id_", true);
    }

    for (const ToggleOverride& toggle : ruleset->overrides)
    {
        uint32_t address = resolve(toggle.symbol, true);
        if (address != 0)
        {
            instance->overrides.push_back({address, toggle.value});
        }
    }

    if (!missing.empty())
    {
        copy_string(error, errorSize, "symbol file '" + symbolPath + "' is missing: " + missing);
        delete instance;
        return nullptr;
    }

    // one stage: its hazard mode for every game. Stage select uses the
    // tournament layout, and the hazard mode follows the chosen stage.
    if (instance->stageSelect)
    {
        instance->overrides.push_back({instance->sssLayoutEntry, kSssLayoutTournament});
    }
    else
    {
        instance->overrides.push_back({instance->hazardModeEntry, ruleset->stages.front().hazardMode});
    }

    if (!has_write_code(*host))
    {
        module_log(instance, RankedLog_Warning, "host can't patch code; leaving the CSS can't be blocked");
    }
    if (instance->settingsShortcut == 0 || instance->pickerPress == 0)
    {
        module_log(instance, RankedLog_Warning, "symbol file '%s' predates this module (no %s): regenerate it "
            "with GenerateRankedRemixSymbols.sh, until then that part of the CSS isn't locked",
            symbolPath.c_str(), instance->settingsShortcut == 0 ? "Toggles.settings_shortcut" :
            "CharacterSelect.check_picker_press_");
    }
    std::string ports;
    for (int port : instance->ports)
    {
        ports += (ports.empty() ? "" : ", ") + std::to_string(port);
    }
    module_log(instance, RankedLog_Info, "ruleset %s (%s), best of %d, ports %s, %zu stage%s, %zu toggle overrides",
        ruleset->id, ruleset->teamSize > 1 ? "2v2 team battle" : "1v1", instance->bestOf, ports.c_str(),
        ruleset->stages.size(), instance->stageSelect ? "s to pick from" : "", instance->overrides.size());
    return reinterpret_cast<RankedModuleInstance*>(instance);
}

MODULE_EXPORT uint32_t RankedModule_GetRulesetDescription(RankedModuleInstance* handle,
    char* out, uint32_t outSize)
{
    Instance* i = reinterpret_cast<Instance*>(handle);
    const Ruleset* r = i->ruleset;

    std::ostringstream description;
    description << "ruleset=" << r->id
                << ";rom=" << i->romMd5
                << ";profile=" << r->profile
                << ";stocks=" << (r->stocks + 1)
                << ";time=" << static_cast<int>(r->timeMinutes)
                << ";stageSelect=" << (i->stageSelect ? "tournament" : "off")
                << ";stages=";
    for (const RankedStage& stage : r->stages)
    {
        description << static_cast<int>(stage.id) << ":" << static_cast<int>(stage.hazardMode) << ",";
    }
    description << ";items=off;teams=" << (r->teamSize > 1 ? "on;teamAttack=on;teamSize=2" : "off")
                << ";bestOf=" << i->bestOf
                << ";roster=";
    for (uint8_t fkind : r->roster)
    {
        description << static_cast<int>(fkind) << ",";
    }
    if (i->lobbyPairing)
    {
        description << ";pairing=";
        for (int team : i->pairing)
        {
            description << team << ",";
        }
    }
    for (const ToggleOverride& toggle : r->overrides)
    {
        description << ";" << toggle.symbol << "=" << toggle.value;
    }

    std::string text = description.str();
    copy_string(out, outSize, text);
    return static_cast<uint32_t>(text.size());
}

MODULE_EXPORT void RankedModule_OnFrame(RankedModuleInstance* handle, uint32_t frame, uint8_t* observation)
{
    (void)frame;
    Instance* i = reinterpret_cast<Instance*>(handle);
    if (!remix_ready(i))
    {
        return; // observation stays zeroed (ready = 0)
    }

    enforce(i);

    Observation o{};
    observe(i, o);
    std::memcpy(observation, &o, sizeof(o));
}

MODULE_EXPORT void RankedModule_OnConfirmedFrame(RankedModuleInstance* handle, uint32_t frame,
    const uint8_t* observation)
{
    Instance* i = reinterpret_cast<Instance*>(handle);
    Observation o;
    std::memcpy(&o, observation, sizeof(o));

    std::lock_guard<std::mutex> lock(i->stateMutex);
    track(i, frame, o);
}

MODULE_EXPORT uint32_t RankedModule_GetStatusText(RankedModuleInstance* handle, char* out, uint32_t outSize)
{
    Instance* i = reinterpret_cast<Instance*>(handle);
    std::lock_guard<std::mutex> lock(i->stateMutex);

    char text[128];
    if (team_set(i))
    {
        std::string teams = pairing_text(i);
        int wins[3] = {0, 0, 0};
        for (int port : i->ports)
        {
            wins[i->pairing[port - 1]] = i->score[port - 1];
        }
        if (teams.empty())
        {
            std::snprintf(text, sizeof(text), "Ranked 2v2 Bo%d - Game %d - pick two teams of two",
                i->bestOf, i->gamesCounted + 1);
        }
        else if (i->setOver)
        {
            std::snprintf(text, sizeof(text), "Ranked 2v2 Bo%d - %s - team %d wins %d-%d",
                i->bestOf, teams.c_str(), i->setWinnerTeam, wins[i->setWinnerTeam], wins[3 - i->setWinnerTeam]);
        }
        else
        {
            std::snprintf(text, sizeof(text), "Ranked 2v2 Bo%d - Game %d - %s - %d : %d",
                i->bestOf, i->gamesCounted + 1, teams.c_str(), wins[1], wins[2]);
        }
        copy_string(out, outSize, text);
        return static_cast<uint32_t>(std::strlen(text));
    }
    int a = i->ports[0], b = i->ports[1];
    if (i->setOver)
    {
        std::snprintf(text, sizeof(text), "Ranked Bo%d - P%d wins the set %d-%d",
            i->bestOf, i->setWinner, i->score[i->setWinner - 1],
            i->score[(i->setWinner == a ? b : a) - 1]);
    }
    else
    {
        std::snprintf(text, sizeof(text), "Ranked Bo%d - Game %d - P%d %d : %d P%d",
            i->bestOf, i->gamesCounted + 1, a, i->score[a - 1], i->score[b - 1], b);
    }
    copy_string(out, outSize, text);
    return static_cast<uint32_t>(std::strlen(text));
}

MODULE_EXPORT void RankedModule_Destroy(RankedModuleInstance* handle)
{
    delete reinterpret_cast<Instance*>(handle);
}
