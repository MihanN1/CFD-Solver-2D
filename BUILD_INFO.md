# Fluid Solver UI — build and validation notes

Date: 2026-08-21

## Scope

This package is the UI source revision only. The solver project is an external,
read-only runtime dependency; no solver source, binary or published archive was
modified.

## Package size

`third_party/sfml` was vendored whole, including the parts a build never reads.
Removed: `examples/`, `test/`, `doc/`, `.github/`, and the `miniaudio`, `dr_mp3`
and `wepoll` header drops under `extlibs/` — SFML's audio and network modules are
switched off here and nothing else includes them. The package went from 30 MB to
15 MB. What is left of `extlibs` is genuinely referenced: `cpp-unicodelib` by
`System/String.cpp`, `stb_image` and `qoi` by `Graphics/Image.cpp`, `glad` and
`vulkan` by the window and graphics modules.

Release binaries are stripped on Linux and macOS, which is another 13% off the
file the user downloads. MSVC already keeps debug information in a separate
`.pdb`.

## Frame loading

The reader used to pull the legacy VTK binary payload through an `ifstream` four
bytes at a time — one virtual dispatch per value, several hundred thousand of
them per frame. It now reads the file once and decodes from memory, swapping the
big-endian words in place, eight at a time through one AVX2 shuffle where that is
compiled in. Measured on a 600×300 grid (3.4 MiB frame, two cores):

| build | parse |
| --- | --- |
| before | 24.8 ms |
| after, no AVX2/OpenMP | 3.2 ms |
| after, AVX2 + OpenMP | 2.6 ms |

Frames also decode on several threads at once — one per spare core, capped at
eight. There was a single loader before, and a request arriving while it was busy
simply waited its turn, which is why flipping quickly past the cached few stalled
on every step. The decoded-frame cache holds up to 256 frames inside its byte
budget rather than 16.

## Static linking policy

- `BUILD_SHARED_LIBS=OFF`: SFML is static.
- `SFML_USE_SYSTEM_DEPS=OFF`: FreeType, HarfBuzz and SheenBidi are inside the
  static dependency graph.
- `CFD_UI_STATIC_RUNTIME=ON` by default.
- Windows/MSVC: static CRT (`/MT`).
- Linux/GCC: static `libgcc`, `libstdc++` and, for the OpenMP rows, `libgomp.a`;
  X11/OpenGL remain dynamic because they are system interfaces.
- macOS: system runtimes and frameworks remain dynamic, as the platform requires.

The one exception is a Windows OpenMP row, which needs `vcomp140.dll` beside it
because MSVC has no static OpenMP runtime. The solver's own `omp` archives ship
that DLL already, and a `-ui` archive is that archive plus the UI binary, so the
pairing supplies it.

## Release matrix

`scripts/build-ui-release.py` produces the 30 archive names of the solver release
with `-ui` before `.zip`:

- Windows x64: 8 rows, x86: 4 rows.
- Linux x64: 8 rows, x86: 4 rows.
- macOS arm64: 2 rows, x64: 4 rows.

AVX2 and OpenMP both change the generated code, so they select a build. CUDA does
not: decoding a frame is a read plus a byte swap, and the colour map has to end
up in host memory for the texture upload anyway, so a GPU round trip would cost
more in transfers than the arithmetic is worth. The `-cuda` suffix therefore
selects a name only, and 22 builds cover the 30 names.

A row named `omp` is configured with `CFD_UI_ENABLE_OPENMP_EXPLICIT=ON`, so a
build machine without an OpenMP runtime fails that row rather than publishing a
single-threaded binary under a name that promises otherwise.

`.github/workflows/build-ui-all.yml` builds on native Windows, Linux and macOS
runners, merges the platform artifacts, verifies all 30 rows, checks that every
archive actually carries a UI executable, adds
`Fluid-Solver-UI-Source-Code.zip`, and writes SHA-256 checksums.

## Assembling a `-ui` release archive

Take the solver's archive for a row and add `Fluid Solver UI[.exe]` from the UI
archive of the same name. Nothing else from the UI archive is needed. The
documentation inside a UI archive is named `README-UI.md` and `BUILD_INFO-UI.md`
on purpose, so that copying the whole folder across cannot replace the solver's
own `README.md` and `LICENSE`.

## Verified in this revision

- Full Linux Release link of `Fluid Solver UI`, with and without AVX2 and OpenMP.
- `FluidSolverRunTests`, `GeometryProcessorTests`, `VtkFrameTests`: passed.
- Reader equivalence: the same data written as BINARY and as ASCII decodes to
  bit-identical pressure, solid, velocity, speed, finite masks and ranges,
  including frames carrying NaN and infinity.
- Reader robustness: truncated payload, truncated header, empty file and a solid
  value outside {0,1} all raise `VtkParseError`.
- Ranges and speeds checked against independently computed reference values.
- Solver discovery: the extension-less solver is found beside the UI, a
  non-executable one is refused with a message saying so, and a configured path
  is used when nothing sits beside the UI.
- Release matrix compared name by name against the solver's `release/0.1`:
  30/30, none missing, none extra.
- The eight `linux-x64` rows built and packaged end to end, producing exactly
  four distinct binaries — each shared by its `-cuda` and plain row, as intended.

## Not verified here

- Windows and macOS builds (no such host in this environment).
- GUI launch and interactive visual checks (the container has no display).
- A full 30-row GitHub Actions run.
