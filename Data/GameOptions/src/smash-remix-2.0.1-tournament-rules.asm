// Smash Remix 2.0.1: "Tournament rules" game option
//
// VS matches are played with 4 stocks, 8 minutes (Remix's timed stock
// rule), no items, no handicap and 100% damage, and players can't change
// that on the character select screen (CSS): the stock/time arrows do
// nothing. The stock/time picker still flips the display, so players can
// check the time limit.
//
// These are Remix's own boot defaults (Settings.set_vs_settings_), so the
// option doesn't change what a match starts with; it keeps the rules from
// drifting. Every time the VS CSS opens they are written back, so anything
// changed in the VS mode menu or VS options menu (or left behind by one of
// Remix's modes) is undone before the next match, and the match itself
// always starts in timed stock mode.
//
// Only normal VS is affected. Remix's own modes (12-Character Battle, Tag
// Team, King of the Hill, Smashketball, Tug of War) set their own rules and
// keep their arrows. Remix's per-player CSS Panel Menu (stock mode, size,
// knockback...) isn't touched; the tournament-profile option turns that
// menu off by default.
//
// Works with or without the other options: it touches no byte that
// vanilla-css, team-battle, skip-to-css, tournament-profile,
// random-vanilla or the stage options write. With team-battle, both hooks
// run in mnPlayersVSInitVars, at different instructions.
//
// This file is the source of the "tournament-rules" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-tournament-rules.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Where the code lives
// --------------------
// Remix code and data: assembled at RAM 0x80400000 and copied there
// unchanged from ROM 0x02C00000 by one boot DMA, so rom = ram - 0x7D800000.
// VS CSS overlay (ovl26, scene 0x10): loaded uncompressed from ROM 0x12FD80
// to RAM 0x80131B00 whenever the screen opens, so rom = ram - 0x80001D80.
// Addresses are from the bass symbol log of a v2.0.1 rebuild (byte-identical
// to the release apart from the header CRCs) and the ssb-decomp-re names.
// All edits are outside the CIC checksum range (0x1000-0x101000).
//
// The new code sits in Item.ScuttleMinion.stage_setting_ (RAM
// 0x8044F0C0-0x8044F1B7), which nothing calls: the Scuttle Minion item is
// spawned by Hazards.minion_stage_setting_ (items/ScuttleMinion.asm,
// SPAWN_ITEM), and a scan of every aligned ROM word finds no j/jal, branch,
// pointer or lui+addiu/ori/load/store pair that reaches into it.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant v0(2)
constant a0(4)
constant t0(8)
constant t1(9)
constant t2(10)
constant s2(18)
constant ra(31)

// Seeks to a RAM address in Remix's code/data.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}
// Seeks to a RAM address in the VS CSS overlay (ovl26).
macro seek_css(variable ram) {
    origin ram - 0x80001D80
    base ram
}

constant VS_MODE_FLAG_HI(0x804F)        // VsRemixMenu.vs_mode_flag = 0x804F7568 (0 = normal VS)
constant VS_MODE_FLAG_LO(0x7568)

constant RULE_TIMED_STOCK(3)            // SCBATTLE_GAMERULE_TIME | SCBATTLE_GAMERULE_STOCK
constant STOCKS(3)                      // 4 stocks (0-based)
constant TIME(8)                        // minutes
constant DAMAGE(100)                    // percent

constant code_start(0x8044F0C0)         // Item.ScuttleMinion.stage_setting_
constant code_end(0x8044F1B8)

// ---------------------------------------------------------------------------
// 1. mnPlayersVSInitVars (0x8013AEC8): the rules when the CSS opens
//
// InitVars copies time, stocks, team battle and the game rule from the VS
// battle state (gSCManagerTransferBattleState, 0x800A4D08, held in s2) into
// the CSS's own variables, and the CSS writes them back when a match starts
// (mnPlayersVSSetSceneData). Hook the is_reset_players load that follows
// (the CSS variables are all set by then, and Remix's mode setup and the
// panel setup come after) and overwrite both copies with the rules. The
// battle state copy is what the VS menus and the picker read, the CSS copy
// is what the screen shows and the match uses. t0 and t1 are free here
// (Remix's mode setup, called next, clobbers them anyway).
seek_css(0x8013AF38)
    j       rules_init                      // was: lbu t2, 0x0010(s2) (0x924A0010); delay slot (sw s4, 0x28(sp)) kept

