# Changelog

## 2026-08-05 — MAJOR! Revert to the last working solver, re-implement all 15 optimizations

Status:
- Written
- Built (CPU path; CUDA path syntax-checked and verified through a host emulator, not nvcc)
- Launched
- Tested
- Verified

Decision:
- The merged optimization branch was not debuggable. Its bugs were structural, not local.
- Went back to the old `double` solver, took its mathematics as ground truth, and rewrote
  `Solver` / `Multigrid` / `MultigridCuda` from scratch with all 15 optimizations
  re-implemented on top of correct math. Kept the ideas from the optimized branch, kept
  none of its code.

Changed:
- `src/Solver.cpp`, `include/Solver.hpp` — rewritten. Fixed the `u_star` row stride in
  `solvePoisson` (`j*nx` → `j*(nx+1)`), stopped treating the outer ring of real fluid cells
  as ghost cells, moved the boundary conditions into the Poisson operator coefficients,
  corrected the outlet through a half-cell Dirichlet ghost, moved free slip into the
  predictor's diffusion stencil, branch-free AVX2 predictor/corrector, local CFL,
  direct-to-buffer binary VTK.
- `src/Multigrid.cpp`, `include/Multigrid.hpp` — rewritten. Restriction is now the exact
  transpose of the prolongation, even-only coarsening (no truncated coarse blocks),
  semi-coarsening for anisotropic grids, precomputed per-level stencil with `invDiag`,
  red/black SOR with AVX2 masked stores, halo-padded arrays, FMG on the first solve then
  warm start, relative convergence tolerance.
- `src/MultigridCuda.cu`, `include/MultigridCuda.cuh` — rewritten. Device memory allocated
  once instead of per time step, pressure resident on the GPU between steps, stencil
  uploaded once, Thrust dropped, error checking added, `applyBCKernel` and
  `zeroSolidPressureKernel` deleted.
- `include/Config.hpp`, `src/Config.cpp` — added `saveInterval`, `outputDir`, `mgTolerance`,
  `smootherOmega`, `dtUpdateInterval`, `dtSafety`, `mgMinCoarseSize`, `useCuda`, and
  `setParam()` for non-interactive configuration.
- `src/main.cpp` — non-interactive `key=value` command line mode, wall-clock timing.
- `src/Mesh.cpp` — constructor initialiser list reordered to match declaration order.
- `CMakeLists.txt` — a missing CUDA toolkit warns and falls back to CPU instead of failing
  configuration outright.

Cause:
- `u_star` was indexed with the pressure row stride, shearing the velocity field by one
  element per row inside the divergence computation.
- The smoother swept only `i=1..nx-2, j=1..ny-2` and `applyBC()` overwrote the rest, but
  those cells are real fluid cells, so a one-cell band along every wall and around the body
  never received a pressure correction. `Solver::applyBC` then ran *after* the corrector and
  overwrote two rows of `u`, destroying the projection it had just computed.
- Multigrid restriction (2×2 average) was not the transpose of the prolongation (bilinear),
  so the coarse-grid correction was not an energy-norm projection. On some grid sizes the
  two-grid operator had spectral radius > 1 and each V-cycle amplified the error — 128×128
  converged, 100×100 diverged.
- `cudaMalloc`/`cudaFree` ran for every level on every pressure solve.

Effect:
- `div(u) = div(u*) − dt·L·p` now holds exactly, face by face, so the projection projects.
- Mass balance measured at 0.00000 % on a cylinder-in-channel case.
- Grids that previously diverged (100×100, 64×128) or stalled (512×17) converge to float
  precision.
- Multigrid convergence factor 0.06–0.13 per V-cycle, verified against an independent
  double-precision reference implementation of the operator.
- CPU and CUDA paths produce bit-identical results under emulation.
- ~41× faster per step than the old solver (1.64 ms vs 67.6 ms at 200×100, single thread),
  before counting the removal of the per-step VTK write.



## 2026-07-18 18:08 — Implement STL/OBJ section-to-mask pipeline

Status:
- Written
- Built
- Launched
- Tested
- Verified

Changed:
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\include\Config.hpp`
- Removed the duplicate `Config::ro` declaration that blocked compilation.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Config.cpp`
- Added Windows-safe geometry-path input, explicit mirror input, and consistent confirmation-loop newline handling.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\include\Mesh.hpp`
- Added `Vertex`, `Triangle`, `GeometryType`, triangle storage, loaders, section construction, rasterization, solid filling, and private contour helpers.
- Kept `initCircle` as the verification and load-failure fallback.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\Mesh.cpp`
- Added OBJ and STL parsing.
- Added central oriented-plane intersection and closed-contour reconstruction.
- Added mirroring, in-plane rotation, normalization, boundary rasterization, and even–odd interior filling.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\src\main.cpp`
- Replaced placeholder geometry messages with implemented behavior.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\README.md`
- Updated status, usage, roadmap, and geometry mathematics.
- `C:\Users\alans\3D Objects\.git\CFD-Solver-2D\Obsidian\Decisions.md`
- Documented the section plane, edge intersection, normalization, rasterization, and filling equations.

Cause:
- The solver required STL/OBJ geometry import and a deterministic conversion from a 3D triangle mesh to the existing 2D `solid` mask.
- `Config::ro` was declared twice, so the existing project did not compile.

Effect:
- `.stl` and `.obj` files can generate immersed-boundary masks.
- `sliceAngleX`, `sliceAngleZ`, `sliceRotation`, and `invertSection` now affect geometry.
- `none`, missing files, unsupported files, empty sections, and invalid contours fall back to the verification circle.
- Geometry is centred and normalized to the previous circle diameter, \(0.2\min(L_x,L_y)\).

Validation:
- MSVC command: direct C++17 compilation of all first-party translation units with `/W4`.
- Result: build succeeded; remaining warnings are vendored-library warnings and two pre-existing unused Solver locals.
- AddressSanitizer command: direct MSVC `/fsanitize=address` build and transformed OBJ execution.
- AddressSanitizer result: exit code 0 with no reported memory error.
- Static-analysis command: MSVC `/analyze` on `Config.cpp` and `Mesh.cpp`.
- Static-analysis result: no first-party diagnostic; one warning originates in vendored `stl_reader.h`.
- OBJ test: watertight cube, 12 triangles, 8 contour nodes, 140 solid cells.
- STL test: binary sphere, 20 triangles, 6 contour nodes, 112 solid cells.
- Transform test: \(20^\circ\) X angle, \(35^\circ\) Z angle, \(30^\circ\) in-plane rotation, mirrored; 78 solid cells.
- Fallback test: `geometryFile = none`; 80 circle cells.
- Path test: raw Windows path containing `3D Objects` loaded successfully.
- Modification test: changed `geometryFile` through the confirmation loop; STL loaded successfully.
- VTK mask checks: 2500 values for a \(50\times50\) grid, only values 0 and 1.
- OBJ extent check: \(x,y\in[0.39,0.61]\), centroid \((0.5,0.5)\).

Remaining:
- Only the largest closed contour is retained.
- Holes and multiple disconnected bodies are not represented.
- Fully coplanar triangles and non-manifold/open intersections fall back to the circle if no closed contour is reconstructed.
- Existing Solver indexing, pressure-gauge, and time-step defects remain outside this change.
