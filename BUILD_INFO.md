# CFD Mask UI Optimized — Build / Validation Information

Date: 2026-08-21

## Scope

This package contains the GUI source revision only. The reference `CFD-Solver-2D` project/release was inspected to reproduce its release matrix; no solver source, binary, or published solver archive was modified.

## Default VTK/output root

A fresh UI now starts with `./output` as its output root, relative to the process working directory. The UI resolves that location to an absolute path before generating the solver's `outputDir`, preserving the current solver requirement that output paths be absolute. Existing saved preferences/configuration can still override the default.

## Static linking policy

- `BUILD_SHARED_LIBS=OFF`: SFML is static.
- `SFML_USE_SYSTEM_DEPS=OFF`: FreeType, HarfBuzz, and SheenBidi are built into the static dependency graph.
- `CFD_UI_STATIC_RUNTIME=ON` by default.
- Windows/MSVC: static CRT (`/MT`, `/MTd` for Debug).
- Linux/GCC: static `libgcc` + `libstdc++`; X11/OpenGL/OS interfaces remain dynamic.
- macOS: system runtime/frameworks remain dynamic because macOS does not support fully static desktop binaries.

## Release matrix

`scripts/build-ui-release.py` generates the exact 30 solver-corresponding UI archive names:

- Windows x64: 8 rows.
- Windows x86: 4 rows.
- Linux x64: 8 rows.
- Linux x86: 4 rows.
- macOS arm64: 2 rows.
- macOS x64: 4 rows.

Each name is the solver row name plus `-ui` before `.zip`.

AVX2 changes generated UI machine code. OpenMP/CUDA do not execute inside this UI, so their suffixes are release-pairing identities only; rows with equal architecture + AVX2 state reuse one built UI binary. This prevents useless OpenMP/CUDA runtime dependencies while preserving exact installer pairing.

`.github/workflows/build-ui-all.yml` builds on native Windows/Linux/macOS runners, merges the platform artifacts, verifies all 30 rows, adds `Fluid-Solver-UI-Source-Code.zip`, and writes SHA-256 checksums.

## Windows CMake GUI build

1. Extract this package.
2. In CMake GUI, set **Where is the source code** to this extracted folder.
3. Set **Where to build the binaries** to a separate empty build folder.
4. Configure with **Visual Studio 18 2026**, platform **x64**.
5. Leave `CFD_SOLVER_EXE` empty unless you intentionally want CMake to copy a selected `Fluid Solver.exe` beside the GUI.
6. Configure until no unresolved red entries remain.
7. Generate.
8. Open Project.
9. Build `cfd_mask_ui_optimized` as **Release | x64**.

Typical executable:

`<build-folder>/Release/Fluid Solver UI.exe`

## Validation evidence available in this package preparation

- Written: release builder, workflow, CMake static/AVX2 controls, VTK default change, and documentation.
- Python syntax: `scripts/build-ui-release.py` passed `py_compile`.
- Release matrix enumeration: exactly 30 archive names.
- Programmatic comparison against `CFD-Solver-2D/release/0.1`: generated names exactly equal every solver binary archive name transformed with `-ui` before `.zip` (30/30, no missing/extra rows).
- ZIP packaging smoke test: executable/file/directory Unix modes and archive layout verified.
- C++17 syntax check of every GUI `src/*.cpp`: passed.
- `FluidSolverRunTests`: passed.
- `GeometryProcessorTests`: passed.
- `VtkFrameTests`: passed.
- CMake configure progressed through project/static-dependency setup but stopped in this Linux container because X11 development components `Xrandr`, `Xcursor`, and `Xi` are not installed.
- Full Linux linked executable build: not performed here.
- Windows Visual Studio build: not performed here.
- macOS build: not performed here.
- GitHub Actions 30-row release run: not performed here.

## Finished solver boundary

The finished solver builds remain external, read-only runtime dependencies. Select one through the GUI at runtime if it is not adjacent to the UI executable.
