// Smash Remix 2.0.1: "Teams" game option (id team-battle)
//
// VS matches are team battles with team attack on. The character select
// screen (CSS) opens in Team Battle and its Free-for-all/Team Battle button
// does nothing, so the room can't drift back to free-for-all. Players pick
// their team colors on their panels as usual; the first time the CSS opens,
// P1 and P2 start on red and P3 and P4 on blue (the game's default).
//
// Every time the VS CSS opens, team battle and team attack are written into
// the VS battle state, so a change made in the VS options menu (team attack)
// or left behind by one of Remix's modes is undone before the next match.
// Only normal VS is affected: Remix's own modes keep their own team rules
// and the button keeps working there.
//
// Works with or without the other options; it touches no byte that
// vanilla-css, tournament-rules, skip-to-css, tournament-profile,
// random-vanilla or the stage options write. This is the casual-room
// version of ranked-ssb's ranked-teams; it doesn't need tournament-rules.
// (The id isn't "teams" because ranked-ssb already has a
// smash-remix-2.0.1-teams.asm, the source of its teams-p1pN options.)
//
// This file is the source of the "team-battle" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-team-battle.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the code lives
// --------------------
// Remix code and data: RAM 0x80400000, copied unchanged from ROM 0x02C00000
// by one boot DMA, so rom = ram - 0x7D800000. VS CSS overlay (ovl26):
// loaded uncompressed from ROM 0x12FD80 to RAM 0x80131B00 whenever the
// screen opens, so rom = ram - 0x80001D80. Addresses are from the bass
// symbol log of a v2.0.1 rebuild and the ssb-decomp-re names. All edits are
// outside the CIC checksum range (0x1000-0x101000).
//
// The new code sits in Hazards.peach_castle_II_setup_ (RAM
// 0x804906B0-0x8049074B), which nothing calls: the source never references
// it, and a scan of every aligned ROM word finds no j/jal, branch, pointer or
// lui+addiu/ori/load/store pair that reaches into it.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant v0(2)
constant t0(8)
constant t1(9)
constant s2(18)
constant ra(31)

// Seeks to a RAM address in Remix's code/data.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}
// Seeks to a RAM address in the VS CSS overlay (ovl26).
macro seek_css(variable ram) {
    origin ram - 0x80001D80
    base ram
}

constant VS_MODE_FLAG_HI(0x804F)        // VsRemixMenu.vs_mode_flag = 0x804F7568 (0 = normal VS)
constant VS_MODE_FLAG_LO(0x7568)
constant disable_mode_toggle_(0x804F7C24)   // VsRemixMenu.disable_mode_toggle_

constant code_start(0x804906B0)         // Hazards.peach_castle_II_setup_
constant code_end(0x8049074C)

// ---------------------------------------------------------------------------
// 1. mnPlayersVSInitVars (0x8013AEC8): team battle when the CSS opens
//
// InitVars loads is_team_battle from the VS battle state (s2 = 0x800A4D08)
// into t0; Remix's TwelveCharBattle.force_ffa_and_stock_ then stores t0 as
// the CSS's team flag (sMNPlayersVSIsTeamBattle, 0x8013BDA8), forcing FFA in
// 12-Character Battle. Hook that load: in normal VS make it TRUE and also
// set is_team_battle and is_team_attack in the battle state (the CSS
// announcer and team panels read the battle state; team attack is only set
// from the VS options menu). The delay slot (lui at, 0x8014) runs before the
// code and at stays untouched; t1 is free until force_ffa_and_stock_.
seek_css(0x8013AF18)
    j       teams_init                      // was: lbu t0, 0x0002(s2) (0x92480002); delay slot (lui at, 0x8014) kept

// ---------------------------------------------------------------------------
// 2. mnPlayersVSCheckGameModeInRange (0x80135270): the FFA/Team Battle button
//
// The CSS cursor handler asks this on an A press and switches between
// free-for-all and team battle on TRUE. Remix hooked it with
// `j VsRemixMenu.disable_mode_toggle_` (which blocks it in 12-Character
// Battle). In normal VS answer FALSE; in Remix's modes go on to Remix's
// routine as before.
seek_css(0x80135270)
    j       teams_button                    // was: j VsRemixMenu.disable_mode_toggle_ (0x0813DF09); delay slot (nop) kept

// ---------------------------------------------------------------------------
// The code for 1-2
seek(code_start)
teams_init:
    lbu     t0, 0x0002(s2)                  // the instruction the hook replaced
    lui     t1, VS_MODE_FLAG_HI
    lw      t1, VS_MODE_FLAG_LO(t1)         // t1 = VsRemixMenu.vs_mode_flag
    bnez    t1, _init_done                  // Remix modes keep their own team rules
    nop
    ori     t0, r0, 1                       // team battle
    sb      t0, 0x0002(s2)                  // battle state is_team_battle
    sb      t0, 0x0009(s2)                  // battle state is_team_attack
_init_done:
    j       0x8013AF20                      // back after the delay slot
    nop

teams_button:
    lui     at, VS_MODE_FLAG_HI
    lw      at, VS_MODE_FLAG_LO(at)
    bnez    at, _remix_mode
    nop
    jr      ra
    or      v0, r0, r0                      // FALSE: the cursor isn't on the button
_remix_mode:
    j       disable_mode_toggle_
    nop

if pc() > code_end {
    error "team-battle code overflows Hazards.peach_castle_II_setup_"
}
