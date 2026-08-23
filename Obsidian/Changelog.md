# Changelog

## 2026-08-24 00:08 — Fix manual-build guidance and Linux prompts

Status:
- Written
- Tested: static workflow assertions

Changed:
- `C:\Users\alans\3D Objects\.git\Solve this problem\CFD-Solver-2D-GUI-part_2\CFD-Solver-2D-GUI-part_2\.github\workflows\build-ui-all.yml`
  - Documented that `workflow_dispatch` must exist on GitHub's default branch for the **Run workflow** button to appear.
  - Added `TZ=Etc/UTC` to both noninteractive Linux package installations.
  - Renamed the visible step from `aarch64` to `arm64`; valid GNU `aarch64-*` tool identifiers remain unchanged.

Cause:
- The workflow exists only in the UI-branch package, while GitHub requires manually dispatched workflows to exist on the repository's default branch.
- The Linux dependency install could invoke `tzdata` without an explicit timezone.
- The user-facing architecture label did not match the release's `arm64` naming.

Effect:
- Linux dependency installation has a deterministic noninteractive UTC timezone.
- The workflow displays `arm64` while retaining correct compiler names.
- Default-branch placement is explicit instead of being incorrectly described as UI-branch-only.

Validation:
- Exact workflow assertions: `workflow_dispatch=1`, UTC/noninteractive installs `=2`, `arm64` label `=1`, old label `=0`, tabs `=0`.
- Result: passed.

Remaining:
- The extracted package has no `.git` metadata or remote. The workflow must still be committed to the actual GitHub default branch, with write access, before the **Run workflow** button can be verified.

## 2026-08-21 17:35 — Add full UI release matrix and ./output default

Status:
- Written
- Tested

Changed:
- `Source/src/Application.cpp`
  - Changed the fresh default output root from the UI executable directory's `output` folder to `./output` relative to the process working directory.
  - Resolves that root to an absolute path before a solver run so the current Fluid Solver absolute-`outputDir` requirement remains satisfied.
  - Preserves existing preference/config overrides.
- `Source/CMakeLists.txt`
  - Added `CFD_UI_STATIC_RUNTIME` (default ON).
  - Added `CFD_UI_ENABLE_AVX2`.
  - Forced static SFML and bundled static FreeType/HarfBuzz/SheenBidi dependency builds.
  - Uses static MSVC CRT on Windows and static libgcc/libstdc++ on Linux/GCC where supported.
  - Renamed the produced desktop executable to `Fluid Solver UI[.exe]`.
- `Source/scripts/build-ui-release.py`
  - Added one cross-platform release builder containing the exact 30-row solver release matrix.
  - Builds only distinct architecture/ISA binaries and fans them out into matching OpenMP/CUDA package names because those are solver features, not UI kernels.
  - Creates archives named `Fluid Solver <version> <system>-<arch> <feature>-ui.zip`.
  - Writes an empty `output/` folder and preserves executable Unix ZIP modes.
  - Can list the matrix and finalize a merged release with exact-row verification, UI source archive, and SHA-256 checksums.
- `Source/.github/workflows/build-ui-all.yml`
  - Added native Windows, Linux, macOS arm64, and macOS x64 build jobs.
  - Added merged release verification/finalization.
- `Source/README.md`, `Source/BUILD_INFO.md`, `Source/Obsidian/*`
  - Documented the release matrix, static-linking limits, package semantics, new executable name, output default, validation, and remaining work.

Cause:
- The finished solver release already has 30 architecture/feature rows and its installer logic pairs a UI row by the same feature name plus `-ui`, but the UI package had no builder that produced those rows.
- The UI build used static SFML but still used the default MSVC CRT/runtime policy and had no AVX2 release switch.
- The requested default VTK/output location is `./output`.

Effect:
- The UI source can now generate the exact solver-corresponding archive set needed by the existing release/installer naming contract.
- AVX2 rows contain AVX2 UI machine code; non-AVX2 rows remain baseline.
- OpenMP/CUDA labels no longer imply unnecessary UI runtime dependencies; they exist only to pair the UI with the corresponding solver row.
- Fresh runs save under `./output/run-<timestamp>/...` unless an existing user preference/config intentionally changes the output root.
- No Fluid Solver source or solver binary changed.

