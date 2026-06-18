// Smash 64 — Tournament Stream Helper (TSH) Integration
//
// Sends player state (character, skin) and stage to TSH's web server.
// Communicates with http://localhost:5000/scoreboard1-*
// Assumes scoreboard 1.
//
// Configuration:
const CONFIG = {
  SERVER_URL: "http://localhost:5500",
  SCOREBOARD: 1,
  // Map port numbers to team-player coordinates: port 1 -> team 1, player 1, etc.
  PORT_MAPPING: {
    1: { team: 1, player: 1 },
    2: { team: 2, player: 1 },
    3: { team: 1, player: 2 },
    4: { team: 2, player: 2 },
  },
  UPDATE_INTERVAL: 60, // frames between updates (60 = ~1 second at 60fps)
};

// Screen IDs (Global.current_screen @ 0x800A4AD0)
const SCREENS = {
  VS_CSS: 0x10,
  CSS_1P: 0x11,
  TRAINING_CSS: 0x12,
  BONUS_1_CSS: 0x13,
  BONUS_2_CSS: 0x14,
  STAGE_SELECT: 0x15,
  VS_BATTLE: 0x16,
  RESULTS: 0x18,
  TRAINING: 0x36,
  REMIX_MODES: 0x77,
};

const CSS_SCREENS = new Set([
  SCREENS.VS_CSS,
  SCREENS.CSS_1P,
  SCREENS.TRAINING_CSS,
  SCREENS.BONUS_1_CSS,
  SCREENS.BONUS_2_CSS,
  SCREENS.STAGE_SELECT,
]);

const BATTLE_SCREENS = new Set([
  SCREENS.VS_BATTLE,
  SCREENS.RESULTS,
  SCREENS.TRAINING,
  SCREENS.REMIX_MODES,
]);

const CURRENT_SCREEN_ADDR = 0x800a4ad0; // Global.current_screen
const STAGE_ID_ADDR = 0x800a4d09; // Global.vs.stage (byte)

// Character ID -> TSH codename (from config.json character_to_codename)
const CHARACTER_CODENAMES = {
  0: "mario",
  1: "fox",
  2: "donkey_kong",
  3: "samus",
  4: "luigi",
  5: "link",
  6: "yoshi",
  7: "captain_falcon",
  8: "kirby",
  9: "pikachu",
  10: "jigglypuff",
  11: "ness",
  // 12: boss — no codename
  13: "metal_mario",
  // 14–25: polygon/NPC characters — no codename
  26: "giant_donkey_kong",
  27: "random",
  // 28: none — no codename
  29: "falco",
  30: "ganondorf",
  31: "young_link",
  32: "dr_mario",
  33: "wario",
  34: "dark_samus",
  35: "link",
  36: "samus",
  37: "ness",
  38: "lucas",
  39: "link",
  40: "captain_falcon",
  41: "fox",
  42: "mario",
  43: "luigi",
  44: "donkey_kong",
  45: "pikachu",
  46: "jigglypuff",
  47: "jigglypuff",
  48: "kirby",
  49: "yoshi",
  50: "pikachu",
  51: "samus",
  52: "bowser",
  53: "giga_bowser",
  54: "mad_piano",
  55: "wolf",
  56: "conker",
  57: "mewtwo",
  58: "marth",
  59: "sonic",
  // 60: sandbag — no codename
  61: "super_sonic",
  62: "sheik",
  63: "marina",
  64: "dedede",
  65: "goemon",
  66: "peppy",
  67: "slippy",
  68: "banjo",
  69: "metal_luigi",
  70: "ebisumaru",
  71: "dragon_king",
  72: "crash",
  73: "peach",
  74: "roy",
  75: "dr_luigi",
  76: "lanky",
};

