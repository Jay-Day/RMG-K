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
 * The module only reads the game. The lobby's game options for the ruleset
 * (ROM patches, see Data/GameOptions) make the game play by it: boot to VS
 * character select, menus out of reach, the rules, the Tournament toggle
 * profile, the stages, and the teams the lobby matched. The module checks
 * that each game was played that way and voids it when not.
 *
 * Observation (OnFrame, read-only): the battle state (rules, players, teams,
 * stage), Remix's VS mode, hazard mode, per-port input delay and stock mode,
 * the Salty Runback flag, and during a battle whether every Remix toggle
 * holds its Tournament profile value (Toggles.profile_defaults_TE with the
 * ruleset's overrides). The toggles the stages decide (random stages, stage
 * select layout, hazard mode) are judged by the stage instead.
 *
 * Tracking (OnConfirmedFrame):
 *  - a game starts during the pre-GO countdown (fresh timer, status Wait),
 *    so its on-screen message shows before players can move
 *  - it ends when the battle scene is left; the winner is the only ranked
 *    port with stocks left (or the one with more stocks on a timeout)
 *  - 2v2 rulesets: team battle with team attack, all four ports human in two
 *    teams of two (any ports, team colours don't matter). The pairing is
 *    fixed by game 1, or from the start when the lobby matched teammates
 *    (RankedSessionConfig.teams; its teams game option opens character
 *    select with each team on one colour, P1's red); the winning team is
 *    the one with stocks left (more stocks between its two players on a
 *    timeout)
 *  - resets, CPUs, extra players, a stage that isn't allowed (or its hazard
 *    mode differs), wrong rules (stocks, time, items, handicap, damage
 *    ratio, teams, team attack, VS mode, input delay, stock mode, any other
 *    Remix setting), a character outside the ruleset's roster, a Salty
 *    Runback restart, or (2v2) other partners void the game
 *  - RankedModule_GetLiveState: the score and, during a game that counts,
 *    each ranked port's stocks and damage percent, for when a player leaves
 *    mid-set
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
constexpr uint32_t kSceneIsReset       = 0x800A4AE2; // u8 A+B+Z+R reset during battle
constexpr uint32_t kTransferBattle     = 0x800A4D08; // SCBattleState used by VS mode
constexpr uint32_t kSuddenDeathBattle  = 0x800A4EF8; // SCBattleState copy for sudden death
constexpr uint32_t kActiveBattlePtr    = 0x800A50E8; // SCBattleState* in use

// SCBattleState offsets
constexpr uint32_t kBattleGkind        = 0x01;
constexpr uint32_t kBattleIsTeam       = 0x02;
constexpr uint32_t kBattleRules        = 0x03; // 1 = time, 2 = stock (3: both)
constexpr uint32_t kBattleTimeLimit    = 0x06; // minutes, 100 = infinite
constexpr uint32_t kBattleStocks       = 0x07; // 0-based
constexpr uint32_t kBattleHandicap     = 0x08; // 0 = off
constexpr uint32_t kBattleTeamAttack   = 0x09;
constexpr uint32_t kBattleDamageRatio  = 0x0B; // percent
constexpr uint32_t kBattleItemToggles  = 0x0C; // u32
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
constexpr uint32_t kPlayerDamage       = 0x4C; // u32, the current stock's damage percent

constexpr uint8_t kSceneVsBattle       = 0x16;

constexpr uint8_t kStatusWait          = 0;
constexpr uint8_t kStatusGo            = 1;
constexpr uint8_t kStatusEnd           = 5;
constexpr uint8_t kStatusSet           = 7; // after GAME SET (and on resets)

constexpr uint8_t kPkindHuman          = 0;
constexpr uint8_t kPkindNone           = 2;

constexpr uint8_t kRulesTimedStock     = 3; // time and stock
constexpr uint8_t kNoHandicap          = 0;
constexpr uint8_t kNormalDamageRatio   = 100;

// Remix Menu.entry layout
constexpr uint32_t kEntryType          = 0x00;
constexpr uint32_t kEntryValue         = 0x04;
constexpr uint32_t kEntryNext          = 0x1C;
constexpr uint32_t kEntryTitle         = 0x28; // inline string
constexpr uint32_t kEntryTypeTitle     = 0x00;
constexpr uint32_t kEntryTypeInput     = 0x0B;
constexpr int      kMaxEntriesPerHead  = 4096;
constexpr int      kMaxHeads           = 32;

// Remix stage select (Stages.asm): 18 icons per page, Random is the last
constexpr uint32_t kSssIconsPerPage    = 18;
constexpr uint32_t kTournamentPages    = 3;  // stage_table_tournament's size
constexpr uint8_t  kStageRandom        = 0xDE;

// the random stage toggles (one per stage), judged by the stage instead
constexpr const char* kRandomStageTogglePrefix = "Toggles.entry_random_stage_";

constexpr const char* kModuleVersion   = "1.0.0";

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

// The stages a ruleset's games may be played on, each with its hazard mode;
// a game anywhere else, or with another hazard mode, doesn't count. The
// lobby's stage game option for the ruleset (stages-dream-land,
// stages-remix-ranked) makes them the only ones on offer:
//  - one stage: stage select stays off and every game is on it
//  - more: players pick on the stage select screen, which shows only these
//    (and Random, which picks among them), at most 51 (3 pages of 17)
// A list is part of the ruleset hash, so peers must run the same one:
// changing it needs that game option changed too, a new module version
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
    uint16_t damage; // percent, capped at 999
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
    uint8_t timeLimit;    // minutes
    uint8_t handicap;
    uint8_t damageRatio;
    uint8_t inputDelay[RANKED_MAX_PLAYERS]; // InputDelay.delay_table, frames (capped at 255)
    uint8_t stockMode[RANKED_MAX_PLAYERS];  // StockMode.stockmode_table
    // in a battle: the first Remix toggle that isn't the ruleset's (1-based
    // index into Instance::toggleChecks, 0 when all are) and its value
    uint16_t toggleMismatch;
    uint32_t toggleValue;
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
    uint32_t saltyRunbackFlag = 0; // GameEnd.is_salty_runback (word), 0 = unknown
    std::vector<std::pair<uint32_t, uint32_t>> overrides; // entry address, value
    bool stageSelect = false;      // more than one allowed stage
    // Toggles the stage decides (hazard mode, stage select layout, random
    // stages): the stage check judges those, not the profile.
    std::set<uint32_t> stageEntries;

    // Every other Remix toggle's ruleset value, built once Remix is loaded
    // from constant Remix data, so every peer and every resimulated frame
    // gets the same. A game where one differs doesn't count.
    struct ToggleCheck
    {
        uint32_t entry;
        uint32_t value;
        std::string title;
    };
    std::vector<ToggleCheck> toggleChecks;
    bool togglesBuilt = false;

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

uint8_t  read8(Instance* i, uint32_t a)  { return i->host.Read8(i->host.context, a); }
uint32_t read32(Instance* i, uint32_t a) { return i->host.Read32(i->host.context, a); }

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
// Ruleset checks
//

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

// a Toggles entry's title, for void reasons
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

// Each Remix toggle's ruleset value: the profile's default (in the order
// Toggles.load_profile_ walks the entries) or the ruleset's override. Title
// and input entries hold no value; the stage's toggles are left to the stage
// check. Read once Remix is loaded, from constant data.
void build_toggle_checks(Instance* i)
{
    std::map<uint32_t, uint32_t> overrides(i->overrides.begin(), i->overrides.end());
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
            const uint32_t type = read32(i, entry + kEntryType);
            if (type != kEntryTypeTitle && type != kEntryTypeInput)
            {
                if (i->stageEntries.count(entry) == 0)
                {
                    auto it = overrides.find(entry);
                    i->toggleChecks.push_back({entry, it != overrides.end() ? it->second : read32(i, defaults),
                        entry_title(i, entry)});
                }
                defaults += 4;
            }
            entry = read32(i, entry + kEntryNext);
        }
    }
    i->togglesBuilt = true;
    module_log(i, RankedLog_Verbose, "%zu Remix toggles checked against the %s profile", i->toggleChecks.size(),
        i->ruleset->profile);
}

