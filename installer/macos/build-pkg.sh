#!/usr/bin/env bash
# Builds the macOS .pkg installer. Must run on a Mac - pkgbuild and
# productbuild are Apple tools and have no Linux equivalent.
#
#   ./installer/macos/build-pkg.sh 0.2            both architectures, one file
#   ./installer/macos/build-pkg.sh 0.2 arm64      one architecture only
#   ./installer/macos/build-pkg.sh 0.2 x64
#
# One package for both architectures is the default. The Installer knows which
# machine it is on, so the arm64 and Intel payloads can sit in the same file
# and only the matching one is ever unpacked - which means one download, and
# nobody picking the wrong one.
#
# A Distribution XML is what makes that possible, and what gives real checkbox
# selection; a .dmg drag-install can do neither. The build is chosen for the
# machine by default - AVX2 from sysctl, OpenMP from the core count - and the
# user only sees those boxes to override them. macOS has no CUDA at all, and
# arm64 has no AVX2 either, so those switches simply are not shown there.
#
# The variant packages themselves are hidden: they carry a "selected"
# expression that reads the visible tick boxes and the architecture, so exactly
# one of them is ever installed. The desktop UI, when a "<variant>-ui" folder
# exists in dist/, is another tick box with packages of its own.
#
# Below those come the shortcut boxes, spelled the way macOS spells them:
#
#   Launchpad     the .app wrappers in /Applications. Without them the payload
#                 is a pair of Unix executables, which Launchpad, the Dock and
#                 Finder all refuse to treat as applications.
#   Desktop       an alias to the wrapper on the desktop.
#
# There is deliberately no "Dock" box any more. Adding to the Dock means
# rewriting com.apple.dock's persistent-apps behind the user's back and then
# restarting the Dock, which is disruptive when it works and silent when it
# does not. The conclusion page says how to do it by hand: drag the app from
# /Applications onto the Dock, or right-click its icon there and Keep in Dock.
#
# Whatever is missing from dist/ is left out rather than breaking the package.

set -euo pipefail

VERSION="${1:-0.2}"
WANT_ARCH="${2:-all}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$REPO/dist"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IDENT="com.mihann1.fluidsolver"
INSTALL_ROOT="/Applications/Fluid Solver"

# The dist/ folders say macos-x64; Apple's hostArchitectures says x86_64. Both
# spellings are accepted on the command line so neither caller has to remember.
case "$WANT_ARCH" in
    all|both)          ARCHES="arm64 x64"; HOSTARCHS="arm64,x86_64"; SUFFIX="" ;;
    x64|x86_64|intel)  ARCHES="x64";       HOSTARCHS="x86_64";       SUFFIX=" macos-x64" ;;
    arm64|aarch64|arm) ARCHES="arm64";     HOSTARCHS="arm64";        SUFFIX=" macos-arm64" ;;
    *) echo "unknown architecture: $WANT_ARCH (use all, x64 or arm64)" >&2; exit 2 ;;
esac
[ -z "$SUFFIX" ] && SUFFIX=" macos"

variants_for() {   # variants_for <arch>
    if [ "$1" = "arm64" ]; then
        # No AVX2 on Apple Silicon, and no CUDA anywhere on macOS.
        echo "omp plain"
    else
        echo "avx2-omp avx2 omp plain"
    fi
}

echo "Building the$SUFFIX package for $VERSION ($ARCHES)"

# ---- the icon ---------------------------------------------------------------
# A Mach-O executable holds no icon of its own; on macOS the icon belongs to the
# .app bundle. So one .icns is built here from the PNG set the repo already has,
# and make_app drops it into every wrapper it creates. Without this the
# Launchpad entry and the desktop alias both show a blank binary.
ICNS=""
build_icns() {
    command -v iconutil >/dev/null 2>&1 || return 1
    local set="$WORK/FluidSolver.iconset"
    mkdir -p "$set"
    # iconutil insists on these exact names; @2x is the same pixel count as the
    # next size up, which is why each PNG is used twice.
    local pairs="16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 \
256:128x128@2x 256:256x256 512:256x256@2x 512:512x512 1024:512x512@2x"
    local pair source target
    for pair in $pairs; do
        source="$REPO/logo/fluid-solver-${pair%%:*}.png"
        target="$set/icon_${pair##*:}.png"
        [ -f "$source" ] || return 1
        cp "$source" "$target"
    done
    iconutil -c icns "$set" -o "$WORK/FluidSolver.icns" >/dev/null 2>&1 || return 1
    ICNS="$WORK/FluidSolver.icns"
    return 0
}

