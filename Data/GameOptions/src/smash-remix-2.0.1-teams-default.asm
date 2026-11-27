// Smash Remix 2.0.1: "Teams by default" game option (id teams-default,
// hidden: turned on by a preset's player-count rule, never shown as a toggle)
//
// The game boots with Team Battle on, so the first character select screen
// (CSS) opens in Team Battle: the announcer says "Team battle!", panels have
// team buttons, and P1 and P2 start on red, P3 and P4 on blue (the game's own
// first-CSS reset). It is only the starting value: the CSS's Free-for-all /
// Team Battle button works as usual, and whatever the players pick stays,
// rematch after rematch, because the CSS writes its team flag back into the
// VS battle state when a match starts and reads it from there when it opens
// again (the VS mode menu keeps it the same way).
//
// Compare team-battle, which writes Team Battle into the CSS every time it
// opens and makes the button do nothing. The two conflict: with both on,
// team-battle's forcing would make this one pointless.
//
// The value is set once per boot, in Remix's Settings.set_vs_settings_,
// which writes Remix's VS defaults (timed stock, 8 minutes, 4 stocks, team
// attack on, item frequency none) into the VS battle state right after the
// game copies its default battle state there, before the first scene. So it
// holds with skip-to-css (logo straight to the CSS) and without it (title,
// main menu and VS mode menu first: they read the flag, they don't reset
// it). The team flag isn't part of save data, so a starting save doesn't
// change it. Team attack stays on, Remix's default.
//
// Works with or without the other options except team-battle; it touches no
// byte that any other option writes.
//
// This file is the source of the "teams-default" patches. To check or
// regenerate them, assemble it over a copy of the ROM with the bass v14 that
// ships with Remix (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-teams-default.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the edit lands
// --------------------
// Remix code and data: RAM 0x80400000, copied unchanged from ROM 0x02C00000
// by one boot DMA, so rom = ram - 0x7D800000. Outside the CIC checksum range
// (0x1000-0x101000). Settings.set_vs_settings_ is at 0x8046F3AC (bass symbol
// log of a v2.0.1 rebuild); it is reached from the hook at 0x800A1B48 in
// scManagerRunLoop, once per boot.

arch n64.cpu
endian msb

constant r0(0)
constant t0(8)
constant t1(9)

// Seeks to a RAM address in Remix's code/data.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

// ---------------------------------------------------------------------------
// Settings.set_vs_settings_, last block (item frequency):
//
//     lui  t0, 0x800A
//     ori  t0, t0, 0x4D24      // t0 = Global.vs.item_frequency (0x800A4D24)
//     ori  t1, r0, 0           // ITEM_FREQUENCY
//     sb   t1, 0x0000(t0)
//
// The block before it stored team attack with t1 = 1 (TEAM_ATTACK). Use that
// 1 for the team flag, Global.vs.teams = 0x800A4D0A = t0 - 0x1A, and store
// the item frequency (0) from r0.
seek(0x8046F400)
    sb      t1, -0x001A(t0)                 // was: ori t1, r0, 0 (0x34090000); is_team_battle = TRUE
    sb      r0, 0x0000(t0)                  // was: sb t1, 0x0000(t0) (0xA1090000); item frequency none, as before
