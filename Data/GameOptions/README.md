# Game options packs

A game options pack is a JSON file in this folder that offers a room's host extra options for one game. Each option is a set of ROM patches. When the host turns an option on, every client applies its patches to the ROM image in memory at match start, before the game boots, and the replay records them. Packs are matched by the MD5 of the unpatched ROM.

A pack can also name presets: sets of its options the host picks in one go from the Game settings list in Create Room or in the room ("Vanilla Tournament"). The host can still turn options on and off one by one with the Customize... button beside the list, where they're listed under the pack's groups, and save a set there under a name of their own. A set that's none of the list's entries shows in the list as "Custom (n)" while it's the one picked. Saved sets are kept on the host's PC per ROM and listed after the presets; a room on one carries it as its preset with the id `user-<8 hex digits>` and the host's name for it, so preset ids starting with `user-` are reserved for those.

An option made from a stage list (Smash Remix's "Stage list") works the same way one level down: in the Customize... view it's a list of named stage lists (Off, the pack's preset lists such as "Dream Land only", the host's saved lists, and "Custom (n stages)" while that's the one picked) with a Customize... button that opens the stage list editor. A room carries the list's name next to its code, so other players see "Dream Land only" or the host's own name for a saved list.

An option can be hidden: it's never listed, counted or named, but a match that has it is played with it. That's for rules other rules turn on (a preset with more players, an old option id, a ranked lobby), not for the host to pick.

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
      "id": "vanilla-tournament",
      "label": "Vanilla Tournament",
      "description": "...",
      "translations": { "ja": { "label": "...", "description": "..." } },
      "options": ["vanilla-css", "tournament-rules"],
      "withPlayers": [
        { "minPlayers": 4, "options": ["teams-default"], "description": "starts on Team Battle (players can still change it)",
          "translations": { "ja": { "description": "..." } } }
      ],
      "aliases": ["1v1-vanilla"]
    },
    {
      "id": "dream-land-ranked",
      "label": "...",
      "options": ["stage-list", "skip-sss"],
      "stageLists": { "stage-list": "dream-land" }
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
      "hidden": false,
      "aliases": ["vanilla-only"],
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
- `hidden`: optional (`true` or `false`, default `false`). A hidden option never shows in the Custom view, its "n of 16 on" count, "Custom (n)", the room's tag or anyone's read-only view, but it's applied and enforced like any other whenever a match has it. See "Hidden options" below for who turns them on and off.
- `aliases`: optional. Ids the option stood in for before, so a choice remembered on a player's PC (a saved set, the last room's settings) that names one gets this option instead. An entry is the old id as a string, or `{ "id": ..., "stageList": ..., "with": [...] }`: `stageList` names one of the option's preset lists (required for an option with a `stageList` block, which needs a list; not allowed on others), and `with` names other options the old id turns on too. A room's option ids are never read as aliases: a room from an older client shows that client's label for an id this pack no longer has.
- `saveData`: optional (`"saveData": "blank"`). `"blank"` says the option only works as meant without save data, say because the game reads the settings it changes from its save. While an option with it is on, the room's matches start from a blank save, even when the host picked one with Sync save data: the host's save picker is locked with the reason, and the pick comes back once no such option is on. The host's client applies the rule when the match starts, so it holds for the match whatever the room shows. Prefer fixing the patch so it doesn't depend on save data, where that's cheap.
- `groups`: optional, in display order. Each has an `id` (`[a-z0-9-]`, up to 32) and a `label`.
- `presets`: optional, in display order. Each has an `id` (`[a-z0-9-]`, up to 32), a `label` (keep it to 48 characters: the lobby server shows no more), a `description` and the `options` it turns on (at least one; the unchanged game is always offered as Default). Turning on exactly a preset's options shows that preset as the room's choice. Don't start an id with `user-`: clients read that as a host's saved set.
- `withPlayers` (on a preset): optional. Options the preset also turns on when a match starts with at least `minPlayers` players (2, 3 or 4), each entry naming options that aren't in its `options` or another entry. Every seated player counts, including players on the same PC as another; spectators don't. The room keeps showing the preset (its name, and its `options` in the Custom view, with a line saying what comes on with more players); the host's client adds the options when the match starts, and they're in that match's manifest and replay. A late joiner joins the match as it started, so options don't change mid-match: a match started with 3 players stays without the 4-player options when a fourth joins it. The preset is skipped (logged) when any player count's set can't be on together. An entry's optional `description` (translatable, under `translations` like a label) ends the Custom view's line about it, "With 4 players in the match, <preset> ..."; without one the line names the options it turns on, and an entry of hidden options without one gets no line. Entries can't name an option with a `stageList` block.
- `aliases` (on a preset): optional. Ids the preset had before. A host's remembered choice of one of them (the last room's game settings) is this preset now. A room's preset id is never read as an alias: a room from an older client on an old id shows that client's label for it.
- `stageLists` (on a preset): optional, `{ "<option id>": "<preset list id>" }`. A preset can turn on an option with a `stageList` block only with one of that option's preset lists named here (say, Dream Land only with Skip stage select for a ranked format); every option named here must be in its `options`. The list is part of the preset: the same options with another list are another preset, or custom. None of the Smash Remix pack's presets does this yet.
- `stageList` (on an option): optional. The option is made from a list of stages the host picks in the stage list editor; see "Stage lists" below.

