// Smash Remix 2.0.1: "Vanilla only" game option
//
// Limits character selection to the original 12 fighters (ids 0x00-0x0B:
// Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu,
// Jigglypuff, Ness). Costumes, shades and teams are unchanged.
//
// This file is the source of the "vanilla-only" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe):
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-vanilla-only.asm
//
// Leave out -create: then bass writes only the bytes assembled below and
// keeps the rest of the ROM. Then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E
// (big-endian .z64, built from the smashremixprivate tag v2.0.1).
//
// Where the code lives
// --------------------
// Remix assembles its code with `origin 0x02C00000` / `base 0x80400000`
// (main.asm). At boot, Boot.load_ (ROM 0x1038, RAM 0x80000438) copies it
// to RAM in a single uncompressed DMA: ROM 0x02C00000 -> RAM 0x80400000,
// 0x1A0000 bytes. Nothing relocates or reloads it after that, so
// rom = ram - 0x80400000 + 0x02C00000 holds for every address below.
// Absolute j/jal targets stay valid because the code runs at the address
// it was assembled for. All edits are above ROM 0x02C00000, outside the
// CIC boot checksum range (0x1000-0x101000), so the header CRCs don't
// change.
//
// The RAM addresses come from the v2.0.1 symbol log (top-level labels in
// Data/Ranked/SmashRemix/2B2D6B295106C54216B7FC7A2F14346E.sym in the
// ranked-ssb branch). Sub-labels were found by disassembling the ROM at
// those addresses and matching src/*.asm at tag v2.0.1. Every edited word's
// original value is in the JSON "expect" field.
//
// Porting to another Remix version: find the same spots in that build
// (bass -sym output, then disassemble), check the original instructions
// match the "was:" comments, and update the addresses. Source references are
// to v2.0.1.
//
// Every path to a character id that this closes is listed at the bottom.

arch n64.cpu
endian msb

// register numbers (the same as Remix's assembler/N64.inc)
constant r0(0)
constant at(1)
constant v0(2)
constant v1(3)
constant a0(4)
constant a1(5)
constant t0(8)
constant t1(9)
constant t2(10)
constant s0(16)
constant sp(29)
constant ra(31)

// character ids (src/Character.asm, scope id)
constant ID_FIRST_NON_VANILLA(0x0C)     // 0x00-0x0B are the original 12 (0x0C = BOSS)
constant ID_PLACEHOLDER(0x1B)           // the random "?" portrait
constant ID_NONE(0x1C)                  // no character

// Toggles.entry_variant_random values: 0 DEFAULT, 1 BONUS, 2 ALL VARIANTS, 3 VANILLA
constant RANDOM_VANILLA(3)

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

// ---------------------------------------------------------------------------
// 1. CharacterSelect.get_character_id_ (src/CharacterSelect.asm ~449)
//
// Every character select screen (VS, Training, 1P, Bonus) calls this each
// frame. It maps the held token's position to a character id: the 30
// portraits (id_table), the bonus bookend (bonus_chars), the random bookend
// (PLACEHOLDER) or the 12cb grid, then applies a d-pad variant. Whatever it
// returns is what the token hovers and what A selects, for human and CPU
// tokens alike.
//
// The variant step (_check_variant, 0x8051EE10-0x8051EE70) is replaced by a
// filter: ids 0x00-0x0B pass and anything else becomes NONE, so a Remix
// portrait, a bonus fighter or a variant reads as an empty spot. The
// random bookend returns PLACEHOLDER by branching straight to _end, so it
// skips the filter and still works. Random's result is limited by edits 3
// and 4.
//
// Regular portraits and the bonus bookend reach _check_variant through
// _begin_check_variant. The 12cb grid doesn't, so edit 2 sends it here.
constant get_character_id__check_variant(0x8051EE10)
constant get_character_id__end(0x8051EE74)
seek(get_character_id__check_variant)
    sltiu   t0, v0, ID_FIRST_NON_VANILLA    // was: lui t0, 0x8051      t0 = 1 if v0 is one of the 12
    bne     t0, r0, get_character_id__end  // was: ori t0, t0, 0xFDF4  keep it
    nop                                     // was: addu t0, t0, a0
    beq     r0, r0, get_character_id__end  // was: lbu t0, 0(t0)       otherwise return...
    ori     v0, r0, ID_NONE                 // was: beqz t0, _end       ...NONE
    // The rest of the old variant code (0x8051EE24-0x8051EE70) can't be
    // reached now. Edit 7 puts its helper at 0x8051EE24. A scan of every
    // aligned word in the ROM finds no j/jal, pointer or lui/addiu|ori pair
    // that targets 0x8051EE14-0x8051EE70, and the function's own branches
    // only target _check_variant (EE10) and _end (EE74).

