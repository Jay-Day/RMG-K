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

Keep patches out of ROM 0x1000-0x101000: the boot code (CIC) checksums that range, so editing it means fixing the header CRCs too.

## Smash Remix 2.0.1: `vanilla-only`

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
- Remix hooked the original screen code wherever it turns a token position into a fighter or a fighter into a portrait: the hit test, the slide-in, token placement, the white flash and the drift of placed tokens. Those hooks take the original code path again, which can only give the 12 original fighters. 12-Character Battle uses the same screen with Remix's own grid, so each hook checks Remix's 12-Character Battle flag first and keeps Remix's behaviour there.
- Tokens are full size again. L-button random and new CPU panels put their token on the original portrait.
- Training, 1P and Bonus keep Remix's select screens, with the lock.

How it was made: Remix's code is assembled at RAM 0x80400000 and copied there unchanged from ROM 0x02C00000 by one boot DMA, so ROM offset = RAM address - 0x7D800000. The original VS select screen's code (overlay 26) is loaded uncompressed from ROM 0x12FD80 to RAM 0x80131B00 whenever that screen opens, so ROM offset = RAM address - 0x80001D80 there. The addresses come from the bass symbol log of a v2.0.1 rebuild (`bass -sym`), from the ssb-decomp-re decompilation of the original game, and from a diff of that overlay between the original ROM and Remix. To check the JSON, copy the ROM and assemble the `.asm` over the copy with Remix's bass v14 (`bass -o copy.z64 smash-remix-2.0.1-vanilla-only.asm`, without `-create`). Then compare the copy against the original ROM.

Porting to a newer Remix: build it with `-sym`, find the same routines, check that the instructions match the `was:` comments in the `.asm`, update the addresses, reassemble, and regenerate `expect`/`bytes` from the diff. Then add a new pack file with the new MD5.

## How the emulator reads packs

- It loads every `*.json` here (the installed `Data/GameOptions`), then the user's `GameOptions` folder in the config directory (`Config/GameOptions` in a portable install). A user pack with the same `id` as a shipped one replaces it.
- A pack needs `format` 1, an `id` of `[a-z0-9._-]` (up to 64), at least one `romMd5` (32 hex digits, any case) and at least one usable option. Unknown fields are ignored. A pack or option that breaks a rule is skipped and logged ("Game options: ..." in the debug log); the rest still load.
- Option ids are `[a-z0-9-]`, up to 32. Labels must be UTF-8 without control characters, up to 256 bytes. If two packs offer the same option id for a ROM, the first pack (shipped ones before the user's, each by file name) wins.
- `offset` can also be a plain JSON number (decimal). Hex in `expect`/`bytes` may be lowercase and may have spaces between bytes.
- One option has at most 256 patches and 64 KiB of patch bytes, and its patches must not overlap. A match carries at most 8 options and 64 KiB of patch bytes in all; options whose patches overlap can't be on together.
- An option that changes ROM 0x1000-0x101000 must also patch all of the header CRCs (0x10-0x17), or it's skipped.
- The host checks every `expect` against its ROM file when the match starts. Players, late joiners, live viewers and replays only get the offsets and bytes (the manifest and the `.krec` carry them), so they don't need the pack.