### Stage lists (`stageList`)

An option with a `stageList` block has its fixed `patches` (the same for every list) and three ranges whose bytes come from the host's list. The host picks 1 to `maxStages` stages, each once, in the order the stage select shows them, and for each stage whether its hazards and its movement are on. The list travels as a short text code (its share code), and the host's client turns it into patches when the match starts.

```json
"stageList": {
  "codePrefix": "STG",
  "codeSalt": "smash-remix-2.0.1",
  "maxStages": 85,
  "table": { "offset": "0x02C568E0", "expect": "...", "pageIcons": 18, "pageStages": 17, "pageColumns": 6, "random": "0xDE" },
  "pages": { "offset": "0x02C56704", "expect": "00000003" },
  "modes": { "offset": "0x02C5B1C0", "expect": "..." },
  "icons": { "fileTable": "0x001AC870", "files": 5455, "file": "0x0A04", "sprites": "0x02C56C9C", "randomSprite": "0x02C56FFC", "fileBases": "0x3CD08" },
  "groups": [ { "id": "page1", "label": "Vanilla and Smash stages", "translations": { "ja": { "label": "..." } } } ],
  "variantKinds": { "dl": { "label": "Dream Land layout", "translations": { "ja": { "label": "..." } } } },
  "presets": [ { "id": "dream-land", "label": "Dream Land only", "description": "...", "stages": [["0x06", 0]] } ],
  "stages": [
    { "id": "0x00", "name": "Peach's Castle", "group": "page1", "hazards": true, "movement": true },
    { "id": "0x6B", "name": "Peach's Castle DL", "variantOf": "0x00", "short": "DL", "kind": "dl", "hazards": true }
  ]
}
```

- `table`: the stage select's stage ids, `pageIcons` per page for as many pages as `expect` holds. Each page gets the next `pageStages` stages of the list on its first icons, and the `random` id on its other icons and wherever the list has run out. `pageColumns` is how many icons a row has (the editor's preview).
- `pages`: the page count, big-endian, as many bytes as `expect` has: the pages the list needs.
- `modes`: a byte per stage id. A listed stage's byte is its mode (bit 0 hazards off, bit 1 movement off); every other byte keeps the unpatched value.
- `expect` on the three ranges is the unpatched ROM's bytes there, checked like any patch's. The ranges count toward the option's patches and bytes, and two options whose ranges or patches overlap are treated as conflicting, as usual.
- `maxStages`: at most `pageStages` times the pages, and at most 127.
- `stages`: every stage a list can have, in the order the editor lists them. `name` is shown as is. A base stage names its `group` (one of `groups`: the editor's headings, in order; for Remix they're the pages of its own stage select). A variant names its base stage in `variantOf` and is shown on its base stage's card as `short`; `kind` names one of `variantKinds`, whose label explains it. `hazards` and `movement` say what the stage has: the editor offers those switches only, and a mode is reduced to what the stage has.
- `presets`: optional named lists, `[stage id, mode]` pairs, in the order the host's list of lists shows them (after Off, before the host's saved lists). Ids are `[a-z0-9-]` up to 32 and can't start with `user-`; labels follow the presets' rules (translatable, up to 48 characters). One that breaks the rules, has the id of an earlier one or the same list, is left out (logged). A list with the same stages and settings as a preset list is that preset list wherever it shows, whoever made it, so a saved list can't be one too.
- `icons`: optional. Where the game keeps the stage select's icons, so the editor can show them: nothing is shipped, they're read from the player's own ROM when the editor opens (names only when it can't be read). The reader handles the original game's file table (vpk0-compressed files, relocation chains) and 16-bit RGBA sprites; `sprites` and `randomSprite` are ROM offsets of `(file index << 24) | offset` words, as Smash Remix's `Stages.icon_offset_table` has them.
- Share code: `codePrefix` + `-` and Crockford base32 in groups of 4 (`STG-220C-6YA8-2T5V-W0`). Bits, most significant first: 4 bits version (1), 7 bits count, then 8 bits stage id and 2 bits mode per stage, then a CRC-16/CCITT-FALSE over `codeSalt` and the bits before it (zero-padded to a byte), then zero bits to a whole character. Reading one ignores case, spaces and hyphens, takes O as 0 and I or L as 1, and the prefix may be left out. A code for another `codeSalt` (another game or version) fails its checksum. Codes are canonical: the same list always gives the same code.
- In a room the list goes as the option's `param` (its code; lobby servers from before params drop it, and clients then don't offer the option there). At match start the host's client checks the code and makes the patches. The manifest and the `.krec` carry the patches like any option's, so players, late joiners, live viewers and replays don't need the pack, and older clients that take game options play the match too (they show the option by its label).
- Next to the code goes the list's name, as `paramPreset` (`{ "id", "label" }`, the same shape and rules as a room's preset): a preset list's id with its English label, or `user-` and a saved list's id with the host's name for it. Other clients show their own pack's label for a preset list's id, else the label; a list without a name shows how many stages it has, or a preset list's name when its stages are one. The lobby filters the name like a preset's, and a host whose saved list's name it refuses sends the list without one. Lobby servers from before names drop it.
- Saved lists are kept per ROM on the player's PC (up to 30, names as for saved sets). A guest who saves the host's list from the read-only view starts with the host's name for it.

