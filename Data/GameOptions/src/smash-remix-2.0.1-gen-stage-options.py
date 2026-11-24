# Generates the stage options' .asm files for Smash Remix 2.0.1:
#   smash-remix-2.0.1-stages-dream-land.asm
#   smash-remix-2.0.1-stages-competitive.asm
#   smash-remix-2.0.1-vanilla-sss-random.asm (Random's stage list for
#   vanilla-sss; smash-remix-2.0.1-vanilla-sss.asm includes it)
#
# stages-dream-land and stages-competitive restrict the stage select screen
# (and Random) to their stages on Remix's tournament layout, the way the
# ranked stage options did, but on their own: each sets the layout toggle
# itself and keeps its boot code in its own space, so a host can turn it on
# without any other option. vanilla-sss brings back the original screen in
# its own .asm; only its Random list is generated here.
#
# The values come from the ROM itself:
#  - Random's stage list: Stages.swap_stage_ calls add_stage_to_random_list_
#    once per stage toggle (lui a0 / ori a0 / jal / ori a1, stage id). Decoding
#    those calls is the only map from a stage id to its toggle entry.
#  - Toggles.profile_defaults_TE: on the tournament layout, Random also skips
#    stages whose Tournament default is off.
#  - Stages.stage_table_tournament (18 stage ids per page, Random last),
#    Stages.stage_table_pages, Stages.tournament_hazard_mode_table.
# Addresses are from the bass symbol log of a v2.0.1 rebuild (bass -sym);
# the script checks the ROM's MD5 and the code it decodes.
#
# usage: python -I smash-remix-2.0.1-gen-stage-options.py <Smash_Remix_2.0.1.z64> <out dir>
import hashlib, os, struct, sys

ROM_MD5 = "2B2D6B295106C54216B7FC7A2F14346E"
REMIX = 0x80400000 - 0x02C00000

# v2.0.1 symbols
SYM = {
    'Stages.swap_stage_': 0x8045E144,
    'Stages.add_stage_to_random_list_': 0x804556B8,
    'Stages.get_stage_id_': 0x80454930,
    'Stages.stage_table': 0x804566F8,
    'Stages.stage_table_pages': 0x80456700,
    'Stages.stage_table_tournament': 0x804568E0,
    'Stages.tournament_hazard_mode_table': 0x8045B1C0,
    'Toggles.entry_sss_layout': 0x80466ED0,
    'Toggles.entry_hazard_mode': 0x80466F0C,
    'Toggles.entry_random_stage_dream_land': 0x80467150,
    'Toggles.profile_defaults_TE': 0x8046A658,
    'VsRemixMenu.global_stage_select': 0x804F757C,
}

STAGE_RANDOM = 0xDE
NO_STAGE = 0xFF
ICONS_PER_PAGE = 18          # Stages.NUM_ICONS (6 columns x 3 rows); Random is the last icon of a page
TOURNAMENT_PAGES = 3         # stage_table_tournament's size
SSS_TOURNAMENT = 1           # Toggles.entry_sss_layout value

# hazard modes (Hazards.mode): 0 hazards on/movement on, 1 hazards off, 2 movement off, 3 both off
OPTIONS = [
    {
        'id': 'stages-dream-land',
        'file': 'smash-remix-2.0.1-stages-dream-land.asm',
        'title': 'Dream Land only',
        'stages': [(0x06, 0)],
        'select_off': True,
    },
    {
        'id': 'stages-competitive',
        'file': 'smash-remix-2.0.1-stages-competitive.asm',
        'title': 'Competitive stages',
        # the ranked ladder's Remix stage list (SmashRemix.cpp kRemixStages)
        'stages': [(0x06, 0), (0x6F, 0), (0xA4, 0), (0x2D, 0)],
        'select_off': False,
    },
    {
        'id': 'vanilla-sss',
        'file': 'smash-remix-2.0.1-vanilla-sss-random.asm',
        'title': 'Vanilla SSS, Random stage list',
        # the original 9 stages; vanilla-sss brings back the original screen, so
        # only Random's list is generated here
        'stages': [(0x00, 0), (0x01, 0), (0x02, 0), (0x03, 0), (0x04, 0),
                   (0x05, 0), (0x06, 0), (0x07, 0), (0x08, 0)],
        'select_off': False,
        'random_only': True,
    },
]


