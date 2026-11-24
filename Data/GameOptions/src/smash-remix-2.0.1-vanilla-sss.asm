// Smash Remix 2.0.1: "Vanilla SSS" game option
//
// The VS stage select screen is the original game's again: the original 10
// big icons in two rows (Peach's Castle, Congo Jungle, Hyrule Castle, Planet
// Zebes, Mushroom Kingdom / Yoshi's Island, Dream Land, Sector Z, Saffron
// City, Random), the original cursor, the yellow name plate with the
// original stage names, and the series emblem on the wooden plaque. Only
// those 9 stages can be picked, and Random picks among them. Hazards and
// stage movement are on, as in the original game.
//
// Mushroom Kingdom is in: it is one of the original game's 9 VS stages
// (an unlockable one), and Remix's default save data, which netplay always
// boots with, has everything unlocked (unlock mask 0x7F). That is also why
// vanilla-css shows Luigi, Ness, Captain Falcon and Jigglypuff.
//
// Part 1 is Random's stage list (generated, smash-remix-2.0.1-vanilla-sss-
// random.asm). Part 2 (edits 1-20) brings back the original screen.
//
// This file is the source of the "vanilla-sss" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create, from this folder
// (it includes smash-remix-2.0.1-vanilla-sss-random.asm):
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-vanilla-sss.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the code lives
// --------------------
// Remix code and data: assembled at RAM 0x80400000 and copied there unchanged
// from ROM 0x02C00000 by one DMA at boot (Boot.load_), so
// rom = ram - 0x7D800000.
// The stage select screen's code (mnMaps, scene 0x15) is an overlay the game
// DMAs uncompressed from ROM 0x14D670 to RAM 0x80131B00 whenever the screen
// opens, so rom = ram - 0x7FFE4490 there (Remix's own OS.patch_start pairs
// for this overlay, Stages.asm, use the same offset). Every edit is outside
// the CIC boot checksum range (0x1000-0x101000).
//
// Function names are from the ssb-decomp-re decompilation (US); Remix names
// and addresses from the bass symbol log of a v2.0.1 rebuild (bass -sym; the
// rebuild matched Smash_Remix_2.0.1.z64 byte for byte apart from the header
// CRCs). Every edited word's original value is in the JSON "expect" field.
//
// The three stage options (vanilla-sss, stages-dream-land,
// stages-competitive) share Random's toggles and can't be on together.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant v0(2)
constant t0(8)
constant sp(29)
constant ra(31)

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}
// Seeks to a RAM address in the stage select overlay.
macro seek_sss(variable ram) {
    origin ram - 0x7FFE4490
    base ram
}
macro toggle(variable entry, variable value) {
    seek(entry + 4)
    dw value
}

// ===========================================================================
// Part 1: Random's stage list
// ===========================================================================
include "smash-remix-2.0.1-vanilla-sss-random.asm"

// ===========================================================================
// Part 2: the original stage select screen
// ===========================================================================
//
// How Remix changed this screen (found by diffing the overlay against the
// ROM Remix is built on, roms/original.z64, and naming every jump target
// with the symbol log; Remix's Stages.asm describes each patch):
//  - it stopped drawing the original icons (mnMapsMakeIcons -> jr ra), the
//    stage name (mnMapsMakeName -> jr ra) and its yellow name plate, moved
//    the "Stage Select" label and its blue bar down and removed the shadow
//    under the name plate;
//  - Stages.setup_ (called by Render when the screen opens) draws Remix's
//    own 6x3 icon grid with pages, the stage name as text, the hazard and
//    layout lines and the R/Z page buttons;
//  - the cursor is a row/column/page on that grid (Stages.row, .column,
//    .page_number): Remix's up_/down_/left_/right_ hooks move it, its
//    cursor routine places it (0x80132A58), and Stages.get_stage_id_ turns
//    it into a stage id. Every hook that needs "the stage under the cursor"
//    calls get_stage_id_: the series emblem, the preview's file and type,
//    Random's preview, and the pick itself (Stages.swap_stage_, which also
//    resolves Random from Random's stage list);
//  - Stages.right_check_ runs Remix's extras each frame: the hold-L settings
//    shortcut, R/Z pages, the hazard toggle (L), layout variants (C-down)
//    and the Random roulette (C-down on Random);
//  - the preview camera looks at the same point for every stage.
// The original code behind those patches is still in the overlay, and so are
// the original icons, names and plate in the screen's file (MNMaps, which
// the screen still loads for the wooden plaque).
//
// So this part:
//  - puts back the original drawing (edits 4-12) and the original cursor
//    and its movement over the 10 original slots (edits 13-18), and turns
//    off Remix's per-frame extras by putting back the original "right not
//    pressed" branch (edit 18);
//  - makes Stages.get_stage_id_ answer with the stage of the original slot
//    under the cursor (edit 1), so every Remix hook that stays (preview
//    file, type and position, Random's preview, the pick) works on the
//    original slots, and sends the Random slot through Remix's Random too
//    (edit 19);
//  - keeps Stages.setup_ from drawing anything (edit 2);
//  - never treats Mushroom Kingdom as locked (edit 3), and puts the cursor
//    on Random if the saved slot isn't one of the 10 (edit 20).
// Remix's d-pad-only direction checks stay (the original also moved the
// cursor with the C buttons and L/R; Remix keeps those free for its
// alternate music, held while a stage is picked). The Random roulette, the
// hazard and layout toggles and the R/Z pages go with Remix's grid, and so
// does the hold-L settings shortcut on this screen (it still works on the
// character select screen). Training's stage select is the same screen and
// looks the same.

