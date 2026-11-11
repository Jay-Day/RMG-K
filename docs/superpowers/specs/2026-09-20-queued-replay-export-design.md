# Queued "Export Replays" — Design

**Status:** Approved, not yet implemented.
**Scope:** `Source/RMG/UserInterface/Dialog/Kaillera/KailleraPlaybackDialog.cpp`/`.hpp`, `RMGK_GAME_STATS`-gated "Export Replays" (`.krec` → `.rmgr`) feature only.

## Problem

Today, clicking "Export Replays" on the Kaillera Playback dialog starts a
single headless export (a second launch of the app with `--export-krec`
etc. CLI flags — see `ReplayFileExport.cpp`) and shows a **modal**
`QProgressDialog` that stays up until that one export finishes. While it's
showing, `onPlaybackExportReplay()` refuses to start another export
(`if (m_exportProcess != nullptr) return;`), so exporting several
recordings means clicking Export, waiting for the modal to close, clicking
Export again, and so on — fully serialized and blocking.

The user wants to be able to select a recording, click Export Replays,
then immediately select another and click Export Replays again — queuing
it up instead of being blocked.

## Non-goals

- **"Export MP4" is untouched.** It keeps its existing modal
  `QProgressDialog` and one-at-a-time behavior. It shares the underlying
  `m_exportProcess`/`m_exportIsReplayFile` bookkeeping with replay export
  (the two are mutually exclusive — only one external export subprocess
  ever runs at a time), but gets no new queue of its own. A replay export
  queued while an MP4 export is running simply waits its turn, same as
  today.
- **No multi-select UI change.** The recordings table stays
  single-selection (`QAbstractItemView::SingleSelection`). Queuing several
  exports means selecting one recording, clicking Export Replays, then
  repeating — not a batch/multi-select action.
- **No change to the actual export subprocess/CLI mechanism**
  (`startReplayFileExportProcess`'s `QProcess` invocation, `ReplayFileExport.cpp`,
  the `.rmgr` writer itself). This is purely a UI/orchestration change in
  `KailleraPlaybackDialog`.

## Data model

A new member on `KailleraPlaybackDialog`:

```cpp
#ifdef RMGK_GAME_STATS
struct ReplayExportQueueItem
{
    QString displayName;   // for the queue/history panel, e.g. the .krec's file name
    QString recordingPath;
    QString romPath;
    QString outputPath;
    int     totalFrames = 0;
};
QList<ReplayExportQueueItem> m_replayExportQueue;
#endif
```

Nothing here is persisted across dialog instances — `KailleraPlaybackDialog`
is constructed fresh each time the Kaillera Playback dialog is opened, so
the queue (and everything else in this design) has the same lifetime as
today's `m_exportProcess`/`m_exportProgressDialog` members.

## Behavior changes

### `onPlaybackExportReplay()`

Keep every existing validation unchanged (emulation-not-running check,
selected-recording check, ROM-resolution check, output-path derivation).

Remove the early `if (m_exportProcess != nullptr) { return; }` guard for
this method specifically (`onPlaybackExport()`'s MP4 guard is untouched).
Once a valid `ReplayExportQueueItem` is built:

1. If an item with the same `recordingPath` is already the in-flight
   export (`m_exportIsReplayFile && m_exportProcess != nullptr` and its
   recording matches) or already sitting in `m_replayExportQueue`, don't
   add it again — instead show a short inline note in the panel (see
   below), e.g. "Already queued: `<name>`". This is a plain de-dup check
   on `recordingPath`, nothing fancier.
2. Otherwise, append the item to `m_replayExportQueue`.
3. If nothing is currently running (`m_exportProcess == nullptr`), pop the
   front of the queue and start it immediately via
   `startReplayFileExportProcess(...)` (existing signature, unchanged).
   Otherwise, just refresh the panel so its "N queued" line is current.

### `startReplayFileExportProcess()`

Unchanged except: **do not create/show `m_exportProgressDialog`** for this
path. Instead, ensure the new inline panel (below) is visible and call the
same progress-refresh entry point so it shows initial state.

### New inline panel

A small, non-modal `QWidget` (`m_replayExportPanel`) added to
`KailleraPlaybackDialog`'s layout (below the recordings table, above the
bottom button row), containing:

