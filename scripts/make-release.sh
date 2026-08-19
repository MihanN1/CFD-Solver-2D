#!/usr/bin/env bash
# One command. It checks the toolchain, installs what is missing, builds every
# row this machine can produce and assembles release/<version>.
#
#     bash scripts/make-release.sh 0.1
#
# Called through bash on purpose: a checkout that came from Windows has no
# executable bit on it, and this way that does not matter.
#
# The whole matrix gets built: each of the three switches is turned on and off
# on its own, because a binary cannot decide at runtime whether it may execute
# an AVX2 instruction - it just dies - and because "with or without" is what
# the installers offer.
#
#     linux-x64      AVX2 {on,off} x OpenMP {on,off} x CUDA {on,off} = 8
#     linux-x86      AVX2 {on,off} x OpenMP {on,off}                 = 4
#     macos-arm64    OpenMP {on,off}                                 = 2
#     macos-x64      AVX2 {on,off} x OpenMP {on,off}                 = 4
#
# There is no 32-bit CUDA and has not been since CUDA 9, and no CUDA on macOS
# at all. One Mac builds both macOS architectures: the second one is a cross
# build, and the libomp it needs is fetched as a Homebrew bottle rather than
# installed. "plain" - no AVX2, no OpenMP, no CUDA - is the row that runs on
# anything, and on Linux the only fully static one, because libcudart_static
# needs dlopen and pthread and so rules -static out.
#
#     --docker           build the Linux rows inside an old-glibc container, so
#                        they also run on distributions older than this one
#     --with-installers  also build the .run / .pkg
#     --no-deps          do not install anything, use what is already here
#     --skip-32          skip the 32-bit rows
#     --skip-cuda        skip the CUDA rows
#     --only=<step>      all | build | installers | package
#
# Rows whose prerequisites are missing are reported and skipped, never failed.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# -h has to be caught before the version is read, or it is taken as the version
# number and the run happily assembles release/--help.
case "${1:-}" in
    -h|--help) sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

VERSION="${1:-0.1}"; shift 2>/dev/null || true

DIST="$REPO/dist"
REL="$REPO/release/$VERSION"
LOGS="$REPO/logs"
# One toolchain directory per system, not one for the whole repository. A venv
# hard-codes absolute paths and symlinks its interpreter, so the copy built
# inside a container is worthless on the host and the other way round - sharing
# a single directory is how it ends up pointing at a python that is not there.
# The distribution and its version are part of the name because the host and
# the container are both linux-x86_64 and would otherwise collide.
toolchain_tag() {
    local os
    case "$(uname -s)" in
        Darwin) os="macos-$(sw_vers -productVersion 2>/dev/null | cut -d. -f1)" ;;
        *)      if [ -r /etc/os-release ]; then
                    os="$( . /etc/os-release 2>/dev/null; printf '%s-%s' "${ID:-linux}" "${VERSION_ID:-0}" )"
                else
                    os=linux
                fi ;;
    esac
    printf '%s-%s' "$os" "$(uname -m)" | tr -c 'A-Za-z0-9._-' '-'
}
TOOLCHAIN="$REPO/.toolchain/$(toolchain_tag)"

SKIP_32=0; SKIP_CUDA=0; ONLY=all; WITH_INSTALLERS=0; NO_DEPS=0; USE_DOCKER=0
CUDA_ARCHS="${CUDA_ARCHS:-}"
DOCKER_IMAGE="${DOCKER_IMAGE:-nvidia/cuda:12.6.3-devel-ubuntu20.04}"

for arg in "$@"; do
    case "$arg" in
        --skip-32)          SKIP_32=1 ;;
        --skip-cuda)        SKIP_CUDA=1 ;;
        --with-installers)  WITH_INSTALLERS=1 ;;
        --no-deps)          NO_DEPS=1 ;;
        --docker)           USE_DOCKER=1 ;;
        --only=*)           ONLY="${arg#--only=}" ;;
        --cuda-archs=*)     CUDA_ARCHS="${arg#--cuda-archs=}" ;;
        --docker-image=*)   DOCKER_IMAGE="${arg#--docker-image=}" ;;
        -h|--help)          sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

