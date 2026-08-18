#!/usr/bin/env bash
# Builds the release rows this machine can produce, then packs them.
#
# The matrix is deliberately small. CUDA disables itself at runtime when no
# NVIDIA device is present, so one CUDA build serves everybody and there is no
# reason to ship a separate non-CUDA one; OpenMP costs nothing and is always
# on. AVX2 is the only axis that has to be split, because a binary cannot
# decide at runtime whether it may execute an AVX2 instruction - it just dies.
# "plain" is the fallback for whatever the other two cannot run on: no CUDA,
# no OpenMP, no AVX2, and on Linux it is the only fully static one, because
# libcudart_static needs dlopen and pthread and so rules -static out.
#
#     linux-x64      avx2-omp-cuda, omp-cuda, plain
#     linux-x86      avx2-omp, omp            (no 32-bit CUDA exists)
#     macos-x64      avx2-omp, omp            (no CUDA on macOS at all)
#     macos-arm64    omp                      (arm64 has no AVX2 either)
#
#     ./scripts/make-release.sh 0.1
#     ./scripts/make-release.sh 0.1 --with-installers
#     ./scripts/make-release.sh 0.1 --only=package
#
# The CUDA architecture list follows the installed toolkit, so nothing has to
# be passed by hand. Rows whose prerequisites are missing are reported and
# skipped rather than failing the run.
#
# Build the Linux rows in a Debian 11 or manylinux2014 container, not on a
# current distro: a statically linked glibc binary records the kernel it was
# built against, so one built on a new system refuses to start on an older one.

set -uo pipefail

VERSION="${1:-0.1}"; shift 2>/dev/null || true
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO/dist"
REL="$REPO/release/$VERSION"

SKIP_32=0; ONLY=all; WITH_INSTALLERS=0; CUDA_ARCHS="${CUDA_ARCHS:-}"
for arg in "$@"; do
    case "$arg" in
        --skip-32)          SKIP_32=1 ;;
        --with-installers)  WITH_INSTALLERS=1 ;;
        --only=*)           ONLY="${arg#--only=}" ;;
        --cuda-archs=*)     CUDA_ARCHS="${arg#--cuda-archs=}" ;;
        -h|--help)          sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

PROBLEMS=()
note() { PROBLEMS+=("$1"); }

case "$(uname -s)" in
    Darwin) PLATFORM=macos; HOSTARCH="$(uname -m)"; [ "$HOSTARCH" = "x86_64" ] && HOSTARCH=x64 ;;
    *)      PLATFORM=linux; HOSTARCH=x64 ;;
esac

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

HAVE_NVCC=0; command -v nvcc >/dev/null 2>&1 && HAVE_NVCC=1
HAVE_M32=0
if [ "$PLATFORM" = linux ]; then
    echo 'int main(){return 0;}' > /tmp/.m32probe.c
    gcc -m32 /tmp/.m32probe.c -o /tmp/.m32probe 2>/dev/null && HAVE_M32=1
    rm -f /tmp/.m32probe /tmp/.m32probe.c
fi

# ---- one row ---------------------------------------------------------------
build_row() {
    local arch="$1" avx2="$2" omp="$3" cuda="$4" archs="$5"
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
    [ "$cuda" = ON ] && args+=(-DCFD_ENABLE_CUDA_EXPLICIT=ON -DCFD_CUDA_ARCHITECTURES="$archs")
    [ "$arch" = "x86" ] && args+=(-DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32)
    if [ "$PLATFORM" = macos ]; then
        [ "$arch" = "arm64" ] && args+=(-DCMAKE_OSX_ARCHITECTURES=arm64) \
                              || args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64)
        [ "$omp" = ON ] && command -v brew >/dev/null 2>&1 && \
            args+=(-DOpenMP_ROOT="$(brew --prefix libomp 2>/dev/null)")
    fi

    if ! cmake "${args[@]}" >/dev/null 2>&1; then echo "configure failed"; note "$name - configure failed"; return; fi
    local jobs; jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    if ! cmake --build "$build" -j"$jobs" >/dev/null 2>&1; then echo "build failed"; note "$name - build failed"; return; fi

    local exe; exe="$(find "$build/bin" -type f -name 'Fluid Solver' 2>/dev/null | head -1)"
    if [ -z "$exe" ]; then echo "no executable"; note "$name - no executable produced"; return; fi

    mkdir -p "$DIST"
    cp "$exe" "$DIST/$name"
    strip "$DIST/$name" 2>/dev/null || strip -x "$DIST/$name" 2>/dev/null
    printf 'ok, %s, %s\n' "$(du -h "$DIST/$name" | cut -f1)" \
        "$(file -b "$DIST/$name" | grep -o 'statically linked\|dynamically linked' || echo 'Mach-O')"
}