### Hidden options

`hidden` options are rules a host doesn't pick one by one. They come on in three ways, and the rule for each is about who owns them:

- A preset names one (in `options` or `withPlayers`), or the lobby sends it (ranked): picking a preset, a saved set or Default in the Game settings list replaces the whole set, hidden options included, so a preset brings exactly its own.
- An alias turns one on (`with`) when a remembered set is read.
- A saved set has one because the set it was saved from did.

Editing options one by one in the Custom view never drops them: the view changes the options it shows and keeps the hidden ones as they are, unless an option the host turns on conflicts with one (it goes off, like any conflict) or needs one (it comes on). Save as keeps them with the set, for the host and for a guest saving the room's set. They still count toward the 16-option limit (the count reads "n of 15" with one on). `withPlayers` options aren't part of the room's set at all: the host's client adds them when the match starts.

Keep patches out of ROM 0x1000-0x101000: the boot code (CIC) checksums that range, so editing it means fixing the header CRCs too.

Options that can be on together must change different bytes. Two options whose patches overlap have to be listed in `conflicts` (on either side).

## Smash Remix 2.0.1: `vanilla-css`

It was called "Vanilla only" (`vanilla-only`) before presets. The patches are the same, and the option's `aliases` make a remembered `vanilla-only` this option.

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

Each option below has its source in `src/`, made and checked the same way as `vanilla-css`: assemble the `.asm` over a copy of the ROM with bass v14, without `-create`, and diff the copy against the original. Code in Remix's own space uses ROM offset = RAM address - 0x7D800000, and code in the VS character select overlay ROM = RAM - 0x80001D80, as above.

Any of them can be on together, except that the two stage options (`vanilla-sss`, `stage-list`) conflict, `teams-default` and `team-battle` conflict, and `j-variants` needs `vanilla-css`. All the state they add lives in emulated RAM. A host-picked starting save loads Remix's settings over the options' defaults at boot, so the options that set Remix settings set them again right after that (`random-vanilla`, `j-gameplay`), and the stage options keep the save's Stage Settings from loading at all. `tournament-profile` is meant to be Remix's whole Tournament profile, so it needs a blank save (`"saveData": "blank"`).

Two options of the first presets release are gone: `stages-dream-land` (Dream Land only, without a stage select) and `stages-competitive` (Competitive stages). They're `stage-list`'s preset lists now, and `stage-list`'s `aliases` turn a remembered one into the Stage list on that list, with `skip-sss` for Dream Land only, since that option skipped the stage select. Their generated sources went with them.

Before that, a tester saw Dream Land only give the save's tournament stage list, and Tournament settings not apply, with a save picked. Remix loads its settings toggles (the settings profile, the stage lists, Random's pick) from a save of its own revision once per boot, overwriting every toggle's value, so a patch that only changed a toggle's starting value lost to the save.

### `j-variants` (J variants)

Needs `vanilla-css`. Players pick among the original 12 on the original character select screen, and every fighter is the Japanese-version variant Remix ships, on the select screen too: the panel shows the J model, its name ("DONKEY KONG", "PURIN"), the announcer's J call and Remix's Japanese flag once picked, as Remix's own select screen does for a J variant. The variants are J Mario, J Fox, J DK, J Samus, J Luigi, J Link, J Yoshi, J Falcon, J Kirby, J Pikachu, Purin and J Ness, for humans, CPUs and random picks; the battle, stock icons and results show them too ("PURIN WIN!").