constant SLOT_RANDOM(9)
constant STAGE_RANDOM(0xDE)

// ---------------------------------------------------------------------------
// 1. Stages.get_stage_id_ (RAM 0x80454930-0x80454A8B, Stages.asm ~1448):
// the stage under the cursor, from the original slot (sMNMapsCursorSlot,
// 0x80134BD8) and the original slot order (mnMapsGetGroundKind). It still
// keeps the stage in Stages.original_stage_id, and still changes no
// register but v0, as its callers expect. This replaces the first 20 words
// of the old body (page/row/column lookup, tournament hazard exceptions,
// layout variants); the rest is unreachable (its only entry is the top).
constant sMNMapsCursorSlot_hi(0x8013)
constant sMNMapsCursorSlot_lo(0x4BD8)
seek(0x80454930)
get_stage_id_:
    addiu   sp, sp, -0x0008                 // was: addiu sp, sp, -0x18
    sw      t0, 0x0004(sp)
    lui     t0, sMNMapsCursorSlot_hi
    lw      t0, sMNMapsCursorSlot_lo(t0)    // t0 = slot (0-9)
    sltiu   v0, t0, SLOT_RANDOM + 1
    bnez    v0, _slot_ok
    nop
    ori     t0, r0, SLOT_RANDOM             // anything else: Random
_slot_ok:
    lui     v0, slot_stage >> 16
    ori     v0, v0, slot_stage & 0xFFFF
    addu    v0, v0, t0
    lbu     v0, 0x0000(v0)                  // v0 = stage id
    lui     t0, 0x8045
    sb      v0, 0x7002(t0)                  // Stages.original_stage_id (0x80457002)
    lw      t0, 0x0004(sp)
    jr      ra
    addiu   sp, sp, 0x0008
slot_stage:                                 // mnMapsGetGroundKind's order
    db 0x00, 0x02, 0x04, 0x03, 0x08         // Peach's Castle, Congo Jungle, Hyrule Castle, Planet Zebes, Mushroom Kingdom
    db 0x05, 0x06, 0x01, 0x07, STAGE_RANDOM // Yoshi's Island, Dream Land, Sector Z, Saffron City, Random
if pc() > 0x80454A8C {
    error "get_stage_id_ overflows its old body"
}

// ---------------------------------------------------------------------------
// 2. Stages.setup_ (RAM 0x80455F6C, Stages.asm ~2825): Render calls it when
// the stage select screen opens; it draws Remix's grid, page number and
// buttons, the stage name and the hazard/layout lines, and registers the
// routine that updates them (update_text_). Draw nothing.
seek(0x80455F6C)
    jr      ra                              // was: addiu sp, sp, -0x30
    nop                                     // was: sw ra, 0x0004(sp)

// ---------------------------------------------------------------------------
// 3. mnMapsCheckLocked (0x80131BAC): Mushroom Kingdom is locked unless the
// save data's unlock mask has it. Never locked, whatever the save data says
// (the original slot movement and icons ask this).
seek_sss(0x80131BAC)
    jr      ra                              // was: addiu at, r0, 8
    or      v0, r0, r0                      // was: bne a0, at, 0x80131BDC; return FALSE

