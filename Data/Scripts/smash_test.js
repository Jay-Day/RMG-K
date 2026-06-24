// Smash 64 — Player State Logger
//
// Player struct node layout:
//   +0x00  u32  next node pointer  (0 = end of list)
//   +0x04  u32  player object ptr  (0 = slot empty, skip)
//   +0x06  u8   skin / costume index
//   +0x08  u32  character ID
//   +0x2C  u32  damage %

const P_STRUCT_HEAD = 0x80130d84;
const OFF_NEXT = 0x00;
const OFF_OBJ = 0x04;
const OFF_SKIN = 0x10;
const OFF_ID = 0x08;
const OFF_DMG = 0x2c;

const OUTPUT_FILE = "smash_state.json";

let frameCount = 0;

emu.on_frame(() => {
  frameCount++;
  if (frameCount % 60 !== 0) return;

  let nodeAddr = memory.read32(P_STRUCT_HEAD);
  const players = [];
  let port = 1;

  while (nodeAddr !== 0) {
    const nextAddr = memory.read32(nodeAddr + OFF_NEXT);
    const objAddr = memory.read32(nodeAddr + OFF_OBJ);

    if (objAddr !== 0) {
      const charId = memory.read32(nodeAddr + OFF_ID);
      const skin = memory.read8(nodeAddr + OFF_SKIN);
      const damage = memory.read32(nodeAddr + OFF_DMG);
      players.push({ port, charId, skin, damage });
    }

    port++;
    nodeAddr = nextAddr;
  }

  // Print to console
  const parts = players.map(
    (p) =>
      "P" +
      p.port +
      " char=" +
      p.charId +
      " skin=" +
      p.skin +
      " dmg=" +
      p.damage +
      "%",
  );
  console.log(
    "frame=" +
      frameCount +
      "  " +
      (parts.length ? parts.join("  ") : "(no players)"),
  );

  // Export JSON (overwrites each update)
  const f = std.open(OUTPUT_FILE, "w");
  if (f) {
    f.puts(JSON.stringify({ players }, null, 2));
    f.close();
  }
});
