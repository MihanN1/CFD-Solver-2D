# Changelog

## 2026-07-19 19:19 — Separate GUI from friend solver

Status:
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI`
- Replaced the embedded `.maskjob` worker with an external `cfd_app.exe`
  console-input adapter.
- Removed direct compilation of friend `Mesh.cpp` and `Solver.cpp`.
- Removed GUI-only wind angle, mask target-fraction input, and unused boundary
  cell count.
- Kept the GUI preview mask separate from the authoritative friend VTK mask.

Cause:
- The GUI and friend solver must remain separate projects without source or
  binary merging.

Effect:
- The GUI launches the unchanged friend executable in a unique run directory,
  redirects its input/output/error streams, loads its VTK output, and survives
  a friend-process failure.

Validation:
- Build: `cmake --build build --parallel`
- Test: `ctest --test-dir build --output-on-failure`
- Result: all 3 tests passed.
- Runtime: launched `Car.obj` with `totalTime = 0`.
- Result: friend solver exited successfully and wrote `solution_0.vtk`.
- Visual: Results view rendered the friend mask and reported that it differs
  from the GUI preview.

Remaining:
- The current friend solver has positive-time staggered-array boundary
  indexing defects; fix them only in the separate friend project.
- Reconcile the GUI preview algorithm with the friend mask algorithm if an
  exact pre-run preview is required.

## 2026-07-19 19:48 — Make setup rotation visible and map WASD to rotation

Status:
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`
- Mapped W/S to positive/negative Slice X rotation and A/D to
  negative/positive Slice Z rotation while Setup has focus.
- Preserved Results-mode WASD panning.
- Applied Slice Rotation to the displayed model about the unchanged section
  normal, matching the 2D contour rotation used by the GUI and friend solver.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`
- Documented controls, cause/effect, and Rodrigues rotation mathematics.

Cause:
- Setup WASD translated the preview instead of rotating it.
- Slice Rotation changed the generated 2D contour and solver input but did not
  transform any visible Setup geometry.

Effect:
- W/S and A/D now rotate the section using the same signs as the arrow keys.
- Q/E, right-drag, Shift + left-drag, and the Slice Rotation slider visibly
  rotate the model in its section plane.
- The section plane normal and fixed friend-inlet arrow remain unchanged by
  Slice Rotation.

Validation:
- Build: `cmake --build build --parallel`
- Result: GUI compilation and linking succeeded.
- Test: `ctest --test-dir build --output-on-failure`
- Result: all 3 tests passed.
- Runtime: launched `Car.obj` in the rebuilt GUI.
- W held for 0.70 s changed Slice X from `0.00 deg` to `49.89 deg` and visibly
  tilted the section plane.
- E held for 0.70 s changed Slice Rotation from `0.00 deg` to `49.47 deg` and
  visibly rotated the model while the plane and fixed arrow remained stable.

Remaining:
- S/A/D mappings are built and source-inspected; only W was exercised at
  runtime.
- Focus gating remains implemented but was not retested in this change; there
  is no automated window-input test boundary.

## 2026-07-19 20:54 — Clarify the section-plane intersection

Status:
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\GeometryProcessor.hpp`
- Added the `SectionSegment` value type and read-only section-segment query.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\GeometryProcessor.cpp`
- Extracted triangle-plane intersection into one reusable implementation used
  by both the preview and 2D contour builder.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`
- Cached section segments by Slice X/Z.
- Drew the intersection with a dark halo and bright orange centre above the
  translucent blue plane.
- Added a legend for the section plane, mesh-plane intersection, and fixed
  flow arrow.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\GeometryProcessorTests.cpp`
- Added nonempty-intersection, plane-equation, and in-plane-rotation-invariance
  checks.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`
- Documented the indicator meaning, tolerance equation, and largest-loop mask
  behavior.

Cause:
- A translucent plane alone did not identify the exact geometry extracted from
  the 3D model.

Effect:
- The orange contour now shows the physical mesh-plane intersection directly
  on the model.
- The preview and mask builder cannot drift to separate edge-intersection
  formulas because both consume the same 3D segments.
- Repeated frames reuse cached segments until Slice X, Slice Z, or the model
  changes.

Validation:
- Build: `cmake --build build --parallel`
- Result: GUI compilation and linking succeeded.
- Test: `ctest --test-dir build --output-on-failure`
- Result: all 3 tests passed.
- Runtime: launched `Car.obj` in the rebuilt GUI.
- Direct window capture verified the orange contour coincides with the model
  and blue plane and the legend is readable.

Remaining:
- Coplanar triangles retain the existing intersection behavior.
- The orange preview shows all mesh-plane segments; mask rasterization retains
  only the largest closed loop for disconnected intersections.

## 2026-07-19 21:39 — Support exact Car section at X 90 and Z 90

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\GeometryProcessor.cpp`
- Replaced global degree-two contour rejection with simple planar-face
  extraction.
- Added guarded closure of a single open path and guarded one-segment terminal
  hook trimming.
- Moved span normalization after invert and in-plane rotation to match the
  friend solver.
- Matched the GUI boundary-cell rasterizer to the friend's centre-distance
  criterion.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\SectionAdapter.hpp`
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\SectionAdapter.cpp`
- Added canonical thin-OBJ generation from the selected GUI contour.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`
- Made Slice X and Slice Z start at exact 90 degrees and made their direct
  sliders whole-degree controls.
- Added cardinal-angle snapping for mouse and keyboard rotation.
- Preserved requested parameters separately, generated a section adapter, and
  launched the unchanged friend solver with neutral adapter transforms.
- Blocked positive total time before launch because the unchanged friend
  corrupts heap memory during positive-time stepping.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests`
- Added branched-section, real exact-90 Car, adapter round-trip, and external
  friend-adapter integration coverage.

Cause:
- The exact Car section has 447 graph nodes and 445 edges in two open path
  components, so no closed cycle exists.
- The unchanged friend solver silently substituted its circle when it received
  that original open section.
- GUI and friend boundary rasterizers used different discrete criteria.

Effect:
- Exact X 90 and Z 90 now produce a guarded repaired Car contour.
- The friend receives a derived closed adapter without any source merge or
  friend-code modification.
- At 50 by 50 cells, the GUI repaired mask equals the solid array parsed from
  the friend's generated VTK.
- Positive-time input now produces a clear GUI validation message instead of
  launching the unsafe friend timestep loop.
- Every run preserves original requested parameters in `requested-input.txt`
  and actual neutral adapter input in `solver-input.txt`.

Validation:
- Build: `cmake --build build --config Release`
- Result: GUI and all test executables compiled and linked.
- Test: `ctest --test-dir build -C Release --output-on-failure`
- Result: all 5 tests passed.
- Integration: `Car.obj`, X 90, Z 90, rotation 0, 50 by 50 cells,
  `totalTime = 0`.
- Result: friend exit code 0, 398 section points, `solution_0.vtk` written and
  parsed, GUI and VTK solid arrays equal.
- Runtime: rebuilt GUI launched with `Car.obj`, loaded 67750 triangles, and
  visibly displayed exact Slice X `90 deg` and Slice Z `90 deg`.

Remaining:
- The repaired contour discards the smaller disconnected open path.
- The repair adds a closure edge and removes one short terminal hook; it is not
  an exact closed section from the source mesh.
- Automated desktop mouse injection did not exercise the actual SFML Run
  button; the same adapter-to-friend pipeline is covered by the passing external
  integration test.
- Positive-time simulation remains unavailable until the separate friend's
  staggered-array boundary indexing defect is fixed.

## 2026-07-23 22:13 — Direct numeric input and durable friend run

Status:
- Written: GUI source, tests, CMake integration, and documentation updated.
- Built: Release GUI and bundled current-source friend executable.
- Launched: rebuilt GUI opened with `Car.obj` and remained responsive.
- Tested: all six CTest targets passed; desktop numeric input was exercised.
- Verified: typed wind speed `2.5` reached solver-input field 5 and completed.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\NumericInput.hpp`: added numeric-input rules and parsing/edit declarations.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\NumericInput.cpp`: added finite decimal/scientific parsing and inline value editing.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\NumericInputTests.cpp`: added accepted/rejected numeric-entry tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\FriendRun.hpp`: declared runtime solver resolution.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\FriendRun.cpp`: prefers adjacent `cfd_app.exe`, then the configured path.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\FriendRunTests.cpp`: tests adjacent priority, fallback, and missing-path diagnostics.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`: removed wind direction; added double-click editing, exact validation, solver existence checks, and VTK progress.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\CMakeLists.txt`: builds numeric-input code/tests and copies the validated friend executable beside the GUI.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`: documents the new controls, executable resolution, and run ownership.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\out\manual\cfd_app.exe`: installed the rebuilt current-source Release executable.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\Obsidian\Changelog.md`: recorded this change and its evidence.

