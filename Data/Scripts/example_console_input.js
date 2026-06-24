// Example: interactive console input
// Demonstrates both ways to receive input in a script:
//
//   emu.on_input(fn)  — event-driven: fn is called each time the user submits
//                        text via the input bar at the bottom of the console.
//
//   emu.input(msg)    — blocking: prints msg to the output, then pauses the
//                        script until the user types something in the input bar.
//                        Returns the submitted string (or "" if empty).
//
// Commands (type in the input bar and press Enter):
//   read <addr>        — read a 32-bit value from a hex address (e.g. read 80000000)
//   write <addr> <val> — write a 32-bit value to a hex address (both hex)
//   writeprompt        — step-by-step write using emu.input()
//   pc                 — print the current program counter
//   regs               — dump all GPRs
//   help               — list commands

print("[input] Script loaded. Type 'help' in the input bar below.");

emu.on_input(function(text) {
  const parts = text.trim().split(/\s+/);
  const cmd   = parts[0].toLowerCase();

  if (cmd === "help") {
    print("Commands:");
    print("  read <addr>        - read u32 at hex address");
    print("  write <addr> <val> - write u32 to hex address");
    print("  writeprompt        - interactive write via input bar");
    print("  pc                 - show program counter");
    print("  regs               - dump GPRs");

  } else if (cmd === "read") {
    if (parts.length < 2) { print("Usage: read <addr>"); return; }
    const addr = parseInt(parts[1], 16);
    if (isNaN(addr))      { print("Bad address: " + parts[1]); return; }
    const val = memory.read32(addr);
    print("0x" + addr.toString(16).padStart(8, "0") + " = 0x" + val.toString(16).padStart(8, "0") + " (" + val + ")");

  } else if (cmd === "write") {
    if (parts.length < 3) { print("Usage: write <addr> <val>"); return; }
    const addr = parseInt(parts[1], 16);
    const val  = parseInt(parts[2], 16);
    if (isNaN(addr)) { print("Bad address: " + parts[1]); return; }
    if (isNaN(val))  { print("Bad value: "   + parts[2]); return; }
    memory.write32(addr, val);
    print("Wrote 0x" + val.toString(16).padStart(8, "0") + " -> 0x" + addr.toString(16).padStart(8, "0"));

  } else if (cmd === "writeprompt") {
    // Demonstrates emu.input(): prints a prompt and waits for the next bar submission.
    const addrStr = emu.input("Address to write (hex):");
    if (addrStr === "") { print("Cancelled."); return; }
    const valStr  = emu.input("Value to write (hex):");
    if (valStr  === "") { print("Cancelled."); return; }
    const addr = parseInt(addrStr, 16);
    const val  = parseInt(valStr,  16);
    if (isNaN(addr)) { print("Bad address: " + addrStr); return; }
    if (isNaN(val))  { print("Bad value: "   + valStr);  return; }
    memory.write32(addr, val);
    print("Wrote 0x" + val.toString(16).padStart(8, "0") + " -> 0x" + addr.toString(16).padStart(8, "0"));

  } else if (cmd === "pc") {
    const pc = emu.getPC();
    print("PC = 0x" + pc.toString(16).padStart(8, "0"));

  } else if (cmd === "regs") {
    const r = emu.getRegs();
    for (const name of Object.keys(r)) {
      print(name.padEnd(4) + " = 0x" + r[name].toString(16).padStart(8, "0"));
    }

  } else {
    print("Unknown command: " + cmd + "  (type 'help' for a list)");
  }
});
