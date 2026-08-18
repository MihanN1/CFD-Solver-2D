#!/usr/bin/env bash
# Everything this machine can produce for a release, in one run:
#
#     every executable   ->  dist/Fluid Solver <ver> <platform> <feature>
#     the installer      ->  dist/Fluid-Solver-<ver>-linux-<arch>.run
#                            dist/Fluid Solver <ver> macos-<arch>.pkg
#     the release folder ->  release/<ver>/
#
# It does not tag anything, does not upload anything and does not touch git.
#
#     ./scripts/make-release.sh 0.1
#     ./scripts/make-release.sh 0.1 --skip-cuda --skip-32
#     ./scripts/make-release.sh 0.1 --only=package
#
# Rows whose prerequisites are missing are reported and skipped rather than
# failing the run; the summary at the end says what did not get built.
#
# Build the Linux rows in a Debian 11 or manylinux2014 container, not on a
# current distro. A statically linked glibc binary still records the kernel it
# was built against, so one built on a new system refuses to start on an older
# one for no reason the user can act on.

set -uo pipefail

VERSION="${1:-0.1}"; shift 2>/dev/null || true
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO/dist"
REL="$REPO/release/$VERSION"
# CUDA 12.x covers sm_50..sm_90. CUDA 13 dropped everything below Turing.
CUDA_ARCHS="${CUDA_ARCHS:-50;60;61;70;75;80;86;89;90}"

SKIP_CUDA=0; SKIP_32=0; ONLY=all
for arg in "$@"; do
    case "$arg" in
        --skip-cuda) SKIP_CUDA=1 ;;
        --skip-32)   SKIP_32=1 ;;
        --only=*)    ONLY="${arg#--only=}" ;;
        -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

PROBLEMS=()
note() { PROBLEMS+=("$1"); }

case "$(uname -s)" in
    Darwin) PLATFORM=macos; HOSTARCH=$(uname -m); [ "$HOSTARCH" = "x86_64" ] && HOSTARCH=x64 ;;
    *)      PLATFORM=linux; HOSTARCH=x64 ;;
esac

# ---- what this machine can do ---------------------------------------------
command -v nvcc >/dev/null 2>&1 || { [ "$SKIP_CUDA" = 0 ] && note "CUDA rows skipped: no nvcc on this machine"; SKIP_CUDA=1; }
[ "$PLATFORM" = macos ] && SKIP_CUDA=1 && SKIP_32=1   # no CUDA and no 32-bit on macOS, ever
if [ "$PLATFORM" = linux ] && [ "$SKIP_32" = 0 ]; then
    echo 'int main(){return 0;}' > /tmp/.m32probe.c
    gcc -m32 /tmp/.m32probe.c -o /tmp/.m32probe 2>/dev/null || {
        note "32-bit rows skipped: install gcc-multilib g++-multilib"; SKIP_32=1; }
    rm -f /tmp/.m32probe /tmp/.m32probe.c
fi

# ---- 1. the executables ----------------------------------------------------
build_row() {
    local arch="$1" avx2="$2" omp="$3" cuda="$4"
    local tags=""
    [ "$avx2" = ON ] && tags="${tags}avx2-"
    [ "$omp"  = ON ] && tags="${tags}omp-"
    [ "$cuda" = ON ] && tags="${tags}cuda-"
    tags="${tags%-}"; [ -z "$tags" ] && tags="plain"

    local name="Fluid Solver $VERSION $PLATFORM-$arch $tags"
    local build="$REPO/build-$PLATFORM-$arch-$tags"
    printf '  %s ... ' "$name"
    rm -rf "$build"

    local args=(-S "$REPO" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCFD_STATIC=ON
                -DCFD_ENABLE_AVX2="$avx2" -DCFD_ENABLE_OPENMP="$omp" -DCFD_ENABLE_CUDA="$cuda")
    [ "$cuda" = ON ] && args+=(-DCFD_ENABLE_CUDA_EXPLICIT=ON -DCFD_CUDA_ARCHITECTURES="$CUDA_ARCHS")
    [ "$arch" = "x86" ] && args+=(-DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32)
    if [ "$PLATFORM" = macos ]; then
        [ "$arch" = "arm64" ] && args+=(-DCMAKE_OSX_ARCHITECTURES=arm64) || args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64)
        [ "$omp" = ON ] && command -v brew >/dev/null 2>&1 && args+=(-DOpenMP_ROOT="$(brew --prefix libomp 2>/dev/null)")
    fi

    if ! cmake "${args[@]}" >/dev/null 2>&1; then echo "configure failed"; note "$name - configure failed"; return; fi
    local jobs; jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    if ! cmake --build "$build" -j"$jobs" >/dev/null 2>&1; then echo "build failed"; note "$name - build failed"; return; fi

    local exe; exe="$(find "$build/bin" -type f -name 'Fluid Solver' 2>/dev/null | head -1)"
    if [ -z "$exe" ]; then echo "no executable"; note "$name - no executable produced"; return; fi

    mkdir -p "$DIST"
    cp "$exe" "$DIST/$name"
    strip "$DIST/$name" 2>/dev/null || strip -x "$DIST/$name" 2>/dev/null
    printf 'ok, %s, %s\n' "$(du -h "$DIST/$name" | cut -f1)" \
        "$(file -b "$DIST/$name" | grep -o 'statically linked\|dynamically linked' || echo 'Mach-O')"
}

