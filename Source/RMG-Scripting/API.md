# RMG-K JavaScript Scripting API

Scripts are plain JavaScript files loaded from the Scripts panel in the emulator UI. Each script runs in a QuickJS context with access to the globals described below.

---

## Console output

```js
print("hello")          // prints to the script output panel
console.log(...)        // same
console.warn(...)       // same
console.error(...)      // same
```

---

## `memory` — N64 memory access

All addresses are N64 virtual addresses (e.g. `0x80000000` for RDRAM start).

```js
// Read
memory.read8(addr); // → number (unsigned byte)
memory.read8s(addr); // → number (signed byte)
memory.read16(addr); // → number (unsigned 16-bit)
memory.read16s(addr); // → number (signed 16-bit)
memory.read32(addr); // → number (unsigned 32-bit)
memory.read32s(addr); // → number (signed 32-bit)
memory.readFloat(addr); // → number (32-bit float)

// Write
memory.write8(addr, value);
memory.write16(addr, value);
memory.write32(addr, value);
memory.writeFloat(addr, value);
```

---

## `emu` — emulator state and callbacks

```js
emu.isRunning(); // → boolean
emu.isPaused(); // → boolean
emu.getPC(); // → number — current R4300 program counter

// General-purpose registers (lower 32 bits, unsigned).
// Accepts a register index (0–31) or MIPS ABI name.
// Names: zero at v0 v1 a0 a1 a2 a3 t0-t7 s0-s7 t8 t9 k0 k1 gp sp fp ra
emu.getReg(index_or_name); // → number
emu.getRegs(); // → object — all 32 GPRs keyed by ABI name

// Multiply result registers.
emu.getHI(); // → number
emu.getLO(); // → number

// Floating-point registers (COP1), returned as float values.
emu.getFReg(index); // → number — f0–f31 (index 0–31)
emu.getFRegs(); // → object — { f0: ..., f1: ..., ..., f31: ... }

// Register a function to be called every emulated frame.
emu.on_frame(callback);

// Register a function to be called when the PC equals addr at the frame boundary.
// Checked once per frame via getPC() — fires if the CPU happens to be at that
// address when the VI interrupt fires.
emu.on_pc(addr, callback);

// Register a function to be called when the user submits text via the input bar
// at the bottom of the Scripting Console. The callback receives the text as a string.
// Multiple scripts can each register their own on_input handler.
emu.on_input(callback);

// Block script execution, print message to the output panel, and wait for the
// user to submit text via the input bar. Returns the submitted string, or ""
// if the bar was submitted empty. Safe to call from any context (frame
// callbacks, on_input handlers, or top-level script code).
emu.input(message); // → string
```

**Example — print a memory value every frame:**

```js
emu.on_frame(() => {
  const hp = memory.read8(0x8033b21d);
  print("HP:", hp);
});
```

**Example — interactive memory read/write from the console input bar:**

```js
emu.on_input((text) => {
  const parts = text.trim().split(/\s+/);
  if (parts[0] === "read") {
    const addr = parseInt(parts[1], 16);
    print("0x" + memory.read32(addr).toString(16).padStart(8, "0"));
  } else if (parts[0] === "write") {
    memory.write32(parseInt(parts[1], 16), parseInt(parts[2], 16));
    print("done");
  }
});
```

**Example — blocking input prompt:**

```js
const a = parseInt(emu.input("First number:"),  10);
const b = parseInt(emu.input("Second number:"), 10);
print(a + " + " + b + " = " + (a + b));
```

---

## `input` — current virtual controller state

```js
input.getPortState(port); // → object — controller state for port 1..4 (or 0..3)
input.getPortStates(); // → array[4] — state for ports 1 through 4
input.getPressedButtons(port); // → array — names of all buttons currently pressed on that port
input.decodeButtons(buttonMask); // → array — decode a 16-bit N64 button mask into button names
```

The `input` object also exposes named button constants:

- `input.BUTTON_A`
- `input.BUTTON_B`
- `input.BUTTON_Z`
- `input.BUTTON_START`
- `input.BUTTON_DPAD_UP`
- `input.BUTTON_DPAD_DOWN`
- `input.BUTTON_DPAD_LEFT`
- `input.BUTTON_DPAD_RIGHT`
- `input.BUTTON_C_UP`
- `input.BUTTON_C_DOWN`
- `input.BUTTON_C_LEFT`
- `input.BUTTON_C_RIGHT`
- `input.BUTTON_L`
- `input.BUTTON_R`

A returned state object contains:

- `port` → number (1–4)
- `connected` → boolean — whether the port is present in the latest PIF controller read
- `valid` → boolean — whether a controller read response is available for that port
- `raw` → number — raw 32-bit packed controller response
- `buttons` → number — combined 16-bit button word
- `buttonsPressed` → array — decoded pressed button names
- `x` → number — signed X axis (-128..127)
- `y` → number — signed Y axis (-128..127)

**Example — log all four virtual ports every frame:**

```js
emu.on_frame(() => {
  const states = input.getPortStates();
  states.forEach((state) => {
    print(
      `P${state.port}: valid=${state.valid} connected=${state.connected} buttons=0x${state.buttons.toString(16)} x=${state.x} y=${state.y}`,
    );
  });
});
```

**Example — print the raw game-facing controller response for a single port:**

