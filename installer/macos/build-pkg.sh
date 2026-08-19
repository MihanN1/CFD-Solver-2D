#!/usr/bin/env bash
# Builds the macOS .pkg installer. Must run on a Mac - pkgbuild and
# productbuild are Apple tools and have no Linux equivalent.
#
#   ./installer/macos/build-pkg.sh 0.1 arm64
#   ./installer/macos/build-pkg.sh 0.1 x64
#
# A Distribution XML is what gives real checkbox selection; a .dmg drag-install
# cannot do that. The user ticks AVX2 and OpenMP independently, exactly as on
# the other two platforms, and the matching build is what lands in
# /Applications. macOS has no CUDA at all, and arm64 has no AVX2 either, so
# that switch simply is not shown there.
#
# The variant packages themselves are hidden: they carry a "selected"
# expression that reads the two visible tick boxes, so exactly one of them is
# ever installed. The desktop UI, when dist/ui-macos-<arch> exists, is a third
# tick box with a package of its own.
#
# Whatever is missing from dist/ is left out rather than breaking the package.

set -euo pipefail

VERSION="${1:-0.1}"
ARCH="${2:-arm64}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$REPO/dist"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IDENT="com.mihann1.fluidsolver"
INSTALL_ROOT="/Applications/Fluid Solver"

# The dist/ folders say macos-x64; Apple's hostArchitectures says x86_64. Both
# spellings are accepted on the command line so neither caller has to remember.
case "$ARCH" in
    x64|x86_64|intel)  ARCH=x64;   HOSTARCH=x86_64 ;;
    arm64|aarch64|arm) ARCH=arm64; HOSTARCH=arm64 ;;
    *) echo "unknown architecture: $ARCH (use x64 or arm64)" >&2; exit 2 ;;
esac

if [ "$ARCH" = "arm64" ]; then
    VARIANTS=(omp plain)
else
    VARIANTS=(avx2-omp avx2 omp plain)
fi

echo "Building the $ARCH package for $VERSION"

# A plain string rather than an array: macOS still ships bash 3.2, where
# "set -u" treats an empty array as unset and ${#FOUND[@]} aborts the script.
FOUND=""
FOUND_COUNT=0
PKGREFS=""
CHOICES=""
CHOICE_IDS=""

# One payload package per variant, all of them hidden.
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
    [ -d "$REPO/models" ] && cp -R "$REPO/models" "$root/" 2>/dev/null || true
    cp "$REPO/README.md" "$REPO/LICENSE" "$root/" 2>/dev/null || true

    # A system-wide install leaves "output" owned by root, and the solver writes
    # its frames there. Hand it to whoever is actually logged in.
    mkdir -p "$WORK/scripts-$v"
    cat > "$WORK/scripts-$v/postinstall" <<'POST'
#!/bin/sh
dest="${2:-/}"
target="${dest%/}/Applications/Fluid Solver/output"
[ -d "$target" ] || exit 0
who="$(stat -f%Su /dev/console 2>/dev/null || true)"
[ -n "$who" ] && chown -R "$who" "$target" 2>/dev/null
chmod 0775 "$target" 2>/dev/null
exit 0
POST
    chmod +x "$WORK/scripts-$v/postinstall"

    pkgbuild --root "$WORK/root-$v" \
             --scripts "$WORK/scripts-$v" \
             --identifier "$IDENT.$v" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/$v.pkg" >/dev/null

    FOUND="$FOUND $v"; FOUND_COUNT=$((FOUND_COUNT + 1))
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.$v\">$v.pkg</pkg-ref>"
done

if [ "$FOUND_COUNT" -eq 0 ]; then
    echo "No macos-$ARCH builds found in $DIST. Build them first." >&2
    exit 1
fi

# ---- which switches are worth showing --------------------------------------
# Only the ones that actually have builds on both sides of them. arm64 has no
# AVX2 build at all, so no AVX2 box; a partial dist can drop the OpenMP one the
# same way.
HAS_AVX2=0; HAS_NO_AVX2=0; HAS_OMP=0; HAS_NO_OMP=0
for v in $FOUND; do
    case "$v" in *avx2*) HAS_AVX2=1 ;; *) HAS_NO_AVX2=1 ;; esac
    case "$v" in *omp*)  HAS_OMP=1  ;; *) HAS_NO_OMP=1  ;; esac
