// Smash Remix 2.0.1: "J variants" game option (id j-variants)
//
// Requires vanilla-css. With both on, players pick among the original 12
// fighters on the original VS character select screen, and every pick
// (human, CPU or random) is the Japanese-version variant Remix ships:
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
// The select screen keeps working with the original ids: the portraits, the
// tokens, the costumes and the battle state it restores are the original
// screen's. Two kinds of edits turn them into J fighters:
//
//  - What the screen shows (edits 3-7): the panel shows each pick the way
//    Remix's own select screen shows a J variant picked with its d-pad
//    controls: the J fighter's model and size (mnPlayersVSMakeFighter), its
//    name ("DONKEY KONG", "PURIN") and series logo
//    (mnPlayersVSMakeNameAndEmblem), the announcer's call (J Fox, Purin;
//    mnPlayersVSAnnounceFighter), the pose when picked
//    (mnPlayersVSGetStatusSelected), and Remix's red-circle J flag on the
//    panel once the fighter is picked (CharacterSelect.
//    update_variant_indicators_). Each of those routines takes the fighter id
//    it is given through Remix's per-fighter tables; these edits give them
//    the J id instead of the original one. Nothing is drawn that Remix
//    doesn't draw itself. Remix places the flag for its own panels; on the
//    original panels that spot is where the cursor rests, above the name and
//    on the CPU level's left arrow, so edit 8 moves it, at 3/4 size, to the
//    one corner of the panel nothing else uses on the VS select screen.
//  - What is played (edits 1-2): when the screen hands over to the stage
//    select or the battle, the ids it wrote to the VS battle state are
//    swapped for the J ids (edit 1); everything after that (stage select,
//    battle, sudden death, results, Remix's salty runback) sees the J
//    fighters. When the VS select screen opens again, the J ids are swapped
//    back (edit 2), before the screen restores its panels and before
//    vanilla-css's safety net (its edit 7) checks them, so a rematch shows
//    the same picks.
//
// Remix's Tag Team takes its fighters from its own queues, not from the
// battle state, so the queues are swapped the same way. 12-Character Battle
// is left alone: its grid and its bookkeeping use the original ids (and
// vanilla-css keeps it on the Default set), so every edit here checks
// VsRemixMenu.vs_mode_flag first. Training, 1P and Bonus use their own
// select screens and are not changed.
//
// J variants share costumes, extra costumes, team costumes and shades with
// the original fighter in Remix (Costumes.num_costumes, the costumes/*.asm
// register_extra_costumes_for_char lists, Character default_costume copied
// from the parent, Teams.add_team_costume), so the costume picked on the
// select screen is valid for the J fighter. Remix's select screen loads a
// fighter's files when its model is first made (dynamically_load_character_,
// hooked into ftManagerMakeFighter) and keeps track of which port uses which
// loaded model (dynamic_css.slot_used_by_port, from the fighter it made), so
// J models load and stay loaded the same way they do on Remix's own screen.
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
// Most of it is in the original VS character select overlay (ovl26), which
// the game DMAs uncompressed from ROM 0x12FD80-0x139D00 to RAM 0x80131B00
// whenever that screen opens: rom = ram - 0x80001D80. Every hook that jumps
// there runs while that screen is loaded. Two pieces of that overlay are
// unreachable once vanilla-css is on:
//  - mnPlayersVSMakePortraitShadow (to 0x8013251F) draws a locked fighter's
//    "?" portrait and is only called by mnPlayersVSMakePortrait for a locked
//    fighter. vanilla-css makes mnPlayersVSCheckFighterLocked always answer
//    "unlocked" (its edit 9) and already uses 0x801321B8-0x801323FB. The id
//    table and the battle state code sit at 0x801323FC-0x801324DB, and the
//    regional flag's mapping at 0x801324DC.
//  - mnPlayersVSCheckFighterLocked itself: vanilla-css's edit 9 returns from
//    its first two words, so the rest of it (0x8013204C-0x80132117) never
//    runs. The display hooks' code sits there.
// One hook (edit 7) is in Remix's resident code, which also runs on the
// Training, 1P and Bonus select screens; its helper is resident too (RAM
// 0x80526CD4, Remix's rom = ram - 0x7D800000) and only goes into the overlay
// on the VS select screen.
// The addresses come from the bass symbol log of a v2.0.1 rebuild
// (`bass -sym`) and the ssb-decomp-re decompilation (US).