// ---------------------------------------------------------------------------
// 4-12. The original drawing (the original instructions put back)

// 4. mnMapsLabelsProcDisplay: the blue bar behind "Stage Select" at its
// original place (Remix moved it down)
seek_sss(0x801321C4)
    dw 0x35EF0218    // ori t7, t7, 0x218            was: ori t7, t7, 0x232
    dw 0x37180200    // ori t8, t8, 0x200            was: ori t8, t8, 0x21a

// 5. mnMapsLabelsProcDisplay: the shadow under the name plate (Remix: 16 nops)
seek_sss(0x801321D4)
    dw 0x8C620000    // lw v0, (v1)                  was: nop
    dw 0x240F0033    // addiu t7, zero, 0x33         was: nop
    dw 0x3C0EFA00    // lui t6, 0xfa00               was: nop
    dw 0x24590008    // addiu t9, v0, 8              was: nop
    dw 0xAC790000    // sw t9, (v1)                  was: nop
    dw 0xAC4F0004    // sw t7, 4(v0)                 was: nop
    dw 0xAC4E0000    // sw t6, (v0)                  was: nop
    dw 0x8C620000    // lw v0, (v1)                  was: nop
    dw 0x3C0E0030    // lui t6, 0x30                 was: nop
    dw 0x3C19F643    // lui t9, 0xf643               was: nop
    dw 0x24580008    // addiu t8, v0, 8              was: nop
    dw 0xAC780000    // sw t8, (v1)                  was: nop
    dw 0x37390304    // ori t9, t9, 0x304            was: nop
    dw 0x35CE82F4    // ori t6, t6, 0x82f4           was: nop
    dw 0xAC4E0004    // sw t6, 4(v0)                 was: nop
    dw 0xAC590000    // sw t9, (v0)                  was: nop

// 6. mnMapsMakeLabels: "Stage Select" at y = 122 (Remix: 128)
seek_sss(0x80132310)
    dw 0x3C0142F4    // lui at, 0x42f4               was: lui at, 0x4300

// 7. mnMapsMakeLabels: draw the yellow name plate (Remix branched over it)
seek_sss(0x80132354)
    dw 0x8E8E0008    // lw t6, 8(s4)                 was: b 0x8013240c

// 8. mnMapsMakeIcons: draw the original icons (Remix: jr ra)
seek_sss(0x80132528)
    dw 0x27BDFF70    // addiu sp, sp, -0x90          was: jr ra
    dw 0xAFB70048    // sw s7, 0x48(sp)              was: nop

// 9. mnMapsMakeName: draw the original stage name (Remix: jr ra)
seek_sss(0x80132738)
    dw 0x27BDFFC0    // addiu sp, sp, -0x40          was: jr ra
    dw 0x3C0E8013    // lui t6, 0x8013               was: nop

// 10. mnMapsSetLogoPosition: the original emblem positions (Remix:
// Stages.extend_series_logos_, its own table)
seek_sss(0x801327F4)
    dw 0x3C0E8013    // lui t6, 0x8013               was: ori at, zero, 0xde
    dw 0x27A30000    // addiu v1, sp, 0              was: beq a1, at, 0x8013284c
    dw 0x25CE47E4    // addiu t6, t6, 0x47e4         was: nop
    dw 0x25C80048    // addiu t0, t6, 0x48           was: lui v1, 0x8052
    dw 0x0060C825    // move t9, v1                  was: ori v1, v1, 0x5e4
    dw 0x8DD80000    // lw t8, (t6)                  was: lui t8, 0x8045
    dw 0x25CE000C    // addiu t6, t6, 0xc            was: ori t8, t8, 0xaed8
    dw 0x2739000C    // addiu t9, t9, 0xc            was: addu t8, t8, a1
    dw 0xAF38FFF4    // sw t8, -0xc(t9)              was: lbu t8, (t8)
    dw 0x8DCFFFF8    // lw t7, -8(t6)                was: ori t2, zero, 0xc
    dw 0xAF2FFFF8    // sw t7, -8(t9)                was: multu t8, t2
    dw 0x8DD8FFFC    // lw t8, -4(t6)                was: mflo t3
    dw 0x15C8FFF8    // bne t6, t0, 0x80132808       was: b 0x80132870
    dw 0xAF38FFFC    // sw t8, -4(t9)                was: addu v0, v1, t3

