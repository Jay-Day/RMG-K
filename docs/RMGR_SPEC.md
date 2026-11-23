# RMG-K Replay File Format (`.rmgr`) — Specification

**Status:** format version `6`. This is the first stable version of the format.

Versions before `6` were used only for prototyping and are not covered by this spec.

## 1. Overview

A `.rmgr` file is a self-contained binary recording of one N64 match. It serves two distinct purposes, and most tools that use these files will want both:

1. **Best-effort deterministic replay** — the recorded controller inputs, together with the RNG seed captured in `MatchSettings`, are enough to re-simulate the match's player-driven and RNG-dependent events from scratch. This is deterministic replay-from-inputs, not frame-perfect *live analysis without re-simulating*: the seed lets a replayer reproduce the original RNG stream by driving it forward with the same inputs, but it does not make any already-recorded state field (e.g. an item spawn that already happened) independently derivable without actually re-running that simulation.
2. **Direct analysis** — recorded game state (position, damage, stocks, action state, …) is enough to build stats, visualizations, or search tooling *without* re-running the emulator at all.

A recording is one of two kinds:

1. **Inputs only.** Player names, port slot types and controller inputs (§4). This works for any N64 title and serves the same purpose as a `.krec`, scoped to a single recording session.
2. **Inputs plus game state.** Adds detailed per-frame game state for analysis and replay (§5). This needs a game family the recorder recognizes (§2.1); today that is `smash64`.

Two things distinguish this format from a plain input-recording format like `.krec`:

- **It's split into two layers.** A small **core** event set (player names, port slot types, controller inputs, match end) is always recorded for *any* N64 title, recognized or not — this layer alone makes a `.rmgr` file at least as useful as a `.krec` recording. A second, **game-family-specific extension** event set (position, damage, stocks, items, stage hazards, …) is only recorded when the loaded ROM's game family is recognized by this recorder. A different, unrecognized N64 game still produces a valid, useful, input-only `.rmgr` file; a recognized game gets the full analysis layer on top. See §2.1.
- **The event stream is compressed.** Per-frame data is highly redundant frame-to-frame, so the entire event stream (everything after the fixed header) is deflate-compressed as a single block, written once when the match ends. See §3.4.

The design is modeled on [Slippi's `.slp` format](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md) (Super Smash Bros. Melee / Dolphin) — a self-describing binary event stream — but is **not** byte-compatible with it, and deliberately drops or changes several of Slippi's choices.

**Target platform:** N64 titles specifically (the container format is not attempting to be console-agnostic) — see §2.1 for how it generalizes across *different* N64 titles rather than different consoles.

## 2. Design goals

- **Complete per-frame state, kept compact.** Every frame records the game state itself (position, damage, stocks, action state, …), so a reader can use it without reimplementing game logic. For example, each fighter's current damage is recorded on every frame, rather than recording hits and expecting the reader to add them up and reset when the player loses a stock. Compression keeps files small despite that redundancy: payloads are plain fixed-size layouts, and space is saved by compressing the stream, not by bit-packing or delta-encoding.
- **Robust versioning.** The container format and the game-specific data it records can both change without breaking compatibility. Every event is a 1-byte command code followed by a payload whose size the first event declares (`EventPayloads`), so a parser can skip events and trailing fields it doesn't know (§6). Each game family's event definitions are versioned separately from the container, and the header names the program that wrote the file and its version (§3.2).
- **Game-agnostic, with game families.** Core events are recorded for any N64 title. A game family (§2.1) groups a game's regional variants and mods under one shared set of extension events, so a reader that supports the family reads every build in it.

**Non-goals:**

- **Live streaming.** The recorder buffers the match in memory and writes the file once, when the match ends, so a match can't be read while it is in progress, and a crash or force-quit mid-match leaves no file. Live reading could be added later (§9).

### 2.1 The core / game-family-extension split

Every `.rmgr` file always contains the **core** event set: `MatchStart` (player names, port slot types), `InputFrame` (controller state, one per seated port per frame), and `MatchEnd` (final frame count, end reason). This is deliberately close to a `.krec`-equivalent recording — enough to know who played, on which ports, and what they pressed, for *any* N64 title, whether or not this recorder has ever heard of it.

A file *additionally* contains a **game-family extension** event set when `FileHeader.gameFamily` is non-empty — i.e. when this recorder specifically knows how to read that title's memory layout. Right now exactly one family is defined, `smash64` (§5), covering Smash Remix today; support for vanilla SSB64 and 19XXTE is forthcoming. A future family (e.g. Mario Kart 64) would define its own, entirely separate extension event set under its own `gameFamily` string — it would never carry Smash-only fields like `damagePercent` or `stocksRemaining`, the same way `smash64` files never carry fields a future racing-game family would need.

`gameFamily` is a coarser identity than `goodName` (§3.2): `goodName` pins down the exact ROM build a file came from; `gameFamily` tells a reader which extension event *definitions* apply, without that reader needing to maintain its own list of every `goodName` string that has ever meant "this is Smash." A new Smash Remix build is automatically readable by any tool that already understands `gameFamily == "smash64"`, with zero changes to that tool, the moment its recorder starts stamping that family string.

### 2.2 How `.rmgr` differs from Slippi's `.slp`

`.rmgr` keeps Slippi's self-describing, append-only event stream, but changes its container, write model, versioning and scope. Compared against the [Slippi replay spec](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md).

| Aspect | .slp | .rmgr | Why |
| --- | --- | --- | --- |
| Container | UBJSON object: raw event bytes, then a metadata element (startAt, lastFrame, players, playedOn, consoleNick) | Fixed 136-byte binary header, then the event stream; no metadata block | Identity and timestamp are readable without parsing or decompressing the stream |
| Writing | Streamed during the game; the raw length stays 0 while writing, and the file can be read live | Buffered in memory, written once at match end | No seek-and-patch step, and the whole stream can be compressed. Cost: no live reading, and a crash leaves no file |
| Compression | None | Event stream deflate-compressed as one block | Per-frame data is highly redundant |
| Byte order | Big-endian | Little-endian | Matches the host platforms; structs are written as-is |
| Scope | Melee only | Any N64 title: core events always, game-family extension events when the game is recognized | One format across N64 games |
| Versioning | One major.minor.build version in Game Start covers every field | A container version, a version per game family's extension spec, and the writer's name and version | The container, a family's game data and the writer change independently, so each gets its own field |
| Event codes | 0x35–0x41; Event Payloads begins with its own byte size (3n+1) | 0x01–0x09; Event Payloads begins with an entry count | Arbitrary; no practical difference. A count byte allows up to 255 event types; a 3n+1 size byte caps the list at 84 |
| Per-frame events | Frame Start, Pre-Frame, Post-Frame, Item Update, Frame Bookend | InputFrame (core); StateFrame, ItemUpdate, StageHazardUpdate (smash64); no frame start or end markers | Inputs are game-agnostic; state belongs to the game family |
| RNG seed | In Game Start, every Frame Start and every Pre-Frame | Once, at match start, in MatchSettings | There is currently no reason to store the seed on every frame |
| Inputs | Processed sticks and triggers (floats), processed and physical buttons, physical triggers, raw stick bytes | Processed buttons (u16) and stick X/Y (i8) only | Processed values are the only form available for both human and CPU ports |
| Player identity | Nametag, display name, connect code, Slippi UID | A 32-byte name per port, from netplay room data | Names come from netplay room metadata, which carries no stable player ID |
| Game end | Method (TIME!, GAME!, No Contest), LRAS initiator, placements | MatchEnd: aborted or normal; MatchResult: stocks remaining per port | Results are game-specific, so they live in the extension layer |
| Items | Spawn ID, owner, velocity, damage, expiration timer; up to 15 per frame | GObj address (can be reused), kind, position, scale | The other fields are not yet mapped in memory |
| Stage hazards | One event type per hazard, sent on change (Fountain of Dreams platforms, Whispy direction, Stadium transformations) | One bitmask event, sent every frame a hazard is active | New hazards claim bits rather than new event types |
| ID tables | Linked external spreadsheets | Listed in the spec | Tables are versioned with the spec and readable offline. Cost: a longer document |
| Unchanged | Event Payloads first; fields only appended; unknown events skipped by size | Same | Old and new parsers stay compatible |

## 3. File structure

```
+----------------+----------------------------------------+
| File Header    | 136 bytes, fixed, uncompressed          |
+----------------+----------------------------------------+
| Event Stream   | deflate-compressed block, decompresses  |
| (compressed)   | to the sequence of events described in  |
|                | §4/§5                                    |
+----------------+----------------------------------------+
```

There is no footer, no trailing metadata block, and no UBJSON/JSON wrapper of any kind. The header is followed immediately by exactly `compressedLength` bytes of deflate-compressed data — nothing before it, nothing after it, no framing around the compressed block itself.

### 3.1 File header (136 bytes)

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `char[4]` | `magic` | Always the ASCII bytes `R`, `M`, `G`, `R` (no NUL). |
| 0x04 | 1 | `u8` | `version` | Container format version. `6` for everything described here. |
| 0x05 | 2 | `u16` | `familyVersion` | Version of the `gameFamily` extension spec this file follows, e.g. `1` for the `smash64` events in §5. `0` when `gameFamily` is empty. See §3.2. |
| 0x07 | 1 | `u8` | `reserved` | Always zero. Reserved for future header fields. |
| 0x08 | 16 | `char[16]` | `gameFamily` | NUL-padded ASCII identifier for which game-family extension event set (§5) applies, e.g. `"smash64"`. **All-zero** (empty) if the loaded ROM's game family isn't recognized by this recorder — the file is still a fully valid core-only recording in that case, just with no extension events. Truncated if longer than 16 bytes (no defined family name is expected to need that much room). |
| 0x18 | 64 | `char[64]` | `goodName` | The recorded ROM's `GoodName` (mupen64plus-core's ROM database identity string), UTF-8, NUL-padded — not necessarily NUL-terminated if it fills the field. Truncated if longer than 64 bytes. All-zero if the ROM database has no identity for the loaded ROM. |
| 0x58 | 32 | `char[32]` | `clientName` | Name and version of the program that wrote the file, UTF-8, NUL-padded — not necessarily NUL-terminated if it fills the field. For example `RMG-K v0.9.16` for a release, or `RMG-K vdev-2310-a1b2c3d4` for a development build (commit count and commit hash). See §3.2. |
| 0x78 | 8 | `u64` | `recordedAtEpochMillis` | Wall-clock time the recording started, **milliseconds** since the Unix epoch (UTC) — from `std::chrono::system_clock`. Independent of the filename's timestamp (§3.3), though the recorder writes the same instant to both (truncated to whole seconds there). |
| 0x80 | 4 | `u32` | `uncompressedLength` | Byte length of the event stream *after* decompression. Lets a reader preallocate its decompression output buffer instead of growing it dynamically. |
| 0x84 | 4 | `u32` | `compressedLength` | Byte length of the deflate-compressed block immediately following the header — i.e. `header_size (136) + compressedLength` is the total file size. Always correct on disk: because the whole match is buffered before anything is written, there is no "0 until finalized" convention, and no truncated-but-valid file is possible — see §2's buffering trade-off. |

