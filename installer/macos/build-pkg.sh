#!/usr/bin/env bash
# Builds the macOS .pkg installer. Must run on a Mac - pkgbuild and
# productbuild are Apple tools and have no Linux equivalent.
#
#   ./installer/macos/build-pkg.sh 0.1 arm64
#
# A Distribution XML is what gives real checkbox component selection; a .dmg
# drag-install cannot do that. The variant is picked the same way as on the
# other two platforms, except macOS has no CUDA at all, and arm64 has no AVX2
# either, so arm64 offers two choices and x64 offers four.

set -euo pipefail

VERSION="${1:-0.1}"
ARCH="${2:-arm64}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$REPO/dist"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IDENT="com.mihann1.fluidsolver"
INSTALL_ROOT="/Applications/Fluid Solver"

if [ "$ARCH" = "arm64" ]; then
    VARIANTS=(omp plain)
else
    VARIANTS=(avx2-omp avx2 omp plain)
fi

echo "Building the $ARCH package for $VERSION"

# One payload package per variant. The Distribution file below turns them into
# radio buttons, so exactly one lands in /Applications.
PKGREFS=""
CHOICES=""
CHOICE_IDS=""
for v in "${VARIANTS[@]}"; do
    src="$DIST/Fluid Solver $VERSION macos-$ARCH $v"
    if [ ! -f "$src" ]; then
        echo "  missing: $src - skipped"
        continue
    fi

    root="$WORK/root-$v$INSTALL_ROOT"
    mkdir -p "$root"
    install -m 0755 "$src" "$root/Fluid Solver"
    mkdir -p "$root/output"
    [ -d "$REPO/models" ] && cp -r "$REPO/models" "$root/" 2>/dev/null || true
    cp "$REPO/README.md" "$REPO/LICENSE" "$root/" 2>/dev/null || true

    pkgbuild --root "$WORK/root-$v" \
             --identifier "$IDENT.$v" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/$v.pkg" >/dev/null

    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.$v\">$v.pkg</pkg-ref>"
    CHOICES="$CHOICES<choice id=\"choice-$v\" title=\"$v\" start_selected=\"false\" selected=\"choice-$v\"><pkg-ref id=\"$IDENT.$v\"/></choice>"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-$v\"/>"
done

if [ -z "$PKGREFS" ]; then
    echo "No macos-$ARCH builds found in $DIST. Build them first." >&2
    exit 1
fi

cat > "$WORK/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Fluid Solver $VERSION</title>
    <organization>com.mihann1</organization>
    <options customize="always" require-scripts="false" hostArchitectures="$ARCH"/>
    <license file="LICENSE"/>
    <background file="background.png" mime-type="image/png" alignment="bottomleft" scaling="proportional"/>
    <choices-outline>$CHOICE_IDS</choices-outline>
    $CHOICES
    $PKGREFS
</installer-gui-script>
EOF

cp "$REPO/LICENSE" "$WORK/LICENSE"
cp "$REPO/logo/fluid-solver-256.png" "$WORK/background.png" 2>/dev/null || true

OUT="$DIST/Fluid Solver $VERSION macos-$ARCH.pkg"
productbuild --distribution "$WORK/Distribution.xml" \
             --package-path "$WORK" \
             --resources "$WORK" \
             "$OUT"

echo "Built: $OUT"
cat <<'NOTE'

Gatekeeper will refuse to open this on any machine but the one that built it
until it is signed and notarised. With an Apple Developer account:

    productsign --sign "Developer ID Installer: <name> (<team>)" in.pkg out.pkg
    xcrun notarytool submit out.pkg --apple-id <id> --team-id <team> \
          --password <app-specific-password> --wait
    xcrun stapler staple out.pkg

Without one, users have to right-click the package and choose Open, then
confirm a warning that says the developer cannot be verified. Say so in the
release notes rather than letting them find out.
NOTE