case "$ONLY" in
    all|build|installers|package) ;;
    *) echo "--only= takes all, build, installers or package" >&2; exit 2 ;;
esac

# The count is kept by hand because macOS still ships bash 3.2, where "set -u"
# treats an empty array as unset and ${#PROBLEMS[@]} aborts the script.
PROBLEMS=(); PROBLEM_COUNT=0
note() { PROBLEMS+=("$1"); PROBLEM_COUNT=$((PROBLEM_COUNT + 1)); }

case "$(uname -s)" in
    Darwin) PLATFORM=macos; HOSTARCH="$(uname -m)"; [ "$HOSTARCH" = "x86_64" ] && HOSTARCH=x64 ;;
    *)      PLATFORM=linux; HOSTARCH=x64 ;;
esac

# Filled in by prepare_macos_openmp; empty means that architecture gets no
# OpenMP rows rather than rows that quietly have no OpenMP in them.
OMPROOT_ARM64=""
OMPROOT_X64=""
OMP_SCRATCH=""
cleanup() { [ -n "$OMP_SCRATCH" ] && rm -rf "$OMP_SCRATCH"; }
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
step() { printf '\n%s\n' "$*"; }
item() { printf '  %-34s' "$1"; }

version_ge() {   # version_ge A B -> true when A >= B
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]
}

# ---- toolchain -------------------------------------------------------------
SUDO=""
if [ "$(id -u)" != 0 ]; then
    command -v sudo >/dev/null 2>&1 && SUDO=sudo
fi

APT_UPDATED=0
apt_install() {   # apt_install <packages...>; echoes nothing when all present
    local want missing=""
    for want in "$@"; do
        dpkg -s "$want" >/dev/null 2>&1 || missing="$missing $want"
    done
    [ -z "$missing" ] && return 0
    [ "$NO_DEPS" = 1 ] && { echo "--no-deps was given, so$missing was not installed"; return 1; }
    if [ -z "$SUDO" ] && [ "$(id -u)" != 0 ]; then
        echo "no sudo here, so install$missing yourself"; return 1
    fi
    if [ "$APT_UPDATED" = 0 ]; then
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get update -qq >/dev/null 2>&1
        APT_UPDATED=1
    fi
    # shellcheck disable=SC2086
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq $missing >/dev/null 2>&1 \
        || { echo "apt could not install$missing - check the network and the sources"; return 1; }
    return 0
}

brew_install() {  # brew_install <formula>
    command -v brew >/dev/null 2>&1 || { echo "no homebrew"; return 1; }
    brew list --formula "$1" >/dev/null 2>&1 && return 0
    [ "$NO_DEPS" = 1 ] && { echo "missing: $1"; return 1; }
    brew install "$1" >/dev/null 2>&1 || { echo "brew install $1 failed"; return 1; }
    return 0
}

cmake_version() {
    cmake --version 2>/dev/null | head -1 | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p'
}

