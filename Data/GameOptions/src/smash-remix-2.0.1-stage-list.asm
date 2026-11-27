// Smash Remix 2.0.1: "stage-list" game option (Stage list), the part that
// is the same for every list
//
// The host picks a list of up to 85 stages (one of the pack's preset lists,
// a saved one or their own), each with its own hazard mode.
// The stage select screen then shows only those stages (Remix's tournament
// layout: 17 stages and Random per page, up to 5 pages, R/Z switch pages),
// Random and a match with stage select off pick only among them, and every
// stage is played with the hazard mode the host chose for it. This file holds
// the fixed edits, the option's `patches` in the pack. The client makes the
// list part (L1-L3) from the pack's `stageList` block and the host's list
// (Source/RMG-Core/StageList.cpp; the README's "Stage lists" says what goes
// where). To check a list by hand, write stage-list-entries.asm with its three
// tables as `origin`/`db` lines and assemble over a copy of the ROM:
//   bass.exe -o patched.z64 smash-remix-2.0.1-stage-list.asm
// then compare patched.z64 with the ROM the match's patches make.
//
// Why the tournament layout: it is Remix's own stage table that holds any
// stage ids page by page, has no C-down layout variants (a variant is a
// stage id of its own, so it can be a list entry) and no L hazard toggle.
// Hovering a stage, or Random's pick, sets the hazard mode from
// Stages.tournament_hazard_mode_table, one byte per stage id; the list part
// writes the chosen modes there. So a stage can be in the list once.
//
// Nothing here depends on save data: a Remix save restores the Stage Select
// Layout, Hazard Mode and random stage toggles, but the Stages code below no
// longer reads the layout toggle, Random's list no longer comes from the
// toggles, and the hazard mode is overwritten on every hover and Random pick.
//
// Remix code and data are at RAM 0x80400000, copied unchanged from ROM
// 0x02C00000 at boot (rom = ram - 0x7D800000).
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.

arch n64.cpu
endian msb

constant r0(0)
constant v0(2)
constant v1(3)
constant a1(5)
constant a2(6)
constant t0(8)
constant t1(9)
constant t2(10)
constant t3(11)
constant t4(12)
constant t5(13)
constant sp(29)
constant ra(31)
constant at(1)

macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

// ---------------------------------------------------------------------------
// F1. Toggles.entry_sss_layout value word: TOURNAMENT at boot, so Remix's
// settings menu shows the layout in use when no save is loaded.
seek(0x80466ED4)
    dw 1                                    // was: 0 (NORMAL)

// ---------------------------------------------------------------------------
// F2. Every read of the Stage Select Layout toggle in Stages.asm reads
// TOURNAMENT (each follows `li rX, Toggles.entry_sss_layout`). A loaded save
// or the settings menu can still change the toggle; the stage select ignores it.
seek(0x804547C4); ori t1, r0, 1             // update_stage_icons_   was: lw t1, 0x0004(t1)
seek(0x804548E4); ori t2, r0, 1             // check_valid_page_     was: lw t2, 0x0004(t2)
seek(0x80454984); ori t1, r0, 1             // get_stage_id_         was: lw t1, 0x0004(t1)
seek(0x80454BCC); ori t3, r0, 1             // update_text_ hazard   was: lw t3, 0x0004(t3)
seek(0x80454CF0); ori t0, r0, 1             // update_text_ layout   was: lw t0, 0x0004(t0)
seek(0x80454D50); ori t3, r0, 1             // update_text_ cursor   was: lw t3, 0x0004(t3)
seek(0x80454EC0); ori t1, r0, 1             // right_check_ pages    was: lw t1, 0x0004(t1)
seek(0x80454FD8); ori t0, r0, 1             // right_check_ L toggle was: lw t0, 0x0004(t0)
seek(0x804550DC); ori t1, r0, 1             // right_check_ C-down   was: lw t1, 0x0004(t1)
seek(0x804562D4); ori t1, r0, 1             // setup_ hazard labels  was: lw t1, 0x0004(t1)
seek(0x804565D0); ori t1, r0, 1             // setup_ L button       was: lw t1, 0x0004(t1)
seek(0x8045EADC); ori at, r0, 1             // swap_stage_ Random    was: lw at, 0x0004(at)

// ---------------------------------------------------------------------------
// F3. Stages.get_stage_id_ (Stages.asm ~1484): on the tournament layout Remix
// forces a hazard mode by page for Dream Land (page 3), Yoshi's Island DL
// (pages 1-2) and Saffron City DL (page 3), because its own tournament table
// lists them twice. Skip those checks; the table decides for every stage.
seek(0x804549BC)
    b       0x80454A10                      // was: lui t3, 0x8045 (li t3, page_number)

// ---------------------------------------------------------------------------
// F4. Stages.add_stage_to_random_list_: Stages.swap_stage_ calls it once per
// stage that has a random toggle (a0 = toggle entry, a1 = stage id, a2 =
// TRUE to add regardless). Remix adds the stage when its toggle is on and,
// on the tournament layout, its Tournament profile default is on. Add it
// when it is on the stage select's table instead. Returns v0 = added and
// v1 = random_count like Remix's routine.
seek(0x804556B8)
    addiu   sp, sp, -0x0010
    sw      t0, 0x0004(sp)
    sw      t1, 0x0008(sp)
    sw      t2, 0x000C(sp)
    lui     t1, 0x8045
    lw      v1, 0x5818(t1)                  // v1 = Stages.random_count (0x80455818)
    bnez    a2, _add                        // Remix's fallback: add every stage
    ori     v0, r0, 0                       // v0 = FALSE
    lui     t0, 0x8045
    ori     t0, t0, 0x68E0                  // t0 = Stages.stage_table_tournament
    addiu   t2, t0, 90                      // t2 = end of its 5 pages
_loop:
    lbu     v0, 0x0000(t0)                  // v0 = stage id of this icon
    beq     v0, a1, _add                    // the stage is on the stage select
    addiu   t0, t0, 0x0001
    bne     t0, t2, _loop
    nop
    b       _end
    ori     v0, r0, 0                       // v0 = FALSE
_add:
    addiu   v1, v1, 0x0001                  // v1 = random_count + 1
    sw      v1, 0x5818(t1)                  // update random_count
    addu    t0, t1, v1
    sb      a1, 0x5757(t0)                  // Stages.random_table[random_count - 1] = stage id
    ori     v0, r0, 1                       // v0 = TRUE
_end:
    lw      t0, 0x0004(sp)
    lw      t1, 0x0008(sp)
    lw      t2, 0x000C(sp)
    jr      ra
    addiu   sp, sp, 0x0010
    while pc() < 0x80455758 {               // rest of the old body (up to Stages.random_table)
        nop
    }

// ---------------------------------------------------------------------------
// F5. Stages.swap_stage_ (Stages.asm ~5416): after Random picks a stage on the
// tournament layout and sets its hazard mode, Remix branches to the end and
// skips setting Stages.dont_freeze_stage for the picked stage, so "movement
// off" followed whatever stage the cursor was on before Random. Fall through
// to that code instead.
seek(0x8045EB04)
    nop                                     // was: b _end (0x1000000C)

// ---------------------------------------------------------------------------
// L1-L3, written by the generator for the list: Stages.stage_table_tournament
// (90 bytes), Stages.stage_table_pages[TOURNAMENT] and
// Stages.tournament_hazard_mode_table (stage ids 0x00-0xD7).
include "stage-list-entries.asm"
