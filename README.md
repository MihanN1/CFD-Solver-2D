(lets just forget that we pushed 12 commits just for things to build normally and files to look at least alright 'cause we're stooooopid as shi)
(from Kuzya: i dunno how it works, i have a feeling it's something alive, and changes by itself <3. TOTALLY NOT ME COMMITING 12 TIMES)
# CFD-Solver-2D

**A 2D incompressible Navier‑Stokes solver for external flows around arbitrary profiles.**

CFD‑Solver‑2D is an educational/research project that implements a finite‑difference CFD solver for unsteady viscous incompressible flow. It uses the **Chorin projection method** on a **staggered MAC grid** with an **immersed boundary** technique to handle complex geometries. The code is written in C++17 and features:
- Interactive console parameter input with confirmation and on-the-fly editing.
- Full numerical solver with VTK output for post-processing in ParaView.
- STL/OBJ loading, central plane section extraction, geometry masking, profile rotation, mirroring, and robust contour reconstruction.
- Real-time visualization with SFML.

The project is designed to simulate external incompressible flow around arbitrary 2D profiles such as cylinders, airfoils, valves, turbine blades, and similar engineering geometries.

---

# Features

## Numerical solver

- ✅ Incompressible Navier–Stokes equations
- ✅ Chorin projection method
- ✅ Staggered (MAC) grid
- ✅ First-order upwind convection
- ✅ Central-difference diffusion
- ✅ Dynamic CFL-based timestep
- ✅ Adaptive pressure correction
- ✅ Optimized Poisson solver
- ✅ Correct residual evaluation
- ✅ Immersed boundary method

---

## Geometry

- ✅ STL import
- ✅ OBJ import
- ✅ Arbitrary slicing plane
- ✅ Automatic contour reconstruction
- ✅ Hole detection
- ✅ Multiple contour support
- ✅ Non-manifold diagnostics
- ✅ Automatic scaling and centering
- ✅ Rotation and mirroring
- ✅ Polygon rasterization using even-odd filling

---

## Visualization

- ✅ Real-time SFML visualization
- ✅ Pressure rendering
- ✅ Velocity rendering
- ✅ Solid mask rendering
- ✅ Pause / Resume
- ✅ Time scrubbing
- ✅ Zoom and camera movement
- ✅ Rendering mode switching

---

## Output

- ✅ VTK export
- ✅ Physical pressure (Pa)
- ✅ Velocity vectors
- ✅ Solid mask
- ✅ ParaView compatible

---

## Mathematical Model (brief)

We solve the 2D incompressible Navier–Stokes equations (kinematic pressure, ρ = 1):

**Momentum (X):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20t}+u\frac{\partial%20u}{\partial%20x}+v\frac{\partial%20u}{\partial%20y}=-\frac{\partial%20p}{\partial%20x}+\nu\nabla^{2}u)

**Momentum (Y):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20v}{\partial%20t}+u\frac{\partial%20v}{\partial%20x}+v\frac{\partial%20v}{\partial%20y}=-\frac{\partial%20p}{\partial%20y}+\nu\nabla^{2}v)

**Continuity (incompressibility):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20x}+\frac{\partial%20v}{\partial%20y}=0)

The **Chorin projection** splits each time step into:

1. **Predictor** – compute intermediate velocities \(u^*, v^*\) without pressure.
2. **Poisson equation** – solve