# CMake 3.28 is what the project asks for and what distributions older than the
# binaries are meant to run on do not have. A virtualenv keeps the newer one
# inside the repo instead of on top of the system copy.
ensure_cmake() {
    local v
    v="$(cmake_version)"
    if [ -n "$v" ] && version_ge "$v" 3.28; then say "cmake $v"; return 0; fi

    # Being executable is not the same as working: a venv left behind by another
    # machine still has a cmake in it, and its shebang points at an interpreter
    # that is not on this one. Trust the version it prints, nothing less.
    if [ -x "$TOOLCHAIN/venv/bin/cmake" ]; then
        PATH="$TOOLCHAIN/venv/bin:$PATH"; export PATH
        v="$(cmake_version)"
        if [ -n "$v" ] && version_ge "$v" 3.28; then say "cmake $v (local)"; return 0; fi
        PATH="${PATH#"$TOOLCHAIN/venv/bin:"}"; export PATH
        rm -rf "$TOOLCHAIN/venv"
    fi
    if [ "$NO_DEPS" = 1 ]; then
        say "${v:-missing} - too old, need 3.28"; note "cmake is older than 3.28"; return 1
    fi

    [ "$PLATFORM" = linux ] && apt_install python3 python3-venv python3-pip >/dev/null 2>&1
    command -v python3 >/dev/null 2>&1 || { say "no python3 to install it with"; note "cmake >= 3.28 is missing and python3 is not here to install it"; return 1; }

    mkdir -p "$TOOLCHAIN"
    if python3 -m venv "$TOOLCHAIN/venv" >/dev/null 2>&1 &&
       "$TOOLCHAIN/venv/bin/pip" install -q --upgrade pip >/dev/null 2>&1 &&
       "$TOOLCHAIN/venv/bin/pip" install -q "cmake>=3.28" >/dev/null 2>&1 &&
       [ -x "$TOOLCHAIN/venv/bin/cmake" ]
    then
        PATH="$TOOLCHAIN/venv/bin:$PATH"; export PATH
        say "cmake $(cmake_version) (installed locally)"; return 0
    fi
    say "could not install one"; note "cmake >= 3.28 could not be installed"; return 1
}

HAVE_M32=0
probe_m32() {
    echo 'int main(){return 0;}' > /tmp/.m32probe.c
    if gcc -m32 /tmp/.m32probe.c -o /tmp/.m32probe 2>/dev/null; then HAVE_M32=1; else HAVE_M32=0; fi
    rm -f /tmp/.m32probe /tmp/.m32probe.c
}

HAVE_NVCC=0
probe_nvcc() {
    command -v nvcc >/dev/null 2>&1 && { HAVE_NVCC=1; return; }
    # Installers put it here and leave PATH alone.
    for p in /usr/local/cuda/bin /opt/cuda/bin; do
        [ -x "$p/nvcc" ] && { PATH="$p:$PATH"; export PATH; HAVE_NVCC=1; return; }
    done
    HAVE_NVCC=0
}

ensure_tools_linux() {
    step "Checking the toolchain"

    item "compiler"
    if command -v g++ >/dev/null 2>&1; then say "g++ $(g++ -dumpversion)"
    else
        local why; why="$(apt_install build-essential)"
        if command -v g++ >/dev/null 2>&1; then say "g++ $(g++ -dumpversion) (installed)"
        else say "missing"; note "nothing can be built - ${why:-install build-essential}"; fi
    fi

    item "cmake"; ensure_cmake

    item "zip, file, binutils"
    if command -v zip >/dev/null 2>&1 && command -v file >/dev/null 2>&1 && command -v strip >/dev/null 2>&1; then
        say "ok"
    else
        local why; why="$(apt_install zip file binutils)"
        if command -v zip >/dev/null 2>&1 && command -v file >/dev/null 2>&1; then say "ok (installed)"
        else say "missing"; note "packaging will be skipped - ${why:-zip/file/binutils are missing}"; fi
    fi

    item "32-bit (multilib)"
    if [ "$SKIP_32" = 1 ]; then say "skipped by --skip-32"
    else
        local why=""
        probe_m32
        if [ "$HAVE_M32" = 0 ]; then
            why="$(apt_install gcc-multilib g++-multilib)"
            probe_m32
        fi
        if [ "$HAVE_M32" = 1 ]; then say "ok"
        else
            say "not available - the four 32-bit rows will be skipped"
            note "linux-x86 rows skipped - ${why:-install gcc-multilib g++-multilib}"
        fi
    fi

    item "nvcc (CUDA)"
    if [ "$SKIP_CUDA" = 1 ]; then say "skipped by --skip-cuda"; HAVE_NVCC=0
    else
        probe_nvcc
        if [ "$HAVE_NVCC" = 1 ]; then say "$(nvcc --version | sed -n 's/.*release \([0-9.]*\).*/CUDA \1/p' | head -1)"
        else
            say "not here - the four CUDA rows will be skipped"
            note "linux-x64 CUDA rows skipped - no nvcc; install the CUDA Toolkit, or use --docker"
        fi
    fi

    if [ "$WITH_INSTALLERS" = 1 ]; then
        item "makeself"
        if command -v makeself >/dev/null 2>&1; then say "ok"
        else
            apt_install makeself >/dev/null
            command -v makeself >/dev/null 2>&1 && say "ok (installed)" || {
                say "missing"; note "linux installer skipped - makeself is not installed"; }
        fi
    fi
}