build_all() {
    echo "Building the executables"
    if [ "$PLATFORM" = macos ]; then
        # arm64 has no AVX2 at all, so it has two rows rather than four.
        local avx_list="ON OFF"; [ "$HOSTARCH" = "arm64" ] && avx_list="OFF"
        for avx2 in $avx_list; do for omp in ON OFF; do
            build_row "$HOSTARCH" "$avx2" "$omp" OFF
        done; done
    else
        for avx2 in ON OFF; do for omp in ON OFF; do
            for cuda in ON OFF; do
                [ "$cuda" = ON ] && [ "$SKIP_CUDA" = 1 ] && continue
                build_row x64 "$avx2" "$omp" "$cuda"
            done
            [ "$SKIP_32" = 1 ] || build_row x86 "$avx2" "$omp" OFF
        done; done
    fi
}

# ---- 2. the installer ------------------------------------------------------
build_installer() {
    echo "Building the installer"
    if [ "$PLATFORM" = macos ]; then
        if [ -x "$REPO/installer/macos/build-pkg.sh" ]; then
            "$REPO/installer/macos/build-pkg.sh" "$VERSION" "$([ "$HOSTARCH" = arm64 ] && echo arm64 || echo x86_64)" \
                || note "macos installer - build-pkg.sh failed"
        else
            note "macos installer - installer/macos/build-pkg.sh is missing"
        fi
        return
    fi

    if ! command -v makeself >/dev/null 2>&1; then
        echo "  makeself is not installed - skipped (apt install makeself)"
        note "linux installer - makeself is not installed"
        return
    fi

    for arch in x64 x86; do
        [ "$arch" = x86 ] && [ "$SKIP_32" = 1 ] && continue
        # An installer with nothing to install is worse than no installer.
        if ! ls "$DIST"/"Fluid Solver $VERSION linux-$arch "* >/dev/null 2>&1; then
            echo "  linux-$arch ... no builds to package - skipped"
            continue
        fi
        printf '  linux-%s ... ' "$arch"
        local pkg; pkg="$(mktemp -d)"
        cp "$DIST"/"Fluid Solver $VERSION linux-$arch "* "$pkg/"
        for f in README.md LICENSE; do [ -f "$REPO/$f" ] && cp "$REPO/$f" "$pkg/"; done
        [ -d "$REPO/models" ] && cp -r "$REPO/models" "$pkg/" 2>/dev/null
        mkdir -p "$pkg/icons"
        cp "$REPO"/logo/fluid-solver-*.png "$pkg/icons/" 2>/dev/null
        sed -e "s/__VERSION__/$VERSION/" -e "s/__ARCH__/$arch/" \
            "$REPO/installer/linux/install.sh" > "$pkg/install.sh"
        chmod +x "$pkg/install.sh"
        if makeself --quiet "$pkg" "$DIST/Fluid-Solver-$VERSION-linux-$arch.run" \
                    "Fluid Solver $VERSION" ./install.sh >/dev/null 2>&1; then
            echo "ok"
        else
            echo "failed"; note "linux-$arch installer - makeself failed"
        fi
        rm -rf "$pkg"
    done
}

# ---- 3. the release folder -------------------------------------------------
package() {
    echo "Assembling release/$VERSION"
    command -v zip >/dev/null 2>&1 || { note "packaging skipped - zip is not installed"; echo "  zip is not installed"; return; }
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
        rm -rf "$stage"; echo "  $base.zip"
    done
    for f in "$DIST"/*setup.exe "$DIST"/*.pkg "$DIST"/*.run; do
        cp "$f" "$REL/"; echo "  $(basename "$f")"
    done
    shopt -u nullglob

    # The two standalone files: what the release was built from, and how to use
    # it. lib/sfml is the UI's dependency and dwarfs everything else.
    local stage; stage="$(mktemp -d)"
    mkdir -p "$stage/Fluid-Solver-Source-Code"
    tar -C "$REPO" \
        --exclude='./.git' --exclude='./.github' --exclude='./.vs' --exclude='./.vscode' \
        --exclude='./build*' --exclude='./out' --exclude='./dist' --exclude='./release' \
        --exclude='./bt-*' --exclude='./lib/sfml' --exclude='./output/*.vtk' \
        -cf - . | tar -C "$stage/Fluid-Solver-Source-Code" -xf -
    ( cd "$stage" && zip -qr9 "$REL/Fluid-Solver-Source-Code.zip" "Fluid-Solver-Source-Code" )
    rm -rf "$stage"; echo "  Fluid-Solver-Source-Code.zip"

    cp "$REPO/README.md" "$REL/README.md"; echo "  README.md"
    ( cd "$REL" && { sha256sum -- * 2>/dev/null || shasum -a 256 -- *; } > SHA256SUMS.txt )
}

# ---- run -------------------------------------------------------------------
echo "Fluid Solver $VERSION - $PLATFORM release"
echo
case "$ONLY" in
    all)         build_all; echo; build_installer; echo; package ;;
    build)       build_all ;;
    installers)  build_installer ;;
    package)     package ;;
    *) echo "--only= takes all, build, installers or package" >&2; exit 2 ;;
esac

echo
echo "Done"
[ -d "$REL" ] && ls -1sh "$REL" | sed 's/^/  /'
if [ "${#PROBLEMS[@]}" -gt 0 ]; then
    echo
    echo "${#PROBLEMS[@]} thing(s) did not work:"
    printf '  %s\n' "${PROBLEMS[@]}"
    echo
    echo "Windows rows are not built here - run scripts\\make-release.ps1 there,"
    echo "drop its output into dist/, and rerun with --only=package."
fi
exit 0