Source: [`src/smash-remix-2.0.1-j-variants.asm`](src/smash-remix-2.0.1-j-variants.asm).

- The select screen keeps the original ids; the routines that draw a panel (model, name and logo, announcer, picked pose, Remix's regional flag) are given the J id. The code sits in `mnPlayersVSCheckFighterLocked`'s body (RAM 0x8013204C), unreachable with `vanilla-css` on, and a never-called Remix debug routine (0x80526CD4).
- On the original panels Remix's J flag sat on the cursor and the name, so it's drawn at 3/4 size in the panel's lower-left corner, the one spot no panel element or standing fighter covers (`CharacterSelect.update_variant_indicators_`, RAM 0x80522220, VS screen only; Training, 1P and Bonus keep Remix's place and size).
- When the VS character select hands over to the stage select or the battle, the four fighter ids it wrote to the VS battle state, and Tag Team's queues, are swapped for the J ids.
- When the VS character select opens again, they're swapped back before anything reads them, so a rematch shows the same picks on the original grid.
- 12-Character Battle keeps the original fighters. Training, 1P and Bonus aren't changed.
- The battle-state swap and the id table sit in the rest of `mnPlayersVSMakePortraitShadow` after `vanilla-css`'s dispatchers (RAM 0x801323FC-0x80132507), which is unreachable once no fighter is locked.

### `vanilla-sss` (Vanilla SSS)

The VS stage select screen is the original game's: 10 big icons in two rows (Peach's Castle, Congo Jungle, Hyrule Castle, Planet Zebes, Mushroom Kingdom, Yoshi's Island, Dream Land, Sector Z, Saffron City and Random), the original cursor, the yellow name plate and the series emblem. Only those 9 stages can be played; Random, and a match with stage select off, pick among them. Hazards and stage movement are on. Remix's extras on that screen go with its grid: R/Z pages, the hazard and layout toggles, the Random roulette and the hold-L settings shortcut (it still works on the character select screen).

Source: [`src/smash-remix-2.0.1-vanilla-sss.asm`](src/smash-remix-2.0.1-vanilla-sss.asm), which includes the Random list [`src/smash-remix-2.0.1-vanilla-sss-random.asm`](src/smash-remix-2.0.1-vanilla-sss-random.asm), generated by [`src/smash-remix-2.0.1-gen-vanilla-sss-random.py`](src/smash-remix-2.0.1-gen-vanilla-sss-random.py) (`python smash-remix-2.0.1-gen-vanilla-sss-random.py Smash_Remix_2.0.1.z64 <out dir>`) from Random's stage list in the ROM.

- The stage select's code (mnMaps, scene 0x15) is loaded from ROM 0x14D670 to RAM 0x80131B00, so ROM = RAM - 0x7FFE4490 there. The original drawing, cursor and cursor movement that Remix patched out are put back, with the original words taken from the ROM Remix is built on.
- `Stages.get_stage_id_` answers with the stage of the original slot under the cursor, so the Remix hooks that stay (the preview, Random's preview, the pick) work on the original slots. `Stages.setup_` draws nothing.
- Random's stage toggles are on for the 9 stages and off for every other one.
- Mushroom Kingdom is never locked, so the screen doesn't depend on save data.
- A 6-word guard ([`src/smash-remix-2.0.1-stage-save-guard.asm`](src/smash-remix-2.0.1-stage-save-guard.asm)) keeps a starting save's Stage Settings from loading (`Toggles.load_`'s call at RAM 0x804604BC), so the list, layout and hazard mode hold with any save.

### `tournament-rules` (Tournament rules)

