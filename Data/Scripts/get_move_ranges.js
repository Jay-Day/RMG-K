// Super Smash Bros. Melee — move hitbox range recorder
// Ported from Project64 API. Use in training mode, always facing right.
//
// Update these two addresses from logfile.log before running:
var characterActionStringTable = 0x80496020; // Character.action_string.table
var sharedStringTable = 0x80402908; // Action.shared_action_string_table

console.log("get_move_ranges: started");

var ranges = {};

var hitbox_offsets = [0x294, 0x358, 0x41c, 0x4e0];

var lastAction = -1;
var actionStartX = 0;
var actionStartY = 0;
var frameStart = -1;
var frameEnd = -1;
var frameCounter = 1;
var minx = Infinity,
  maxx = -Infinity;
var miny = Infinity,
  maxy = -Infinity;
var relativeminx = Infinity,
  relativemaxx = -Infinity;
var relativeminy = Infinity,
  relativemaxy = -Infinity;

function readString(addr) {
  var s = "";
  for (var i = 0; i < 256; i++) {
    var c = memory.read8(addr + i);
    if (c === 0) break;
    s += String.fromCharCode(c);
  }
  return s;
}

emu.on_frame(function () {
  var pAddr = 0x800466fc;
  var pObject = memory.read32(pAddr);
  var pStruct = memory.read32(pObject + 0x84);

  var action = memory.read32(pStruct + 0x24);
  var playerPos = memory.read32(pStruct + 0x78);
  var playerX = memory.readFloat(playerPos);
  var playerY = memory.readFloat(playerPos + 0x4);

  if (action !== lastAction) {
    // Resolve previous action's name
    var actionName = "0x" + lastAction.toString(16);

    if (lastAction !== -1) {
      if (lastAction < 0x00dc) {
        var ptr = memory.read32(sharedStringTable + (lastAction << 2));
        if (ptr) actionName = readString(ptr);
      } else {
        var charId = memory.read32(pStruct + 0x0008);
        var tablePos = memory.read32(
          characterActionStringTable + (charId << 2),
        );
        var strAddr = tablePos + ((lastAction - 0x00dc) << 2);
        if (strAddr) {
          try {
            var namePtr = memory.read32(strAddr);
            if (namePtr) actionName = readString(namePtr);
          } catch (e) {
            console.log("Error reading action name for " + actionName);
          }
        }
      }
    }

    // Log and record completed action if it had hitboxes
    if (maxx > -Infinity) {
      console.log(
        "== " +
          actionName +
          " (0x" +
          lastAction.toString(16) +
          ") ==" +
          "\nframeStart: " +
          frameStart +
          "\nframeEnd:   " +
          frameEnd +
          "\nabsolute:   " +
          [minx, maxx, miny, maxy].join(", ") +
          "\nrelative:   " +
          [relativeminx, relativemaxx, relativeminy, relativemaxy].join(", ") +
          "\n==========",
      );
      ranges[actionName] = {
        frameStart: frameStart,
        frameEnd: frameEnd,
        absolute: [minx, maxx, miny, maxy],
        relative: [relativeminx, relativemaxx, relativeminy, relativemaxy],
      };

      // Write updated JSON to file
      var f = std.open("ranges.txt", "wb");
      if (f) {
        f.puts(JSON.stringify(ranges, null, 2));
        f.close();
      }
    }

    // Reset accumulators for the new action
    minx = Infinity;
    maxx = -Infinity;
    miny = Infinity;
    maxy = -Infinity;
    relativeminx = Infinity;
    relativemaxx = -Infinity;
    relativeminy = Infinity;
    relativemaxy = -Infinity;
    lastAction = action;
    frameCounter = 1;
    frameStart = -1;
    frameEnd = -1;
    actionStartX = playerX;
    actionStartY = playerY;
  }

  // Accumulate hitbox extents for all 4 hitbox slots
  for (var j = 0; j < 4; j++) {
    var hb = pStruct + hitbox_offsets[j];
    var active = memory.read32(hb);
    if (!active) continue;

    if (frameStart === -1) frameStart = frameCounter;
    frameEnd = frameCounter;

    var x = memory.readFloat(hb + 0x44);
    var y = memory.readFloat(hb + 0x48);
    var size = memory.readFloat(hb + 0x24);
    var r = size / 2.0;

    relativemaxx = Math.max(x + r - playerX, relativemaxx).toFixed(0);
    relativeminx = Math.min(x - r - playerX, relativeminx).toFixed(0);
    relativemaxy = Math.max(y + r - playerY, relativemaxy).toFixed(0);
    relativeminy = Math.min(y - r - playerY, relativeminy).toFixed(0);

    var rx = x - actionStartX;
    var ry = y - actionStartY;
    maxx = Math.max(rx + r, maxx).toFixed(0);
    minx = Math.min(rx - r, minx).toFixed(0);
    maxy = Math.max(ry + r, maxy).toFixed(0);
    miny = Math.min(ry - r, miny).toFixed(0);
  }

  frameCounter += 1;
});
