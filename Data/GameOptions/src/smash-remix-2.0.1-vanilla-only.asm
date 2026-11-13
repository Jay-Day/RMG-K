// Smash Remix 2.0.1: "Vanilla only" game option
//
// Limits character selection to the original 12 fighters (ids 0x00-0x0B:
// Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu,
// Jigglypuff, Ness), and gives the VS character select screen back its
// original look: the original 12 portraits in the original grid. Costumes,
// shades and teams are unchanged.
//
// Part 1 (edits 1-8) is the lock: it closes every way Remix offers to pick a
// non-original character, on every select screen. Part 2 (edits 9-29) brings
// back the original VS select screen.
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
// rom = ram - 0x80400000 + 0x02C00000 holds for those addresses.
// Part 2 also edits the original VS character select overlay (ovl26),
// which the game DMAs uncompressed from ROM 0x12FD80-0x139D00 to RAM
// 0x80131B00 every time that screen opens: rom = ram - 0x80001D80 there.
// Absolute j/jal targets stay valid because the code runs at the address
// it was assembled for. All edits are outside the CIC boot checksum range
// (0x1000-0x101000), so the header CRCs don't change.
//
// The RAM addresses come from the bass symbol log of a v2.0.1 rebuild
// (`bass -sym`; the rebuilt ROM matched Smash_Remix_2.0.1.z64 byte for byte
// apart from the header CRCs) and, for the original game's routines, from the
// ssb-decomp-re decompilation (US). Every edited word's original value is
// in the JSON "expect" field.
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

// ===========================================================================
// Part 1: the lock (edits 1-8)
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. CharacterSelect.get_character_id_ (src/CharacterSelect.asm ~449)
//
// Every character select screen (VS, Training, 1P, Bonus) calls this each
// frame (VS only in 12-Character Battle once edit 14 is in). It maps the
// held token's position to a character id: the 30
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

// ===========================================================================
// Part 2: the original VS character select screen
// ===========================================================================
//
// With the option on, the VS character select screen shows the original
// 12-portrait grid: the original full-size portraits in their original
// places and order, sliding in as in the original game, with the original
// token placement and hit boxes. Remix's 10x3 grid of small portraits, its
// bonus bookend (left) and random "?" bookend (right) aren't drawn.
//
// How Remix changed this screen (VS overlay ovl26: ROM 0x12FD80-0x139D00,
// RAM 0x80131B00, so ROM = RAM - 0x80001D80; found by diffing the overlay
// against Super Smash Bros. (U) and naming every jump target with the bass
// symbol log of a v2.0.1 rebuild):
//  - it stopped loading the portraits file (file 0x13, dMNPlayersVSFileIDs[5]
//    0x13 -> 0) and stopped drawing the portraits (mnPlayersVSMakePortrait
//    -> jr ra);
//  - CharacterSelect.setup_ draws its own grid (draw_portraits_: portraits
//    from Remix file 0xA05, bookends from 0xA06);
//  - it hooked every place the original code turns a token position into a
//    fighter or a fighter into a portrait: mnPlayersVSGetPuckFighterKind,
//    mnPlayersVSGetPortrait, mnPlayersVSGetNextPortraitX (slide-in),
//    mnPlayersVSCenterPuckInPortrait, mnPlayersVSMakePortraitFlash, and the
//    CPU token drift (mnPlayersVSPuckAdjustPortraitEdge/Placed);
//  - it shrinks the tokens to 0.8125 to fit its small portraits.
// The original code behind those hooks is still in the ROM, and so is the
// original portraits file (VPK0-compressed, byte-identical, file table entry
// 0x13 still points at it), and the original grid tables in the overlay's
// data (portrait x/velocity 0x8013B3F0/0x8013B420, fighter-to-portrait
// 0x8013B504).
//
// 12-Character Battle uses the same overlay with Remix's own 3x8 grid
// (TwelveCharBattle.twelve_cb_flag = 1, set up by TwelveCharBattle.setup_
// instead of CharacterSelect.setup_). It keeps Remix's behaviour: every
// hook below checks the flag and takes Remix's path when it is set, so 12cb
// runs exactly the code it ran before. Training, 1P and Bonus have their own
// overlays and keep Remix's layout (with the Part 1 lock).
//
// So this part:
//  - loads file 0x13 again and lets mnPlayersVSMakePortrait run (not in 12cb);
//  - skips Remix's draw_portraits_ on the VS screen when not in 12cb;
//  - turns the grid hooks into "12cb ? Remix : original" dispatchers;
//  - reverts the token shrink and offsets (Remix's 12cb path already used
//    the original values there, so the original code is right for both).
// The dispatchers live in the VS overlay itself, in code the original game
// only runs for a locked fighter: mnPlayersVSMakePortraitShadow and its
// display callback (0x801321B8-0x8013251F). Edit 9 makes "locked" impossible
// (Remix unlocks everything anyway), so that code is never reached. It is
// only in RAM while the VS screen is loaded, which is the only time any of
// the hooks that jump there run (select_random_char_ is VS-only).
//
// In normal VS the fighter under a token now comes from the original
// mnPlayersVSGetPuckFighterKind, which can only return the 12 originals or
// NONE: no Remix portrait, bookend or d-pad variant exists on this grid.
// Part 1's filter still guards 12cb, Training, 1P and Bonus.
//
// Random: the original screen has no random portrait, so Remix's "?"
// bookend is gone with the rest of its grid. L-button random (Remix's
// select_random_char_, Toggles "L random") and the random fighter a new CPU
// panel gets both stay: they don't need a portrait, still go through Part 1's
// VANILLA random mode, and now place the token on the original grid.
// The panel menu, costume/shade selection, stock/time arrows, names and
// series logos are Remix's and stay as they are.