- `QLabel* m_replayExportStatusLabel` — driven by the **existing**
  `buildExportProgressSummary()` string builder (already game/format
  agnostic text — no changes needed there beyond what already branches on
  `m_exportIsReplayFile`).
- `QProgressBar* m_replayExportProgressBar` — same range/value logic
  `updateExportProgressDialog()` already computes for the modal dialog's
  progress bar, just targeting this bar instead when `m_exportIsReplayFile`
  is true.
- `QLabel* m_replayExportQueueLabel` — e.g. "2 more queued", hidden when
  the queue is empty.
- `QListWidget* m_replayExportHistoryList` — one line appended per
  finished item: `"✓ <name> → <output path>"` on success, `"✗ <name>: <short error>"`
  on failure. Capped at a reasonable length (e.g. last 20 entries) — this
  is a convenience log, not a persisted record.

The panel starts hidden and becomes visible the first time a replay
export starts; it then stays visible and just keeps updating in place for
the rest of the dialog's lifetime (no hide/show flicker logic — simplest
option that satisfies the requirement).

`updateExportProgressDialog()` is renamed to `updateExportProgress()` (or
kept, with an early branch) and split so that:
- If `m_exportIsReplayFile`: update `m_replayExportStatusLabel`/
  `m_replayExportProgressBar`/`m_replayExportQueueLabel`.
- Else: exactly today's code, updating `m_exportProgressDialog`.

Both branches reuse the same `buildExportProgressSummary()` and the same
frame-based range/value math — that logic itself doesn't change, only
which widget it's written into.

### `onExportProcessFinished()`

Keep the existing preamble (`onExportProcessOutput()`,
`processExportOutputText(...)`, capturing `canceled`/`outputPath`/
`fullLog`/`logSummary`, calling `resetExportUi()`) unchanged — this still
resets the shared process/progress bookkeeping and re-enables both export
buttons.

Then branch on `m_exportIsReplayFile` (still correctly reflects which
export just finished — nothing resets it before this point):

- **MP4** (`!m_exportIsReplayFile`): unchanged — the same
  canceled/success (`showExportFinishedDialog`)/failure
  (`QMessageBox::warning`) flow as today.
- **Replay** (`m_exportIsReplayFile`): no popup. Append one line to
  `m_replayExportHistoryList` reflecting canceled/success/failure. Then:
  - If `m_replayExportQueue` is non-empty, pop the front item and call
    `startReplayFileExportProcess(...)` for it, continuing the chain.
  - If empty, leave the panel showing its last state (idle).

### Dialog close / cancel

No new code needed. `KailleraPlaybackDialog::~KailleraPlaybackDialog()`
already kills `m_exportProcess` on destruction, and since the dialog (and
therefore `m_replayExportQueue`) is destroyed along with it, closing the
dialog while exports are queued naturally drops everything still queued —
matching the chosen "cancel remaining queue on close" behavior with no
extra logic.

## Error handling

- Validation failures when *adding* to the queue (bad selection, ROM not
  found, emulation running) behave exactly as today — an immediate
  `QMessageBox` and nothing queued. These are synchronous, pre-queue
  checks; nothing about them changes.
- A failure *during* a queued export (subprocess exits non-zero) is
  reported in the history list (`✗ ...`), and the queue continues to the
  next item rather than stopping — one bad recording shouldn't block the
  rest of the queue.
- The de-dup check (same `recordingPath` already queued/in-flight) is a
  no-op with a short inline note, not an error dialog — this is an
  expected, harmless case (user double-clicked, or selected the same row
  twice), not a failure.

## Testing

- Local syntax verification only: `g++ -std=c++20 -fsyntax-only` (native)
  and `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only` (against real
  MSYS2 Qt/headers) on the modified files, matching this session's
  established compile-check practice.
- **No interactive Qt UI verification is possible in this environment**
  (no display). The user should manually click through the queued-export
  flow (select recording → export → select another → export while the
  first is still running → confirm both complete and the panel/history
  update correctly, including a deliberately-failing export) before
  relying on this in practice.