Cause:
- Wind direction existed only as a GUI-derived mask rotation; the friend inlet has no direction input.
- Slider-only entry prevented exact or out-of-track values.
- A compile-time temporary solver path broke Run when that temporary directory disappeared.
- Expensive runs had no live progress and therefore appeared inactive.

Effect:
- Slice/object rotation now determines the mask directly; no wind-angle subtraction remains.
- Double-clicking a displayed value replaces it in place with validated typed input.
- The GUI owns an adjacent solver copy; removing the configured temporary path no longer breaks Run.
- Active runs report saved VTK frame count; closing the GUI still cancels its child process.

Math:
- Solver continuous equations and discrete operators are unchanged.
- The UI mask mapping is now direct:
- ![](https://latex.codecogs.com/svg.image?\phi_{\mathrm{mask}}=\phi_{\mathrm{sliceRotation}})
- The generated OBJ already contains this rotation, so friend adapter transformations remain zero.

Validation:
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCFD_SOLVER_EXE="C:\Users\alans\3D Objects\.git\CFD-Solver-2D\out\manual\cfd_app.exe"`.
- Build: Visual Studio developer environment, then `cmake --build build`.
- Result: `cfd_mask_ui.exe`, bundled `cfd_app.exe`, and all test executables linked.
- Test: `ctest --test-dir build --output-on-failure`.
- Result: 6/6 passed: GeometryProcessor, VtkFrame, FriendRun, NumericInput, SectionAdapter, ExactNinetyIntegration.
- Runtime: double-clicked Wind speed, entered `2.5`, pressed Enter, then Run.
- Protocol: generated `solver-input.txt` field 5 equals exactly `2.5`.
- Solver result: output contains `Simulation complete`; `solution_0.vtk` exists; GUI remained responsive.
- GUI SHA-256: `EB4D11B954A0D3AA7A509CCF1C9ADDF090DFA946FF7EE3A9F261171FAE59ADA1`.
- Bundled solver SHA-256: `EEE5AA5517D203F32BF2D4AB1E37D06AABDE5A6479D132DD87B1EF40CBDCC852`.
- Installed solver SHA-256: `EEE5AA5517D203F32BF2D4AB1E37D06AABDE5A6479D132DD87B1EF40CBDCC852`.

Remaining:
- Positive-time numerical trust remains blocked by the friend's `v_star[idxV(nx,j)]` outlet alias.
- Typed values may exceed slider ranges; Run validation remains authoritative.
- Closing the GUI intentionally cancels any active friend solver.
- Captured screenshots were not visually inspected because the local image viewer was ACL-blocked.

## 2026-07-23 22:22 — Remove obsolete temporary friend executable

Status:
- Verified: the exact old executable was unreferenced, deleted, and confirmed absent.

Changed:
- `C:\Users\alans\AppData\Local\Temp\CFD-Solver-2D-current-20260723-201956\release\cfd_app.exe`: deleted the obsolete temporary executable only.

Cause:
- The old temporary executable remained after the GUI was switched to its adjacent current-source solver copy.

Effect:
- The obsolete friend binary can no longer be selected accidentally.
- The temporary directory and all unrelated files were preserved.

Validation:
- Pre-delete SHA-256: `5977388DD57F99442ED8B3F516BC368D6CF4FC95B1ED38A4F9D148F4CB805BA4`.
- Current GUI source and CMake cache contained no reference to the old temporary directory.
- Post-delete `Test-Path` returned false for the exact executable.

Remaining:
- The containing temporary build directory was not deleted.

## 2026-07-24 00:08 — Stop partial renders, velocity vectors, and current friend build

Status:
- Written: Stop control, partial-series recovery, vector overlay, tests, CMake integration, README, and changelog.
- Built: Release GUI and adjacent current-source friend executable.
- Launched: rebuilt GUI opened with solver result data and remained responsive.
- Tested: all seven CTest targets passed; current Release and AddressSanitizer solver smokes passed.
- Verified: installed and GUI-bundled friend executables match the newly rebuilt current-source SHA-256.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`: added Stop render ownership/termination, partial-frame loading, Vectors toggle, and arrow drawing.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\VtkFrame.hpp`: added recoverable-series result and parser interface.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\VtkFrame.cpp`: added ordered recovery that rejects truncated, duplicate, or layout-incompatible files while retaining complete compatible frames.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\VtkFrameTests.cpp`: added recovery coverage for valid, truncated, and incompatible frames.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\VelocityOverlay.hpp`: added vector-overlay configuration, arrow data, and planner ownership.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\VelocityOverlay.cpp`: added bounded cell sampling, direction normalization, magnitude normalization, and filtering.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\VelocityOverlayTests.cpp`: added direction, magnitude, filtering, cap, and invalid-input tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\CMakeLists.txt`: added vector-overlay source and seventh test target.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`: documented Stop, recoverable frames, vector controls, equations, and the current save interval.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_mask_ui.exe`: rebuilt Release GUI.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_app.exe`: bundled the new current-source friend executable.
- `C:\Users\alans\AppData\Local\Temp\CFDMaskUI\runs\run-78399858078200`: move-time recovery reported a complete move to `D:\CFDMaskUI-Recovery\run-78399858078200`; final audit no longer finds the run at either path.

Cause:
- The GUI could only wait for natural solver completion; aborting discarded access to already completed frames.
- Velocity magnitude did not expose flow direction.
- A validation run filled the system drive, so its complete output directory had to be relocated before compilation could continue.
- The GUI had to bundle the newest friend source build rather than the previous installed binary.

Effect:
- Stop render terminates only the child solver owned by the GUI, waits up to five seconds, closes its process handles, and opens every complete layout-compatible frame already saved.
- Truncated or incompatible tail files are skipped and reported; original VTK files are not modified.
- Vectors: Off/On overlays sampled in-plane velocity arrows in the Results view; solid, non-finite, and zero-magnitude cells are excluded.
- Moving the 11.4 GB run restored C: space at the time; its present location is unknown because final audit finds neither the source nor destination directory.
- Run friend solver resolves to the adjacent binary whose hash equals the installed current-source build.

Math:
- Solver continuous equations and discrete operators are unchanged.
- Screen-space direction is dimensionless:
- ![](https://latex.codecogs.com/svg.image?\mathbf{d}_{screen}=\frac{(u,-v)}{\sqrt{u^2&plus;v^2}})
- Relative magnitude and arrow length are:
- ![](https://latex.codecogs.com/svg.image?m=\operatorname{clamp}(\frac{\sqrt{u^2&plus;v^2}}{u_{ref}},0,1))
- ![](https://latex.codecogs.com/svg.image?\ell=10&plus;18\sqrt{m}\;\mathrm{px})

Numerical form:
- Sampling strides are `max(1, ceil(36 / cellWidthPixels))` and `max(1, ceil(36 / cellHeightPixels))`.
- Arrow count is capped at 2,000; direction is normalized only when the in-plane speed exceeds `1e-12`.
- Recoverable parsing preserves numerical step order and adopts the first complete frame as the required layout contract.

Validation:
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCFD_SOLVER_EXE="C:\Users\alans\3D Objects\.git\CFD-Solver-2D\out\manual\cfd_app.exe"`.
- Build: `cmake --build build`; result linked `cfd_mask_ui.exe` and bundled `cfd_app.exe`.
- Tests: `ctest --test-dir build --output-on-failure`; result 7/7 passed.
- Recovery test retained steps 0 and 3 while rejecting one truncated and one incompatible VTK.
- Solver Release smoke: exit 0, two frames.
- Solver AddressSanitizer smoke: exit 0, two frames, no sanitizer diagnostic.
- Smoke VTK: `CELL_DATA 100`, pressure 100, solid 100 with four solids, velocity 100 triples, non-finite values 0.
- GUI SHA-256: `06528D15B4B4D89E3BF7EB1EC2BAF3456057A13EC60926F71EA5C9CB1BDBEB85`.
- Installed and bundled friend SHA-256: `7B8F61FA7F9BBDE7513DCC845DE78CC564BCF824FEEF5718405E0D650B5FCCE7`.
- Move-time inventory: 11,416,587,608 bytes, 647 VTK files, including 243 files below 1 KiB. Final `Test-Path` is false for both the original Temp run and `D:\CFDMaskUI-Recovery\run-78399858078200`.

Remaining:
- End-to-end clicking of Stop could not be verified because Windows foreground ownership was nondeterministic during desktop automation; guarded attempts launched no solver and created no run directory.
- The current location of `run-78399858078200` is unknown; do not treat the recorded D: recovery path as available.
- The vector planner and control compiled and passed tests; screenshot capture could not isolate arrow pixels reliably, so the overlay is not visually verified.
- Positive-time numerical trust remains blocked by the existing friend `v_star[idxV(nx,j)]` outlet alias.

## 2026-07-29 22:12 — Current multigrid solver and binary VTK compatibility

Status:
- Written: GUI input protocol, controls, executable discovery, binary VTK parser, fixture discovery, and regression tests were updated.
- Built: Release GUI and all seven test executables linked successfully.
- Launched: `cfd_mask_ui.exe` opened with window title `CFD Mask UI` and closed through its main window.
- Tested: all seven CTest targets and one positive-time current-solver integration smoke passed.
- Verified: the GUI serialized the exact current 17-value solver protocol, parsed current binary frames, and bundled a byte-identical copy of the external Release solver.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\CMakeLists.txt`: added sibling `CFD-Solver-2D_build` executable discovery, CUDA `.cu`/`.cuh` freshness inputs, and current neighboring `CAR.obj` fixture fallback.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\FriendRun.hpp`: removed obsolete `tolerance` and `maxIterations` fields.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\FriendRun.cpp`: removed obsolete validation and serialized fixed unused `Re = 0` plus default `mgIterations = 2` in the current field order.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`: removed obsolete SOR tolerance/iteration controls and renamed active omega to `Multigrid omega`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\VtkFrame.cpp`: retained legacy ASCII parsing and added strict big-endian 32-bit binary float/int parsing with delimiter, type, truncation, and solid-mask validation.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\FriendRunTests.cpp`: changed protocol expectations from 18 values to the current 17 values and removed obsolete-field tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\VtkFrameTests.cpp`: added exact current-format binary frame, truncated-payload, and invalid-binary-mask tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\CMakeCache.txt`: configured `CFD_SOLVER_EXE` to the sibling external Release build.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_mask_ui.exe`: rebuilt Release GUI.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_app.exe`: replaced the stale July 23 bundle with the current July 29 solver.

Cause:
- Friend console input removed SOR tolerance and maximum-iteration fields and inserted `mgIterations`, so the old GUI shifted geometry and every later field into the wrong slot.
- Friend VTK output changed from ASCII to big-endian binary while retaining legacy structured-points `CELL_DATA`.
- The friend executable moved to the sibling `CFD-Solver-2D_build` directory.
- The car fixture moved from `CFD-Solver-2D\models\Car.obj` to the neighboring `CAR.obj`.
- CUDA source changes were absent from the GUI stale-solver guard.

Effect:
- Run now sends exactly 17 values plus one empty confirmation line.
- The GUI no longer exposes solver parameters that do not exist.
- Current `solution_<step>.vtk` files load directly as individual frames or ordered frame series.
- Fresh GUI builds find and bundle the current external solver by default.
- Changes to current CUDA source/header files can invalidate a stale selected solver during CMake configuration.

Math:
- Solver equations and numerical algorithms were not changed.
- Exported pressure remains density-scaled:
- ![](https://latex.codecogs.com/svg.image?p_{\mathrm{VTK}}=\rho\,p_{\mathrm{solver}})
- Exported cell-centered velocity remains:
- ![](https://latex.codecogs.com/svg.image?u_{i,j}^{c}=\frac{u_{i,j}&plus;u_{i&plus;1,j}}{2},\qquad%20v_{i,j}^{c}=\frac{v_{i,j}&plus;v_{i,j&plus;1}}{2})
- Displayed velocity magnitude remains:
- ![](https://latex.codecogs.com/svg.image?|\mathbf{u}_{i,j}|=\sqrt{(u_{i,j}^{c})^2&plus;(v_{i,j}^{c})^2})
- A four-byte big-endian payload word is reconstructed as:
- ![](https://latex.codecogs.com/svg.image?w=256^3b_0&plus;256^2b_1&plus;256b_2&plus;b_3)
- For solver `CELL_DATA`, the parser requires:
- ![](https://latex.codecogs.com/svg.image?N_{\mathrm{samples}}=n_xn_y,\qquad%20\mathrm{DIMENSIONS}=(n_x&plus;1,n_y&plus;1,1))

Numerical form:
- Binary `float` and `int` payload values are exactly four bytes and are decoded from VTK big-endian order into host 32-bit words.
- Pressure and velocity ranges exclude cells whose `solid` value is one.
- Non-finite pressure/velocity samples remain loadable but are excluded from ranges and recorded as warnings.
- Frame order remains the integer suffix in `solution_<step>.vtk`; the suffix is not interpreted as physical time.

Validation:
- Configure: `cmake -S "C:\Users\alans\3D Objects\.git\CFD-Mask-UI" -B "C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCFD_SOLVER_EXE="C:\Users\alans\3D Objects\.git\CFD-Solver-2D_build\bin\Release\cfd_app.exe"`.
- Build: initialized Visual Studio x64 environment, then `cmake --build "C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build" --parallel`; result succeeded.
- Initial direct build without `vcvars64.bat` failed before source compilation because MSVC standard-library include paths were absent; no code change was made for that environment error.
- Test: `ctest --test-dir "C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build" --output-on-failure`; result 7/7 passed.
- Positive-time smoke: grid 20 by 20, total time `0.001 s`; current solver exited zero, wrote `solution_0.vtk` and `solution_1.vtk`, and the integration test parsed finite final pressure/velocity with the expected solid mask.
- Retained evidence: `C:\Users\alans\AppData\Local\Temp\cfd_mask_ui_exact_ninety_66731624230500`.
- Serialized evidence: 17 value lines plus one empty confirmation line; geometry occupies field 12, density occupies field 17.
- Binary evidence: `BINARY`, `DIMENSIONS 21 21 1`, `CELL_DATA 400`; both frames are 8,245 bytes.
- GUI SHA-256: `9827623FAA76BD7A5462E1AB67D00FC5E494AACFD65B60FF39F581CA2B61B459`.
- External and bundled solver SHA-256: `7F0ED55A71AF9DD249CB8675F139554CCDE6D606A0EAB4BBFA3BBD0E79871361`.

Remaining:
- Positive-time interface compatibility is verified; numerical correctness of the friend solver is not verified by this GUI task.
- `Re` remains an unused friend input slot, so the GUI sends zero and exposes no control.
- `mgIterations` remains ignored by the friend solve call, so the GUI sends the documented default two and exposes no control.
- Friend wind direction remains fixed along positive X; the current friend interface has no direction parameter.
- GUI launch was verified, but mouse/keyboard interaction and resized-window visuals were not re-tested in this change.

## 2026-07-29 23:09 — VTK Explorer access and blocked interval rebuild

Status:
- Written: added the VTK Explorer target policy, top-bar control, Windows launch integration, tests, and user documentation.
- Built: `ExplorerTarget.cpp`, `ExplorerTargetTests.cpp`, and `Application.cpp` compiled; the targeted Explorer test executable linked.
- Tested: the new Explorer target test passed, and all seven tests in the previously configured CTest suite passed.
- The official solver and GUI package were not built because the current friend source has a preprocessor syntax error.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\include\ExplorerTarget.hpp`: added the selected-file or run-directory target value and selection interface.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\ExplorerTarget.cpp`: selected the displayed VTK first, fell back to the active run directory, and returned no target when neither exists.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\tests\ExplorerTargetTests.cpp`: added selected-file precedence, directory fallback, empty-state, and Unicode-path tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\src\Application.cpp`: added the responsive `Show VTK in Explorer` button, dynamic enabled state, Windows Explorer launch, selected-frame reveal, run-directory fallback, and status reporting.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\CMakeLists.txt`: compiled the target policy into `mask_ui_core` and registered `ExplorerTargetTests`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`: documented the current 17-value protocol, binary VTK support, external solver location, Explorer control, and source save interval.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\Obsidian\Changelog.md`: recorded this change and its exact validation boundary.
- Friend solver source was inspected and built but was not edited by Codex.

Cause:
- The user needs direct access to the folder containing generated VTK files and to the exact file currently displayed.
- The friend changed `SAVE_INTERVAL` from one to twenty and changed the pressure solve toward the CPU path.
- The latest CPU-path commit contains `#ifdef 0`, which MSVC rejects because `#ifdef` requires an identifier.

Effect:
- A loaded frame maps to an Explorer file-selection request.
- An active run with no loaded frame maps to its run-directory open request.
- The button is disabled when neither a frame nor a run directory exists.
- Drawing and hit testing share the same button bounds recomputed by the existing responsive layout.
- Sparse frames such as steps 0, 20, 40, and a non-multiple final step are already discovered and sorted numerically.
- No new solver hook, parameter, VTK transformation, or friend-source edit was introduced.

Math:
- Explorer interaction does not change the numerical method: not applicable.
- The current friend source requests this frame-index set:
- ![](https://latex.codecogs.com/svg.image?\mathcal{F}=\{0\}\cup\{n\in\mathbb{N}\mid n\bmod20=0\}\cup\{n_{\mathrm{final}}\})
- The filename suffix is solver step index `n`, not physical time.

Numerical form:
- Save the initial state at step zero.
- Save a periodic frame when `step % 20 == 0`.
- Save the final step unconditionally, even when its index is not divisible by twenty.
- The GUI orders frames by parsed integer suffix and does not require adjacent indices.

Validation:
- Solver build command: `cmake --build "C:\Users\alans\3D Objects\.git\CFD-Solver-2D_build" --config Release --target cfd_app --parallel`.
- Solver build result: failed at `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Multigrid.cpp:19` with MSVC `C1016: #ifdef expected an identifier`.
- Friend-side correction required before a current rebuild: replace `#ifdef 0` with `#if 0`; Codex did not apply it.
- Targeted MSVC compile: `ExplorerTarget.cpp`, `ExplorerTargetTests.cpp`, and `Application.cpp` compiled successfully.
- Targeted test: `ExplorerTargetTests passed`.
- Existing configured regression command: `ctest --test-dir "C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build" --output-on-failure --no-tests=error`; result 7/7 passed.
- Current external and bundled solver SHA-256 remains stale: `7F0ED55A71AF9DD249CB8675F139554CCDE6D606A0EAB4BBFA3BBD0E79871361`.

Remaining:
- Friend must correct `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Multigrid.cpp:19` before the current solver can build.
- Rebuild the external Release solver after that correction.
- Reconfigure and rebuild the GUI so the new button and byte-identical current solver are packaged together.
- Run all eight configured CTest targets.
- Launch the rebuilt GUI and verify disabled, active-run-directory, selected-frame, and resized-window Explorer interactions.
- Run enough solver steps to verify files at zero, each multiple of twenty, and the unconditional final step.

## 2026-07-29 23:19 — Current rebuild and VTK Explorer verification

Status:
- Built: the external Release solver, Release GUI, and all eight test executables built successfully.
- Launched: the rebuilt GUI opened the retained sparse-frame result directory.
- Tested: all eight CTest targets passed.
- Verified: external and bundled solver hashes match, interval-20 output produced steps 0, 20, and 21, and Explorer selected the displayed `solution_0.vtk`.

Changed:
- Friend corrected `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Multigrid.cpp:19` from `#ifdef 0` to `#if 0`; Codex did not edit friend source.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D_build\bin\Release\cfd_app.exe`: rebuilt from current friend working-tree source.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_mask_ui.exe`: rebuilt with the VTK Explorer control.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_app.exe`: replaced with the byte-identical current external solver.

Cause:
- The friend fixed the preprocessor syntax error that blocked the prior rebuild.

Effect:
- `Show VTK in Explorer` is present in the rebuilt GUI.
- The displayed VTK can be selected directly in Windows Explorer.
- The packaged solver now contains the current CPU-path selection and save interval.

Math:
- Explorer interaction: not applicable.
- Verified save-index set for the bounded cadence run:
- ![](https://latex.codecogs.com/svg.image?\mathcal{F}=\{0,20,21\})
- Zero-inlet cadence-test timestep:
- ![](https://latex.codecogs.com/svg.image?\Delta%20t=\frac{1}{2\nu\left(\Delta%20x^{-2}&plus;\Delta%20y^{-2}\right)}=1\,\mathrm{s})

Numerical form:
- Parameters: `Lx = Ly = 10 m`, `nx = ny = 50`, `U0 = 0 m/s`, `nu = 0.01 m^2/s`, `totalTime = 21 s`.
- Solver exit: zero.
- Generated files: `solution_0.vtk`, `solution_20.vtk`, `solution_21.vtk`.

Validation:
- External solver SHA-256: `9C2C946E461905DB01E69484503E89DEFB6FAF4B76BD255D042A0914A595BDE9`.
- Bundled solver SHA-256: `9C2C946E461905DB01E69484503E89DEFB6FAF4B76BD255D042A0914A595BDE9`.
- GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.
- CTest: 8/8 passed.
- Retained cadence evidence: `C:\Users\alans\AppData\Local\Temp\cfd_interval20_smoke_20260729_2315`.
- A separate aerodynamic smoke at `0.5 s` exited but produced non-finite pressure and velocity by final step 6; friend-solver numerical correctness is not verified.

Remaining:
- Resized visual capture was inconclusive because the automated window became minimized; normal-size button rendering and clicking are verified.
- Active-run directory fallback is unit-tested but was not exercised through desktop automation.
- Friend-solver non-finite aerodynamic output requires separate numerical diagnosis.

## 2026-07-29 23:42 — Face-mask index syntax repair

Status:
- Written: corrected two malformed linear-index expressions in friend `Solver.cpp`.
- Built: external Release solver rebuilt successfully; the current solver was copied beside the GUI.
- Tested: all eight CTest targets passed.
- Verified: external and bundled solver SHA-256 values are identical.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Solver.cpp:63`: restored multiplication in both horizontal-face adjacent-cell indices.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Solver.cpp:72`: restored multiplication in both vertical-face adjacent-cell indices.

Cause:
- `jnx` was parsed as an undeclared identifier and `(j-1)nx` omitted the required multiplication operator.

Effect:
- `buildFaceMasks()` compiles while preserving the friend's new `!(solidA && solidB)` face-mask behavior.
- The current CUDA-path solver is bundled with the GUI.

Math:
- Horizontal adjacent-cell indices:
- ![](https://latex.codecogs.com/svg.image?k_R=jn_x&plus;i,\qquad%20k_L=jn_x&plus;i-1)
- Vertical adjacent-cell indices:
- ![](https://latex.codecogs.com/svg.image?k_T=jn_x&plus;i,\qquad%20k_B=(j-1)n_x&plus;i)

Validation:
- Solver build: succeeded.
- GUI configure: succeeded; GUI code required no recompilation.
- CTest: 8/8 passed.
- External and bundled solver SHA-256: `D1920CB588E3460A591AA3E499ADA1210AD064DE190D6AE7B8E99228E401FA05`.

Remaining:
- The semantic change from `!solidA && !solidB` to `!(solidA && solidB)` belongs to the friend and was preserved, not numerically validated.

## 2026-07-30 00:04 — Current CUDA diagnostic solver rebuild

Status:
- Built: current external Release solver rebuilt successfully.
- Tested: all eight CTest targets passed.
- Verified: external and bundled solver hashes are identical.

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\README.md`: corrected the documented current save interval from twenty to one.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI\build\cfd_app.exe`: refreshed from the current external solver.

Cause:
- Friend changed the CUDA routing, face-mask condition, multigrid cycle count, residual diagnostics, divergence diagnostics, and save interval.

Effect:
- The GUI bundle now launches the current friend solver.
- Current solver output requests one VTK frame per step plus the initial and final saves.

Math:
- Current save condition:
- ![](https://latex.codecogs.com/svg.image?n\bmod1=0)

Validation:
- External and bundled solver SHA-256: `32525D4C0CFD6FC7B6776ECA79D1B2A56C55C8716692F61529D87663CE5ED1C1`.
- CTest: 8/8 passed.

Remaining:
- Friend numerical changes compiled and passed interface tests but were not numerically validated.

## 2026-08-01 17:31 — Isolate optimized GUI from the preserved GUI

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized`: created as a source-only copy of the preserved GUI.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CMakeLists.txt`: renamed the project and executable target to `CFDMaskUIOptimized` and `cfd_mask_ui_optimized`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\main.cpp`: changed the usage name to `cfd_mask_ui_optimized`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: changed the window title and temporary run root to optimized-GUI-specific identities.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the separate project, executable, and temporary run directory.

Cause:
- The optimized GUI must coexist with the preserved GUI without replacing its source, executable, build cache, or run directories.

Effect:
- The preserved GUI remains at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI`.
- The optimized GUI builds as `cfd_mask_ui_optimized.exe` and stores new runs under `%TEMP%\CFDMaskUIOptimized\runs`.
- Old and new GUI processes cannot select the same default temporary run root.

Math:
- Not applicable; project identity and filesystem ownership do not alter geometry, CFD, or visualization mathematics.

Validation:
- Before this identity patch, 29 of 29 copied files matched the preserved GUI by SHA-256.
- Configure: Visual Studio 18 2026 generation succeeded in `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_build`.
- Build: Release `cfd_mask_ui_optimized` target succeeded.
- GUI SHA-256: `5742B1C3290C393B2D49CEC5F4160BA3E8786CC44DFC037D23B705BAF4E8373A`.
- External and bundled solver SHA-256: `332083E05BD548D229EF687AA2854528E32FFBDD3CFA55522CBC9B4A1F8E4E96`.
- CTest: 8 of 8 configured tests passed in 4.32 seconds.
- Launch: the preserved `CFD Mask UI` and new `CFD Mask UI Optimized` windows ran simultaneously and both reported responsive.

Remaining:
- Establish GUI responsiveness and solver performance baselines before optimization.
- Optimize only the new GUI and separately owned solver interfaces without changing the preserved GUI.

## 2026-08-01 18:22 — Event-driven idle rendering

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`, `Application::run`: replaced unconditional idle polling and rendering with timed SFML event waiting and dirty redraw ownership.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`, `Application::run`: retained 16 ms held-key updates and 100 ms solver/future polling, while fully idle operation waits indefinitely for an OS event.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`, `Application::syncWindowLayout`: skipped view and layout regeneration when the client size is unchanged.

Cause:
- The preserved GUI redrew the full SFML scene continuously while no state changed.
- A 60 FPS limiter alone still consumed 88.44% of one CPU core in the first 5-second measurement.

Effect:
- Unchanged idle frames are not generated.
- Input wakes the loop immediately; held movement remains limited to approximately 60 Hz.
- Solver and asynchronous VTK operations retain at most 100 ms polling latency.
- The preserved GUI source and executable remain unchanged.

Math:
- Not applicable; event scheduling and redraw ownership do not alter CFD, geometry, or visualization equations.

Validation:
- Configure: Visual Studio 18 2026 generation succeeded in `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_event_build_20260801`.
- Build: canonical and alternate Release `cfd_mask_ui_optimized` targets succeeded.
- CTest: 8 of 8 tests passed in the canonical build in 0.48 seconds.
- Preserved old executable SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.
- Measured optimized executable SHA-256: `98A60A98CD325D8A1CA3D455CCDABAB623D33DCE6B3234B9D038EB81DC719389`.
- Matched protocol: 2-second warmup, 20-second idle sample, resize wakeup, responsiveness check, normal close.
- Preserved old GUI: 16640.625 ms CPU in 20 seconds, or 83.2031% of one core.
- Optimized GUI conservative sample: 31.25 ms CPU in 20 seconds, or 0.1562% of one core.
- Idle CPU ratio: 532.5 times; speed improvement: 53150%; CPU reduction: 99.812%.
- Both measured processes accepted a resize, remained responsive, and closed normally.
- Preserved old `Application.cpp` SHA-256 remained `3A75CB47EAEE172CF9B974DA20FED714F5520949F7A2E2C9F290172C3A1A4ACB`.

Remaining:
- The 53150% result applies only to unchanged GUI idle work; interaction rendering and complete solver throughput require separate benchmarks.
- The current transient solver still produces non-finite fields in the bounded 256 by 256 circular-obstacle case and is not numerically verified.

## 2026-08-01 19:15 — Validated solver defaults and guarded optimized bundle

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\include\FriendRun.hpp`
- Changed the new GUI's relaxation default to `omega = 1.0`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\FriendRun.cpp`
- Changed the emitted multigrid cycle count from two to six and constrained `omega` to `(0, 1]`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`
- Changed the relaxation slider range/default to `(0.1, 1.0]` with default `1.0`.
- Replaced the obsolete `u_star` stride warning with the active projection-guard behavior.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\FriendRunTests.cpp`
- Updated exact serialization and invalid-range assertions for six cycles and `omega <= 1.0`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\ExactNinetyIntegrationTests.cpp`
- Updated the integration configuration to the validated relaxation value.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`
- Documented the constrained relaxation range and six-cycle projection setting.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_build\Release\cfd_app.exe`
- Bundled the current guarded CUDA solver without changing the preserved GUI or its executable.

Cause:
- Two cycles with `omega = 1.85` failed the projection criterion before the first timestep.
- Six cycles with `omega = 1.0` were the minimum tested configuration that passed.
- The optimized GUI still displayed a warning for the now-fixed `u_star` stride defect.

Effect:
- New optimized-GUI runs emit the tested safe configuration by default.
- Values above the validated relaxation bound are rejected before launching the solver.
- Numerically invalid timesteps stop with a visible solver diagnostic instead of appearing as result frames.
- The old GUI remains at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI` and is unchanged.

Math:
- GUI-enforced relaxation interval and cycle count:
- ![](https://latex.codecogs.com/svg.image?0<\omega\leq1,\qquad N_{\mathrm{cycle}}=6)
- Bundled projection guard:
- ![](https://latex.codecogs.com/svg.image?D_{\mathrm{after}}\leq\max\left(10^{-3}\,\mathrm{s}^{-1},\,0.01D_{\mathrm{before}}\right))

Validation:
- Canonical optimized GUI Release build: succeeded.
- CTest: 8/8 passed, including friend-input serialization and exact-90 integration.
- External and bundled solver SHA-256: `D7FDFF09CC563E7673E9BE66DC6A616FA26AD845E05184CC2261B7CC89A0A8D2`.
- Final optimized GUI SHA-256: `166C2A0FD652708617E5538F2B37C2F47A3253D01897A7E0DE9509118558ED92`.
- Bundled safe case: exit `0`, 28 finite VTK frames, step-1 projection ratio `0.00788371`.
- Bundled unsafe case: exit `3`, one initial frame, no failed timestep output.
- Startup/initialization trace: 671.875 ms process CPU during the first 30 seconds of the final run.
- Sustained idle trace: 0.000 ms process CPU during the following 20 seconds; process remained responsive and closed normally.
- Windows process accounting advanced in 15.625 ms quanta, so the sustained-idle upper bound is `0.078125%` of one core.
- Preserved GUI baseline: `83.2031%` of one core over 20 seconds.
- Conservative sustained-idle ratio: greater than `1065×`; improvement greater than `106400%`; CPU reduction greater than `99.906%`.
- Preserved old `Application.cpp` SHA-256: `3A75CB47EAEE172CF9B974DA20FED714F5520949F7A2E2C9F290172C3A1A4ACB`.
- Preserved old `FriendRun.cpp` SHA-256: `BE1DF85C5C0D43FAA74AAF909309C68FDB1A000E3B0AB533D19CE671AAA29647`.

Remaining:
- The greater-than-106400% result applies only to sustained unchanged GUI idle work after initialization, not startup, interaction rendering, VTK loading, or solver throughput.
- Long simulations still write every unique timestep because the friend solver retains `SAVE_INTERVAL = 1`.
- Numerical verification is bounded to the documented manufactured solutions and circular-obstacle run.

## 2026-08-08 17:49 — Pixel inspection and streaming frame navigation

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\include\VtkFrame.hpp` and `src\VtkFrame.cpp`: added screen-to-VTK pixel sampling, decoded-size accounting, a bounded least-recently-used decoded-frame cache, and lazy series indexing.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: added the hover tooltip with pixel X/Y, physical x/y, speed, and pressure; solid and non-finite values are explicit.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: changed startup from full-series decoding to filename indexing plus one active-frame decode.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: retained complete GUI-owned run validation by decoding and checking the required initial frame against the active frame.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: added a 1 GiB decoded-frame LRU cache and latest-request-wins presentation during asynchronous slider navigation.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: fixed initial layout and dynamic button state so a 1600 by 900 launch no longer requires a resize before controls are correct.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: removed the obsolete `u_star` stride warning; the current solver uses the `(nx+1)` U-field stride.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\VtkFrameTests.cpp`: added lazy-index, pixel-coordinate, finite-value, and LRU-eviction tests plus a real-series benchmark mode.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented hover semantics, lazy validation, cache ownership, and remaining VRAM/prefetch limits.

Cause:
- Hover inspection was absent, so a rendered pixel could not be tied to its numerical pressure, speed, or coordinates.
- The viewer parsed every frame before first display, making startup proportional to total dataset bytes.
- Frame requests were rejected while a parse was active, so slider navigation could present an obsolete request.
- Dynamic control state was refreshed only during resize, and the initial 1600 by 900 layout pass was skipped.

Effect:
- Mouse movement over a rendered sample performs no file I/O and reports the exact decoded frame value.
- Startup decodes one frame; other frames are decoded on demand and retained only within the RAM budget.
- Completed GUI-owned runs still prove that both the initial and active frames are parseable and layout-compatible before presentation.
- New slider requests replace the pending presentation target; completed obsolete reads may populate the cache but cannot replace the newest requested display.
- Initial and asynchronous state changes update control layout/selection without requiring manual resize.
- The preserved GUI at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI` remains unchanged.

Math:
- Screen to VTK index: `i = floor((x_s - o_x) / w_c)`, `j = n_y - 1 - floor((y_s - o_y) / h_c)`.
- Cell-data position: `x = x_0 + (i + 1/2) dx`, `y = y_0 + (j + 1/2) dy`; point-data position omits the half-cell offset.
- Speed: `|u| = sqrt(u_x^2 + u_y^2 + u_z^2)`, as decoded by the existing parser.
- Cache invariant: `sum(decoded frame bytes) <= 1 GiB`; oversized frames remain displayable but are not cached.

Validation:
- Current solver Release build: succeeded with CUDA architecture 75, OpenMP, and AVX2 enabled.
- Optimized GUI Release build: succeeded and bundled the current solver executable.
- Final CTest: 8 of 8 tests passed in 0.30 seconds.
- Real series: 381 files, 499,484,523 bytes, 256 by 256 cells.
- Previous full-catalog timings: 7,305 ms cold; 4,210 ms and 4,110 ms warm.
- Lazy-index timings: 17, 18, 17, 18, and 17 ms.
- Warm-median catalog latency changed from 4,210 ms to 17 ms: 247.6 times faster and 99.596% lower latency.
- Live finite tooltip matched step 0: pixel `(47,181)`, physical position `(0.19 m, 0.71 m)`, speed `1.00 m/s`, pressure `0.00 Pa`.
- Live rapid slider sequence ended on its newest requested step 300; the process remained responsive.
- Final executable launched, remained responsive, displayed the finite tooltip, and closed normally.
- Final optimized GUI SHA-256: `A37A60DD67B0237B6B4A56B60F235DB0B6BF1E44CB7BF9B2C6561AFB2FC7D3D8`.
- Bundled and external current solver SHA-256: `0DF8C17DD07771718E8AF26FA0778F6FB0BA7563B5B52289AD1D8E5BA6E71E3E`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.
- Visual evidence: `validation\hover-tooltip-20260808.png` and `validation\latest-request-wins-20260808.png`.

Remaining:
- Lazy indexing reports malformed or incompatible non-active frames only when selected.
- Series scalar ranges expand as frames are decoded rather than being known globally at startup.
- Direction-aware prefetch, asynchronous GPU upload, scalar GPU shaders, and a bounded multi-texture VRAM cache are not implemented.
- CUDA 13.3 emitted upstream CCCL deprecation warnings while building the solver; compilation succeeded.

## 2026-08-08 18:05 — Git-ready source/build separation and runtime solver selection

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\.gitignore`: excluded CMake caches, IDE projects, binaries, compiler products, user presets, and runtime solver-selection state from Git.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\CMakePresets.json`: added a Windows Visual Studio 2026 configure preset that always writes generated files to the sibling `CFD-Mask-UI-Optimized_build` directory, plus matching Release build/test presets.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: separated source-controlled build definitions from generated build products and documented the preset commands.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: added the `Select solver EXE` button and Windows executable picker.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\include\FriendRun.hpp` and `src\FriendRun.cpp`: added external solver validation and UTF-8 persistence in `solver-selection.txt` beside the GUI executable.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\FriendRunTests.cpp`: added valid selection, invalid extension, persistence, restoration, and missing-file tests.

Cause:
- Generated build products must not share ownership with source intended for Git.
- The compiled GUI previously selected only its bundled/configured solver, so pointing it at a newly built friend executable required reconfiguration or rebuilding.

Effect:
- The source directory can be initialized or copied into Git without generated CMake or Visual Studio files.
- `cmake --preset windows-vs2026` writes only to `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_build`.
- A user-selected external solver path overrides the bundled fallback on later GUI launches.
- Rebuilding or replacing the executable at the selected path changes the next solver run without rebuilding the GUI.
- The selection button is disabled while a solver process is active, and the selected path is validated before every launch.
- The preserved GUI at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI` remains unchanged.

Math:
- Not applicable; repository layout and executable ownership do not alter CFD or visualization equations.

Validation:
- `cmake --list-presets`, `cmake --list-presets=build`, and `cmake --list-presets=test`: all three preset types parsed successfully.
- `cmake --preset windows-vs2026`: configured successfully into the sibling build directory.
- `cmake --build --preset release`: Release GUI and all test targets built successfully.
- `ctest --preset release`: 8 of 8 tests passed in 3.75 seconds.
- Live GUI: `Select solver EXE` was visible and enabled.
- Live Windows dialog: title `Select friend solver executable` opened, was cancelled, the GUI remained responsive, and the process closed normally.
- Visual evidence: `validation\solver-selector-20260808.png`.
- Source audit: no CMake cache, IDE project, compiler output, library, or executable exists inside the source tree.
- Final optimized GUI SHA-256: `D0E16B20BCDF2BE526F01C8293AAA1C4E7279B2A6A7FD04F78EDB2A5B3A82A8F`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.

Remaining:
- The GUI validates file existence and `.exe` type, but a newly selected solver must retain the existing 17-value input and VTK output protocol.
- Deleting the build directory also deletes its `solver-selection.txt`; the GUI then falls back to the adjacent/configured solver.
- The source directory is Git-ready but was not initialized as a Git repository because repository creation was not requested.
- The provided preset is Windows/Visual Studio 2026-specific; other toolchains should use an additional preset or the documented manual command.

## 2026-08-08 18:41 — New friend solver command-interface adapter

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\include\FriendRun.hpp`: replaced the obsolete console-input writer interface with named argument construction and argument-record interfaces; changed the coarse SOR default to `1.85`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\FriendRun.cpp`: mapped GUI-owned values to the new solver's `key=value` interface, preserved full numeric precision, aligned the minimum grid with the solver's `8`-cell check, accepted SOR relaxation only in `(0, 2)`, and removed obsolete Reynolds/multigrid-cycle console slots.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: changed `ChildProcess::start` from redirected interactive stdin to direct `CreateProcessW` arguments with Windows quoting and `NUL` stdin; changed run records to `requested-arguments.txt` and `solver-arguments.txt`; changed the visible control to `Coarse SOR omega`, range `1.0` to `1.95`, default `1.85`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\FriendRunTests.cpp`: replaced positional-console tests with exact named-argument, path-with-spaces, record, range, and positive-time mapping tests.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\ExactNinetyIntegrationTests.cpp`: launched the current friend through named arguments and retained adapter/VTK validation for zero-step and opt-in positive-time runs.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the new runtime contract, executable-owned defaults, renamed run records, grid/SOR constraints, and interactive-only compatibility boundary.

Cause:
- The selected new friend executable is valid and supports non-interactive `key=value` arguments, but the GUI still wrote the old 17-value stdin sequence.
- The new interactive prompt inserted density and adaptive/multigrid controls, so old positional values shifted into unrelated fields before launch could complete correctly.
- The GUI's old `nx, ny >= 2` and `omega <= 1` validation no longer matched the current executable's `nx, ny >= 8` and coarse SOR control.

Effect:
- The selected executable receives values by name, so prompt insertion/reordering no longer corrupts a run.
- Paths containing spaces are passed as one argument without invoking a shell.
- `outputDir=.` keeps VTK ownership inside the unique GUI run directory.
- Newly added friend-only controls use that executable's compiled defaults; selecting or rebuilding a compatible friend executable does not require rebuilding the GUI.
- The preserved GUI at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI` remains unchanged.

Math:
- Not applicable to governing or discrete equations; this change transports existing configuration values without changing the solver algorithms.
- The transported coarse SOR relaxation constraint is `0 < omega < 2`; the GUI default is `1.85`.

Validation:
- Exact selected executable `--help`: exited `0` and advertised `key=value` non-interactive execution.
- Initial adapter build exposed a missing `<sstream>` include; the include was added and the final build succeeded.
- `cmake --preset windows-vs2026 -DCFD_SOLVER_EXE=<selected Debug cfd_app.exe>`: configured successfully into the sibling build directory.
- `cmake --build --preset release`: GUI and all test targets built successfully.
- Final `ctest --preset release`: 8 of 8 tests passed in 0.70 seconds.
- Positive-time exact-90 interface smoke: `50 by 50`, `totalTime=0.001 s`, passed and produced at least two compatible frames; numerical correctness was not established by this smoke.
- Final live GUI Run click: created `%TEMP%\CFDMaskUIOptimized\runs\run-435877122142700`, wrote the named argument records, imported 398 section points, selected CUDA, produced `solution_0.vtk` with SHA-256 `29404F46BFFA55803BED9D85611C9DE9B9F6A504AA0A887B5BF752D0D27FF088`, emitted no stderr, completed, and the GUI closed normally.
- Final optimized GUI SHA-256: `CB14FFD8C8639A3D39F4986F788312FE8C9ACFB307352681FD7A8612F8755061`.
- Selected and bundled friend solver SHA-256: `4BDF9E11A3AB15EC2A0760722C6C19B9565C1B542730856E73A81C696B252B7E`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.

Remaining:
- Runtime-selected solvers must retain the named `key=value` interface and compatible VTK contract; older interactive-only builds are not supported by this optimized adapter.
- Timestep cadence/safety, smoother omega, multigrid cycles/tolerance/coarse size, save interval, and CPU/CUDA selection are executable-owned defaults rather than GUI controls.
- The positive-time smoke verified the interface and finite parseable output, not CFD numerical correctness or CPU/CUDA parity.

## 2026-08-08 19:07 — Dark green interface palette

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: changed application, panel, viewport, text, muted text, accent, controls, buttons, borders, overlays, solid-cell neutral color, and section-plane colors to a centralized dark green palette; changed the section-plane legend from `BLUE` to `GREEN`.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the theme boundary and preserved semantic result colors.

Cause:
- The optimized GUI was requested to use a dark greenish appearance.

Effect:
- Setup and Results chrome now use dark forest-green surfaces with mint-green active controls and borders.
- Pressure/velocity gradients, invalid-value magenta, mesh-intersection orange, simulation-running amber, and warning colors retain their existing meanings and contrast.
- The old GUI at `C:\Users\alans\3D Objects\.git\CFD-Mask-UI` remains unchanged.

Math:
- Not applicable; this is a presentation-only change and does not alter solver inputs, CFD equations, discretization, VTK decoding, or numerical values.

Validation:
- `cmake --build --preset release`: succeeded.
- `ctest --preset release`: 8 of 8 tests passed in 0.74 seconds.
- Final executable launched with `CAR.obj`; the 1600 by 900 setup view rendered the dark green palette, readable text/controls, green section plane, and preserved orange intersection, then closed normally.
- Visual inspection source: `%TEMP%\cfd-mask-ui-dark-green-final.png`.
- Final optimized GUI SHA-256: `8ED0F6A9778B5D48DBA71ACEA4C30BB576A1842CC0D01412259996A4B0F891DB`.
- Visual inspection PNG SHA-256: `82C2C2A8A09188771AE1C588FCDD439C89AC6C32FB37969F8CCCF1D388EE32A1`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.

Remaining:
- None.

## 2026-08-08 19:15 — Reduce blue from green theme

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: reduced the blue channel across the dark-green palette, shifted mint accents toward leaf green, and changed the model preview from blue-gray to neutral green-gray.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the warmer reduced-blue theme.

Cause:
- The first dark-green palette still appeared too blue/cyan.

Effect:
- Panels, viewport, controls, borders, section plane, and mesh preview now read as warmer forest/leaf green.
- Scientific pressure/velocity gradients remain unchanged so their quantitative color scale retains its meaning.
- The old GUI remains unchanged.

Math:
- Not applicable; only presentation colors changed.

Validation:
- `cmake --build --preset release`: succeeded.
- `ctest --preset release`: 8 of 8 tests passed in 0.75 seconds.
- Final executable launched with `CAR.obj`, rendered the warmer palette with readable contrast, and closed normally.
- Visual inspection source: `%TEMP%\cfd-mask-ui-less-blue-final.png`.
- Final optimized GUI SHA-256: `4B4A705C7F9B8609CD08B66E081F53DC18328F5C09D155094A6F278D65C83966`.
- Visual inspection PNG SHA-256: `FDBCB713365ADF392D0C58F5CA582B7DA427D6FA3EF93B1564ACE8DA7507CECF`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.

Remaining:
- None.

## 2026-08-08 19:18 — Near-black theme with minimal green accents

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: changed green-tinted surfaces to near-black/charcoal neutrals, limited bright green to active controls and the section-plane outline, reduced section-plane fill opacity, and neutralized the mesh preview.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the minimal-green theme boundary.

Cause:
- The warmer forest-green version still used too much green across large surfaces; the requested direction was darker and closer to black gaming-hardware styling with only small green highlights.

Effect:
- Most screen area is black or neutral charcoal.
- Green now identifies active tabs, slider state, focused controls, and the section plane instead of tinting inactive surfaces.
- Scientific result colors, warnings, invalid values, and the orange mesh intersection remain unchanged.
- The old GUI remains unchanged.

Math:
- Not applicable; presentation colors only.

Validation:
- `cmake --build --preset release`: succeeded.
- `ctest --preset release`: 8 of 8 tests passed in 1.42 seconds.
- Final executable launched with `CAR.obj`; inactive controls and the mesh rendered neutral, green remained limited to active accents, text remained readable, and the GUI closed normally.
- Visual inspection source: `%TEMP%\cfd-mask-ui-razer-dark-final.png`.
- Final optimized GUI SHA-256: `17572DEE0EC525A58572F6528C6DA5167807BEB0E77C7D5EC14273F71304FD67`.
- Visual inspection PNG SHA-256: `9CF5CBF2006CBFB9198AE6E30825ACF3C81A7B2DCDA0E0CD57BBFFAD9152B41F`.
- Preserved old GUI SHA-256: `736E1CBC552235B95CDB7EDAFBB3531EF5B2DBDC7911CE43244A76008756BC3D`.

Remaining:
- None.

## 2026-08-09 11:38 — Adaptive nearest-frame VTK loading indicators

Status:
- Written
- Built
- Launched
- Tested

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\include\VtkFrame.hpp`: added the adaptive-frame-window result/interface and explicit cache membership/window-pruning operations.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\VtkFrame.cpp`: added power-of-two resident-fraction planning around the selected frame and bounded cache pruning with byte-accounting preservation.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\src\Application.cpp`: added a maximum-16-frame, maximum-1-GiB adaptive window; nearest-first single-worker prefetch; latest-request demand priority; visible cache-fraction status; and a lower-right animated indicator for simulation, VTK indexing, requested-frame decoding, and VTK prefetch.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\tests\VtkFrameTests.cpp`: added cache-pruning and adaptive-window tests for empty, single, small, 381-frame centre, and 381-frame boundary cases.
- `C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized\README.md`: documented the resident-limit formula, power-of-two divisor, demand/prefetch order, memory boundary, and loading indicator states.

Cause:
- Retaining or eagerly decoding every frame scales RAM and startup work with the complete VTK series.
- On-demand-only decoding makes rapid nearby scrolling repeatedly wait for disk and parsing.
- Long simulation and VTK operations previously lacked one consistent animated busy signal.

Effect:
- Only the closest power-of-two fraction is planned for decoded residency; the active frame and nearest neighbours are favored while the 1 GiB byte cap remains authoritative.
- Slider movement immediately records the newest requested index. A currently parsing frame is allowed to finish safely, then the requested frame runs before speculative prefetch resumes.
- Cache entries outside the new nearest window are released as the selection moves.
- Simulation and all asynchronous VTK states now expose their actual operation in the corner without creating an idle animation loop.
- The old GUI and friend solver remain unchanged.

Math:
- `M = max(1, min(16, floor(B / S)))`, where `M` is the resident-frame limit, `B = 1 GiB`, and `S` is the active frame's decoded byte size.
- For `N > 1`, choose the smallest `D` in `{2, 4, 8, 16, ...}` satisfying `ceil(N / D) <= M`; plan `K = ceil(N / D)` nearest indices.
- Example: `N = 381`, `M = 16` gives `D = 32`, `K = 12`.
- CFD governing equations, discretization, solver inputs, and VTK values are unchanged.

Validation:
- `cmake --build "C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_build" --config Release --parallel`: succeeded.
- `ctest --test-dir "C:\Users\alans\3D Objects\.git\CFD-Mask-UI-Optimized_build" -C Release --output-on-failure`: 8 of 8 tests passed in 0.59 seconds.
- Focused planner/cache tests verified 10 frames plan one half (5 frames), 381 frames plan one thirty-second (12 frames), edge clamping, and exact cache byte ownership after pruning.
- Live GUI launched against the existing single-frame `solution_0.vtk`, remained responsive, rendered the Results view, and closed normally.
- Final optimized GUI SHA-256: `9696A294B46081E9F0E4F3BB1324D6A355E454ADA5E3A9107C1AEDBCDCBEF134`.
- Preserved old GUI source SHA-256: `Application.cpp` `3A75CB47EAEE172CF9B974DA20FED714F5520949F7A2E2C9F290172C3A1A4ACB`; `FriendRun.cpp` `BE1DF85C5C0D43FAA74AAF909309C68FDB1A000E3B0AB533D19CE671AAA29647`.

Remaining:
- No authentic large VTK series was available locally, so end-to-end scrolling speed, peak RAM, and the short-lived VTK loading animation were not measured on hundreds of real frames.
- The simulation-start animation is compiled and state-driven but was not exercised by starting a new solver run in this change.
- A user request arriving during prefetch waits for only the single parse already in progress; the parser is not forcibly cancelled.
