// Smash Remix 2.0.1: "J gameplay" game option (id j-gameplay)
//
// Remix's settings start with the gameplay of its own Japanese profile
// ("Load Profile: Japanese" in Remix's settings): everything that profile
// sets differently from the default (Community) profile, except the Pokemon
// Stadium announcer:
//
//   Gameplay   Hitlag               NORMAL  -> JAPANESE
//              DI                   NORMAL  -> JAPANESE
//              Japanese Sounds      DEFAULT -> ALWAYS (every fighter's hits sound like the Japanese version)
//              Japanese Stun/Sleep  OFF     -> ON
//              Momentum Slide       OFF     -> ON
//              Shield Stun          DEFAULT -> JAPANESE
//   Stage      Whispy Mode          NORMAL  -> JAPANESE
//   Pokemon    Pokemon SFX          DEFAULT -> JAPANESE
//
// Left out: Pokemon Announcer (the Japanese profile turns it off; that word
// belongs to tournament-profile, which also turns it off). The characters
// are not changed: that's j-variants.
//
// These are defaults, not locks: players can still change any of them in
// Remix's settings (hold L on the character or stage select screen) during
// the match. They hold with any starting save: netplay usually boots with
// empty save data, and then Remix starts every toggle at its assembled value
// (edit 1 changes those). A host-picked starting save that holds Remix
// settings is loaded over them at boot, so edit 2 sets them again right after
// Remix loads the save.
//
// Works with or without the other options; it touches no byte that another
// option of the pack writes.
//
// This file is the source of the "j-gameplay" patches in
// ../smash-remix-2.0.1.json. To check or regenerate them, assemble it over a
// copy of the ROM with the bass v14 that ships with Remix
// (smashremixprivate/assembler/bass.exe), without -create:
//
//   copy Smash_Remix_2.0.1.z64 patched.z64
//   bass.exe -o patched.z64 smash-remix-2.0.1-j-gameplay.asm
//
// then diff patched.z64 against the original.
//
// ROM: Smash_Remix_2.0.1.z64, MD5 2B2D6B295106C54216B7FC7A2F14346E.
//
// Each toggle is a Menu.entry in Remix's data (RAM 0x80400000, copied from
// ROM 0x02C00000 at boot, so rom = ram - 0x7D800000); its current value is
// the word at +0x04. The values are the JP column of the entry(...) lines in
// src/Toggles.asm (~2420-2425, ~2554, ~2623); every one of them is 1.
// Addresses are from the bass symbol log of a v2.0.1 rebuild.

arch n64.cpu
endian msb

constant r0(0)
constant at(1)
constant t1(9)
constant sp(29)
constant ra(31)

// Seeks to a Remix RAM address.
macro seek(variable ram) {
    origin ram - 0x80400000 + 0x02C00000
    base ram
}

// value words (entry + 4)
constant HITLAG(0x80462428)
constant DI(0x80462458)
constant JAPANESE_SOUNDS(0x80462484)
constant J_STUN_SLEEP(0x804624BC)
constant MOMENTUM_SLIDE(0x804624F8)
constant SHIELD_STUN(0x80462530)
constant WHISPY_MODE(0x80466F44)
constant POKEMON_SFX(0x804690A4)
constant JP(1)                              // the Japanese profile's value for all eight

constant Toggles_load_(0x80460458)

// ---------------------------------------------------------------------------
// 1. The assembled values (what a boot without save data keeps)
seek(HITLAG);          dw JP                // was: 0 NORMAL
seek(DI);              dw JP                // was: 0 NORMAL
seek(JAPANESE_SOUNDS); dw JP                // was: 0 DEFAULT
seek(J_STUN_SLEEP);    dw JP                // was: 0 OFF
seek(MOMENTUM_SLIDE);  dw JP                // was: 0 OFF
seek(SHIELD_STUN);     dw JP                // was: 0 DEFAULT
seek(WHISPY_MODE);     dw JP                // was: 0 NORMAL
seek(POKEMON_SFX);     dw JP                // was: 0 DEFAULT

// ---------------------------------------------------------------------------
// 2. Boot.load_ (RAM 0x8046F368, src/Boot.asm): after a save is loaded
//
// When the save has Remix settings, Boot.load_ calls Toggles.load_, which
// overwrites every toggle with the saved value. Call jg_after_load instead,
// which calls Toggles.load_ and then sets the eight toggles again. (Boot.load_
// returns with a jump, not through ra, and restores t0 itself.)
seek(0x8046F384)
    jal     jg_after_load                   // was: jal Toggles.load_ (0x0C118116); delay slot (nop) kept

// The code sits in CharacterSelectDebugMenu.DpadControl.force_settings_for_training_
// (RAM 0x80527204-0x8052727F): its only call is commented out in
// src/Render.asm and a ROM-wide scan finds no jump, branch, pointer or lui
// pair into it.
seek(0x80527204)
jg_after_load:
    addiu   sp, sp, -0x0018
    sw      ra, 0x0014(sp)
    jal     Toggles_load_                   // what Boot.load_ called
    nop
    lw      ra, 0x0014(sp)
    addiu   sp, sp, 0x0018
    lui     at, 0x8046
    ori     t1, r0, JP
    sw      t1, 0x2428(at)                  // Hitlag
    sw      t1, 0x2458(at)                  // DI
    sw      t1, 0x2484(at)                  // Japanese Sounds
    sw      t1, 0x24BC(at)                  // Japanese Stun/Sleep
    sw      t1, 0x24F8(at)                  // Momentum Slide
    sw      t1, 0x2530(at)                  // Shield Stun
    sw      t1, 0x6F44(at)                  // Whispy Mode
    lui     at, 0x8047
    jr      ra
    sw      t1, -0x6F5C(at)                 // Pokemon SFX (0x804690A4)

if pc() > 0x80527280 {
    error "j-gameplay code overflows force_settings_for_training_"
}