if build_icns; then
    echo "  icon: $(basename "$ICNS")"
else
    echo "  icon: could not build an .icns, the wrappers will have none"
fi

# Plain strings rather than arrays: macOS still ships bash 3.2, where "set -u"
# treats an empty array as unset and ${#FOUND[@]} aborts the script. Entries are
# "arch:feature", so one list covers both architectures.
FOUND=""
FOUND_COUNT=0
FOUND_UI=""
FOUND_UI_COUNT=0
PKGREFS=""
CHOICES=""
CHOICE_IDS=""

# pkgbuild records the mode every file happens to have, and a "<variant>-ui"
# archive assembled on Windows carries no Unix modes at all - unpacking one
# leaves everything 0600, which ships a UI that will not run and a README the
# user cannot open. The two executables are named, so they are set outright;
# the rest is levelled to the usual 0755/0644.
normalise_root() {   # normalise_root <install root>
    find "$1" -type d -exec chmod 0755 {} + 2>/dev/null || true
    find "$1" -type f -exec chmod 0644 {} + 2>/dev/null || true
    [ -f "$1/Fluid Solver" ] && chmod 0755 "$1/Fluid Solver"
    [ -f "$1/Fluid Solver UI" ] && chmod 0755 "$1/Fluid Solver UI"
    return 0
}

# One payload package per architecture and variant, all of them hidden.
for a in $ARCHES; do
for v in $(variants_for "$a"); do
    tag="$a-$v"
    src="$DIST/Fluid Solver $VERSION macos-$a $v"
    if [ ! -f "$src" ]; then
        echo "  missing: $(basename "$src") - skipped"
        continue
    fi

    root="$WORK/root-$tag$INSTALL_ROOT"
    mkdir -p "$root"
    install -m 0755 "$src" "$root/Fluid Solver"
    mkdir -p "$root/output"
    [ -d "$REPO/models" ] && cp -R "$REPO/models" "$root/" 2>/dev/null || true
    cp "$REPO/README.md" "$REPO/LICENSE" "$root/" 2>/dev/null || true
    normalise_root "$root"

    # A system-wide install leaves "output" owned by root, and the solver writes
    # its frames there. Hand it to whoever is actually logged in.
    mkdir -p "$WORK/scripts-$tag"
    cat > "$WORK/scripts-$tag/postinstall" <<'POST'
#!/bin/sh
dest="${2:-/}"
target="${dest%/}/Applications/Fluid Solver/output"
[ -d "$target" ] || exit 0
who="$(stat -f%Su /dev/console 2>/dev/null || true)"
[ -n "$who" ] && chown -R "$who" "$target" 2>/dev/null
chmod 0775 "$target" 2>/dev/null
exit 0
POST
    chmod +x "$WORK/scripts-$tag/postinstall"

    # The same thing plus the sweep, for the package that carries no UI. macOS
    # packages only ever add files, so without this an install that ticks the
    # Desktop UI followed by one that unticks it leaves the old UI binary in
    # /Applications - and the shortcut packages below keep building a Launchpad
    # entry for it and aiming the desktop alias at it. The removal comes first,
    # because the chown half gives up early when there is no output/ yet.
    mkdir -p "$WORK/scripts-solver-$tag"
    cat > "$WORK/scripts-solver-$tag/postinstall" <<'POST'
