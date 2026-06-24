// Example: per-frame hook + RAM reads + file append
// Load via: View -> Scripting Console -> Run Selected

let frameCounter = 0;

emu.on_frame(() => {
  frameCounter++;

  // Example addresses (adjust per game)
  const u8 = memory.read8(0x80000000);
  const u16 = memory.read16(0x80000002);
  const u32 = memory.read32(0x80000004);

  console.log(
    `frame=${frameCounter} pc=0x${emu.getPC().toString(16).padStart(8, "0")} u8=${u8} u16=${u16} u32=${u32}`,
  );
});