# Homebrew ships libomp per architecture. The host one is installed normally;
# the other one is fetched as a bottle and unpacked into a scratch directory,
# which is all a cross build needs and avoids a second Homebrew under Rosetta.
# The static archive is preferred so the result does not depend on that copy
# still being on the machine that runs it.
omp_root_from() {   # omp_root_from <prefix> -> echoes it when usable
    local p="$1"
    [ -n "$p" ] || return 1
    [ -f "$p/include/omp.h" ] || return 1
    { [ -f "$p/lib/libomp.a" ] || [ -f "$p/lib/libomp.dylib" ]; } || return 1
    printf '%s' "$p"
}

fetch_libomp_bottle() {   # fetch_libomp_bottle <tag...> -> echoes an unpacked prefix
    command -v brew >/dev/null 2>&1 || return 1
    local tag path out
    for tag in "$@"; do
        brew fetch --force --quiet --bottle-tag="$tag" libomp >/dev/null 2>&1 || continue
        path="$(brew --cache --bottle-tag="$tag" libomp 2>/dev/null)"
        [ -n "$path" ] && [ -f "$path" ] || continue
        rm -rf "$OMP_SCRATCH/$tag"; mkdir -p "$OMP_SCRATCH/$tag"
        tar -xf "$path" -C "$OMP_SCRATCH/$tag" 2>/dev/null || continue
        for out in "$OMP_SCRATCH/$tag"/libomp/*/; do
            omp_root_from "${out%/}" && return 0
        done
    done
    return 1
}

prepare_macos_openmp() {
    # Created here rather than inside fetch_libomp_bottle: that one runs in a
    # command substitution, so anything it assigns is lost with the subshell and
    # the trap would never clean it up.
    OMP_SCRATCH="$(mktemp -d)"
    local hostroot=""
    if brew_install libomp >/dev/null 2>&1; then
        hostroot="$(omp_root_from "$(brew --prefix libomp 2>/dev/null)")"
    fi

    if [ "$HOSTARCH" = arm64 ]; then
        OMPROOT_ARM64="$hostroot"
        OMPROOT_X64="$(fetch_libomp_bottle monterey ventura sonoma sequoia)"
    else
        OMPROOT_X64="$hostroot"
        OMPROOT_ARM64="$(fetch_libomp_bottle arm64_monterey arm64_ventura arm64_sonoma arm64_sequoia)"
    fi
}

ensure_tools_macos() {
    step "Checking the toolchain"

    item "Xcode command line tools"
    if xcode-select -p >/dev/null 2>&1; then say "ok"
    else
        say "missing - run: xcode-select --install"
        note "Xcode command line tools are missing - nothing can be built without them"
        return
    fi

    item "cmake"; ensure_cmake

    item "zip, file"
    command -v zip >/dev/null 2>&1 && say "ok" || { say "missing"; note "zip is missing - packaging will be skipped"; }

    item "libomp (host $HOSTARCH)"
    prepare_macos_openmp
    local hostroot other othername
    if [ "$HOSTARCH" = arm64 ]; then hostroot="$OMPROOT_ARM64"; other="$OMPROOT_X64"; othername="x64"
    else hostroot="$OMPROOT_X64"; other="$OMPROOT_ARM64"; othername="arm64"; fi

    if [ -n "$hostroot" ]; then say "$hostroot"
    else
        say "missing - the host OpenMP rows will be skipped"
        note "libomp for $HOSTARCH is missing - install Homebrew and rerun, or run: brew install libomp"
    fi

    item "libomp (cross $othername)"
    if [ -n "$other" ]; then say "bottle unpacked"
    else
        say "not available - the $othername OpenMP rows will be skipped"
        note "no libomp bottle for $othername - its OpenMP rows were skipped"
    fi
}

ensure_tools() {
    [ "$PLATFORM" = macos ] && { ensure_tools_macos; return; }
    if command -v apt-get >/dev/null 2>&1; then
        ensure_tools_linux
    else
        step "Checking the toolchain"
        say "  This is not a Debian-style system, so nothing is installed automatically."
        say "  Make sure these are present: a C++ compiler, cmake >= 3.28, zip, file,"
        say "  binutils, 32-bit libraries (glibc-devel.i686 / lib32-gcc-libs), and nvcc"
        say "  for the CUDA rows."
        item "cmake"; ensure_cmake
        [ "$SKIP_32" = 0 ] && probe_m32
        [ "$SKIP_CUDA" = 0 ] && probe_nvcc
    fi
}

# ---- CUDA architectures ----------------------------------------------------
# CUDA 13 dropped Maxwell, Pascal and Volta, and nvcc errors out rather than
# warning when asked for one of them, so the list follows the toolkit.
resolve_cuda_archs() {
    [ -n "$CUDA_ARCHS" ] && { echo "$CUDA_ARCHS"; return; }
    local major
    major="$(nvcc --version 2>/dev/null | sed -n 's/.*release \([0-9]\+\)\..*/\1/p' | head -1)"
    if [ -n "$major" ] && [ "$major" -lt 13 ] 2>/dev/null; then
        echo "50;60;61;70;75;80;86;89;90"
    else
        echo "75;80;86;89;90"
    fi
}