done
SHOW_AVX2=0; [ "$HAS_AVX2" = 1 ] && [ "$HAS_NO_AVX2" = 1 ] && SHOW_AVX2=1
SHOW_OMP=0;  [ "$HAS_OMP" = 1 ]  && [ "$HAS_NO_OMP" = 1 ]  && SHOW_OMP=1

# The condition under which a given variant is the one to install, written in
# terms of the visible tick boxes.
variant_condition() {
    local f="$1" e="" o=""
    if [ "$SHOW_AVX2" = 1 ]; then
        case "$f" in *avx2*) e="choices['choice-avx2'].selected" ;;
                     *)      e="!choices['choice-avx2'].selected" ;; esac
    fi
    if [ "$SHOW_OMP" = 1 ]; then
        case "$f" in *omp*) o="choices['choice-omp'].selected" ;;
                     *)     o="!choices['choice-omp'].selected" ;; esac
        if [ -n "$e" ]; then e="$e &amp;&amp; $o"; else e="$o"; fi
    fi
    [ -z "$e" ] && e="true"
    printf '%s' "$e"
}

# The tick boxes come first in the outline, then the hidden variants.
if [ "$SHOW_AVX2" = 1 ]; then
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-avx2\"/>"
    CHOICES="$CHOICES<choice id=\"choice-avx2\" title=\"AVX2\" description=\"Vector kernels. Needs an Intel Mac from about 2013 on.\" start_selected=\"hasAvx2()\"/>"
fi
if [ "$SHOW_OMP" = 1 ]; then
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-omp\"/>"
    CHOICES="$CHOICES<choice id=\"choice-omp\" title=\"OpenMP\" description=\"Use every core of the CPU instead of one.\" start_selected=\"true\"/>"
fi

UI_SRC="$DIST/ui-macos-$ARCH"
HAVE_UI=0
if [ -d "$UI_SRC" ]; then
    root="$WORK/root-ui$INSTALL_ROOT"
    mkdir -p "$root"
    cp -R "$UI_SRC"/. "$root/"
    [ -f "$root/Fluid Solver UI" ] && chmod 0755 "$root/Fluid Solver UI"
    pkgbuild --root "$WORK/root-ui" \
             --identifier "$IDENT.ui" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/ui.pkg" >/dev/null
    HAVE_UI=1
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.ui\">ui.pkg</pkg-ref>"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-ui\"/>"
    CHOICES="$CHOICES<choice id=\"choice-ui\" title=\"Desktop UI\" description=\"Configures runs, launches the solver and draws the frames.\" start_selected=\"true\"><pkg-ref id=\"$IDENT.ui\"/></choice>"
fi

# "build-", not "choice-": the tick boxes are already choice-avx2 and
# choice-omp, and two choices sharing an id is not a thing the Installer
# survives.
for v in $FOUND; do
    CHOICE_IDS="$CHOICE_IDS<line choice=\"build-$v\"/>"
    CHOICES="$CHOICES<choice id=\"build-$v\" title=\"$v\" visible=\"false\" enabled=\"false\" start_selected=\"$(variant_condition "$v")\" selected=\"$(variant_condition "$v")\"><pkg-ref id=\"$IDENT.$v\"/></choice>"
done

cat > "$WORK/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Fluid Solver $VERSION</title>
    <organization>com.mihann1</organization>
    <options customize="always" require-scripts="false" hostArchitectures="$HOSTARCH"/>
    <domains enable_anywhere="true" enable_currentUserHome="true" enable_localSystem="true"/>
    <license file="LICENSE"/>
    <background file="background.png" mime-type="image/png" alignment="bottomleft" scaling="proportional"/>
    <script><![CDATA[
// Ticked by default when this Mac can actually use AVX2. The key does not
// exist on Apple Silicon, where sysctl throws rather than returning 0.
function hasAvx2() {
    try { return system.sysctl('hw.optional.avx2_0') == 1; } catch (e) { return false; }
}
    ]]></script>
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

echo "Built: $OUT  ($FOUND_COUNT build(s):$FOUND$([ "$HAVE_UI" = 1 ] && echo ' + UI'))"
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
