// Smash Remix 2.0.1: "ranked-teams" game option (server only)
//
// Team battle with team attack, for the 2v2 rulesets. Without this option a
// ranked match is free-for-all (the VS battle state's default).
//
// Needs ranked-rules: its code sits in the body of Toggles.settings_shortcut,
// which ranked-rules makes unreachable (see that file), and ranked-rules
// keeps the CSS's team button from switching teams off again.
//
// Assemble over a copy of the ROM to check the JSON (see
// smash-remix-2.0.1-ranked-rules.asm):
//   bass.exe -o patched.z64 smash-remix-2.0.1-ranked-teams.asm
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.

arch n64.cpu
endian msb

constant r0(0)
constant t0(8)
constant t1(9)

macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}
macro seek_logo(variable ram) {
    origin ram - 0x7FFB2E40
    base ram
}

constant lbRelocInitSetup(0x800CDF78)
constant VS_BATTLE_HI(0x800A)           // gSCManagerTransferBattleState = 0x800A4D08
constant teams_boot_init(0x8046F020)    // settings_shortcut body, this option's words

// ---------------------------------------------------------------------------
// 1. mnStartupFuncStart (N64 logo scene start, 0x80131CB8) calls
// lbRelocInitSetup(&rl_setup) once, after scManagerRunLoop has set up the VS
// battle state and before the CSS first reads it. Call teams_boot_init
// instead; it jumps on to lbRelocInitSetup with a0 untouched.
seek_logo(0x80131D10)
    jal     teams_boot_init                 // was: jal lbRelocInitSetup (0x0C0337DE); delay slot (addiu a0, sp, 0x60) kept

// 2. The CSS loads team battle from the battle state when it opens
// (mnPlayersVSInitVars) and writes it back at match start. Team attack is
// only set from the VS options menu, which ranked-rules makes unreachable.
seek(teams_boot_init)
    lui     t0, VS_BATTLE_HI
    ori     t1, r0, 1
    sb      t1, 0x4D0A(t0)                  // +0x02 is_team_battle = on
    j       lbRelocInitSetup
    sb      t1, 0x4D11(t0)                  // +0x09 is_team_attack = on

// 3. VsRemixMenu.global_teams: Remix's saved VS menu team flag, restored into
// the battle state when the CSS is left for the VS menu (blocked by
// ranked-rules). Set to on, as the ranked module did.
seek(0x804F7578)
    dw 1                                    // was: 0 (OS.FALSE)