```js
emu.on_frame(() => {
  const state = input.getPortState(1);
  if (!state.valid) {
    print(`Port ${state.port} not ready`);
    return;
  }

  print(
    `Port ${state.port}: raw=0x${state.raw.toString(16).padStart(8, "0")} ` +
      `buttons=0x${state.buttons.toString(16).padStart(4, "0")} ` +
      `pressed=[${state.buttonsPressed.join(", ")}] x=${state.x} y=${state.y}`,
  );
});
```

**Example — use named button constants and decode helper:**

```js
emu.on_frame(() => {
  const state = input.getPortState(1);
  if (!state.valid) return;

  const pressed = state.buttonsPressed;
  const aPressed = pressed.includes("A");
  const upPressed = pressed.includes("DpadUp");

  if (aPressed && upPressed) {
    print("Port 1 is pressing A + DpadUp");
  }

  // Decode an arbitrary raw button mask
  const names = input.decodeButtons(state.buttons);
  print(`Pressed: ${names.join(", ")}`);
});
```

---

## `netplay` — current netplay session info

All functions return safe defaults (0, false, "none") when no session is active.

```js
netplay.isActive(); // → boolean — true if any netplay session is running

netplay.getMode(); // → "kaillera" | "rollback" | "none"
//   "kaillera" = Kaillera server game
//   "rollback" = P2P GekkoNet rollback session

netplay.getPlayerNumber(); // → number — local player slot (1–4, or 0 if inactive)
netplay.getNumPlayers(); // → number — total players in session

netplay.getFrameDelay(); // → number — input delay frames
//   Kaillera: server-assigned delay
//   Rollback: locally configured delay

netplay.getPredictionWindow(); // → number — rollback prediction window in frames
//   (rollback P2P only; 0 for Kaillera)

// Rollback-specific
netplay.isRollingBack(); // → boolean — currently executing a rollback frame
netplay.getFramesAhead(); // → number — GekkoNet speed-pacing factor
//   positive = running ahead, negative = catching up

// Network stats (rollback P2P only; return 0 for Kaillera server)
netplay.getPing(); // → integer — transport-layer RTT in ms; matches the
//   value shown in the netplay window
netplay.getAvgPing(); // → float — GekkoNet rolling average RTT in ms
netplay.getJitter(); // → float — GekkoNet jitter in ms

// Player names
// For Kaillera server sessions only the local player's name is known at
// session start. For rollback P2P both local and remote names are available.
// Falls back to "Player N" if a name has not been set.
netplay.getPlayerName(n); // → string — display name of player slot n (1–4)
```

**Example — show ping overlay every frame:**

```js
emu.on_frame(() => {
  if (!netplay.isActive()) return;
  const mode = netplay.getMode();
  if (mode === "rollback") {
    print(
      `P${netplay.getPlayerNumber()} | ping ${netplay.getPing()}ms | rollback: ${netplay.isRollingBack()}`,
    );
  } else {
    print(
      `P${netplay.getPlayerNumber()}/${netplay.getNumPlayers()} delay ${netplay.getFrameDelay()}`,
    );
  }
});
```

---

## `fetch` / `fetchAsync` — HTTP requests

```js
// Blocking — waits for the response before continuing.
// Use for scripts that need the result (e.g. reading data from a server).
const res = fetch("https://example.com/api");
const res2 = fetch("https://example.com/api", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ key: "value" }),
});
if (res2.ok) print(res2.body);

// Non-blocking — fires the request on a background thread and returns immediately.
// Use inside on_frame or anywhere you don't need the response.
fetchAsync("https://example.com/api");
fetchAsync("https://example.com/api", { method: "POST", body: "data" });
```

`fetch()` response object fields: `status`, `ok`, `body` (string), `headers` (string), `json()` method.

`fetchAsync()` returns `undefined`. On network failure, a `[fetchAsync error]` line is printed to the script's output panel.

---

## Standard modules

QuickJS `std` and `os` modules are available as globals:

```js
std.out.puts("hello\n");
const f = std.open("file.txt", "r");

os.sleep(1000); // milliseconds
```

---

## Examples

See `Data/Scripts/` for ready-to-load example scripts:

| File                          | What it shows                                                 |
| ----------------------------- | ------------------------------------------------------------- |
| `example_netplay.js`          | Session info, player names, per-frame ping / rollback overlay |
| `example_frame_logger.js`     | Per-frame hook, memory reads, file output                     |
| `example_breakpoint_watch.js` | PC breakpoint and register inspection                         |
| `example_input_state.js`      | Read the current virtual controller state for all four ports  |
| `example_console_input.js`    | Interactive `on_input` handler — read/write memory, dump regs |
| `example_input_prompt.js`     | `emu.input()` — asks for two numbers and prints their sum     |

---

## Notes

- Scripts are evaluated once when loaded. Use `emu.on_frame` for per-frame logic.
- All JS runs on the emulation thread during frame callbacks — keep callbacks fast.
- `emu.on_input` callbacks fire when the user presses Enter in the input bar; they are safe to call any time (emulation running or stopped).
- `emu.input(msg)` prints the message then waits for the next input bar submission. Subsequent submissions are delivered to `on_input` handlers as usual once the wait completes.
- Input from the bar is broadcast to every running script that has registered an `on_input` handler, unless a script is currently blocking in `emu.input()`.
- `fetch` makes real network requests; use sparingly inside `on_frame`.
- Player name availability by mode:
  - **Rollback P2P**: both local and remote player names are set at session start.
  - **Kaillera server**: only the local player's name is populated; other slots fall back to `"Player N"`.
