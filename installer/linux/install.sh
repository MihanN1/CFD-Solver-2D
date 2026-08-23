#!/usr/bin/env bash
# Payload script of the self-extracting Linux installer.
#
# makeself runs this from inside the unpacked archive, so every variant file
# for every architecture is right here in the working directory. The machine is
# measured, one of them is installed as "Fluid Solver", and nothing downstream
# has to know which.
#
# Interactive:   ./Fluid-Solver-<ver>-linux.run
# Pick by hand:  ./Fluid-Solver-<ver>-linux.run -- --choose
# Unattended:    ./Fluid-Solver-<ver>-linux.run -- --avx2 --openmp --cuda \
#                    --ui --prefix=/opt/fluid-solver --menu --desktop --yes
#
# By default the three accelerator switches are not asked about at all: AVX2
# comes from /proc/cpuinfo, CUDA from whether an NVIDIA driver is loaded,
# OpenMP from the core count, and the best build the payload has for that
# answer is what goes in. --choose brings back the questions; naming a switch
# on the command line overrides it for that switch alone.
#
# The two shortcut switches are separate because they land in two different
# places: --menu writes the .desktop entry the application menu reads, and
# --desktop drops a copy on the desktop itself. --shortcut is kept as an alias
# for --menu. There is deliberately no --taskbar any more - see the note the
# installer prints at the end.
#
# Which program those shortcuts start is a second, independent question:
# --icon-ui and --icon-console, either, both or neither. Defaults to the UI
# alone when one is installed, and to the console solver when none is. Both are
# ignored on an install without the UI, which has only one program to point at.

set -uo pipefail

APP="Fluid Solver"
VERSION="__VERSION__"

PREFIX=""
ARCH=""
WANT_AVX2=""
WANT_OMP=""
WANT_CUDA=""
WANT_UI=""
WANT_MENU=""
WANT_DESKTOP=""
WANT_ICON_UI=""
WANT_ICON_CONSOLE=""
ASSUME_YES=0
CHOOSE=0

for arg in "$@"; do
    case "$arg" in
        --prefix=*)  PREFIX="${arg#--prefix=}" ;;
        --arch=*)    ARCH="${arg#--arch=}" ;;
        --choose)    CHOOSE=1 ;;
        --auto)      CHOOSE=0 ;;
        --avx2)      WANT_AVX2=1 ;;
        --no-avx2)   WANT_AVX2=0 ;;
        --openmp)    WANT_OMP=1 ;;
        --no-openmp) WANT_OMP=0 ;;
        --cuda)      WANT_CUDA=1 ;;
        --no-cuda)   WANT_CUDA=0 ;;
        --ui)        WANT_UI=1 ;;
        --no-ui)     WANT_UI=0 ;;
        --menu|--shortcut)       WANT_MENU=1 ;;
        --no-menu|--no-shortcut) WANT_MENU=0 ;;
        --desktop)    WANT_DESKTOP=1 ;;
        --no-desktop) WANT_DESKTOP=0 ;;
        --icon-ui)         WANT_ICON_UI=1 ;;
        --no-icon-ui)      WANT_ICON_UI=0 ;;
        --icon-console)    WANT_ICON_CONSOLE=1 ;;
        --no-icon-console) WANT_ICON_CONSOLE=0 ;;
        --yes|-y)    ASSUME_YES=1 ;;
        --help|-h)
            sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# What this machine is. The names match the ones in the file names, not the
# ones uname prints. An aarch64 box with no arm64 rows in the payload has no
# fallback the way Windows on ARM does, so it is told so rather than handed an
# x86 binary it cannot execute.
if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        x86_64|amd64)   ARCH=x64 ;;
        aarch64|arm64)  ARCH=arm64 ;;
        i386|i486|i586|i686) ARCH=x86 ;;
        *) ARCH="$(uname -m)" ;;
    esac
    # A 64-bit kernel running a 32-bit userspace, and 32-bit-only payloads.
    if [ "$ARCH" = x64 ] && ! ls "$APP $VERSION linux-x64 "* >/dev/null 2>&1; then
        ls "$APP $VERSION linux-x86 "* >/dev/null 2>&1 && ARCH=x86
    fi
fi

