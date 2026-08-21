# CFD Mask UI Optimized

This package builds the GUI independently of the CFD solver source tree.
`CFD-Solver-2D-main` is not a build dependency and must not be modified for GUI work.

## Requirements

- CMake 3.28 or newer.
- Visual Studio 18 2026 with **Desktop development with C++**.
- Windows SDK.

The package includes the GUI's direct source dependencies:

- `third_party/tinyobjloader/tiny_obj_loader.h`
- `third_party/sfml/` (SFML source)

Therefore `CFD_ROOT_DIR` is no longer required and the first configure does not
need a solver checkout. With the release-default static dependency policy, SFML
may download its pinned FreeType/HarfBuzz/SheenBidi sources on a clean build.

## Build with CMake GUI

1. Extract the ZIP.
2. Open **CMake GUI**.
3. **Where is the source code:** select the extracted `Source` folder.
4. **Where to build the binaries:** select a separate sibling folder, for example
   `CFD-Mask-UI-Optimized_build`.
5. If you previously configured an older package in that build folder, use
   **File -> Delete Cache** first.
6. Press **Configure**.
7. Generator: **Visual Studio 18 2026**; platform: **x64**.
8. Leave `CFD_SOLVER_EXE` empty unless you explicitly want CMake to copy a built
   `Fluid Solver.exe` beside the GUI.
9. Press **Configure** again until there are no red unresolved entries.
10. Press **Generate**.
11. Press **Open Project** and build `cfd_mask_ui_optimized` in **Release | x64**.

Typical executable location for a Visual Studio multi-config build:

```text
<build-folder>/Release/Fluid Solver UI.exe
```

`CFD_UI_STATIC_RUNTIME=ON` is the default. On Windows this selects the static
MSVC CRT (`/MT`). SFML and its bundled FreeType/HarfBuzz/SheenBidi dependencies
are also built static. Linux additionally links `libgcc`/`libstdc++` statically
when GCC is used; X11/OpenGL/OS libraries remain dynamic because they are system
interfaces. macOS system runtimes/frameworks remain dynamic by platform design.


## Release matrix

The UI now has its own release builder and GitHub Actions workflow:

```text
scripts/build-ui-release.py
.github/workflows/build-ui-all.yml
```

The matrix matches the finished Fluid Solver 0.1 binary matrix exactly: 30 UI
archives across Windows/Linux/macOS, x64/x86/arm64, AVX2/non-AVX2, and the same
OpenMP/CUDA feature-name combinations. Every archive name is the matching solver
archive name with `-ui` appended before `.zip`, for example:

```text
Fluid Solver 0.1 windows-x64 cuda-ui.zip
Fluid Solver 0.1 windows-x64 avx2-omp-cuda-ui.zip
Fluid Solver 0.1 macos-arm64 plain-ui.zip
```

List the exact 30 names without building:

```text
python scripts/build-ui-release.py --version 0.1 --list
```

Build all Windows rows locally with Visual Studio 18 2026:

```text
python scripts/build-ui-release.py --version 0.1 --arch x64 --arch x86
```

On Linux the same command builds x64/x86; the 32-bit development libraries must
be installed. On macOS, CI builds arm64 and x64 on native runners. The workflow
merges all four native-platform jobs, verifies that all 30 expected archives are
present, creates `Fluid-Solver-UI-Source-Code.zip`, and writes `SHA256SUMS.txt`.

AVX2 is a real UI compile-time ISA switch. OpenMP and CUDA are solver-kernel
features, not UI execution backends, so those suffixes are compatibility/package
identity only; archives with the same architecture and AVX2 state reuse the same
UI binary. This is required so the existing installer can pair
`<feature>-ui` with the matching solver `<feature>` row by name without adding
meaningless OpenMP/CUDA dependencies to the UI executable.

Each binary archive contains one top-level folder with the same stem,
`Fluid Solver UI[.exe]`, README/LICENSE/build info, and an empty `output/` folder.

## Solver integration

The solver is optional at GUI build time. At runtime the GUI can use
**Select solver EXE** to point to `Fluid Solver.exe`. Selecting/replacing the
solver does not require rebuilding the GUI.

For a fresh UI with no saved preference, the default VTK/output root is
`./output`, resolved from the process working directory and converted to an
absolute run directory before it is passed to the current Fluid Solver. A saved
`ui-preferences.txt` or loaded `.cfdui` configuration can intentionally override
that default.

The current UI launch contract does not emit gravity, `restart`, `restartFile`,
or `addTime`. VTK restart parsing/state support is retained for later continuation
work when the solver has the required restart interface.

## Tests

Enable `BUILD_TESTING` in CMake GUI if wanted, then build the test targets and run
CTest. `SolverCompatibilityTests` is added only when `CFD_SOLVER_EXE` points to an
existing solver executable.

## Parameter panel

The setup parameter panel is one scrollable list. It contains all current
solver-facing numerical controls rather than hiding the multigrid/timestep
controls on a separate Basic/Advanced page.

- Mouse wheel over the left parameter panel scrolls the controls.
- The visible scrollbar can be dragged or clicked.
- Mouse wheel over the 3D preview still controls preview zoom.
- The 21 user-editable controls cover the solver's physical, timestep,
  multigrid, output-frequency, and CUDA settings.
- `outputDir`, `geometryFile`, transformed slice arguments, and `invertSection`
  are generated from the GUI workflow rather than exposed as ordinary sliders.


## Current UI revision — 2026-08-21

This source revision is UI-only. It does not modify the Fluid Solver numerical
code or either finished solver distribution.

Implemented UI behavior:

- warns before launching when the slice contains multiple disconnected contours;
  the user may cancel or explicitly reduce the run to the largest contour, and
  the preview/adapter are updated to match that choice;
- identifies the selected `Fluid Solver.exe` and displays its version/build;
- distinguishes CPU-only and CUDA-capable solver builds and disables CUDA requests
  when the selected solver cannot provide CUDA;
- renames the process-stop action to **Stop simulation** and confirms before
  closing the application while a simulation is active;
- keeps all numerical controls visible in one scrollable panel, grouped by
  physical, geometry, grid, timestep, multigrid, output, and backend purpose;
- provides integer +/- adjustment, inline invalid-field highlighting, and
  parameter help text;
- displays derived grid/runtime information including `dx`, `dy`, cell count,
  Reynolds number, approximate timestep, approximate VTK count, and estimated
  multigrid levels;
- persists the output-root preference and supports Save/Load of `.cfdui`
  configuration files;
- writes UI-owned run metadata with solver identity and requested parameters;
- exposes a configurable decoded-VTK cache budget;
- shows `u`, `v`, speed, and pressure for result inspection;
- adds result-frame keyboard navigation, playback, series/current-frame range
  selection, and a Run details viewer;
- retains VTK restart parsing infrastructure but does not expose unsupported
  restart/continuation launch arguments.

Current fresh-run CLI compatibility remains the 24-key Fluid Solver contract.
Gravity and continuation keys are not emitted.

## Validation status of this source revision

Performed in the available Linux validation environment:

- C++17 syntax check of every `src/*.cpp`: passed.
- `FluidSolverRunTests`: compiled and passed.
- `GeometryProcessorTests`: compiled and passed.
- `VtkFrameTests`: compiled and passed.
- Generated 30-name release matrix compared exactly against the provided solver `release/0.1` binary archive set: 30/30, no missing or extra rows.

Not performed here:

- Visual Studio 18 2026 Windows Release build;
- Windows GUI launch and interactive visual verification;
- live execution of the Windows-only finished Fluid Solver binaries.