Every normal VS match is 4 stocks, 8 minutes (Remix's timed stock rule), no items, no handicap and 100% damage. On the character select the stock/time arrows do nothing; the stock/time picker still flips to show the time limit. A change made in the VS mode menu or VS options is undone the next time the character select opens. Remix's own modes (12-Character Battle, Tag Team, King of the Hill, Smashketball, Tug of War) keep their own rules.

Source: [`src/smash-remix-2.0.1-tournament-rules.asm`](src/smash-remix-2.0.1-tournament-rules.asm).

- These are Remix's boot defaults, so the option is a lock rather than a default. A hook in `mnPlayersVSInitVars` (RAM 0x8013AF38) writes the rules into the character select's variables and the VS battle state each time the screen opens, in normal VS only (`VsRemixMenu.vs_mode_flag` 0).
- The stock/time arrow hit tests return FALSE in normal VS.
- `VsRemixMenu.mode_specific_setup_start_` always starts a normal VS match in timed stock mode instead of the rule the VS mode menu saved.
- The code sits in `Item.ScuttleMinion.stage_setting_` (RAM 0x8044F0C0-0x8044F1B7), a Remix function nothing calls.

### `team-battle` (Teams, hidden)

Hidden: it's for rules that force teams, such as a ranked lobby's 2v2. The presets' "with 4 players" rule uses `teams-default` instead, which players can change. A set saved with "Teams" on before it was hidden still has it (and still forces teams).

The character select says "Team Battle" and team attack is on. The Free-for-all/Team Battle button does nothing, so the room can't switch back to free-for-all. Players pick team colors on their panels as usual; the first time the screen opens, P1 and P2 are red and P3 and P4 blue, so a 1v1 needs one player to change color. Remix's own modes keep their team rules and the button works there.

Source: [`src/smash-remix-2.0.1-team-battle.asm`](src/smash-remix-2.0.1-team-battle.asm). The id isn't `teams` because ranked-ssb has a `src/smash-remix-2.0.1-teams.asm` for its `teams-p1pN` options.

- A hook in `mnPlayersVSInitVars` (RAM 0x8013AF18) sets the screen's team flag and the VS battle state's team battle and team attack each time the screen opens, in normal VS only.
- The button's hit test (`mnPlayersVSCheckGameModeInRange`) returns FALSE in normal VS and goes on to Remix's routine in its modes.
- The code sits in `Hazards.peach_castle_II_setup_` (RAM 0x804906B0-0x8049074B), which nothing references.

### `teams-default` (Teams by default, hidden)

Hidden: the presets turn it on for a match that starts with 4 players. The first character select screen of the session is in Team Battle ("Team Battle" at the top left, P1 and P2 red, P3 and P4 blue, team attack on), and the Free-for-all/Team Battle button works: whatever the players pick stays, after a match, on every rematch and through the VS mode menu. It conflicts with `team-battle`, which would force Team Battle every time the screen opens.

Source: [`src/smash-remix-2.0.1-teams-default.asm`](src/smash-remix-2.0.1-teams-default.asm).

- Remix's `Settings.set_vs_settings_` runs once per boot, right after the game copies its default VS battle state, and writes Remix's VS defaults. Its last block now also stores `is_team_battle = TRUE` (RAM 0x800A4D0A); two words at RAM 0x8046F400. From there the game's own handling of the team flag takes over (the character select reads it when it opens and writes it back when it's left; the VS mode menu keeps it).
- The team flag isn't in save data, so a starting save can't change it.

### `skip-to-css` (Skip to character select)

After the N64 logo the game goes straight to the VS character select, with no splash, title or menus. Holding B there no longer backs out to the VS mode menu; the Back button at the top right still does.

Source: [`src/smash-remix-2.0.1-skip-to-css.asm`](src/smash-remix-2.0.1-skip-to-css.asm).

- The N64 logo's next scene (RAM 0x80131C94) is 0x10, the VS character select, instead of Remix's splash (0x37).
- `VsRemixMenu.global_game_mode` and `global_stocks` are set to what the skipped VS mode menu would have saved (timed stock, 4 stocks). Without that, the first match would be a time match although the screen shows "STOCK 4".
- `mnPlayersVSDetectBack` (RAM 0x80138140) returns at once.

### `skip-sss` (Skip stage select)

From the character select, Start goes straight to the battle, also on every rematch; the stage select never shows. The stage is drawn at random from the stages in play: with `stage-list`, among the list's stages, each with its hazard and movement setting (a one-stage list always gives that stage); with `vanilla-sss`, among the original 9; with neither, Remix's own Random list. As with Stage Select off in the original game, the stage just played isn't drawn for the next match unless it's the only one. The VS options' "Stage Select" row stays Off. It applies to every VS mode that uses the VS character select; Training, 1P and Bonus have their own screens.

Source: [`src/smash-remix-2.0.1-skip-sss.asm`](src/smash-remix-2.0.1-skip-sss.asm).

- `mnPlayersVSFuncRun` (RAM 0x8013A9E0) picks scene 0x15 (the stage select) or 0x16 (the battle, with Remix's random stage pick) by `is_stage_select`; the load becomes a 0, so it's always the battle.
- Before Remix's stage pick, the same routine points `gSCManagerBattleState` at the VS battle state, as the battle does when it starts (RAM 0x8013AA00, over the original game's dead stage count). Remix's sound replacement treats scene 0x16 as "in a match" and reads the stage through that pointer, which is NULL until the first battle of the boot: on the direct character select -> battle path a sound in that frame stopped the game (seen with a starting save whose Random Select picked Remix characters, and on the unpatched ROM with Remix's own Stage Select Off).
- The VS options (overlay at RAM 0x80131B00, ROM 0x1252E0) open the row as Off, and its left/right and A handlers store Off.
- The old `stages-dream-land` cleared `is_stage_select` once at boot, so players could turn the stage select back on; `skip-sss` shares no byte with it.