### 3.2 `gameFamily`, `familyVersion`, `goodName` and `clientName`

Four header fields describe what produced a file. Each answers a different question:

- **`gameFamily`** says *which extension event definitions apply*. It changes rarely. Empty means core events only: the loaded ROM isn't recognized.
- **`familyVersion`** says *which version of that family's extension spec* the file follows. It is bumped whenever the family's event definitions change, for example when a new event type or a new trailing field is added to the `smash64` events. Every build in a family shares one numbering, so `smash64` version 2 means the same events for every `goodName`. Every build writes the full layout for its family version. When a field doesn't exist in a particular ROM (vanilla SSB64 has none of Smash Remix's gameplay settings, for example), the writer stores `0` there, and `goodName` tells a reader whether that `0` is a real value. `0` when `gameFamily` is empty.
- **`goodName`** says *which exact ROM build* produced the file. `SmashRemix2.0.1` and a hypothetical `SmashRemix2.0.2` are different `goodName`s under the same `gameFamily`, because their memory layouts can differ. It never changes for a given ROM.
- **`clientName`** says *which program and version wrote the file*, such as `RMG-K v0.9.16`. When a writer bug is fixed (for example, a field read from the wrong memory offset), readers can use `clientName` to tell affected files from fixed ones. A fix like that changes no event definition, so it doesn't bump `familyVersion`.

A reader should check `gameFamily` and `familyVersion` before trusting an extension event's meaning, and can use `goodName` and `clientName` to apply build-specific rules or work around known writer bugs. Core events need none of this: they're valid to read regardless of these fields.

### 3.3 Filename convention

