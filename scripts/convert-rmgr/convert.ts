/**
 * One-time migration tool: converts existing old-format (`.rmgr` version 3
 * or 4) recordings to the new format-v5 spec (docs/RMGR_SPEC.md), using
 * `legacyReader.ts` (this directory - NOT part of `@rmg-k/rmgr`, which only
 * reads v5) to read the old files and `@rmg-k/rmgr`'s own `serializeReplay`
 * to write the new ones.
 *
 * All existing recordings map to the `smash64` game family (every sample
 * file's goodName is "SmashRemix2.0.1") - MatchSettings/MatchResult are
 * always populated. When an old file has no GameEnd (a truncated/crashed
 * recording - the old format tolerated this, v5 does not), MatchResult's
 * placements are inferred from the last recorded frame's stocksRemaining
 * per port, same fallback `examples/inspect-replay.ts` (in rmgr-ts) used
 * for exactly this case before v5 removed it as a real possibility.
 *
 * Usage:
 *   node convert.ts [output-dir] [file-or-glob-dir ...]
 *
 * With no arguments, converts every `*.rmgr` file directly under the RMG-K
 * repo root into ./converted/ (relative to this script).
 *
 * Requires Node 22.6+ (native .ts execution) or run via `node
 * --experimental-strip-types convert.ts` on Node 22.
 */
