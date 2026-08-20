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
# Below those come the three shortcut boxes, the same three the other two
# platforms ask about, spelled the way macOS spells them:
#
#   Launchpad     the .app wrappers in /Applications. Without them the payload
#                 is a pair of Unix executables, which Launchpad, the Dock and
#                 Finder all refuse to treat as applications.
#   Desktop       an alias to the wrapper on the desktop.
#   Dock          the wrapper added to the Dock's persistent-apps.
#
# Each is a script-only package, so unticking one simply leaves that step out.
# They come last in the outline because they run after the binaries they point
# at have landed.
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
FOUND_UI=""
FOUND_UI_COUNT=0
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

    # The UI is built per variant too, so it gets its own hidden package with
    # the same condition on it. Ticking "Desktop UI" then always lands the one
    # that matches the solver being installed.
    #
    # That folder is a complete install - the solver, the UI, the dylibs and
    # output/ together, because the UI is a shell that starts the solver. So
    # this package REPLACES the plain one rather than overlaying it: two
    # packages in one product writing the same path is a race over which
    # postinstall and which file mode survive.
    uisrc="$DIST/Fluid Solver $VERSION macos-$ARCH $v-ui"
    [ -d "$uisrc" ] || continue
    uiroot="$WORK/root-ui-$v$INSTALL_ROOT"
    mkdir -p "$uiroot"
    cp -R "$uisrc"/. "$uiroot/"
    mkdir -p "$uiroot/output"
    [ -d "$REPO/models" ] && cp -R "$REPO/models" "$uiroot/" 2>/dev/null || true
    cp "$REPO/README.md" "$REPO/LICENSE" "$uiroot/" 2>/dev/null || true
    chmod 0755 "$uiroot/Fluid Solver" 2>/dev/null || true
    [ -f "$uiroot/Fluid Solver UI" ] && chmod 0755 "$uiroot/Fluid Solver UI"
    pkgbuild --root "$WORK/root-ui-$v" \
             --scripts "$WORK/scripts-$v" \
             --identifier "$IDENT.ui.$v" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/ui-$v.pkg" >/dev/null
    FOUND_UI="$FOUND_UI $v"; FOUND_UI_COUNT=$((FOUND_UI_COUNT + 1))
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.ui.$v\">ui-$v.pkg</pkg-ref>"
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

# One visible tick box for the UI, carrying no package of its own - the actual
# payload hangs off the hidden per-variant choices below, which read this box.
HAVE_UI=0
if [ "$FOUND_UI_COUNT" -gt 0 ]; then
    HAVE_UI=1
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-ui\"/>"
    CHOICES="$CHOICES<choice id=\"choice-ui\" title=\"Desktop UI\" description=\"Configures runs, launches the solver and draws the frames. Installed to match the solver build above.\" start_selected=\"true\"/>"
fi