constant a2(6)
constant t3(11)
constant t4(12)
constant t5(13)
constant t6(14)
constant t7(15)
constant t8(24)
constant t9(25)
constant s2(18)
constant s6(22)
constant f4(4)
constant f12(12)
constant f6(6)
constant f8(8)
constant f10(10)

// TwelveCharBattle.twelve_cb_flag (RAM 0x804F7F58): 1 on the 12cb screen
constant TCB_FLAG_HI(0x804F)
constant TCB_FLAG_LO(0x7F58)

// Seeks to a RAM address in the VS character select overlay (ovl26).
macro seek_vs(variable ram) {
    origin ram - 0x80131B00 + 0x0012FD80
    base ram
}

// Remix routines and data the 12cb paths keep using (v2.0.1 symbol log)
constant remix_get_character_id(0x8051ED08)      // CharacterSelect.get_character_id_
constant remix_place_token_from_id(0x8051F3F8)   // CharacterSelect.place_token_from_id_
constant remix_vs_x_position(0x8051EEE0)         // CharacterSelect.token_autoposition_._vs_x_position
constant remix_y_fix(0x8051EF40)                 // ...token_autoposition_.token_autoposition_y_fix_
constant remix_fix_right(0x8051F038)             // ...token_autoposition_.fix_right_boundary_check_
constant remix_fix_bottom(0x8051F04C)            // ...token_autoposition_.fix_bottom_boundary_check_
constant remix_portrait_x_position_pointer(0x80520DB0) // CharacterSelect.portrait_x_position_pointer
constant tcb_portrait_velocity(0x804F8128)       // TwelveCharBattle.portrait_velocity
constant tcb_get_portrait_id(0x804F8EF4)         // TwelveCharBattle.get_portrait_id_

// Original VS overlay routines (ssb-decomp-re names)
constant vs_get_portrait(0x80132168)             // mnPlayersVSGetPortrait
constant vs_make_portrait(0x80132520)            // mnPlayersVSMakePortrait