arch n64.cpu
endian msb

// register numbers (the same as Remix's assembler/N64.inc)
constant r0(0)
constant at(1)
constant v1(3)
constant a0(4)
constant a1(5)
constant a2(6)
constant a3(7)
constant t0(8)
constant t1(9)
constant t2(10)
constant t4(12)
constant t6(14)
constant t8(24)
constant t9(25)
constant t3(11)
constant s1(17)
constant s2(18)
constant s3(19)
constant s4(20)
constant s5(21)
constant f0(0)
constant f2(2)
constant sp(29)
constant ra(31)

// Seeks to a RAM address in the VS character select overlay (ovl26).
macro seek_vs(variable ram) {
    origin ram - 0x80131B00 + 0x0012FD80
    base ram
}

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
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

// Remix routines the display hooks go on to (v2.0.1 symbol log)
constant prevent_series_logo_and_name_display_(0x80525304) // CharacterSelectDebugMenu
constant get_fgm_(0x8051F994)           // CharacterSelect.get_fgm_: announcer call by fighter (t4)
constant get_action_(0x8051F674)        // CharacterSelect.get_action_: pose when picked by fighter (a0)

constant JV_SPACE(0x801323FC)           // first word after vanilla-css's dispatchers
constant JV_SPACE_END(0x80132520)       // mnPlayersVSMakePortrait
constant JV_LOCKED_SPACE(0x8013204C)    // mnPlayersVSCheckFighterLocked after vanilla-css's two words
constant JV_LOCKED_SPACE_END(0x80132118) // mnPlayersVSGetFighterKind
constant JV_RESIDENT(0x80526CD4)        // CharacterSelectDebugMenu.DpadFunctions.clear_settings_for_training_
constant JV_RESIDENT_END(0x80526D0C)

// Maps the fighter id in register {reg} to its J variant when it is one of
// the 12 and the screen isn't 12-Character Battle. Uses at only.
macro jv_css_map(reg, done) {
    lui     at, VS_MODE_FLAG_HI
    lw      at, VS_MODE_FLAG_LO(at)         // at = Remix VS mode
    xori    at, at, MODE_12CB
    beqz    at, {done}                      // 12cb: keep the original id
    sltiu   at, {reg}, 0x000C               // one of the 12?
    beqz    at, {done}
    lui     at, JV_TABLE_HI
    addu    at, at, {reg}
    lbu     {reg}, JV_TABLE_LO(at)          // {reg} = its J variant
}

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
// 3. mnPlayersVSMakeFighter (0x80134A8C): the panel's fighter model
//
// (fighter_gobj, player, fkind = a2, costume). It makes the model with
// ftManagerMakeFighter(desc.fkind = fkind) and scales it with Remix's
// Character.menu_zoom table (Remix's patch at 0x80134BD8), both read from
// the fkind it stored at 0x70(sp). Map a2 before that store. ra is already
// saved at 0x1C(sp) and reloaded at the end, so a jal here is safe.
seek_vs(0x80134AA8)
    jal     jv_css_fighter                  // was: sw a2, 0x70(sp) (0xAFA60070); delay slot (addiu t1, t7, 0x3C) kept

// ---------------------------------------------------------------------------
// 4. mnPlayersVSMakeNameAndEmblem (0x80132A14): the panel's name and logo
//
// (gobj, player, fkind = a2). The fkind is copied to a3 in this call's delay
// slot and only a3 is used after it (Remix's get_name_texture_ and
// get_series_logo_offset_ index their tables with a3 * 4).
seek_vs(0x80132A2C)
    jal     jv_css_name                     // was: jal prevent_series_logo_and_name_display_ (0x0C1494C1); delay slot (move a3, a2) kept