// Stage ID -> TSH codename. Derived from add_stage display names in Stages.asm
// cross-referenced against config.json stage_to_codename. BTT/BTP stages omitted.
const STAGE_NAMES = {
  0x00: "peachs_castle",
  0x01: "sector_z",
  0x02: "congo_jungle",
  0x03: "planet_zebes",
  0x04: "hyrule_castle",
  0x05: "yoshis_island",
  0x06: "dream_land",
  0x07: "saffron_city",
  0x08: "mushroom_kingdom",
  0x09: "dream_land_beta_1",
  0x0a: "dream_land_beta_2",
  0x0b: "how_to_play",
  0x0c: "mini_yoshis_island",
  0x0d: "meta_crystal",
  0x0e: "duel_zone",
  // 0x0F Race to the Finish — no TSH codename
  0x10: "final_destination",
  // 0x11–0x28: BTT/BTP stages — omitted
  0x29: "deku_tree",
  0x2a: "first_destination",
  0x2b: "ganons_tower",
  0x2c: "gym_leader_castle",
  0x2d: "pokemon_stadium",
  0x2e: "tal_tal_heights",
  0x2f: "glacial_river",
  0x30: "warioware_inc",
  0x31: "battlefield",
  0x32: "flat_zone",
  0x33: "dr_mario",
  0x34: "cool_cool_mountain",
  0x35: "dragon_king",
  0x36: "great_bay",
  0x37: "frays_stage",
  0x38: "tower_of_heaven",
  0x39: "fountain_of_dreams",
  0x3a: "muda_kingdom",
  0x3b: "mementos",
  0x3c: "showdown",
  0x3d: "spiral_mountain",
  0x3e: "n64",
  0x3f: "mute_city_dl",
  0x40: "mad_monster_mansion",
  0x41: "mushroom_kingdom_dl", // Mushroom Kingdom DL
  0x42: "mushroom_kingdom_omega", // Mushroom Kingdom ~
  0x43: "bowsers_stadium",
  0x44: "peachs_castle_ii",
  0x45: "delfino_plaza",
  0x46: "corneria",
  0x47: "kitchen_island",
  0x48: "big_blue",
  0x49: "onett",
  0x4a: "crateria", // Crateria (ZLANDING)
  0x4b: "frosty_village",
  0x4c: "smashville",
  // 0x4D–0x4F: BTT — omitted
  0x50: "battlefield_dl",
  // 0x51–0x54: BTT — omitted
  0x55: "hyrule_temple",
  // 0x56: BTT — omitted
  // 0x57: BTP — omitted
  0x58: "new_pork_city",
  // 0x59: BTP — omitted
  0x5a: "smashketball",
  // 0x5B: BTP — omitted
  0x5c: "norfair",
  0x5d: "raid_blue",
  0x5e: "congo_falls",
  0x5f: "pigmask_fortress", // Pigmask Fortress (OSOHE)
  0x60: "yoshis_story",
  0x61: "world_1-1",
  0x62: "flat_zone_ii",
  0x63: "gerudo_valley",
  // 0x64–0x66: BTP — omitted
  0x67: "hyrule_castle_dl",
  0x68: "hyrule_castle_omega",
  0x69: "congo_jungle_dl",
  0x6a: "congo_jungle_omega",
  0x6b: "peachs_castle_dl",
  0x6c: "peachs_castle_omega",
  // 0x6D: BTP — omitted
  0x6e: "frays_stage_night",
  0x6f: "goomba_road",
  // 0x70: BTP — omitted
  0x71: "sector_z_dl",
  0x72: "saffron_city_dl",
  0x73: "yoshis_island_dl",
  0x74: "planet_zebes_dl",
  0x75: "sector_z_omega",
  0x76: "saffron_city_omega",
  0x77: "yoshis_island_omega",
  0x78: "dream_land_omega",
  0x79: "planet_zebes_omega",
  // 0x7A–0x7B: BTT/BTP — omitted
  0x7c: "bowsers_keep",
  0x7d: "rith_essa",
  0x7e: "venom",
  // 0x7F–0x82: BTT/BTP — omitted
  0x83: "windy",
  0x84: "datadyne_central",
  0x85: "planet_clancer",
  0x86: "jungle_japes",
  // 0x87: BTT — omitted
  0x88: "game_boy_land",
  // 0x89–0x8C: BTT/BTP — omitted
  // 0x8B Allstar Rest Area — no TSH codename
  0x8d: "castle_siege",
  0x8e: "yoshis_island_ii",
  0x8f: "final_destination_dl",
  0x90: "final_tentination",
  0x91: "cool_cool_mountain_sr",
  0x92: "duel_zone_dl",
  0x93: "cool_cool_mountain_dl",
  0x94: "meta_crystal_dl",
  0x95: "dream_greens", // Dream Greens (DREAM_LAND_SR)
  0x96: "peachs_castle_beta",
  0x97: "hyrule_castle_sr",
  0x98: "sector_z_sr",
  0x99: "mute_city",
  // 0x9A Home Run Contest — no TSH codename
  0x9b: "mushroom_kingdom_sr",
  0x9c: "green_hill_zone",
  0x9d: "subcon",
  0x9e: "pirate_land",
  0x9f: "casino_night_zone",
  // 0xA0–0xA1: BTT/BTP — omitted
  0xa2: "metallic_madness",
  0xa3: "rainbow_road",
  0xa4: "pokemon_stadium_2",
  0xa5: "norfair_remix",
  0xa6: "toads_turnpike",
  0xa7: "tal_tal_heights_remix",
  // 0xA8: BTP — omitted
  0xa9: "dream_land_dl", // Winter Dream Land
  // 0xAA: BTT — omitted
  0xab: "glacial_river_remix",
  // 0xAC: BTT — omitted
  0xad: "dragon_king_remix",
  // 0xAE–0xAF: BTP/BTT — omitted
  0xb0: "draculas_castle",
  0xb1: "reverse_castle",
  // 0xB2: BTP — omitted
  0xb3: "mt_dedede",
  0xb4: "edo_town",
  0xb5: "deku_tree_dl",
  0xb6: "crateria_dl", // Crateria DL (ZLANDING_DL)
  // 0xB7: BTT — omitted
  0xb8: "first_destination_remix",
  // 0xB9: BTP — omitted
  0xba: "twilight_city",
  0xbb: "melrode",
  0xbc: "meta_crystent",
  // 0xBD Remix 1p Race to the Finish — no TSH codename
  0xbe: "grim_reapers_cavern",
  0xbf: "scuttle_town",
  0xc0: "big_boos_haunt",
  0xc1: "dinosaur_land", // Dinosaur Land (YOSHIS_ISLAND_MELEE)
  // 0xC2: BTT — omitted
  0xc3: "spawned_fear",
  0xc4: "smashville_remix",
  // 0xC5: BTP — omitted
  0xc6: "poke_floats",
  0xc7: "big_snowman",
  0xc8: "dream_land_beta_dl",
  0xc9: "lmao_castle",
  0xca: "discovery_falls",
  // 0xCB: BTT — omitted
  0xcc: "discovery_falls_remix",
  0xcd: "n64_remix",
  // 0xCE–0xD0: BTP/BTT — omitted
  // 0xD1 Soccer — no TSH codename
  0xd2: "time_twister",
  0xd3: "time_twister",
  0xd4: "n_sanity_beach",
  0xd5: "snow_go",
  0xd6: "future_frenzy",
  0xd7: "autumn_how_to_play",
};

