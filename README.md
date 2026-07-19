(lets just forget that we pushed 12 commits just for things to build normally and files to look at least alright 'cause we're stooooopid as shi)
(from Kuzya: i dunno how it works, i have a feeling it's something alive, and changes by itself <3. TOTALLY NOT ME COMMITING 12 TIMES)
# CFD-Solver-2D

**A 2D incompressible Navier‑Stokes solver for external flows around arbitrary profiles.**

CFD‑Solver‑2D is an educational/research project that implements a finite‑difference CFD solver for unsteady viscous incompressible flow. It uses the **Chorin projection method** on a **staggered MAC grid** with an **immersed boundary** technique to handle complex geometries. The code is written in C++17 and features:
- Interactive console parameter input with confirmation and on‑the‑fly editing.
- **Currently**: full numerical solver with VTK output for post‑processing in ParaView.
- **Currently**: STL/OBJ loading, central plane section extraction, and geometry masking.
- **Planned**: real‑time visualisation with SFML.

The ultimate goal is to simulate the **Karman vortex street** behind a cylinder or an airfoil at moderate Reynolds numbers.

---

## Current Status (after Sprint 2)

- ✅ **Interactive configuration** – all parameters are entered via console, can be reviewed and modified before starting.
- ✅ **Structured grid** – uniform Cartesian grid with imported-profile or circle mask.
- ✅ **Full Navier–Stokes solver** – Chorin projection method:
  - Predictor step with upwind convection and central diffusion.
  - SOR iterative solver for the pressure Poisson equation.
  - Corrector step updating velocities.
  - Dynamic time step based on CFL and diffusive stability.
- ✅ **VTK output** – saves pressure (physical, in Pa) and velocity fields every N steps (configurable).
- ✅ **Profile import** – STL/OBJ loading, oriented central slicing, rotation, mirroring, and mask generation.
- ❌ **Built‑in visualization** – SFML rendering will be added in Sprint 4.

---

## Mathematical Model (brief)

We solve the 2D incompressible Navier–Stokes equations (kinematic pressure, ρ = 1):

**Momentum (X):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20t}%20+%20u\frac{\partial%20u}{\partial%20x}%20+%20v\frac{\partial%20u}{\partial%20y}%20=%20-\frac{\partial%20p}{\partial%20x}%20+%20\nu%20\nabla^2%20u)

**Momentum (Y):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20v}{\partial%20t}%20+%20u\frac{\partial%20v}{\partial%20x}%20+%20v\frac{\partial%20v}{\partial%20y}%20=%20-\frac{\partial%20p}{\partial%20y}%20+%20\nu%20\nabla^2%20v)

**Continuity (incompressibility):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20x}%20+%20\frac{\partial%20v}{\partial%20y}%20=%200)

The **Chorin projection** splits each time step into:
1. **Predictor** – compute intermediate velocities \(u^*, v^*\) without pressure.
2. **Poisson equation** – solve ![](https://latex.codecogs.com/svg.image?\nabla^2%20p%20=%20\frac{1}{\Delta%20t}%20\left(%20\frac{\partial%20u^*}{\partial%20x}%20+%20\frac{\partial%20v^*}{\partial%20y}%20\right)) using SOR.
3. **Corrector** – update velocities with the pressure gradient.

Boundary conditions: no‑slip on solid walls, constant velocity at inlet, zero‑gradient at outlet, free‑slip at top/bottom.

### Geometry section

The imported triangle mesh is centred at its bounding-box centre \(\mathbf{c}\).
The section plane is

\[
(\mathbf{x}-\mathbf{c})\cdot\mathbf{n}=0,
\]

where \(\mathbf{n}\) is constructed from `sliceAngleX` and `sliceAngleZ`.
Each triangle edge with signed endpoint distances \(d_a\) and \(d_b\) crosses
the plane at

\[
\mathbf{x}_{section}
=
\mathbf{a}
+
\frac{d_a}{d_a-d_b}(\mathbf{b}-\mathbf{a}).
\]

The resulting segments are connected into closed contours. The largest closed
contour is optionally mirrored, rotated by `sliceRotation`, scaled to
\(0.2\min(L_x,L_y)\), centred in the domain, rasterized, and filled with the
even–odd point-in-polygon rule.

---

## Architecture

```text
CFD-Solver-2D/
├── .vscode/
├── output/ (VTK files are written here)
├── src/
│ ├── main.cpp
│ ├── Config.cpp
│ ├── Mesh.cpp
│ ├── Solver.cpp
│ └── tiny_obj_loader_impl.cpp (to be used in Sprint 3)
├── include/
│ ├── Config.hpp
│ ├── Mesh.hpp
│ ├── Solver.hpp
│ └── tiny_obj_loader.h
├── models/ (place STL/OBJ files here)
├── lib/
│ ├── sfml/ (will be used in Sprint 4)
│ └── stl_reader/ (STL import)
├── build/ (build directory, ignored by Git)
├── CMakeLists.txt
├── .gitignore
├── LICENSE
└── README.md
```

---

## Requirements

- **C++17** compatible compiler (MSVC 2019/2022, GCC 9+, Clang 10+)
- **CMake** 3.10 or higher
- (Optional) **ParaView** or similar to visualize the VTK output.
---

## Build — PowerShell

```powershell
cd "...\CFD-Solver-2D-full-fixed"

if (Test-Path build) { Remove-Item build -Recurse -Force }
if (Test-Path install) { Remove-Item install -Recurse -Force }

cmake -S . -B build -G "Visual Studio 18 2026" -A x64

cmake --build build --config Release

cmake --install build --config Release --prefix install
```

## Run

```powershell
.\install\bin\cfd_app.exe
```
Follow the interactive prompts to set:
- Domain size (Lx, Ly)
- Grid resolution (nx, ny)
- Flow parameters (U0, nu, Re – if set to 0 it will be computed later)
- Time parameters (CFL, totalTime)
- SOR parameters (omega, tol, maxIterSOR)
- Geometry file (`.stl`, `.obj`, or `none` for the verification circle)
- Section orientation (`sliceAngleX`, `sliceAngleZ`)
- In-plane rotation and optional mirroring
After confirmation, the solver starts and writes solution_*.vtk files in the working directory.

Visualization of Results
- Open the generated .vtk files in ParaView.
- Build contours, streamlines, and vector fields.
You can animate the sequence to observe vortex shedding.

## Future Work (Roadmap)

| Sprint | Focus |
|--------|-------|
| **3**  | Geometry hardening: multiple contours, holes, and malformed/non-manifold mesh diagnostics |
| **4**  | SFML‑based real‑time rendering, interactive controls (pause, mode switching, zoom, time scrubbing) |
| **Bonus** | Optional extension to compressible flows (gas dynamics) after core features are stable. Also the code will be properly optimized, fixed and checked.|

## Contributing / Feedback

This is a personal educational project, but suggestions and issues are welcome. Feel free to open an issue or pull request.

---

**Happy simulating!**  
If you have any questions, don't hesitate to open an issue.
