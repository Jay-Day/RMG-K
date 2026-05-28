// Example: PC watch callback + file write
// Replace WATCH_PC with an address from your game.

const WATCH_PC = 0x80001000;

console.log(`watching pc 0x${WATCH_PC.toString(16).padStart(8, "0")}`);

emu.on_breakpoint(WATCH_PC, (pc) => {
  console.log(`hit pc=0x${pc.toString(16).padStart(8, "0")}`);
});