# A desktop file and a dock favourite belong to a person, not to a machine, so
# under sudo they go to whoever ran sudo rather than to root, who has no
# session to put them in.
TARGET_USER="${SUDO_USER:-$(id -un)}"
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)"
[ -n "$TARGET_HOME" ] || TARGET_HOME="$HOME"

as_target_user() {
    if [ "$TARGET_USER" != "$(id -un)" ] && command -v sudo >/dev/null 2>&1; then
        sudo -u "$TARGET_USER" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$TARGET_USER")/bus" \
            "$@"
    else
        "$@"
    fi
}

# ---- what this machine can actually use -----------------------------------
HAS_AVX2=0; grep -qm1 '\bavx2\b' /proc/cpuinfo 2>/dev/null && HAS_AVX2=1
HAS_NVIDIA=0
if command -v nvidia-smi >/dev/null 2>&1 || [ -e /dev/nvidia0 ] || [ -e /proc/driver/nvidia/version ]; then
    HAS_NVIDIA=1
fi
CORES=$(nproc 2>/dev/null || echo 1)

# ---- what this installer actually carries ----------------------------------
# A switch is only worth asking about when both answers are in the payload.
# There is no 32-bit CUDA build, and a build machine without nvcc or without
# multilib produces a partial matrix, so this is read off the payload rather
# than assumed.
feature_of() {   # <avx2> <omp> <cuda> -> avx2-omp-cuda | ... | plain
    local f=""
    [ "$1" = 1 ] && f="${f}avx2-"
    [ "$2" = 1 ] && f="${f}omp-"
    [ "$3" = 1 ] && f="${f}cuda-"
    f="${f%-}"; printf '%s' "${f:-plain}"
}

COMBOS=(); FEATURES=()
for a in 1 0; do for o in 1 0; do for c in 1 0; do
    f="$(feature_of "$a" "$o" "$c")"
    [ -f "$APP $VERSION linux-$ARCH $f" ] || continue
    COMBOS+=("$a$o$c"); FEATURES+=("$f")
done; done; done

if [ "${#FEATURES[@]}" -eq 0 ]; then
    echo "This installer carries no linux-$ARCH build." >&2
    echo "What it does have:" >&2
    for other in x64 x86 arm64; do
        ls "$APP $VERSION linux-$other "* >/dev/null 2>&1 &&
            echo "  linux-$other" >&2
    done
    echo "Pass --arch=<one of those> to install it anyway, if your machine can" >&2
    echo "run it." >&2
    exit 1
fi

varies() {       # <position 0=avx2 1=omp 2=cuda> -> offered both ways?
    local pos="$1" seen0=0 seen1=0 c
    for c in "${COMBOS[@]}"; do
        if [ "${c:$pos:1}" = 1 ]; then seen1=1; else seen0=1; fi
    done
    [ "$seen0" = 1 ] && [ "$seen1" = 1 ]
}

only_value() {   # <position> -> the single value present on that axis
    printf '%s' "${COMBOS[0]:$1:1}"
}

have_combo() {
    local c
    for c in "${COMBOS[@]}"; do [ "$c" = "$1" ] && return 0; done
    return 1
}

echo "$APP $VERSION - linux-$ARCH"
echo
echo "This machine: AVX2 $([ $HAS_AVX2 = 1 ] && echo supported || echo 'not supported'),"\
     "NVIDIA driver $([ $HAS_NVIDIA = 1 ] && echo present || echo absent), $CORES cores."
echo "This installer carries ${#FEATURES[@]} build(s): ${FEATURES[*]}"

# Defaults follow the machine; anything given on the command line wins; an axis
# the payload does not vary is pinned to what it does have.
[ -z "$WANT_AVX2" ] && WANT_AVX2=$HAS_AVX2
[ -z "$WANT_OMP"  ] && WANT_OMP=$([ "$CORES" -gt 1 ] && echo 1 || echo 0)
[ -z "$WANT_CUDA" ] && WANT_CUDA=$HAS_NVIDIA
varies 0 || WANT_AVX2="$(only_value 0)"
varies 1 || WANT_OMP="$(only_value 1)"
varies 2 || WANT_CUDA="$(only_value 2)"

