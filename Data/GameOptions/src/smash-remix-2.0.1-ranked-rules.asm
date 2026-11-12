// Smash Remix 2.0.1: "ranked-rules" game option (server only)
//
// What every ranked ruleset shares. The lobby puts this option on every
// ranked match; hosts never see it. With it:
//  - the game boots straight from the N64 logo to the VS character select
//    screen (CSS), skipping Remix's splash and the title screen
//  - only the CSS, stage select, battle and results are reachable: the CSS's
//    Back button and hold-B back do nothing, and the hold-L settings shortcut
//    (CSS and stage select) is gone. Stage select's B goes back to the CSS,
//    a battle (or its A+B+Z+R reset) ends on the CSS (Skip Results Screen),
//    and Remix already removed the CSS and stage select 5-minute idle return
//    to the title (Timeouts.asm).
//  - the rules are 4 stock, 8 minutes (Remix's timed stock mode), no items,
//    no handicap, 100% damage: set in the VS battle state at boot, and the CSS
//    controls that change them (stock/time arrows, the stock/time picker, the
//    Free-for-all/Team Battle button) do nothing
//  - Remix's toggles are the Tournament profile with two overrides: Skip
//    Results Screen on (straight back to the CSS) and Salty Runback off. The
//    CSS Panel Menu is off in that profile, so the per-port panel (input
//    delay, stock mode...) can't be opened.
//  - Remix's saved VS menu settings (VsRemixMenu.global_*) match the rules
//
// Teams (ranked-teams), the team colours (teams-p1pN) and the stages
// (stages-dream-land / stages-remix-ranked) are separate options and don't
// touch any byte this file writes. They rely on its locks, and ranked-teams,
// teams-p1pN and stages-dream-land keep their boot code in the body of
// Toggles.settings_shortcut, which edit 3 below makes unreachable, so they
// need this option. The lobby always sends them together.
//
// This file is the source of the "ranked-rules" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-ranked-rules.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the code lives
// --------------------
// Remix code and data: assembled at RAM 0x80400000, copied unchanged from ROM
// 0x02C00000 by one DMA at boot (see smash-remix-2.0.1-vanilla-only.asm), so
// rom = ram - 0x7D800000.
//
// Vanilla overlays: SSB64 loads each scene's overlay with a plain DMA from
// ROM to a fixed RAM address (syDmaLoadOverlay: no relocation, no
// decompression), so an overlay's code can be patched at its ROM offset. The
// offsets come from Remix's own OS.patch_start(rom, ram) pairs for the same
// overlays (Boot.asm for the N64 logo, VsRemixMenu.asm for the VS CSS):
//   N64 logo (mnStartup, scene 0x1B):  rom = ram - 0x7FFB2E40
//   VS CSS   (mnPlayersVS, scene 0x10): rom = ram - 0x80001D80
// Every overlay edit below is outside the CIC boot checksum range
// (0x1000-0x101000), so the header CRCs don't change.
//
// The values the ranked module used to pin every frame are now ROM defaults
// (Remix data) or written once at boot (vanilla RAM that the game sets up at
// runtime). Netplay boots with empty save data, so Remix never loads saved
// toggles (Boot.load_ calls SRAM.initialize_ instead of Toggles.load_) and
// every toggle starts at its assembled value.
//
// Function names are from the SSB64 decomp (ssb-decomp-re); Remix names and
// addresses from the v2.0.1 symbol log (Data/Ranked/SmashRemix/<MD5>.sym).

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant v0(2)
constant a0(4)
constant t0(8)
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
// Seeks to a RAM address in the VS character select overlay.
macro seek_css(variable ram) {
    origin ram - 0x80001D80
    base ram
}

// vanilla
constant syTaskmanSetLoadScene(0x80005C74)
constant VS_BATTLE_HI(0x800A)           // gSCManagerTransferBattleState = 0x800A4D08
constant SCENE_VS_CSS(0x10)

// Remix
constant Toggles_settings_shortcut(0x8046EFCC)
constant CharacterSelect_check_picker_press_(0x80522EC0)
constant ranked_boot_init(0x8046EFD4)   // in settings_shortcut's unreachable body

// ---------------------------------------------------------------------------
// 1. Boot straight to the VS character select screen
//
// mnStartupActorFuncRun (N64 logo, 0x80131C20): when the logo ends it sets
// the next scene. Remix (Boot.asm) made that 0x37, its splash (shown with
// the 1P victory screen), which goes on to the title. Make it the VS CSS
// (0x10). The ranked module did the same in RAM (kLogoNextScene).
seek_logo(0x80131C94)
    ori     t1, r0, SCENE_VS_CSS            // was: ori t1, r0, 0x37 (0x34090037)

