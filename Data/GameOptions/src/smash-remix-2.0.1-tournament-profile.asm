// Smash Remix 2.0.1: "Tournament settings" game option
//
// Remix's settings (hold L on the character or stage select screen) start
// on Remix's own Tournament profile instead of Community: hold Start to
// pause, CSS Panel Menu off, combo meters off, Salty Runback off, CPUs at
// level 10 with the original AI, no music title at match start, Pokemon
// announcer and Dragon King HUD off. The "Load Profile" line at the top of
// the settings menu shows Tournament too.
//
// These are defaults, not locks: players can still change any of them in
// the settings menu during the match. Netplay boots with empty save data, so
// Remix starts every toggle at its assembled value (Boot.load_ calls
// SRAM.initialize_ instead of Toggles.load_); this option changes those
// values. If the host picked a starting save that holds Remix settings,
// Remix loads those instead and this option has no effect.
//
// Left out on purpose: the Stage Settings toggles that belong to the stage
// options (Stage Select Layout, Hazard Mode, the random stage list and the
// stage profile selector), and Skip Results Screen (off in the Tournament
// profile; ranked-ssb turns it on). Because the stage toggles aren't set,
// the settings menu's "Current Profile" line usually reads Custom.
//
// Works with or without the other options; it touches no byte that
// vanilla-css, tournament-rules, team-battle, skip-to-css, random-vanilla
// or the stage options write.
//
// This file is the source of the "tournament-profile" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-tournament-profile.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Each toggle is a Menu.entry in Remix's data (RAM 0x80400000, copied from
// ROM 0x02C00000 at boot, so rom = ram - 0x7D800000); its current value is
// the word at +0x04, assembled with the Community (CE) default. The entries
// below are those in Toggles.profile_head_table (Remix, Gameplay, Music,
// Stage and Pokemon settings) whose Tournament (TE) default differs, minus
// the stage ones listed above (src/Toggles.asm ~2379-2560, entry(...,
// default_ce, default_te, ...)). Addresses are from the bass symbol log of a
// v2.0.1 rebuild. ranked-ssb's ranked-rules writes the same words.

arch n64.cpu
endian msb

// Sets a toggle's current value (Menu.entry + 0x04).
macro toggle(variable entry, variable value) {
    origin entry + 4 - 0x80400000 + 0x02C00000
    base entry + 4
    dw value
}

toggle(0x80461AA8, 1)                       // head_super_menu "Load Profile:" selector: Community (0) -> Tournament (1) (menu only)
toggle(0x80461CA8, 1)                       // Hold To Pause: OFF -> ON
toggle(0x80461CE0, 0)                       // CSS Panel Menu: ON -> OFF (per-port panel: stock mode, size, input delay...)
toggle(0x80461EBC, 1)                       // Hold To Exit Training: OFF -> ON
toggle(0x80461EFC, 0)                       // Improved Combo Meter: ON -> OFF
toggle(0x80461F3C, 0)                       // Tech Chase Combo Meter: ON -> OFF
toggle(0x80461F7C, 0)                       // Combo Meter: ON -> OFF
toggle(0x80462028, 0)                       // Salty Runback: 1 -> 0 (OFF)
toggle(0x80462200, 10)                      // Default CPU LVL (V.S.): 0 (uses 3) -> 10
toggle(0x804625D4, 0)                       // Improved AI: ON -> OFF
toggle(0x80462B98, 0)                       // Music Title at Match Start: ON -> OFF
toggle(0x80466FB4, 2)                       // Pokemon Announcer: POKEMON (0) -> OFF (2)
toggle(0x80466FF0, 2)                       // Dragon King HUD: DEFAULT (0) -> OFF (2)