ask() {   # ask <question> <default 0|1> -> echoes 0 or 1
    local q="$1" def="$2" reply hint
    [ "$ASSUME_YES" = 1 ] && { echo "$def"; return; }
    hint=$([ "$def" = 1 ] && echo "Y/n" || echo "y/N")
    read -r -p "$q [$hint] " reply </dev/tty || reply=""
    case "${reply,,}" in
        y|yes) echo 1 ;;
        n|no)  echo 0 ;;
        *)     echo "$def" ;;
    esac
}

# Only when asked for. The default is that nobody is asked anything about the
# accelerators: the three answers above already came from the machine, and a
# question whose right answer the installer already knows is a question that
# only gives the user a chance to get it wrong.
if [ "$CHOOSE" = 1 ] && [ "$ASSUME_YES" != 1 ]; then
    echo
    varies 0 && WANT_AVX2=$(ask "Use the AVX2 build?"   "$WANT_AVX2")
    varies 1 && WANT_OMP=$(ask  "Use the OpenMP build?" "$WANT_OMP")
    varies 2 && WANT_CUDA=$(ask "Use the CUDA build?"   "$WANT_CUDA")
fi

if [ "$WANT_AVX2" = 1 ] && [ "$HAS_AVX2" = 0 ]; then
    echo "This CPU has no AVX2; that build would die with an illegal instruction." >&2
    exit 1
fi
if [ "$WANT_CUDA" = 1 ] && [ "$HAS_NVIDIA" = 0 ]; then
    echo "note: no NVIDIA driver here, so the CUDA build will run on the CPU anyway."
fi

# Every switch is dropped in turn rather than all at once, so a payload missing
# "avx2-omp" still lands on "avx2" or "omp" instead of falling to "plain".
# CUDA goes first: without a driver it is the one that buys nothing.
best_feature() {
    local a="$1" o="$2" c="$3"
    local try
    for try in "$a$o$c" "$a${o}0" "${a}0$c" "${a}00" "0$o$c" "0${o}0" "000"; do
        if have_combo "$try"; then
            # An AVX2 build on a CPU without AVX2 is never a candidate, whatever
            # else is missing: it dies on its first instruction.
            [ "${try:0:1}" = 1 ] && [ "$HAS_AVX2" = 0 ] && continue
            printf '%s' "$try"
            return 0
        fi
    done
    return 1
}

FEAT="$(feature_of "$WANT_AVX2" "$WANT_OMP" "$WANT_CUDA")"
if ! have_combo "$WANT_AVX2$WANT_OMP$WANT_CUDA"; then
    PICKED="$(best_feature "$WANT_AVX2" "$WANT_OMP" "$WANT_CUDA")"
    if [ -n "$PICKED" ]; then
        WANT_AVX2="${PICKED:0:1}"; WANT_OMP="${PICKED:1:1}"; WANT_CUDA="${PICKED:2:1}"
        NEW_FEAT="$(feature_of "$WANT_AVX2" "$WANT_OMP" "$WANT_CUDA")"
        echo "note: no '$FEAT' build in this installer; using '$NEW_FEAT' instead."
        FEAT="$NEW_FEAT"
    else
        echo
        echo "This installer does not carry the '$FEAT' build. It has:"
        i=1; for f in "${FEATURES[@]}"; do echo "  $i) $f"; i=$((i+1)); done
        [ "$ASSUME_YES" = 1 ] && exit 1
        read -r -p "Install which one? [1] " reply </dev/tty || reply=""
        [ -z "$reply" ] && reply=1
        case "$reply" in *[!0-9]*) echo "not a number" >&2; exit 1 ;; esac
        if [ "$reply" -lt 1 ] || [ "$reply" -gt "${#FEATURES[@]}" ]; then
            echo "out of range" >&2; exit 1
        fi
        FEAT="${FEATURES[$((reply-1))]}"
    fi
fi

echo "Installing the '$FEAT' build."
[ "$CHOOSE" = 0 ] && [ "$ASSUME_YES" != 1 ] &&
    echo "(rerun with --choose to pick AVX2/OpenMP/CUDA yourself)"

SRC="$APP $VERSION linux-$ARCH $FEAT"

