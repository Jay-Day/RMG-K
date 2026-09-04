# RMG-K Replay File Format (`.rmgr`) — Specification

**Status:** format version `5`. This is the first time the `.rmgr` format
has been formally specified — this document did not exist before this
branch, and it has not yet been shared with the upstream RMG-K
maintainers. **The writer (`Source/RMG-Core/Replay.cpp` /
`Source/RMG-Core/ReplayMemory.cpp`) implements this version.** The reader
side (`rmgr-ts`, `rmgr-viewer`) has not been updated yet — see §10.

`version` starts at `5`, not `1`: this fork already produced real files
under that earlier in-development numbering while this format was being
prototyped, and starting past it avoids a reader mistaking one of those
for a valid file under this spec. There is no migration path from
anything recorded before this document, and none is planned.

## 1. Overview

A `.rmgr` file is a self-contained binary recording of one N64 match. It
is designed for two distinct consumers:

1. **Best-effort deterministic replay** — the recorded controller inputs
   are enough to re-simulate the match's player-driven events from
   scratch, but this is not full frame-perfect determinism: no RNG seed is
   captured (§9), so any RNG-dependent outcome (item spawn rolls, certain
   move variance, …) can diverge from the original match on replay.
2. **Direct analysis** — recorded game state (position, damage, stocks,
   action state, …) is enough to build stats, visualizations, or search
   tooling *without* re-running the emulator at all.

Two things distinguish this format from a plain input-recording format
like `.krec`:

- **It's split into two layers.** A small **core** event set (player
  names, port slot types, controller inputs, match end) is always recorded
  for *any* N64 title, recognized or not — this layer alone makes a
  `.rmgr` file at least as useful as a `.krec` recording. A second,
  **game-family-specific extension** event set (position, damage, stocks,
  items, stage hazards, …) is only recorded when the loaded ROM's game
  family is recognized by this recorder. A different, unrecognized N64
  game still produces a valid, useful, input-only `.rmgr` file; a
  recognized game gets the full analysis layer on top. See §2.
- **The event stream is compressed.** Per-frame data is highly redundant
  frame-to-frame, so the entire event stream (everything after the fixed
  header) is deflate-compressed as a single block, written once when the
  match ends. See §3.4.

The design is modeled on [Slippi's `.slp` format](https://github.com/project-slippi/slippi-wiki/blob/master/SPEC.md)
(Super Smash Bros. Melee / Dolphin) — a self-describing binary event stream
— but is **not** byte-compatible with it, and deliberately drops or changes
several of Slippi's choices.

**Target platform:** N64 titles specifically (the container format is not
attempting to be console-agnostic) — see §2 for how it generalizes across
*different* N64 titles rather than different consoles.

## 2. Design goals

- **Buffered, then written once.** The recorder buffers the entire match's
  events in memory as it plays and writes the file in a single pass —
  header, then the compressed event-stream blob — only once the match
  ends. Every header field (including both length fields) is known before
  the first byte is written; there is no seek-back-and-patch step.
  **Trade-off, accepted deliberately:** a crash or force-quit mid-match
  produces no file at all, not a truncated one (a rejected alternative
  design streamed events to disk incrementally specifically to avoid this,
  at the cost of the seek-and-patch mechanics that made compression
  awkward). A match's event data is at most a few MB, so holding it in RAM
  for a match's duration is not a real memory concern.
- **Self-describing, forward-compatible event stream, no schema compiler.**
  Every event is a 1-byte command code followed by a payload whose size was
  declared up front by the very first event in the stream (`EventPayloads`,
  §5.1). A parser that doesn't recognize a command code can still skip it
  correctly and keep reading — the file can grow new event types or new
  trailing fields on existing events without breaking old parsers, and an
  old file's smaller payload sizes are exactly as valid to a new parser.
  This mechanism is already game-agnostic on its own — what's new to this
  spec is *which* events exist and which layer (§2.1) each belongs to, not
  the mechanism itself.
- **Core vs. game-family-extension event layering.** See §2.1.
- **No FlatBuffers, no UBJSON, no schema-compiler toolchain.** FlatBuffers'
  benefits don't survive being wrapped per-event in a growing append-only
  stream, and this format skips the JSON-like-wrapper problem entirely by
  not having one — the decompressed stream *is* the binary event data.
- **Little-endian**, matching the host platforms this project targets.
  All multi-byte integer and float fields in this document are
  little-endian unless stated otherwise.
- **Native struct layout, no bit-packing.** Every event payload is a
  fixed-size, `#pragma pack(push, 1)` C struct with no padding and no
  bitfields — the byte layout tables below are exactly `sizeof()` that
  struct, field by field, in declaration order. Space efficiency is
  achieved by compressing the resulting stream (§3.4), not by hand-rolled
  bit-packing or delta-encoding of the struct layout itself.

### 2.1 The core / game-family-extension split

Every `.rmgr` file always contains the **core** event set: `MatchStart`
(player names, port slot types), `InputFrame` (controller state, one per
seated port per frame), and `MatchEnd` (final frame count, end reason).
This is deliberately close to a `.krec`-equivalent recording — enough to
know who played, on which ports, and what they pressed, for *any* N64
title, whether or not this recorder has ever heard of it.

A file *additionally* contains a **game-family extension** event set when
`FileHeader.gameFamily` (§3.1) is non-empty — i.e. when this recorder
specifically knows how to read that title's memory layout. Right now
exactly one family is defined, `smash64` (§4), covering Smash Remix and
(in principle) vanilla SSB64. A future family (e.g. Mario Kart 64) would
define its own, entirely separate extension event set under its own
`gameFamily` string — it would never carry Smash-only fields like
`damagePercent` or `stocksRemaining`, the same way `smash64` files never
carry fields a future racing-game family would need.

`gameFamily` is a coarser identity than `goodName` (§3.2): `goodName`
pins down the exact ROM build a file came from; `gameFamily` tells a
reader which extension event *definitions* apply, without that reader
needing to maintain its own list of every `goodName` string that has ever
meant "this is Smash." A new Smash Remix build is automatically readable
by any tool that already understands `gameFamily == "smash64"`, with zero
changes to that tool, the moment its recorder starts stamping that family
string.

## 3. File structure

```
+----------------+----------------------------------------+
| File Header    | 108 bytes, fixed, uncompressed          |
+----------------+----------------------------------------+
| Event Stream   | deflate-compressed block, decompresses  |
| (compressed)   | to the sequence of events described in  |
|                | §4/§5                                    |
+----------------+----------------------------------------+
```

There is no footer, no trailing metadata block, and no UBJSON/JSON wrapper
of any kind. The header is followed immediately by exactly
`compressedLength` (§3.1) bytes of deflate-compressed data — nothing
before it, nothing after it, no framing around the compressed block itself.

### 3.1 File header (108 bytes)

| Offset | Size | Type       | Field                    | Notes |
|-------:|-----:|------------|---------------------------|-------|
| 0x00   | 4    | `char[4]`  | `magic`                   | Always the ASCII bytes `R`, `M`, `G`, `R` (no NUL). |
| 0x04   | 1    | `u8`       | `version`                 | Format version. `5` for everything described here — starts past `1` rather than at it; see the note at the top of this document for why. |
| 0x05   | 3    | `u8[3]`    | `reserved`                | Always zero. Reserved for future header fields. |
| 0x08   | 16   | `char[16]` | `gameFamily`              | NUL-padded ASCII identifier for which game-family extension event set (§2.1, §4) applies, e.g. `"smash64"`. **All-zero** (empty) if the loaded ROM's game family isn't recognized by this recorder — the file is still a fully valid core-only recording in that case, just with no extension events. Truncated if longer than 16 bytes (no defined family name is expected to need that much room). |
| 0x18   | 64   | `char[64]` | `goodName`                | The recorded ROM's `GoodName` (mupen64plus-core's ROM database identity string), UTF-8, NUL-padded — not necessarily NUL-terminated if it fills the field. Truncated if longer than 64 bytes. All-zero if the ROM database has no identity for the loaded ROM. |
| 0x58   | 4    | `u32`      | `recorderSchemaVersion`   | This recorder's revision of its own understanding of `goodName`'s memory layout — see §3.3. `0` when `gameFamily` is empty (no extension schema applies to an unrecognized game). |
| 0x5C   | 8    | `u64`      | `recordedAtEpochMillis`   | Wall-clock time the recording started, **milliseconds** since the Unix epoch (UTC) — from `std::chrono::system_clock`. Independent of the filename's timestamp (§3.3), though the recorder writes the same instant to both (truncated to whole seconds there). |
| 0x64   | 4    | `u32`      | `uncompressedLength`      | Byte length of the event stream *after* decompression. Lets a reader preallocate its decompression output buffer instead of growing it dynamically. |
| 0x68   | 4    | `u32`      | `compressedLength`        | Byte length of the deflate-compressed block immediately following the header — i.e. `header_size (108) + compressedLength` is the total file size. Always correct on disk: because the whole match is buffered before anything is written (§2), there is no "0 until finalized" convention, and no truncated-but-valid file is possible — see §2's buffering trade-off. |