# "build-", not "choice-": the tick boxes are already choice-avx2 and
# choice-omp, and two choices sharing an id is not a thing the Installer
# survives.
has_ui() {   # is there a UI archive for this variant?
    case " $FOUND_UI " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

for v in $FOUND; do
    cond="$(variant_condition "$v")"
    # The plain package is the one to install when the UI box is off, or when
    # this variant has no UI at all. With the box on and a UI present, the "-ui"
    # package below installs instead - it carries this same solver.
    has_ui "$v" && cond="($cond) &amp;&amp; !choices['choice-ui'].selected"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"build-$v\"/>"
    CHOICES="$CHOICES<choice id=\"build-$v\" title=\"$v\" visible=\"false\" enabled=\"false\" start_selected=\"$cond\" selected=\"$cond\"><pkg-ref id=\"$IDENT.$v\"/></choice>"
done

# The UI half of the same conditions, ANDed with the Desktop UI box. A variant
# with no UI archive simply has no line here, so ticking the box next to it
# leaves the plain package in place rather than installing the wrong thing.
for v in $FOUND_UI; do
    cond="($(variant_condition "$v")) &amp;&amp; choices['choice-ui'].selected"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"uibuild-$v\"/>"
    CHOICES="$CHOICES<choice id=\"uibuild-$v\" title=\"$v UI\" visible=\"false\" enabled=\"false\" start_selected=\"$cond\" selected=\"$cond\"><pkg-ref id=\"$IDENT.ui.$v\"/></choice>"
done

# ---- the three shortcut boxes ----------------------------------------------
# Shared preamble for all three script-only packages. Written once to a file
# and pasted in front of each postinstall, because macOS still ships bash 3.2
# and a here-string held in a variable is more trouble than a file.
#
# The payload is a pair of Unix executables, and neither Launchpad nor the Dock
# will take one of those. make_app puts a minimal bundle around them: the UI
# runs straight, the solver reads from a console and so is handed to Terminal.
cat > "$WORK/common.sh" <<'COMMON'
#!/bin/sh
dest="${2:-/}"
apps="${dest%/}/Applications"
base="$apps/Fluid Solver"

# Whoever is actually logged in. Everything a shortcut touches is theirs, not
# root's, even though this script runs as root.
who="$(stat -f%Su /dev/console 2>/dev/null || true)"
whohome="$(eval echo "~$who" 2>/dev/null || true)"

make_app() {   # make_app <app name> <binary> <needs a terminal: 0|1> <id>
    [ -f "$base/$2" ] || return 1
    app="$apps/$1.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" || return 1

    if [ "$3" = 1 ]; then
        cat > "$app/Contents/MacOS/launcher" <<LAUNCH
#!/bin/sh
open -a Terminal "$base/$2"
LAUNCH
    else
        cat > "$app/Contents/MacOS/launcher" <<LAUNCH
#!/bin/sh
cd "$base" || exit 1
exec "$base/$2"
LAUNCH
    fi
    chmod 0755 "$app/Contents/MacOS/launcher"

    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>$1</string>
    <key>CFBundleDisplayName</key><string>$1</string>
    <key>CFBundleExecutable</key><string>launcher</string>
    <key>CFBundleIdentifier</key><string>com.mihann1.fluidsolver.launcher.$4</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
    touch "$app"
    return 0
}

# Both wrappers, whichever binaries are actually there. Called by all three
# packages, so ticking only "Dock" still gets something to put in the Dock.
ensure_apps() {
    make_app "Fluid Solver UI" "Fluid Solver UI" 0 ui
    make_app "Fluid Solver" "Fluid Solver" 1 solver
}

# The UI is the windowed program, so a single shortcut points at it; without
# it the console solver takes its place.
shortcut_app() {
    if [ -d "$apps/Fluid Solver UI.app" ]; then
        printf '%s' "$apps/Fluid Solver UI.app"
    else
        printf '%s' "$apps/Fluid Solver.app"
    fi
}
COMMON

# One script-only package per box. --nopayload is what makes them free: they
# install nothing and only run their postinstall.
make_script_pkg() {   # make_script_pkg <id suffix> <postinstall tail file>
    mkdir -p "$WORK/scripts-$1"
    cat "$WORK/common.sh" "$2" > "$WORK/scripts-$1/postinstall"
    chmod +x "$WORK/scripts-$1/postinstall"
    pkgbuild --nopayload \
             --scripts "$WORK/scripts-$1" \
             --identifier "$IDENT.$1" \
             --version "$VERSION" \
             "$WORK/$1.pkg" >/dev/null
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.$1\">$1.pkg</pkg-ref>"
}

cat > "$WORK/tail-launchpad" <<'TAIL'
ensure_apps
exit 0
TAIL

cat > "$WORK/tail-desktop" <<'TAIL'
ensure_apps
target="$(shortcut_app)"
[ -d "$target" ] || exit 0
[ -n "$who" ] && [ -d "$whohome/Desktop" ] || exit 0
link="$whohome/Desktop/$(basename "$target")"
rm -rf "$link"
ln -s "$target" "$link" 2>/dev/null
chown -h "$who" "$link" 2>/dev/null
exit 0
TAIL

# defaults and killall both run as the logged-in user: the Dock is a per-user
# thing and root has no Dock to write to.
cat > "$WORK/tail-dock" <<'TAIL'
ensure_apps
target="$(shortcut_app)"
[ -d "$target" ] || exit 0
[ -n "$who" ] || exit 0
uid="$(id -u "$who" 2>/dev/null)" || exit 0

entry="<dict><key>tile-data</key><dict><key>file-data</key><dict><key>_CFURLString</key><string>$target</string><key>_CFURLStringType</key><integer>0</integer></dict></dict></dict>"

launchctl asuser "$uid" sudo -u "$who" \
    defaults write com.apple.dock persistent-apps -array-add "$entry" 2>/dev/null
launchctl asuser "$uid" sudo -u "$who" killall Dock 2>/dev/null
exit 0
TAIL

make_script_pkg launchpad "$WORK/tail-launchpad"
make_script_pkg desktop   "$WORK/tail-desktop"
make_script_pkg dock      "$WORK/tail-dock"

# Last in the outline, so they run after the binaries they point at are in
# place. Ticked the way the other two platforms tick them: the menu entry and
# the desktop icon on, the taskbar off.
CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-launchpad\"/><line choice=\"choice-desktop\"/><line choice=\"choice-dock\"/>"
CHOICES="$CHOICES<choice id=\"choice-launchpad\" title=\"Launchpad\" description=\"Show Fluid Solver in Launchpad and the Applications folder. Without this the binaries are still installed, but only a terminal can start them.\" start_selected=\"true\"><pkg-ref id=\"$IDENT.launchpad\"/></choice>"
CHOICES="$CHOICES<choice id=\"choice-desktop\" title=\"Desktop\" description=\"Put an alias on the desktop.\" start_selected=\"true\"><pkg-ref id=\"$IDENT.desktop\"/></choice>"
CHOICES="$CHOICES<choice id=\"choice-dock\" title=\"Dock\" description=\"Add it to the Dock. The Dock restarts once, which is what makes the new icon appear.\" start_selected=\"false\"><pkg-ref id=\"$IDENT.dock\"/></choice>"

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

echo "Built: $OUT  ($FOUND_COUNT build(s):$FOUND$([ "$HAVE_UI" = 1 ] && echo ' + UI') + Launchpad/Desktop/Dock)"
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