### `tournament-profile` (Tournament settings)

Remix's settings (hold L on the character select or stage select) start on Remix's own Tournament profile values instead of Community: Hold To Pause on, CSS Panel Menu off, Hold To Exit Training on, the three combo meters off, Salty Runback off, Default CPU level 10, Improved AI off, Music Title at Match Start off, Pokemon Announcer off and Dragon King HUD off. "Load Profile" shows Tournament. They're defaults: anyone can change them during the match.

Source: [`src/smash-remix-2.0.1-tournament-profile.asm`](src/smash-remix-2.0.1-tournament-profile.asm).

- Each change is one toggle's value word (entry + 4) in Remix's `Toggles`. With no save data, netplay's default, Remix starts every toggle at that value.
- The stage toggles the stage options own (Stage Select Layout, Hazard Mode, Random's stage list) and Skip Results Screen are left alone, so "Current Profile" reads Custom.
- It needs a blank save (`"saveData": "blank"`): while it's on, the room plays with a blank save. A save with Remix settings in it would win over these values, and for "Tournament settings" to mean Remix's Tournament profile, every other Remix setting a save can change (Tripping, Game Speed and so on) has to start from Remix's defaults too.

### `random-vanilla` (Random: original 12)

On Remix's own character select, every random pick is one of the original 12: the "?" portrait, L-button random and new CPU panels. Remix Settings shows "Random Select: VANILLA ONLY", and anyone can set it back there. Players can still pick any character by hand.

Source: [`src/smash-remix-2.0.1-random-vanilla.asm`](src/smash-remix-2.0.1-random-vanilla.asm).

- One word: the Random Select toggle (`Toggles.entry_variant_random` + 4, RAM 0x804620D8) starts on VANILLA ONLY (3) instead of DEFAULT (0).
- With `vanilla-css` on it changes nothing (`vanilla-css` already forces Remix's vanilla random), so it's for rooms with Remix's roster on the screen.
- It holds with a starting save too: `Boot.load_` sets the toggle again after Remix loads the save (RAM 0x8046F394, code at 0x804FBEC4).

### `j-gameplay` (J gameplay)

Remix's settings that its Japanese profile changes start on the Japanese values: Hitlag, DI, Japanese Sounds (ALWAYS: every fighter's hits use the Japanese sounds), Japanese Stun/Sleep, Momentum Slide and Shield Stun on the Gameplay page, Whispy Mode on Stage Settings and Pokemon SFX on Pokemon Settings. They're defaults: anyone can change them in Remix's settings during the match. It stacks with `j-variants` or works alone, for rooms with Remix's roster.

Source: [`src/smash-remix-2.0.1-j-gameplay.asm`](src/smash-remix-2.0.1-j-gameplay.asm).

- Remix has no single "J gameplay" toggle. Its Japanese profile (`Toggles.profile_defaults_JP`) differs from the default Community profile in 9 of its 531 toggles; this sets 8 of them. The Pokemon Announcer, which the Japanese profile turns off, belongs to `tournament-profile` (it turns it off too, and options that can be on together can't change the same byte). Hitstun stays NORMAL, as in the Japanese profile.
- Each toggle's value word is patched, and `Boot.load_`'s `jal Toggles.load_` (RAM 0x8046F384, the save path only) goes through a small routine that loads the save and then sets the eight values again, once per boot, so a starting save's other settings still load (music, player tags and so on). The routine sits in a never-called Remix debug routine (`CharacterSelectDebugMenu.DpadControl.force_settings_for_training_`, RAM 0x80527204).

### `stage-list` (Stage list)

The stage select shows only the stages on the list, in its order: Remix's tournament-style grid, 17 stages per page with Random on the last icon of every page and on unused icons, up to 5 pages (R/Z switch pages). Hovering a stage shows "Hazards: On/Off" and "Movement: On/Off" as the list sets them for it, and L and C-down don't change them. Random, the roulette and a match with stage select off pick only among the listed stages, each with its setting. Rematches come back to the same list. A stage can be on the list once; Remix's variants (DL, Ω, Remix's versions) are stages of their own, so a second Dream Land is a variant like Winter Dream Land or Dream Land Ω. With `skip-sss` on, the battle starts on one of the listed stages without the stage select.

In the Custom view it's a list rather than a switch: Off, the three preset lists below, the host's saved lists, and "Custom (n stages)" while the host's own unsaved list is the one picked. Customize... beside it opens the stage list editor on the list picked (on the one it had last in the view when it's off): the stages as cards with the stage select's own icons, read from the host's ROM, under Remix's page headings, with search and filters for stages with hazards, movement or neither; variants as chips on their base stage's card. The list shows the stages in match order with the page breaks and a preview of each page; each stage has a Hazards and a Movement switch when it has them. "Start from a list" offers the preset lists and the saved ones; a list can be saved under a name (per game, up to 30; not a preset list's stages or name, nor a list saved already), renamed and deleted, and shared as a code (Copy code / Paste code). Guests see the list by its name (the host's name for a saved one), read-only, and can copy its code or save it, with the host's name filled in. A saved set of game settings with the option on keeps its list.