# ---- one row ---------------------------------------------------------------
build_row() {
    local arch="$1" avx2="$2" omp="$3" cuda="$4" archs="$5"
    local tags=""
    [ "$avx2" = ON ] && tags="${tags}avx2-"
    [ "$omp"  = ON ] && tags="${tags}omp-"
    [ "$cuda" = ON ] && tags="${tags}cuda-"
    tags="${tags%-}"; [ -z "$tags" ] && tags="plain"

    local label="$PLATFORM-$arch"
    local name="Fluid Solver $VERSION $label $tags"
    local build="$REPO/build-$label-$tags"
    local log="$LOGS/$label-$tags.log"
    mkdir -p "$LOGS"
    printf '  %-52s' "$name"
    rm -rf "$build"

    local args=(-S "$REPO" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCFD_STATIC=ON
                -DCFD_ENABLE_AVX2="$avx2" -DCFD_ENABLE_OPENMP="$omp" -DCFD_ENABLE_CUDA="$cuda")
    # Without these a missing toolkit or runtime quietly yields a binary that
    # would then be published under a name promising the feature it lacks.
    [ "$cuda" = ON ] && args+=(-DCFD_ENABLE_CUDA_EXPLICIT=ON -DCFD_CUDA_ARCHITECTURES="$archs")
    [ "$omp" = ON ] && args+=(-DCFD_ENABLE_OPENMP_EXPLICIT=ON)
    [ "$arch" = "x86" ] && args+=(-DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32)

    if [ "$PLATFORM" = macos ]; then
        local osxarch omproot
        if [ "$arch" = arm64 ]; then osxarch=arm64; omproot="$OMPROOT_ARM64"
        else osxarch=x86_64; omproot="$OMPROOT_X64"; fi
        args+=(-DCMAKE_OSX_ARCHITECTURES="$osxarch"
               -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}")
        [ "$omp" = ON ] && [ -n "$omproot" ] && args+=(-DCFD_OPENMP_ROOT="$omproot")
    fi

    if ! cmake "${args[@]}" > "$log" 2>&1; then
        say "configure failed, logs/$label-$tags.log"; note "$name - configure failed"; return
    fi
    local jobs; jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    if ! cmake --build "$build" -j"$jobs" >> "$log" 2>&1; then
        say "build failed, logs/$label-$tags.log"; note "$name - build failed"; return
    fi

    local exe; exe="$(find "$build/bin" -type f -name 'Fluid Solver' 2>/dev/null | head -1)"
    if [ -z "$exe" ]; then say "no executable"; note "$name - no executable produced"; return; fi

    mkdir -p "$DIST"
    cp "$exe" "$DIST/$name"
    if [ "$PLATFORM" = macos ]; then strip -x "$DIST/$name" 2>/dev/null
    else strip "$DIST/$name" 2>/dev/null; fi
    say "ok, $(du -h "$DIST/$name" | cut -f1), $(file -b "$DIST/$name" | grep -o 'statically linked\|dynamically linked' || echo 'Mach-O')"
}

