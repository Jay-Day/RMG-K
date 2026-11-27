// Smash Remix 2.0.1: Stage Settings guard for vanilla-sss
//
// Part of vanilla-sss (it was in stages-dream-land and stages-competitive too,
// before those became stage-list's preset lists). Its stage list is Remix
// toggles (the random stage toggles), patched as assembled values. Netplay
// usually boots with empty save data and keeps those values, but a
// host-picked starting save that holds Remix settings is loaded over them at
// boot, so the match would take its stages from the save's random stage list.
//
// This guard makes Toggles.load_ skip the Stage Settings block of the save:
// every Stage Settings toggle (Stage Select Layout, Hazard Mode, Whispy Mode,
// Saffron Pokemon Rate, Pokemon Announcer, Dragon King HUD, Camera Mode,
// Yoshi's Island clouds, the stage Load Profile selector and the random stage
// toggles) keeps its ROM value. The other blocks of the save (Remix,
// Gameplay, Music, Pokemon settings, player tags) still load. Saving is not
// changed: Toggles.save_ still writes the current Stage Settings to SRAM, so
// a kept netplay save records what the match played with.
//
// Build: assemble vanilla-sss's .asm and then this file over the same copy
// of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-vanilla-sss.asm
//   bass.exe -o patched.z64 smash-remix-2.0.1-stage-save-guard.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
// Remix's code and data are copied from ROM 0x02C00000 to RAM 0x80400000 at
// boot, so rom = ram - 0x7D800000. Addresses are from the bass symbol log of
// a v2.0.1 rebuild.

arch n64.cpu
endian msb

constant at(1)
constant a0(4)
constant ra(31)

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

constant Menu_import_(0x80444824)
constant head_stage_settings(0x80466ED0)   // Toggles.head_stage_settings

// ---------------------------------------------------------------------------
// Toggles.load_ (RAM 0x80460458, src/Toggles.asm ~890), called only by
// Boot.load_ when the save has Remix settings. For each SRAM block it reads
// the block (SRAM.load_) and then unpacks it into the block's toggles with
// Menu.import_(a0 = first entry, a1 = block). Call the guard instead.
seek(0x804604BC)
    jal     stage_import_guard              // was: jal Menu.import_ (0x0C111209); delay slot (lw a1, 0x0(a1)) kept

// The guard sits in Hazards.rttf_bomb_2_ (RAM 0x8048D1A4-0x8048D213), which
// nothing references (a ROM-wide scan finds no jump, branch, pointer or lui
// pair into it). The rest of that function stays free for the stage options.
seek(0x8048D1A4)
stage_import_guard:
    lui     at, (head_stage_settings >> 16)
    ori     at, at, (head_stage_settings & 0xFFFF)
    beq     a0, at, _skip                   // the Stage Settings block: keep the ROM values
    nop
    j       Menu_import_                    // any other block: import as usual (returns to Toggles.load_)
    nop
_skip:
    jr      ra
    nop

if pc() > 0x8048D214 {
    error "stage guard overflows Hazards.rttf_bomb_2_"
}