// 11. mnMapsMakeEmblem: the original emblem of the slot's stage (Remix:
// Stages.extend_series_logos_, the emblem from get_stage_id_ and its table)
seek_sss(0x801328AC)
    dw 0x3C0E8013    // lui t6, 0x8013               was: sw ra, 0x14(sp)
    dw 0x27A20018    // addiu v0, sp, 0x18           was: sw a0, 0x40(sp)
    dw 0x25CE4834    // addiu t6, t6, 0x4834         was: jal Stages.get_stage_id_
    dw 0xAFBF0014    // sw ra, 0x14(sp)              was: nop
    dw 0xAFA40040    // sw a0, 0x40(sp)              was: sw v0, 0x44(sp)
    dw 0xAFA50044    // sw a1, 0x44(sp)              was: move t1, v0
    dw 0x25C80024    // addiu t0, t6, 0x24           was: lui t8, 0x8045
    dw 0x0040C825    // move t9, v0                  was: ori t8, t8, 0xaed8
    dw 0x8DD80000    // lw t8, (t6)                  was: addu t8, t8, v0
    dw 0x25CE000C    // addiu t6, t6, 0xc            was: lbu t8, (t8)
    dw 0x2739000C    // addiu t9, t9, 0xc            was: sw t8, 0x18(sp)
    dw 0xAF38FFF4    // sw t8, -0xc(t9)              was: ori t2, zero, 0xc
    dw 0x8DCFFFF8    // lw t7, -8(t6)                was: multu t8, t2
    dw 0xAF2FFFF8    // sw t7, -8(t9)                was: lui v0, 0x8052
    dw 0x8DD8FFFC    // lw t8, -4(t6)                was: ori v0, v0, 0x5e0
    dw 0x15C8FFF8    // bne t6, t0, 0x801328cc       was: b 0x801328fc
    dw 0xAF38FFFC    // sw t8, -4(t9)                was: ori at, zero, 0xde
seek_sss(0x8013294C)
    dw 0x00184880    // sll t1, t8, 2                was: mflo t1

// 12. mnMapsSetPreviewCameraPosition: the camera looks at each stage's
// original point (Remix: 1600, 1600 for every stage)
seek_sss(0x801339A4)
    dw 0xC4500000    // lwc1 f16, (v0)               was: lui t8, 0x44c8
    dw 0xE4900048    // swc1 f16, 0x48(a0)           was: sw t8, 0x48(a0)
    dw 0xC4520004    // lwc1 f18, 4(v0)              was: sw t8, 0x4c(a0)
    dw 0xE492004C    // swc1 f18, 0x4c(a0)           was: nop

// ---------------------------------------------------------------------------
// 13-18. The original cursor and its movement over the 10 slots

// 13. mnMapsSetCursorPosition: place the cursor on the slot (Remix: from
// Stages.row/column)
seek_sss(0x80132A58)
    dw 0x28A10005    // slti at, a1, 5               was: lw t8, 0x74(a0)
    dw 0x10200010    // beqz at, 0x80132aa0          was: lui a1, 0x8045
    dw 0x00054080    // sll t0, a1, 2                was: ori a1, a1, 0x7001
    dw 0x00057080    // sll t6, a1, 2                was: lbu t9, (a1)
    dw 0x01C57023    // subu t6, t6, a1              was: ori t6, zero, 0x2a
    dw 0x000E70C0    // sll t6, t6, 3                was: multu t6, t9
    dw 0x01C57021    // addu t6, t6, a1              was: mflo t6
    dw 0x000E7040    // sll t6, t6, 1                was: addiu t7, t6, 0x18
    dw 0x25CF0017    // addiu t7, t6, 0x17           was: mtc1 t7, f4
    dw 0x448F2000    // mtc1 t7, f4                  was: cvt.s.w f6, f4
    dw 0x8C980074    // lw t8, 0x74(a0)              was: swc1 f6, 0x58(t8)
    dw 0x3C0141B8    // lui at, 0x41b8               was: lui a1, 0x8045
    dw 0x468021A0    // cvt.s.w f6, f4               was: ori a1, a1, 0x7000
    dw 0x44814000    // mtc1 at, f8                  was: lbu t9, (a1)
    dw 0xE7060058    // swc1 f6, 0x58(t8)            was: ori t6, zero, 0x20
    dw 0x8C990074    // lw t9, 0x74(a0)              was: multu t6, t9
    dw 0x03E00008    // jr ra                        was: mflo t6
    dw 0xE728005C    // swc1 f8, 0x5c(t9)            was: addiu t7, t6, 0xe
    dw 0x01054023    // subu t0, t0, a1              was: mtc1 t7, f4
    dw 0x000840C0    // sll t0, t0, 3                was: cvt.s.w f6, f4
    dw 0x01054021    // addu t0, t0, a1              was: swc1 f6, 0x5c(t8)
    dw 0x00084040    // sll t0, t0, 1                was: jr ra
    dw 0x2509FF1D    // addiu t1, t0, -0xe3          was: nop