# The desktop UI is built per variant, exactly like the solver, and each one
# lives in ui/<feature>/. So the question comes after the feature is settled:
# asking earlier would mean offering a UI that may not exist for the build the
# user is about to get, which is the same lie the feature boxes avoid.
UI_SRC="ui/$ARCH/$FEAT"
if [ ! -d "$UI_SRC" ]; then
    [ "$WANT_UI" = 1 ] && echo "note: this installer carries no UI for the '$FEAT' build."
    WANT_UI=0
elif [ -z "$WANT_UI" ] && [ "$ASSUME_YES" != 1 ]; then
    WANT_UI=$(ask "Install the desktop UI as well?" 1)
elif [ -z "$WANT_UI" ]; then
    WANT_UI=1
fi

# ---- where it goes ---------------------------------------------------------
# Root installs system-wide; anyone else lands in their own home, which is also
# what keeps the "output beside the executable" rule working.
if [ -z "$PREFIX" ]; then
    if [ "$(id -u)" = 0 ]; then PREFIX=/opt/fluid-solver
    else PREFIX="$HOME/.local/share/fluid-solver"; fi
    [ "$ASSUME_YES" != 1 ] && {
        read -r -p "Install to [$PREFIX]: " reply </dev/tty || reply=""
        [ -n "$reply" ] && PREFIX="$reply"
    }
fi

if ! mkdir -p "$PREFIX" 2>/dev/null; then
    echo "Cannot create $PREFIX. Run with sudo, or pass --prefix=<a writable path>." >&2
    exit 1
fi

HAVE_UI=0
# Whether the UI archive already brought the solver with it. The destination is
# not the thing to ask: a second run over an existing prefix always finds a
# "Fluid Solver" there, which is how unticking AVX2 used to leave the old AVX2
# binary in place while the installer reported the plain one - and the next run
# still died with an illegal instruction.
SOLVER_FROM_UI=0
if [ "$WANT_UI" = 1 ] && [ -d "$UI_SRC" ]; then
    # The ui/<feature>/ folder is a complete install: the solver, the UI, the
    # shared objects and output/ together, because the UI is a shell that starts
    # the solver and they have to live in one directory. So it goes in INSTEAD
    # of the bare solver file - copying one over the other would put the same
    # binary down twice and leave its mode depending on which copy ran last.
    cp -r "$UI_SRC"/. "$PREFIX/"
    [ -f "$UI_SRC/Fluid Solver" ] && SOLVER_FROM_UI=1
    chmod 0755 "$PREFIX/Fluid Solver" 2>/dev/null
    [ -f "$PREFIX/Fluid Solver UI" ] && { chmod 0755 "$PREFIX/Fluid Solver UI"; HAVE_UI=1; }
fi
[ "$SOLVER_FROM_UI" = 1 ] || install -m 0755 "$SRC" "$PREFIX/Fluid Solver"
# A UI left over from an earlier run, when this one was told not to install it.
# Left alone it would keep the desktop entry, the dock favourite and the
# fluid-solver-ui symlink pointing at a UI the user just declined - and at the
# wrong variant besides.
[ "$HAVE_UI" = 0 ] && rm -f "$PREFIX/Fluid Solver UI" 2>/dev/null
mkdir -p "$PREFIX/output" "$PREFIX/models"
[ -d models ] && cp -r models/. "$PREFIX/models/" 2>/dev/null
for f in README.md LICENSE; do [ -f "$f" ] && cp "$f" "$PREFIX/"; done

# A "<variant>-ui" archive zipped on Windows carries no Unix permission bits at
# all, and everything cp lays down from it then arrives 0600 - README and
# LICENSE unreadable to anyone but the installing user, output/ at 0700 so a
# system-wide install cannot be written to by the person who has to use it. cp
# also keeps the mode a file already has, so copying the repository's own README
# over it does not undo that. The two binaries were handled above; this settles
# the rest, and costs nothing when the archive was well formed.
# output/ is pruned rather than levelled with the rest: what is in there are the
# user's own frames from an earlier run, and this is an installer.
chmod 0755 "$PREFIX" "$PREFIX/output" 2>/dev/null
find "$PREFIX" -mindepth 1 -path "$PREFIX/output" -prune -o \
     -type d -exec chmod 0755 {} + 2>/dev/null
find "$PREFIX" -mindepth 1 -path "$PREFIX/output" -prune -o \
     -type f ! -name 'Fluid Solver' ! -name 'Fluid Solver UI' \
     -exec chmod 0644 {} + 2>/dev/null