// ---------------------------------------------------------------------------
// 5. mnPlayersVSAnnounceFighter (0x801367F0): the announcer's call
//
// t4 = the panel's fkind (loaded at 0x8013688C), then Remix's get_fgm_ picks
// the call from its fgm_table.
seek_vs(0x80136890)
    jal     jv_css_fgm                      // was: jal CharacterSelect.get_fgm_ (0x0C147E65); delay slot (nop) kept

// ---------------------------------------------------------------------------
// 6. mnPlayersVSGetStatusSelected (0x801348EC): the pose when picked
//
// Remix replaced the routine with a jump to its get_action_ (a0 = fkind,
// selection_action_table). Only the panel's fighter process calls it, for
// the model edit 3 made.
seek_vs(0x801348EC)
    j       jv_css_action                   // was: j CharacterSelect.get_action_ (0x08147D9D); delay slot (nop) kept

// ---------------------------------------------------------------------------
// 7. CharacterSelect.update_variant_indicators_ (Remix, RAM 0x80522180):
//    the regional flag
//
// Every frame, for each panel whose fighter is picked, Remix looks the
// fighter up in Character.variant_type.table and draws the J flag (or the
// E flag) next to the panel for a J (or E) variant, from its own CSS images
// file, at the panel's d-pad icon position. The load of the panel's fighter
// id is replaced by a call that maps it on the VS select screen.
seek(0x80522180)
    jal     jv_flag_id                      // was: lw a0, 0x48(t1) (0x8D240048); delay slot (lui a1, 0x8049) kept

// ---------------------------------------------------------------------------
// 8. CharacterSelect.update_variant_indicators_ (Remix, RAM 0x80522228):
//    where the flag goes on the VS select screen
//
// When the flag is made, Remix takes x from the panel's d-pad image (minus 8
// on the VS screen, for its own panels) and y = 191: a 16x10 sprite at scale
// 1. On the original panels that is x = panel + 26 to 41, y = 191 to 200
// (panel = 69 * port): it covers the cursor where it rests when the screen
// opens (x 37-63), touches the name (y 201) and sits on the CPU level's or
// handicap's left arrow (y 197). Measured on every J fighter's standing pose
// (human and CPU panels, model shown vs hidden), plus the name, the level
// and handicap rows, the team label and buttons and the cursor at rest, the
// only part of the panel nothing covers is its lower-left corner, x = panel
// + 23 to 34, y = 182 to 195 (DK's and Captain Falcon's feet reach x = 77 on
// the right). The flag goes there at 3/4 size: x = panel + 23 to 34, y = 184
// to 191. Fighters still pass over it while they play their picked pose, as
// anywhere on the panel. Only the VS screen is changed (Training, 1P and
// Bonus keep Remix's place and size); with j-variants on, vanilla-css is on,
// so the VS screen always has the original panels.
//
// Remix's block (0x80522220-0x8052225B) loaded the render control object
// with lui/ori/lw, compared the select screen's player struct base with
// CSS_PLAYER_STRUCT (lui/ori, 0x8013BA88), added -8 to x on the VS screen,
// then set y = 191, color 0, palette 0 and scale 1 around the
// Render.draw_texture_ call. Loading with lui/lw and comparing only the low
// half (0xBA88; Training 0x8558, 1P 0x8EC0, Bonus 0x7620) frees the two
// words the VS y and scale need.
constant FLAG_DX(0xC130)                    // -11.0: d-pad image x (panel + 34) -> panel + 23
constant FLAG_Y(0x4338)                     // 184.0
constant FLAG_SCALE(0x3F40)                 // 0.75
seek(0x80522220)
    lw      t3, 0x10D0(t3)                  // was: ori t3, t3, 0x10D0 (0x356B10D0); t3 = render control object (lui t3, 0x8052 before)
    lhu     t6, 0x0042(t3)                  // was: lw t3, 0x0(t3) (0x8D6B0000); low half of the player struct base
    xori    t6, t6, 0xBA88                  // was: lw t6, 0x40(t3) (0x8D6E0040); 0 on the VS screen
    lui     t1, FLAG_DX                     // was: lui t5, 0x8013 (0x3C0D8013)
    lui     s5, 0x3F80                      // was: ori t5, t5, 0xBA88 (0x35ADBA88); scale 1 (Remix's)
    bnez    t6, flag_common                 // was: bne t5, t6, 0x80522244 (0x15AE0003); not VS: Remix's place
    lui     s2, 0x433F                      // was: lui t1, 0xC100 (0x3C09C100); delay slot: uly = 191 (Remix's)
    mtc1    t1, f2                          // unchanged
    add.s   f0, f0, f2                      // unchanged; VS: ulx -= 11
    lui     s2, FLAG_Y                      // was: mfc1 s1, f0 (0x44110000); VS: uly = 184
    lui     s5, FLAG_SCALE                  // was: lui s2, 0x433F (0x3C12433F); VS: scale 0.75