// ---------------------------------------------------------------------------
// 9. mnPlayersVSCheckFighterLocked (0x80132044): never locked
//
// The original screen shows Luigi, Ness, Captain Falcon and Jigglypuff as
// dark "?" portraits until they're unlocked (sMNPlayersVSFighterMask, from the save). Remix unlocks
// them in its default save data and its own grid never asked, so a save
// could only change what this screen shows. Always answer "unlocked". This
// also makes mnPlayersVSMakePortraitShadow unreachable; the code below
// reuses its space.
seek_vs(0x80132044)
    jr      ra                              // was: addiu at, r0, 4
    or      v0, r0, r0                      // was: beq a0, at, ...; return FALSE

// ---------------------------------------------------------------------------
// 10. dMNPlayersVSFileIDs[5] (0x8013B3B4): load the portraits file again
//
// File 0x13 (MNPlayersPortraits) holds the original portraits, the portrait
// background, the white flash square and the "?" shadows. Remix replaced the
// id with 0 (a file the screen loads anyway). It is loaded in 12cb too; it
// fits in the scene heap next to Remix's files (checked live).
seek_vs(0x8013B3B4)
    dw      0x00000013                      // was: 0

// ---------------------------------------------------------------------------
// 11. mnPlayersVSMakePortrait (0x80132520): draw the original portraits
//
// Remix turned it into `jr ra`. Send it through a 12cb check instead
// (vs_make_portrait_guard below); the second word (nop) stays.
seek_vs(vs_make_portrait)
    j       vs_make_portrait_guard          // was: jr ra

// ---------------------------------------------------------------------------
// 12. mnPlayersVSGetNextPortraitX (0x80131DC8): original slide-in
//
// Remix replaced the copying of the original position/velocity tables with
// its own tables. Use the originals (still in the overlay's data) unless
// 12cb is on, in which case do exactly what Remix did for 12cb.
seek_vs(0x80131DC8)
    mtc1    a1, f12                         // f12 = current x (Remix's first line too)
    lui     t0, TCB_FLAG_HI
    lw      t0, TCB_FLAG_LO(t0)             // t0 = 12cb flag
    lui     v1, 0x8014
    addiu   v1, v1, -0x4C10                 // v1 = original end x table (0x8013B3F0)
    beqz    t0, 0x80131E3C                  // not 12cb: continue in the original code
    addiu   a1, v1, 0x0030                  // a1 = original velocity table (0x8013B420)
    lui     v1, 0x8052
    lw      v1, 0x0DB0(v1)                  // 12cb: v1 = *portrait_x_position_pointer
    lui     a1, (tcb_portrait_velocity >> 16)
    j       0x80131E3C
    ori     a1, a1, (tcb_portrait_velocity & 0xFFFF) // a1 = TwelveCharBattle.portrait_velocity
    // the last three words of Remix's version (0x80131DF8-0x80131E00) are now unreachable

// ---------------------------------------------------------------------------
// 13. mnPlayersVSGetPortrait (0x80132168): fighter -> original portrait
//
// Remix looks the fighter up in its own portrait_id_table (30 slots plus
// bookends), or asks TwelveCharBattle in 12cb. Outside 12cb return the
// original portrait (0-11, top row 0-5) from the original table at
// 0x8013B504; anything that isn't one of the 12 gets portrait 0, as Remix
// did for NONE. The 12cb path is Remix's, unchanged.
seek_vs(vs_get_portrait)
    lui     t0, TCB_FLAG_HI
    lw      t0, TCB_FLAG_LO(t0)             // t0 = 12cb flag
    bnez    t0, _gp_12cb
    sltiu   t1, a0, 0x000C                  // t1 = 1 if one of the 12
    beqz    t1, _gp_return
    or      v0, r0, r0                      // v0 = 0
    sll     t1, a0, 2
    lui     v0, 0x8014
    addu    v0, v0, t1
    lw      v0, -0x4AFC(v0)                 // v0 = original portraits[fighter] (0x8013B504)
_gp_return:
    jr      ra
    nop
_gp_12cb:
    or      a1, s2, r0                      // a1 = port, as Remix did:
    lui     t1, 0x8014                      // s2 holds the port except when
    addiu   t1, t1, -0x6B6C                 // called from 0x8013948C (ra 0x80139494)
    bnel    t1, ra, _gp_12cb_go
    lw      a1, 0x0040(sp)                  // ...where it is at 0x40(sp)