# ---- the rows --------------------------------------------------------------
build_all() {
    echo "Building the executables"
    local archs; archs="$(resolve_cuda_archs)"
    [ "$HAVE_NVCC" = 1 ] && echo "  CUDA architectures: $archs"

    if [ "$PLATFORM" = macos ]; then
        if [ "$HOSTARCH" = "arm64" ]; then
            build_row arm64 OFF ON OFF "$archs"
        else
            build_row x64 ON  ON OFF "$archs"
            build_row x64 OFF ON OFF "$archs"
        fi
        return
    fi

    # linux-x64: two CUDA rows split on AVX2, plus the fully static fallback
    if [ "$HAVE_NVCC" = 1 ]; then
        build_row x64 ON  ON ON "$archs"
        build_row x64 OFF ON ON "$archs"
    else
        echo "  (both CUDA rows skipped: nvcc is not on PATH)"
        note "linux-x64 CUDA rows skipped - nvcc is not on PATH"
    fi
    build_row x64 OFF OFF OFF "$archs"

    if [ "$SKIP_32" = 1 ]; then
        :
    elif [ "$HAVE_M32" = 0 ]; then
        echo "  (both 32-bit rows skipped: no multilib)"
        note "linux-x86 rows skipped - install gcc-multilib g++-multilib"
    else
        build_row x86 ON  ON OFF "$archs"
        build_row x86 OFF ON OFF "$archs"
    fi
}

# ---- installer -------------------------------------------------------------
build_installer() {
    echo "Building the installer"
    if [ "$PLATFORM" = macos ]; then
        if [ -x "$REPO/installer/macos/build-pkg.sh" ]; then
            "$REPO/installer/macos/build-pkg.sh" "$VERSION" \
                "$([ "$HOSTARCH" = arm64 ] && echo arm64 || echo x86_64)" \
                || note "macos installer - build-pkg.sh failed"
        else
            note "macos installer - installer/macos/build-pkg.sh is missing"
        fi
        return
    fi
    if ! command -v makeself >/dev/null 2>&1; then
        echo "  makeself is not installed - skipped (apt install makeself)"
        note "linux installer - makeself is not installed"; return
    fi
    for arch in x64 x86; do
        [ "$arch" = x86 ] && [ "$SKIP_32" = 1 ] && continue
        ls "$DIST"/"Fluid Solver $VERSION linux-$arch "* >/dev/null 2>&1 || {
            echo "  linux-$arch ... nothing to package - skipped"; continue; }
        printf '  linux-%s ... ' "$arch"
        local pkg; pkg="$(mktemp -d)"
        cp "$DIST"/"Fluid Solver $VERSION linux-$arch "* "$pkg/"
        for f in README.md LICENSE; do [ -f "$REPO/$f" ] && cp "$REPO/$f" "$pkg/"; done
        [ -d "$REPO/models" ] && cp -r "$REPO/models" "$pkg/" 2>/dev/null
        mkdir -p "$pkg/icons"; cp "$REPO"/logo/fluid-solver-*.png "$pkg/icons/" 2>/dev/null
        sed -e "s/__VERSION__/$VERSION/" -e "s/__ARCH__/$arch/" \
            "$REPO/installer/linux/install.sh" > "$pkg/install.sh"
        chmod +x "$pkg/install.sh"
        if makeself --quiet "$pkg" "$DIST/Fluid-Solver-$VERSION-linux-$arch.run" \
                    "Fluid Solver $VERSION" ./install.sh >/dev/null 2>&1
        then echo "ok"; else echo "failed"; note "linux-$arch installer - makeself failed"; fi
        rm -rf "$pkg"
    done
}

# ---- release folder --------------------------------------------------------
package() {
    echo "Assembling release/$VERSION"
    command -v zip >/dev/null 2>&1 || { echo "  zip is not installed"; note "packaging skipped - zip is not installed"; return; }
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

    local stage; stage="$(mktemp -d)"
    mkdir -p "$stage/Fluid-Solver-Source-Code"
    tar -C "$REPO" \
        --exclude='./.git' --exclude='./.github' --exclude='./.vs' --exclude='./.vscode' \
        --exclude='./build*' --exclude='./out' --exclude='./dist' --exclude='./release' \
        --exclude='./bt-*' --exclude='./_to_delete' --exclude='./lib/sfml' \
        --exclude='./output/*.vtk' \
        -cf - . | tar -C "$stage/Fluid-Solver-Source-Code" -xf -
    ( cd "$stage" && zip -qr9 "$REL/Fluid-Solver-Source-Code.zip" "Fluid-Solver-Source-Code" )
    rm -rf "$stage"; echo "  Fluid-Solver-Source-Code.zip"

    cp "$REPO/README.md" "$REL/README.md"; echo "  README.md"
    ( cd "$REL" && { sha256sum -- * 2>/dev/null || shasum -a 256 -- *; } > SHA256SUMS.txt )
}

# ---- run -------------------------------------------------------------------
echo "Fluid Solver $VERSION - $PLATFORM"
echo
case "$ONLY" in
    all)        build_all; echo
                [ "$WITH_INSTALLERS" = 1 ] && { build_installer; echo; }
                package ;;
    build)      build_all ;;
    installers) build_installer ;;
    package)    package ;;
    *) echo "--only= takes all, build, installers or package" >&2; exit 2 ;;
esac

echo
echo "Done"
[ -d "$REL" ] && ls -1sh "$REL" | sed 's/^/  /'
if [ "$WITH_INSTALLERS" = 0 ] && [ "$ONLY" = all ]; then
    echo
    echo "Installers were not built. Add --with-installers once the UI folder is in dist/."
fi
if [ "${#PROBLEMS[@]}" -gt 0 ]; then
    echo
    echo "${#PROBLEMS[@]} thing(s) did not work:"
    printf '  %s\n' "${PROBLEMS[@]}"
fi
echo
echo "Windows rows are built by scripts\\make-release.ps1 there."
echo "Drop its dist\\ output in beside this one and rerun with --only=package."
exit 0