// the first toggle that isn't the ruleset's (see Observation)
void check_toggles(Instance* i, Observation& o)
{
    for (size_t n = 0; n < i->toggleChecks.size() && n < 0xFFFF; n++)
    {
        const uint32_t value = read32(i, i->toggleChecks[n].entry + kEntryValue);
        if (value != i->toggleChecks[n].value)
        {
            o.toggleMismatch = static_cast<uint16_t>(n + 1);
            o.toggleValue = value;
            return;
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
    o.timeLimit = timeLimit;
    o.handicap = read8(i, battle + kBattleHandicap);
    o.damageRatio = read8(i, battle + kBattleDamageRatio);
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
        o.players[port].damage = static_cast<uint16_t>(std::min<uint32_t>(read32(i, player + kPlayerDamage), 999));
        if (i->inputDelayTable != 0)
        {
            o.inputDelay[port] = static_cast<uint8_t>(std::min<uint32_t>(read32(i, i->inputDelayTable + port * 4), 255));
        }
        if (i->stockModeTable != 0)
        {
            o.stockMode[port] = static_cast<uint8_t>(std::min<uint32_t>(read32(i, i->stockModeTable + port * 4), 255));
        }
    }

    // only a game's start is judged, and the settings can't change in battle
    if (o.scene == kSceneVsBattle)
    {
        check_toggles(i, o);
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
    if (o.gameRules != kRulesTimedStock || o.stocks != i->ruleset->stocks || o.timeLimit != i->ruleset->timeMinutes ||
        o.isTeam != wantTeam || (team_set(i) && o.teamAttack != 1) || o.handicap != kNoHandicap ||
        o.damageRatio != kNormalDamageRatio || o.itemRate != 0 || o.itemToggles != 0)
    {
        std::snprintf(reason, sizeof(reason),
            "rules differ: rules=%u stocks=%u time=%u team=%u teamAttack=%u handicap=%u damage=%u itemRate=%u items=%08X",
            o.gameRules, o.stocks, o.timeLimit, o.isTeam, o.teamAttack, o.handicap, o.damageRatio, o.itemRate,
            o.itemToggles);
        return reason;
    }
    for (int port : i->ports)
    {
        if (o.inputDelay[port - 1] != 0 || o.stockMode[port - 1] != 0)
        {
            std::snprintf(reason, sizeof(reason), "rules differ: P%d has input delay %u, stock mode %u",
                port, o.inputDelay[port - 1], o.stockMode[port - 1]);
            return reason;
        }
    }
    if (o.toggleMismatch != 0 && o.toggleMismatch <= i->toggleChecks.size())
    {
        const Instance::ToggleCheck& toggle = i->toggleChecks[o.toggleMismatch - 1];
        std::snprintf(reason, sizeof(reason), "rules differ: Remix setting \"%s\" is %u, the ruleset's is %u",
            toggle.title.c_str(), o.toggleValue, toggle.value);
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
    instance->saltyRunbackFlag  = resolve("GameEnd.is_salty_runback", false);
    instance->stageSelect       = ruleset->stages.size() > 1;

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

    // The stage's toggles: its hazard mode, the stage select layout and the
    // stages Random picks from. A game counts on an allowed stage with its
    // hazard mode, however those are set.
    instance->stageEntries.insert(instance->hazardModeEntry);
    if (const uint32_t layout = resolve("Toggles.entry_sss_layout", false); layout != 0)
    {
        instance->stageEntries.insert(layout);
    }
    for (const auto& [name, address] : symbols)
    {
        if (name.rfind(kRandomStageTogglePrefix, 0) == 0)
        {
            instance->stageEntries.insert(address);
        }
    }
    if (instance->inputDelayTable == 0 || instance->stockModeTable == 0)
    {
        module_log(instance, RankedLog_Warning, "symbol file '%s' has no %s: games don't check it",
            symbolPath.c_str(), instance->inputDelayTable == 0 ? "InputDelay.delay_table" : "StockMode.stockmode_table");
    }

    std::string ports;
    for (int port : instance->ports)
    {
        ports += (ports.empty() ? "" : ", ") + std::to_string(port);
    }
    module_log(instance, RankedLog_Info, "ruleset %s (%s), best of %d, ports %s, %zu stage%s, %zu toggle overrides "
        "(read-only, module %s)", ruleset->id, ruleset->teamSize > 1 ? "2v2 team battle" : "1v1", instance->bestOf,
        ports.c_str(), ruleset->stages.size(), instance->stageSelect ? "s to pick from" : "", instance->overrides.size(),
        kModuleVersion);
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
    if (!i->togglesBuilt)
    {
        build_toggle_checks(i);
    }

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

MODULE_EXPORT int RankedModule_GetLiveState(RankedModuleInstance* handle, RankedLiveState* out)
{
    Instance* i = reinterpret_cast<Instance*>(handle);
    if (i == nullptr || out == nullptr || out->size < sizeof(RankedLiveState))
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(i->stateMutex);

    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(RankedLiveState));
    out->size = size;
    out->gameNumber = i->gamesCounted + 1;
    // a game counts from its countdown until the battle scene is left; one
    // that won't count (wrong character, rules...) has no lead
    out->inGame = !i->setOver && i->inBattle && i->gameStarted && i->invalidReason.empty() ? 1 : 0;
    for (int port = 0; port < RANKED_MAX_PLAYERS; port++)
    {
        out->score[port] = i->score[port];
        out->teams[port] = i->pairing[port];
        out->players[port] = {-1, -1, -1};
    }
    if (out->inGame)
    {
        for (int port : i->ports)
        {
            const PlayerObservation& player = i->previous.players[port - 1];
            out->players[port - 1] = {player.fkind, std::max(0, player.stockCount + 1), player.damage};
        }
    }
    return 1;
}

MODULE_EXPORT void RankedModule_Destroy(RankedModuleInstance* handle)
{
    delete reinterpret_cast<Instance*>(handle);
}