Not part of the on-disk format itself (a reader must not depend on it — the header's own `recordedAtEpochMillis` is the source of truth for when a recording started), but the recorder names files `YYYYMMDD-HHMMSS[-Player1][-Player2]...rmgr` — 4-digit year, 24-hour clock, local time, one hyphen-joined segment per seated player's name (each sourced from `MatchStart.playerNames`, capped at 24 characters, filesystem-unsafe characters replaced with `_`). The timestamp reflects the same instant written to `recordedAtEpochMillis`, just truncated to whole seconds and rendered as local wall-clock time instead of a UTC epoch value.

If that name is already taken (e.g. two matches recorded within the same second), the recorder appends `-2`, `-3`, ... before the extension until it finds a free name, rather than overwriting an existing file.

Headless export of an existing `.krec` uses a different, caller-chosen base name instead — by convention `<krec name>.rmgr`, the same stem as the source `.krec` it was exported from. Each match within that `.krec` gets its own explicitly-numbered file: `<krec name>-1.rmgr`, `<krec name>-2.rmgr`, ... The same collision-avoidance above still applies on top.

For this headless export path, `recordedAtEpochMillis` is derived from the source `.krec`'s own recording-start timestamp plus elapsed frames converted to milliseconds under a constant-60fps assumption.

### 3.4 Compression

Everything after the 136-byte header is one deflate-compressed block, covering the entire event stream — `EventPayloads` through the final `MatchEnd`/`MatchResult`. Compressed at the highest available deflate level; this only costs CPU time once, at match end, not per frame, since the recorder buffers events in memory during the match rather than compressing incrementally.

The header itself is **not** compressed — `magic`, `version`, `gameFamily`, `familyVersion`, `clientName`, `goodName`, and the timestamp fields are readable by any tool without decompressing anything, which matters for a file browser or library UI that wants to list many recordings' identity/timestamp without paying the cost of decompressing each one's (much larger) event stream.

A reader decompresses exactly `compressedLength` bytes starting at file offset `136` (`0x88`) and should get back exactly `uncompressedLength` bytes of event-stream data to parse as described below. There is no partial-file/truncated-recording case to handle for the event stream itself: because the whole match is buffered before any of this is written, a `.rmgr` file on disk is either the complete, valid output of a match that reached `MatchEnd`, or the file doesn't exist at all.

## 4. Core events

Always present in every file, regardless of `gameFamily`.

### 4.1 Match Start — code `0x02`

Written exactly once, as the first event after `EventPayloads`. Player display names are sourced from netplay room metadata (RMG-K's own slot-indexed name table, populated by every netplay path), never from any in-game name tag — for an offline match, or a port with no assigned name, the corresponding `playerNames` entry is all zero bytes.

Payload size: **132 bytes.**

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 128 | `char[4][32]` | `playerNames` | 4× a 32-byte, NUL-padded (not necessarily NUL-terminated if exactly 32 chars) name string, port 0-3 in order. |
| 0x80 | 4 | `u8[4]` | `slotType` | Per port 0-3: `0` human, `1` CPU, `2` empty. |

Game-family-specific match settings (stage, character, stock count, damage ratio, items, teams, handicap, CPU difficulty, …) are **not** part of this event — see `MatchSettings` (§5.1) for the `smash64` extension equivalent. A game-agnostic reader (or a recognized-but-not-yet-supported title) has everything it needs from `MatchStart`/`InputFrame`/`MatchEnd` alone to reconstruct who played what inputs, on which ports.

### 4.2 Input Frame — code `0x03`

Input-side data, captured **before** the game processes that frame's inputs. One event per seated port per frame. Uses the game's already-processed button/stick values, which is the one input representation available uniformly for **both** human and CPU-controlled ports.

Named `InputFrame` rather than "Pre-Frame" since this event has no game-family-specific counterpart to be "pre-" relative to — it's the entire core per-frame input record, full stop.

Payload size: **9 bytes.**

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i32` | `frame` | Frame counter, `0` at the first frame this match's recording enters the "ongoing" game state. Monotonically increasing, one recorded frame per real emulated frame. |
| 0x04 | 1 | `u8` | `port` | `0`-`3`. |
| 0x05 | 2 | `u16` | `buttons` | Processed button bitmask. See §8.4. |
| 0x07 | 1 | `i8` | `stickX` | Processed stick X, signed. |
| 0x08 | 1 | `i8` | `stickY` | Processed stick Y, signed. |

### 4.3 Match End — code `0x05`

Written exactly once, as the last event in the stream.

Payload size: **5 bytes.**

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i32` | `finalFrame` | The last `frame` value seen in any `InputFrame` event this match — total recorded match length. |
| 0x04 | 1 | `u8` | `endReason` | `0` aborted (match reset, or the emulator/process stopped mid-match — these two causes are not currently distinguished), `1` normal end. |

Final per-port results (e.g. Smash's stocks-remaining placements) are not a universal concept across N64 titles and are **not** part of this event — see `MatchResult` (§5.5) for the `smash64` extension equivalent.

## 5. `smash64` game-family extension events

Present only when `FileHeader.gameFamily == "smash64"`. Covers Smash Remix today; the events below are `smash64` family version `1`.

### 5.0 Event Payloads — code `0x01`

Always the first event in the decompressed stream. Declares the exact payload size (not including the 1-byte command code) of every other event type this file uses — the entire forward-compatibility mechanism a parser relies on to skip codes it doesn't recognize.

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 1 | `u8` | `count` | Number of `(code, size)` entries that follow. |
| 0x01 | 3×count | see below | `entries` | `count` repetitions of the 3-byte entry below. |

Each entry:

| Offset (rel.) | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| +0x00 | 1 | `u8` | `code` | The event command code this entry describes. |
| +0x01 | 2 | `u16` | `size` | That event's payload size, in bytes. |

A file with `gameFamily` empty declares only the three core codes (`MatchStart`, `InputFrame`, `MatchEnd`). A `smash64` file declares those three plus the five below. **A parser must always read an event's size from that file's own `EventPayloads` event, never hardcode it, and must always read `count` itself rather than assuming a fixed number of entries** — this is exactly why a reader that doesn't recognize a `gameFamily` (or an older reader facing a newer family version's appended fields) can still parse the rest of a file correctly.

### 5.1 Match Settings — code `0x08`

Written exactly once, immediately after `MatchStart`. Everything Smash-specific and static for the whole match. If a port's characters aren't yet spawned when this is captured (e.g. written during the pre-match countdown), that port's `characterId`/`costumeId`/`teamColor`/ `portTeam`/`portHandicap`/`portCpuLevel` entries are left at `0` rather than the real value — a reader can't distinguish "genuinely 0" from "not available yet" for those alone.

Payload size: **75 bytes**.

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 1 | `u8` | `stageId` | See §8.2. |
| 0x01 | 1 | `u8` | `gameType` | `1` time, `2` stock, `3` both (Remix always forces stock). |
| 0x02 | 1 | `u8` | `stockCountSetting` | 0-based (i.e. `2` means "3 stocks"). |
| 0x03 | 1 | `u8` | `timeLimitMinutes` | `100` = infinite. |
| 0x04 | 1 | `u8` | `damageRatio` | `50` = 50%, `200` = 200%. |
| 0x05 | 1 | `u8` | `itemFrequency` | `0` none .. `5` high. |
| 0x06 | 1 | `u8` | `teamsEnabled` | `0` off, `1` on. |
| 0x07 | 1 | `u8` | `handicapMode` | `0` off, `1` on, `2` auto. |
| 0x08 | 4 | `u8[4]` | `characterId` | Per port 0-3. See §8.1. Meaningless for a port whose `MatchStart.slotType == 2` (empty). |
| 0x0C | 4 | `u8[4]` | `costumeId` | Per port 0-3. |
| 0x10 | 4 | `u8[4]` | `teamColor` | Per port 0-3. |
| 0x14 | 4 | `u8[4]` | `portTeam` | Team number per port 0-3. |
| 0x18 | 4 | `u8[4]` | `portHandicap` | Per-port handicap value, meaningful only when `handicapMode != 0`. |
| 0x1C | 4 | `u8[4]` | `portCpuLevel` | CPU difficulty per port; meaningless for a `human` port. |
| 0x20 | 4 | `i32` | `rngSeed` | `sSYUtilsRandomSeed` at match start — vanilla SSB64 code, unmodified by Remix, region/version-independent. See the note below the table for the determinism story. |
| 0x24 | 1 | `u8` | `hitstun` | Gameplay setting. |
| 0x25 | 1 | `u8` | `hitlag` | Gameplay setting. |
| 0x26 | 1 | `u8` | `di` | Gameplay setting. |
| 0x27 | 1 | `u8` | `japaneseSounds` | Gameplay setting. |
| 0x28 | 1 | `u8` | `japaneseStunSleep` | Gameplay setting. |
| 0x29 | 1 | `u8` | `momentumSlide` | Gameplay setting. |
| 0x2A | 1 | `u8` | `shieldStun` | Gameplay setting. |
| 0x2B | 1 | `u8` | `zCancel` | Gameplay setting. |
| 0x2C | 1 | `u8` | `punishFailedZCancel` | Gameplay setting. |
| 0x2D | 1 | `u8` | `improvedAI` | Gameplay setting. |
| 0x2E | 1 | `u8` | `tripping` | Gameplay setting. |
| 0x2F | 1 | `u8` | `rage` | Gameplay setting. |
| 0x30 | 1 | `u8` | `footstoolJumping` | Gameplay setting. |
| 0x31 | 1 | `u8` | `airDodging` | Gameplay setting. |
| 0x32 | 1 | `u8` | `jabLocking` | Gameplay setting. |
| 0x33 | 1 | `u8` | `edgeCJumping` | Gameplay setting. |
| 0x34 | 1 | `u8` | `perfectShielding` | Gameplay setting. |
| 0x35 | 1 | `u8` | `parrying` | Gameplay setting (labeled BETA in Remix's own menu). |
| 0x36 | 1 | `u8` | `spotDodging` | Gameplay setting. |
| 0x37 | 1 | `u8` | `fastFallAerials` | Gameplay setting. |
| 0x38 | 1 | `u8` | `ledgeTrumping` | Gameplay setting. |
| 0x39 | 1 | `u8` | `wallTeching` | Gameplay setting. |
| 0x3A | 1 | `u8` | `chargeSmashes` | Gameplay setting. |
| 0x3B | 1 | `u8` | `itemContainers` | Gameplay setting. |
| 0x3C | 1 | `u8` | `gameSpeed` | Gameplay setting. |
| 0x3D | 1 | `u8` | `specialZoom` | Gameplay setting (labeled BETA in Remix's own menu). |
| 0x3E | 1 | `u8` | `blastzoneWarp` | Gameplay setting (labeled BETA in Remix's own menu). |
| 0x3F | 1 | `u8` | `singleButtonMode` | Gameplay setting. |
| 0x40 | 1 | `u8` | `allItemsRDropAerial` | Gameplay setting. |
| 0x41 | 1 | `u8` | `moveStaling` | Gameplay setting. |
| 0x42 | 1 | `u8` | `stopwatchItem` | Gameplay setting. |
| 0x43 | 1 | `u8` | `stageSelectLayout` | Stage setting. |
| 0x44 | 1 | `u8` | `hazardMode` | Stage setting. |
| 0x45 | 1 | `u8` | `whispyMode` | Stage setting. |
| 0x46 | 1 | `u8` | `saffronPokemonRate` | Stage setting. |
| 0x47 | 1 | `u8` | `pokemonAnnouncer` | Stage setting. |
| 0x48 | 1 | `u8` | `dragonKingHUD` | Stage setting. |
| 0x49 | 1 | `u8` | `cameraMode` | Stage setting. |
| 0x4A | 1 | `u8` | `yoshiIslandCloudAnims` | Stage setting. |

**RNG seed and determinism.** `rngSeed` is `sSYUtilsRandomSeed` (`0x8003B940`, live value, big-endian `s32`) captured once, at the same moment as the rest of `MatchSettings`. It's a classic LCG: every "random" draw anywhere in the game — item spawns, CPU decisions, hit-effect variance, damage rolls, Whispy Woods' wind timing, and more — advances this one shared 32-bit value via `seed = (seed * 214013 + 2531011) mod 2^32`. There's no dedicated per-mechanic seed and no min/max — any 32-bit value is valid state. Loading this seed at match start and then replaying this match's recorded `InputFrame` events reproduces the same sequence of draws, and therefore the same RNG-dependent outcomes, as the original match. This is sufficient for deterministic replay-from-inputs; it is not a substitute for re-deriving an already-recorded outcome without actually re-simulating.

**Gameplay/stage settings.** The 39 fields from `hitstun` through `yoshiIslandCloudAnims` are Smash Remix's own `Toggles.asm` menu-configured mutators — **match properties, captured once alongside the rest of `MatchSettings`, not per-frame data**: they reflect the setting in effect for the whole match, not anything that can change frame to frame. Every value is a raw `u8` read from a 4-byte, big-endian memory word (the *live value* at a `Toggles.asm` entry's `+0x4` offset, not the entry's own base address) — see §5.1.1/§5.1.2 for what each value means. Out of Remix's full settings surface, this intentionally covers only the "Gameplay Settings" and core "Stage Settings" categories — the ~18 named plus ~170 auto-generated per-stage/per-track random-pool toggles (stage selection weighting, music selection weighting, and similar), and the Player Tags/other cosmetic-only settings, are not captured. Any of those can be added later the same way, via an ordinary field-append, if a concrete need comes up.

#### 5.1.1 Gameplay settings — value meanings

| Field | Values (0-based) |
| --- | --- |
| `hitstun` | `0`=Normal; `1`=Melee |
| `hitlag` | `0`=Normal; `1`=Japanese; `2`=Melee; `3`=Ultimate; `4`=None |
| `di` | `0`=Normal; `1`=Japanese; `2`=Ultimate |
| `japaneseSounds` | `0`=Default; `1`=Always; `2`=Never |
| `japaneseStunSleep` | `0`=Off; `1`=On |
| `momentumSlide` | `0`=Off; `1`=On |
| `shieldStun` | `0`=Default; `1`=Japanese; `2`=Melee; `3`=Brawl; `4`=Ultimate |
| `zCancel` | `0`=Default; `1`=Disabled; `2`=Melee (7 frames); `3`=Auto; `4`=Glide Mode |
| `punishFailedZCancel` | `0`=Off; `1`=7% Damage; `2`=Lava Floor; `3`=Shield-Break; `4`=Instant K.O.; `5`=Force Taunt; `6`=Bury; `7`=Laugh Track; `8`=Egg; `9`=Sleep; `10`=Trip; `11`=Random |
| `improvedAI` | `0`=Off; `1`=On |
| `tripping` | `0`=Off; `1`=Low; `2`=High; `3`="Brawl" |
| `rage` | `0`=Off; `1`=Ultimate; `2`=Smash 4; `3`=Berserk; `4`=Fatigue |
| `footstoolJumping` | `0`=Off; `1`=On |
| `airDodging` | `0`=Off; `1`=Melee; `2`=Ultimate; `3`=Air Dash |
| `jabLocking` | `0`=Off; `1`=On |
| `edgeCJumping` | `0`=Off; `1`=On |
| `perfectShielding` | `0`=Off; `1`=On |
| `parrying` | `0`=Off; `1`=On |
| `spotDodging` | `0`=Off; `1`=On |
| `fastFallAerials` | `0`=Off; `1`=On |
| `ledgeTrumping` | `0`=Off; `1`=On |
| `wallTeching` | `0`=Off; `1`=On |
| `chargeSmashes` | `0`=Off; `1`=On; `2`=Unlimited Charge |
| `itemContainers` | `0`=Default; `1`=Off; `2`=Never Explode; `3`=Always Explode |
| `gameSpeed` | `0`=1/1; `1`=1.2x; `2`=1.3x; `3`=1.5x; `4`=1.75x; `5`=2.0x; `6`=3.0x; `7`=1/8; `8`=1/4; `9`=1/3; `10`=1/2; `11`=2/3; `12`=3/4 |
| `specialZoom` | `0`=Off; `1`=Match End; `2`=Any KO |
| `blastzoneWarp` | `0`=Off; `1`=Left/Right; `2`=Top/Bottom; `3`=All |
| `singleButtonMode` | `0`=Off; `1`="A"; `2`="B"; `3`="R"; `4`="A"+"C"; `5`="B"+"C"; `6`="R"+"C" |
| `allItemsRDropAerial` | `0`=Off; `1`=On |
| `moveStaling` | `0`=Default; `1`=Disabled; `2`=Lenient; `3`=Strict; `4`=Wait For It; `5`=Reverse; `6`=Cheap Shot |
| `stopwatchItem` | `0`=Default; `1`=Slow Always; `2`=Fast Always; `3`=Backfire Always; `4`=Backfire Never |

#### 5.1.2 Stage settings — value meanings

| Field | Values (0-based) |
| --- | --- |
| `stageSelectLayout` | `0`=Normal; `1`=Tournament |
| `hazardMode` | `0`=Normal; `1`=Hazards Off; `2`=Movement Off; `3`=All Off |
| `whispyMode` | `0`=Normal; `1`=Japanese; `2`=Super; `3`=Hyper |
| `saffronPokemonRate` | `0`=Normal; `1`=Super; `2`=Hyper; `3`=Quick Attack |
| `pokemonAnnouncer` | `0`=Stadium; `1`=All Stages; `2`=Off |
| `dragonKingHUD` | `0`=Dragon King; `1`=All Stages; `2`=Off |
| `cameraMode` | `0`=Normal; `1`=Bonus; `2`=Fixed; `3`=Scene |
| `yoshiIslandCloudAnims` | `0`=Off; `1`=On |

### 5.2 State Frame — code `0x04`

State-side data, captured **after** that frame's physics/collision resolution — the resulting state. One event per seated port per frame, always immediately following that port's `InputFrame` in the stream.

Named `StateFrame` for the same reason as `InputFrame` above — it's paired with the core input event by convention/ordering, not by a "Pre/Post" naming relationship baked into the format itself.

Payload size: **71 bytes**.

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i32` | `frame` | Same frame counter as the paired `InputFrame`. |
| 0x04 | 1 | `u8` | `port` | `0`-`3`. |
| 0x05 | 1 | `u8` | `characterId` | See §8.1. |
| 0x06 | 2 | `u16` | `actionStateId` | See §8.3. |
| 0x08 | 4 | `f32` | `positionX` | IEEE-754 single precision. |
| 0x0C | 4 | `f32` | `positionY` |  |
| 0x10 | 4 | `i32` | `facingDirection` | `1` = facing right, `-1` = facing left. |
| 0x14 | 4 | `f32` | `velocityX` |  |
| 0x18 | 4 | `f32` | `velocityY` |  |
| 0x1C | 4 | `u32` | `damagePercent` | Whole-number percent, as the game itself stores it (not a float). |
| 0x20 | 1 | `i8` | `stocksRemaining` | 0-based; negative once eliminated. |
| 0x21 | 1 | `u8` | `jumpsRemaining` | `jumpsMax` (per-character) minus `jumps_used`, which resets to `0` on landing. `0` through most of a grounded match is normal. |
| 0x22 | 1 | `u8` | `groundedState` | `0` grounded, `1` airborne. |
| 0x23 | 1 | `u8` | `hurtboxState` | The motion-script hit status (`GMHitStatus`, low byte of `hitstatus`, `FTStruct+0x5B8`): `0` hurtboxes off, `1` normal, `2` invincible (can be hit, takes no damage/knockback), `3` intangible (can't be hit). Dodges, rolls and ledge-grab intangibility show here; respawn invincibility doesn't (see `specialHitStatus`), and Super Star item invincibility isn't recorded. |
| 0x24 | 2 | `u16` | `hitstunCounter` | Non-zero while in hitstun. |
| 0x26 | 4 | `u32` | `actionFrameCounter` | Frame counter of the current action state (resets when the action state changes). |
| 0x2A | 4 | `u32` | `comboHitCount` | Belongs to the *victim* (this port), not the attacker: hits taken in the current unbroken chain. `0` = no active chain, `1` = a single hit, `2+` = an actual combo. |
| 0x2E | 4 | `u32` | `comboDamage` | Running damage dealt within the same chain as `comboHitCount`; zeroes at the same instant. |
| 0x32 | 4 | `f32` | `scaleX` | The fighter's render scale: its root joint's `DObj` `scale.vec.f.x` (`FTStruct+0x8E8` → `DObj+0x40`). `1.0` × Remix's Giant/Tiny setting normally; `0` if unreadable. |
| 0x36 | 4 | `f32` | `scaleY` | Same joint, `DObj+0x44`. |
| 0x3A | 4 | `i32` | `characterSpecific` | `FTStruct+0xADC`, the first word of the per-character `passive_vars` union — meaning depends on the character (table below). |
| 0x3E | 4 | `i32` | `shieldHealth` | `FTStruct+0x34` (`shield_health`). |
| 0x42 | 1 | `u8` | `specialHitStatus` | Low byte of `special_hitstatus` (`FTStruct+0x5AC`), a hit status (values as `hurtboxState`) set by the timed invincible/intangible counters: respawn invincibility (`2`), wall-bounce and being trapped in Yoshi's egg (`3`), and a 1-frame re-hit grace after taking damage. |
| 0x43 | 4 | `f32` | `knockbackResist` | `FTStruct+0x7E8` (`knockback_resist_status`): temporary armor, knockback units subtracted from incoming knockback; cleared on every action change. Among the original 12 only Yoshi's double jump sets it (140 US / 110 JP). `0` = no armor. |

The game treats `hurtboxState` and `specialHitStatus` as independent layers active at the same time: a fighter can't be hit if either is `3`, and takes no damage if either is `2`. Dodges, rolls and ledge-grab intangibility show in `hurtboxState`; respawn invincibility in `specialHitStatus`. A third layer, the Super Star item's invincibility (`star_hitstatus`, `FTStruct+0x5B4`), isn't recorded.

`scaleX`/`scaleY` matter for gameplay, not just visuals: Remix derives the fighter's ECB and ledge-grab reach from the same size multiplier. In particular, Kirby's aerial up-special can leave him stuck slightly larger than normal until he respawns (a known Remix issue), which shows up here.

`characterSpecific` by character (identical in US and JP builds):

| Character | Meaning | Values |
| --- | --- | --- |
| Samus | Stored Charge Shot level (`charge_level`) | `0`–`7`; reset to `0` on firing |
| Donkey Kong | Stored Giant Punch charge (`charge_level`) | `0`–`10`; reset to `0` on release |
| Kirby | Copied ability (`copy_id`) — the copied fighter's character ID | §8.1 IDs; **`8` (Kirby) = no copy ability**. Can be lost at random (1 in 12) when hit, or replaced by inhaling again |
| Everyone else | Not meaningful | — |

These values appear to **persist through losing a stock**. In the decompilation, every reset of Samus's/DK's `charge_level` and Kirby's `copy_id` is tied to a gameplay event (firing the charge, losing the copy ability, the 1-in-12 roll when hit), and the respawn path (`ftcommonrebirth.c`, `ftcommondead.c`, `ftcommonentry.c`) doesn't clear them. This comes from reading the code, not from testing in a live game — treat them as persisting until a recording shows otherwise.

### 5.3 Item Update — code `0x06`

Zero or more per frame — one per live Item or Weapon `GObj` currently not held by a fighter, following that frame's `InputFrame`/`StateFrame` pairs. Items and Weapons live on two separate `GObj` lists (`gGCCommonLinks[4]` Item, `gGCCommonLinks[5]` Weapon), not one shared list. A held item (e.g. Link's bomb while still in his hand) is not emitted: while held, its position reads as a meaningless local offset near `(0,0,0)` instead of a world coordinate, and that's used as the "currently held" proxy.

Payload size: **33 bytes**.

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i32` | `frame` | Same frame counter as that frame's `InputFrame`/`StateFrame`. |
| 0x04 | 4 | `u32` | `objectAddress` | The `GObj`'s own RDRAM address — the closest available stable per-object identity, valid for as long as that object is alive. Not a semantic spawn ID; the address can be reused once an object is freed. |
| 0x08 | 1 | `u8` | `linkId` | `4` = Item, `5` = Weapon — which enum `kind` below means. See §8.6. |
| 0x09 | 4 | `i32` | `kind` | `ITKind` (`linkId == 4`) or `WPKind` (`linkId == 5`) — the real, named per-instance type. Full enums in §8.6. |
| 0x0D | 4 | `f32` | `positionX` | IEEE-754 single precision. World-space. |
| 0x11 | 4 | `f32` | `positionY` |  |
| 0x15 | 4 | `f32` | `positionZ` |  |
| 0x19 | 4 | `f32` | `scaleX` | The object's render scale — its `DObj`'s `scale.vec.f.x` (`GObj+0x74` → `DObj+0x40`). |
| 0x1D | 4 | `f32` | `scaleY` | `DObj+0x44` (`scale.vec.f.y`). |

`scaleX`/`scaleY` are constant for most objects. Samus's Charge Shot (`WPKind` `0x02`) is the notable exception: the orb she charges in front of her cannon is the same weapon object that's later fired, and while charging the game sets `scaleX = scaleY = gfx_size / 30` for its current charge level (`wpSamusChargeShotProcUpdate`), which climbs one level every 20 frames of charging:

| Charge level | `gfx_size` | Scale (`gfx_size / 30`) | Damage | Hitbox size |
| --: | --: | --: | --: | --: |
| 0 | 150 | 5.00 | 3 | 100 |
| 1 | 230 | 7.67 | 6 | 120 |
| 2 | 280 | 9.33 | 9 | 140 |
| 3 | 340 | 11.33 | 12 | 160 |
| 4 | 410 | 13.67 | 15 | 180 |
| 5 | 490 | 16.33 | 18 | 200 |
| 6 | 600 | 20.00 | 21 | 240 |
| 7 (full) | 700 | 23.33 | 26 | 260 |

(From `dWPSamusChargeShotWeaponAttributes[]`, `wpsamuschargeshot.c`, same for US and JP builds.)

Deliberately not captured (not yet mapped in memory): velocity, damage/knockback dealt, owner/attacker port, and any per-object timer/expiration. A future family version can append any of these as trailing fields once mapped, without breaking this version's readers.

### 5.4 Stage Hazard Update — code `0x07`

Zero or one per frame, following that frame's `ItemUpdate` events — written only when at least one tracked hazard is currently active, same sparse convention as `ItemUpdate` (never a zeroed/placeholder event for "nothing active"). Currently tracks exactly one hazard: Whispy Woods' wind on Dream Land. More hazards (Zebes' rising acid, Duel Zone's disappearing platforms, …) can claim more bits in `hazardFlags` later via the field-append mechanism, without needing a new event type.

Payload size: **5 bytes.**

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i32` | `frame` | Same frame counter as that frame's `InputFrame`/`StateFrame`. |
| 0x04 | 1 | `u8` | `hazardFlags` | Bitmask. Bit `0x01`: Whispy Woods currently blowing (Dream Land only). Bit `0x02`: wind direction — `0` = blowing left, `1` = blowing right; only meaningful when bit `0x01` is also set. |

### 5.5 Match Result — code `0x09`

Written exactly once, as the last event in the stream — immediately after the core `MatchEnd` event.

Payload size: **4 bytes.**

| Offset | Size | Type | Field | Notes |
| --: | --: | --- | --- | --- |
| 0x00 | 4 | `i8[4]` | `placements` | Final stocks remaining, per port 0-3. `-1` for any port that was never seated. |

## 6. Versioning and forward compatibility

Two independent mechanisms:

- **Field additions to an existing event:** always append new fields to the *end* of that event's payload, never insert in the middle. An old parser — which learned the event's size from that file's own `EventPayloads` event, which will correctly declare the *old*, smaller size for an old file — simply never reads the new trailing bytes. A new parser reading an old file sees the old, smaller declared size and correctly knows not to read fields that were never written.
- **New event types:** an old parser encountering a command code it doesn't recognize looks up its declared size in `EventPayloads` and skips exactly that many bytes, then continues from the next event.

`FileHeader.version` (currently `6`) is reserved for a breaking change to the header or the overall framing itself (e.g. the header layout, or the buffered/compressed structure) — not for anything the two mechanisms above already cover, and not for tracking which game/ROM produced a file or how that recorder's understanding of it has evolved either — that's `familyVersion`, `goodName` and `clientName` (§3.2).

Within a game family, every change to the event definitions, whether a new event type or a new trailing field, bumps `familyVersion`. Fields added after a family's version `1` are marked in the layout tables with the version that added them.

## 7. Byte order and encoding

Everything in this file — the header and every (decompressed) event payload — is **little-endian**. Values are read directly from emulated memory via `DebugMemRead8/16/32` (which already normalize to host byte order) and written to disk as raw native-layout structs on little-endian host platforms.

Every event payload has a fixed size, with no padding and no bitfields: the layout tables in §3–§5 list every field in order, and the field sizes add up to the payload size.

Floats are IEEE-754 single precision (32-bit), stored as the exact bit pattern the game itself holds in memory — reinterpret the 4 bytes as a `float`/`f32`, don't scale or convert.

Strings (`playerNames` in `MatchStart`, `goodName`/`gameFamily`/`clientName` in the header) are fixed-width byte arrays, NUL-padded, **not necessarily NUL-terminated** if the value fills the full field width — always read up to the declared field width and trim trailing NULs, never scan for a terminator past the field boundary.

## 8. Reference tables

### 8.1 Character IDs

Valid range `0x00`-`0x60`.

**Vanilla (`0x00`-`0x1C`):**

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x00` | Mario |  | `0x0E` | Polygon Mario |
| `0x01` | Fox |  | `0x0F` | Polygon Fox |
| `0x02` | Donkey Kong |  | `0x10` | Polygon Donkey Kong |
| `0x03` | Samus |  | `0x11` | Polygon Samus |
| `0x04` | Luigi |  | `0x12` | Polygon Luigi |
| `0x05` | Link |  | `0x13` | Polygon Link |
| `0x06` | Yoshi |  | `0x14` | Polygon Yoshi |
| `0x07` | Captain Falcon |  | `0x15` | Polygon Captain Falcon |
| `0x08` | Kirby |  | `0x16` | Polygon Kirby |
| `0x09` | Pikachu |  | `0x17` | Polygon Pikachu |
| `0x0A` | Jigglypuff |  | `0x18` | Polygon Jigglypuff |
| `0x0B` | Ness |  | `0x19` | Polygon Ness |
| `0x0C` | Master Hand |  | `0x1A` | Giant DK |
| `0x0D` | Metal Mario |  | `0x1B` | Random |
|  |  |  | `0x1C` | (unused) |

**Remix fighters (`0x1D`-`0x4C`):**

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x1D` | Falco |  | `0x35` | Giga Bowser |
| `0x1E` | Ganondorf |  | `0x36` | Piano |
| `0x1F` | Young Link |  | `0x37` | Wolf |
| `0x20` | Dr. Mario |  | `0x38` | Conker |
| `0x21` | Wario |  | `0x39` | Mewtwo |
| `0x22` | Dark Samus |  | `0x3A` | Marth |
| `0x23` | Link (EU) |  | `0x3B` | Sonic |
| `0x24` | Samus (JP) |  | `0x3C` | Sandbag |
| `0x25` | Ness (JP) |  | `0x3D` | Super Sonic |
| `0x26` | Lucas |  | `0x3E` | Sheik |
| `0x27` | Link (JP) |  | `0x3F` | Marina |
| `0x28` | Falcon (JP) |  | `0x40` | King Dedede |
| `0x29` | Fox (JP) |  | `0x41` | Goemon |
| `0x2A` | Mario (JP) |  | `0x42` | Peppy |
| `0x2B` | Luigi (JP) |  | `0x43` | Slippy |
| `0x2C` | DK (JP) |  | `0x44` | Banjo |
| `0x2D` | Pikachu (EU) |  | `0x45` | Metal Luigi |
| `0x2E` | Jigglypuff (JP) |  | `0x46` | Ebisumaru |
| `0x2F` | Jigglypuff (EU) |  | `0x47` | Dragon King |
| `0x30` | Kirby (JP) |  | `0x48` | Crash |
| `0x31` | Yoshi (JP) |  | `0x49` | Peach |
| `0x32` | Pikachu (JP) |  | `0x4A` | Roy |
| `0x33` | Samus (EU) |  | `0x4B` | Dr. Luigi |
| `0x34` | Bowser |  | `0x4C` | Lanky Kong |

**Remix polygons (`0x4D`-`0x60`):**

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x4D` | Wario |  | `0x57` | Dark Samus |
| `0x4E` | Lucas |  | `0x58` | Marth |
| `0x4F` | Bowser |  | `0x59` | Mewtwo |
| `0x50` | Wolf |  | `0x5A` | Dedede |
| `0x51` | Dr. Mario |  | `0x5B` | Young Link |
| `0x52` | Sonic |  | `0x5C` | Goemon |
| `0x53` | Sheik |  | `0x5D` | Conker |
| `0x54` | Marina |  | `0x5E` | Banjo |
| `0x55` | Falco |  | `0x5F` | Peach |
| `0x56` | Ganondorf |  | `0x60` | Crash |

### 8.2 Stage IDs

**Vanilla:**

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x00` | Peach's Castle |  | `0x09` | Dream Land Beta 1 |
| `0x01` | Sector Z |  | `0x0A` | Dream Land Beta 2 |
| `0x02` | Congo Jungle |  | `0x0B` | How to Play |
| `0x03` | Planet Zebes |  | `0x0C` | Mini Yoshi's Island |
| `0x04` | Hyrule Castle |  | `0x0D` | Meta Crystal |
| `0x05` | Yoshi's Island |  | `0x0E` | Duel Zone |
| `0x06` | Dream Land |  | `0x0F` | Race to the Finish |
| `0x07` | Saffron City |  | `0x10` | Final Destination |
| `0x08` | Mushroom Kingdom |  |  |  |

Remix adds a very large number of additional stages (`0x29` onward, into the `0xD0`+ range) not enumerated here — see the *Known limitations* note in §9.

### 8.3 Action state IDs

`0x000`-`0x0DB` are shared across every character; `>= 0x0DC` is character-specific. Where a name ends in a numbered/lettered range (e.g. `Walk1-3`), the `Value` column's range covers that many consecutive, individually-meaningful states in that order — not one state that happens to span multiple codes.

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x000` | DeadD (KO bottom) |  | `0x054` | CliffCatch |
| `0x001` | DeadS (KO side) |  | `0x055` | CliffWait |
| `0x002` | DeadU (KO top) |  | `0x056` | CliffQuick |
| `0x003` | ScreenKO |  | `0x057-0x058` | CliffClimbQuick1-2 |
| `0x004` | ScreenKOWait |  | `0x059` | CliffSlow |
| `0x005` | Entry (spawn) |  | `0x05A-0x05B` | CliffClimbSlow1-2 |
| `0x007` | Revive1 |  | `0x05C-0x05F` | CliffAttack Quick/Slow |
| `0x008` | Revive2 |  | `0x060-0x063` | CliffEscape Quick/Slow |
| `0x009` | ReviveWait |  | `0x064-0x07D` | Item pickup/throw actions |
| `0x00A` | Idle |  | `0x07E-0x097` | Item-specific attacks |
| `0x00B-0x00D` | Walk1-3 |  | `0x098` | ShieldOn |
| `0x00F` | Dash |  | `0x099` | Shield |
| `0x010` | Run |  | `0x09A` | ShieldOff |
| `0x011` | RunBrake |  | `0x09B` | ShieldStun |
| `0x012` | Turn |  | `0x09C` | RollF |
| `0x013` | TurnRun |  | `0x09D` | RollB |
| `0x014` | JumpSquat |  | `0x09E` | ShieldBreak |
| `0x015` | ShieldJumpSquat |  | `0x09F` | ShieldBreakFall |
| `0x016` | JumpF |  | `0x0A0-0x0A3` | Stun land/start |
| `0x017` | JumpB |  | `0x0A4` | Stun |
| `0x018` | JumpAerialF |  | `0x0A5` | Sleep |
| `0x019` | JumpAerialB |  | `0x0A6` | Grab |
| `0x01A` | Fall |  | `0x0A7` | GrabPull |
| `0x01B` | FallAerial |  | `0x0A8` | GrabWait |
| `0x01C` | Crouch |  | `0x0A9` | ThrowF |
| `0x01D` | CrouchIdle |  | `0x0AA` | ThrowB |
| `0x01E` | CrouchEnd |  | `0x0AB-0x0B3` | Captured/inhaled/egg-laid |
| `0x01F` | LandingLight |  | `0x0B5-0x0BC` | Being thrown |
| `0x020` | LandingHeavy |  | `0x0BD` | Taunt |
| `0x021` | Pass (platform drop) |  | `0x0BE` | Jab1 |
| `0x022` | ShieldDrop |  | `0x0BF` | Jab2 |
| `0x023` | Teeter |  | `0x0C0` | DashAttack |
| `0x024` | TeeterStart |  | `0x0C1-0x0C5` | FTilt (High->Low) |
| `0x025-0x027` | DamageHigh1-3 |  | `0x0C7` | UTilt |
| `0x028-0x02A` | DamageMid1-3 |  | `0x0C9` | DTilt |
| `0x02B-0x02D` | DamageLow1-3 |  | `0x0CA-0x0CE` | FSmash (High->Low) |
| `0x02E-0x030` | DamageAir1-3 |  | `0x0CF` | USmash |
| `0x031-0x032` | DamageElec1-2 |  | `0x0D0` | DSmash |
| `0x033` | DamageFlyHigh |  | `0x0D1` | Nair |
| `0x034` | DamageFlyMid |  | `0x0D2` | Fair |
| `0x035` | DamageFlyLow |  | `0x0D3` | Bair |
| `0x036` | DamageFlyTop |  | `0x0D4` | Uair |
| `0x037` | DamageFlyRoll |  | `0x0D5` | Dair |
| `0x038` | WallBounce |  | `0x0D6-0x0DA` | Aerial landing lag (N/F/B/U/D) |
| `0x039` | Tumble |  | `0x0DB` | LandingAirX (Z-cancel) |
| `0x03A` | FallSpecial |  |  |  |
| `0x03B` | LandingSpecial |  |  |  |
| `0x03C` | Tornado |  |  |  |
| `0x03D` | Barrel |  |  |  |
| `0x03E-0x041` | Pipe |  |  |  |
| `0x042` | CeilingBonk |  |  |  |
| `0x043-0x048` | Knocked down/getup |  |  |  |
| `0x049-0x04A` | TechF/TechB |  |  |  |
| `0x04B-0x04E` | Getup roll fwd/back |  |  |  |
| `0x04F` | DownAttackD |  |  |  |
| `0x050` | DownAttackU |  |  |  |
| `0x051` | Tech |  |  |  |
| `0x052` | Clang |  |  |  |
| `0x053` | ClangRecoil |  |  |  |

Derived predicates a consumer may find useful: dead/being-KO'd = `actionStateId <= 0x004`; respawning = `actionStateId == 0x005` or `0x007`-`0x009`; in hitstun = `actionStateId` in `0x025`-`0x039` (or check `hitstunCounter` directly); shielding = `actionStateId` in `0x098`-`0x09B`; grabbed = `actionStateId` in `0x0AB`-`0x0BC`; attacking = `actionStateId >= 0x0BE`. Airborne state should come from `groundedState`, not be inferred from `actionStateId` alone.

#### 8.3.1 Character-specific action states (`>= 0x0DC`)

**There is no cross-character ID sharing at the engine level.** Every character's status enum independently restarts at `0x0DC` in its own source file — the same numeric value means something different (or nothing) per character, entirely dependent on which character is asking. **The one exception**: Luigi's status table is byte-for-byte identical to Mario's and points at the literal same functions — not merely coincidentally same-numbered — so the two share one table below. No other pair shares code this way; every other character below has genuinely its own, independently-verified table (confirmed directly against each character's own `ftchar/ft<name>/ft<name>.h` in the decompilation, not inferred or guessed).

A handful of states in this range are **not** special moves despite sharing the block: `Attack13` (Mario/Luigi/Falcon/Link/Ness) and `Attack100Start/Loop/End` (Fox/Kirby/Jigglypuff) are jab-combo continuation states that simply live in the same numeric block as the specials in the original engine's per-character table layout.

These tables cover the original 12 characters only (by moveset, i.e. also covering their same-moveset Polygon/JP/EU-region variant IDs, but *not* any Remix-added unique character, even one that looks like a reskin, e.g. `Dr. Mario`/`Dr. Luigi`/`Dark Samus`/`Giant DK`: unlike Polygon/JP/EU variants, these are not confirmed to share the original character's code). A character-specific ID for any character not covered below has no name yet and should render as a generic placeholder (e.g. `Special 0x0DC`).

No Japanese names exist for any of the states below — like `ItemUpdate`'s `kind`, these come straight from the decompilation's English symbol names, and guessing a translation would be worse than not having one.

**Mario / Luigi** (9 states, `0x0DC`-`0x0E4`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack13 (jab-combo continuation, not a special) |  | `0x0E0` | SpecialAirN |
| `0x0DD` | AppearR |  | `0x0E1` | SpecialHi |
| `0x0DE` | AppearL |  | `0x0E2` | SpecialAirHi |
| `0x0DF` | SpecialN |  | `0x0E3` | SpecialLw |
|  |  |  | `0x0E4` | SpecialAirLw |

**Fox** (26 states, `0x0DC`-`0x0F5`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack100Start (jab-combo continuation, not a special) |  | `0x0EA` | SpecialAirHiEnd |
| `0x0DD` | Attack100Loop (jab-combo continuation, not a special) |  | `0x0EB` | SpecialAirHiBound |
| `0x0DE` | Attack100End (jab-combo continuation, not a special) |  | `0x0EC` | SpecialLwStart |
| `0x0DF` | AppearR |  | `0x0ED` | SpecialLwHit |
| `0x0E0` | AppearL |  | `0x0EE` | SpecialLwEnd |
| `0x0E1` | SpecialN |  | `0x0EF` | SpecialLwLoop |
| `0x0E2` | SpecialAirN |  | `0x0F0` | SpecialLwTurn |
| `0x0E3` | SpecialHiStart |  | `0x0F1` | SpecialAirLwStart |
| `0x0E4` | SpecialAirHiStart |  | `0x0F2` | SpecialAirLwHit |
| `0x0E5` | SpecialHiHold |  | `0x0F3` | SpecialAirLwEnd |
| `0x0E6` | SpecialAirHiHold |  | `0x0F4` | SpecialAirLwLoop |
| `0x0E7` | SpecialHi |  | `0x0F5` | SpecialAirLwTurn |
| `0x0E8` | SpecialAirHi |  |  |  |
| `0x0E9` | SpecialHiEnd |  |  |  |

**Donkey Kong** (30 states, `0x0DC`-`0x0F9`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | AppearR |  | `0x0EB` | ThrowFWait |
| `0x0DD` | AppearL |  | `0x0EC` | ThrowFWalkSlow |
| `0x0DE` | SpecialNStart |  | `0x0ED` | ThrowFWalkMiddle |
| `0x0DF` | SpecialAirNStart |  | `0x0EE` | ThrowFWalkFast |
| `0x0E0` | SpecialNLoop |  | `0x0EF` | ThrowFTurn |
| `0x0E1` | SpecialAirNLoop |  | `0x0F0` | ThrowFKneeBend |
| `0x0E2` | SpecialNEnd |  | `0x0F1` | ThrowFFall |
| `0x0E3` | SpecialAirNEnd |  | `0x0F2` | ThrowFLanding |
| `0x0E4` | SpecialNFull |  | `0x0F3` | ThrowFDamage (ThrowFEnd) |
| `0x0E5` | SpecialAirNFull |  | `0x0F4` | ThrowFF |
| `0x0E6` | SpecialHi |  | `0x0F5` | ThrowAirFF |
| `0x0E7` | SpecialAirHi |  | `0x0F6` | HeavyThrowF |
| `0x0E8` | SpecialLwStart |  | `0x0F7` | HeavyThrowB |
| `0x0E9` | SpecialLwLoop |  | `0x0F8` | HeavyThrowF4 |
| `0x0EA` | SpecialLwEnd |  | `0x0F9` | HeavyThrowB4 |

**Samus** (11 states, `0x0DC`-`0x0E6`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | AppearR |  | `0x0E2` | SpecialAirNEnd |
| `0x0DD` | AppearL |  | `0x0E3` | SpecialHi |
| `0x0DE` | SpecialNStart |  | `0x0E4` | SpecialAirHi |
| `0x0DF` | SpecialNLoop |  | `0x0E5` | SpecialLw |
| `0x0E0` | SpecialNEnd |  | `0x0E6` | SpecialAirLw |
| `0x0E1` | SpecialAirNStart |  |  |  |

**Link** (17 states, `0x0DC`-`0x0EC`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack13 (jab-combo continuation, not a special) |  | `0x0E5` | SpecialN |
| `0x0DD` | Attack100Start |  | `0x0E6` | SpecialNGet |
| `0x0DE` | Attack100Loop |  | `0x0E7` | SpecialNEmpty |
| `0x0DF` | Attack100End |  | `0x0E8` | SpecialAirN |
| `0x0E0` | AppearR |  | `0x0E9` | SpecialAirNReturn |
| `0x0E1` | AppearL |  | `0x0EA` | SpecialAirNEmpty |
| `0x0E2` | SpecialHi |  | `0x0EB` | SpecialLw |
| `0x0E3` | SpecialHiEnd |  | `0x0EC` | SpecialAirLw |
| `0x0E4` | SpecialAirHi |  |  |  |

**Yoshi** (14 states, `0x0DC`-`0x0E9`) — `SpecialHi`/`SpecialAirHi` here are the plain double-jump slot; Yoshi has no real up-B:

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | AppearR |  | `0x0E3` | SpecialAirLwLoop |
| `0x0DD` | AppearL |  | `0x0E4` | SpecialN |
| `0x0DE` | SpecialHi |  | `0x0E5` | SpecialNCatch |
| `0x0DF` | SpecialAirHi |  | `0x0E6` | SpecialNRelease |
| `0x0E0` | SpecialLwStart |  | `0x0E7` | SpecialAirN |
| `0x0E1` | SpecialLwLanding |  | `0x0E8` | SpecialAirNCatch |
| `0x0E2` | SpecialAirLwStart |  | `0x0E9` | SpecialAirNRelease |

**Captain Falcon** (19 states, `0x0DC`-`0x0EE`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack13 (jab-combo continuation, not a special) |  | `0x0E6` | SpecialLw |
| `0x0DD` | Attack100Start |  | `0x0E7` | SpecialLwAir |
| `0x0DE` | Attack100Loop |  | `0x0E8` | SpecialLwLanding |
| `0x0DF` | Attack100End |  | `0x0E9` | SpecialAirLw |
| `0x0E0` | AppearRStart |  | `0x0EA` | SpecialLwBound |
| `0x0E1` | AppearLStart |  | `0x0EB` | SpecialHi |
| `0x0E2` | AppearREnd |  | `0x0EC` | SpecialHiCatch |
| `0x0E3` | AppearLEnd |  | `0x0ED` | SpecialHiThrow |
| `0x0E4` | SpecialN |  | `0x0EE` | SpecialAirHi |
| `0x0E5` | SpecialAirN |  |  |  |

**Pikachu** (18 states, `0x0DC`-`0x0ED`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | AppearR |  | `0x0E6` | SpecialAirLwHit |
| `0x0DD` | AppearL |  | `0x0E7` | SpecialAirLwEnd |
| `0x0DE` | SpecialN |  | `0x0E8` | SpecialHiStart |
| `0x0DF` | SpecialAirN |  | `0x0E9` | SpecialHi |
| `0x0E0` | SpecialLwStart |  | `0x0EA` | SpecialHiEnd |
| `0x0E1` | SpecialLwLoop |  | `0x0EB` | SpecialAirHiStart |
| `0x0E2` | SpecialLwHit |  | `0x0EC` | SpecialAirHi |
| `0x0E3` | SpecialLwEnd |  | `0x0ED` | SpecialAirHiEnd |
| `0x0E4` | SpecialAirLwStart |  |  |  |
| `0x0E5` | SpecialAirLwLoop |  |  |  |

**Jigglypuff** (16 states, `0x0DC`-`0x0EB`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack100Start (unused) |  | `0x0E4` | AppearR |
| `0x0DD` | Attack100Loop (unused) |  | `0x0E5` | AppearL |
| `0x0DE` | Attack100End (unused) |  | `0x0E6` | SpecialN |
| `0x0DF` | JumpAerialF1 |  | `0x0E7` | SpecialAirN |
| `0x0E0` | JumpAerialF2 |  | `0x0E8` | SpecialHi |
| `0x0E1` | JumpAerialF3 |  | `0x0E9` | SpecialAirHi |
| `0x0E2` | JumpAerialF4 |  | `0x0EA` | SpecialLw |
| `0x0E3` | JumpAerialF5 |  | `0x0EB` | SpecialAirLw |

**Ness** (25 states, `0x0DC`-`0x0F4`):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack13 (jab-combo continuation, not a special) |  | `0x0E9` | SpecialAirHiHold |
| `0x0DD` | AppearRStart |  | `0x0EA` | SpecialAirHiEnd |
| `0x0DE` | AppearLStart |  | `0x0EB` | SpecialAirHiBound |
| `0x0DF` | AppearWait |  | `0x0EC` | SpecialAirHiJibaku |
| `0x0E0` | AppearREnd |  | `0x0ED` | SpecialLwStart |
| `0x0E1` | AppearLEnd |  | `0x0EE` | SpecialLwHold |
| `0x0E2` | SpecialN |  | `0x0EF` | SpecialLwHit |
| `0x0E3` | SpecialAirN |  | `0x0F0` | SpecialLwEnd |
| `0x0E4` | SpecialHiStart |  | `0x0F1` | SpecialAirLwStart |
| `0x0E5` | SpecialHiHold |  | `0x0F2` | SpecialAirLwHold |
| `0x0E6` | SpecialHiEnd |  | `0x0F3` | SpecialAirLwHit |
| `0x0E7` | SpecialHiJibaku |  | `0x0F4` | SpecialAirLwEnd |
| `0x0E8` | SpecialAirHiStart |  |  |  |

**Kirby** (83 states, `0x0DC`-`0x12E`) — by far the largest, since it includes a full duplicate move-set slot for every character whose neutral-B he can copy. `Purin` is Jigglypuff's Japanese name, used verbatim in the decompilation's own symbol names. There is no `CopyKirbySpecialN` and no Copy-DK-SpecialHi/Lw slots, since only neutral-B is copyable — matches the decompilation's own naming exactly, nothing inferred:

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x0DC` | Attack100Start (jab-combo continuation, not a special) |  | `0x101` | SpecialHiLanding |
| `0x0DD` | Attack100Loop (jab-combo continuation, not a special) |  | `0x102` | SpecialAirHi |
| `0x0DE` | Attack100End (jab-combo continuation, not a special) |  | `0x103` | SpecialAirHiFall |
| `0x0DF` | JumpAerialF1 |  | `0x104` | SpecialLwStart |
| `0x0E0` | JumpAerialF2 |  | `0x105` | SpecialLwUnk |
| `0x0E1` | JumpAerialF3 |  | `0x106` | SpecialLwHold |
| `0x0E2` | JumpAerialF4 |  | `0x107` | SpecialLwEnd |
| `0x0E3` | JumpAerialF5 |  | `0x108` | SpecialAirLwStart |
| `0x0E4` | ThrowF |  | `0x109` | SpecialAirLwHold |
| `0x0E5` | ThrowFFall |  | `0x10A` | SpecialAirLwLanding |
| `0x0E6` | ThrowFLanding |  | `0x10B` | SpecialAirLwFall |
| `0x0E7` | CopyMarioSpecialN |  | `0x10C` | SpecialAirLwEnd |
| `0x0E8` | CopyMarioSpecialAirN |  | `0x10D` | SpecialNStart |
| `0x0E9` | CopyLuigiSpecialN |  | `0x10E` | SpecialNLoop |
| `0x0EA` | CopyLuigiSpecialAirN |  | `0x10F` | SpecialNEnd |
| `0x0EB` | CopyFoxSpecialN |  | `0x110` | SpecialNCatch |
| `0x0EC` | CopyFoxSpecialAirN |  | `0x111` | SpecialNEat |
| `0x0ED` | CopySamusSpecialNStart |  | `0x112` | SpecialNThrow |
| `0x0EE` | CopySamusSpecialNLoop |  | `0x113` | SpecialNWait |
| `0x0EF` | CopySamusSpecialNEnd |  | `0x114` | SpecialNTurn |
| `0x0F0` | CopySamusSpecialAirNStart |  | `0x115` | SpecialNCopy |
| `0x0F1` | CopySamusSpecialAirNEnd |  | `0x116` | SpecialAirNStart |
| `0x0F2` | CopyDonkeySpecialNStart |  | `0x117` | SpecialAirNLoop |
| `0x0F3` | CopyDonkeySpecialAirNStart |  | `0x118` | SpecialAirNEnd |
| `0x0F4` | CopyDonkeySpecialNLoop |  | `0x119` | SpecialAirNCatch |
| `0x0F5` | CopyDonkeySpecialAirNLoop |  | `0x11A` | SpecialAirNEat |
| `0x0F6` | CopyDonkeySpecialNEnd |  | `0x11B` | SpecialAirNThrow |
| `0x0F7` | CopyDonkeySpecialAirNEnd |  | `0x11C` | SpecialAirNWait |
| `0x0F8` | CopyDonkeySpecialNFull |  | `0x11D` | SpecialAirNTurn |
| `0x0F9` | CopyDonkeySpecialAirNFull |  | `0x11E` | SpecialAirNCopy |
| `0x0FA` | AppearR |  | `0x11F` | CopyLinkSpecialN |
| `0x0FB` | AppearL |  | `0x120` | CopyLinkSpecialNGet |
| `0x0FC` | CopyPikachuSpecialN |  | `0x121` | CopyLinkSpecialNEmpty |
| `0x0FD` | CopyPikachuSpecialAirN |  | `0x122` | CopyLinkSpecialAirN |
| `0x0FE` | CopyNessSpecialN |  | `0x123` | CopyLinkSpecialAirNReturn |
| `0x0FF` | CopyNessSpecialAirN |  | `0x124` | CopyLinkSpecialAirNEmpty |
| `0x100` | SpecialHi |  | `0x125` | CopyPurinSpecialN |
|  |  |  | `0x126` | CopyPurinSpecialAirN |
|  |  |  | `0x127` | CopyCaptainSpecialN |
|  |  |  | `0x128` | CopyCaptainSpecialAirN |
|  |  |  | `0x129` | CopyYoshiSpecialN |
|  |  |  | `0x12A` | CopyYoshiSpecialNCatch |
|  |  |  | `0x12B` | CopyYoshiSpecialNRelease |
|  |  |  | `0x12C` | CopyYoshiSpecialAirN |
|  |  |  | `0x12D` | CopyYoshiSpecialAirNCatch |
|  |  |  | `0x12E` | CopyYoshiSpecialAirNRelease |

### 8.4 Controller button bits (`InputFrame.buttons`)

| Bit | Button |  | Bit | Button |
| --: | --- | --- | --: | --- |
| `0x8000` | A |  | `0x0100` | D-Right |
| `0x4000` | B |  | `0x0020` | L |
| `0x2000` | Z |  | `0x0010` | R |
| `0x1000` | Start |  | `0x0008` | C-Up |
| `0x0800` | D-Up |  | `0x0004` | C-Down |
| `0x0400` | D-Down |  | `0x0002` | C-Left |
| `0x0200` | D-Left |  | `0x0001` | C-Right |

### 8.5 `game_status` (internal, not directly exposed as an event field)

Governs the recorder's own state machine (not written to the file directly, but explains the `frame` counter's start point and `MatchEnd.endReason`): `0` pre-match countdown, `1` ongoing (this is the only state that produces `InputFrame`/`StateFrame` events, and `frame == 0` is the first frame this state is observed), `2` paused, `5` ended.

### 8.6 `ItemUpdate.linkId` and `.kind` (`smash64` family)

`linkId` (`ItemUpdate` offset `0x08`) is `4` (Item) or `5` (Weapon) — this recorder never emits any other value. It selects which of the two enums below `kind` is a value from.

**`WPKind`** (`linkId == 5` — free-flying character special-move projectiles):

| Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- |
| `0x00` | Fireball |  | `0x10` | BulletNormal |
| `0x01` | Blaster |  | `0x11` | BulletHard |
| `0x02` | ChargeShot |  | `0x12` | ArwingLaser2D |
| `0x03` | SamusBomb |  | `0x13` | ArwingLaser3D |
| `0x04` | Cutter |  | `0x14` | LGunAmmo |
| `0x05` | EggThrow |  | `0x15` | FFlowerFlame |
| `0x06` | YoshiStar |  | `0x16` | StarRodStar |
| `0x07` | Boomerang |  | `0x17`-`0x1F` | Pokémon/monster weapons (not individually enumerated here) |
| `0x08` | SpinAttack |  |  |  |
| `0x09` | ThunderJoltAir |  |  |  |
| `0x0A` | ThunderJoltGround |  |  |  |
| `0x0B` | ThunderHead |  |  |  |
| `0x0C` | ThunderTrail |  |  |  |
| `0x0D` | PKFire |  |  |  |
| `0x0E` | PKThunderHead |  |  |  |
| `0x0F` | PKThunderTrail |  |  |  |

`0x1F` is `nWPKindMonsterEnd` — Remix's own mod-added weapon IDs continue numbering from there.

**`ITKind`** (`linkId == 4` — thrown/spawned items, stage hazard objects, and some fighter-held things like Link's pulled bomb):

| Value | Name |  | Value | Name |  | Value | Name |
| --: | --- | --- | --: | --- | --- | --: | --- |
| `0x00` | Crate |  | `0x0F` | Bob-omb |  | `0x1E` | Venusaur |
| `0x01` | Barrel |  | `0x10` | Bumper |  | `0x1F` | Porygon |
| `0x02` | Capsule |  | `0x11` | GreenShell |  | `0x20` | Onix |
| `0x03` | Egg |  | `0x12` | RedShell |  | `0x21` | Snorlax |
| `0x04` | MaximTomato |  | `0x13` | Pokéball |  | `0x22` | Goldeen |
| `0x05` | Heart |  | `0x14` | PKFirePillar |  | `0x23` | Meowth |
| `0x06` | Star |  | `0x15` | Bomb |  | `0x24` | Charizard |
| `0x07` | BeamSword |  | `0x16` | PowBlock |  | `0x25` | Beedrill |
| `0x08` | HomeRunBat |  | `0x17` | Bumper (stage) |  | `0x26` | Blastoise |
| `0x09` | Fan |  | `0x18` | PiranhaPlant |  | `0x27` | Chansey |
| `0x0A` | StarRod |  | `0x19` | Target |  | `0x28` | Starmie |
| `0x0B` | RayGun |  | `0x1A` | RTTFBomb |  | `0x29` | Hitmonlee |
| `0x0C` | FireFlower |  | `0x1B` | Chansey (stage) |  | `0x2A` | Koffing |
| `0x0D` | Hammer |  | `0x1C` | Electrode |  | `0x2B` | Clefairy |
| `0x0E` | MotionSensorBomb |  | `0x1D` | Charmander |  | `0x2C` | Mew |

Two entries repeat a name at a different value (`Bumper` at both `0x10` and `0x17`; `Chansey` at both `0x1B` and `0x27`) — that's the source enum's own structure, not a transcription error. Remix's `BOWSER_BOMB` is a custom out-of-range value (`0x011A`) specific to one stage hazard, not part of the normal `0x00`-`0x2C` span.

### 8.7 `StageHazardUpdate.hazardFlags` bits (`smash64` family)

| Bit | Meaning |
| --: | --- |
| `0x01` | Whispy Woods is currently blowing (Dream Land only). |
| `0x02` | Wind direction: `0` = blowing left, `1` = blowing right. Only meaningful when bit `0x01` is also set. |

All other bits are currently always `0`, reserved for hazards not yet tracked (Zebes' rising acid, Duel Zone's disappearing platforms, …).

## 9. Known limitations / not yet implemented

- **Hitbox/hurtbox tracking is deliberately not included.** Per-frame hitbox and hurtbox geometry is not recorded. It can be added later as a new event type if there's a concrete need.
- **Item/weapon tracking has no velocity, damage, owner/attacker port, or expiration data.** `ItemUpdate` (§5.3) records a named `kind`, position and render scale for every live, non-held Item/Weapon object, but none of those additional fields have been mapped in memory yet.
- **Stage hazard tracking covers exactly one hazard.** `StageHazardUpdate` (§5.4) currently only tracks Whispy Woods' wind on Dream Land.
- **Most of Smash Remix's settings surface beyond Gameplay/Stage Settings still isn't captured.** `MatchSettings` (§5.1) now records `rngSeed` plus all 31 Gameplay Settings and 8 (core) Stage Settings, but Remix also has Music Settings, Player Tags, and ~18 named plus ~170 auto-generated per-stage/per-track random-pool toggles that are deliberately out of scope for now — none of those are mapped to memory or captured here yet. This is a mapping gap to close via ordinary field-appends as/if a concrete need for them comes up, not a structural limitation of `MatchSettings` itself.
- **No aggregate damage-dealt/taken breakdown or incoming-damage-this-hit field**, even though the emulator exposes them.
- **`MatchEnd.endReason` cannot currently distinguish time-out from stock-out, or either from a "no contest"/Salty Runback-triggered end** — all three collapse to `1` ("normal end"). Salty Runback (holding Start on the no-contest/results screen to instantly restart, bypassing the normal win/loss flow) is at least correctly recognized as its own match boundary (§5.1's determinism note doesn't apply across it). **Known gap**: two runbacks triggered back-to-back, with no non-runback match end between them, may not be recorded as separate matches.
- **Character-specific action states (`>= 0x0DC`) are only documented for the original 12 characters** (§8.3.1). This is a gap in this document, not in the format: the values are recorded for every character, but Remix-added characters (Bowser, Marth, Sonic, Ganondorf, Wolf, Mewtwo, Dr. Mario/Luigi, Dark Samus, and so on) don't have a named table here yet, so a reader should show a generic label (e.g. `Special 0x0DC`).
- **Remix-specific stage IDs (`0x29`+) are not enumerated** in §8.2. This is a gap in this document, not in the format: the stage ID is recorded for every match.
- **No ROM-identity check for the extension layer.** Nothing currently verifies the loaded ROM matches `gameFamily`'s expectations beyond `goodName` comparison; a mismatch would produce an extension layer full of garbage bytes rather than falling back to core-only recording. This is a correctness gap to close during implementation, not an accepted limitation of the design itself.
- **No live reading.** The file is written only at match end, so nothing can read a match in progress (spectating, stream overlays), and a crash leaves no file. `.slp` supports live reading (§2.2). Adding it would mean writing events as they happen with the header's length fields left at `0` until the end; dropping compression, or flushing the deflate stream periodically so a reader can decompress what is already written; and adding an end-of-frame marker event. A crash would then leave a truncated file, so readers would need rules for handling one.
- **RNG seed timing relative to frame `0` is not pinned down.** `MatchSettings`, and its `rngSeed`, may be captured during the pre-match countdown (§5.1), but `InputFrame` events start only once the match is ongoing. If the RNG advances during those unrecorded frames, replaying from the seed alone may not reproduce the match. `.slp` avoids this by recording the seed at the start of every frame.
- **No attacker attribution.** `StateFrame` (§5.2) does not record who last hit a fighter or with which move, so damage and KOs can't be credited to an attacker. `.slp` records both (Last Hit By, Last Hitting Attack ID).
- **No stable player identity, and no stated name encoding.** `playerNames` (§4.1) holds a display name only, with no persistent ID like Slippi's connect code, and its character encoding is not yet specified.
- **Only processed inputs are recorded.** `InputFrame` (§4.2) carries the processed buttons and stick position; physical button state and raw stick values, which `.slp` also records, are not captured.
- **Netplay lag and rollback corrections are not recorded.** Each frame reflects the game's final, canonical state, not necessarily what a player experienced.