### 3.2 `goodName`, `gameFamily`, and `recorderSchemaVersion` — three independent axes

Three fields, three different questions, and conflating any two of them is
the mistake to avoid:

- **`gameFamily`** identifies *which extension event definitions apply* —
  a coarse, slow-growing identifier (§2.1). Empty means "core events only,
  no extension layer, whatever this ROM is isn't recognized."
- **`goodName`** identifies *which exact ROM build* produced the file — a
  unique identity within a family, not the family itself. `SmashRemix2.0.1`
  and a hypothetical `SmashRemix2.0.2` are different `goodName`s, both
  under `gameFamily = "smash64"`, even though they're the "same" mod,
  because their memory layouts can differ build to build.
- **`recorderSchemaVersion`** identifies *which revision of this
  recorder's interpretation* of that specific `goodName`'s memory layout
  produced the file. It's bumped whenever that interpretation changes in a
  way that affects recorded output — which includes but isn't limited to
  adding a new field. A fix to an *existing* field's offset (silently
  changing recorded *values* without changing any event's declared byte
  size) needs a bump too, since the per-event `EventPayloads`
  declared-size mechanism (§6) has no way to signal that on its own.

`recorderSchemaVersion` is its own counter **per `goodName`**, not global
and not per-`gameFamily`: `SmashRemix2.0.1` schema `3` and
`SmashRemix2.0.2` schema `1` are unrelated numbering spaces, each starting
fresh at `1` for that `goodName`'s first supported revision. This is
deliberately how one shared `gameFamily` accommodates ROM builds with
different field sets — e.g. Smash Remix's extra settings (teams, handicap,
CPU level, item frequency, a wider character-ID range) that a
hypothetical vanilla-SSB64 recorder wouldn't have are just later-schema
field-appends (§6) on the shared `smash64` extension events. A
vanilla-only reader simply never advances far enough in schema version to
know about Remix-only trailing fields, and the per-file `EventPayloads`
declared size means it never misreads them either.

A reader that only knows how to interpret one specific `(gameFamily,
goodName, recorderSchemaVersion)` combination should check all three
explicitly before trusting an extension event's semantics, rather than
assuming every file it can open was produced by the same recorder
revision it was built against. Core events (§4.1-§4.3) need none of this
— they're valid to read regardless of `gameFamily`/`goodName`.

### 3.3 Filename convention

Not part of the on-disk format itself (a reader must not depend on it —
the header's own `recordedAtEpochMillis` is the source of truth for when a
recording started), but the recorder names files
`YYYYMMDD-HHMMSS[-Player1][-Player2]...rmgr` — 4-digit year, 24-hour clock,
local time, one hyphen-joined segment per seated player's name (each
sourced from `MatchStart.playerNames`, §4.1, capped at 24 characters,
filesystem-unsafe characters replaced with `_`). The timestamp reflects the
same instant written to `recordedAtEpochMillis`, just truncated to whole
seconds and rendered as local wall-clock time instead of a UTC epoch
value.