// Player struct node layout (Global.p_struct_head linked list, exists during battle)
const P_STRUCT_HEAD = 0x80130d84;
const OFF_NEXT = 0x00;
const OFF_OBJ = 0x04;
const OFF_SKIN = 0x10;
const OFF_ID = 0x08;
const OFF_DMG = 0x2c;

// CSS player struct layout (CharacterSelect.asm)
const CSS_OFF_CHAR_ID = 0x48; // word — selected character ID
const CSS_OFF_COSTUME = 0x4c; // word — selected costume index
const CSS_OFF_TYPE = 0x84; // word — panel state: 0=human, 1=CPU, 2=closed

// Per-screen CSS struct base addresses and strides (CharacterSelect.asm constants)
const CSS_STRUCTS = {
  [SCREENS.VS_CSS]: { base: 0x8013ba88, stride: 0xbc },
  [SCREENS.CSS_1P]: { base: 0x80138ec0, stride: 0xb8 },
  [SCREENS.TRAINING_CSS]: { base: 0x80138558, stride: 0xb8 },
  [SCREENS.BONUS_1_CSS]: { base: 0x80137620, stride: 0xb8 },
  [SCREENS.BONUS_2_CSS]: { base: 0x80137620, stride: 0xb8 },
};

let frameCount = 0;
let prevInBattle = false;

function getCharacterCodename(charId) {
  return CHARACTER_CODENAMES[charId] || null;
}

function getStageName(stageId) {
  return STAGE_NAMES[stageId] || null;
}

function sendToTSH(team, player, codename, skin) {
  const endpoint =
    CONFIG.SERVER_URL +
    "/scoreboard" +
    CONFIG.SCOREBOARD +
    "-update-team-" +
    team +
    "-" +
    player;

  const payload = {
    mains: {
      ssb64: [[codename, String(skin)]],
    },
  };

  const options = {
    method: "POST",
    body: JSON.stringify(payload),
    headers: { "Content-Type": "application/json" },
  };

  fetchAsync(endpoint, options);
}

function sendStageToTSH(codename) {
  const endpoint =
    CONFIG.SERVER_URL +
    "/scoreboard" +
    CONFIG.SCOREBOARD +
    "-set-current-stage";

  const options = {
    method: "POST",
    body: JSON.stringify({ codename: codename }),
    headers: { "Content-Type": "application/json" },
  };

  fetchAsync(endpoint, options);
}