# The frames land here, and under sudo the person who ran it is the one who has
# to be able to write them, not root.
if [ "$(id -u)" = 0 ] && [ "$TARGET_USER" != root ]; then
    chown "$TARGET_USER" "$PREFIX/output" 2>/dev/null
fi

# What to take back out later. Recorded now, while it is still known: a UI
# variant carries its own shared objects and the uninstaller cannot guess their
# names. output/ and models/ are handled separately and stay out of this list.
INSTALLED=()
while IFS= read -r f; do
    INSTALLED+=("${f#./}")
done < <(cd "$PREFIX" && find . -maxdepth 1 -type f ! -name uninstall.sh -printf '%P\n' | sort)


# ---- launcher, icons, desktop entry ---------------------------------------
if [ "$(id -u)" = 0 ]; then
    BINDIR=/usr/local/bin; APPDIR=/usr/share/applications; ICONROOT=/usr/share/icons/hicolor
else
    BINDIR="$HOME/.local/bin"; APPDIR="$HOME/.local/share/applications"
    ICONROOT="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
fi
mkdir -p "$BINDIR" "$APPDIR"
ln -sf "$PREFIX/Fluid Solver" "$BINDIR/fluid-solver"
[ "$HAVE_UI" = 1 ] && ln -sf "$PREFIX/Fluid Solver UI" "$BINDIR/fluid-solver-ui"

for size in 16 22 24 32 48 64 128 256; do
    if [ -f "icons/fluid-solver-$size.png" ]; then
        mkdir -p "$ICONROOT/${size}x${size}/apps"
        cp "icons/fluid-solver-$size.png" "$ICONROOT/${size}x${size}/apps/fluid-solver.png"
    fi
done

# The same entry text is now needed in up to two places - the menu directory
# and the desktop - so it is written by one function rather than pasted twice.
# The UI is a window, so unlike the solver it does not want a terminal.
write_entry() {   # write_entry <path> <solver|ui>
    if [ "$2" = ui ]; then
        cat > "$1" <<EOF
[Desktop Entry]
Type=Application
Name=Fluid Solver UI
Comment=Configure runs and view the frames
Exec="$PREFIX/Fluid Solver UI"
Path=$PREFIX
Icon=fluid-solver
Terminal=false
Categories=Science;Physics;Education;
EOF
    else
        cat > "$1" <<EOF
[Desktop Entry]
Type=Application
Name=Fluid Solver
Comment=2D incompressible Navier-Stokes solver
Exec="$PREFIX/Fluid Solver"
Path=$PREFIX
Icon=fluid-solver
Terminal=true
Categories=Science;Physics;Education;
EOF
    fi
}

# Two separate questions, because they are two separate places: the application
# menu and the desktop itself. The dock used to be a third; it is not asked
# about any more - see the note at the end of the run.
[ -z "$WANT_MENU" ]    && WANT_MENU=$(ask "Create an application menu entry?" 1)
[ -z "$WANT_DESKTOP" ] && WANT_DESKTOP=$(ask "Put a shortcut on the desktop?" 1)

# Where the shortcuts go is one question, which program they start is another,
# and this is the second one. It is only worth asking when a UI actually went
# in: without one there is a single program to point at and nothing to choose
# between, so the console solver takes every shortcut that was asked for.
WANT_ANY_PLACE=0
[ "$WANT_MENU" = 1 ]    && WANT_ANY_PLACE=1
[ "$WANT_DESKTOP" = 1 ] && WANT_ANY_PLACE=1

if [ "$HAVE_UI" = 1 ] && [ "$WANT_ANY_PLACE" = 1 ]; then
    [ -z "$WANT_ICON_UI" ] &&
        WANT_ICON_UI=$(ask "  point them at the UI (the window)?" 1)
    [ -z "$WANT_ICON_CONSOLE" ] &&
        WANT_ICON_CONSOLE=$(ask "  point them at the console solver as well?" 0)
elif [ "$HAVE_UI" = 1 ]; then
    [ -z "$WANT_ICON_UI" ] && WANT_ICON_UI=1
    [ -z "$WANT_ICON_CONSOLE" ] && WANT_ICON_CONSOLE=0
