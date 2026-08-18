#!/usr/bin/env bash
# Builds every Linux row of the release matrix.
#
#   x86-64 : AVX2 {on,off} x OpenMP {on,off} x CUDA {on,off} = 8
#   i686   : AVX2 {on,off} x OpenMP {on,off}                 = 4
#            (there is no 32-bit CUDA and has not been since CUDA 9)
#
# Build this inside an old-glibc image, not on a current distro. A statically
# linked glibc binary still records the kernel version it was built against,
# so a binary built on a new system refuses to start on an older one for no
# reason the user can act on. Debian 11 or manylinux2014 is the right base.
#
#   ./scripts/build-linux.sh                 # everything it can
#   ./scripts/build-linux.sh --skip-cuda
#   ./scripts/build-linux.sh --skip-32

set -uo pipefail

VERSION="${VERSION:-0.1.0}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$REPO/dist"
# CUDA 12.x covers sm_50..sm_90. CUDA 13 dropped everything below Turing.
CUDA_ARCHS="${CUDA_ARCHS:-75;80;86;89;90}"

SKIP_CUDA=0
SKIP_32=0
for arg in "$@"; do
    case "$arg" in
        --skip-cuda) SKIP_CUDA=1 ;;
        --skip-32)   SKIP_32=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v nvcc >/dev/null 2>&1 || SKIP_CUDA=1
echo 'int main(){return 0;}' > /tmp/.m32probe.c
gcc -m32 /tmp/.m32probe.c -o /tmp/.m32probe 2>/dev/null || SKIP_32=1
rm -f /tmp/.m32probe /tmp/.m32probe.c

mkdir -p "$DIST"

build_row() {
    local arch="$1" avx2="$2" omp="$3" cuda="$4"

    local tags=""
    [ "$avx2" = ON ] && tags="${tags}avx2-"
    [ "$omp"  = ON ] && tags="${tags}omp-"
    [ "$cuda" = ON ] && tags="${tags}cuda-"
    tags="${tags%-}"; [ -z "$tags" ] && tags="plain"

    local label="linux-$arch"
    local name="Fluid Solver $VERSION $label $tags"
    local build="$REPO/build-$label-$tags"

    echo "==> $name"
    rm -rf "$build"

    local args=(
        -S "$REPO" -B "$build"
        -DCMAKE_BUILD_TYPE=Release
        -DCFD_STATIC=ON
        -DCFD_ENABLE_AVX2="$avx2"
        -DCFD_ENABLE_OPENMP="$omp"
        -DCFD_ENABLE_CUDA="$cuda"
    )
    # Otherwise a missing toolkit quietly yields a CPU-only binary that would
    # then be published under a name promising CUDA.
    [ "$cuda" = ON ] && args+=(-DCFD_ENABLE_CUDA_EXPLICIT=ON -DCFD_CUDA_ARCHITECTURES="$CUDA_ARCHS")
    [ "$arch" = "x86" ] && args+=(-DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32)

    if ! cmake "${args[@]}" >/dev/null 2>&1 || ! cmake --build "$build" -j"$(nproc)" >/dev/null 2>&1; then
        echo "    build failed - skipped"
        return
    fi

    local exe
    exe="$(find "$build/bin" -type f -name 'Fluid Solver' | head -1)"
    if [ -z "$exe" ]; then echo "    no executable produced - skipped"; return; fi

    cp "$exe" "$DIST/$name"
    strip "$DIST/$name" 2>/dev/null
    printf '    ok, %s, %s\n' \
        "$(du -h "$DIST/$name" | cut -f1)" \
        "$(file -b "$DIST/$name" | grep -o 'statically linked\|dynamically linked')"
}

for avx2 in ON OFF; do
    for omp in ON OFF; do
        for cuda in ON OFF; do
            [ "$cuda" = ON ] && [ "$SKIP_CUDA" = 1 ] && continue
            build_row x64 "$avx2" "$omp" "$cuda"
        done
        [ "$SKIP_32" = 1 ] || build_row x86 "$avx2" "$omp" OFF
    done
done

echo
echo "Built into $DIST :"
ls -1 "$DIST"
[ "$SKIP_CUDA" = 1 ] && echo "note: CUDA rows skipped, no nvcc on this machine"
[ "$SKIP_32"   = 1 ] && echo "note: 32-bit rows skipped, install gcc-multilib g++-multilib"
exit 0
