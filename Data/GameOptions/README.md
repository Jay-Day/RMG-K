# Game options packs

A game options pack is a JSON file in this folder that offers a room's host extra options for one game. Each option is a set of ROM patches. When the host turns an option on, every client applies its patches to the ROM image in memory at match start, before the game boots, and the replay records them. Packs are matched by the MD5 of the unpatched ROM.

## Schema (`format` 1)

```json
{
  "format": 1,
  "id": "smash-remix-2.0.1",
  "game": "Smash Remix 2.0.1",
  "romMd5": ["2B2D6B295106C54216B7FC7A2F14346E"],
  "options": [
    {
      "id": "vanilla-only",
      "label": "Vanilla only",
      "description": "Only the original 12 characters can be picked.",
      "translations": { "ja": { "label": "...", "description": "..." } },
      "patches": [
        { "offset": "0x02D1ED68", "expect": "10000042", "bytes": "10000029", "note": "..." }
      ]
    }
  ]
}
```

- `romMd5`: uppercase MD5s of the ROMs the pack applies to, as big-endian `.z64` files.
- `offset`: a ROM file offset in hex, in big-endian `.z64` byte order.
- `expect`: the bytes the unpatched ROM has at `offset`. `bytes` is the replacement. Both are uppercase hex strings of the same length.
- If any `expect` doesn't match, the option can't be used with that ROM. Apply all of an option's patches or none of them.
- `note`: free text for maintainers.
- `translations`: optional. Keys are UI language codes.
- `serverOnly`: optional, `false` by default. A server-only option is never offered to a room's host as a switch: only the lobby puts it on a match, as part of a ranked ruleset (the lobby loads the same packs; see "Ranked matches" below). The emulator still reads its label and translations, to show what a ranked room plays with.

Keep patches out of ROM 0x1000-0x101000: the boot code (CIC) checksums that range, so editing it means fixing the header CRCs too.

## Smash Remix 2.0.1: `vanilla-only`

Only Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu, Jigglypuff and Ness (ids 0x00-0x0B) can be picked, as a human or as a CPU. Costumes, shades and teams work as before.

The source is [`src/smash-remix-2.0.1-vanilla-only.asm`](src/smash-remix-2.0.1-vanilla-only.asm). It names the Remix routine each edit hooks and the instruction it replaces, and lists every way to get a character that it closes. In short:

- `CharacterSelect.get_character_id_` maps a token's position to a character on every character select screen. Its d-pad variant step now returns no character for anything outside 0x00-0x0B. That covers Remix portraits, the bonus bookend, variants and the 12-Character Battle grid.
- Both random routines always use Remix's own "Vanilla" random mode. That covers the "?" portrait, L-button random, new CPU panels and Tag Team.
- 12-Character Battle stays on its Default (vanilla) character set.
- The vanilla fighters' d-pad variant table rows are emptied, so their panels show no variant icons.
- A safety net drops a non-vanilla character restored when the VS character select opens.

How it was made: Remix's code is assembled at RAM 0x80400000 and copied there unchanged from ROM 0x02C00000 by one boot DMA, so ROM offset = RAM address - 0x7D800000. The addresses come from the bass symbol log of the 2.0.1 build (`Data/Ranked/SmashRemix/<MD5>.sym` on the ranked branch) plus disassembly of the ROM. To check the JSON, copy the ROM and assemble the `.asm` over the copy with Remix's bass v14 (`bass -o copy.z64 smash-remix-2.0.1-vanilla-only.asm`, without `-create`). Then compare the copy against the original ROM.

Porting to a newer Remix: build it with `-sym`, find the same routines, check that the instructions match the `was:` comments in the `.asm`, update the addresses, reassemble, and regenerate `expect`/`bytes` from the diff. Then add a new pack file with the new MD5.

## Smash Remix 2.0.1: ranked options (`serverOnly`)

The lobby puts these on ranked matches; hosts never see them. They replace what the ranked module used to write into memory every frame, so the module only reads.