If that name is already taken (e.g. two matches recorded within the same
second), the recorder appends `-2`, `-3`, ... before the extension until it
finds a free name, rather than overwriting an existing file.

Headless export of an existing `.krec` (see `Replay::SetOutputPathOverride()`)
uses a different, caller-chosen base name instead — by convention
`<krec name>.rmgr`, the same stem as the source `.krec` it was exported
from. Each match within that `.krec` gets its own explicitly-numbered file:
`<krec name>-1.rmgr`, `<krec name>-2.rmgr`, ... The same collision-avoidance
above still applies on top.

For this headless export path, `recordedAtEpochMillis` is derived from the
source `.krec`'s own recording-start timestamp plus elapsed frames
converted to milliseconds under a constant-60fps assumption; see
`Replay::SetRecordedAtBaseOverride()`.

### 3.4 Compression

Everything after the 112-byte header is one deflate-compressed block,
covering the entire event stream (§4/§5) — `EventPayloads` through the
final `MatchEnd`/`MatchResult`. Compressed at the highest available
deflate level; this only costs CPU time once, at match end, not per frame,
since the recorder buffers events in memory during the match (§2) rather
than compressing incrementally.

The header itself is **not** compressed — `magic`, `version`, `gameFamily`,
`goodName`, and the timestamp fields are readable by any tool without
decompressing anything, which matters for a file browser or library UI
that wants to list many recordings' identity/timestamp without paying the
cost of decompressing each one's (much larger) event stream.

A reader decompresses exactly `compressedLength` bytes starting at file
offset `112` (`0x70`) and should get back exactly `uncompressedLength`
bytes of event-stream data (§4/§5) to parse as described below. There is
no partial-file/truncated-recording case to handle for the event stream
itself: because the whole match is buffered before any of this is written
(§2), a `.rmgr` file on disk is either the complete, valid output of a
match that reached `MatchEnd`, or the file doesn't exist at all.

## 4. Core events

Always present in every file, regardless of `gameFamily`.

### 4.1 Match Start — code `0x02`

Written exactly once, as the first event after `EventPayloads`. Player
display names are sourced from netplay room metadata (RMG-K's own
slot-indexed name table, populated by every netplay path), never from any
in-game name tag — for an offline match, or a port with no assigned name,
the corresponding `playerNames` entry is all zero bytes.

Payload size: **132 bytes.**

| Offset | Size | Type        | Field         | Notes |
|-------:|-----:|-------------|---------------|-------|
| 0x00   | 128  | `char[4][32]` | `playerNames` | 4× a 32-byte, NUL-padded (not necessarily NUL-terminated if exactly 32 chars) name string, port 0-3 in order. |
| 0x80   | 4    | `u8[4]`     | `slotType`    | Per port 0-3: `0` human, `1` CPU, `2` empty. |

Game-family-specific match settings (stage, character, stock count,
damage ratio, items, teams, handicap, CPU difficulty, …) are **not** part
of this event — see `MatchSettings` (§5.1) for the `smash64` extension
equivalent. A game-agnostic reader (or a recognized-but-not-yet-supported
title) has everything it needs from `MatchStart`/`InputFrame`/`MatchEnd`
alone to reconstruct who played what inputs, on which ports.

### 4.2 Input Frame — code `0x03`

Input-side data, captured **before** the game processes that frame's
inputs. One event per seated port per frame. Uses the game's
already-processed button/stick values, which is the one input
representation available uniformly for **both** human and CPU-controlled
ports.

Named `InputFrame` rather than "Pre-Frame" since this event has no
game-family-specific counterpart to be "pre-" relative to — it's the
entire core per-frame input record, full stop.

Payload size: **9 bytes.**

| Offset | Size | Type   | Field     | Notes |
|-------:|-----:|--------|-----------|-------|
| 0x00   | 4    | `i32`  | `frame`   | Frame counter, `0` at the first frame this match's recording enters the "ongoing" game state. Monotonically increasing, one recorded frame per real emulated frame. |
| 0x04   | 1    | `u8`   | `port`    | `0`-`3`. |
| 0x05   | 2    | `u16`  | `buttons` | Processed button bitmask. See §7.4. |
| 0x07   | 1    | `i8`   | `stickX`  | Processed stick X, signed. |
| 0x08   | 1    | `i8`   | `stickY`  | Processed stick Y, signed. |

### 4.3 Match End — code `0x05`

Written exactly once, as the last event in the stream.

Payload size: **5 bytes.**

| Offset | Size | Type   | Field        | Notes |
|-------:|-----:|--------|--------------|-------|
| 0x00   | 4    | `i32`  | `finalFrame` | The last `frame` value seen in any `InputFrame` event this match — total recorded match length. |
| 0x04   | 1    | `u8`   | `endReason`  | `0` aborted (match reset, or the emulator/process stopped mid-match — these two causes are not currently distinguished), `1` normal end. |

