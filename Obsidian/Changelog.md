# Changelog

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