flag_common:
    mfc1    s1, f0                          // was: ori s3, r0, 0 (0x34130000); s1 = ulx
    ori     s3, r0, 0                       // was: ori s4, r0, 0 (0x34140000); color
    jal     0x80400C4C                      // unchanged: Render.draw_texture_
    ori     s4, r0, 0                       // was: lui s5, 0x3F80 (0x3C153F80); delay slot: palette

// ---------------------------------------------------------------------------
// The display hooks' code, in mnPlayersVSCheckFighterLocked's dead body
seek_vs(JV_LOCKED_SPACE)

// Edit 3: a2 = fkind; returns to MakeFighter with a2 and 0x70(sp) mapped.
jv_css_fighter:
    jv_css_map(a2, _fighter_done)
_fighter_done:
    jr      ra
    sw      a2, 0x0070(sp)                  // the store edit 3 replaced

// Edit 4: a3 = fkind (copied from a2 in the delay slot); goes on to Remix's
// routine, which returns to MakeNameAndEmblem.
jv_css_name:
    jv_css_map(a3, _name_done)
_name_done:
    j       prevent_series_logo_and_name_display_
    nop

// Edit 5: t4 = fkind; goes on to get_fgm_, which returns to AnnounceFighter.
jv_css_fgm:
    jv_css_map(t4, _fgm_done)
_fgm_done:
    j       get_fgm_
    nop

// Edit 6: a0 = fkind; goes on to get_action_, which returns to the caller.
jv_css_action:
    jv_css_map(a0, _action_done)
_action_done:
    j       get_action_
    nop

constant jv_locked_end(pc())
if jv_locked_end > JV_LOCKED_SPACE_END {
    error "j-variants display code overflows mnPlayersVSCheckFighterLocked"
}

// ---------------------------------------------------------------------------
// The table and the battle state routines (see "Where the code lives")
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

// Edit 7, the overlay part (VS select screen only): a0 = the panel's fighter;
// returns to update_variant_indicators_ with it mapped.
jv_flag_vs:
    jv_css_map(a0, _flag_done)
_flag_done:
    jr      ra
    nop

constant jv_end(pc())
if jv_end > JV_SPACE_END {
    error "j-variants code overflows mnPlayersVSMakePortraitShadow"
}

// ---------------------------------------------------------------------------
// Edit 7, the resident part, in CharacterSelectDebugMenu.DpadFunctions.
// clear_settings_for_training_ (RAM 0x80526CD4-0x80526D0B): its only call is
// commented out in src/Render.asm and a ROM-wide scan finds no jump, branch,
// pointer or lui pair into it. update_variant_indicators_ also runs on the
// Training, 1P and Bonus select screens, where another overlay sits at
// 0x80131B00, so the overlay part is only entered on the VS select screen.
// at is free at the hook (the routine doesn't use it there).
seek(JV_RESIDENT)
jv_flag_id:
    lw      a0, 0x0048(t1)                  // the load edit 7 replaced: a0 = the panel's fighter
    lui     at, 0x800A
    lbu     at, 0x4AD0(at)                  // Global.current_screen
    xori    at, at, 0x0010                  // the VS select screen?
    bnez    at, _flag_return
    nop
    j       jv_flag_vs                      // yes: map it there (the overlay is loaded)
    nop
_flag_return:
    jr      ra
    nop

if pc() > JV_RESIDENT_END {
    error "j-variants resident code overflows clear_settings_for_training_"
}