# ---- the rows --------------------------------------------------------------
build_all() {
    local archs; archs="$(resolve_cuda_archs)"
    local avx2 omp cuda

    if [ "$PLATFORM" = macos ]; then
        local rows=6
        [ -z "$OMPROOT_ARM64" ] && rows=$((rows - 1))
        [ -z "$OMPROOT_X64" ] && rows=$((rows - 2))
        step "Building the executables ($rows rows)"
        # No CUDA on macOS at all, and arm64 has no AVX2 either.
        for omp in ON OFF; do
            [ "$omp" = ON ] && [ -z "$OMPROOT_ARM64" ] && continue
            build_row arm64 OFF "$omp" OFF "$archs"
        done
        for avx2 in ON OFF; do
            for omp in ON OFF; do
                [ "$omp" = ON ] && [ -z "$OMPROOT_X64" ] && continue
                build_row x64 "$avx2" "$omp" OFF "$archs"
            done
        done
        return
    fi

    local rows=4
    [ "$HAVE_NVCC" = 1 ] && rows=$((rows + 4))
    [ "$SKIP_32" = 0 ] && [ "$HAVE_M32" = 1 ] && rows=$((rows + 4))
    step "Building the executables ($rows rows)"
    [ "$HAVE_NVCC" = 1 ] && say "  CUDA architectures: $archs"

    for avx2 in ON OFF; do
        for omp in ON OFF; do
            for cuda in ON OFF; do
                [ "$cuda" = ON ] && [ "$HAVE_NVCC" = 0 ] && continue
                build_row x64 "$avx2" "$omp" "$cuda" "$archs"
            done
            if [ "$SKIP_32" = 0 ] && [ "$HAVE_M32" = 1 ]; then
                build_row x86 "$avx2" "$omp" OFF "$archs"
            fi
        done
    done
}

# ---- installer -------------------------------------------------------------
build_installer() {
    step "Building the installer"
    if [ "$PLATFORM" = macos ]; then
        if [ -f "$REPO/installer/macos/build-pkg.sh" ]; then
            for a in arm64 x64; do
                ls "$DIST"/"Fluid Solver $VERSION macos-$a "* >/dev/null 2>&1 || continue
                bash "$REPO/installer/macos/build-pkg.sh" "$VERSION" "$a" \
                    || note "macos-$a installer - build-pkg.sh failed"
            done
        else
            note "macos installer - installer/macos/build-pkg.sh is missing"
        fi
        return
    fi
    if ! command -v makeself >/dev/null 2>&1; then
        say "  makeself is not installed - skipped"
        note "linux installer - makeself is not installed"; return
    fi
    for arch in x64 x86; do
        [ "$arch" = x86 ] && [ "$SKIP_32" = 1 ] && continue
        ls "$DIST"/"Fluid Solver $VERSION linux-$arch "* >/dev/null 2>&1 || {
            printf '  %-52s' "linux-$arch"; say "nothing to package - skipped"; continue; }
        printf '  %-52s' "linux-$arch"
        local pkg; pkg="$(mktemp -d)"
        cp "$DIST"/"Fluid Solver $VERSION linux-$arch "* "$pkg/"
        for f in README.md LICENSE; do [ -f "$REPO/$f" ] && cp "$REPO/$f" "$pkg/"; done
        [ -d "$REPO/models" ] && cp -r "$REPO/models" "$pkg/" 2>/dev/null
        mkdir -p "$pkg/icons"; cp "$REPO"/logo/fluid-solver-*.png "$pkg/icons/" 2>/dev/null
        # install.sh offers the UI only when this folder made it into the
        # payload, so a platform without a UI build simply never asks.
        [ -d "$DIST/ui-linux-$arch" ] && { mkdir -p "$pkg/ui"; cp -r "$DIST/ui-linux-$arch"/. "$pkg/ui/"; }
        sed -e "s/__VERSION__/$VERSION/" -e "s/__ARCH__/$arch/" \
            "$REPO/installer/linux/install.sh" > "$pkg/install.sh"
        chmod +x "$pkg/install.sh"
        if makeself --quiet "$pkg" "$DIST/Fluid-Solver-$VERSION-linux-$arch.run" \
                    "Fluid Solver $VERSION" ./install.sh >/dev/null 2>&1
        then say "ok"; else say "failed"; note "linux-$arch installer - makeself failed"; fi
        rm -rf "$pkg"
    done
}