Validation:
- `python3 -m py_compile Source/scripts/build-ui-release.py`
  - Result: passed.
- `python3 Source/scripts/build-ui-release.py --version 0.1 --list`
  - Result: exactly 30 expected archive names.
- Programmatic comparison with provided `CFD-Solver-2D/release/0.1`
  - Result: 30/30 generated UI row names match the solver binary archive set with `-ui` appended; no missing/extra rows.
- ZIP packaging smoke test
  - Result: top-level row directory, `Fluid Solver UI`, documentation, `output/`, mode 0755 for executable/directories and 0644 for text files.
- C++17 syntax check of all `Source/src/*.cpp`
  - Result: passed.
- `FluidSolverRunTests`, `GeometryProcessorTests`, `VtkFrameTests`
  - Result: passed.
- CMake configure with locally available fetched SFML text/font dependencies
  - Result: project/CMake changes parsed; configure stopped at missing Linux X11 development components `Xrandr`, `Xcursor`, `Xi`.

Remaining:
- Run the full GitHub Actions release workflow and verify all 30 archives are built on the four native runner classes.
- Build/launch Windows x64/x86 UI binaries and inspect runtime dependencies.
- Build/launch Linux x64/x86 and macOS arm64/x64 UI binaries.
- Verify each UI row pairs with the corresponding finished solver row in the installer.


## 2026-08-21 12:26 — Align optimized UI hooks with current Fluid Solver

Status:
- Written
- Tested: focused adapter/parser tests

Changed:
- `Source/include/FriendRun.hpp`
  - Removed active `restart`, `restartFile`, `addTime`, `gravityEnabled`, `gravityAccel`, and `gravityAngle` fields from the current solver launch configuration.
  - Added `dtUpdateInterval`, `dtSafety`, `smootherOmega`, `mgIterations`, `mgTolerance`, `mgMinCoarseSize`, `saveInterval`, and `useCuda` with current solver defaults.
- `Source/src/FriendRun.cpp`
  - Replaced optional-gravity/restart argument generation with the current 24-key `Config::setParam()` contract.
  - Added validation for the newly exposed numerical/output controls.
  - Required an absolute `outputDirectory` so current `Fluid Solver.exe` cannot reinterpret `.` relative to its own executable directory.
- `Source/src/Application.cpp`
  - Removed gravity and add-time sliders from the active setup UI.
  - Kept `ro` as density and changed the visible label to `Density ro`.
  - Added current solver controls for VTK save interval, CUDA backend, dt update interval/safety, coarse SOR omega, multigrid smoother omega, V-cycles, tolerance, and minimum coarse size.
  - Split controls into Basic/Advanced pages so the larger current parameter set fits the existing left panel.
  - New runs now pass the absolute timestamped run directory as `outputDir`.
  - Disabled current continuation launch while retaining the VTK restart parser/state model for the planned future restart-capable solver.
  - Future restart metadata, when present, can populate current numerical controls instead of the obsolete gravity controls.
- `Source/tests/FriendRunTests.cpp`
  - Replaced old gravity/restart contract tests with exact current-hook coverage, obsolete-key rejection, and absolute-output validation.
- `Source/tests/SolverCompatibilityTests.cpp`
  - Replaced old live continuation expectation with a fresh current-solver run/VTK parse expectation.
- `Source/tests/VtkFrameTests.cpp`
  - Kept future `RestartData` parsing coverage but changed synthetic config metadata from gravity keys to current solver keys.
- `Source/README.md`
  - Documented current hooks, Basic/Advanced controls, absolute output behavior, `ro`, and deferred VTK continuation.
- `BUILD_INFO.md`
  - Marked packaged Release binaries as historical/not rebuilt and recorded actual validation performed for this source update.

Cause:
- The optimized UI targeted `CFD-Solver-2D-optional-gravity-update`, while current `CFD-Solver-2D-main` exposes a different command-line configuration surface and produces `Fluid Solver.exe`.
- Current main has no gravity or VTK restart CLI hooks, but does expose eight numerical/output/backend parameters that the old UI could not send.
- Current solver resolves relative output paths against its executable directory, so `outputDir=.` could place results outside the UI-created run folder.

