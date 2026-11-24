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
// This is a default, not a lock. Netplay boots with empty save data, so
// Remix starts every toggle at its assembled value (Boot.load_ calls
// SRAM.initialize_ instead of Toggles.load_); this option changes that
// value. If the host picked a starting save that holds Remix settings, Remix
// loads those instead and this option has no effect.
//
// With vanilla-css on this option changes nothing a player can see:
// vanilla-css already makes both random routines use the VANILLA mode
// whatever this toggle says (its edits 3 and 4 replace the toggle loads; this
// option writes the toggle's value, so the two never touch the same bytes and
// can be on together). It is meant for Remix-roster rooms.
//
// This file is the source of the "random-vanilla" patch in
// ../smash-remix-2.0.1.json. To check or regenerate it, assemble it over a
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
// Toggles.entry_variant_random (src/Toggles.asm ~2398, "Random Select") is a
// Menu.entry in Remix's data (RAM 0x804620D4 in the v2.0.1 symbol log;
// rom = ram - 0x7D800000); its current value is the word at +0x04. Values:
// 0 DEFAULT, 1 INCLUDE BONUS, 2 ALL VARIANTS, 3 VANILLA ONLY. It is read by
// CharacterSelect.get_random_char_id_ and TwelveCharBattle.get_random_char_id_.

arch n64.cpu
endian msb

constant entry_variant_random(0x804620D4)
constant RANDOM_VANILLA(3)

origin entry_variant_random + 4 - 0x80400000 + 0x02C00000
base entry_variant_random + 4
    dw RANDOM_VANILLA                       // was: 0 (DEFAULT)
