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

// Register a function to be called every emulated frame.
emu.on_frame(callback);

// Register a function to be called when the PC reaches a specific address.
emu.on_breakpoint(addr, callback);
```

**Example — print a memory value every frame:**

```js
emu.on_frame(() => {
  const hp = memory.read8(0x8033b21d);
  print("HP:", hp);
});
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

## `fetch` — HTTP requests

Follows the browser `fetch` API (blocking, runs synchronously).

```js
const res = await fetch("https://example.com/api");
const data = res.json();

// With options
const res2 = await fetch("https://example.com/api", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ key: "value" }),
});
```

Response object fields: `status`, `ok`, `body` (string), `headers` (object), `json()` method.

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

---

## Notes

- Scripts are evaluated once when loaded. Use `emu.on_frame` for per-frame logic.
- All JS runs on the emulation thread during frame callbacks — keep callbacks fast.
- `fetch` makes real network requests; use sparingly inside `on_frame`.
- Player name availability by mode:
  - **Rollback P2P**: both local and remote player names are set at session start.
  - **Kaillera server**: only the local player's name is populated; other slots fall back to `"Player N"`.