// ---------------------------------------------------------------------------
// 2. Boot-time rules
//
// The same function then calls syTaskmanSetLoadScene. Call ranked_boot_init
// instead, which sets the rules in the VS battle state and then jumps to
// syTaskmanSetLoadScene. It runs once, after scManagerRunLoop has copied
// dSCManagerDefaultBattleState (time rule, 3 minutes, 2 stocks, all items at
// middle rate) into gSCManagerTransferBattleState and before the CSS first
// reads it (mnPlayersVSInitVars). Boot code because that default lives in the
// main segment (ROM 0x42D18), inside the CIC-checksummed first MiB.
seek_logo(0x80131CA0)
    jal     ranked_boot_init                // was: jal syTaskmanSetLoadScene (0x0C00171D)

// ---------------------------------------------------------------------------
// 3. Toggles.settings_shortcut (Toggles.asm ~3260): hold L on the CSS or
// stage select opens Remix's settings. The CSS (CharacterSelect.asm ~2581),
// 12cb CSS and stage select (Stages.asm ~1880) call it each frame and leave
// the screen when it returns a0 = TRUE. Return FALSE right away.
//
// The rest of the function (0x8046EFD4-0x8046F1AC) is then unreachable: its
// only entry is the top (a whole-ROM scan finds no jump, branch, pointer or
// lui/addiu|ori pair that targets 0x8046EFD0-0x8046F1A8 from outside the
// function). This option's boot code and the code of ranked-teams,
// teams-p1pN and stages-dream-land live there, each in its own words:
//   0x8046EFD4-0x8046F013  ranked-rules
//   0x8046F020-0x8046F03F  ranked-teams
//   0x8046F040-0x8046F07F  teams-p1p2 / teams-p1p3 / teams-p1p4
//   0x8046F080-0x8046F09F  stages-dream-land
seek(Toggles_settings_shortcut)
    jr      ra                              // was: addiu sp, sp, -0x10 (0x27BDFFF0)
    addu    a0, r0, r0                      // was: sw t0, 0x0004(sp) (0xAFA80004)  a0 = FALSE

// The rules in gSCManagerTransferBattleState (SCBattleState at 0x800A4D08).
// The CSS loads its copy of time, stocks, team battle and game rule from
// here when it opens (mnPlayersVSInitVars) and writes them back when a match
// starts (mnPlayersVSSetSceneData). Edits 4 and 5 keep the CSS from changing
// them; only the VS menus (unreachable, edits 1, 3, 5, 6) change the rest.
// The game rule is also set from VsRemixMenu.global_game_mode at match start
// (edit 8).
seek(ranked_boot_init)
    lui     t0, VS_BATTLE_HI
    ori     t1, r0, 3
    sb      t1, 0x4D0B(t0)                  // +0x03 game_rules = 3 (time + stock: Remix's timed stock)
    sb      t1, 0x4D0F(t0)                  // +0x07 stocks = 3 (4 stocks, 0-based)
    ori     t1, r0, 8
    sb      t1, 0x4D0E(t0)                  // +0x06 time_limit = 8 minutes
    sb      r0, 0x4D10(t0)                  // +0x08 handicap = off
    ori     t1, r0, 100
    sb      t1, 0x4D13(t0)                  // +0x0B damage_ratio = 100%
    sw      r0, 0x4D14(t0)                  // +0x0C item_toggles = none
    j       syTaskmanSetLoadScene           // the call this replaced
    sb      r0, 0x4D24(t0)                  // +0x1C item_appearance_rate = none
    // is_team_battle (+0x02) and is_team_attack (+0x09) stay off (the
    // default) unless ranked-teams is on; is_stage_select (+0x0A) stays on
    // (the default) unless stages-dream-land is on.

// ---------------------------------------------------------------------------
// 4. CharacterSelect.check_picker_press_ (CharacterSelect.asm ~6298): in
// timed stock mode, clicking the stock/time display flips the CSS between
// showing stocks and the time limit and lets the arrows edit either. Return
// right away (the ranked module wrote the same two words).
seek(CharacterSelect_check_picker_press_)
    jr      ra                              // was: addiu sp, sp, -0x30 (0x27BDFFD0)
    nop                                     // was: sw ra, 0x0004(sp) (0xAFBF0004)

