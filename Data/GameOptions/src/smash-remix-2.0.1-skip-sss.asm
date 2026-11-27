// Smash Remix 2.0.1: "Skip stage select" game option (id skip-sss)
//
// From the character select screen (CSS), Start goes straight to the
// battle; there is no stage select. The stage is drawn at random from the
// stages in play, the way the game picks one when Stage Select is off in the
// VS options:
//   - with a stage list (stage-list): among the list's stages, each with the
//     list's hazard mode; a one-stage list always gives that stage;
//   - with vanilla-sss: among the original 9;
//   - with no stage option: Remix's own Random (its random stage toggles).
// As with Stage Select off in the original game, the stage just played is
// not drawn again for the next match (unless it is the only one).
//
// Players can't bring the stage select back: the CSS ignores the VS options'
// Stage Select setting, and that row of the VS options menu always shows Off
// and doesn't change. It holds in every VS mode that uses the VS CSS (normal
// VS and Remix's 12-Character Battle, Tag Team, King of the Hill,
// Smashketball, Tug of War). Training, 1P and Bonus have their own screens
// and aren't affected. Stage Select isn't part of save data, so a starting
// save doesn't change any of this; which stages Random can draw follows the
// stage option in play (the stage options keep a save's stage settings from
// loading).
//
// The old "Dream Land only" option (stages-dream-land) also skipped the stage
// select, differently: it turned Stage Select off once at boot (a hook in the
// N64 logo's start function and VsRemixMenu.global_stage_select = 0), so a
// player could turn it back on in the VS options. This option replaces that
// part; it shares no byte with it.
//
// Works with or without the other options; it touches no byte that any of
// them writes.
//
// This file is the source of the "skip-sss" patches. To check or regenerate
// them, assemble it over a copy of the ROM with the bass v14 that ships with
// Remix (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-skip-sss.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the edits land
// --------------------
// VS CSS overlay (ovl26, scene 0x10): loaded uncompressed from ROM 0x12FD80
// to RAM 0x80131B00 whenever the screen opens, so rom = ram - 0x80001D80. VS
// options overlay (ovl20, mnVSOptions, scene 0x0A): loaded from ROM 0x1252E0
// to RAM 0x80131B00, so rom = ram - 0x8000C820. Both outside the CIC
// checksum range (0x1000-0x101000). Names from ssb-decomp-re; Remix hooks
// from the bass symbol log of a v2.0.1 rebuild.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant v1(3)
constant t0(8)
constant t2(10)
constant t3(11)
constant t9(25)

// Seeks to a RAM address in the VS CSS overlay (ovl26).
macro seek_css(variable ram) {
    origin ram - 0x80001D80
    base ram
}
// Seeks to a RAM address in the VS options overlay (ovl20).
macro seek_vs_options(variable ram) {
    origin ram - 0x8000C820
    base ram
}

// ---------------------------------------------------------------------------
// 1. mnPlayersVSFuncRun (0x8013A920): the scene after the CSS
//
// 30 frames after Start, the CSS loads gSCManagerTransferBattleState
// .is_stage_select (0x800A4D12) and goes to the stage select (scene 0x15)
// when it is set; otherwise it goes to the battle (0x16) and picks the stage
// through Remix's Stages.swap_stage_._stage_select_off_begin (Random's list,
// the tournament hazard mode on the tournament layout), repeating the draw
// while it equals the stage last played (Remix's one-stage guard makes a
// one-stage list end that loop). Load 0 instead.
seek_css(0x8013A9E0)
    or      t9, r0, r0                      // was: lbu t9, 0x4D12(t9) (0x93394D12)

// 1b. Same routine, 0x8013AA00: the battle state pointer for that last frame
//
// The CSS writes scene 0x16 here, and its fighter models keep animating
// (and playing sounds) for the rest of the frame. Remix's
// SFXReplace.sfx_replace treats scene 0x16 as "in a match" and reads the
// stage id through gSCManagerBattleState (0x800A50E8), which is NULL until
// the first battle of the boot sets it: a sound in that frame read address
// 1 and stopped the game thread (the game froze on the way to the first
// battle; seen with CPU Lanky Kong and Young Link). Remix's own Stage
// Select Off has the same bug; this option makes that path the only one,
// so point gSCManagerBattleState at gSCManagerTransferBattleState here, as
// the VS battle does when it starts (scVSBattleStartScene). The original
// instructions here count the stages for the original game's random pick,
// which Remix's swap_stage_ (called next, at 0x8013AA1C) replaced; their
// result (s0) is never read. t2 = 0x800A0000 (lui at 0x8013A9E8).
seek_css(0x8013AA00)
    addiu   t3, t2, 0x4D08                  // was: lbu t2, 0x4937(t2) (0x914A4937); t3 = &gSCManagerTransferBattleState
    sw      t3, 0x50E8(t2)                  // was: addiu s0, r0, 8 (0x24100008); gSCManagerBattleState = t3
    b       0x8013AA1C                      // was: andi t3, t2, 0x10 (0x314B0010); on to Remix's stage pick
    nop                                     // was: beqz t3, 0x8013AA1C (0x11600003)

// ---------------------------------------------------------------------------
// 2. mnVSOptionsInitVars (0x80133878): the menu shows Stage Select Off
//
// The VS options menu copies is_stage_select into its row's status when it
// opens (and writes the status back when it closes). Remix sets the flag
// again from VsRemixMenu.global_stage_select (default on) when the CSS goes
// back to the VS mode menu, so open the row as Off whatever the flag says.
seek_vs_options(0x801338C8)
    or      t0, r0, r0                      // was: lbu t0, 0x000A(v0) (0x9048000A)

// ---------------------------------------------------------------------------
// 3-4. The row's toggles: left/right (0x80133FC4, sets On when it is Off)
// and A (0x801344A4, flips it). Store Off where they would store On, so the
// row plays its sound and stays Off.
seek_vs_options(0x80133FDC)
    sw      r0, 0x48EC(at)                  // was: sw t3, 0x48EC(at) (0xAC2B48EC); delay slot of the sound call
seek_vs_options(0x801344D0)
    sw      r0, 0x0000(v1)                  // was: sw t6, 0x0000(v1) (0xAC6E0000)
