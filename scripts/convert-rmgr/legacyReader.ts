/**
 * Minimal, self-contained reader for the OLD (pre-v5) `.rmgr` format -
 * versions 3 and 4, the only versions this repo's own C++ recorder ever
 * actually wrote (see docs/RMGR_SPEC.md's history). This is NOT part of
 * `@rmg-k/rmgr` (that package only reads v5, matching the "total break, no
 * migration path" design) - it exists solely so `convert.ts` can read
 * existing old-format files to migrate them. Delete this once nothing on
 * disk still needs converting.
 *
 * Only the subset of the old format actually needed for a full-fidelity
 * conversion is implemented: GameStart, PreFrameUpdate, PostFrameUpdate,
 * GameEnd, ItemUpdate, StageHazardUpdate. HitboxUpdate/HurtboxUpdate
 * (schema v5+, removed in v5 anyway) are skipped via their declared size
 * if present, never read.
 */

class LegacyBinaryReader {
  private readonly bytes: Uint8Array;
  private readonly view: DataView;
  private offset = 0;
  constructor(bytes: Uint8Array) {
    this.bytes = bytes;
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }
  get position(): number {
    return this.offset;
  }
  hasMore(): boolean {
    return this.offset < this.bytes.byteLength;
  }
  readU8(): number {
    const v = this.view.getUint8(this.offset);
    this.offset += 1;
    return v;
  }
  readI8(): number {
    const v = this.view.getInt8(this.offset);
    this.offset += 1;
    return v;
  }
  readU16(): number {
    const v = this.view.getUint16(this.offset, true);
    this.offset += 2;
    return v;
  }
  readI32(): number {
    const v = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return v;
  }
  readU32(): number {
    const v = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return v;
  }
  readF32(): number {
    const v = this.view.getFloat32(this.offset, true);
    this.offset += 4;
    return v;
  }
  readU64(): number {
    const v = this.view.getBigUint64(this.offset, true);
    this.offset += 8;
    return Number(v);
  }
  readBytes(count: number): Uint8Array {
    const v = this.bytes.subarray(this.offset, this.offset + count);
    this.offset += count;
    return v;
  }
  readFixedString(width: number): string {
    const raw = this.readBytes(width);
    const nul = raw.indexOf(0);
    return new TextDecoder("ascii").decode(nul === -1 ? raw : raw.subarray(0, nul));
  }
  readFixedUtf8String(width: number): string {
    const raw = this.readBytes(width);
    const nul = raw.indexOf(0);
    return new TextDecoder("utf-8").decode(nul === -1 ? raw : raw.subarray(0, nul));
  }
  skip(count: number): void {
    this.offset += count;
  }
}

const EventCode = {
  EventPayloads: 0x01,
  GameStart: 0x02,
  PreFrameUpdate: 0x03,
  PostFrameUpdate: 0x04,
  GameEnd: 0x05,
  ItemUpdate: 0x06,
  StageHazardUpdate: 0x07,
  HitboxUpdate: 0x08,
  HurtboxUpdate: 0x09,
} as const;

const SLOT_TYPE_BY_WIRE = ["human", "cpu", "empty"] as const;
const HANDICAP_MODE_BY_WIRE = ["off", "on", "auto"] as const;

export interface LegacyPortSettings {
  slotType: (typeof SLOT_TYPE_BY_WIRE)[number];
  characterId: number;
  costumeId: number;
  teamColor: number;
  team: number;
  handicap: number;
  cpuLevel: number;
}

export interface LegacyGameStart {
  stageId: number;
  gameType: number;
  stockCountSetting: number;
  timeLimitMinutes: number;
  damageRatio: number;
  itemFrequency: number;
  teamsEnabled: boolean;
  handicapMode: (typeof HANDICAP_MODE_BY_WIRE)[number];
  ports: readonly [LegacyPortSettings, LegacyPortSettings, LegacyPortSettings, LegacyPortSettings];
  playerNames: readonly [string, string, string, string];
}

export interface LegacyPreFrame {
  frame: number;
  port: number;
  buttons: number;
  stickX: number;
  stickY: number;
}

export interface LegacyPostFrame {
  frame: number;
  port: number;
  characterId: number;
  actionStateId: number;
  positionX: number;
  positionY: number;
  facingDirection: 1 | -1;
  velocityX: number;
  velocityY: number;
  damagePercent: number;
  stocksRemaining: number;
  jumpsRemaining: number;
  grounded: boolean;
  hurtboxState: number;
  hitstunCounter: number;
  actionFrameCounter: number;
  comboHitCount: number;
  comboDamage: number;
}

export interface LegacyItemUpdate {
  frame: number;
  objectAddress: number;
  linkId: number;
  kind: number;
  positionX: number;
  positionY: number;
  positionZ: number;
}