Effect:
- New-run UI arguments now match the current friend's solver input surface without changing the solver itself.
- All current solver controls are available from the UI while keeping the normal page compact.
- Gravity is no longer presented or emitted.
- VTK continuation remains architecturally preserved but cannot be launched against a solver that does not support restart yet.
- `CFD-Solver-2D-main.zip` and all solver source files remain unchanged.

Validation:
- `g++ -std=c++17 -Wall -Wextra -pedantic -Iinclude src/FriendRun.cpp tests/FriendRunTests.cpp ...`
- Result: `FriendRunTests` passed.
- `g++ -std=c++17 -Wall -Wextra -pedantic -Iinclude src/VtkFrame.cpp tests/VtkFrameTests.cpp ...`
- Result: `VtkFrameTests` passed.
- `g++ -std=c++17 -Wall -Wextra -pedantic -Iinclude -I<extracted SFML 3 headers> -DCFD_SOLVER_EXE=\"\" -fsyntax-only src/Application.cpp`
- Result: syntax check passed; one pre-existing non-Windows unused-parameter warning in `chooseSolverExecutable`.
- Visual Studio 18 2026 build: not performed.
- GUI launch/manual visual verification: not performed.
- Live Windows `Fluid Solver.exe` run: not performed.

Remaining:
- Build and launch the updated GUI on Windows/Visual Studio 18 2026.
- Run the updated `SolverCompatibilityTests` against a current built `Fluid Solver.exe`.
- Implement actual VTK-state continuation only when the friend solver gains a restart contract.
- Current solver multi-object handling remains a separate solver-side limitation and was not touched.


## 2026-08-21 12:16 — Current solver executable filename

Status:
- Written

Changed:
- `Source/src/FriendRun.cpp`
  - Changed the automatic adjacent-solver fallback from `cfd_app.exe` to `Fluid Solver.exe`.
- `Source/CMakeLists.txt`
  - Changed automatic solver discovery candidates and the post-build bundled filename from `cfd_app.exe` to `Fluid Solver.exe`.
- `Source/README.md`
  - Updated solver-path examples to `Fluid Solver.exe`.

Cause:
- Current `CFD-Solver-2D-main` keeps the CMake target name `cfd_app` but sets its produced file name through `CFD_APP_NAME` / `OUTPUT_NAME` to `Fluid Solver`. The optimized UI still searched for the old on-disk name.

Effect:
- A newly built optimized UI source can automatically discover/bundle the current solver under its actual Windows filename, while runtime manual EXE selection remains available.
- No solver source was changed.
- The historical `Release/cfd_app.exe` included in this old package was not renamed or replaced; it belongs to the older optional-gravity solver package.

Validation:
- Static source inspection: all automatic-discovery/bundle references in `Source/src/FriendRun.cpp`, `Source/CMakeLists.txt`, and `Source/README.md` now use `Fluid Solver.exe`.
- Build: not performed.
- Launch: not performed.
- Tests: not performed.

Remaining:
- Update the old optional-gravity/restart argument hooks to the current `CFD-Solver-2D-main` parameter contract before treating this package as current-solver compatible.
- Pass an absolute run output directory; current solver resolves relative `outputDir` against its executable directory.


## 2026-08-17 09:18 — Optional-gravity solver compatibility and continuation

