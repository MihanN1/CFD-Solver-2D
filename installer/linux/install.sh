#!/usr/bin/env bash
# Payload script of the self-extracting Linux installer.
#
# makeself runs this from inside the unpacked archive, so every variant file
# is right here in the working directory. Exactly one of them gets installed as
# "Fluid Solver", so nothing downstream has to know which.
#
# Interactive:   ./Fluid-Solver-<ver>-linux-<arch>.run
# Unattended:    ./Fluid-Solver-<ver>-linux-<arch>.run -- --avx2 --openmp --cuda \
#                    --ui --prefix=/opt/fluid-solver --shortcut --yes

set -uo pipefail

APP="Fluid Solver"
VERSION="__VERSION__"
ARCH="__ARCH__"

PREFIX=""
WANT_AVX2=""
WANT_OMP=""
WANT_CUDA=""
WANT_UI=""
SHORTCUT=""
ASSUME_YES=0

for arg in "$@"; do
    case "$arg" in
        --prefix=*)  PREFIX="${arg#--prefix=}" ;;
        --avx2)      WANT_AVX2=1 ;;
        --no-avx2)   WANT_AVX2=0 ;;
        --openmp)    WANT_OMP=1 ;;
        --no-openmp) WANT_OMP=0 ;;
        --cuda)      WANT_CUDA=1 ;;
        --no-cuda)   WANT_CUDA=0 ;;
        --ui)        WANT_UI=1 ;;
        --no-ui)     WANT_UI=0 ;;
        --shortcut)  SHORTCUT=1 ;;
        --no-shortcut) SHORTCUT=0 ;;
        --yes|-y)    ASSUME_YES=1 ;;
        --help|-h)
            sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

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
    echo "This installer carries no linux-$ARCH build at all - it was packed from" >&2
    echo "an empty dist/. Please report it." >&2
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

if [ "$ASSUME_YES" != 1 ]; then
    echo
    varies 0 && WANT_AVX2=$(ask "Use the AVX2 build?"   "$WANT_AVX2")
    varies 1 && WANT_OMP=$(ask  "Use the OpenMP build?" "$WANT_OMP")
    varies 2 && WANT_CUDA=$(ask "Use the CUDA build?"   "$WANT_CUDA")
fi

# The desktop UI is a separate program and only some platforms have one, so the
# question exists only when the payload actually carries it.
if [ ! -d ui ]; then
    WANT_UI=0
elif [ -z "$WANT_UI" ]; then
    WANT_UI=$(ask "Install the desktop UI as well?" 1)
fi

if [ "$WANT_AVX2" = 1 ] && [ "$HAS_AVX2" = 0 ]; then
    echo "This CPU has no AVX2; that build would die with an illegal instruction." >&2
    exit 1
fi
if [ "$WANT_CUDA" = 1 ] && [ "$HAS_NVIDIA" = 0 ]; then
    echo "note: no NVIDIA driver here, so the CUDA build will run on the CPU anyway."
fi

FEAT="$(feature_of "$WANT_AVX2" "$WANT_OMP" "$WANT_CUDA")"
if ! have_combo "$WANT_AVX2$WANT_OMP$WANT_CUDA"; then
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

SRC="$APP $VERSION linux-$ARCH $FEAT"

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

install -m 0755 "$SRC" "$PREFIX/Fluid Solver"
mkdir -p "$PREFIX/output" "$PREFIX/models"
[ -d models ] && cp -r models/. "$PREFIX/models/" 2>/dev/null
for f in README.md LICENSE; do [ -f "$f" ] && cp "$f" "$PREFIX/"; done

HAVE_UI=0
if [ "$WANT_UI" = 1 ] && [ -d ui ]; then
    cp -r ui/. "$PREFIX/" 2>/dev/null
    [ -f "$PREFIX/Fluid Solver UI" ] && { chmod 0755 "$PREFIX/Fluid Solver UI"; HAVE_UI=1; }
fi

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

[ -z "$SHORTCUT" ] && SHORTCUT=$(ask "Create an application menu entry?" 1)
if [ "$SHORTCUT" = 1 ]; then
    cat > "$APPDIR/fluid-solver.desktop" <<EOF
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
    chmod 0644 "$APPDIR/fluid-solver.desktop"
    # The UI is a window, so unlike the solver it does not want a terminal.
    if [ "$HAVE_UI" = 1 ]; then
        cat > "$APPDIR/fluid-solver-ui.desktop" <<EOF
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
        chmod 0644 "$APPDIR/fluid-solver-ui.desktop"
    fi
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" 2>/dev/null
    command -v gtk-update-icon-cache  >/dev/null 2>&1 && gtk-update-icon-cache -q "$ICONROOT" 2>/dev/null
fi

# ---- uninstaller -----------------------------------------------------------
cat > "$PREFIX/uninstall.sh" <<EOF
#!/usr/bin/env bash
# Leaves $PREFIX/output alone: those are the user's results, not ours.
set -e
rm -f "$BINDIR/fluid-solver" "$BINDIR/fluid-solver-ui"
rm -f "$APPDIR/fluid-solver.desktop" "$APPDIR/fluid-solver-ui.desktop"
for s in 16 22 24 32 48 64 128 256; do
    rm -f "$ICONROOT/\${s}x\${s}/apps/fluid-solver.png"
done
rm -f "$PREFIX/Fluid Solver" "$PREFIX/Fluid Solver UI" "$PREFIX/README.md" "$PREFIX/LICENSE"
rm -rf "$PREFIX/models"
echo "Removed. Your results are still in $PREFIX/output"
EOF
chmod 0755 "$PREFIX/uninstall.sh"

echo
echo "Installed the $FEAT build to $PREFIX"
[ "$HAVE_UI" = 1 ] && echo "  the desktop UI went in beside it"
echo "  run it with:   fluid-solver          (if $BINDIR is on your PATH)"
echo "  frames go to:  $PREFIX/output"
echo "  uninstall:     $PREFIX/uninstall.sh"
