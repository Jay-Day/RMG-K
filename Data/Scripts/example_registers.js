// example_registers.js
//
// Demonstrates how to read CPU registers and interpret their values.
//
// Run this script while a game is running. It prints a one-shot snapshot on
// load, then registers an on_frame hook that prints every 60 frames so the
// output stays readable.

// ─── Snapshot on load ────────────────────────────────────────────────────────

if (!emu.isRunning()) {
    print("[registers] emulation is not running — start a game first");
} else {
    printSnapshot();
}

// ─── Per-frame hook (prints every 60 frames) ─────────────────────────────────

let frameCount = 0;
emu.on_frame(() => {
    frameCount++;
    if (frameCount % 60 === 0) printSnapshot();
});

// ─── Snapshot function ───────────────────────────────────────────────────────

function printSnapshot() {
    const pc = emu.getPC();
    const regs = emu.getRegs();

    print("=== register snapshot ===");
    print("PC: 0x" + pc.toString(16).padStart(8, "0"));

    // ── Integers ─────────────────────────────────────────────────────────────
    //
    // getReg / getRegs return the lower 32 bits of a GPR as an unsigned number
    // (JS number, range 0..4294967295).
    //
    // For an unsigned value just use it directly.
    // For a signed value coerce with |0 — JS will sign-extend the 32-bit pattern.

    const v0_unsigned = regs.v0;            // return value of last syscall/function
    const v0_signed   = regs.v0 | 0;

    print("v0 (unsigned): " + v0_unsigned);
    print("v0 (signed):   " + v0_signed);

    // a0-a3 are function arguments. They're often small positive integers but
    // can also be negative or N64 addresses.
    print("a0: " + (regs.a0 | 0) + "  a1: " + (regs.a1 | 0) +
          "  a2: " + (regs.a2 | 0) + "  a3: " + (regs.a3 | 0));

    // ── Pointer / address ────────────────────────────────────────────────────
    //
    // On N64 addresses are 32-bit values in KSEG0 (0x80xxxxxx) or KSEG1
    // (0xA0xxxxxx). Pass them directly to memory.*().
    //
    // sp (stack pointer) is a good example — it always holds a valid address.

    const sp = regs.sp;
    print("sp: 0x" + sp.toString(16).padStart(8, "0") +
          "  → [sp+0]: 0x" + memory.read32(sp).toString(16).padStart(8, "0"));

    // ra (return address) — where the current function will return to.
    print("ra: 0x" + regs.ra.toString(16).padStart(8, "0"));

    // ── Float reinterpretation from a GPR (rare) ─────────────────────────────
    //
    // Normally floats live in f-registers. Occasionally code uses mtc1 to move
    // an integer bit-pattern from a GPR into an FP register. If you need to see
    // what float a GPR's bit-pattern represents, use ArrayBuffer to reinterpret.

    const t0_bits  = regs.t0;
    const buf      = new ArrayBuffer(4);
    new Uint32Array(buf)[0] = t0_bits;
    const t0_as_float = new Float32Array(buf)[0];
    print("t0: 0x" + t0_bits.toString(16).padStart(8, "0") +
          "  (if float bits: " + t0_as_float.toFixed(6) + ")");

    // ── Floating-point registers ──────────────────────────────────────────────
    //
    // getFReg(n) returns the float value directly — no reinterpretation needed.
    // On N64 in 32-bit FGR mode only even registers (f0,f2,f4,...) are used as
    // independent single-precision floats. Odd registers alias the upper half of
    // the preceding even register.

    const f0  = emu.getFReg(0);
    const f2  = emu.getFReg(2);
    const f4  = emu.getFReg(4);
    const f12 = emu.getFReg(12);   // first FP argument by MIPS O32 ABI
    const f14 = emu.getFReg(14);   // second FP argument
    print("f0: "  + f0.toFixed(6)  + "  f2: "  + f2.toFixed(6) +
          "  f4: " + f4.toFixed(6));
    print("f12 (fp arg 0): " + f12.toFixed(6) +
          "  f14 (fp arg 1): " + f14.toFixed(6));

    // getFRegs() returns all 32 at once as { f0, f1, ..., f31 }.
    const fr = emu.getFRegs();
    print("f24-f31: " +
        [24,25,26,27,28,29,30,31].map(i => "f"+i+"="+fr["f"+i].toFixed(3)).join("  "));

    // ── HI / LO ──────────────────────────────────────────────────────────────
    //
    // Result registers from MULT / DIV instructions. Also unsigned 32-bit.

    print("HI: 0x" + emu.getHI().toString(16).padStart(8, "0") +
          "  LO: 0x" + emu.getLO().toString(16).padStart(8, "0"));

    print("=========================");
}