_gp_12cb_go:
    j       tcb_get_portrait_id
    nop

// ---------------------------------------------------------------------------
// 14. mnPlayersVSGetPuckFighterKind (0x8013786C): original hit test
//
// Remix's hook jumps to CharacterSelect.get_character_id_ (its grid,
// bookends, variants, 12cb). Outside 12cb continue in the original routine
// instead: rows y 36-78 and 79-121, columns 45 px from x 25, the 12
// original fighters only.
seek_vs(0x8013786C)
    j       vs_get_kind_dispatch            // was: j CharacterSelect.get_character_id_ (nop stays)

// ---------------------------------------------------------------------------
// 15. mnPlayersVSCenterPuckInPortrait (0x801387A8): original token placement
//
// Used when a CPU panel gets a random fighter, when tokens are placed on
// screen entry and by L-button random (edit 16). Remix's hook jumps to
// CharacterSelect.place_token_from_id_ (its grid and bookends).
seek_vs(0x801387A8)
    j       vs_place_dispatch               // was: j CharacterSelect.place_token_from_id_ (nop stays)

// ---------------------------------------------------------------------------
// 16. CharacterSelect.select_random_char_ (RAM 0x80523254): L-button random
//
// It calls place_token_from_id_ directly, with a stack frame shaped like
// mnPlayersVSCenterPuckInPortrait's. Call the same dispatcher. It only runs
// on the VS screen (from check_variant_input_._vs), where the dispatcher is
// loaded.
seek(0x80523254)
    jal     vs_place_dispatch               // was: jal CharacterSelect.place_token_from_id_

// ---------------------------------------------------------------------------
// 17-19. mnPlayersVSMakePortraitFlash (0x8013647C): original white flash
//
// The flash over a portrait when a fighter is picked. Remix moved it to its
// own display layers (link 0x12, layer 0x1B instead of 30 and 37) and draws
// a flash copy of its own portrait at its own portrait's place. Outside 12cb
// use the original layers and the original white square at the original
// place. Both layer numbers are set by small helpers; the end of the routine
// (original 0x80136524-0x801365B8) is copied unchanged below.
seek_vs(0x801364C4)
    jal     vs_flash_link                   // 17. was: or a0, r0, r0
    or      a1, r0, r0                      //     (unchanged)
    or      a0, r0, r0                      //     was: addiu a2, r0, 0x12 (a2 now set by the helper)
seek_vs(0x801364F4)
    jal     vs_flash_layer                  // 18. was: or a0, v0, r0
    or      a0, v0, r0                      //     was: addiu a2, r0, 0x1B (a2 now set by the helper)
seek_vs(0x80136524)
    j       vs_flash_sprite                 // 19. was: lw a0, 0x3C(sp)
    nop                                     //     was: lui t2, 0x8052

// ---------------------------------------------------------------------------
// 20-24. CPU token drift (mnPlayersVSPuckAdjustPortraitEdge 0x80139460 and
// mnPlayersVSPuckAdjustPlaced 0x8013961C)
//
// A placed token keeps drifting back inside its portrait and away from
// other tokens on the same fighter. Remix changed the portrait's x/y, width
// and height to its grid (and 12cb's) and the spacing between tokens from 15
// to 10. Outside 12cb use the original numbers (45x43 portraits, spacing 15).
seek_vs(0x801394AC)
    j       vs_drift_x                      // 20. was: ori t8, r0, 0xA
    nop                                     //     was: divu v0, t8 (rest of Remix's 9 words unreachable)
seek_vs(0x80139540)
    jal     vs_drift_y                      // 21. was: jal ...token_autoposition_y_fix_ (delay: lui at, 0x4120)
seek_vs(0x8013956C)
    jal     vs_drift_width                  // 22. was: jal ...fix_right_boundary_check_
    lui     at, 0x4234                      //     was: lui at, 0x41C0 (24.0); now 45.0 (original)