# ---- release folder --------------------------------------------------------
package() {
    step "Assembling release/$VERSION"
    command -v zip >/dev/null 2>&1 || { say "  zip is not installed"; note "packaging skipped - zip is not installed"; return; }
    rm -rf "$REL"; mkdir -p "$REL"

    shopt -s nullglob
    for entry in "$DIST"/"Fluid Solver $VERSION "*; do
        local base; base="$(basename "$entry")"
        case "$base" in *.zip|*.pkg|*setup.exe|*.run) continue ;; esac
        local stage; stage="$(mktemp -d)"
        mkdir -p "$stage/$base"
        if [ -d "$entry" ]; then cp -r "$entry"/. "$stage/$base/"
        else cp "$entry" "$stage/$base/Fluid Solver"; chmod +x "$stage/$base/Fluid Solver"; fi
        for f in README.md LICENSE; do [ -f "$REPO/$f" ] && cp "$REPO/$f" "$stage/$base/"; done
        mkdir -p "$stage/$base/output"
        ( cd "$stage" && zip -qr9 "$REL/$base.zip" "$base" )
        rm -rf "$stage"; say "  $base.zip"
    done
    for f in "$DIST"/*setup.exe "$DIST"/*.pkg "$DIST"/*.run; do
        cp "$f" "$REL/"; say "  $(basename "$f")"
    done
    shopt -u nullglob

    local stage; stage="$(mktemp -d)"
    mkdir -p "$stage/Fluid-Solver-Source-Code"
    tar -C "$REPO" \
        --exclude='./.git' --exclude='./.github' --exclude='./.vs' --exclude='./.vscode' \
        --exclude='./build*' --exclude='./out' --exclude='./dist' --exclude='./release' \
        --exclude='./bt-*' --exclude='./_to_delete' --exclude='./lib/sfml' \
        --exclude='./.toolchain' --exclude='./logs' \
        --exclude='./output/*.vtk' \
        -cf - . | tar -C "$stage/Fluid-Solver-Source-Code" -xf -
    ( cd "$stage" && zip -qr9 "$REL/Fluid-Solver-Source-Code.zip" "Fluid-Solver-Source-Code" )
    rm -rf "$stage"; say "  Fluid-Solver-Source-Code.zip"

    cp "$REPO/README.md" "$REL/README.md"; say "  README.md"

    # Written outside and moved in: created in place, the file already exists
    # when the glob runs and ends up listing a checksum of itself, taken halfway
    # through being written.
    local sums; sums="$(mktemp)"
    if command -v sha256sum >/dev/null 2>&1; then
        ( cd "$REL" && sha256sum -- * ) > "$sums" 2>/dev/null
    else
        ( cd "$REL" && shasum -a 256 -- * ) > "$sums" 2>/dev/null
    fi
    mv "$sums" "$REL/SHA256SUMS.txt"; chmod 0644 "$REL/SHA256SUMS.txt"
    say "  SHA256SUMS.txt"
}

# ---- the container route ---------------------------------------------------
# A statically linked glibc binary still records the kernel it was built
# against, and the CUDA rows cannot be static at all, so rows built on a
# current distribution refuse to start on an older one. One old image with the
# CUDA toolkit already in it covers every Linux row at once.
run_in_docker() {
    command -v docker >/dev/null 2>&1 || {
        say "docker is not installed. Install it, or drop --docker and build natively." >&2; exit 1; }
    case "$REPO" in
        /mnt/*) say "note: $REPO is on a Windows drive - the build will be slow and the"
                say "      container may not be able to write there. Copy the repository into"
                say "      the Linux filesystem first." ;;
    esac

    local inner="--only=build"
    [ "$SKIP_32" = 1 ]   && inner="$inner --skip-32"
    [ "$SKIP_CUDA" = 1 ] && inner="$inner --skip-cuda"
    # Quoted, or the semicolons in the architecture list end up separating
    # commands for the shell inside the container instead of staying together.
    [ -n "$CUDA_ARCHS" ] && inner="$inner --cuda-archs='$CUDA_ARCHS'"

    step "Building the Linux rows in $DOCKER_IMAGE"
    local plat=""
    [ "$(uname -m)" = "x86_64" ] || plat="--platform linux/amd64"
    # shellcheck disable=SC2086
    if ! docker run --rm $plat -v "$REPO":/src -w /src -e DEBIAN_FRONTEND=noninteractive \
        "$DOCKER_IMAGE" bash -c "
            bash scripts/make-release.sh '$VERSION' $inner
            chown -R $(id -u):$(id -g) /src/dist /src/logs /src/.toolchain /src/build-* 2>/dev/null
            true"
    then
        note "the container never ran - nothing was built and nothing was packaged"
        say ""
        say "  The container did not start. Nothing was built, so packaging is skipped"
        say "  rather than quietly rebuilding an archive out of whatever was already"
        say "  in dist/. Fix the error above and run it again."
        return 1
    fi

    # The script inside the container reports failures but still exits 0, so an
    # empty dist is the signal that every row died.
    if ! ls "$DIST"/"Fluid Solver $VERSION linux-"* >/dev/null 2>&1; then
        note "the container produced no linux rows - see logs/"
        say ""
        say "  Not one Linux row came out of the container, so packaging is skipped."
        say "  The reason is in logs/linux-*.log."
        return 1
    fi

    [ "$ONLY" = build ] && return 0
    [ "$WITH_INSTALLERS" = 1 ] && build_installer
    package
}

# ---- run -------------------------------------------------------------------
say "Fluid Solver $VERSION - $PLATFORM-$HOSTARCH"

if [ "$USE_DOCKER" = 1 ] && [ "$PLATFORM" = linux ] && [ "$ONLY" != installers ] && [ "$ONLY" != package ]; then
    run_in_docker
else
    [ "$USE_DOCKER" = 1 ] && [ "$PLATFORM" != linux ] && note "--docker only applies to Linux and was ignored"
    case "$ONLY" in
        all)        ensure_tools; build_all
                    [ "$WITH_INSTALLERS" = 1 ] && build_installer
                    package ;;
        build)      ensure_tools; build_all ;;
        installers) build_installer ;;
        package)    package ;;
    esac
fi

step "Done"
if [ -d "$REL" ]; then
    ls -1sh "$REL" | tail -n +2 | sed 's/^/  /'
fi
if [ "$WITH_INSTALLERS" = 0 ] && [ "$ONLY" = all ]; then
    say ""
    say "Installers were not built. Add --with-installers for those."
fi
if [ "$PROBLEM_COUNT" -gt 0 ]; then
    say ""
    say "$PROBLEM_COUNT thing(s) did not work:"
    printf '  %s\n' "${PROBLEMS[@]}"
fi
say ""
say "Windows rows are built by scripts\\make-release.ps1 there."
say "Drop its dist\\ output in beside this one and rerun with --only=package."
exit 0