export interface LegacyFrame {
  frame: number;
  ports: Partial<Record<number, { pre: LegacyPreFrame; post: LegacyPostFrame }>>;
  items: LegacyItemUpdate[];
  hazardFlags: number;
}

export interface LegacyGameEnd {
  endReason: "aborted" | "normal";
  placements: readonly [number, number, number, number];
}

export interface LegacyReplay {
  header: {
    version: number;
    goodName: string;
    recorderSchemaVersion: number;
    recordedAtEpochMillis: number;
  };
  gameStart: LegacyGameStart;
  frames: LegacyFrame[];
  gameEnd: LegacyGameEnd | null;
}

function readPortTuple<T>(read: () => T): readonly [T, T, T, T] {
  return [read(), read(), read(), read()];
}

export function parseLegacyReplay(data: Uint8Array): LegacyReplay {
  const r = new LegacyBinaryReader(data);
  const magic = new TextDecoder("ascii").decode(r.readBytes(4));
  if (magic !== "RMGR") {
    throw new Error(`bad magic bytes: got "${magic}"`);
  }
  const version = r.readU8();
  if (version < 3 || version > 4) {
    throw new Error(`unsupported legacy version ${version} - only 3/4 are handled`);
  }
  r.skip(3); // reserved
  const streamLength = r.readU32();
  const goodName = r.readFixedUtf8String(64);
  const recorderSchemaVersion = r.readU32();

  let recordedAtEpochMillis: number;
  if (version === 3) {
    recordedAtEpochMillis = r.readU64() * 1000;
  } else {
    recordedAtEpochMillis = r.readU64();
    r.readU32(); // recordedAtNanosOffset, unused
  }

  const headerSize = r.position;
  const streamEnd = streamLength > 0 ? headerSize + streamLength : data.byteLength;

  const firstCode = r.readU8();
  if (firstCode !== EventCode.EventPayloads) {
    throw new Error(`expected EventPayloads first, got 0x${firstCode.toString(16)}`);
  }
  const count = r.readU8();
  const declaredSizes = new Map<number, number>();
  for (let i = 0; i < count; i++) {
    const code = r.readU8();
    const size = r.readU16();
    declaredSizes.set(code, size);
  }

  let gameStart: LegacyGameStart | undefined;
  let gameEnd: LegacyGameEnd | null = null;
  const frameEntries = new Map<
    number,
    Map<number, { pre?: LegacyPreFrame; post?: LegacyPostFrame }>
  >();
  const itemsByFrame = new Map<number, LegacyItemUpdate[]>();
  const hazardFlagsByFrame = new Map<number, number>();

  const entryFor = (frameNumber: number, port: number) => {
    let ports = frameEntries.get(frameNumber);
    if (!ports) {
      ports = new Map();
      frameEntries.set(frameNumber, ports);
    }
    let entry = ports.get(port);
    if (!entry) {
      entry = {};
      ports.set(port, entry);
    }
    return entry;
  };

  while (r.position < Math.min(streamEnd, data.byteLength) && r.hasMore()) {
    const code = r.readU8();
    switch (code) {
      case EventCode.GameStart: {
        const declared = declaredSizes.get(EventCode.GameStart)!;
        const start = r.position;
        const stageId = r.readU8();
        const gameType = r.readU8();
        const stockCountSetting = r.readU8();
        const timeLimitMinutes = r.readU8();
        const damageRatio = r.readU8();
        const itemFrequency = r.readU8();
        const base = readPortTuple(() => ({
          slotType: SLOT_TYPE_BY_WIRE[r.readU8()]!,
          characterId: r.readU8(),
          costumeId: r.readU8(),
          teamColor: r.readU8(),
        }));
        const playerNames = readPortTuple(() => r.readFixedString(32));

        let teamsEnabled = false;
        let handicapMode: LegacyGameStart["handicapMode"] = "off";
        let portTeam: readonly [number, number, number, number] = [0, 0, 0, 0];
        let portHandicap: readonly [number, number, number, number] = [0, 0, 0, 0];
        let portCpuLevel: readonly [number, number, number, number] = [0, 0, 0, 0];
        if (declared - (r.position - start) >= 14) {
          teamsEnabled = r.readU8() !== 0;
          handicapMode = HANDICAP_MODE_BY_WIRE[r.readU8()] ?? "off";
          portTeam = readPortTuple(() => r.readU8());
          portHandicap = readPortTuple(() => r.readU8());
          portCpuLevel = readPortTuple(() => r.readU8());
        }
        const remaining = declared - (r.position - start);
        if (remaining > 0) r.skip(remaining);

        const ports = [0, 1, 2, 3].map((i) => ({
          ...base[i]!,
          team: portTeam[i]!,
          handicap: portHandicap[i]!,
          cpuLevel: portCpuLevel[i]!,
        })) as unknown as LegacyGameStart["ports"];

        gameStart = {
          stageId,
          gameType,
          stockCountSetting,
          timeLimitMinutes,
          damageRatio,
          itemFrequency,
          teamsEnabled,
          handicapMode,
          ports,
          playerNames,
        };
        break;
      }
      case EventCode.PreFrameUpdate: {
        const frame = r.readI32();
        const port = r.readU8();
        const buttons = r.readU16();
        const stickX = r.readI8();
        const stickY = r.readI8();
        entryFor(frame, port).pre = { frame, port, buttons, stickX, stickY };
        break;
      }
      case EventCode.PostFrameUpdate: {
        const declared = declaredSizes.get(EventCode.PostFrameUpdate)!;
        const start = r.position;
        const frame = r.readI32();
        const port = r.readU8();
        const characterId = r.readU8();
        const actionStateId = r.readU16();
        const positionX = r.readF32();
        const positionY = r.readF32();
        const facingDirection = r.readI32() as 1 | -1;
        const velocityX = r.readF32();
        const velocityY = r.readF32();
        const damagePercent = r.readU32();
        const stocksRemaining = r.readI8();
        const jumpsRemaining = r.readU8();
        const grounded = r.readU8() === 0;
        const hurtboxState = r.readU8();
        const hitstunCounter = r.readU16();
        const actionFrameCounter = r.readU32();
        let comboHitCount = 0;
        let comboDamage = 0;
        if (declared - (r.position - start) >= 8) {
          comboHitCount = r.readU32();
          comboDamage = r.readU32();
        }
        const remaining = declared - (r.position - start);
        if (remaining > 0) r.skip(remaining);
        entryFor(frame, port).post = {
          frame,
          port,
          characterId,
          actionStateId,
          positionX,
          positionY,
          facingDirection,
          velocityX,
          velocityY,
          damagePercent,
          stocksRemaining,
          jumpsRemaining,
          grounded,
          hurtboxState,
          hitstunCounter,
          actionFrameCounter,
          comboHitCount,
          comboDamage,
        };
        break;
      }
      case EventCode.GameEnd: {
        const endReason = r.readU8() === 1 ? "normal" : "aborted";
        const placements = readPortTuple(() => r.readI8());
        gameEnd = { endReason, placements };
        break;
      }
      case EventCode.ItemUpdate: {
        const declared = declaredSizes.get(EventCode.ItemUpdate)!;
        const start = r.position;
        const frame = r.readI32();
        const objectAddress = r.readU32();
        const linkId = r.readU8();
        const kind = r.readI32();
        const positionX = r.readF32();
        const positionY = r.readF32();
        const positionZ = r.readF32();
        const remaining = declared - (r.position - start);
        if (remaining > 0) r.skip(remaining);
        let items = itemsByFrame.get(frame);
        if (!items) {
          items = [];
          itemsByFrame.set(frame, items);
        }
        items.push({ frame, objectAddress, linkId, kind, positionX, positionY, positionZ });
        break;
      }
      case EventCode.StageHazardUpdate: {
        const declared = declaredSizes.get(EventCode.StageHazardUpdate)!;
        const start = r.position;
        const frame = r.readI32();
        const hazardFlags = r.readU8();
        const remaining = declared - (r.position - start);
        if (remaining > 0) r.skip(remaining);
        hazardFlagsByFrame.set(frame, hazardFlags);
        break;
      }
      default: {
        const size = declaredSizes.get(code);
        if (size === undefined) {
          throw new Error(`unrecognized event code 0x${code.toString(16)} with no declared size`);
        }
        r.skip(size);
        break;
      }
    }
  }

  if (!gameStart) {
    throw new Error("file has no GameStart event");
  }

  const frameNumbers = [
    ...new Set([...frameEntries.keys(), ...itemsByFrame.keys(), ...hazardFlagsByFrame.keys()]),
  ].sort((a, b) => a - b);

  const frames: LegacyFrame[] = frameNumbers.map((frameNumber) => {
    const portEntries = frameEntries.get(frameNumber);
    const ports: LegacyFrame["ports"] = {};
    if (portEntries) {
      for (const [port, entry] of portEntries) {
        if (entry.pre && entry.post) {
          ports[port] = { pre: entry.pre, post: entry.post };
        }
      }
    }
    return {
      frame: frameNumber,
      ports,
      items: itemsByFrame.get(frameNumber) ?? [],
      hazardFlags: hazardFlagsByFrame.get(frameNumber) ?? 0,
    };
  });

  return {
    header: { version, goodName, recorderSchemaVersion, recordedAtEpochMillis },
    gameStart,
    frames,
    gameEnd,
  };
}