seek_vs(0x801395AC)
    jal     vs_drift_height                 // 23. was: jal ...fix_bottom_boundary_check_
    lui     at, 0x422C                      //     was: lui at, 0x41C0 (24.0); now 43.0 (original)
seek_vs(0x801396CC)
    jal     vs_drift_spacing                // 24. was: lui at, 0x4120 (10.0); delay slot lui s4 stays

// ---------------------------------------------------------------------------
// 25-28. Token size and offset (original bytes)
//
// Remix scales tokens to 0.8125 and moves them to match (except in 12cb,
// where its hooks run the original instructions). The original instructions
// are right for both, so put them back.
seek_vs(0x801391A8)                         // 25. token created (adjust_created_token_)
    lw      a1, 0x0088(sp)
    lui     t2, 0x8014
seek_vs(0x80138764)                         // 26. token switched HMN/CPU (adjust_changed_token_)
    lwc1    f8, 0x0038(sp)
    lhu     t4, 0x0024(v0)
seek_vs(0x80138A00)                         // 27. held token x offset 11.0 (adjust_scaled_token_position_._x)
    lui     at, 0x4130
    mtc1    at, f6
seek_vs(0x80138A10)                         // 28. held token y offset -14.0 (..._y)
    lui     at, 0xC160
    add.s   f8, f4, f6

// ---------------------------------------------------------------------------
// 29. CharacterSelect.draw_portraits_ (RAM 0x80520B0C): no Remix grid on VS
//
// draw_portraits_ draws Remix's grid on VS, 1P, Training and Bonus (called
// from CharacterSelect.setup_ with a0 = 0) and the 12cb grid (called from
// TwelveCharBattle.setup_ with a0 = 1). Return before drawing anything when
// the screen is VS and a0 = 0. The screen check that picks the slide-in
// routine is rewritten to do that in the same 13 words: s6 gets the VS
// offset first and the others are adjusted by the difference.
seek(0x80520B0C)
    lui     t0, 0x800A
    lbu     t0, 0x4AD0(t0)                  // t0 = Global.current_screen (was: ori t0, t0, 0x4AD0)
    xori    t1, t0, 0x0010                  // t1 = 0 on VS
    beq     t1, a0, 0x80520DA0              // VS and not 12cb: go to _end (draw nothing)
    addiu   s6, s6, 0x1EE4                  // s6 = VS slide-in routine (0x80131EE4)
    beqz    t1, 0x80520B40                  // 12cb: _loop
    ori     t1, r0, 0x0011
    beql    t0, t1, 0x80520B40              // 1P
    addiu   s6, s6, 0x255C - 0x1EE4
    ori     t1, r0, 0x0012
    beql    t0, t1, 0x80520B40              // Training
    addiu   s6, s6, 0x1D90 - 0x1EE4
    addiu   s6, s6, 0x2150 - 0x1EE4         // Bonus

// ---------------------------------------------------------------------------
// The dispatchers, in mnPlayersVSPortraitProcDisplay/MakePortraitShadow's
// space (0x801321B8-0x8013251F, unreachable after edit 9)
seek_vs(0x801321B8)

// MakePortrait: original routine unless 12cb (then do nothing, as Remix did)
vs_make_portrait_guard:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    bnez    at, _mpg_skip
    nop
    addiu   sp, sp, -0x0070                 // original first two instructions
    j       vs_make_portrait + 8
    lui     t7, 0x8014
_mpg_skip:
    jr      ra
    nop

// GetPuckFighterKind: original routine unless 12cb
vs_get_kind_dispatch:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    bnez    at, _gk_12cb
    mfc1    v1, f10                         // original first two instructions
    j       0x80137874
    mfc1    a1, f6
_gk_12cb:
    j       remix_get_character_id
    nop