| Preset list | Stages |
|---|---|
| Dream Land only (`dream-land`) | Dream Land, hazards on |
| Competitive stages (`competitive`) | Dream Land, Goomba Road, Pokemon Stadium 2 and Pokemon Stadium, hazards on |
| Dream Land variants (`dream-land-variants`) | Remix's own tournament page of Dream Land clones in its order (Peach's Castle DL, Congo Jungle DL, Hyrule Castle DL, Planet Zebes DL, Mushroom Kingdom DL, Duel Zone DL, Yoshi's Island DL, Dream Land, Sector Z DL, Saffron City DL, Final Destination DL, Meta Crystal DL, Mute City DL, Winter Dream Land, Deku Tree DL, Crateria DL, Battlefield DL), with that page's settings (hazards and movement off), then Dream Land Beta DL and Cool Cool Mountain DL: 19 stages on 2 pages. Fray's Stage - Night, which Remix files as a DL variant, isn't a Dream Land layout and is left out. |

The first two were the `stages-dream-land` and `stages-competitive` options; the option's `aliases` take a remembered one to its list (`stages-dream-land` with `skip-sss`).

Source: [`src/smash-remix-2.0.1-stage-list.asm`](src/smash-remix-2.0.1-stage-list.asm) (the fixed part); the list part is made by the client (see "Stage lists").

