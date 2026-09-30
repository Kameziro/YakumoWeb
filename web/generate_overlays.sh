#!/usr/bin/env bash
# Recompiles the game's 355 code overlays to C++ (profiles/mhp3rd/overlays/),
# without building the native overlay libraries: the web build links the
# sources into the program. Run in the container, with the disc image mounted:
#
#   YAKUMO_ISO=/path/to/your.iso web/docker/run.sh web/generate_overlays.sh [native_build_dir] [jobs]
#
# native_build_dir (default out/linux) must hold psp_recomp, which
# generate.sh builds. Overlays already recompiled are skipped.
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
profile_dir="$repo_dir/profiles/mhp3rd"
build_dir="${1:-$repo_dir/out/linux}"
jobs="${2:-$(nproc)}"
iso="${YAKUMO_ISO_PATH:-/iso/game.iso}"
extract_dir="$profile_dir/analysis/overlays"

if [[ ! -e "$iso" ]]; then
    echo "error: $iso not found; mount the disc image (YAKUMO_ISO=... web/docker/run.sh)" >&2
    exit 1
fi
mkdir -p "$extract_dir"
echo "extracting overlays from DATA.BIN into $extract_dir"
python3 "$profile_dir/tools/databin.py" "$iso" extract-overlays "$extract_dir" > /dev/null

recompile() {
    local image="$1" stem rest base name
    stem="$(basename "$image" .bin)"  # overlay_<BASE>_<name>
    rest="${stem#overlay_}"
    base="${rest%%_*}"
    name="${rest#*_}"
    if compgen -G "$PROFILE_DIR/overlays/ovl${base}_${name}_*/meta.txt" > /dev/null; then return 0; fi
    if ! python3 "$PROFILE_DIR/tools/add_overlay.py" --no-build "$BUILD_DIR" "$image" "0x$base" > /dev/null 2>&1; then
        echo "failed: $stem"
    fi
}
export -f recompile
export PROFILE_DIR="$profile_dir" BUILD_DIR="$build_dir"

total=$(ls "$extract_dir"/overlay_*.bin | wc -l)
echo "recompiling $total overlays with $jobs jobs"
failed=$(ls "$extract_dir"/overlay_*.bin | xargs -P "$jobs" -I{} bash -c 'recompile "$1"' _ {})
done=$(ls -d "$profile_dir"/overlays/*/ 2>/dev/null | wc -l)
echo "recompiled: $done corpora in $profile_dir/overlays"
if [[ -n "$failed" ]]; then
    echo "$failed"
    exit 1
fi
