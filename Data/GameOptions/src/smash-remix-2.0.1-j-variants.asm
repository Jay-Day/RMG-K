// Smash Remix 2.0.1: "J variants" game option (id j-variants)
//
// Requires vanilla-css. With both on, players still pick among the original
// 12 fighters on the original VS character select screen, but every pick
// (human, CPU or random) plays as the Japanese-version variant Remix ships:
//
//   Mario    0x00 -> J Mario   0x2A     Yoshi       0x06 -> J Yoshi  0x31
//   Fox      0x01 -> J Fox     0x29     C. Falcon   0x07 -> J Falcon 0x28
//   DK       0x02 -> J DK      0x2C     Kirby       0x08 -> J Kirby  0x30
//   Samus    0x03 -> J Samus   0x24     Pikachu     0x09 -> J Pikachu 0x32
//   Luigi    0x04 -> J Luigi   0x2B     Jigglypuff  0x0A -> Purin    0x2E
//   Link     0x05 -> J Link    0x27     Ness        0x0B -> J Ness   0x25
//
// (ids from src/Character.asm, scope id and the define_character list; the
// same pairs are the D-LEFT column of Character.variants.table, which
// vanilla-css empties for these 12 fighters.)
//
// How: the select screen keeps working with the original ids. When it
// hands over to the stage select or the battle, the ids it wrote to the VS
// battle state are swapped for the J ids (edit 1); everything after that
// (stage select, battle, sudden death, results, Remix's salty runback) sees
// the J fighters. When the VS select screen opens again, the J ids are
// swapped back (edit 2), before the screen restores its panels and before
// vanilla-css's safety net (its edit 7) checks them, so a rematch shows the
// same picks on the original grid and nothing is dropped. The select screen
// therefore shows the original fighter (portrait, model, name); the battle,
// stock icons and results show the J one (e.g. "PURIN WIN!").
//
// Remix's Tag Team takes its fighters from its own queues, not from the
// battle state, so the queues are swapped the same way. 12-Character Battle
// is left alone: its grid and its bookkeeping use the original ids (and
// vanilla-css keeps it on the Default set). Training, 1P and Bonus use their
// own select screens and are not changed.
//
// J variants share costumes, extra costumes, team costumes and shades with
// the original fighter in Remix (Costumes.num_costumes, the costumes/*.asm
// register_extra_costumes_for_char lists, Character default_costume copied
// from the parent, Teams.add_team_costume), so the costume picked on the
// select screen is valid for the J fighter.
//
// This file is the source of the "j-variants" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-j-variants.asm
//
// then diff patched.z64 against the original. Every edited word's original
// value is in the JSON "expect" field.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the code lives
// --------------------
// All of it is in the original VS character select overlay (ovl26), which
// the game DMAs uncompressed from ROM 0x12FD80-0x139D00 to RAM 0x80131B00
// whenever that screen opens: rom = ram - 0x80001D80. Both hooks run while
// that screen is loaded. The table and routines sit right after vanilla-css's
// dispatchers, at 0x801323FC-0x801324DB, in the rest of
// mnPlayersVSMakePortraitShadow (which runs to 0x8013251F). That routine
// draws a locked fighter's "?" portrait and is only called from
// mnPlayersVSMakePortrait for a locked fighter. Remix turned mnPlayersVSMakePortrait into `jr ra`, and
// vanilla-css, which brings it back, makes mnPlayersVSCheckFighterLocked
// always answer "unlocked" (its edit 9), so the routine is never reached
// either way; vanilla-css already uses 0x801321B8-0x801323FB of it.
// The addresses come from the bass symbol log of a v2.0.1 rebuild
// (`bass -sym`) and the ssb-decomp-re decompilation (US).

arch n64.cpu
endian msb

// register numbers (the same as Remix's assembler/N64.inc)
constant r0(0)
constant v1(3)
constant a0(4)
constant a1(5)
constant a2(6)
constant a3(7)
constant t0(8)
constant t1(9)
constant t2(10)
constant t6(14)
constant t8(24)
constant t9(25)
constant ra(31)

// Seeks to a RAM address in the VS character select overlay (ovl26).
macro seek_vs(variable ram) {
    origin ram - 0x80131B00 + 0x0012FD80
    base ram
}

constant syTaskmanSetLoadScene(0x80005C74)
constant VS_MODE_FLAG_HI(0x804F)        // VsRemixMenu.vs_mode_flag = 0x804F7568
constant VS_MODE_FLAG_LO(0x7568)
constant MODE_12CB(1)                   // VsRemixMenu.mode.TWELVE_CB
constant TT_QUEUES_HI(0x8050)           // TagTeam.character_queues = 0x804FD248: 4 ports x 6 slots
constant TT_QUEUES_LO(-0x2DB8)          //   x 4 bytes (+0 char_id, +1 costume, +2 shade, +3 Random's pick / Sonic flag)
// gSCManagerTransferBattleState (0x800A4D08): the VS battle state the select
// screen writes and restores from; players[4] at +0x20, 0x74 bytes each,
// fkind at +0x03 of a player.
constant FKIND_P1_HI(0x800A)
constant FKIND_P1_LO(0x4D2B)

constant JV_SPACE(0x801323FC)           // first word after vanilla-css's dispatchers
constant JV_SPACE_END(0x80132520)       // mnPlayersVSMakePortrait