else
    WANT_ICON_UI=0
    WANT_ICON_CONSOLE=1
fi

# The .desktop files in the menu directory. They are also what a dock favourite
# would refer to, so writing them is what makes pinning by hand possible at all
# - the note at the end of the run depends on this having happened.
NEED_ENTRY=0
[ "$WANT_MENU" = 1 ] && NEED_ENTRY=1

if [ "$NEED_ENTRY" = 1 ]; then
    if [ "$WANT_ICON_CONSOLE" = 1 ]; then
        write_entry "$APPDIR/fluid-solver.desktop" solver
        chmod 0644 "$APPDIR/fluid-solver.desktop"
    else
        rm -f "$APPDIR/fluid-solver.desktop" 2>/dev/null
    fi
    if [ "$HAVE_UI" = 1 ] && [ "$WANT_ICON_UI" = 1 ]; then
        write_entry "$APPDIR/fluid-solver-ui.desktop" ui
        chmod 0644 "$APPDIR/fluid-solver-ui.desktop"
    else
        rm -f "$APPDIR/fluid-solver-ui.desktop" 2>/dev/null
    fi
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" 2>/dev/null
    command -v gtk-update-icon-cache  >/dev/null 2>&1 && gtk-update-icon-cache -q "$ICONROOT" 2>/dev/null
fi

# ---- the desktop itself ----------------------------------------------------
# A copy of the same entry, owned by the user and marked executable and trusted
# - without that GNOME shows it as an untrusted text file and refuses to launch
# it until the user right-clicks "Allow launching".
DESKTOP_SHORTCUTS=()
place_on_desktop() {   # place_on_desktop <entry file name> <solver|ui>
    local path="$DESKTOP_DIR/$1"
    write_entry "$path" "$2"
    chmod 0755 "$path"
    chown "$TARGET_USER" "$path" 2>/dev/null
    as_target_user gio set "$path" metadata::trusted true 2>/dev/null
    DESKTOP_SHORTCUTS+=("$path")
}

if [ "$WANT_DESKTOP" = 1 ]; then
    DESKTOP_DIR="$(as_target_user xdg-user-dir DESKTOP 2>/dev/null)"
    [ -d "$DESKTOP_DIR" ] || DESKTOP_DIR="$TARGET_HOME/Desktop"
    if mkdir -p "$DESKTOP_DIR" 2>/dev/null; then
        # Whatever the second pair of questions asked for: both icons, one, or
        # - if the user unticked both - none at all.
        [ "$HAVE_UI" = 1 ] && [ "$WANT_ICON_UI" = 1 ] &&
            place_on_desktop fluid-solver-ui.desktop ui
        [ "$WANT_ICON_CONSOLE" = 1 ] &&
            place_on_desktop fluid-solver.desktop solver
        # An icon this run did not want, left over from one that did.
        [ "$WANT_ICON_UI" = 1 ] || rm -f "$DESKTOP_DIR/fluid-solver-ui.desktop" 2>/dev/null
        [ "$WANT_ICON_CONSOLE" = 1 ] || rm -f "$DESKTOP_DIR/fluid-solver.desktop" 2>/dev/null
    else
        echo "note: could not write to $DESKTOP_DIR, so no desktop shortcut."
        WANT_DESKTOP=0
    fi
fi

# ---- the taskbar -----------------------------------------------------------
# It used to try. GNOME keeps its dock in a gsettings list of desktop-file ids,
# which is scriptable; Plasma keeps its launchers inside one appletsrc file
# whose layout changes between releases; XFCE, Cinnamon and the rest each have
# their own. So on most desktops the box did nothing and printed an apology,
# and on the one where it worked it was rewriting a list that belongs to the
# user's session. Doing it by hand is one right-click and works everywhere, so
# that is what the end of this script says. The menu entry written above is
# what makes it possible.

