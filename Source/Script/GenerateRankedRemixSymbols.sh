#!/usr/bin/env bash
#
# Generates the Smash Remix ranked module's symbol file for one ROM build.
#
# Remix code moves with every build, so the ranked module resolves its
# addresses from bass' symbol log (patch.bat: `bass ... -sym logfile.log`).
# The log must come from the exact build that produced the ROM.
#
# Output: <output directory>/<ROM MD5>.sym, for example
#   Bin/Release/Ranked/SmashRemix/<MD5>.sym
#
set -e

if [[ $# -ne 3 ]]; then
    echo "$0 <rom.z64> <logfile.log> <output directory>"
    exit 1
fi

rom="$1"
logfile="$2"
output_dir="$3"

magic="$(head -c 4 "$rom" | od -An -tx1 | tr -d ' \n')"
case "$magic" in
    80371240) md5="$(md5sum "$rom" | cut -d' ' -f1)" ;;
    37804012) md5="$(dd if="$rom" conv=swab status=none | md5sum | cut -d' ' -f1)" ;;
    *) echo "$rom: unsupported ROM byte order ($magic), use a .z64 or .v64 file"; exit 1 ;;
esac
md5="${md5^^}"

mkdir -p "$output_dir"
output="$output_dir/$md5.sym"

{
    echo "# Smash Remix ranked symbols"
    echo "# rom: $(basename "$rom")"
    echo "# log: $(basename "$logfile")"
    echo "# generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    # top level labels of the scopes the module uses (no ._sub labels)
    grep -E '^[0-9a-fA-F]{8} (Toggles|VsRemixMenu|GameEnd|InputDelay|StockMode|CharacterSelect|Stages)\.[A-Za-z0-9_]+$' "$logfile" \
        | tr -d '\r'
} > "$output"

count="$(grep -vc '^#' "$output")"
echo "wrote $count symbols to $output"
