// Smash Remix 2.0.1: "Skip to character select" game option
//
// The game boots from the N64 logo straight to the VS character select
// screen (CSS), skipping Remix's splash, the title screen and the menus.
// Holding B on the CSS no longer goes back to the VS mode menu, so nobody
// leaves the CSS by accident when they hold B to put a character back; the
// Back button at the top of the screen still works for a player who wants
// the menus (VS options, Remix's modes, other modes).
//
// After a match the game comes back to the CSS as usual (through the results
// screen, unless Remix's Skip Results Screen toggle is on), so the room
// stays on CSS -> stage select -> battle -> results -> CSS.
//
// The rules are whatever the VS battle state holds: Remix boots with 4
// stocks, 8 minutes timed stock, items off. Normally leaving the VS mode menu
// saves the rule into VsRemixMenu.global_game_mode and global_stocks, and a
// match starts with that saved rule; with the menu skipped they kept their
// assembled values (Time, 9 stocks), so the first match would have been a
// time match even though the CSS shows 4 stocks. They are set to what the
// menu would have saved (timed stock, 4 stocks). Players can still change the
// rules through the Back button and the VS mode menu, unless the
// tournament-rules option is on.
//
// Works with or without the other options; it touches no byte that
// vanilla-css, tournament-rules, team-battle, tournament-profile,
// random-vanilla or the stage options write.
//
// This file is the source of the "skip-to-css" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-skip-to-css.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the edits land
// --------------------
// N64 logo overlay (mnStartup, scene 0x1B): SSB64 loads each scene's overlay
// with a plain DMA from ROM, so rom = ram - 0x7FFB2E40 there (Remix's own
// OS.patch_start pairs in Boot.asm). Remix data: rom = ram - 0x7D800000. VS
// CSS overlay (ovl26): rom = ram - 0x80001D80. All edits are outside the CIC
// checksum range (0x1000-0x101000). Addresses are from the bass symbol log of
// a v2.0.1 rebuild and the ssb-decomp-re names. ranked-ssb's ranked-rules
// makes the same boot edit (1) and the same saved-rule edits (2).

arch n64.cpu
endian msb

constant r0(0)
constant t1(9)
constant ra(31)

// Seeks to a RAM address in Remix's code/data.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}
// Seeks to a RAM address in the N64 logo overlay.
macro seek_logo(variable ram) {
    origin ram - 0x7FFB2E40
    base ram
}
// Seeks to a RAM address in the VS CSS overlay (ovl26).
macro seek_css(variable ram) {
    origin ram - 0x80001D80
    base ram
}

constant SCENE_VS_CSS(0x10)

// ---------------------------------------------------------------------------
// 1. mnStartupActorFuncRun (N64 logo, 0x80131C20): next scene
//
// When the logo ends it sets the next scene. Remix (Boot.asm) made that
// 0x37, its splash (shown with the 1P victory screen), which goes on to the
// title. Make it the VS CSS.
seek_logo(0x80131C94)
    ori     t1, r0, SCENE_VS_CSS            // was: ori t1, r0, 0x37 (0x34090037)

// ---------------------------------------------------------------------------
// 2. VsRemixMenu.global_game_mode and global_stocks (VsRemixMenu.asm ~30)
//
// VsRemixMenu.save_global_settings_ copies the battle state's rule, time,
// stocks, teams and stage select into these when the VS mode menu is left,
// and mode_specific_setup_start_ copies global_game_mode into the CSS's
// game rule when a normal VS match starts. Give the two that differ from
// Remix's boot battle state (Settings.set_vs_settings_: timed stock, 8
// minutes, 4 stocks) the values the menu would have saved. global_time is
// already 8; teams (off) and stage select (on) already match.
seek(0x804F756C)                            // VsRemixMenu.global_game_mode
    dw 3                                    // was: 1 (Time); now timed stock
seek(0x804F7574)                            // VsRemixMenu.global_stocks
    dw 3                                    // was: 8; now 4 stocks (0-based)

// ---------------------------------------------------------------------------
// 3. mnPlayersVSDetectBack (0x80138140): hold B to go back
//
// Called for each port every frame; after B is held for 40 frames it calls
// mnPlayersVSBackToVSMode. Return right away. The Back button
// (mnPlayersVSCheckBackInRange) also calls mnPlayersVSBackToVSMode and is
// left alone.
seek_css(0x80138140)
    jr      ra                              // was: sll t6, a0, 2 (0x00047080)
    nop                                     // was: subu t6, t6, a0 (0x01C47023)