// ---------------------------------------------------------------------------
// 2. get_character_id_, 12cb branch (0x8051ED68)
//
// In 12-Character Battle the id comes from TwelveCharBattle.get_character_id_
// and used to go straight to _end. Send it through the filter too.
seek(0x8051ED68)
    beq     r0, r0, get_character_id__check_variant    // was: b _end (0x10000042)

// ---------------------------------------------------------------------------
// 3. CharacterSelect.get_random_char_id_ (src/CharacterSelect.asm ~1258)
//
// The shared "pick a random character" routine of the normal CSS layouts.
// It resolves the "?" portrait when the match starts (save_random_char_
// on VS/Training/1P/Bonus), the L-button random pick (select_random_char_),
// the random fighter a panel gets when switched to CPU (vanilla 0x80138858),
// and Tag Team's random queue slots. Remix already has a VANILLA mode
// (Toggles.entry_variant_random = 3) that rerolls until the id is <= NESS.
// Use that mode whatever the toggle says, by replacing the second toggle
// load. The first load (0x8051F0F8) only widens the roll range to include
// bonus fighters, and the VANILLA check rejects those.
seek(0x8051F24C)
    ori     s0, r0, RANDOM_VANILLA          // was: lw s0, 0x0004(s0) (0x8E100004)

// ---------------------------------------------------------------------------
// 4. TwelveCharBattle.get_random_char_id_ (src/TwelveCharBattle.asm ~4466)
//
// 12cb's own random routine, which rolls over every character id. Remix uses
// it when a 12cb "?" is resolved and when a custom 12cb grid is randomized
// with d-pad down. Same fix: always take its VANILLA branch.
seek(0x804FBDAC)
    ori     t0, r0, RANDOM_VANILLA          // was: lw t0, 0x0004(t0) (0x8D080004)

// ---------------------------------------------------------------------------
// 5. TwelveCharBattle.update_character_set_ (src/TwelveCharBattle.asm ~2417)
//
// 12cb offers the character sets Default (the 12 vanilla fighters), Japanese,
// Polygon, Remix and Custom (editable by cycling portraits with Z/R).
// Keep both players on Default: the set index is always set to 0 instead of
// index + direction. Then the 12cb grid only holds vanilla fighters, and the
// Custom editing code (TwelveCharBattle.check_input_, which only runs on
// set 4) can't be reached. CharacterSelect.get_random_char_id_'s 12cb path
// and L-random in 12cb roll from this grid, so they stay vanilla too.
seek(0x804F9EC0)
    or      t2, r0, r0                      // was: addu t2, t2, a1 (0x01455021)

// ---------------------------------------------------------------------------
// 6. Character.variants.table, rows 0x00-0x0B (src/Character.asm ~2530)
//
// The d-pad variants (Metal, Polygon, J/E, Giant DK, Master Hand...) of the
// 12 vanilla fighters. Edit 1 already blocks them. Emptying the rows (all
// NONE) also hides the d-pad variant icons on their panels
// (draw_variant_indicator_ checks for 0x1C1C1C1C), so the panel doesn't
// show variants that can't be picked. Only CharacterSelect reads this table
// (get_character_id_, the ALL VARIANTS random mode, draw_variant_indicator_).
// variants_with_same_model is a separate table and isn't touched.
seek(0x8049C9A0)
    fill 12 * 4, ID_NONE

