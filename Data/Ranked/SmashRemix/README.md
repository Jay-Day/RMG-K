# Smash Remix ranked symbols

The Smash Remix ranked module (`Ranked/SmashRemix.dll`) needs a symbol file for every ROM build it supports. The file is named `<ROM MD5>.sym` and lives in `Ranked/SmashRemix/`.

Generate one from the bass symbol log of the same build:

```bash
./Source/Script/GenerateRankedRemixSymbols.sh path/to/ssb64asm.z64 path/to/logfile.log Data/Ranked/SmashRemix
```

`*.sym` files in this directory are installed next to the module.