- Remix's "Stage Select Layout" toggle picks its normal layout or its tournament layout (`Stages.stage_table_tournament`, 5 pages of 18 stage ids). The option fixes the layout to tournament: the toggle's starting value, and its 12 reads in Remix's Stages code become a constant, so a loaded save or the settings menu can't switch back.
- The list goes into the tournament table (`table`), its page count (`pages`), and the mode of each listed stage into `Stages.tournament_hazard_mode_table` (`modes`), which Remix applies on every hover and every Random pick. The per-page hazard exceptions in `Stages.get_stage_id_` (Dream Land, Yoshi's Island DL, Saffron City DL) are taken out.
- `Stages.add_stage_to_random_list_` adds a stage to Random's list when it's on the stage select's table, instead of by its random toggle and Tournament default, so Random never depends on toggles or save data.
- `Stages.swap_stage_` set the hazard mode after a tournament-layout Random pick but skipped the "don't freeze stage" flag (a Remix bug), so "movement off" on a Random pick followed the stage hovered before; the branch is taken out.
- Nothing it changes reads save data. Not covered: saved Whispy Mode, Saffron Pokemon rate, Camera Mode and Yoshi's Island cloud settings still load from a save.
- Checked headless against the unpatched ROM for 6 lists (1 stage, a full page, 2 pages with every mode, 85 stages, variants, the Competitive stages list), from emulated RAM: every icon of every page, Random draws, stage select off, direct picks and rematch (16/16 checks each), and with a non-default Remix save (17/17).

## Smash Remix 2.0.1: presets

| Preset | Options | With 4 players, also | Old ids (aliases) |
|---|---|---|---|
| Vanilla Tournament (`vanilla-tournament`) | `vanilla-css`, `vanilla-sss`, `tournament-rules`, `skip-to-css`, `tournament-profile` | `teams-default` | `1v1-vanilla`, `teams-vanilla` |
| Remix Tournament (`remix-tournament`) | `tournament-rules`, `skip-to-css`, `tournament-profile` | `teams-default` | `1v1-remix`, `teams-remix` |

- Both are competitive formats, so both have the tournament rules (4 stocks, 8 minutes, no items), start at the character select, and use Remix's Tournament settings. Both play from a blank save (`tournament-profile`).
- There's no separate teams preset: a match that starts with 4 players (players on the same PC count) starts on Team Battle, and players can still switch to free-for-all in the game's menu (`teams-default`, hidden). With 2 or 3, it starts on free-for-all. The Custom view says so under the preset's options.
- The Remix preset leaves the roster and the stage select as Remix has them.
- They replaced four presets (1v1 Vanilla, 1v1 Remix, Teams Vanilla, Teams Remix). A host whose last room was on one of those gets the tournament preset with the same roster.
- `j-variants`, `j-gameplay`, `stage-list`, `skip-sss` and `random-vanilla` are left for custom sets.

## How the emulator reads packs

- It loads every `*.json` here (the installed `Data/GameOptions`), then the user's `GameOptions` folder in the config directory (`Config/GameOptions` in a portable install). A user pack with the same `id` as a shipped one replaces it.
- A pack needs `format` 1, an `id` of `[a-z0-9._-]` (up to 64), at least one `romMd5` (32 hex digits, any case) and at least one usable option. Unknown fields are ignored. A pack or option that breaks a rule is skipped and logged ("Game options: ..." in the debug log); the rest still load.
- Option ids are `[a-z0-9-]`, up to 32. Labels must be UTF-8 without control characters, up to 256 bytes. If two packs offer the same option id for a ROM, the first pack (shipped ones before the user's, each by file name) wins. The same goes for group and preset ids.
- `offset` can also be a plain JSON number (decimal). Hex in `expect`/`bytes` may be lowercase and may have spaces between bytes.
- One option has at most 256 patches and 64 KiB of patch bytes, and its patches must not overlap. A match carries at most 16 options and 64 KiB of patch bytes in all; options whose patches overlap can't be on together, and are treated as conflicting when the pack doesn't say so (logged).
- Clients from before presets take at most 8 options. A room with more than 8 on can't start, or take a late joiner, while one of them is seated, and a lobby server from before presets takes at most 8.
- `group` naming a group the pack doesn't list is ignored (logged). An id in `requires` or `conflicts` that names no option in the pack, or the option itself, is ignored (logged). An option that requires one that was skipped, or that would need two conflicting options on together, is skipped (logged).
- A preset is skipped (logged) when it names an option the pack doesn't have or skipped, turns on no options, or names a set that can't be on together: more than 16, more than 64 KiB, two that conflict or overlap, or one without an option it requires. A preset with another one's id, or with the same options and stage lists as an earlier one, is skipped too. So is one whose `withPlayers` isn't a list of entries with a `minPlayers` of 2, 3 or 4 (each at most once) and at least one usable option not named before (none with a `stageList` block), or makes a set like that for some player count; one that turns on an option with a `stageList` block without naming one of its preset lists in `stageLists`; and one whose `stageLists` isn't an object, names an option it doesn't turn on, or names a list the option doesn't have. Bad `aliases` entries are ignored (logged), and so is a `withPlayers` `description` that isn't a valid label.
- `saveData` with any value but `"blank"` is ignored (logged), and so is `hidden` with anything but `true` or `false`.
- An option's alias is ignored (logged) when it isn't an id (or an object with one), is the option's own id or any option's id in the pack (skipped options too), is another alias's, has a `stageList` on an option without a `stageList` block or none (or one the option's lists don't have) on an option with one, names in `with` the option itself, an option with a `stageList` block or no usable option, or makes a set that can't be on together (one without what it requires, two that conflict). An alias whose id is an option another pack offers for the ROM, or whose `with` names options another pack took, isn't offered either.
- An option whose `stageList` breaks a rule is skipped (logged): a code prefix of 1-8 letters A-Z and a salt of 1-64 printable ASCII characters; the three ranges with offset and `expect`, not overlapping each other, the option's patches or 0x1000-0x101000; `pageIcons` 1-64, `pageStages` 1 to `pageIcons`, whole pages in the table; `maxStages` 1 to what the pages hold (and 127); a page count wide enough; at least one stage, each with a unique id below the `modes` table's length that isn't the `random` id, and a name; every `variantOf` naming a base stage. A group or kind a stage names that isn't listed is ignored; an incomplete `icons` block is ignored (logged: names only). A preset list that breaks the rules, starts with `user-`, or has the id or the stages of an earlier one is left out (logged).
- When a match starts, the host's client checks the options again: one without an option it requires, two that conflict, or two that overlap keep the match from starting with that set.
- An option that changes ROM 0x1000-0x101000 must also patch all of the header CRCs (0x10-0x17), or it's skipped.
- The host checks every `expect` against its ROM file when the match starts. Players, late joiners, live viewers and replays only get the offsets and bytes (the manifest and the `.krec` carry them), so they don't need the pack. That goes for a stage list's patches too, which the host's client makes from the room's list then.
