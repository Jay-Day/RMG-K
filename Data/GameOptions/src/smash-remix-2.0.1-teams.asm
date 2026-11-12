// Smash Remix 2.0.1: "teams-p1p2", "teams-p1p3", "teams-p1p4" game options
// (server only)
//
// For a 2v2 set whose teams the lobby matched: P1's teammate is port
// TEAMMATE. The VS character select screen opens with all four ports human,
// no characters picked, P1 and TEAMMATE on the red team and the other two on
// blue. Players may change colours afterwards (the ranked module only checks
// the pairing). This is what the ranked module wrote during the N64 logo.
//
// Used with ranked-rules and ranked-teams (its code sits in the body of
// Toggles.settings_shortcut, which ranked-rules makes unreachable). The three
// options patch the same bytes with different team colours, so only one can
// be on at a time; the lobby picks one per match.
//
// Assemble once per option over a copy of the ROM to check the JSON (see
// smash-remix-2.0.1-ranked-rules.asm):
//   bass.exe -d TEAMMATE=2 -o patched.z64 smash-remix-2.0.1-teams.asm   (teams-p1p2)
//   bass.exe -d TEAMMATE=3 -o patched.z64 smash-remix-2.0.1-teams.asm   (teams-p1p3)
//   bass.exe -d TEAMMATE=4 -o patched.z64 smash-remix-2.0.1-teams.asm   (teams-p1p4)
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.

arch n64.cpu
endian msb

if !{defined TEAMMATE} {
    error "assemble with -d TEAMMATE=2, 3 or 4"
}
if {TEAMMATE} < 2 || {TEAMMATE} > 4 {
    error "TEAMMATE must be 2, 3 or 4"
}

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

constant gcMakeGObjSPAfter(0x80009968)
constant VS_BATTLE_HI(0x800A)           // gSCManagerTransferBattleState = 0x800A4D08
constant pairing_boot_init(0x8046F040)  // settings_shortcut body, these options' words

// SCBattleState: +0x10 is_reset_players; players[4] at +0x20, 0x74 bytes
// each (SCPlayerData: +0x02 pkind 0 = human, +0x04 team 0 = red, 1 = blue)
constant RESET_PLAYERS(0x4D18)
constant P1(0x4D28)
constant P2(0x4D9C)
constant P3(0x4E10)
constant P4(0x4E84)

// ---------------------------------------------------------------------------
// 1. mnStartupFuncStart (N64 logo scene start, 0x80131CB8) calls
// gcMakeGObjSPAfter once, after scManagerRunLoop has set up the VS battle
// state and before the CSS first reads it. Call pairing_boot_init instead;
// it jumps on to gcMakeGObjSPAfter with a0-a3 untouched.
seek_logo(0x80131D28)
    jal     pairing_boot_init               // was: jal gcMakeGObjSPAfter (0x0C00265A); delay slot (lui a3, 0x8000) kept

// 2. When the CSS first opens it resets every slot to its defaults if
// is_reset_players is set (mnPlayersVSInitVars), which it is at boot.
// Clear it, so the CSS restores each slot from the battle state instead:
// every port human, character NONE (the boot default), and these team
// colours. In team battle (ranked-teams) the CSS shows them as is.
seek(pairing_boot_init)
    lui     t0, VS_BATTLE_HI
    ori     t1, r0, 1                       // t1 = blue
    sb      r0, RESET_PLAYERS(t0)           // is_reset_players = FALSE
    sb      r0, P1 + 0x02(t0)               // P1 human
    sb      r0, P2 + 0x02(t0)               // P2 human
    sb      r0, P3 + 0x02(t0)               // P3 human
    sb      r0, P4 + 0x02(t0)               // P4 human
    sb      r0, P1 + 0x04(t0)               // P1 red
if {TEAMMATE} == 2 {
    sb      r0, P2 + 0x04(t0)               // P2 red (P1's teammate)
} else {
    sb      t1, P2 + 0x04(t0)               // P2 blue
}
if {TEAMMATE} == 3 {
    sb      r0, P3 + 0x04(t0)               // P3 red (P1's teammate)
} else {
    sb      t1, P3 + 0x04(t0)               // P3 blue
}
    j       gcMakeGObjSPAfter
if {TEAMMATE} == 4 {
    sb      r0, P4 + 0x04(t0)               // P4 red (P1's teammate)
} else {
    sb      t1, P4 + 0x04(t0)               // P4 blue
}