Final per-port results (e.g. Smash's stocks-remaining placements) are not
a universal concept across N64 titles and are **not** part of this event
— see `MatchResult` (§5.4) for the `smash64` extension equivalent.

## 5. `smash64` game-family extension events

Present only when `FileHeader.gameFamily == "smash64"`. Covers Smash Remix
today; the field set is written to also accommodate a hypothetical vanilla
SSB64 recorder via `recorderSchemaVersion`-scoped field-appends (§3.2) —
none of that is implemented yet, but the layering doesn't assume Remix
specifically.

### 5.0 Event Payloads — code `0x01`

Always the first event in the decompressed stream. Declares the exact
payload size (not including the 1-byte command code) of every other event
type this file uses — the entire forward-compatibility mechanism a parser
relies on to skip codes it doesn't recognize.

| Offset | Size    | Type      | Field         | Notes |
|-------:|--------:|-----------|---------------|-------|
| 0x00   | 1       | `u8`      | `count`       | Number of `(code, size)` entries that follow. |
| 0x01   | 3×count | see below | `entries`     | `count` repetitions of the 3-byte entry below. |

Each entry:

| Offset (rel.) | Size | Type  | Field  | Notes |
|---------------:|-----:|-------|--------|-------|
| +0x00          | 1    | `u8`  | `code` | The event command code this entry describes. |
| +0x01          | 2    | `u16` | `size` | That event's payload size, in bytes. |

A file with `gameFamily` empty declares only the three core codes
(`MatchStart`, `InputFrame`, `MatchEnd`). A `smash64` file declares those
three plus the five below. **A parser must always read an event's size
from that file's own `EventPayloads` event, never hardcode it, and must
always read `count` itself rather than assuming a fixed number of
entries** — this is exactly why a reader that doesn't recognize a
`gameFamily` (or an older reader facing a newer schema's appended fields)
can still parse the rest of a file correctly.

### 5.1 Match Settings — code `0x08`

Written exactly once, immediately after `MatchStart`. Everything
Smash-specific and static for the whole match. If a port's characters
aren't yet spawned when this is captured (e.g. written during the
pre-match countdown), that port's `characterId`/`costumeId`/`teamColor`/
`portTeam`/`portHandicap`/`portCpuLevel` entries are left at `0` rather
than the real value — a reader can't distinguish "genuinely 0" from "not
available yet" for those alone.

Payload size: **32 bytes.**

| Offset | Size | Type    | Field                | Notes |
|-------:|-----:|---------|-----------------------|-------|
| 0x00   | 1    | `u8`    | `stageId`             | See §7.2. |
| 0x01   | 1    | `u8`    | `gameType`            | `1` time, `2` stock, `3` both (Remix always forces stock). |
| 0x02   | 1    | `u8`    | `stockCountSetting`   | 0-based (i.e. `2` means "3 stocks"). |
| 0x03   | 1    | `u8`    | `timeLimitMinutes`    | `100` = infinite. |
| 0x04   | 1    | `u8`    | `damageRatio`         | `50` = 50%, `200` = 200%. |
| 0x05   | 1    | `u8`    | `itemFrequency`       | `0` none .. `5` high. |
| 0x06   | 1    | `u8`    | `teamsEnabled`        | `0` off, `1` on. |
| 0x07   | 1    | `u8`    | `handicapMode`        | `0` off, `1` on, `2` auto. |
| 0x08   | 4    | `u8[4]` | `characterId`         | Per port 0-3. See §7.1. Meaningless for a port whose `MatchStart.slotType == 2` (empty). |
| 0x0C   | 4    | `u8[4]` | `costumeId`           | Per port 0-3. |
| 0x10   | 4    | `u8[4]` | `teamColor`           | Per port 0-3. |
| 0x14   | 4    | `u8[4]` | `portTeam`            | Team number per port 0-3. |
| 0x18   | 4    | `u8[4]` | `portHandicap`        | Per-port handicap value, meaningful only when `handicapMode != 0`. |
| 0x1C   | 4    | `u8[4]` | `portCpuLevel`        | CPU difficulty per port; meaningless for a `human` port. |

### 5.2 State Frame — code `0x04`

State-side data, captured **after** that frame's physics/collision
resolution — the resulting state. One event per seated port per frame,
always immediately following that port's `InputFrame` in the stream.

Named `StateFrame` for the same reason as `InputFrame` above — it's paired
with the core input event by convention/ordering, not by a "Pre/Post"
naming relationship baked into the format itself.

Payload size: **50 bytes.**

| Offset | Size | Type   | Field                | Notes |
|-------:|-----:|--------|------------------------|-------|
| 0x00   | 4    | `i32`  | `frame`                | Same frame counter as the paired `InputFrame`. |
| 0x04   | 1    | `u8`   | `port`                 | `0`-`3`. |
| 0x05   | 1    | `u8`   | `characterId`           | See §7.1. |
| 0x06   | 2    | `u16`  | `actionStateId`         | See §7.3. |
| 0x08   | 4    | `f32`  | `positionX`             | IEEE-754 single precision. |
| 0x0C   | 4    | `f32`  | `positionY`             | |
| 0x10   | 4    | `i32`  | `facingDirection`       | `1` = facing right, `-1` = facing left. |
| 0x14   | 4    | `f32`  | `velocityX`             | |
| 0x18   | 4    | `f32`  | `velocityY`             | |
| 0x1C   | 4    | `u32`  | `damagePercent`         | Whole-number percent, as the game itself stores it (not a float). |
| 0x20   | 1    | `i8`   | `stocksRemaining`       | 0-based; negative once eliminated. |
| 0x21   | 1    | `u8`   | `jumpsRemaining`        | `jumpsMax` (per-character) minus `jumps_used`, which resets to `0` on landing. `0` through most of a grounded match is normal. |
| 0x22   | 1    | `u8`   | `groundedState`         | `0` grounded, `1` airborne. |
| 0x23   | 1    | `u8`   | `hurtboxState`          | `0x03` = intangible/invincible; see `ReplayMemory.cpp` for the full set observed. |
| 0x24   | 2    | `u16`  | `hitstunCounter`        | Non-zero while in hitstun. |
| 0x26   | 4    | `u32`  | `actionFrameCounter`    | Frame counter of the current action state (resets when the action state changes). |
| 0x2A   | 4    | `u32`  | `comboHitCount`         | Belongs to the *victim* (this port), not the attacker: hits taken in the current unbroken chain. `0` = no active chain, `1` = a single hit, `2+` = an actual combo. |
| 0x2E   | 4    | `u32`  | `comboDamage`           | Running damage dealt within the same chain as `comboHitCount`; zeroes at the same instant. |

### 5.3 Item Update — code `0x06`

Zero or more per frame — one per live Item or Weapon `GObj` currently not
held by a fighter, following that frame's `InputFrame`/`StateFrame` pairs.
Items and Weapons live on two separate `GObj` lists (`gGCCommonLinks[4]`
Item, `gGCCommonLinks[5]` Weapon), not one shared list. A held item (e.g.
Link's bomb while still in his hand) is not emitted: while held, its
position reads as a meaningless local offset near `(0,0,0)` instead of a
world coordinate, and that's used as the "currently held" proxy.

Payload size: **25 bytes.**

| Offset | Size | Type    | Field           | Notes |
|-------:|-----:|---------|------------------|-------|
| 0x00   | 4    | `i32`   | `frame`          | Same frame counter as that frame's `InputFrame`/`StateFrame`. |
| 0x04   | 4    | `u32`   | `objectAddress`  | The `GObj`'s own RDRAM address — the closest available stable per-object identity, valid for as long as that object is alive. Not a semantic spawn ID; the address can be reused once an object is freed. |
| 0x08   | 1    | `u8`    | `linkId`         | `4` = Item, `5` = Weapon — which enum `kind` below means. See §7.6. |
| 0x09   | 4    | `i32`   | `kind`           | `ITKind` (`linkId == 4`) or `WPKind` (`linkId == 5`) — the real, named per-instance type. Full enums in §7.6. |
| 0x0D   | 4    | `f32`   | `positionX`      | IEEE-754 single precision. World-space. |
| 0x11   | 4    | `f32`   | `positionY`      | |
| 0x15   | 4    | `f32`   | `positionZ`      | |

Deliberately not captured (not yet mapped in memory): velocity,
damage/knockback dealt, size, owner/attacker port, and any per-object
timer/expiration. A future schema version can append any of these as
trailing fields (§6) once mapped, without breaking this version's readers.

### 5.4 Stage Hazard Update — code `0x07`

Zero or one per frame, following that frame's `ItemUpdate` events —
written only when at least one tracked hazard is currently active, same
sparse convention as `ItemUpdate` (never a zeroed/placeholder event for
"nothing active"). Currently tracks exactly one hazard: Whispy Woods' wind
on Dream Land. More hazards (Zebes' rising acid, Duel Zone's disappearing
platforms, …) can claim more bits in `hazardFlags` later via the
field-append mechanism (§6), without needing a new event type.

Payload size: **5 bytes.**

| Offset | Size | Type  | Field         | Notes |
|-------:|-----:|-------|----------------|-------|
| 0x00   | 4    | `i32` | `frame`        | Same frame counter as that frame's `InputFrame`/`StateFrame`. |
| 0x04   | 1    | `u8`  | `hazardFlags`  | Bitmask. Bit `0x01`: Whispy Woods currently blowing (Dream Land only). Bit `0x02`: wind direction — `0` = blowing left, `1` = blowing right; only meaningful when bit `0x01` is also set. |

### 5.5 Match Result — code `0x09`

Written exactly once, as the last event in the stream — immediately after
the core `MatchEnd` event (§4.3).

Payload size: **4 bytes.**

| Offset | Size | Type    | Field          | Notes |
|-------:|-----:|---------|-----------------|-------|
| 0x00   | 4    | `i8[4]` | `placements`    | Final stocks remaining, per port 0-3. `-1` for any port that was never seated. |

## 6. Versioning and forward compatibility

Two independent mechanisms:

- **Field additions to an existing event:** always append new fields to the
  *end* of that event's payload, never insert in the middle. An old parser
  — which learned the event's size from that file's own `EventPayloads`
  event, which will correctly declare the *old*, smaller size for an old
  file — simply never reads the new trailing bytes. A new parser reading an
  old file sees the old, smaller declared size and correctly knows not to
  read fields that were never written.
- **New event types:** an old parser encountering a command code it doesn't
  recognize looks up its declared size in `EventPayloads` and skips exactly
  that many bytes, then continues from the next event.

`FileHeader.version` (currently `5`) is reserved for a breaking change to
the header or the overall framing itself (e.g. the header layout, or the
buffered/compressed structure of §2-§3) — not for anything the two
mechanisms above already cover, and not for tracking which game/ROM
produced a file or how that recorder's understanding of it has evolved
either — that's `gameFamily`/`goodName`/`recorderSchemaVersion` (§3.2), a
deliberately separate axis from the container format itself.

**No file recorded before this document existed is expected to parse
under this spec.** This is a from-scratch design, not a continuation of
any prior schema-history mechanism — see the note at the top of this
document.

## 7. Byte order and encoding

Everything in this file — the header and every (decompressed) event
payload — is **little-endian**. Values are read directly from emulated
memory via `DebugMemRead8/16/32` (which already normalize to host byte
order) and written to disk as raw native-layout structs on little-endian
host platforms.

Floats are IEEE-754 single precision (32-bit), stored as the exact bit
pattern the game itself holds in memory — reinterpret the 4 bytes as a
`float`/`f32`, don't scale or convert.

Strings (`playerNames` in `MatchStart`, `goodName`/`gameFamily` in the
header) are fixed-width byte arrays, NUL-padded, **not necessarily
NUL-terminated** if the value fills the full field width — always read up
to the declared field width and trim trailing NULs, never scan for a
terminator past the field boundary.

## 8. Reference tables

### 8.1 Character IDs

Valid range `0x00`-`0x60`.

**Vanilla (`0x00`-`0x1C`):**

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x00` | Mario | | `0x0E` | Polygon Mario |
| `0x01` | Fox | | `0x0F` | Polygon Fox |
| `0x02` | Donkey Kong | | `0x10` | Polygon Donkey Kong |
| `0x03` | Samus | | `0x11` | Polygon Samus |
| `0x04` | Luigi | | `0x12` | Polygon Luigi |
| `0x05` | Link | | `0x13` | Polygon Link |
| `0x06` | Yoshi | | `0x14` | Polygon Yoshi |
| `0x07` | Captain Falcon | | `0x15` | Polygon Captain Falcon |
| `0x08` | Kirby | | `0x16` | Polygon Kirby |
| `0x09` | Pikachu | | `0x17` | Polygon Pikachu |
| `0x0A` | Jigglypuff | | `0x18` | Polygon Jigglypuff |
| `0x0B` | Ness | | `0x19` | Polygon Ness |
| `0x0C` | Master Hand | | `0x1A` | Giant DK |
| `0x0D` | Metal Mario | | `0x1B` | Random |
| | | | `0x1C` | (unused) |

**Remix fighters (`0x1D`-`0x4C`):**

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x1D` | Falco | | `0x35` | Giga Bowser |
| `0x1E` | Ganondorf | | `0x36` | Piano |
| `0x1F` | Young Link | | `0x37` | Wolf |
| `0x20` | Dr. Mario | | `0x38` | Conker |
| `0x21` | Wario | | `0x39` | Mewtwo |
| `0x22` | Dark Samus | | `0x3A` | Marth |
| `0x23` | Link (EU) | | `0x3B` | Sonic |
| `0x24` | Samus (JP) | | `0x3C` | Sandbag |
| `0x25` | Ness (JP) | | `0x3D` | Super Sonic |
| `0x26` | Lucas | | `0x3E` | Sheik |
| `0x27` | Link (JP) | | `0x3F` | Marina |
| `0x28` | Falcon (JP) | | `0x40` | King Dedede |
| `0x29` | Fox (JP) | | `0x41` | Goemon |
| `0x2A` | Mario (JP) | | `0x42` | Peppy |
| `0x2B` | Luigi (JP) | | `0x43` | Slippy |
| `0x2C` | DK (JP) | | `0x44` | Banjo |
| `0x2D` | Pikachu (EU) | | `0x45` | Metal Luigi |
| `0x2E` | Jigglypuff (JP) | | `0x46` | Ebisumaru |
| `0x2F` | Jigglypuff (EU) | | `0x47` | Dragon King |
| `0x30` | Kirby (JP) | | `0x48` | Crash |
| `0x31` | Yoshi (JP) | | `0x49` | Peach |
| `0x32` | Pikachu (JP) | | `0x4A` | Roy |
| `0x33` | Samus (EU) | | `0x4B` | Dr. Luigi |
| `0x34` | Bowser | | `0x4C` | Lanky Kong |

**Remix polygons (`0x4D`-`0x60`):**

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x4D` | Wario | | `0x57` | Dark Samus |
| `0x4E` | Lucas | | `0x58` | Marth |
| `0x4F` | Bowser | | `0x59` | Mewtwo |
| `0x50` | Wolf | | `0x5A` | Dedede |
| `0x51` | Dr. Mario | | `0x5B` | Young Link |
| `0x52` | Sonic | | `0x5C` | Goemon |
| `0x53` | Sheik | | `0x5D` | Conker |
| `0x54` | Marina | | `0x5E` | Banjo |
| `0x55` | Falco | | `0x5F` | Peach |
| `0x56` | Ganondorf | | `0x60` | Crash |

### 8.2 Stage IDs

**Vanilla:**

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x00` | Peach's Castle | | `0x09` | Dream Land Beta 1 |
| `0x01` | Sector Z | | `0x0A` | Dream Land Beta 2 |
| `0x02` | Congo Jungle | | `0x0B` | How to Play |
| `0x03` | Planet Zebes | | `0x0C` | Mini Yoshi's Island |
| `0x04` | Hyrule Castle | | `0x0D` | Meta Crystal |
| `0x05` | Yoshi's Island | | `0x0E` | Duel Zone |
| `0x06` | Dream Land | | `0x0F` | Race to the Finish |
| `0x07` | Saffron City | | `0x10` | Final Destination |
| `0x08` | Mushroom Kingdom | | | |

Remix adds a very large number of additional stages (`0x29` onward, into the
`0xD0`+ range) not enumerated here — see the *Known limitations* note in §9.

### 8.3 Action state IDs

`0x000`-`0x0DB` are shared across every character; `>= 0x0DC` is
character-specific (special moves — see §9). Where a name ends in a
numbered/lettered range (e.g. `Walk1-3`), the `Value` column's range
covers that many consecutive, individually-meaningful states in that
order — not one state that happens to span multiple codes.

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x000` | DeadD (KO bottom) | | `0x054` | CliffCatch |
| `0x001` | DeadS (KO side) | | `0x055` | CliffWait |
| `0x002` | DeadU (KO top) | | `0x056` | CliffQuick |
| `0x003` | ScreenKO | | `0x057-0x058` | CliffClimbQuick1-2 |
| `0x004` | ScreenKOWait | | `0x059` | CliffSlow |
| `0x005` | Entry (spawn) | | `0x05A-0x05B` | CliffClimbSlow1-2 |
| `0x007` | Revive1 | | `0x05C-0x05F` | CliffAttack Quick/Slow |
| `0x008` | Revive2 | | `0x060-0x063` | CliffEscape Quick/Slow |
| `0x009` | ReviveWait | | `0x064-0x07D` | Item pickup/throw actions |
| `0x00A` | Idle | | `0x07E-0x097` | Item-specific attacks |
| `0x00B-0x00D` | Walk1-3 | | `0x098` | ShieldOn |
| `0x00F` | Dash | | `0x099` | Shield |
| `0x010` | Run | | `0x09A` | ShieldOff |
| `0x011` | RunBrake | | `0x09B` | ShieldStun |
| `0x012` | Turn | | `0x09C` | RollF |
| `0x013` | TurnRun | | `0x09D` | RollB |
| `0x014` | JumpSquat | | `0x09E` | ShieldBreak |
| `0x015` | ShieldJumpSquat | | `0x09F` | ShieldBreakFall |
| `0x016` | JumpF | | `0x0A0-0x0A3` | Stun land/start |
| `0x017` | JumpB | | `0x0A4` | Stun |
| `0x018` | JumpAerialF | | `0x0A5` | Sleep |
| `0x019` | JumpAerialB | | `0x0A6` | Grab |
| `0x01A` | Fall | | `0x0A7` | GrabPull |
| `0x01B` | FallAerial | | `0x0A8` | GrabWait |
| `0x01C` | Crouch | | `0x0A9` | ThrowF |
| `0x01D` | CrouchIdle | | `0x0AA` | ThrowB |
| `0x01E` | CrouchEnd | | `0x0AB-0x0B3` | Captured/inhaled/egg-laid |
| `0x01F` | LandingLight | | `0x0B5-0x0BC` | Being thrown |
| `0x020` | LandingHeavy | | `0x0BD` | Taunt |
| `0x021` | Pass (platform drop) | | `0x0BE` | Jab1 |
| `0x022` | ShieldDrop | | `0x0BF` | Jab2 |
| `0x023` | Teeter | | `0x0C0` | DashAttack |
| `0x024` | TeeterStart | | `0x0C1-0x0C5` | FTilt (High->Low) |
| `0x025-0x027` | DamageHigh1-3 | | `0x0C7` | UTilt |
| `0x028-0x02A` | DamageMid1-3 | | `0x0C9` | DTilt |
| `0x02B-0x02D` | DamageLow1-3 | | `0x0CA-0x0CE` | FSmash (High->Low) |
| `0x02E-0x030` | DamageAir1-3 | | `0x0CF` | USmash |
| `0x031-0x032` | DamageElec1-2 | | `0x0D0` | DSmash |
| `0x033` | DamageFlyHigh | | `0x0D1` | Nair |
| `0x034` | DamageFlyMid | | `0x0D2` | Fair |
| `0x035` | DamageFlyLow | | `0x0D3` | Bair |
| `0x036` | DamageFlyTop | | `0x0D4` | Uair |
| `0x037` | DamageFlyRoll | | `0x0D5` | Dair |
| `0x038` | WallBounce | | `0x0D6-0x0DA` | Aerial landing lag (N/F/B/U/D) |
| `0x039` | Tumble | | `0x0DB` | LandingAirX (Z-cancel) |
| `0x03A` | FallSpecial | | | |
| `0x03B` | LandingSpecial | | | |
| `0x03C` | Tornado | | | |
| `0x03D` | Barrel | | | |
| `0x03E-0x041` | Pipe | | | |
| `0x042` | CeilingBonk | | | |
| `0x043-0x048` | Knocked down/getup | | | |
| `0x049-0x04A` | TechF/TechB | | | |
| `0x04B-0x04E` | Getup roll fwd/back | | | |
| `0x04F` | DownAttackD | | | |
| `0x050` | DownAttackU | | | |
| `0x051` | Tech | | | |
| `0x052` | Clang | | | |
| `0x053` | ClangRecoil | | | |

Derived predicates a consumer may find useful: dead/being-KO'd =
`actionStateId <= 0x004`; respawning = `actionStateId == 0x005` or
`0x007`-`0x009`; in hitstun = `actionStateId` in `0x025`-`0x039` (or check
`hitstunCounter` directly, §5.2); shielding = `actionStateId` in
`0x098`-`0x09B`; grabbed = `actionStateId` in `0x0AB`-`0x0BC`; attacking =
`actionStateId >= 0x0BE`. Airborne state should come from `groundedState`
(§5.2), not be inferred from `actionStateId` alone.

### 8.4 Controller button bits (`InputFrame.buttons`)

| Bit | Button | | Bit | Button |
|---:|---|---|---:|---|
| `0x8000` | A | | `0x0100` | D-Right |
| `0x4000` | B | | `0x0020` | L |
| `0x2000` | Z | | `0x0010` | R |
| `0x1000` | Start | | `0x0008` | C-Up |
| `0x0800` | D-Up | | `0x0004` | C-Down |
| `0x0400` | D-Down | | `0x0002` | C-Left |
| `0x0200` | D-Left | | `0x0001` | C-Right |

### 8.5 `game_status` (internal, not directly exposed as an event field)

Governs the recorder's own state machine (not written to the file
directly, but explains the `frame` counter's start point and
`MatchEnd.endReason`): `0` pre-match countdown, `1` ongoing (this is the
only state that produces `InputFrame`/`StateFrame` events, and `frame ==
0` is the first frame this state is observed), `2` paused, `5` ended.

### 8.6 `ItemUpdate.linkId` and `.kind` (`smash64` family)

`linkId` (`ItemUpdate` offset `0x08`) is `4` (Item) or `5` (Weapon) — this
recorder never emits any other value. It selects which of the two enums
below `kind` is a value from.

**`WPKind`** (`linkId == 5` — free-flying character special-move
projectiles):

| Value | Name | | Value | Name |
|---:|---|---|---:|---|
| `0x00` | Fireball | | `0x10` | BulletNormal |
| `0x01` | Blaster | | `0x11` | BulletHard |
| `0x02` | ChargeShot | | `0x12` | ArwingLaser2D |
| `0x03` | SamusBomb | | `0x13` | ArwingLaser3D |
| `0x04` | Cutter | | `0x14` | LGunAmmo |
| `0x05` | EggThrow | | `0x15` | FFlowerFlame |
| `0x06` | YoshiStar | | `0x16` | StarRodStar |
| `0x07` | Boomerang | | `0x17`-`0x1F` | Pokémon/monster weapons (not individually enumerated here) |
| `0x08` | SpinAttack | | | |
| `0x09` | ThunderJoltAir | | | |
| `0x0A` | ThunderJoltGround | | | |
| `0x0B` | ThunderHead | | | |
| `0x0C` | ThunderTrail | | | |
| `0x0D` | PKFire | | | |
| `0x0E` | PKThunderHead | | | |
| `0x0F` | PKThunderTrail | | | |

`0x1F` is `nWPKindMonsterEnd` — Remix's own mod-added weapon IDs continue
numbering from there.

**`ITKind`** (`linkId == 4` — thrown/spawned items, stage hazard objects,
and some fighter-held things like Link's pulled bomb):

| Value | Name | | Value | Name | | Value | Name |
|---:|---|---|---:|---|---|---:|---|
| `0x00` | Crate | | `0x0F` | Bob-omb | | `0x1E` | Venusaur |
| `0x01` | Barrel | | `0x10` | Bumper | | `0x1F` | Porygon |
| `0x02` | Capsule | | `0x11` | GreenShell | | `0x20` | Onix |
| `0x03` | Egg | | `0x12` | RedShell | | `0x21` | Snorlax |
| `0x04` | MaximTomato | | `0x13` | Pokéball | | `0x22` | Goldeen |
| `0x05` | Heart | | `0x14` | PKFirePillar | | `0x23` | Meowth |
| `0x06` | Star | | `0x15` | Bomb | | `0x24` | Charizard |
| `0x07` | BeamSword | | `0x16` | PowBlock | | `0x25` | Beedrill |
| `0x08` | HomeRunBat | | `0x17` | Bumper (stage) | | `0x26` | Blastoise |
| `0x09` | Fan | | `0x18` | PiranhaPlant | | `0x27` | Chansey |
| `0x0A` | StarRod | | `0x19` | Target | | `0x28` | Starmie |
| `0x0B` | RayGun | | `0x1A` | RTTFBomb | | `0x29` | Hitmonlee |
| `0x0C` | FireFlower | | `0x1B` | Chansey (stage) | | `0x2A` | Koffing |
| `0x0D` | Hammer | | `0x1C` | Electrode | | `0x2B` | Clefairy |
| `0x0E` | MotionSensorBomb | | `0x1D` | Charmander | | `0x2C` | Mew |

Two entries repeat a name at a different value (`Bumper` at both `0x10` and
`0x17`; `Chansey` at both `0x1B` and `0x27`) — that's the source enum's own
structure, not a transcription error. Remix's `BOWSER_BOMB` is a custom
out-of-range value (`0x011A`) specific to one stage hazard, not part of the
normal `0x00`-`0x2C` span.

### 8.7 `StageHazardUpdate.hazardFlags` bits (`smash64` family)

| Bit | Meaning |
|---:|---|
| `0x01` | Whispy Woods is currently blowing (Dream Land only — see §5.4). |
| `0x02` | Wind direction: `0` = blowing left, `1` = blowing right. Only meaningful when bit `0x01` is also set. |

All other bits are currently always `0`, reserved for hazards not yet
tracked (Zebes' rising acid, Duel Zone's disappearing platforms, …).

## 9. Known limitations / not yet implemented

- **Hitbox/hurtbox tracking has been removed entirely.** An earlier
  version of this format recorded per-frame hitbox/hurtbox geometry; it
  never worked reliably (a real match with confirmed hits/KOs recorded
  zero hitbox events even after fixing the known bugs in the read path)
  and has been dropped rather than carried forward as dead weight. A
  future design can reintroduce this from scratch if there's a concrete
  need.
- **Item/weapon tracking has no velocity, damage, owner/attacker port, or
  expiration data.** `ItemUpdate` (§5.3) records a named `kind` (§8.6) and
  position for every live, non-held Item/Weapon object, but none of those
  additional fields have been mapped in memory yet.
- **Stage hazard tracking covers exactly one hazard.** `StageHazardUpdate`
  (§5.4) currently only tracks Whispy Woods' wind on Dream Land.
- **No RNG seed is recorded.** No known Smash Remix RNG seed address has
  been identified yet, so any RNG-dependent outcome (item spawn rolls,
  certain move variance, …) is not currently reproducible from a `.rmgr`
  file alone — see the determinism caveat in §1.
- **Most of Smash Remix's own settings/mutators aren't captured yet.**
  `MatchSettings` (§5.1) covers the original SSB64 settings plus a small
  set of Remix additions (teams, handicap, CPU level, item frequency);
  Remix has added considerably more match-configuration options since
  those fields were last extended, and none of the newer ones are mapped
  to memory or captured here yet. This is a mapping gap to close via
  ordinary field-appends (§6) as each setting gets identified, not a
  structural limitation of `MatchSettings` itself.
- **No aggregate damage-dealt/taken breakdown or incoming-damage-this-hit
  field**, even though the emulator exposes them.
- **`MatchEnd.endReason` cannot currently distinguish time-out from
  stock-out** — both collapse to `1` ("normal end").
- **Character-specific action states (`>= 0x0DC`) have no shared table** —
  meaning is entirely per-character.
- **Remix-specific stage IDs (`0x29`+) are not enumerated** in §8.2.
- **No ROM-identity check for the extension layer.** Nothing currently
  verifies the loaded ROM matches `gameFamily`'s expectations beyond
  `goodName` comparison; a mismatch would produce an extension layer full
  of garbage bytes rather than falling back to core-only recording. This
  is a correctness gap to close during implementation, not an accepted
  limitation of the design itself.
- **Only one `gameFamily` (`smash64`) is actually defined.** The layering
  in §2.1 is designed to support additional families (e.g. a future Mario
  Kart 64 family) without touching `smash64`'s definitions, but no second
  family exists yet.

## 10. Reference implementation

- **Writer:** `Source/RMG-Core/Replay.cpp` / `Source/RMG-Core/Replay.hpp` -
  implements this version (`5`). Memory reading:
  `Source/RMG-Core/ReplayMemory.cpp` / `Source/RMG-Core/ReplayMemory.hpp`.
- **TypeScript reader/writer + tests:** [`rmgr-ts`](https://github.com/hopskipnfall/rmgr-ts),
  its own repository (extracted from this one). **Not yet updated for this
  version** - still reads the pre-this-document, unspecified in-development
  layout. `rmgr-viewer` (consumer of `rmgr-ts`) is therefore also not yet
  able to read files this writer produces.

The reader-side work is tracked separately: `rmgr-ts` needs its parser/types
updated for this version's header (§3.1), compression (§3.4), and event
catalog (§4/§5) before `rmgr-viewer` (which consumes `rmgr-ts`) can read
anything this writer produces.
