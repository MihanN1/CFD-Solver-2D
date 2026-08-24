# CFD Mask UI Optimized

This package builds the GUI independently of the CFD solver source tree.
`CFD-Solver-2D-main` is not a build dependency and must not be modified for GUI work.

## Requirements

- CMake 3.28 or newer.
- Visual Studio 2022 or newer with **Desktop development with C++**.
- Windows SDK.

On Linux: `build-essential`, `libx11-dev`, `libxrandr-dev`, `libxcursor-dev`,
`libxi-dev`, `libudev-dev`, `libgl1-mesa-dev`. On macOS: the Xcode command line
tools, plus `brew install libomp` for the OpenMP rows.

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
7. Generator: **Visual Studio 17 2022** (or newer); platform: **x64**.
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

Build all Windows rows locally:

```text
python scripts/build-ui-release.py --version 0.1 --arch x64 --arch x86
```

On Linux the same command builds x64/x86; the 32-bit development libraries must
be installed. On macOS, CI builds arm64 and x64 on native runners. The workflow
merges all four native-platform jobs, verifies that all 30 expected archives are
present, creates `Fluid-Solver-UI-Source-Code.zip`, and writes `SHA256SUMS.txt`.

AVX2 and OpenMP are both real compile-time switches for this UI: AVX2 vectorises
the byte swap that decodes a VTK frame, OpenMP spreads that decode and the colour
map across every core. CUDA is not, and deliberately so - decoding a frame is a
read plus a byte swap, and the colour map has to land in host memory for the
texture upload anyway, so a round trip to the GPU would cost more in transfers
than the arithmetic is worth. The `-cuda` suffix therefore selects a name and not
a build, which is what lets 22 builds cover the 30 archive names the solver
release pairs against.

A row whose name promises OpenMP is configured with
`CFD_UI_ENABLE_OPENMP_EXPLICIT=ON`, so a build machine without an OpenMP runtime
fails the row instead of quietly publishing a single-threaded binary under a name
that says otherwise.

On Windows an OpenMP row needs `vcomp140.dll` beside it, because MSVC has no
static OpenMP runtime. The solver's own `omp` archives already ship that DLL, and
a `-ui` archive is that archive plus the UI binary, so the pairing supplies it.

Each binary archive contains one top-level folder with the same stem,
`Fluid Solver UI[.exe]`, `README-UI.md`, `BUILD_INFO-UI.md`, and an empty
`output/` folder. The documentation is renamed on the way in on purpose: these
archives are merged by hand into the solver's `-ui` archive, which has a
`README.md` and a `LICENSE` of its own, and a plain copy would replace them.

**To assemble a `-ui` release archive:** take the solver's archive for that row,
and add `Fluid Solver UI[.exe]` from the UI archive of the same name. Nothing
else from the UI archive is needed - the binary is fully static apart from the
system graphics stack.

## Solver integration

The solver is optional at GUI build time. At runtime the UI looks for the solver
beside its own executable first - `Fluid Solver.exe` on Windows, `Fluid Solver`
with no extension on Linux and macOS - and **Select solver** points it somewhere
else. Selecting or replacing the solver does not require rebuilding the UI.

For a fresh UI with no saved preference, the output root is `output` **beside the
UI executable**, which is where the solver writes its own frames and where the
installers create the folder. It is not the process working directory: a Start
Menu shortcut, a desktop icon or a drag-and-drop each hand the process some other
directory, and frames used to land wherever that happened to be. A saved
`ui-preferences.txt` or a loaded `.cfdui` configuration still overrides it.

What goes on the command line depends on what the selected executable
understands, because a solver exits on the first argument it does not know.
`restart`, `restartFile` and `addTime` need 0.1.1 or newer; `avx2`, `openmp`,
`threads` and `tray` need 0.2. `gravityEnabled`/`gravityAccel`/`gravityAngle`
and `wallMotion` came after 0.2 was published, so no version number separates
them - the UI looks for those key names in the executable's own parameter table
instead, and leaves them off when they are not there.

`wallMotion` is written for every body the mask holds, numbered 1..n the way the
solver numbers them: it flood-fills its mask 8-connected in grid scan order, and
the UI counts the same way so the numbers agree. A continuation sends no
`wallMotion` at all while the wall controls sit at their defaults, so the setting
stored in the frame survives.

## Frame loading

A VTK frame is read in one go and decoded from memory. The legacy binary payload
is big endian, so every 32-bit word is swapped in place - eight at a time through
one AVX2 shuffle where that is compiled in - rather than pulled through the
stream a value at a time, which is what the reader used to do. On a 600x300 grid
that is 24.8 ms down to 2.6 ms per frame.

Frames also decode on several threads at once, one per spare core up to eight.
There used to be a single loader, and a request while it was busy simply waited,
which is why flipping quickly past the few cached steps stalled on every one. The
decoded-frame cache holds up to 256 frames within its byte budget instead of 16,
so an ordinary series ends up entirely resident after one pass.

The colour map that turns a frame into the displayed texture runs across cores
too, and its buffer is kept between frames rather than reallocated per step.

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
- The user-editable controls cover the solver's physical, gravity, wall,
  timestep, multigrid, output-frequency and acceleration settings.
- The WALLS group is one setting applied to every body: rotation about each
  body's own centroid, sliding in x and y, or free-slip, which is exclusive
  with the other three. Per-body lines still have to be typed on the solver's
  own command line.
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

- Full Linux Release link of `Fluid Solver UI`, with and without AVX2/OpenMP.
- `FluidSolverRunTests`, `GeometryProcessorTests`, `VtkFrameTests`: passed.
- Reader equivalence: the same data written as BINARY and as ASCII decodes to
  bit-identical pressure, solid, velocity, speed, finite masks and ranges,
  including frames carrying NaN and infinity.
- Reader robustness: truncated payload, truncated header, empty file and a solid
  value outside {0,1} all raise `VtkParseError` rather than misbehaving.
- Ranges and speeds checked against independently computed reference values.
- Solver discovery: the extension-less solver is found beside the UI, a
  non-executable one is refused with a message that says so, and a configured
  path is used when nothing sits beside the UI.
- Generated 30-name release matrix compared exactly against the solver
  `release/0.1` archive set: 30/30, no missing or extra rows.
- Linux x64 release rows built and packaged end to end through
  `scripts/build-ui-release.py`.

Not performed here:

- Windows and macOS builds;
- GUI launch and interactive visual verification (the container has no display).