// 14. mnMapsMakeCursor: the original full-size red cursor (Remix: scaled to
// 0.84375, colored by the hazard mode)
seek_sss(0x80132B40)
    dw 0x94590024    // lhu t9, 0x24(v0)             was: ori t1, zero, 0x201
    dw 0x240B00FF    // addiu t3, zero, 0xff         was: sh t1, 0x24(v0)
    dw 0xA04B0028    // sb t3, 0x28(v0)              was: lui t2, 0x3f58
    dw 0x3329FFDF    // andi t1, t9, 0xffdf          was: sw t2, 0x18(v0)
    dw 0xA4490024    // sh t1, 0x24(v0)              was: sw t2, 0x1c(v0)
    dw 0x352A0001    // ori t2, t1, 1                was: lui t0, 0x8046
    dw 0xA44A0024    // sh t2, 0x24(v0)              was: ori t0, t0, 0x6f0c
    dw 0xA0400029    // sb zero, 0x29(v0)            was: jal Stages.update_cursor_color_
    dw 0xA040002A    // sb zero, 0x2a(v0)            was: lw a0, 4(t0)

// 15-18. mnMapsFuncRun: up, down, left and right move between the original
// slots again (Remix jumped to Stages.up_/down_/left_/right_, which move
// its row/column and fake the slot). Edit 18's first word is also where
// Remix ran Stages.right_check_ (settings shortcut, pages, hazard toggle,
// layout variants, roulette) every frame the cursor doesn't move right.
seek_sss(0x80133F60)                        // 15. up
    dw 0x3C038013    // lui v1, 0x8013               was: j Stages.up_
    dw 0x8C634BD8    // lw v1, 0x4bd8(v1)            was: nop
seek_sss(0x80134034)                        // 16. down
    dw 0x3C038013    // lui v1, 0x8013               was: j Stages.down_
    dw 0x8C634BD8    // lw v1, 0x4bd8(v1)            was: nop
seek_sss(0x80134104)                        // 17. left
    dw 0x3C058013    // lui a1, 0x8013               was: j Stages.left_
    dw 0x24A54BD8    // addiu a1, a1, 0x4bd8         was: nop
seek_sss(0x80134200)                        // 18. right
    dw 0x1040003C    // beqz v0, 0x801342f4          was: j Stages.right_check_
    dw 0xAFA20020    // sw v0, 0x20(sp)              was: nop
    dw 0x3C058013    // lui a1, 0x8013               was: j Stages.right_
    dw 0x24A54BD8    // addiu a1, a1, 0x4bd8         was: nop

// ---------------------------------------------------------------------------
// 19. mnMapsSaveSceneData (0x80133B78): when a stage is picked, the original
// resolves the Random slot itself (syUtilsRandTimeUCharRange over the 9
// stages), and every other slot through mnMapsGetGroundKind, which Remix
// replaced with Stages.swap_stage_ (0x80133C04). Remix never had slot 9, so
// its Random went through swap_stage_ too. Send slot 9 there as well:
// get_stage_id_ answers Random and swap_stage_ picks from Random's stage
// list (Part 1) with Remix's seeded random number, as in Remix.
seek_sss(0x80133BCC)
    beq     r0, r0, 0x80133C04              // was: bne a0, at, 0x80133C04 (delay slot addiu s1, s1, 0x4AD0 kept)

// ---------------------------------------------------------------------------
// 20. mnMapsGetSlot (0x80132498): mnMapsInitVars starts the cursor on the
// slot last picked (gSCManagerSceneData.maps_vsmode_gkind, or
// maps_training_gkind in Training), saved as that slot's stage. For any
// other stage id the original returns whatever v0 holds. Return the Random
// slot instead (a safety net: with this option on, only the 10 slots' ids
// are saved there).
seek_sss(0x801324B0)
    ori     v0, r0, SLOT_RANDOM             // was: nop (delay slot of jr ra)