import { readFileSync, writeFileSync, mkdirSync, readdirSync, statSync } from "node:fs";
import { join, basename, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import {
  serializeReplay,
  parseReplay,
  SMASH_64_FAMILY,
  type SerializableReplay,
  type MatchSettings,
  type MatchStart,
  type Frame,
} from "@rmg-k/rmgr";
import { parseLegacyReplay, type LegacyReplay } from "./legacyReader.ts";

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(SCRIPT_DIR, "..", "..");

/** Builds a proper 4-tuple from a 4-element array - `Array.prototype.map`'s return type is `T[]`, not a tuple, even when the source is one. */
function portTuple<T>(values: readonly T[]): readonly [T, T, T, T] {
  return [values[0]!, values[1]!, values[2]!, values[3]!];
}

function toMatchStart(legacy: LegacyReplay): MatchStart {
  return {
    playerNames: legacy.gameStart.playerNames,
    slotType: portTuple(legacy.gameStart.ports.map((p) => p.slotType)),
  };
}

function toMatchSettings(legacy: LegacyReplay): MatchSettings {
  const gs = legacy.gameStart;
  return {
    stageId: gs.stageId,
    gameType: gs.gameType,
    stockCountSetting: gs.stockCountSetting,
    timeLimitMinutes: gs.timeLimitMinutes,
    damageRatio: gs.damageRatio,
    itemFrequency: gs.itemFrequency,
    teamsEnabled: gs.teamsEnabled,
    handicapMode: gs.handicapMode,
    characterId: portTuple(gs.ports.map((p) => p.characterId)),
    costumeId: portTuple(gs.ports.map((p) => p.costumeId)),
    teamColor: portTuple(gs.ports.map((p) => p.teamColor)),
    portTeam: portTuple(gs.ports.map((p) => p.team)),
    portHandicap: portTuple(gs.ports.map((p) => p.handicap)),
    portCpuLevel: portTuple(gs.ports.map((p) => p.cpuLevel)),
  };
}

function toFrames(legacy: LegacyReplay): Frame[] {
  return legacy.frames.map((f) => {
    const ports: { -readonly [K in 0 | 1 | 2 | 3]?: Frame["ports"][K] } = {};
    for (const [portStr, data] of Object.entries(f.ports)) {
      if (!data) continue;
      const port = Number(portStr) as 0 | 1 | 2 | 3;
      ports[port] = {
        input: {
          frame: data.pre.frame,
          port,
          buttons: data.pre.buttons,
          stickX: data.pre.stickX,
          stickY: data.pre.stickY,
        },
        state: { ...data.post, port },
      };
    }
    return {
      frame: f.frame,
      ports,
      items: f.items,
      hazardFlags: f.hazardFlags,
    };
  });
}

/** Infers final placements from the last recorded frame's stocksRemaining, for a truncated old recording with no real GameEnd. -1 for a port that was never seated at all. */
function inferPlacements(legacy: LegacyReplay): readonly [number, number, number, number] {
  const placements: [number, number, number, number] = [-1, -1, -1, -1];
  const lastFrame = legacy.frames[legacy.frames.length - 1];
  if (!lastFrame) return placements;
  for (const [portStr, data] of Object.entries(lastFrame.ports)) {
    if (!data) continue;
    const port = Number(portStr);
    placements[port] = data.post.stocksRemaining;
  }
  return placements;
}

function toSerializableReplay(legacy: LegacyReplay): SerializableReplay {
  const frames = toFrames(legacy);
  const finalFrame = legacy.frames.length > 0 ? legacy.frames[legacy.frames.length - 1]!.frame : 0;

  return {
    gameFamily: SMASH_64_FAMILY,
    goodName: legacy.header.goodName,
    recorderSchemaVersion: legacy.header.recorderSchemaVersion,
    recordedAtEpochMillis: legacy.header.recordedAtEpochMillis,
    matchStart: toMatchStart(legacy),
    matchSettings: toMatchSettings(legacy),
    frames,
    matchEnd: legacy.gameEnd
      ? { finalFrame, endReason: legacy.gameEnd.endReason }
      : { finalFrame, endReason: "aborted" },
    matchResult: {
      placements: legacy.gameEnd ? legacy.gameEnd.placements : inferPlacements(legacy),
    },
  };
}

interface ConversionStat {
  file: string;
  oldBytes: number;
  newBytes: number;
  oldParseMs: number;
  newParseMs: number;
  frames: number;
}

function findDefaultInputFiles(): string[] {
  return readdirSync(REPO_ROOT)
    .filter((name) => name.endsWith(".rmgr"))
    .map((name) => join(REPO_ROOT, name))
    .filter((path) => statSync(path).isFile());
}

async function convertOne(inputPath: string, outputDir: string): Promise<ConversionStat> {
  const oldBytes = readFileSync(inputPath);

  const parseStart = performance.now();
  const legacy = parseLegacyReplay(new Uint8Array(oldBytes));
  const oldParseMs = performance.now() - parseStart;

  const serializable = toSerializableReplay(legacy);
  const newBytes = await serializeReplay(serializable);

  const outputPath = join(outputDir, basename(inputPath));
  writeFileSync(outputPath, newBytes);

  // Re-parse the freshly-written v5 file (not the in-memory data we just
  // built) to get a fair, apples-to-apples "how long does parsing THIS
  // format take" comparison against the old file's own parse time above.
  const newParseStart = performance.now();
  await parseReplay(newBytes);
  const newParseMs = performance.now() - newParseStart;

  return {
    file: basename(inputPath),
    oldBytes: oldBytes.byteLength,
    newBytes: newBytes.byteLength,
    oldParseMs,
    newParseMs,
    frames: legacy.frames.length,
  };
}

function formatBytes(n: number): string {
  return n >= 1_000_000 ? `${(n / 1_000_000).toFixed(2)} MB` : `${(n / 1000).toFixed(1)} KB`;
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const outputDir = args[0] ? join(process.cwd(), args[0]) : join(SCRIPT_DIR, "converted");
  const inputFiles = args.length > 1 ? args.slice(1).map((p) => join(process.cwd(), p)) : findDefaultInputFiles();

  if (inputFiles.length === 0) {
    console.error("No .rmgr files found to convert.");
    process.exit(1);
  }

  mkdirSync(outputDir, { recursive: true });
  console.log(`Converting ${inputFiles.length} file(s) -> ${outputDir}\n`);

  const stats: ConversionStat[] = [];
  for (const file of inputFiles) {
    try {
      stats.push(await convertOne(file, outputDir));
    } catch (err) {
      console.error(`FAILED: ${basename(file)} - ${(err as Error).message}`);
    }
  }

  if (stats.length === 0) {
    console.error("Nothing converted successfully.");
    process.exit(1);
  }

  const totalOld = stats.reduce((sum, s) => sum + s.oldBytes, 0);
  const totalNew = stats.reduce((sum, s) => sum + s.newBytes, 0);
  const totalOldParseMs = stats.reduce((sum, s) => sum + s.oldParseMs, 0);
  const totalNewParseMs = stats.reduce((sum, s) => sum + s.newParseMs, 0);

  console.log(
    "File".padEnd(40) +
      "Frames".padStart(8) +
      "Old size".padStart(12) +
      "New size".padStart(12) +
      "Ratio".padStart(8) +
      "Old parse".padStart(12) +
      "New parse".padStart(12),
  );
  for (const s of stats) {
    const ratio = (s.newBytes / s.oldBytes) * 100;
    console.log(
      s.file.padEnd(40) +
        String(s.frames).padStart(8) +
        formatBytes(s.oldBytes).padStart(12) +
        formatBytes(s.newBytes).padStart(12) +
        `${ratio.toFixed(1)}%`.padStart(8) +
        `${s.oldParseMs.toFixed(2)}ms`.padStart(12) +
        `${s.newParseMs.toFixed(2)}ms`.padStart(12),
    );
  }

  console.log(`\nConverted ${stats.length}/${inputFiles.length} file(s).`);
  console.log(
    `Total size: ${formatBytes(totalOld)} -> ${formatBytes(totalNew)} ` +
      `(${((totalNew / totalOld) * 100).toFixed(1)}% of original, ` +
      `${(totalOld / totalNew).toFixed(2)}x smaller)`,
  );
  console.log(
    `Total parse time: ${totalOldParseMs.toFixed(2)}ms (old) -> ${totalNewParseMs.toFixed(2)}ms (new)`,
  );
  console.log(
    `Average per file: ${(totalOldParseMs / stats.length).toFixed(2)}ms (old) -> ` +
      `${(totalNewParseMs / stats.length).toFixed(2)}ms (new)`,
  );
}

await main();