# ---- uninstaller -----------------------------------------------------------
cat > "$PREFIX/uninstall.sh" <<EOF
#!/usr/bin/env bash
# Leaves $PREFIX/output alone: those are the user's results, not ours.
#
# Deliberately not "set -e". Almost every line here is allowed to fail - a
# shortcut the user already deleted, a gsettings schema that is not installed -
# and stopping at the first one would leave the rest of the install on disk
# with nothing said about it. Each step cleans up what it can and moves on.
set -u
rm -f "$BINDIR/fluid-solver" "$BINDIR/fluid-solver-ui" 2>/dev/null
rm -f "$APPDIR/fluid-solver.desktop" "$APPDIR/fluid-solver-ui.desktop" 2>/dev/null
$(for f in ${DESKTOP_SHORTCUTS[@]+"${DESKTOP_SHORTCUTS[@]}"}; do printf 'rm -f "%s" 2>/dev/null\n' "$f"; done)
# Take the dock favourites back out. This installer no longer puts them in,
# but 0.1 did, and an upgrade over that install would otherwise leave a
# favourite pointing at a .desktop file that is about to be deleted - which
# GNOME draws as a blank tile nobody can get rid of.
# Both entry names, whichever of them the older install actually pinned:
# removing a name that is not in the list is a no-op, and guessing wrong is not.
# The "|| true" is load-bearing on any desktop that is not GNOME: gsettings is
# installed with glib, so it is on PATH, but reading a schema it does not have
# exits non-zero.
if command -v gsettings >/dev/null 2>&1; then
    for entry in fluid-solver-ui.desktop fluid-solver.desktop; do
        cur="\$(gsettings get org.gnome.shell favorite-apps 2>/dev/null || true)"
        case "\$cur" in
            *"'\$entry'"*)
                gsettings set org.gnome.shell favorite-apps "\$(printf '%s' "\$cur" |
                    sed -e "s/'\$entry', //" \\
                        -e "s/, '\$entry'//" \\
                        -e "s/'\$entry'//")" 2>/dev/null || true ;;
        esac
    done
fi
for s in 16 22 24 32 48 64 128 256; do
    rm -f "$ICONROOT/\${s}x\${s}/apps/fluid-solver.png" 2>/dev/null
done
rm -rf "$PREFIX/models" 2>/dev/null
# Everything the install put down, listed at install time. A UI variant brings
# its own shared objects along and there is no way to guess their names later,
# so they are recorded rather than pattern-matched - and listing beats "delete
# the folder", which would be a bad idea for a prefix the user chose.
while IFS= read -r f; do
    [ -n "\$f" ] && rm -f "$PREFIX/\$f" 2>/dev/null
done <<'MANIFEST'
$(printf '%s\n' "${INSTALLED[@]}")
MANIFEST
rm -f "$PREFIX/uninstall.sh" 2>/dev/null
rmdir "$PREFIX" 2>/dev/null   # only if output/ was empty and the user took it
echo "Removed. Your results are still in $PREFIX/output"
EOF
chmod 0755 "$PREFIX/uninstall.sh"

echo
echo "Installed the linux-$ARCH $FEAT build to $PREFIX"
[ "$HAVE_UI" = 1 ] && echo "  the desktop UI went in beside it"
if [ "$WANT_MENU" = 1 ]; then
    [ "$HAVE_UI" = 1 ] && [ "$WANT_ICON_UI" = 1 ] && echo "  menu entry:    $APPDIR/fluid-solver-ui.desktop"
    [ "$WANT_ICON_CONSOLE" = 1 ] && echo "  menu entry:    $APPDIR/fluid-solver.desktop"
fi
for f in ${DESKTOP_SHORTCUTS[@]+"${DESKTOP_SHORTCUTS[@]}"}; do
    echo "  desktop icon:  $f"
done
echo "  run it with:   fluid-solver          (if $BINDIR is on your PATH)"
echo "  frames go to:  $PREFIX/output"
echo "  uninstall:     $PREFIX/uninstall.sh"

# What the "add it to the taskbar" box used to promise, said plainly instead.
if [ "$WANT_MENU" = 1 ]; then
    if [ "$HAVE_UI" = 1 ] && [ "$WANT_ICON_UI" = 1 ]; then PIN_NAME="Fluid Solver UI"
    else PIN_NAME="Fluid Solver"; fi
    echo
    echo "One thing this installer no longer tries to do for you: the taskbar."
    echo "Open the application menu, find \"$PIN_NAME\", right-click it and choose"
    echo "\"Pin to Dash\", \"Add to Favourites\" or \"Add to Panel\", whichever your"
    echo "desktop calls it. One click, and it works on all of them - which is more"
    echo "than the box that used to be here could say."
fi