// ---------------------------------------------------------------------------
// 5. VS CSS: stock/time arrows, Free-for-all/Team Battle, Back
//
// mnPlayersVSFuncRun's cursor handler (decomp mnplayersvs.c ~3318) asks
// these "is the cursor over the button" functions on an A press and acts on
// TRUE. Each returns FALSE now, so the arrows can't change the stock count
// or time, the team button can't switch teams on or off, and Back does
// nothing. Same words the ranked module wrote when the overlay loaded.
macro not_in_range(variable ram) {
    seek_css(ram)
    jr      ra
    or      v0, r0, r0                      // v0 = FALSE
}
not_in_range(0x80134F64)                    // mnPlayersVSCheckTimeArrowRInRange; was: lw v0, 0x74(a0); lui at, 0x4040
not_in_range(0x8013502C)                    // mnPlayersVSCheckTimeArrowLInRange; was: lw v0, 0x74(a0); lui at, 0x4040
not_in_range(0x80135270)                    // mnPlayersVSCheckGameModeInRange; was: j 0x804F7C24 (Remix hook); nop
not_in_range(0x80138218)                    // mnPlayersVSCheckBackInRange; was: lw v0, 0x74(a0); lui at, 0x4040

// ---------------------------------------------------------------------------
// 6. mnPlayersVSBackToVSMode (0x801380F4): leaves the CSS for the VS menu.
// Called by the Back button (edit 5) and by holding B for 40 frames
// (mnPlayersVSDetectBack). Return right away.
seek_css(0x801380F4)
    jr      ra                              // was: lui v0, 0x800A (0x3C02800A)
    nop                                     // was: addiu v0, v0, 0x4AD0 (0x24424AD0)

// ---------------------------------------------------------------------------
// 7. Toggles: the Tournament profile, with Skip Results Screen on and Salty
// Runback off
//
// Each toggle is a Menu.entry; its current value is the word at +0x04,
// assembled with the Community (CE) default. These are the entries in
// Toggles.profile_head_table (Remix, gameplay, music, stage, Pokemon
// settings) whose Tournament (TE) value differs, plus the two overrides.
// The random stage toggles belong to the stage options; the hazard mode
// toggle is NORMAL (0) in both profiles and stays so.
macro toggle(variable entry, variable value) {
    seek(entry + 4)
    dw value
}
toggle(0x80461C6C, 1)                       // Skip Results Screen: OFF -> ON (override; TE is OFF)
toggle(0x80461CA8, 1)                       // Hold To Pause: OFF -> ON
toggle(0x80461CE0, 0)                       // CSS Panel Menu: ON -> OFF (closes the per-port panel: input delay, stock mode...)
toggle(0x80461EBC, 1)                       // Hold To Exit Training: OFF -> ON
toggle(0x80461EFC, 0)                       // Improved Combo Meter: ON -> OFF
toggle(0x80461F3C, 0)                       // Tech Chase Combo Meter: ON -> OFF
toggle(0x80461F7C, 0)                       // Combo Meter: ON -> OFF
toggle(0x80462028, 0)                       // Salty Runback: 1 -> 0 (OFF; also the override)
toggle(0x80462200, 10)                      // Default CPU LVL (V.S.): 0 -> 10
toggle(0x804625D4, 0)                       // Improved AI: ON -> OFF
toggle(0x80462B98, 0)                       // Music Title at Match Start: ON -> OFF
toggle(0x80466ED0, 1)                       // Stage Select Layout: NORMAL -> TOURNAMENT
toggle(0x80466FB4, 2)                       // Pokemon Announcer: POKEMON (0) -> OFF (2)
toggle(0x80466FF0, 2)                       // Dragon King HUD: DEFAULT (0) -> OFF (2)
toggle(0x804670A0, 1)                       // Stage settings "Load Profile:": 0 -> 1 (menu only)

// ---------------------------------------------------------------------------
// 8. Remix's saved VS menu settings (VsRemixMenu.asm ~30)
//
// VsRemixMenu.mode_specific_setup_start_ copies global_game_mode into the
// CSS's game rule when a match starts, so it must be timed stock (3).
// The rest are restored when leaving the CSS for the VS menu (blocked by
// edits 5 and 6) or used by Remix modes; they're set to the rules too, as
// the ranked module did. global_time is already 8, vs_mode_flag 0 (normal
// VS), global_stockmode_table all 0 (normal stock mode).
seek(0x804F756C)                            // VsRemixMenu.global_game_mode
    dw 3                                    // was: 1
seek(0x804F7574)                            // VsRemixMenu.global_stocks
    dw 3                                    // was: 8