// ---------------------------------------------------------------------------
// 7. VS character select setup: sanitize restored characters
//
// When the VS CSS opens, vanilla 0x8013ACBC reads each port's character and
// player kind from the VS battle state (0x800A4D08, players at +0x20,
// stride 0x74) and jumps to Remix's
// TwelveCharBattle.setup_ports_and_characters_. That routine returns with
// `lbu t0, 0x23(v1); j 0x8013ACC4; lbu a1, 0x22(v1)`, and the vanilla code
// then stores t0 into the panel (sw t0, 0x48(v0)) and places the token.
// Detour that return through a check, so a non-vanilla id that is somehow
// in the battle state is dropped instead of restored as already selected.
// In a patched session only vanilla ids, PLACEHOLDER and NONE should get
// there, so this is a safety net.
//
// The check sits in the now unreachable part of get_character_id_ (see 1).
// On entry: t0 = char_id, a1 = player kind (0 human, 1 CPU, 2 none),
// v1 = battle state player - 0x20. Remix's routine already clobbers t2,
// so t2 is safe to use as scratch here.
constant setup_ports_and_characters__return_jump(0x804F8A90)
constant vs_css_init_player_return(0x8013ACC4)
constant restore_check(0x8051EE24)

seek(setup_ports_and_characters__return_jump)
    j       restore_check                   // was: j 0x8013ACC4 (0x0804EB31); delay slot (lbu a1, 0x22(v1)) kept

seek(restore_check)
    sltiu   t2, t0, ID_FIRST_NON_VANILLA    // vanilla?
    bne     t2, r0, _restore_ok
    addiu   t2, t0, -ID_PLACEHOLDER         // (delay slot) t2 = id - PLACEHOLDER
    sltiu   t2, t2, 2                       // PLACEHOLDER or NONE?
    bne     t2, r0, _restore_ok
    nop
    ori     t0, r0, ID_NONE                 // anything else: no character
    sb      t0, 0x0023(v1)                  // battle state char_id = NONE
    sb      r0, 0x0026(v1)                  // costume 0
    xori    t2, a1, 1                       // a CPU without a character would be stuck,
    bne     t2, r0, _restore_ok             // so close a CPU panel instead
    nop
    ori     a1, r0, 2                       // player kind = none
    sb      a1, 0x0022(v1)
_restore_ok:
    j       vs_css_init_player_return
    nop

// ---------------------------------------------------------------------------
// Selection paths in 2.0.1 and how each is closed
//
// VS mode (all closed):
//  - Hover/A on a Remix portrait (id_table slots 1, 2, 9-12, 19-30): edit 1
//  - Bonus bookend, cycled with Z/R (bonus_chars, all Remix): edit 1
//  - D-pad variants of a vanilla portrait: edits 1 and 6
//  - Master Hand (Link D-up with the 20th player tag set): edits 1 and 6
//  - CPU tokens picked up and dropped by a human: edit 1 (same routine).
//    The vanilla CSS drops a carried CPU token on the last fighter it was
//    over once it reaches a spot with no fighter (the same as carrying it off
//    the grid), so a CPU token carried onto a Remix portrait lands on the last
//    vanilla portrait it crossed.
//  - Panel switched to CPU (vanilla 0x80138848 rolls a random fighter): edit 3
//  - Random "?" resolved at match start (save_random_char_._vs): edit 3, or
//    edit 4 under 12cb
//  - L-button random (select_random_char_): edit 3
//  - Tag Team random queue slots (TagTeam.asm ~613): edit 3
//  - 12cb grid, character sets, Custom editing, 12cb d-pad random: edits 2, 4
//    and 5
//  - Characters restored when the CSS opens (battle state): edit 7
//  - Token auto-placement (place_token_from_id_) only places tokens for ids
//    that already passed the paths above.
// Other modes:
//  - Training, 1P (including Remix 1P modes), Bonus/BTT/BTP/HRC: hovering,
//    variants and their "?" resolution go through edits 1, 3 and 6. Their
//    restored-character paths aren't filtered: they can only hold what
//    their own CSS (patched) let through, or the boot defaults.
//  - Not character selection, left alone: Kirby's starting hat (CSS panel
//    menu "Kirby Hat", may copy a Remix fighter's power), Remix 1P enemies,
//    the attract demo (uses its own battle state).