// CenterPuckInPortrait / L-button random: original placement unless 12cb.
// Entered with CenterPuckInPortrait's frame (0x18 bytes, ra at 0x14(sp)),
// a1 = fighter, a2 = token object.
vs_place_dispatch:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    bnez    at, _pd_12cb
    sw      a2, 0x0018(sp)                  // where the original code reloads it from
    jal     vs_get_portrait                 // original: v0 = mnPlayersVSGetPortrait(fighter)
    or      a0, a1, r0
    j       0x801387B0                      // rest of the original routine
    nop
_pd_12cb:
    j       remix_place_token_from_id
    nop

// MakePortraitFlash, display link: 30 (original) or 0x12 (12cb)
vs_flash_link:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    beqz    at, _fl_return
    addiu   a2, r0, 0x001E
    addiu   a2, r0, 0x0012
_fl_return:
    jr      ra
    nop

// MakePortraitFlash, display layer: 37 (original) or 0x1B (12cb)
vs_flash_layer:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    beqz    at, _fy_return
    addiu   a2, r0, 0x0025
    addiu   a2, r0, 0x001B
_fy_return:
    jr      ra
    nop

// MakePortraitFlash, sprite and position: Remix's code (still in place at
// 0x80136524) in 12cb, otherwise a copy of the original 0x80136524-0x801365B8
vs_flash_sprite:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    beqz    at, _fs_original
    nop
    lw      a0, 0x003C(sp)                  // Remix's first two instructions
    j       0x8013652C
    lui     t2, 0x8052
_fs_original:                               // original 0x80136524-0x801365B8, unchanged
    lui     t2, 0x8014
    lw      t2, -0x3B4C(t2)                 // t2 = sMNPlayersVSFiles[5] (portraits file)
    lui     t3, 0x0000
    addiu   t3, t3, 0x06F0                  // t3 = WhiteSquare sprite offset
    lw      a0, 0x003C(sp)
    jal     0x800CCFDC                      // lbCommonMakeSObjForGObj
    addu    a1, t2, t3
    slti    at, s0, 6
    bnez    at, _fs_1
    or      v1, s0, r0
    beq     r0, r0, _fs_1
    addiu   v1, s0, -6
_fs_1:
    sll     t4, v1, 2
    subu    t4, t4, v1
    sll     t4, t4, 2
    subu    t4, t4, v1
    sll     t4, t4, 2
    addu    t4, t4, v1                      // t4 = column * 45
    addiu   t5, t4, 0x001A
    mtc1    t5, f4
    slti    at, s0, 6
    or      v1, r0, r0
    cvt.s.w f6, f4
    bnez    at, _fs_2
    swc1    f6, 0x0058(v0)                  // x = column * 45 + 26
    beq     r0, r0, _fs_2
    addiu   v1, r0, 1
_fs_2:
    sll     t6, v1, 2
    subu    t6, t6, v1
    sll     t6, t6, 2
    subu    t6, t6, v1
    sll     t6, t6, 2
    subu    t6, t6, v1                      // t6 = row * 43
    addiu   t7, t6, 0x0025
    mtc1    t7, f8
    nop
    cvt.s.w f10, f8
    swc1    f10, 0x005C(v0)                 // y = row * 43 + 37
    j       0x801365BC                      // original epilogue
    nop

// PuckAdjustPortraitEdge, portrait left edge (a0 = column): original
// (column * 45 + 25) unless 12cb (Remix's 9 words, unchanged)
vs_drift_x:
    lui     t0, TCB_FLAG_HI
    lw      t0, TCB_FLAG_LO(t0)
    bnez    t0, _dx_12cb
    nop
    sll     t8, a0, 2                       // original 0x801394AC-0x801394CC
    subu    t8, t8, a0
    sll     t8, t8, 2
    subu    t8, t8, a0
    sll     t8, t8, 2
    addu    t8, t8, a0
    addiu   t9, t8, 0x0019
    mtc1    t9, f4
    slti    at, v0, 6
    j       0x801394D0
    nop
_dx_12cb:                                   // Remix's 0x801394AC-0x801394CC
    ori     t8, r0, 0x000A
    divu    v0, t8
    mfhi    t8
    ori     t9, r0, 0x0018
    multu   t9, t8
    mflo    t9
    jal     remix_vs_x_position
    addiu   t9, t9, 0x0029
    addu    t9, v0, r0
    j       0x801394D0
    nop