// ---------------------------------------------------------------------------
// 2-3. mnPlayersVSCheckTimeArrowRInRange (0x80134F64) and ...LInRange
// (0x8013502C): the stock/time arrows
//
// The CSS cursor handler asks these on an A press and changes the stock
// count or time on TRUE. In normal VS answer FALSE; in Remix's modes run the
// original routine (its first two instructions are copied below).
seek_css(0x80134F64)
    j       arrow_r                         // was: lw v0, 0x0074(a0) (0x8C820074)
    nop                                     // was: lui at, 0x4040 (0x3C014040)
seek_css(0x8013502C)
    j       arrow_l                         // was: lw v0, 0x0074(a0) (0x8C820074)
    nop                                     // was: lui at, 0x4040 (0x3C014040)

// ---------------------------------------------------------------------------
// 4. VsRemixMenu.mode_specific_setup_start_ (0x804F79F4): the rule at match
// start
//
// When a normal VS match starts, Remix copies VsRemixMenu.global_game_mode
// (the rule last saved by the VS mode menu) into the CSS's game rule before
// mnPlayersVSSetSceneData writes it to the battle state. Use timed stock
// instead. The lui before it stays; Remix's modes take another path.
seek(0x804F7A5C)
    ori     t0, r0, RULE_TIMED_STOCK        // was: lw t0, 0x756C(t0) (0x8D08756C)

// ---------------------------------------------------------------------------
// The code for 1-3
seek(code_start)
rules_init:
    lbu     t2, 0x0010(s2)                  // the instruction the hook replaced
    lui     t0, VS_MODE_FLAG_HI
    lw      t0, VS_MODE_FLAG_LO(t0)         // t0 = VsRemixMenu.vs_mode_flag
    bnez    t0, _rules_done                 // Remix modes keep their own rules
    lui     t0, 0x8014
    ori     t1, r0, RULE_TIMED_STOCK
    sw      t1, -0x4254(t0)                 // CSS game rule (0x8013BDAC)
    sb      t1, 0x0003(s2)                  // battle state game_rules
    ori     t1, r0, STOCKS
    sw      t1, -0x4280(t0)                 // CSS stocks (0x8013BD80)
    sb      t1, 0x0007(s2)                  // battle state stocks
    ori     t1, r0, TIME
    sw      t1, -0x4284(t0)                 // CSS time (0x8013BD7C)
    sb      t1, 0x0006(s2)                  // battle state time_limit
    sb      r0, 0x0008(s2)                  // handicap off
    ori     t1, r0, DAMAGE
    sb      t1, 0x000B(s2)                  // damage_ratio
    sw      r0, 0x000C(s2)                  // item_toggles: none
    sb      r0, 0x001C(s2)                  // item_appearance_rate: none
_rules_done:
    j       0x8013AF40                      // back after the delay slot
    nop

arrow_r:
    lui     at, VS_MODE_FLAG_HI
    lw      at, VS_MODE_FLAG_LO(at)
    beqz    at, _arrow_locked
    lw      v0, 0x0074(a0)                  // original first instruction
    j       0x80134F6C
    lui     at, 0x4040                      // original second instruction
arrow_l:
    lui     at, VS_MODE_FLAG_HI
    lw      at, VS_MODE_FLAG_LO(at)
    beqz    at, _arrow_locked
    lw      v0, 0x0074(a0)
    j       0x80135034
    lui     at, 0x4040
_arrow_locked:
    jr      ra
    or      v0, r0, r0                      // FALSE: the cursor isn't on the arrow

if pc() > code_end {
    error "tournament-rules code overflows Item.ScuttleMinion.stage_setting_"
}
