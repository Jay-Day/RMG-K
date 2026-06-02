// Smash 64 — Tournament Stream Helper (TSH) Integration
//
// Sends player state (character, skin) to TSH's web server for live scoreboard updates.
// Communicates with http://localhost:5000/update-team-<team>-<player>
// Assumes scoreboard 1.
//
// Configuration:
const CONFIG = {
  SERVER_URL: "http://localhost:5000",
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

// Character ID to name mapping (Smash 64 roster)
const CHARACTER_NAMES = {
  0: "Mario",
  1: "Fox",
  2: "Donkey Kong",
  3: "Samus",
  4: "Luigi",
  5: "Link",
  6: "Yoshi",
  7: "Captain Falcon",
  8: "Kirby",
  9: "Pikachu",
  10: "Jigglypuff",
  11: "Ness",
  12: "boss",
  13: "Metal Mario",
  14: "nmario",
  15: "nfox",
  16: "ndonkey",
  17: "nsamus",
  18: "nluigi",
  19: "nlink",
  20: "nyoshi",
  21: "ncaptain",
  22: "nkirby",
  23: "npikachu",
  24: "npuff",
  25: "nness",
  26: "Giant Donkey Kong",
  27: "Random",
  28: "none",
  29: "Falco",
  30: "Ganondorf",
  31: "Young Link",
  32: "Dr. Mario",
  33: "Wario",
  34: "Dark Samus",
  35: "Link",
  36: "Samus",
  37: "Ness",
  38: "Lucas",
  39: "Link",
  40: "Captain Falcon",
  41: "Fox",
  42: "Mario",
  43: "Luigi",
  44: "Donkey Kong",
  45: "Pikachu",
  46: "Jigglypuff",
  47: "Jigglypuff",
  48: "Kirby",
  49: "Yoshi",
  50: "Pikachu",
  51: "Samus",
  52: "Bowser",
  53: "Giga Bowser",
  54: "Mad Piano",
  55: "Wolf",
  56: "Conker",
  57: "Mewtwo",
  58: "Marth",
  59: "Sonic",
  60: "Sandbag",
  61: "Super Sonic",
  62: "Sheik",
  63: "Marina",
  64: "King Dedede",
  65: "Goemon",
  66: "Peppy",
  67: "Slippy",
  68: "Banjo & Kazooie",
  69: "Metal Luigi",
  70: "Ebisumaru",
  71: "Dragon King",
  72: "Crash",
  73: "Peach",
  74: "Roy",
  75: "Dr. Luigi",
  76: "Lanky Kong",
};

// Player struct node layout (same as smash_test.js)
const P_STRUCT_HEAD = 0x80130d84;
const OFF_NEXT = 0x00;
const OFF_OBJ = 0x04;
const OFF_SKIN = 0x10;
const OFF_ID = 0x08;
const OFF_DMG = 0x2c;

let frameCount = 0;

function getCharacterName(charId) {
  return CHARACTER_NAMES[charId] || "Unknown";
}

function sendToTSH(team, player, characterName, skin) {
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
      ssb64: [[characterName, String(skin)]],
    },
  };

  const options = {
    method: "POST",
    body: JSON.stringify(payload),
    headers: {
      "Content-Type": "application/json",
    },
  };

  const response = fetch(endpoint, options);
  if (response.ok) {
    console.log(
      "TSH Update sent: Team " +
        team +
        ", Player " +
        player +
        " -> " +
        characterName +
        " (skin " +
        skin +
        ")",
    );
  } else {
    console.log(
      "TSH Update failed (HTTP " +
        response.status +
        "): Team " +
        team +
        ", Player " +
        player,
    );
  }
}

// Winner detection — replicates function 0x801333E4 → 0x8013234C logic.
// Disassembly uses: lui $at, 0x8014; lw/sw with signed 16-bit offsets.
// 0x9bd0 (signed) = -0x6430, so address = 0x80140000 - 0x6430 = 0x80139bd0
// 0x9bb0 (signed) = -0x6450, so address = 0x80140000 - 0x6450 = 0x80139bb0
// Winner = first player p where flag[p]==1 AND clear[p]==0
const WINNER_FLAG_BASE  = 0x80139bd0;
const WINNER_CLEAR_BASE = 0x80139bb0;

function getWinner() {
  for (let p = 0; p < 4; p++) {
    if (memory.read32(WINNER_FLAG_BASE  + p * 4) === 1 &&
        memory.read32(WINNER_CLEAR_BASE + p * 4) === 0) {
      return p;
    }
  }
  return -1;
}

// Winner detection via memory polling
let winnerReported = false;
let prevScreen = 0;
emu.on_frame(() => {
  const screen = memory.read8(0x800a4ad0);
  if (screen !== 0x18) {
    if (prevScreen === 0x18) winnerReported = false;
    prevScreen = screen;
    return;
  }
  prevScreen = screen;
  if (winnerReported) return;
  const w = getWinner();
  if (w < 0) return;
  winnerReported = true;
  print("[TSH] Winner: player " + (w + 1));
});

// Character/skin updates
emu.on_frame(() => {
  frameCount++;
  if (frameCount % CONFIG.UPDATE_INTERVAL !== 0) return;

  let nodeAddr = memory.read32(P_STRUCT_HEAD);
  let port = 1;

  while (nodeAddr !== 0) {
    const nextAddr = memory.read32(nodeAddr + OFF_NEXT);
    const objAddr = memory.read32(nodeAddr + OFF_OBJ);

    if (objAddr !== 0) {
      const charId = memory.read32(nodeAddr + OFF_ID);
      const skin = memory.read8(nodeAddr + OFF_SKIN);
      const characterName = getCharacterName(charId);

      // Get team/player mapping for this port
      const mapping = CONFIG.PORT_MAPPING[port];
      if (mapping) {
        sendToTSH(mapping.team, mapping.player, characterName, skin);
      }
    }

    port++;
    nodeAddr = nextAddr;
  }
});