// PuckAdjustPortraitEdge, portrait top edge: original (t1 = row * 43, + 36)
// unless 12cb
vs_drift_y:
    lui     t2, TCB_FLAG_HI
    lw      t2, TCB_FLAG_LO(t2)
    bnez    t2, _dy_12cb
    nop
    jr      ra
    addiu   t2, t1, 0x0024                  // original
_dy_12cb:
    j       remix_y_fix
    nop

// PuckAdjustPortraitEdge, portrait width: 45.0 (set by the caller) unless 12cb
vs_drift_width:
    lui     t0, TCB_FLAG_HI
    lw      t0, TCB_FLAG_LO(t0)
    beqz    t0, _dw_return
    nop
    j       remix_fix_right
    lui     at, 0x41C0                      // Remix's value
_dw_return:
    jr      ra
    mtc1    at, f10                         // original

// PuckAdjustPortraitEdge, portrait height: 43.0 (set by the caller) unless 12cb
vs_drift_height:
    lui     t0, TCB_FLAG_HI
    lw      t0, TCB_FLAG_LO(t0)
    beqz    t0, _dh_return
    nop
    j       remix_fix_bottom
    lui     at, 0x41C0                      // Remix's value
_dh_return:
    jr      ra
    mtc1    at, f4                          // original

// PuckAdjustPlaced, spacing between tokens: 15.0 (original) or 10.0 (12cb)
vs_drift_spacing:
    lui     at, TCB_FLAG_HI
    lw      at, TCB_FLAG_LO(at)
    beqz    at, _ds_return
    lui     at, 0x4170
    lui     at, 0x4120
_ds_return:
    jr      ra
    nop

constant vs_dispatch_end(pc())
if vs_dispatch_end > 0x80132520 {
    error "VS dispatchers overflow mnPlayersVSMakePortraitShadow"
}

// ---------------------------------------------------------------------------
// Selection paths in 2.0.1 and how each is closed
//
// VS mode (all closed):
//  - Hover/A anywhere on the screen: the original hit test (edit 14) only
//    knows the 12 original portraits. There are no Remix portraits, no bonus
//    bookend and no random "?" bookend on this grid (edit 29), and d-pad
//    variants and Z/R bonus cycling have nothing to act on. Edits 1 and 6
//    still stand behind it.
//  - CPU tokens picked up and dropped by a human: the same hit test. The
//    original CSS drops a carried CPU token on the last fighter it was over
//    once it reaches a spot with no fighter.
//  - Panel switched to CPU (vanilla 0x80138848 rolls a random fighter): edit 3
//  - L-button random (select_random_char_): edit 3; the token goes onto the
//    original portrait (edits 15, 16)
//  - Tag Team random queue slots (TagTeam.asm ~613): edit 3
//  - Random "?" resolved at match start (save_random_char_._vs): edit 3, or
//    edit 4 under 12cb (normal VS has no "?" to pick any more)
//  - 12cb grid, character sets, Custom editing, 12cb d-pad random: edits 2, 4
//    and 5 (12cb keeps Remix's grid; every Part 2 hook takes Remix's path)
//  - Characters restored when the CSS opens (battle state): edit 7; the
//    tokens of restored characters are placed by edit 15
//  - Token auto-placement only places tokens for ids that already passed the
//    paths above.
// Other modes:
//  - Training, 1P (including Remix 1P modes), Bonus/BTT/BTP/HRC keep Remix's
//    select screens: hovering, variants and their "?" resolution go through
//    edits 1, 3 and 6. Their restored-character paths aren't filtered: they
//    can only hold what their own CSS (patched) let through, or the boot
//    defaults.
//  - Not character selection, left alone: Kirby's starting hat (CSS panel
//    menu "Kirby Hat", may copy a Remix fighter's power), Remix 1P enemies,
//    the attract demo (uses its own battle state).