![](https://latex.codecogs.com/svg.image?\nabla^{2}p=\frac{1}{\Delta%20t}\left(\frac{\partial%20u^{*}}{\partial%20x}+\frac{\partial%20v^{*}}{\partial%20y}\right))

using SOR.

3. **Corrector** – update velocities with the pressure gradient.

Boundary conditions: no-slip on solid walls, constant velocity at inlet, zero-gradient at outlet, free-slip at top/bottom.

### Geometry section

The imported triangle mesh is centred at its bounding-box centre

![](https://latex.codecogs.com/svg.image?\mathbf{c})

The section plane is

![](https://latex.codecogs.com/svg.image?(\mathbf{x}-\mathbf{c})\cdot\mathbf{n}=0)

where

![](https://latex.codecogs.com/svg.image?\mathbf{n})

is constructed from `sliceAngleX` and `sliceAngleZ`.

Each triangle edge with signed endpoint distances

![](https://latex.codecogs.com/svg.image?d_{a})

and

![](https://latex.codecogs.com/svg.image?d_{b})

crosses the plane at

![](https://latex.codecogs.com/svg.image?\mathbf{x}_{section}=\mathbf{a}+\frac{d_{a}}{d_{a}-d_{b}}(\mathbf{b}-\mathbf{a}))

The resulting segments are connected into closed contours. Multiple contours and holes are detected automatically. Geometry consistency is validated before rasterization. The resulting contours can optionally be mirrored, rotated by `sliceRotation`, scaled to

![](https://latex.codecogs.com/svg.image?0.2\,\min(L_x,L_y))

centred in the computational domain, rasterized, and filled using the even–odd point-in-polygon rule.

---

# Numerical Optimizations

The solver contains numerous low-level optimizations while preserving numerical accuracy.

Implemented optimizations include:

- Precomputed reciprocal grid spacing
- Elimination of repeated divisions
- Cached row offsets for structured-grid indexing
- Reduced address arithmetic
- Cache-friendly memory traversal
- Optimized pressure residual computation
- Reduced temporary allocations
- Optimized boundary-condition application
- Optimized predictor and corrector kernels
- Parallel execution
- GPU acceleration
- Multigrid-accelerated pressure solver

The implementation prioritizes computational performance without changing the numerical formulation.

---

# Architecture

```text
CFD-Solver-2D/
├── .vscode/
├── output/
├── src/
│   ├── main.cpp
│   ├── Config.cpp
│   ├── Mesh.cpp
│   ├── Solver.cpp
│   ├── Multigrid.cpp
│   └── tiny_obj_loader_impl.cpp
├── include/
│   ├── Config.hpp
│   ├── Mesh.hpp
│   ├── Solver.hpp
│   ├── Multigrid.hpp
│   └── tiny_obj_loader.h
├── models/
├── lib/
│   ├── sfml/
│   └── stl_reader/
├── build/
├── CMakeLists.txt
├── LICENSE
└── README.md
```

---

# Requirements

- C++17 compatible compiler
- CMake 3.10+
- SFML
- ParaView (optional)

Supported compilers:

- MSVC 2022+
- GCC 9+
- Clang 10+

---

# Build (PowerShell)

```powershell
cd "...\CFD-Solver-2D"

if (Test-Path build) { Remove-Item build -Recurse -Force }
if (Test-Path install) { Remove-Item install -Recurse -Force }

cmake -S . -B build -G "Visual Studio 18 2026" -A x64

cmake --build build --config Release

cmake --install build --config Release --prefix install
```

---

# Run

```powershell
.\install\bin\cfd_app.exe
```

Configure:

- Domain size
- Grid resolution
- Flow parameters
- Reynolds number
- Time parameters
- Pressure solver parameters
- Geometry
- Slice orientation
- Visualization options

After confirmation the simulation starts immediately.

---

# Visualization

The application provides built-in real-time visualization.

Available display modes:

- Pressure
- Velocity magnitude
- Velocity vectors
- Solid mask

Interactive controls include:

- Pause
- Resume
- Simulation speed
- Camera movement
- Zoom
- Time navigation
- Rendering mode switching

---

# Export

Simulation results are automatically written in VTK format.

The exported files contain:

- Pressure (Pa)
- Velocity vectors
- Solid mask

The files can be opened directly in ParaView for further analysis, contour generation, streamline visualization and animation.

---

# Performance

The solver is designed to remain numerically faithful while reducing computational overhead through algorithmic and implementation-level optimizations.

Performance improvements include:

- reduced arithmetic cost;
- improved cache locality;
- reduced indexing overhead;
- optimized memory access patterns;
- accelerated pressure solver;
- optimized boundary-condition processing.

---

# Future Work

Possible future extensions include:

- Adaptive mesh refinement (AMR)
- Turbulence models
- Compressible flow solver
- Cavity flow
- Moving objects (NO idea how to do it for now, but ill figure that out)
- Flow start coordinates and width of the flow

---

## Contributing / Feedback

This is a personal educational project, but suggestions and issues are welcome. Feel free to open an issue or pull request.

---

**Happy simulating!**  
If you have any questions, don't hesitate to open an issue.