def main():
    rom = open(sys.argv[1], 'rb').read()
    assert hashlib.md5(rom).hexdigest().upper() == ROM_MD5, "not the Smash Remix 2.0.1 ROM"
    out_dir = sys.argv[2]
    sym = SYM

    def r32(ram):
        o = ram - REMIX
        return struct.unpack('>I', rom[o:o + 4])[0]

    def r8(ram):
        return rom[ram - REMIX]

    def title(entry):
        o = entry + 0x28 - REMIX
        return rom[o:o + 48].split(b'\0')[0].decode('latin1').replace('\x7e', '~')

    # Random's stage list, in the order swap_stage_ adds stages
    base, add = sym['Stages.swap_stage_'], sym['Stages.add_stage_to_random_list_']
    assert r32(add) == 0x27BDFFF0, "add_stage_to_random_list_ moved"
    jal = 0x0C000000 | ((add >> 2) & 0x03FFFFFF)

    def decode(at):
        lui, ori, j, idw = r32(at), r32(at + 4), r32(at + 8), r32(at + 12)
        if (lui >> 16) != 0x3C04 or (ori >> 16) != 0x3484 or j != jal or (idw & 0xFFFFFF00) != 0x34050000:
            return None
        return idw & 0xFF, ((lui & 0xFFFF) << 16) | (ori & 0xFFFF)

    at = base
    while at < base + 0x100 and decode(at) is None:
        at += 4
    random_list = []
    while decode(at) is not None:
        random_list.append(decode(at))
        at += 16
    entries = dict(random_list)
    assert entries.get(0x06) == sym['Toggles.entry_random_stage_dream_land'], "random list decoding is off"
    assert len(entries) == len(random_list), "a stage is in the random list twice"

    te = sym['Toggles.profile_defaults_TE']
    layout_entry = sym['Toggles.entry_sss_layout']
    hazard_entry = sym['Toggles.entry_hazard_mode']
    stage_table = sym['Stages.stage_table']
    pages_table = sym['Stages.stage_table_pages']
    tournament = sym['Stages.stage_table_tournament']
    hazards = sym['Stages.tournament_hazard_mode_table']
    hovered = sym['Stages.get_stage_id_']
    assert r32(stage_table + SSS_TOURNAMENT * 4) == tournament
    assert title(layout_entry) == 'Stage Select Layout' and r32(layout_entry + 4) == 0
    assert title(hazard_entry) == 'Hazard Mode'
    assert r32(sym['VsRemixMenu.global_stage_select']) == 1

    for opt in OPTIONS:
        stages = opt['stages']
        allowed = {sid: mode for sid, mode in stages}
        assert len(allowed) == len(stages)
        assert all(sid in entries for sid in allowed), "an allowed stage has no random toggle"
        L = []
        w = L.append

        def emit_random(w, n):
            w("// ---------------------------------------------------------------------------")
            w("// %d. Random's stage toggles (Toggles.entry_random_stage_*, value word at +4):" % (n + 1))
            w("// on for this option's stages, off for every other stage in Random's list.")
            w("// Only toggles whose value changes are listed (assembled values are the")
            w("// Community profile's).")
            kept = []
            for sid, entry in random_list:
                want = 1 if sid in allowed else 0
                cur = r32(entry + 4)
                if cur != want:
                    w("toggle(0x%08X, %d)    // 0x%02X %s: %s -> %s" % (entry, want, sid, title(entry), "ON" if cur else "OFF", "ON" if want else "OFF"))
                else:
                    kept.append((sid, entry, cur))
            w("// unchanged:")
            for sid, entry, cur in kept:
                w("//   0x%08X 0x%02X %s: already %s" % (entry, sid, title(entry), "ON" if cur else "OFF"))
            w("")
        if opt.get('random_only'):
            w("// Smash Remix 2.0.1: Random's stage list for the \"%s\" game option" % opt['id'])
            w("// (included by smash-remix-2.0.1-%s.asm)" % opt['id'])
            w("//")
            w("// GENERATED by smash-remix-2.0.1-gen-stage-options.py from the ROM; edit the")
            w("// stage list there and regenerate. Random (on the stage select screen, or")
            w("// when stage select is off) picks among these stages only:")
            for sid, mode in stages:
                w("//   0x%02X %s" % (sid, title(entries[sid])))
            w("//")
            w("// Netplay boots with empty save data, so Remix never loads saved toggles and")
            w("// every toggle starts at its assembled value, which this file sets.")
            w("")
            n = 0
            emit_random(w, n)
            path = os.path.join(out_dir, opt['file'])
            with open(path, 'w', newline='\n') as f:
                f.write('\n'.join(L) + '\n')
            print("wrote", path)
            continue
        w("// Smash Remix 2.0.1: \"%s\" game option (%s)" % (opt['id'], opt['title']))
        w("//")
        w("// GENERATED by smash-remix-2.0.1-gen-stage-options.py from the ROM; edit the")
        w("// stage list there and regenerate. Stages and the hazard mode each one gets:")
        for sid, mode in stages:
            w("//   0x%02X %-24s hazard mode %d" % (sid, title(entries[sid]), mode))
        w("// (hazard modes: 0 hazards and movement on, 1 hazards off, 2 movement off, 3 both off)")
        w("//")
        if opt['select_off']:
            w("// Stage select is off: every game is on the stage above. When a match starts")
            w("// without stage select, Remix picks from Random's stage list")
            w("// (Stages.swap_stage_), which holds only this stage. If a player turns")
            w("// stage select back on in the VS options, the stage select screen offers")
            w("// only this stage and Random.")
        else:
            w("// The stage select screen offers only these stages and Random, and Random")
            w("// picks among them only.")
        w("//")
        w("// How: Remix's tournament stage select layout (Toggles \"Stage Select Layout\")")
        w("// is a stage table of its own that has no d-pad layout variants (DL, Omega,")
        w("// Remix...) and no L-button hazard toggle: hovering a stage, or Random's")
        w("// pick, sets the hazard mode from Stages.tournament_hazard_mode_table. This")
        w("// option turns that layout on and rewrites its first page; Random's stage")
        w("// toggles hold exactly these stages.")
        w("//")
        w("// Netplay boots with empty save data, so Remix never loads saved toggles and")
        w("// every toggle starts at its assembled value, which this file sets. Changing")
        w("// them on purpose in Remix's settings menu (hold L on the character or stage")
        w("// select screen) still works, for every player alike.")
        w("//")
        w("// The stage options (vanilla-sss, stages-dream-land, stages-competitive)")
        w("// write the same bytes and can't be on together.")
        w("//")
        w("// To check the JSON, assemble this file over a copy of the ROM with the bass")
        w("// v14 that ships with Remix, without -create (bass then writes only these")
        w("// bytes), and diff the copy against the original:")
        w("//   copy Smash_Remix_2.0.1.z64 patched.z64")
        w("//   bass.exe -o patched.z64 %s" % opt['file'])
        w("//")
        w("// Remix code and data are at RAM 0x80400000, copied unchanged from ROM")
        w("// 0x02C00000 at boot (rom = ram - 0x7D800000); the N64 logo overlay is")
        w("// loaded uncompressed to RAM 0x80131B00 (rom = ram - 0x7FFB2E40).")
        w("//")
        w("// ROM: Smash_Remix_2.0.1.z64, MD5 %s." % ROM_MD5)
        w("")
        w("arch n64.cpu")
        w("endian msb")
        w("")
        w("constant r0(0)")
        w("constant t0(8)")
        w("")
        w("macro seek(variable ram) {")
        w("    origin ram - 0x80400000 + 0x02C00000")
        w("    base ram")
        w("}")
        w("macro seek_logo(variable ram) {")
        w("    origin ram - 0x7FFB2E40")
        w("    base ram")
        w("}")
        w("macro toggle(variable entry, variable value) {")
        w("    seek(entry + 4)")
        w("    dw value")
        w("}")
        w("")
        n = 0
        if opt['select_off']:
            w("// ---------------------------------------------------------------------------")
            w("// %d. Stage select off" % (n + 1)); n += 1
            w("//")
            w("// mnStartupFuncStart (N64 logo scene start, 0x80131CB8) calls")
            w("// gcMakeDefaultCameraGObj once per boot, after scManagerRunLoop has set up")
            w("// the VS battle state and before any menu reads it. Call stage_boot_init")
            w("// instead; it turns stage select off in the battle state (the VS options")
            w("// menu shows and changes it; the character select screen reads it when a")
            w("// match starts, mnPlayersVSFuncRun) and jumps on with a0-a3 untouched.")
            w("//")
            w("// stage_boot_init sits in the logo overlay's own dead code: the \"tap to skip")
            w("// to the title\" branch of mnStartupActorFuncRun (0x80131C60-0x80131C7C), which")
            w("// Remix's Boot.asm made unreachable by branching over it (0x80131C58,")
            w("// `b 0x80131C80`). Nothing else jumps or branches into it, and the overlay is")
            w("// in RAM while its start function runs.")
            w("constant gcMakeDefaultCameraGObj(0x8000B9FC)")
            w("constant stage_boot_init(0x80131C60)")
            assert rom[0x80131D44 - 0x7FFB2E40:0x80131D44 - 0x7FFB2E40 + 4] == bytes.fromhex('0C002E7F')
            assert rom[0x80131C58 - 0x7FFB2E40:0x80131C58 - 0x7FFB2E40 + 4] == bytes.fromhex('10000009')
            w("seek_logo(0x80131D44)")
            w("    jal     stage_boot_init                 // was: jal gcMakeDefaultCameraGObj (0x0C002E7F); delay slot (addiu a3, r0, 2) kept")
            w("seek_logo(stage_boot_init)")
            w("    lui     t0, 0x800A                      // was: lui v0, 0x800A")
            w("    j       gcMakeDefaultCameraGObj         // was: addiu v0, v0, 0x4AD0")
            w("    sb      r0, 0x4D12(t0)                  // was: lbu t7, 0(v0); gSCManagerTransferBattleState (0x800A4D08) +0x0A is_stage_select = off")
            w("")
            w("// VsRemixMenu.global_stage_select: Remix's copy of the battle state's flag,")
            w("// saved when the VS menu is left for the character select screen and put")
            w("// back when the character select screen is left for the VS menu. Off, so")
            w("// it matches the boot value until the first save.")
            w("seek(0x%08X)" % sym['VsRemixMenu.global_stage_select'])
            w("    dw 0                                    // was: 1")
            w("")
        w("// ---------------------------------------------------------------------------")
        w("// %d. Stage Select Layout: TOURNAMENT" % (n + 1)); n += 1
        w("toggle(0x%08X, %d)                       // was: NORMAL (0)" % (layout_entry, SSS_TOURNAMENT))
        if r32(hazard_entry + 4) != 0:
            w("toggle(0x%08X, 0)                        // Hazard Mode: NORMAL" % hazard_entry)
        else:
            w("// Toggles.entry_hazard_mode starts at 0 (hazards and movement on); on this")
            w("// layout hovering a stage or Random's pick sets it from the table below.")
        w("")
        emit_random(w, n); n += 1
        w("// Tournament profile defaults (Toggles.profile_defaults_TE, one word per toggle):")
        w("// on the tournament layout add_stage_to_random_list_ also skips a stage whose")
        w("// Tournament default is off.")
        for sid, mode in stages:
            entry = entries[sid]
            tog = r32(entry + 0x24)
            addr = te + (tog - 1) * 4
            if r32(addr) != 1:
                w("seek(0x%08X)    // TE default of toggle #%d (0x%02X %s)" % (addr, tog, sid, title(entry)))
                w("    dw 1                                    // was: 0")
            else:
                w("//   toggle #%d (0x%02X %s): already on" % (tog, sid, title(entry)))
        w("")
        # stage select page: the stages, then Random on every other icon
        per_page = ICONS_PER_PAGE - 1
        pages = (len(stages) + per_page - 1) // per_page
        assert pages <= TOURNAMENT_PAGES
        slots = []
        nxt = 0
        for slot in range(pages * ICONS_PER_PAGE):
            if slot % ICONS_PER_PAGE != per_page and nxt < len(stages):
                slots.append(stages[nxt][0]); nxt += 1
            else:
                slots.append(STAGE_RANDOM)
        w("// ---------------------------------------------------------------------------")
        w("// %d. Stage select: the tournament layout's stage table (Stages.stage_table_tournament," % (n + 1)); n += 1
        w("// %d stage ids per page, 6 columns by 3 rows). %d page: this option's stages, Random (0x%02X)" % (ICONS_PER_PAGE, pages, STAGE_RANDOM))
        w("// on every other icon.")
        old = rom[tournament - REMIX: tournament - REMIX + pages * ICONS_PER_PAGE]
        w("// was: %s" % ' '.join('%02X' % b for b in old))
        w("seek(0x%08X)" % tournament)
        for row in range(0, len(slots), 6):
            w("    db " + ', '.join('0x%02X' % s for s in slots[row:row + 6]))
        w("")
        w("// Stages.stage_table_pages[TOURNAMENT]: pages the tournament layout has (R/Z")
        w("// switch pages; with one page they stay on it)")
        w("seek(0x%08X)" % (pages_table + SSS_TOURNAMENT * 4))
        w("    dw %d                                    // was: %d" % (pages, r32(pages_table + SSS_TOURNAMENT * 4)))
        w("")
        w("// Stages.tournament_hazard_mode_table (one byte per stage id): the hazard mode")
        w("// hovering a stage or Random's pick applies.")
        for sid, mode in stages:
            cur = r8(hazards + sid)
            if cur != mode:
                w("seek(0x%08X)    // 0x%02X %s" % (hazards + sid, sid, title(entries[sid])))
                w("    db %d                                    // was: %d" % (mode, cur))
            else:
                w("//   0x%02X %s: already %d" % (sid, title(entries[sid]), mode))
        w("")
        # get_stage_id_: per-page exceptions
        checks = []
        for a in range(hovered, hovered + 0x400, 4):
            word = r32(a)
            if word == 0x03E00008:
                break
            if word in (0x34080006, 0x34080073, 0x34080072):
                checks.append((a, word))
        assert len(checks) == 3, checks
        w("// Stages.get_stage_id_ (Stages.asm ~1484): on the tournament layout Remix")
        w("// gives Dream Land (page 3), Yoshi's Island DL (pages 1-2) and Saffron City")
        w("// DL (page 3) a hazard mode by page instead of from the table. Compare")
        w("// against no stage (0x%02X) instead, so the table decides for every stage." % NO_STAGE)
        for a, word in checks:
            w("seek(0x%08X)" % a)
            w("    ori     t0, r0, 0x%02X                    // was: ori t0, r0, 0x%02X (0x%08X)" % (NO_STAGE, word & 0xFF, word))
        path = os.path.join(out_dir, opt['file'])
        with open(path, 'w', newline='\n') as f:
            f.write('\n'.join(L) + '\n')
        print("wrote", path)


main()
