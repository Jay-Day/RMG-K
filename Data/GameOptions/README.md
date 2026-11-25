# Game options packs

A game options pack is a JSON file in this folder that offers a room's host extra options for one game. Each option is a set of ROM patches. When the host turns an option on, every client applies its patches to the ROM image in memory at match start, before the game boots, and the replay records them. Packs are matched by the MD5 of the unpatched ROM.

A pack can also name presets: sets of its options the host picks in one go from the room's Game Settings list ("1v1 Vanilla"). The host can still turn options on and off one by one (Game Settings > Custom), where they're listed under the pack's groups, and save a set there under a name of their own. Saved sets are kept on the host's PC per ROM and listed after the presets; a room on one carries it as its preset with the id `user-<8 hex digits>` and the host's name for it, so preset ids starting with `user-` are reserved for those.

## Schema (`format` 1)

```json
{
  "format": 1,
  "id": "smash-remix-2.0.1",
  "game": "Smash Remix 2.0.1",
  "romMd5": ["2B2D6B295106C54216B7FC7A2F14346E"],
  "groups": [
    { "id": "characters", "label": "Characters", "translations": { "ja": { "label": "..." } } }
  ],
  "presets": [
    {
      "id": "1v1-vanilla",
      "label": "1v1 Vanilla",
      "description": "...",
      "translations": { "ja": { "label": "...", "description": "..." } },
      "options": ["vanilla-css"]
    }
  ],
  "options": [
    {
      "id": "vanilla-css",
      "label": "Vanilla CSS",
      "group": "characters",
      "description": "Only the original 12 characters can be picked.",
      "translations": { "ja": { "label": "...", "description": "..." } },
      "requires": [],
      "conflicts": [],
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
- `translations`: optional, on options, groups and presets. Keys are UI language codes.
- `group`: optional. The id of one of the pack's `groups`; the host's list of options shows it under that group's heading. Options without one come last, under "Other".
- `requires`: optional. Ids of options that must be on whenever this one is (J variants needs Vanilla CSS). Turning this one on turns them on too, and turning one of them off turns this one off.
- `conflicts`: optional. Ids of options that can't be on together with this one (two stage lists). It goes both ways, so naming it on one side is enough. Turning this one on turns them off.
- `groups`: optional, in display order. Each has an `id` (`[a-z0-9-]`, up to 32) and a `label`.
- `presets`: optional, in display order. Each has an `id` (`[a-z0-9-]`, up to 32), a `label` (keep it to 48 characters: the lobby server shows no more), a `description` and the `options` it turns on (at least one; the unchanged game is always offered as Default). Turning on exactly a preset's options shows that preset as the room's choice. Don't start an id with `user-`: clients read that as a host's saved set.

Keep patches out of ROM 0x1000-0x101000: the boot code (CIC) checksums that range, so editing it means fixing the header CRCs too.

Options that can be on together must change different bytes. Two options whose patches overlap have to be listed in `conflicts` (on either side).

## Smash Remix 2.0.1: `vanilla-css`

It was called "Vanilla only" (`vanilla-only`) before presets. The patches are the same.

Only Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu, Jigglypuff and Ness (ids 0x00-0x0B) can be picked, as a human or as a CPU, and the VS character select screen looks like the original game's: the 12 original portraits in the original grid. Costumes, shades and teams work as before.

The source is [`src/smash-remix-2.0.1-vanilla-only.asm`](src/smash-remix-2.0.1-vanilla-only.asm). It names the routine each edit hooks and the instruction it replaces, and lists every way to get a character that it closes. In short:

Part 1, the lock (every character select screen):

- `CharacterSelect.get_character_id_` maps a token's position to a character on Remix's select screens. Its d-pad variant step now returns no character for anything outside 0x00-0x0B. That covers Remix portraits, the bonus bookend, variants and the 12-Character Battle grid.
- Both random routines always use Remix's own "Vanilla" random mode. That covers the "?" portrait, L-button random, new CPU panels and Tag Team.
- 12-Character Battle stays on its Default (vanilla) character set.
- The vanilla fighters' d-pad variant table rows are emptied, so their panels show no variant icons.
- A safety net drops a non-vanilla character restored when the VS character select opens.

Part 2, the original VS character select screen:

- Remix stopped loading the original portraits file (file 0x13) and drew its own grid instead. The file is still in the ROM, unchanged, so the screen loads it again and the original routine draws the portraits. Remix's grid, its bonus bookend and its random "?" aren't drawn. No image data is shipped.
- Remix hooked the original screen code wherever it turns a token position into a fighter or a fighter into a portrait: the hit test, the slide-in, token placement, the white flash and the drift of placed tokens. Those hooks take the original code path again, which can only give the 12 original fighters. 12-Character Battle uses the same screen with Remix's own grid, so each hook checks Remix's 12-Character Battle flag first and keeps Remix's behavior there.
- Tokens are full size again. L-button random and new CPU panels put their token on the original portrait.
- Training, 1P and Bonus keep Remix's select screens, with the lock.

How it was made: Remix's code is assembled at RAM 0x80400000 and copied there unchanged from ROM 0x02C00000 by one boot DMA, so ROM offset = RAM address - 0x7D800000. The original VS select screen's code (overlay 26) is loaded uncompressed from ROM 0x12FD80 to RAM 0x80131B00 whenever that screen opens, so ROM offset = RAM address - 0x80001D80 there. The addresses come from the bass symbol log of a v2.0.1 rebuild (`bass -sym`), from the ssb-decomp-re decompilation of the original game, and from a diff of that overlay between the original ROM and Remix. To check the JSON, copy the ROM and assemble the `.asm` over the copy with Remix's bass v14 (`bass -o copy.z64 smash-remix-2.0.1-vanilla-only.asm`, without `-create`). Then compare the copy against the original ROM.

Porting to a newer Remix: build it with `-sym`, find the same routines, check that the instructions match the `was:` comments in the `.asm`, update the addresses, reassemble, and regenerate `expect`/`bytes` from the diff. Then add a new pack file with the new MD5.

## Smash Remix 2.0.1: the other options

Each option below has its source in `src/`, made and checked the same way as `vanilla-css`: assemble the `.asm` over a copy of the ROM with bass v14, without `-create`, and diff the copy against the original. The three stage options' sources are generated by [`src/smash-remix-2.0.1-gen-stage-options.py`](src/smash-remix-2.0.1-gen-stage-options.py) (`python smash-remix-2.0.1-gen-stage-options.py Smash_Remix_2.0.1.z64 <out dir>`), which reads Random's stage list and the stage tables from the ROM; edit the stage lists there and regenerate. Code in Remix's own space uses ROM offset = RAM address - 0x7D800000, and code in the VS character select overlay ROM = RAM - 0x80001D80, as above.

Any of them can be on together, except that the three stage options conflict with each other and `j-variants` needs `vanilla-css`. All the state they add lives in emulated RAM, and none of them reads save data, so every client runs the same game.

### `j-variants` (J variants)

Needs `vanilla-css`. Players pick among the original 12 on the original character select screen, and from the stage select on every fighter is the Japanese-version variant Remix ships: J Mario, J Fox, J DK, J Samus, J Luigi, J Link, J Yoshi, J Falcon, J Kirby, J Pikachu, Purin and J Ness. That goes for humans, CPUs and random picks. The select screen shows the original fighter; the battle, stock icons and results show the J one ("PURIN WIN!").

Source: [`src/smash-remix-2.0.1-j-variants.asm`](src/smash-remix-2.0.1-j-variants.asm).

- When the VS character select hands over to the stage select or the battle, the four fighter ids it wrote to the VS battle state, and Tag Team's queues, are swapped for the J ids.
- When the VS character select opens again, they're swapped back before anything reads them, so a rematch shows the same picks on the original grid.
- 12-Character Battle keeps the original fighters. Training, 1P and Bonus aren't changed.
- The code and the id table sit in the rest of `mnPlayersVSMakePortraitShadow` after `vanilla-css`'s dispatchers (RAM 0x801323FC-0x801324DB), which is unreachable once no fighter is locked.

### `vanilla-sss` (Vanilla SSS)

The VS stage select screen is the original game's: 10 big icons in two rows (Peach's Castle, Congo Jungle, Hyrule Castle, Planet Zebes, Mushroom Kingdom, Yoshi's Island, Dream Land, Sector Z, Saffron City and Random), the original cursor, the yellow name plate and the series emblem. Only those 9 stages can be played; Random, and a match with stage select off, pick among them. Hazards and stage movement are on. Remix's extras on that screen go with its grid: R/Z pages, the hazard and layout toggles, the Random roulette and the hold-L settings shortcut (it still works on the character select screen).

Source: [`src/smash-remix-2.0.1-vanilla-sss.asm`](src/smash-remix-2.0.1-vanilla-sss.asm), which includes the generated Random list [`src/smash-remix-2.0.1-vanilla-sss-random.asm`](src/smash-remix-2.0.1-vanilla-sss-random.asm).

- The stage select's code (mnMaps, scene 0x15) is loaded from ROM 0x14D670 to RAM 0x80131B00, so ROM = RAM - 0x7FFE4490 there. The original drawing, cursor and cursor movement that Remix patched out are put back, with the original words taken from the ROM Remix is built on.
- `Stages.get_stage_id_` answers with the stage of the original slot under the cursor, so the Remix hooks that stay (the preview, Random's preview, the pick) work on the original slots. `Stages.setup_` draws nothing.
- Random's stage toggles are on for the 9 stages and off for every other one.
- Mushroom Kingdom is never locked, so the screen doesn't depend on save data.

### `stages-dream-land` (Dream Land only)

There is no stage select: from the character select, Start goes straight to the battle, and every game is on Dream Land with hazards on. If a player turns stage select back on in the VS options, the stage select offers only Dream Land and Random, and Random is Dream Land.

Source: [`src/smash-remix-2.0.1-stages-dream-land.asm`](src/smash-remix-2.0.1-stages-dream-land.asm) (generated).

- At boot, the N64 logo's scene start (RAM 0x80131D44) calls 3 words of new code that clear `is_stage_select` in the VS battle state, whose defaults are in the CIC range and can't be patched directly. The code sits in the logo overlay's unreachable "tap to skip to title" branch (RAM 0x80131C60). `VsRemixMenu.global_stage_select` is set to 0 too.
- Remix's Stage Select Layout is set to Tournament. That layout's page 1 holds Dream Land and Random, it has 1 page, and the per-page hazard exceptions in `Stages.get_stage_id_` are taken out.
- Every random stage toggle is off except Dream Land. A match without stage select takes its stage from Random's list.

This is a standalone version of ranked-ssb's option of the same name, which needs `ranked-rules`.

### `stages-competitive` (Competitive stages)

The stage select is Remix's tournament layout with one page: Dream Land, Goomba Road, Pokemon Stadium 2 and Pokemon Stadium, with Random on every other icon. Random picks among the 4. Hazards are on.

Source: [`src/smash-remix-2.0.1-stages-competitive.asm`](src/smash-remix-2.0.1-stages-competitive.asm) (generated).

- The same layout edits as `stages-dream-land` (Tournament layout, page 1 rewritten, 1 page, no per-page hazard exceptions), with the 4 stages.
- Random's stage toggles are on for the 4 stages and off for every other one. There is no boot code.

It's the stage list of ranked-ssb's `stages-remix-ranked`, without the need for `ranked-rules`.

### `tournament-rules` (Tournament rules)

Every normal VS match is 4 stocks, 8 minutes (Remix's timed stock rule), no items, no handicap and 100% damage. On the character select the stock/time arrows do nothing; the stock/time picker still flips to show the time limit. A change made in the VS mode menu or VS options is undone the next time the character select opens. Remix's own modes (12-Character Battle, Tag Team, King of the Hill, Smashketball, Tug of War) keep their own rules.

Source: [`src/smash-remix-2.0.1-tournament-rules.asm`](src/smash-remix-2.0.1-tournament-rules.asm).

- These are Remix's boot defaults, so the option is a lock rather than a default. A hook in `mnPlayersVSInitVars` (RAM 0x8013AF38) writes the rules into the character select's variables and the VS battle state each time the screen opens, in normal VS only (`VsRemixMenu.vs_mode_flag` 0).
- The stock/time arrow hit tests return FALSE in normal VS.
- `VsRemixMenu.mode_specific_setup_start_` always starts a normal VS match in timed stock mode instead of the rule the VS mode menu saved.
- The code sits in `Item.ScuttleMinion.stage_setting_` (RAM 0x8044F0C0-0x8044F1B7), a Remix function nothing calls.

### `team-battle` (Teams)

The character select says "Team Battle" and team attack is on. The Free-for-all/Team Battle button does nothing, so the room can't switch back to free-for-all. Players pick team colors on their panels as usual; the first time the screen opens, P1 and P2 are red and P3 and P4 blue, so a 1v1 needs one player to change color. Remix's own modes keep their team rules and the button works there.

Source: [`src/smash-remix-2.0.1-team-battle.asm`](src/smash-remix-2.0.1-team-battle.asm). The id isn't `teams` because ranked-ssb has a `src/smash-remix-2.0.1-teams.asm` for its `teams-p1pN` options.

- A hook in `mnPlayersVSInitVars` (RAM 0x8013AF18) sets the screen's team flag and the VS battle state's team battle and team attack each time the screen opens, in normal VS only.
- The button's hit test (`mnPlayersVSCheckGameModeInRange`) returns FALSE in normal VS and goes on to Remix's routine in its modes.
- The code sits in `Hazards.peach_castle_II_setup_` (RAM 0x804906B0-0x8049074B), which nothing references.

### `skip-to-css` (Skip to character select)

After the N64 logo the game goes straight to the VS character select, with no splash, title or menus. Holding B there no longer backs out to the VS mode menu; the Back button at the top right still does.

Source: [`src/smash-remix-2.0.1-skip-to-css.asm`](src/smash-remix-2.0.1-skip-to-css.asm).

- The N64 logo's next scene (RAM 0x80131C94) is 0x10, the VS character select, instead of Remix's splash (0x37).
- `VsRemixMenu.global_game_mode` and `global_stocks` are set to what the skipped VS mode menu would have saved (timed stock, 4 stocks). Without that, the first match would be a time match although the screen shows "STOCK 4".
- `mnPlayersVSDetectBack` (RAM 0x80138140) returns at once.

### `tournament-profile` (Tournament settings)

Remix's settings (hold L on the character select or stage select) start on Remix's own Tournament profile values instead of Community: Hold To Pause on, CSS Panel Menu off, Hold To Exit Training on, the three combo meters off, Salty Runback off, Default CPU level 10, Improved AI off, Music Title at Match Start off, Pokemon Announcer off and Dragon King HUD off. "Load Profile" shows Tournament. They're defaults: anyone can change them during the match.

Source: [`src/smash-remix-2.0.1-tournament-profile.asm`](src/smash-remix-2.0.1-tournament-profile.asm).

- Each change is one toggle's value word (entry + 4) in Remix's `Toggles`. With no save data, netplay's default, Remix starts every toggle at that value.
- The stage toggles the stage options own (Stage Select Layout, Hazard Mode, Random's stage list) and Skip Results Screen are left alone, so "Current Profile" reads Custom.
- A host-picked starting save with Remix settings in it wins over these values.

### `random-vanilla` (Random: original 12)

On Remix's own character select, every random pick is one of the original 12: the "?" portrait, L-button random and new CPU panels. Remix Settings shows "Random Select: VANILLA ONLY", and anyone can set it back there. Players can still pick any character by hand.

Source: [`src/smash-remix-2.0.1-random-vanilla.asm`](src/smash-remix-2.0.1-random-vanilla.asm).

- One word: the Random Select toggle (`Toggles.entry_variant_random` + 4, RAM 0x804620D8) starts on VANILLA ONLY (3) instead of DEFAULT (0).
- With `vanilla-css` on it changes nothing (`vanilla-css` already forces Remix's vanilla random), so it's for rooms with Remix's roster on the screen.

## Smash Remix 2.0.1: presets

| Preset | Options |
|---|---|
| 1v1 Vanilla | `vanilla-css`, `vanilla-sss`, `tournament-rules`, `skip-to-css`, `tournament-profile` |
| 1v1 Remix | `tournament-rules`, `skip-to-css`, `tournament-profile` |
| Teams Vanilla | `vanilla-css`, `vanilla-sss`, `tournament-rules`, `team-battle`, `skip-to-css`, `tournament-profile` |
| Teams Remix | `tournament-rules`, `team-battle`, `skip-to-css`, `tournament-profile` |

- All four are competitive formats, so all four have the tournament rules (4 stocks, 8 minutes, no items), start at the character select, and use Remix's Tournament settings.
- The Remix presets leave the roster and the stage select as Remix has them. `team-battle` is only in the Teams presets; in the 1v1 presets the Free-for-all/Team Battle button stays free.
- `j-variants`, the other two stage options and `random-vanilla` are left for custom sets.

## How the emulator reads packs

- It loads every `*.json` here (the installed `Data/GameOptions`), then the user's `GameOptions` folder in the config directory (`Config/GameOptions` in a portable install). A user pack with the same `id` as a shipped one replaces it.
- A pack needs `format` 1, an `id` of `[a-z0-9._-]` (up to 64), at least one `romMd5` (32 hex digits, any case) and at least one usable option. Unknown fields are ignored. A pack or option that breaks a rule is skipped and logged ("Game options: ..." in the debug log); the rest still load.
- Option ids are `[a-z0-9-]`, up to 32. Labels must be UTF-8 without control characters, up to 256 bytes. If two packs offer the same option id for a ROM, the first pack (shipped ones before the user's, each by file name) wins. The same goes for group and preset ids.
- `offset` can also be a plain JSON number (decimal). Hex in `expect`/`bytes` may be lowercase and may have spaces between bytes.
- One option has at most 256 patches and 64 KiB of patch bytes, and its patches must not overlap. A match carries at most 16 options and 64 KiB of patch bytes in all; options whose patches overlap can't be on together, and are treated as conflicting when the pack doesn't say so (logged).
- Clients from before presets take at most 8 options. A room with more than 8 on can't start, or take a late joiner, while one of them is seated, and a lobby server from before presets takes at most 8.
- `group` naming a group the pack doesn't list is ignored (logged). An id in `requires` or `conflicts` that names no option in the pack, or the option itself, is ignored (logged). An option that requires one that was skipped, or that would need two conflicting options on together, is skipped (logged).
- A preset is skipped (logged) when it names an option the pack doesn't have or skipped, turns on no options, or names a set that can't be on together: more than 16, more than 64 KiB, two that conflict or overlap, or one without an option it requires. A preset with another one's id, or with the same options as an earlier one, is skipped too.
- When a match starts, the host's client checks the options again: one without an option it requires, two that conflict, or two that overlap keep the match from starting with that set.
- An option that changes ROM 0x1000-0x101000 must also patch all of the header CRCs (0x10-0x17), or it's skipped.
- The host checks every `expect` against its ROM file when the match starts. Players, late joiners, live viewers and replays only get the offsets and bytes (the manifest and the `.krec` carry them), so they don't need the pack.