#!/bin/sh
dest="${2:-/}"
base="${dest%/}/Applications/Fluid Solver"
rm -f "$base/Fluid Solver UI" 2>/dev/null
rm -rf "${dest%/}/Applications/Fluid Solver UI.app" 2>/dev/null
target="$base/output"
[ -d "$target" ] || exit 0
who="$(stat -f%Su /dev/console 2>/dev/null || true)"
[ -n "$who" ] && chown -R "$who" "$target" 2>/dev/null
chmod 0775 "$target" 2>/dev/null
exit 0
POST
    chmod +x "$WORK/scripts-solver-$tag/postinstall"

    pkgbuild --root "$WORK/root-$tag" \
             --scripts "$WORK/scripts-solver-$tag" \
             --identifier "$IDENT.$tag" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/$tag.pkg" >/dev/null

    FOUND="$FOUND $tag"; FOUND_COUNT=$((FOUND_COUNT + 1))
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.$tag\">$tag.pkg</pkg-ref>"

    # The UI is built per variant too, so it gets its own hidden package with
    # the same condition on it. Ticking "Desktop UI" then always lands the one
    # that matches the solver being installed.
    #
    # That folder is a complete install - the solver, the UI, the dylibs and
    # output/ together, because the UI is a shell that starts the solver. So
    # this package REPLACES the plain one rather than overlaying it: two
    # packages in one product writing the same path is a race over which
    # postinstall and which file mode survive.
    uisrc="$DIST/Fluid Solver $VERSION macos-$a $v-ui"
    [ -d "$uisrc" ] || continue
    uiroot="$WORK/root-ui-$tag$INSTALL_ROOT"
    mkdir -p "$uiroot"
    cp -R "$uisrc"/. "$uiroot/"
    mkdir -p "$uiroot/output"
    [ -d "$REPO/models" ] && cp -R "$REPO/models" "$uiroot/" 2>/dev/null || true
    cp "$REPO/README.md" "$REPO/LICENSE" "$uiroot/" 2>/dev/null || true
    normalise_root "$uiroot"
    pkgbuild --root "$WORK/root-ui-$tag" \
             --scripts "$WORK/scripts-$tag" \
             --identifier "$IDENT.ui.$tag" \
             --version "$VERSION" \
             --install-location / \
             "$WORK/ui-$tag.pkg" >/dev/null
    FOUND_UI="$FOUND_UI $tag"; FOUND_UI_COUNT=$((FOUND_UI_COUNT + 1))
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.ui.$tag\">ui-$tag.pkg</pkg-ref>"
done
done

if [ "$FOUND_COUNT" -eq 0 ]; then
    echo "No macos builds found in $DIST for: $ARCHES. Build them first." >&2
    exit 1
fi

# ---- which switches are worth showing --------------------------------------
# Only the ones that actually have builds on both sides of them, counted across
# everything in the file. arm64 has no AVX2 build at all, so a package built for
# arm64 alone shows no AVX2 box; a combined one does, and the hidden per-variant
# conditions below ignore it on the ARM side.
HAS_AVX2=0; HAS_NO_AVX2=0; HAS_OMP=0; HAS_NO_OMP=0
for t in $FOUND; do
    case "$t" in *avx2*) HAS_AVX2=1 ;; *) HAS_NO_AVX2=1 ;; esac
    case "$t" in *omp*)  HAS_OMP=1  ;; *) HAS_NO_OMP=1  ;; esac
done
SHOW_AVX2=0; [ "$HAS_AVX2" = 1 ] && [ "$HAS_NO_AVX2" = 1 ] && SHOW_AVX2=1
SHOW_OMP=0;  [ "$HAS_OMP" = 1 ]  && [ "$HAS_NO_OMP" = 1 ]  && SHOW_OMP=1

# The condition under which a given "arch:feature" is the one to install,
# written in terms of the visible tick boxes and the machine's architecture.
variant_condition() {   # variant_condition <arch-feature tag>
    local t="$1" a="${1%%-*}" f="${1#*-}" e="" o=""
    if [ "$a" = "arm64" ]; then e="isArm()"; else e="!isArm()"; fi
    if [ "$SHOW_AVX2" = 1 ]; then
        case "$f" in *avx2*) e="$e &amp;&amp; choices['choice-avx2'].selected" ;;
                     *)      e="$e &amp;&amp; !choices['choice-avx2'].selected" ;; esac
    fi
    if [ "$SHOW_OMP" = 1 ]; then
        case "$f" in *omp*) o="choices['choice-omp'].selected" ;;
                     *)     o="!choices['choice-omp'].selected" ;; esac
        e="$e &amp;&amp; $o"
    fi
    printf '%s' "$e"
}