print("[TSH] Script loaded");

// Winner detection via PC hook (if supported) or memory polling fallback
// v0 contains winner ID (0-3) at: 0x80131EB0 + 0x10C

const WINNER_PC_BASE = 0x80131eb0;
const WINNER_PC_OFFSET = 0x10c;

function reportWinner(winner) {
  print("[TSH] Winner: player " + (winner + 1));
  const port = winner + 1;
  const mapping = CONFIG.PORT_MAPPING[port];
  if (mapping) {
    const scoreupUrl =
      CONFIG.SERVER_URL +
      "/scoreboard" +
      CONFIG.SCOREBOARD +
      "-team" +
      mapping.team +
      "-scoreup";
    fetchAsync(scoreupUrl);
  }
}

let useOnPC = true;
try {
  emu.on_pc(WINNER_PC_BASE + WINNER_PC_OFFSET, () => {
    const screen = memory.read8(CURRENT_SCREEN_ADDR);
    if (!BATTLE_SCREENS.has(screen)) return;
    const winner = emu.getReg("v0") | 0;
    if (winner < 0 || winner > 3) return;
    reportWinner(winner);
  });
  print("[TSH] Using per-instruction PC hook");
} catch (e) {
  print("[TSH] PC hook not supported: " + e.toString());
  useOnPC = false;
}

if (!useOnPC) {
  const WINNER_FLAG_BASE = 0x80139bd0;
  const WINNER_CLEAR_BASE = 0x80139bb0;

  let winnerReported = false;
  let prevScreen = 0;

  function getWinner() {
    for (let p = 0; p < 4; p++) {
      if (
        memory.read32(WINNER_FLAG_BASE + p * 4) === 1 &&
        memory.read32(WINNER_CLEAR_BASE + p * 4) === 0
      ) {
        return p;
      }
    }
    return -1;
  }

  emu.on_frame(() => {
    const screen = memory.read8(CURRENT_SCREEN_ADDR);
    if (screen !== SCREENS.RESULTS) {
      if (prevScreen === SCREENS.RESULTS) winnerReported = false;
      prevScreen = screen;
      return;
    }
    prevScreen = screen;

    if (winnerReported) return;

    const w = getWinner();
    if (w < 0) return;

    winnerReported = true;
    reportWinner(w);
  });
  print("[TSH] Using memory polling fallback");
}

// Character/skin + stage updates
emu.on_frame(() => {
  frameCount++;

  const screen = memory.read8(CURRENT_SCREEN_ADDR);
  const inBattle = BATTLE_SCREENS.has(screen);

  if (prevInBattle && !inBattle) sendStageToTSH("");
  prevInBattle = inBattle;

  if (frameCount % CONFIG.UPDATE_INTERVAL !== 0) return;

  if (inBattle) {
    // Send stage every interval (not just on change)
    const stageId = memory.read8(STAGE_ID_ADDR);
    const stageName = getStageName(stageId);
    if (stageName) sendStageToTSH(stageName);
    // Read characters from in-battle player struct linked list
    let nodeAddr = memory.read32(P_STRUCT_HEAD);
    let port = 1;

    while (nodeAddr !== 0) {
      const nextAddr = memory.read32(nodeAddr + OFF_NEXT);
      const objAddr = memory.read32(nodeAddr + OFF_OBJ);

      if (objAddr !== 0) {
        const charId = memory.read32(nodeAddr + OFF_ID);
        const skin = memory.read8(nodeAddr + OFF_SKIN);
        const codename = getCharacterCodename(charId);

        const mapping = CONFIG.PORT_MAPPING[port];
        if (mapping && codename) {
          sendToTSH(mapping.team, mapping.player, codename, skin);
        }
      }

      port++;
      nodeAddr = nextAddr;
    }
  } else if (CSS_SCREENS.has(screen)) {
    const css = CSS_STRUCTS[screen];
    if (css) {
      for (let port = 1; port <= 4; port++) {
        const base = css.base + (port - 1) * css.stride;
        const type = memory.read32(base + CSS_OFF_TYPE);
        if (type === 2) continue; // closed slot

        const charId = memory.read32(base + CSS_OFF_CHAR_ID);
        const costume = memory.read32(base + CSS_OFF_COSTUME) & 0xff;
        const codename = getCharacterCodename(charId);
        const mapping = CONFIG.PORT_MAPPING[port];
        if (mapping && codename) {
          sendToTSH(mapping.team, mapping.player, codename, costume);
        }
      }
    }
  }
});