| Option | Source | What it does |
| --- | --- | --- |
| `ranked-rules` | `src/smash-remix-2.0.1-ranked-rules.asm` | Boots from the N64 logo straight to the VS character select. 4 stocks, 8 minutes (timed stock), no items, no handicap, 100% damage. The CSS's stock/time arrows, stock/time picker, team button, Back and hold-B do nothing, and the hold-L settings shortcut is gone, so only the CSS, stage select, battle and results are reachable. Remix's toggles are the Tournament profile, with Skip Results Screen on and Salty Runback off. |
| `ranked-teams` | `src/smash-remix-2.0.1-ranked-teams.asm` | Team battle and team attack on (2v2). |
| `stages-dream-land` | `src/smash-remix-2.0.1-stages-dream-land.asm` (generated) | No stage select; every game is on Dream Land (hazards on). |
| `stages-remix-ranked` | `src/smash-remix-2.0.1-stages-remix-ranked.asm` (generated) | Stage select on the tournament layout, with Dream Land, Goomba Road, Pokemon Stadium 2, Pokemon Stadium, and Random (picking among them) on the other icons. |
| `teams-p1p2`, `teams-p1p3`, `teams-p1p4` | `src/smash-remix-2.0.1-teams.asm` (`bass -d TEAMMATE=N`) | The CSS opens with all four ports human, P1 and its teammate red, the others blue. |

How they fit together:
- Netplay boots with an empty save, so every Remix toggle starts at its assembled (Community) value. The options patch those values and Remix's saved VS menu settings in the ROM.
- The VS battle state's defaults are in the CIC-checksummed first MiB (ROM 0x42D18), so they're set by boot code instead. Each option that needs it hooks its own `jal` in the N64 logo overlay (ROM = RAM - 0x7FFB2E40) and jumps on to the original callee afterwards: `ranked-rules` the logo's exit (0x80131CA0), `ranked-teams`, `teams-p1pN` and `stages-dream-land` three calls in its scene start (0x80131D10, 0x80131D28, 0x80131D44).
- Their code sits in the body of `Toggles.settings_shortcut` (0x8046EFCC), which `ranked-rules` turns into an immediate return; nothing else jumps into it. Each option owns its own words there (`ranked-rules` 0x8046EFD4, `ranked-teams` 0x8046F020, `teams-p1pN` 0x8046F040, `stages-dream-land` 0x8046F080). So those options need `ranked-rules`; the lobby always sends them together.
- The VS CSS overlay is patched at its ROM offset (ROM = RAM - 0x80001D80). SSB64 loads overlays with a plain DMA, with no relocation and no compression.
- No two options in a ruleset's set touch the same byte, `ranked-rules` and `vanilla-only` included. The two stage options overlap each other, and so do the three team options; a match never has more than one of each.
- `smash-remix-2.0.1-gen-stage-options.py` writes the two stage `.asm` files from the ROM and the `.sym`. It decodes Random's stage list from `Stages.swap_stage_` and lays out the tournament table the way the module's `build_stage_select` did. To change a stage list, edit it there, regenerate, and reassemble.

## How the emulator reads packs

- It loads every `*.json` here (the installed `Data/GameOptions`), then the user's `GameOptions` folder in the config directory (`Config/GameOptions` in a portable install). A user pack with the same `id` as a shipped one replaces it.
- A pack needs `format` 1, an `id` of `[a-z0-9._-]` (up to 64), at least one `romMd5` (32 hex digits, any case) and at least one usable option. Unknown fields are ignored. A pack or option that breaks a rule is skipped and logged ("Game options: ..." in the debug log); the rest still load.
- Option ids are `[a-z0-9-]`, up to 32. Labels must be UTF-8 without control characters, up to 256 bytes. If two packs offer the same option id for a ROM, the first pack (shipped ones before the user's, each by file name) wins.
- `offset` can also be a plain JSON number (decimal). Hex in `expect`/`bytes` may be lowercase and may have spaces between bytes.
- One option has at most 256 patches and 64 KiB of patch bytes, and its patches must not overlap. A match carries at most 8 options and 64 KiB of patch bytes in all; options whose patches overlap can't be on together.
- An option that changes ROM 0x1000-0x101000 must also patch all of the header CRCs (0x10-0x17), or it's skipped.
- The host checks every `expect` against its ROM file when the match starts. Players, late joiners, live viewers and replays only get the offsets and bytes (the manifest and the `.krec` carry them), so they don't need the pack.

## Ranked matches

A ranked room's game options are its ruleset's, which the lobby picks from its own copy of these packs (`rulesetOptions` in its `ranked.json`, often server-only options). Nobody in the room can turn them on or off, the host included. When a ranked match starts, the lobby's `MATCH_BEGIN` carries every option's patches with their `expect` bytes. Every player checks each `expect` against their own ROM and doesn't start if one doesn't match, so no player needs a pack. The host's pre-match manifest has to carry exactly the lobby's options: a player whose host sent other ones doesn't start either. The `.krec` gets them like any other match's, so ranked replays play without a pack too.