# On Apple Silicon the AVX2 box is meaningless - there is no AVX2 build for it -
# so it starts off and the ARM variants ignore it. On Intel it starts ticked
# when the CPU actually has AVX2. Either way nobody has to answer anything.
if [ "$SHOW_AVX2" = 1 ]; then
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-avx2\"/>"
    CHOICES="$CHOICES<choice id=\"choice-avx2\" title=\"AVX2\" description=\"Vector kernels. Chosen for you: on for an Intel Mac from about 2013 on, and ignored on Apple Silicon, which has no AVX2 at all.\" start_selected=\"hasAvx2()\" enabled=\"!isArm()\"/>"
fi
if [ "$SHOW_OMP" = 1 ]; then
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-omp\"/>"
    CHOICES="$CHOICES<choice id=\"choice-omp\" title=\"OpenMP\" description=\"Use every core of the CPU instead of one. Chosen for you: on for anything with more than one core.\" start_selected=\"hasManyCores()\"/>"
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
has_ui() {   # is there a UI archive for this arch-feature?
    case " $FOUND_UI " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

for t in $FOUND; do
    cond="$(variant_condition "$t")"
    # The plain package is the one to install when the UI box is off, or when
    # this variant has no UI at all. With the box on and a UI present, the "-ui"
    # package below installs instead - it carries this same solver.
    has_ui "$t" && cond="($cond) &amp;&amp; !choices['choice-ui'].selected"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"build-$t\"/>"
    CHOICES="$CHOICES<choice id=\"build-$t\" title=\"$t\" visible=\"false\" enabled=\"false\" start_selected=\"$cond\" selected=\"$cond\"><pkg-ref id=\"$IDENT.$t\"/></choice>"
done

# The UI half of the same conditions, ANDed with the Desktop UI box. A variant
# with no UI archive simply has no line here, so ticking the box next to it
# leaves the plain package in place rather than installing the wrong thing.
for t in $FOUND_UI; do
    cond="($(variant_condition "$t")) &amp;&amp; choices['choice-ui'].selected"
    CHOICE_IDS="$CHOICE_IDS<line choice=\"uibuild-$t\"/>"
    CHOICES="$CHOICES<choice id=\"uibuild-$t\" title=\"$t UI\" visible=\"false\" enabled=\"false\" start_selected=\"$cond\" selected=\"$cond\"><pkg-ref id=\"$IDENT.ui.$t\"/></choice>"
done

# ---- the two shortcut boxes ------------------------------------------------
# Shared preamble for both script-only packages. Written once to a file and
# pasted in front of each postinstall, because macOS still ships bash 3.2 and a
# here-string held in a variable is more trouble than a file.
#
# The payload is a pair of Unix executables, and neither Launchpad nor the Dock
# will take one of those. make_app puts a minimal bundle around them: the UI
# runs straight, the solver reads from a console and so is handed to Terminal.
cat > "$WORK/common.sh" <<'COMMON'
#!/bin/sh
dest="${2:-/}"
apps="${dest%/}/Applications"
base="$apps/Fluid Solver"
# The .icns travels inside the package's scripts folder. Installer runs the
# postinstall from that folder, but which of $0 and the working directory points
# at it has changed between macOS releases, so both are tried.
icon_source=""
for candidate in "$(dirname "$0")/FluidSolver.icns" "./FluidSolver.icns"; do
    [ -f "$candidate" ] && { icon_source="$candidate"; break; }
done

# Whoever is actually logged in. Everything a shortcut touches is theirs, not
# root's, even though this script runs as root.
who="$(stat -f%Su /dev/console 2>/dev/null || true)"
whohome="$(eval echo "~$who" 2>/dev/null || true)"

make_app() {   # make_app <app name> <binary> <needs a terminal: 0|1> <id>
    [ -f "$base/$2" ] || return 1
    app="$apps/$1.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" || return 1

    # The icon travels with the shortcut package and lands beside the wrapper.
    icon_line=""
    if [ -f "$icon_source" ]; then
        mkdir -p "$app/Contents/Resources"
        cp "$icon_source" "$app/Contents/Resources/FluidSolver.icns"
        icon_line="    <key>CFBundleIconFile</key><string>FluidSolver</string>"
    fi

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
$icon_line
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

# The wrapper for one of the two programs, built on demand. Every shortcut
# package calls this for its own program first, so ticking only "Desktop" still
# produces something for the alias to point at.
ensure_app() {   # ensure_app ui|solver -> echoes the .app when it was made
    if [ "$1" = ui ]; then
        make_app "Fluid Solver UI" "Fluid Solver UI" 0 ui || return 1
        printf '%s' "$apps/Fluid Solver UI.app"
    else
        make_app "Fluid Solver" "Fluid Solver" 1 solver || return 1
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
    [ -n "$ICNS" ] && cp "$ICNS" "$WORK/scripts-$1/FluidSolver.icns"
    pkgbuild --nopayload \
             --scripts "$WORK/scripts-$1" \
             --identifier "$IDENT.$1" \
             --version "$VERSION" \
             "$WORK/$1.pkg" >/dev/null
    PKGREFS="$PKGREFS<pkg-ref id=\"$IDENT.$1\">$1.pkg</pkg-ref>"
}

# Two tails per place, one per program. A postinstall cannot read the tick
# boxes, so "which program" has to be baked into the package instead - the same
# trick the hidden per-variant packages already use, and the reason there are
# four of these rather than two.
for kind in ui solver; do
    cat > "$WORK/tail-launchpad-$kind" <<TAIL
ensure_app $kind >/dev/null
exit 0
TAIL

    cat > "$WORK/tail-desktop-$kind" <<TAIL
target="\$(ensure_app $kind)"
[ -n "\$target" ] && [ -d "\$target" ] || exit 0
[ -n "\$who" ] && [ -d "\$whohome/Desktop" ] || exit 0
link="\$whohome/Desktop/\$(basename "\$target")"
rm -rf "\$link"
ln -s "\$target" "\$link" 2>/dev/null
chown -h "\$who" "\$link" 2>/dev/null
exit 0
TAIL

    for place in launchpad desktop; do
        make_script_pkg "$place-$kind" "$WORK/tail-$place-$kind"
    done
done

# Where a shortcut goes and what it starts are two separate questions, so they
# are two blocks of tick boxes. The second one only appears when there is a UI
# to choose - otherwise the console solver takes every shortcut that was asked
# for, and there is nothing to ask about.
if [ "$HAVE_UI" = 1 ]; then
    CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-icon-ui\"/><line choice=\"choice-icon-console\"/>"
    CHOICES="$CHOICES<choice id=\"choice-icon-ui\" title=\"Shortcuts start the UI\" description=\"The window: configures runs, launches the solver and draws the frames.\" start_selected=\"true\"/>"
    CHOICES="$CHOICES<choice id=\"choice-icon-console\" title=\"Shortcuts start the console solver\" description=\"The console version, where every parameter is typed at the prompt. Can be ticked alongside the UI, or instead of it.\" start_selected=\"false\"/>"
fi

# What each of the four hidden packages is worth, in terms of those boxes. The
# console half also fires when no UI went in at all, which is what keeps an
# install without one from ending up with no shortcuts anywhere.
if [ "$HAVE_UI" = 1 ]; then
    UI_ICON_COND="choices['choice-ui'].selected &amp;&amp; choices['choice-icon-ui'].selected"
    CONSOLE_ICON_COND="!choices['choice-ui'].selected || choices['choice-icon-console'].selected"
else
    UI_ICON_COND="false"
    CONSOLE_ICON_COND="true"
fi

# Last in the outline, so they run after the binaries they point at are in
# place. Ticked the way the other two platforms tick them: the menu entry and
# the desktop icon on.
CHOICE_IDS="$CHOICE_IDS<line choice=\"choice-launchpad\"/><line choice=\"choice-desktop\"/>"
CHOICES="$CHOICES<choice id=\"choice-launchpad\" title=\"Launchpad\" description=\"Show Fluid Solver in Launchpad and the Applications folder. Without this the binaries are still installed, but only a terminal can start them.\" start_selected=\"true\"/>"
CHOICES="$CHOICES<choice id=\"choice-desktop\" title=\"Desktop\" description=\"Put an alias on the desktop.\" start_selected=\"true\"/>"

for place in launchpad desktop; do
    for kind in ui solver; do
        if [ "$kind" = ui ]; then cond="($UI_ICON_COND)"; else cond="($CONSOLE_ICON_COND)"; fi
        cond="choices['choice-$place'].selected &amp;&amp; $cond"
        CHOICE_IDS="$CHOICE_IDS<line choice=\"sc-$place-$kind\"/>"
        CHOICES="$CHOICES<choice id=\"sc-$place-$kind\" title=\"$place $kind\" visible=\"false\" enabled=\"false\" start_selected=\"$cond\" selected=\"$cond\"><pkg-ref id=\"$IDENT.$place-$kind\"/></choice>"
    done
done

# The last page. This is where the Dock box used to be, said as an instruction
# instead of as a promise the installer could not keep.
cat > "$WORK/conclusion.txt" <<'CONCLUSION'
Fluid Solver is in your Applications folder.

Adding it to the Dock is the one thing this installer deliberately leaves to
you: doing it from a package means rewriting the Dock's own preference file
and restarting it, which is disruptive when it works and silent when it does
not. By hand it is one gesture and always works:

  - open Applications, drag "Fluid Solver UI" (or "Fluid Solver") to the Dock,

or, once it is running:

  - right-click its icon in the Dock and choose Options > Keep in Dock.

Frames are written to /Applications/Fluid Solver/output.
CONCLUSION

cat > "$WORK/Distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Fluid Solver $VERSION</title>
    <organization>com.mihann1</organization>
    <options customize="always" require-scripts="false" hostArchitectures="$HOSTARCHS"/>
    <domains enable_anywhere="true" enable_currentUserHome="true" enable_localSystem="true"/>
    <license file="LICENSE"/>
    <conclusion file="conclusion.txt"/>
    <background file="background.png" mime-type="image/png" alignment="bottomleft" scaling="proportional"/>
    <script><![CDATA[
// Which machine this is. hw.optional.arm64 does not exist on Intel, where
// sysctl throws rather than returning 0, so both are wrapped.
function isArm() {
    try { return system.sysctl('hw.optional.arm64') == 1; } catch (e) { return false; }
}
// Ticked by default when this Mac can actually use AVX2. The key does not
// exist on Apple Silicon, where sysctl throws rather than returning 0.
function hasAvx2() {
    try { return system.sysctl('hw.optional.avx2_0') == 1; } catch (e) { return false; }
}
// OpenMP is worth having on anything with more than one core, which is every
// Mac that can run this - but the check is cheap and honest.
function hasManyCores() {
    try { return system.sysctl('hw.ncpu') > 1; } catch (e) { return true; }
}
    ]]></script>
    <choices-outline>$CHOICE_IDS</choices-outline>
    $CHOICES
    $PKGREFS
</installer-gui-script>
EOF

cp "$REPO/LICENSE" "$WORK/LICENSE"
cp "$REPO/logo/fluid-solver-256.png" "$WORK/background.png" 2>/dev/null || true

OUT="$DIST/Fluid Solver $VERSION$SUFFIX.pkg"
productbuild --distribution "$WORK/Distribution.xml" \
             --package-path "$WORK" \
             --resources "$WORK" \
             "$OUT"

echo "Built: $OUT  ($FOUND_COUNT build(s):$FOUND$([ "$HAVE_UI" = 1 ] && echo ' + UI') + Launchpad/Desktop)"
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