// ---------------------------------------------------------------------------
// 1. mnPlayersVSFuncRun (0x8013A920), leaving for the stage select / battle
//
// When the screen proceeds (0x8013A9D0-0x8013AA54: scene 0x15 or 0x16 is
// chosen), Remix's VsRemixMenu.mode_specific_setup_start_ (at 0x8013AA40)
// runs the mode's setup, then mnPlayersVSSetSceneData writes every panel's
// fighter, costume and kind to the battle state (Remix resolves Random there,
// CharacterSelect.save_random_char_). The next call starts the scene change.
// Swap the ids just before it. This is the only way from this screen to the
// stage select or a battle; going back to the VS menu or the title uses
// other calls.
seek_vs(0x8013AA48)
    jal     jv_map                          // was: jal syTaskmanSetLoadScene (0x0C00171D); delay slot (nop) kept

// ---------------------------------------------------------------------------
// 2. mnPlayersVSStartScene (0x8013B33C): the VS select screen opens
//
// The scene's first instruction. Everything that reads the battle state for
// this screen (mnPlayersVSInitVars, Remix's mode setups and
// TwelveCharBattle.setup_ports_and_characters_, vanilla-css's safety net)
// runs later, from the scene's start function.
seek_vs(0x8013B33C)
    j       jv_unmap                        // was: lui t6, 0x800A (0x3C0E800A); delay slot (addiu sp, sp, -0x18) kept

// ---------------------------------------------------------------------------
// The table and routines (see "Where the code lives")
seek_vs(JV_SPACE)

// id -> its counterpart: 0x00-0x0B give the J variant, 0x24-0x32 give the
// original fighter (Lucas 0x26, E Pikachu 0x2D and E Jigglypuff 0x2F stay).
// 0x0C-0x23 are never looked up; they hold the id itself.
jv_table:
    db 0x2A, 0x29, 0x2C, 0x24, 0x2B, 0x27   // Mario, Fox, DK, Samus, Luigi, Link
    db 0x31, 0x28, 0x30, 0x32, 0x2E, 0x25   // Yoshi, Falcon, Kirby, Pikachu, Jigglypuff, Ness
    db 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
    db 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23
    db 0x03, 0x0B, 0x26, 0x05, 0x07, 0x01   // J Samus, J Ness, (Lucas), J Link, J Falcon, J Fox
    db 0x00, 0x04, 0x02, 0x2D, 0x0A, 0x2F   // J Mario, J Luigi, J DK, (E Pikachu), Purin, (E Jigglypuff)
    db 0x08, 0x06, 0x09                     // J Kirby, J Yoshi, J Pikachu
    db 0x00                                 // (pad to a word)
constant JV_TABLE_HI(0x8013)
constant JV_TABLE_LO(jv_table - 0x80130000)
if JV_TABLE_LO >= 0x8000 {
    error "jv_table: adjust JV_TABLE_HI/LO"
}

// Edit 1: original -> J, except in 12-Character Battle. Entered by jal from
// 0x8013AA48 (ra = 0x8013AA50); goes on to syTaskmanSetLoadScene, which
// returns there. Nothing is live in caller-saved registers at that call.
jv_map:
    or      t8, ra, r0
    lui     t0, VS_MODE_FLAG_HI
    lw      t0, VS_MODE_FLAG_LO(t0)         // t0 = Remix VS mode
    ori     t1, r0, MODE_12CB
    beq     t0, t1, _map_done               // 12cb: keep the original ids
    or      a2, r0, r0                      // ids 0x00...
    jal     jv_remap_all
    ori     a3, r0, 0x000C                  // ...0x0B
_map_done:
    j       syTaskmanSetLoadScene
    or      ra, t8, r0

// Edit 2: J -> original, every time the screen opens. Entered by j from
// 0x8013B33C after its delay slot; no arguments are live.
jv_unmap:
    or      t8, ra, r0
    ori     a2, r0, 0x0024                  // ids 0x24...
    jal     jv_remap_all
    ori     a3, r0, 0x000F                  // ...0x32
    or      ra, t8, r0
    j       0x8013B344
    lui     t6, 0x800A                      // the instruction edit 2 replaced

// Remaps the ids a2 ... a2 + a3 - 1 through jv_table in the battle state's
// four players and in Tag Team's queue slots. A queue slot's +3 byte (the
// fighter Random resolved to) is left alone: with vanilla-css no slot can be
// Random (the original grid has no "?", and CPU and L-button picks are
// fighters). Clobbers a0, a1, v1, t0-t2, t9.
jv_remap_all:
    or      t9, ra, r0
    lui     a0, FKIND_P1_HI
    addiu   a0, a0, FKIND_P1_LO
    ori     a1, r0, 0x0074
    jal     jv_remap                        // battle state players[0-3].fkind
    addiu   v1, a0, 4 * 0x74
    lui     a0, TT_QUEUES_HI
    addiu   a0, a0, TT_QUEUES_LO
    ori     a1, r0, 0x0004
    jal     jv_remap                        // Tag Team queue slots (+0)
    addiu   v1, a0, 24 * 4
    jr      t9
    nop

// For a0 = first byte, a1 = stride, v1 = end: each byte in a2 ... a2 + a3 - 1
// becomes jv_table[byte].
jv_remap:
    lbu     t0, 0x0000(a0)
    subu    t1, t0, a2
    sltu    t1, t1, a3                      // in range?
    beqz    t1, _remap_next
    lui     t2, JV_TABLE_HI
    addu    t2, t2, t0
    lbu     t2, JV_TABLE_LO(t2)
    sb      t2, 0x0000(a0)
_remap_next:
    addu    a0, a0, a1
    bne     a0, v1, jv_remap
    nop
    jr      ra
    nop

constant jv_end(pc())
if jv_end > JV_SPACE_END {
    error "j-variants code overflows mnPlayersVSMakePortraitShadow"
}
