#!/usr/bin/env bash
# Turns a dist/ full of built executables into the folder that gets uploaded.
#
#   ./scripts/package-release.sh 0.1
#
# What it produces in release/<version>/ :
#
#   Fluid Solver <ver> <platform> <features>.zip   one per built variant,
#                                                  each holding "Fluid Solver"
#                                                  plus README and LICENSE
#   Fluid Solver <ver> <platform> setup.exe        installers, copied through
#   Fluid-Solver-Source-Code.zip                   the source tree, no build
#                                                  output, no .git, no vendored
#                                                  SFML
#   README.md                                      the README of exactly this
#                                                  version
#   SHA256SUMS.txt                                 so a download can be checked
#
# It never builds anything. Run build-linux.sh / build-windows.ps1 first, and
# collect the artifacts of the other platforms into dist/ before running this.

set -uo pipefail

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    echo "usage: $0 <version>   e.g. $0 0.1" >&2
    exit 2
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO/dist"
OUT="$REPO/release/$VERSION"

[ -d "$DIST" ] || { echo "No $DIST - build something first." >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"

command -v zip >/dev/null 2>&1 || { echo "zip is not installed." >&2; exit 1; }

pack() {   # pack <source path> <archive base name>
    local src="$1" name="$2" stage
    stage="$(mktemp -d)"
    mkdir -p "$stage/$name"

    if [ -d "$src" ]; then
        cp -r "$src"/. "$stage/$name/"          # a folder: exe plus its DLLs
    else
        cp "$src" "$stage/$name/Fluid Solver"   # a bare ELF or Mach-O
        chmod +x "$stage/$name/Fluid Solver"
    fi
    cp "$REPO/README.md" "$REPO/LICENSE" "$stage/$name/" 2>/dev/null
    mkdir -p "$stage/$name/output"

    ( cd "$stage" && zip -qr9 "$OUT/$name.zip" "$name" )
    rm -rf "$stage"
    printf '  %s.zip\n' "$name"
}

echo "Packing the built variants"
found=0
shopt -s nullglob
for entry in "$DIST"/"Fluid Solver $VERSION "*; do
    base="$(basename "$entry")"
    case "$base" in
        *.zip|*.pkg|*setup.exe|*.run) continue ;;
    esac
    pack "$entry" "$base"
    found=$((found + 1))
done
shopt -u nullglob
[ "$found" = 0 ] && echo "  nothing matched \"Fluid Solver $VERSION *\" in $DIST"

echo "Copying the installers"
shopt -s nullglob
for f in "$DIST"/*setup.exe "$DIST"/*.pkg "$DIST"/*.run; do
    cp "$f" "$OUT/"; printf '  %s\n' "$(basename "$f")"
done
shopt -u nullglob

# The source archive and the README are the two files that are not a build of
# anything: one is what the release was built from, the other is how to use it.
echo "Packing the source"
STAGE="$(mktemp -d)"
SRCDIR="$STAGE/Fluid-Solver-Source-Code"
mkdir -p "$SRCDIR"
# lib/ carries a vendored SFML checkout that the solver does not even link, and
# it is larger than everything else combined. stl_reader is kept, it is one
# header the build actually needs.
tar -C "$REPO" \
    --exclude='./.git' --exclude='./.github' --exclude='./.vs' --exclude='./.vscode' \
    --exclude='./build*' --exclude='./out' --exclude='./dist' --exclude='./release' \
    --exclude='./bt-*' --exclude='./lib/sfml' --exclude='./output/*.vtk' \
    -cf - . | tar -C "$SRCDIR" -xf -
( cd "$STAGE" && zip -qr9 "$OUT/Fluid-Solver-Source-Code.zip" "Fluid-Solver-Source-Code" )
rm -rf "$STAGE"
echo "  Fluid-Solver-Source-Code.zip"

cp "$REPO/README.md" "$OUT/README.md"
echo "  README.md"

( cd "$OUT" && sha256sum -- * > SHA256SUMS.txt 2>/dev/null || shasum -a 256 -- * > SHA256SUMS.txt )

echo
echo "release/$VERSION contains:"
ls -1sh "$OUT" | sed 's/^/  /'
echo
echo "Upload every file in that folder as release assets."