Status:
- Written
- Built
- Launched: friend solver only
- Tested
- Verified: new-run and continuation adapter contract

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\include\FriendRun.hpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\src\FriendRun.cpp`
  - Added `restart`, `restartFile`, `addTime`, `gravityEnabled`,
    `gravityAccel`, and `gravityAngle`.
  - New runs send gravity parameters.
  - Continuation sends only restart/add-time/gravity/output overrides so the
    VTK-restored configuration remains authoritative.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\include\VtkFrame.hpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\src\VtkFrame.cpp`
  - Added binary `FIELD RestartData` parsing, config/time/solve metadata, exact
    restart-state size validation, continued-frame filename support, and
    seek-based skipping of unused state payloads.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\include\GeometryProcessor.hpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\src\GeometryProcessor.cpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\include\SectionAdapter.hpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\src\SectionAdapter.cpp`
  - Preserved all deduplicated simple disconnected slice loops.
  - Rasterized their union and wrote one normalized OBJ component/group per
    contour while preserving relative placement.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\src\Application.cpp`
  - Added solver-matched gravity/add-time controls, continuation launch,
    restart-only enablement, output-root selection, unique run children,
    stored-time progress text, missing-solver warning/gating, and distinct
    new-run/continuation result validation.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\CMakeLists.txt`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\README.md`
  - Separated source from build output, added dependency-root discovery,
    pinned SFML 3.1.0 build-tree fallback, optional solver bundling, runtime
    solver selection, default-off tests, and VS 2026 commands.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\tests\FriendRunTests.cpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\tests\VtkFrameTests.cpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\tests\GeometryProcessorTests.cpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CFD-Solver-2D-GUI-part\tests\SolverCompatibilityTests.cpp`
  - Added focused unit and live friend-solver compatibility coverage.

Cause:
- The optional-gravity solver added gravity/add-time inputs and binary restart
  fields, while the GUI rejected `FIELD` and recognized only new-run filenames.
- The prior contour pipeline retained only the largest loop.
- The prior CMake package required a bundled/stale solver and absent test/SFML
  sources, preventing a clean source-only build.

Effect:
- New and continued simulations use the friend solver's current argument and
  VTK contracts.
- Continued VTK names such as `solution_3_6.vtk` load and sort by solve 6.
- Multiple disconnected section objects reach both mask and solver adapter.
- A friend EXE can be selected/replaced without rebuilding the GUI.
- Builds remain outside the code directory.

Math:
- Not applicable. No governing CFD equation, discretization, convergence
  criterion, or solver numerical method was changed by this GUI work.

Validation:
- Solver configure: `cmake -S . -B ..\CFD-Solver-2D-optional-gravity-update_build -G "Visual Studio 18 2026" -A x64`
- Solver build: `cmake --build ..\CFD-Solver-2D-optional-gravity-update_build --config Release --parallel`
- Result: built with MSVC 19.51, CUDA 13.3, OpenMP, and AVX2.
- GUI test build: configured with Visual Studio 18 2026, explicit current
  solver EXE, and `BUILD_TESTING=ON`; Release build succeeded.
- Tests: `ctest --test-dir ..\CFD-Mask-UI-Optimized_build -C Release --output-on-failure`
- Result: 4/4 passed.
- Live compatibility result: new two-object run produced restart-capable VTK;
  continuation restored the mask/config and advanced from `t=0.01 s`, solve 3
  to `t=0.02 s`, solve 6.
- Fresh source-only GUI configure/build: no manual dependency-root or solver
  path; Release executable built successfully with runtime solver selection.

Remaining:
- GUI window layout and button interaction were compiled but not manually
  visually exercised.
- Nested contours are treated as a union of solids; cavity/hole semantics were
  not requested or implemented.
- Large VTK-series scrolling/RAM and solver throughput were not benchmarked.
  No 50,000% speed claim is supported.

## 2026-08-21 13:35 — Make GUI build independent of solver source

Status:
- Written

Changed:
- `Source/CMakeLists.txt`
  - Removed the mandatory `CFD_ROOT_DIR` solver/dependency checkout requirement.
  - Removed solver-owned `stl_reader` include/test-data coupling from the GUI build.
  - Uses GUI-owned `third_party/tinyobjloader` for the OBJ loader header.
  - Uses GUI-owned `third_party/sfml` before the existing pinned FetchContent fallback.
  - Keeps `CFD_SOLVER_EXE` optional; a missing solver no longer blocks GUI configuration/build.
- `Source/third_party/tinyobjloader/tiny_obj_loader.h`
  - Added the GUI's direct tinyobjloader dependency to the GUI package.
- `Source/third_party/sfml/`
  - Added the SFML source dependency to the GUI package so CMake GUI does not need the solver checkout or network access.
- `Source/README.md`
  - Replaced old `CFD_ROOT_DIR` instructions with CMake GUI-only build steps.
- Package layout
  - Removed historical `Release/cfd_mask_ui_optimized.exe` and `Release/cfd_app.exe` from the new GUI-only package to prevent launching stale binaries.

Cause:
- The previous GUI CMake configuration failed before generation because it treated headers/SFML stored under the friend's solver checkout as mandatory GUI dependencies.
- The GUI should be buildable by itself, while the solver remains a read-only runtime dependency selected separately.

Effect:
- Configuring the GUI no longer requires `CFD-Solver-2D-main`, `CFD_ROOT_DIR`, or any solver source folder.
- `Fluid Solver.exe` remains optional at build time and selectable at runtime.
- No CFD solver source file was changed.

Validation:
- CMake parsed the rewritten standalone GUI configuration past the former `CFD_ROOT_DIR` failure and entered bundled SFML configuration.
- Full configure could not complete in this Linux validation environment because its X11 development packages (`Xrandr`, `Xcursor`, `Xi`) are absent; this is platform-specific and does not validate the requested Windows Visual Studio build.
- Solver archive/source was only read to copy third-party dependency sources; it was not modified.

Remaining:
- Visual Studio 18 2026 Release build and GUI launch must be performed on Windows.

## 2026-08-21 14:02 — Unify and scroll solver parameters

Status:
- Written
- Tested

Changed:
- `Source/src/Application.cpp`
  - Removed the Basic/Advanced parameter-page split.
  - Shows all 21 user-editable current Fluid Solver controls in one parameter list.
  - Added independent mouse-wheel scrolling when the pointer is over the left parameter panel.
  - Added a visible scrollbar with click and drag interaction.
  - Preserved mouse-wheel zoom behavior over the 3D setup viewport.
  - Added scroll-aware layout, hit testing, and row visibility so off-screen controls cannot receive slider input.
- `Source/README.md`
  - Documented the unified scrollable parameter panel and the distinction between slider controls and workflow-generated solver arguments.

Cause:
- The prior UI already contained the seven advanced timestep/multigrid controls, but hid them behind the `Parameters: Basic/Advanced` page switch.
- On a short window, the Basic page filled the panel and made the current solver controls appear incomplete.
- The left parameter panel had no scrolling behavior; the mouse wheel only zoomed the 3D preview when used over that viewport.

Effect:
- The left panel now exposes every current user-editable solver control through one scrollable list.
- The following controls are no longer hidden behind a second page: `dtUpdateInterval`, `dtSafety`, `omega`, `smootherOmega`, `mgIterations`, `mgTolerance`, and `mgMinCoarseSize`.
- Existing `saveInterval` and `useCuda` controls remain in the same unified list.
- No CFD solver source or numerical method was changed.

Validation:
- C++17 syntax check:
  `g++ -std=c++17 -fsyntax-only -DCFD_SOLVER_EXE='""' -ISource/include -ISource/third_party/sfml/include -ISource/third_party/tinyobjloader Source/src/Application.cpp`
- Result: passed.
- `FriendRunTests` compiled and ran against `Source/src/FriendRun.cpp`.
- Result: passed.
- Windows Visual Studio 18 2026 executable build and manual scrollbar interaction test were not performed in this environment.

Remaining:
- Build the GUI with Visual Studio 18 2026 on Windows and visually verify wheel scrolling, scrollbar dragging, and all 21 controls at the target window sizes.


## 2026-08-21 15:43 — Complete current Fluid Solver UI integration and safety pass

Status:
- Written
- Tested

Changed:
- `Source/src/Application.cpp`
  - Added explicit multi-contour safety before simulation launch.
  - Added largest-contour-only confirmation and synchronized preview/adapter behavior.
  - Added selected Fluid Solver identity/version/build inspection and display.
  - Added CPU-only versus CUDA-capable UI behavior and disabled unavailable CUDA requests.
  - Renamed the process action to `Stop simulation` and added active-run exit confirmation.
  - Reorganized the scrollable parameter panel by purpose without hiding solver controls.
  - Added integer +/- controls, parameter help, and invalid-input highlighting.
  - Added derived `dx`, `dy`, cell-count, Reynolds, approximate timestep/VTK-count, and MG-level information.
  - Added large-grid warnings.
  - Added persistent output-root selection.
  - Added `.cfdui` Save/Load configuration workflow.
  - Added run metadata recording with UI version, solver identity, selected paths, contour policy, and numerical parameters.
  - Added configurable decoded VTK cache size.
  - Added result `u`/`v` inspection, frame keyboard navigation, playback, global/current frame ranges, and Run details viewer.
  - Kept continuation disabled until the solver exposes a real restart contract.
- `Source/include/FluidSolverRun.hpp`
  - Replaced the old friend-solver naming with the current Fluid Solver launch interface.
- `Source/src/FluidSolverRun.cpp`
  - Replaced the old friend-solver naming with the current Fluid Solver launch implementation while preserving the current 24-key fresh-run contract.
- `Source/tests/FluidSolverRunTests.cpp`
  - Replaced the old friend-solver test target and retained exact current-CLI validation.
- `Source/CMakeLists.txt`
  - Updated the source/test target names from `FriendRun` to `FluidSolverRun`.
- `Source/include/GeometryProcessor.hpp`
- `Source/src/GeometryProcessor.cpp`
- `Source/tests/GeometryProcessorTests.cpp`
  - Added geometry support/test coverage required for explicit largest-contour UI behavior.
- `Source/include/VtkFrame.hpp`
- `Source/src/VtkFrame.cpp`
- `Source/tests/VtkFrameTests.cpp`
  - Added result vector-component access used by UI pixel inspection while retaining current VTK parsing.
- `Source/tests/SolverCompatibilityTests.cpp`
  - Updated fresh-run compatibility test naming/interface to `FluidSolverRun`.
- `Source/README.md`
  - Documented the complete current UI revision and validation state.
- `Source/Obsidian/Current State.md`
- `Source/Obsidian/Decisions.md`
- `Source/Obsidian/Errors.md`
- `Source/Obsidian/Tasks.md`
  - Added current-state, architectural decision, known-error, and remaining-task documentation.

Cause:
- The finished Fluid Solver builds use the current 24-parameter fresh-run interface, while the UI still contained development-era naming and lacked several correctness/diagnostic protections around solver selection, multi-contour geometry, backend capability, input validation, and result workflow.
- The current solver accepts only one closed contour, so silently sending multiple UI contours can make the solver geometry differ from the GUI preview.

Effect:
- The UI now protects the current solver boundary instead of changing it.
- Multi-contour geometry cannot be silently launched as if every contour were solved.
- CPU/CUDA intent, selected solver identity, run arguments, output location, and derived numerical information are visible to the user.
- Result navigation and diagnostics require fewer external file inspections.
- No Fluid Solver source, binary, numerical equation, discretization, multigrid implementation, or finished release package was modified.

Validation:
- Syntax command:
  `g++ -std=c++17 -fsyntax-only -DCFD_SOLVER_EXE='""' -DCFD_MASK_UI_VERSION='"0.1.0"' -Iinclude -Ithird_party/sfml/include -Ithird_party/tinyobjloader src/<each .cpp>`
- Result: every `src/*.cpp` passed C++17 syntax checking.
- Test command:
  `g++ -std=c++17 -O2 -Iinclude tests/FluidSolverRunTests.cpp src/FluidSolverRun.cpp ... && FluidSolverRunTests`
- Result: passed.
- Test command:
  `g++ -std=c++17 -O2 -Iinclude -Ithird_party/tinyobjloader tests/GeometryProcessorTests.cpp src/GeometryProcessor.cpp src/SectionAdapter.cpp src/tiny_obj_loader_impl.cpp ... && GeometryProcessorTests`
- Result: passed.
- Test command:
  `g++ -std=c++17 -O2 -Iinclude tests/VtkFrameTests.cpp src/VtkFrame.cpp ... && VtkFrameTests`
- Result: passed.

Remaining:
- Build `cfd_mask_ui_optimized` with Visual Studio 18 2026 in `Release | x64` on Windows.
- Launch and visually verify the new controls, dialogs, scrolling, playback, and Run details UI.
- Run a live fresh simulation against both finished Windows solver distributions.
- Continuation remains intentionally unavailable until the solver has a real restart interface.
