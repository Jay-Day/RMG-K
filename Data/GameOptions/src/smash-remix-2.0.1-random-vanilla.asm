// Smash Remix 2.0.1: "Random: original 12" game option
//
// Remix's Random Select setting starts on VANILLA ONLY instead of DEFAULT,
// so the random "?" portrait, L-button random (when that toggle is on) and
// the fighter a new CPU panel gets only pick Mario, Fox, DK, Samus, Luigi,
// Link, Yoshi, Captain Falcon, Kirby, Pikachu, Jigglypuff or Ness. The
// select screen keeps Remix's full roster; players can still pick anyone by
// hand, and anyone can change Random Select back in Remix's settings (hold L
// on the character or stage select screen).
//
// This is a default, not a lock. It holds with any starting save: netplay
// usually boots with empty save data, and then Remix starts every toggle at
// its assembled value (edit 1 changes it). A host-picked starting save that
// holds Remix settings is loaded over those values at boot
// (Boot.load_ -> Toggles.load_), so edit 2 sets the toggle again right after
// that, once per boot. Changes made in the settings menu later in the
// session stand.
//
// With vanilla-css on this option changes nothing a player can see:
// vanilla-css already makes both random routines use the VANILLA mode
// whatever this toggle says (its edits 3 and 4 replace the toggle loads; this
// option writes the toggle's value, so the two never touch the same bytes and
// can be on together). It is meant for Remix-roster rooms.
//
// This file is the source of the "random-vanilla" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-random-vanilla.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Remix's code and data are copied from ROM 0x02C00000 to RAM 0x80400000 at
// boot, so rom = ram - 0x7D800000. Addresses are from the bass symbol log of
// a v2.0.1 rebuild.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant t1(9)

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

// Toggles.entry_variant_random (src/Toggles.asm ~2398, "Random Select") is a
// Menu.entry; its current value is the word at +0x04. Values: 0 DEFAULT,
// 1 INCLUDE BONUS, 2 ALL VARIANTS, 3 VANILLA ONLY. It is read by
// CharacterSelect.get_random_char_id_ and TwelveCharBattle.get_random_char_id_.
constant entry_variant_random(0x804620D4)
constant RANDOM_VANILLA(3)

// ---------------------------------------------------------------------------
// 1. The toggle's assembled value (what a boot without save data keeps)
seek(entry_variant_random + 4)
    dw RANDOM_VANILLA                       // was: 0 (DEFAULT)

// ---------------------------------------------------------------------------
// 2. Boot.load_ (RAM 0x8046F368, src/Boot.asm): after the save data
//
// Boot.load_ runs once per boot: it loads Remix's settings from SRAM when
// the save has them (Toggles.load_) or initializes SRAM when it doesn't,
// then jumps back into the boot code at 0x80000638. Jump to rv_after_boot
// instead, which sets the toggle and then goes there. The boot code at
// 0x8000063C calls a function next, so no caller-saved register is live.
seek(0x8046F394)
    j       rv_after_boot                   // was: j 0x80000638 (0x0800018E); delay slot (nop) kept

// The code sits in TwelveCharBattle.prevent_invalid_cpu_
// (RAM 0x804FBEC4-0x804FBEFF): its patch is commented out in
// src/TwelveCharBattle.asm and a ROM-wide scan finds no jump, branch,
// pointer or lui pair into it from anywhere else.
seek(0x804FBEC4)
rv_after_boot:
    lui     at, 0x8046
    ori     t1, r0, RANDOM_VANILLA
    sw      t1, 0x20D8(at)                  // 0x804620D8: Random Select = VANILLA ONLY
    j       0x80000638                      // back into the boot code
    nop

if pc() > 0x804FBF00 {
    error "random-vanilla code overflows TwelveCharBattle.prevent_invalid_cpu_"
}
