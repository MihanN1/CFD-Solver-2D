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

# Future Work

Possible future extensions include:

- Uploading last .vtk of some past simulations to continue on with it.
- Adaptive mesh refinement (AMR)
- Turbulence models
- Compressible flow solver
- Cavity flow
- Add optional gravity for fluids
- Moving objects (NO idea how to do it for now, but ill figure that out)
- Moving WALLS
- Flow start coordinates and width of the flow

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
│   ├── MultigridCuda.cu
│   └── tiny_obj_loader_impl.cpp
├── include/
│   ├── Config.hpp
│   ├── Mesh.hpp
│   ├── Solver.hpp
│   ├── Multigrid.hpp
│   ├── MultigridCuda.cuh
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

## Contributing / Feedback

This is a personal educational project, but suggestions and issues are welcome. Feel free to open an issue or pull request.

---

**Happy simulating!**  
If you have any questions, don't hesitate to open an issue. Complete guide for our solver is down here.

Alright. Building it up from the physics, because every design choice in the code falls out of one problem.

## The one-paragraph version

Pressure has no equation of its own; it's whatever makes the flow divergence-free. So each step you advance momentum ignoring pressure, measure the divergence you created, solve a Poisson equation for the pressure that cancels it, and subtract its gradient. The grid is staggered so pressure gradients and divergences land exactly where they're needed and the chessboard mode can't survive. The Poisson operator encodes every boundary condition in its coefficients, which guarantees it's exactly `div ∘ grad` and therefore that the projection actually projects. Multigrid solves it in `O(N)` by exploiting the fact that SOR smooths error fast but converges slowly — so you smooth on every grid size at once. Everything else is SIMD, threads, and not allocating memory in the inner loop.

---

## 1. What we're actually solving

Incompressible Navier–Stokes:

```
∂u/∂t + (u·∇)u  =  −∇p + ν∇²u        momentum
∇·u = 0                                incompressibility
```

Read the momentum equation as "F = ma for a blob of fluid": it accelerates because neighbours push it (pressure), because friction drags it (viscosity), and it carries itself along (convection).

The second equation is the troublemaker. It's not an evolution equation — there's no `∂/∂t` in it. It's a **constraint**: at every instant, everywhere, the fluid must neither pile up nor thin out. What flows into a cell must flow out.

And notice: **there is no equation for pressure anywhere.** No `∂p/∂t`. Pressure isn't a thing that evolves — it's whatever value it has to take, right now, to make the constraint hold. It's a Lagrange multiplier. That single fact determines the entire architecture of the solver.

---

## 2. The projection idea

If you can't march pressure forward in time, what do you do? You cheat, then fix it.

**Step 1 — cheat.** Advance the momentum equation and pretend pressure doesn't exist:

```
u* = uⁿ + dt·( −(u·∇)u + ν∇²u )
```

`u*` is a perfectly good velocity field except for one thing: it's not divergence-free. Mass is appearing and vanishing all over the place.

**Step 2 — measure the damage.** Compute `∇·u*` in every cell. Positive means mass is being created there, negative means destroyed.

**Step 3 — find the fix.** We want a correction that removes exactly that divergence. Write the correction as the gradient of some scalar `p` (this isn't arbitrary — the Helmholtz decomposition says any vector field splits uniquely into a divergence-free part plus a gradient, and we want to throw away the gradient part):

```
uⁿ⁺¹ = u* − dt·∇p
```

Take the divergence of both sides and demand `∇·uⁿ⁺¹ = 0`:

```
0 = ∇·u* − dt·∇²p        →        ∇²p = ∇·u* / dt
```

That's a **Poisson equation**. Solve it, and you get the pressure field whose gradient exactly cancels the divergence you created.

**Step 4 — apply it.** `uⁿ⁺¹ = u* − dt·∇p`. Now the field is divergence-free and you've advanced one step.

That's Chorin's fractional step method, and it's literally the four function calls in `Solver::run`:

```cpp
predictor();      // step 1
solvePoisson();   // steps 2 and 3
corrector();      // step 4
```

The Poisson solve is ~90% of the runtime. Everything else in this codebase is either feeding it or making it fast.

---

## 3. The grid, and why it's weird

The obvious layout — put `u`, `v`, `p` all at cell centres — is broken. Here's why.

To get `∂p/∂x` at a cell centre from centred values you'd use `(p[i+1] − p[i−1]) / 2dx`. Now imagine a pressure field that alternates `+1, −1, +1, −1` like a chessboard. Every `p[i+1] − p[i−1]` is zero. **That field produces no force at all.** Nothing in the equations damps it, so it grows from roundoff noise until your pressure plot looks like a chessboard. This is a genuine, famous failure mode.

The fix is to **stagger**: pressure at cell centres, velocities on cell faces.

```
              v(i,j+1)
                 ↑
        ┌────────┴────────┐
        │                 │
 u(i,j) →     p(i,j)      → u(i+1,j)
        │                 │
        └────────┬────────┘
                 ↑
              v(i,j)
```

Now `∂p/∂x` at the `u` face between cells `i−1` and `i` is `(p[i] − p[i−1]) / dx` — **adjacent** cells, no gap. A chessboard produces a huge gradient and gets crushed immediately. Same for divergence: it's the net flux through the four faces of a cell, which are exactly where the velocities live. Everything lands where you need it. No interpolation.

The price is that the three fields have three different shapes, and this is where the index arithmetic bites:

```
p   nx     × ny        row stride = nx        idxP(i,j) = j*nx     + i
u   (nx+1) × ny        row stride = nx+1      idxU(i,j) = j*(nx+1) + i    ← different!
v   nx     × (ny+1)    row stride = nx        idxV(i,j) = j*nx     + i
```

`u` has `nx+1` columns because a row of `nx` cells has `nx+1` vertical faces (think fence posts vs fence panels). Mixing up `j*nx` and `j*(nx+1)` is the single easiest way to destroy this solver, and it's exactly the bug that was in there.

---

## 4. Walking through one time step

### `computeDt` — how big a step can we take?

The scheme is explicit, so there's a speed limit. Two of them.

**Advective (CFL):** in one step, fluid must not cross more than a fraction of a cell. If it jumped two cells, the stencil never even looked at the cell it passed through — information outran the numerics. Condition: `|u|·dt/dx + |v|·dt/dy ≤ 1`.

**Diffusive:** the explicit viscous term goes unstable if `dt > 1/(2ν(1/dx² + 1/dy²))`. Physically, momentum must not diffuse more than about a cell per step.

Take the smaller, multiply by `CFL` (0.4) and `dtSafety` (0.9) for margin.

One refinement: the Courant number is computed **per cell** from that cell's own faces, then maxed. Taking a global `max|u|` and a global `max|v|` and adding them assumes the worst horizontal and worst vertical flow happen in the same place, which they usually don't — so you'd shrink `dt` for a cell that doesn't exist.

Called every 5 steps, not every step, since it's a full sweep over the grid and velocities don't change much in 5 steps.

### `predictor` — momentum without pressure

For each `u` face, evaluate the right-hand side and step forward:

```cpp
dudx = (uij > 0) ? (uij − uleft)*invDx : (uright − uij)*invDx;   // upwind
dudy = (vn  > 0) ? (uij − ubot )*invDy : (utop   − uij)*invDy;
d2x  = (uright − 2*uij + uleft)*invDx2;                          // central
d2y  = (utop   − 2*uij + ubot )*invDy2;

uStar = uij − dt*(uij*dudx + vn*dudy) + dt*ν*(d2x + d2y);
```

**Convection uses upwind** — the derivative is taken on the side the flow is *coming from*. If fluid moves right, what's arriving at this face came from the left, so ask the left neighbour. Using a centred difference here would be unstable: it lets information propagate against the flow, which is physically wrong and numerically explosive.

(Upwind is only first-order accurate and adds artificial diffusion of roughly `u·dx/2`. That's the source of the "why is there no wake" question — on a coarse grid that numerical viscosity can exceed your physical `ν`.)

**Diffusion uses centred differences** — friction genuinely acts in both directions equally, no upwinding needed.

`vn` is the vertical velocity *at the u-face*, which doesn't exist there, so it's the average of the four surrounding `v` faces. This is the one place staggering makes you interpolate.

Then `v` gets the mirror-image treatment.

### `solvePoisson` — build the right-hand side, then solve

The RHS is one line of physics per cell:

```cpp
div = (u*[i+1,j] − u*[i,j])·invDx + (v*[i,j+1] − v*[i,j])·invDy;
rhs = div / dt;
```

Flux out the right face minus flux in the left, plus top minus bottom. That's net mass creation. Divide by `dt`.

Then hand it to the multigrid, which is §5.

### `corrector` — apply the fix

```cpp
u[i,j] = u*[i,j] − dt·(p[i,j] − p[i−1,j])·invDx;
v[i,j] = v*[i,j] − dt·(p[i,j] − p[i,j−1])·invDy;
```

Two adjacent pressures, subtract, scale. Done. Notice how clean this is *because* of staggering.

---

## 5. The Poisson operator — the heart of the thing

This is the part worth understanding properly, because it's where correctness lives.

### The core requirement

The three operators — the **divergence** that builds the RHS, the **Laplacian** that gets solved, and the **gradient** the corrector applies — must satisfy `Laplacian = divergence ∘ gradient` **exactly, face by face**. Not approximately. Not "except at boundaries."

If they disagree at even one face, then at that cell `∇·uⁿ⁺¹ ≠ 0` no matter how perfectly you solve the linear system. And since the error persists into the next step, it accumulates. Forever.

### How boundaries are handled

The trick is that **boundary conditions live in the coefficients**, not in a separate fix-up pass. The operator for each cell is:

```
(L p)ᵢⱼ = cW·p(i−1,j) + cE·p(i+1,j) + cS·p(i,j−1) + cN·p(i,j+1) − diag·pᵢⱼ
```

and each coefficient answers one question: **is this face something the corrector will update?**

| Face | Is velocity there prescribed? | Coefficient |
|---|---|---|
| between two fluid cells | no, corrector owns it | `1/dx²` or `1/dy²` |
| touching a solid cell | yes — it's 0 | **0** |
| inlet, `i=0` | yes — it's `U0` | **0** |
| walls, `j=0` and `j=ny` | yes — it's 0 | **0** |
| outlet, `i=nx` | no — free to adjust | Dirichlet, see below |

The logic is beautifully simple: **if the corrector can't change the velocity on a face, that face contributes nothing to the Laplacian.** A closed coefficient and a prescribed velocity are the same statement.

The outlet is the one interesting case. We want `p = 0` *on the face*, but pressure lives at cell centres, half a cell away. So use a ghost value `p_ghost = −p(nx−1,j)` — then the average of the two, which is the face value, is exactly zero. Its contribution to the Laplacian is `(p_ghost − p)/dx² = −2p/dx²`: nothing in the numerator, `2/dx²` added to the diagonal. And the corrector applies the matching gradient:

```cpp
u[nx,j] = u*[nx,j] + 2·dt·p[nx−1,j]·invDx;
```

Two payoffs: (a) with one real Dirichlet condition the matrix is non-singular, so no pinning and no drift; (b) the outlet velocity is *corrected by the pressure*, so the solver balances outflow against inflow by itself. Measured mass error: 0.00000%.

### The identity

Substitute the corrector into the divergence and everything telescopes:

```
∇·uⁿ⁺¹ = ∇·u* − dt·(L p)
```

with exactly that `L`. Set `rhs = ∇·u*/dt`, solve `L p = rhs`, and you get `∇·u* − dt·(∇·u*/dt) = 0`. Guaranteed by construction.

### What's actually stored

Six float arrays per grid: `cW, cE, cS, cN, diag, invDiag`. Built once in `buildCoefficients()`, because the geometry never changes during a run. The solver loop becomes:

```cpp
num  = cW[id]*p[id−1] + cE[id]*p[id+1] + cS[id]*p[id−nx] + cN[id]*p[id+nx];
pNew = (num − rhs[id]) * invDiag[id];
p[id] += omega * (pNew − p[id]);
```

No branches. No "is this solid?" tests. No division — `invDiag` is precomputed, and division is ~20 cycles in the innermost loop of the whole program.

Cells that shouldn't be solved (solid, or fluid completely walled in) get `invDiag = 0`, so `pNew = 0` and the update does nothing. The masking handles solids for free.

---

## 6. Multigrid — why the solve is fast

### Why plain iteration is hopeless

Gauss-Seidel/SOR updates each cell from its four neighbours. Information moves **one cell per sweep**. On a 256-wide grid, a pressure change at the inlet needs 256 sweeps just to *reach* the outlet, and thousands to converge. That's `O(N²)` work per time step, and it's exactly what the old solver was doing — hence 70 ms/step.

### The insight

Watch what SOR actually does to the error. After two or three sweeps, the error is **smooth** — all the jagged, cell-to-cell wiggle is gone. What remains is a broad, gentle shape spanning the whole domain, and SOR barely touches it, because a smooth error looks locally like a constant and a constant is already "solved" for a Laplacian.

So SOR is excellent at high-frequency error and useless at low-frequency error.

But here's the thing: **smooth error is smooth relative to the grid spacing.** Put it on a grid with twice the spacing and it's no longer smooth — it's now high-frequency, and SOR eats it. Recurse.

### The V-cycle

```
at level L:
  smooth 2×                    kill the high frequencies here
  r = rhs − L·p                what's left over
  restrict r to level L+1      it's smooth, a coarser grid can hold it
  recurse                      solve L·e = r there
  p += prolongate(e)           bring the correction back up
  smooth 2×                    clean up interpolation artifacts
```

At the coarsest level the grid is tiny, so just hammer it with 50+ sweeps until it's solved.

Cost: each level is 4× smaller, and `1 + ¼ + ¹⁄₁₆ + … = ⁴⁄₃`, so a whole V-cycle costs about the same as ~3 fine-grid sweeps. And each cycle cuts the error by **~10×, independent of grid size**. That's the whole point: `O(N²)` becomes `O(N)`, and the iteration count stops caring how big your grid is.

Two V-cycles per time step is usually enough.

### Restriction and prolongation must match

Going down (**restriction**) and coming back up (**prolongation**) can't be designed independently. The requirement is `R = Pᵀ / (cells per coarse cell)`.

When that holds, the coarse-grid correction is an orthogonal projection in the energy norm — it provably **cannot make the error worse**. When it doesn't hold, you have no guarantee, and the two-grid operator can have spectral radius above 1, meaning each V-cycle *amplifies* error. This was the bug that made 100×100 explode while 128×128 worked fine.

So `P` is cell-centred bilinear (a fine cell sits ¼ of a coarse cell off-centre, giving weights ¾ and ¼ per axis), and `R` is computed as its literal transpose — implemented as a gather so OpenMP doesn't need atomics.

Two extra wrinkles:

- **Only coarsen even cell counts.** An odd count leaves the last coarse cell covering one fine cell instead of two, its column of `P` carries half the weight of the others, and the coarse grid gets a residual that's half as big as it should be. Inconsistent → divergence.
- **Semi-coarsening.** A point smoother only damps error in the direction it's strongly coupled to. On a grid with `dx ≪ dy` the `y` coupling (`1/dy²`) is tiny, so `y` error survives and the cycle stalls. So when the aspect ratio is worse than 2:1, only the over-resolved axis gets coarsened, driving the coarse grids toward isotropy.

---

## 7. The solid body

The obstacle isn't a mesh — it's a **mask**. `Mesh` rasterises the geometry (a circle, or a slice through an STL/OBJ) into a per-cell `solid` array of 0s and 1s. Immersed boundary, simplest flavour.

From that, `buildFaceMasks()` derives which faces are open:

```cpp
uOpen[idxU(i,j)] = !solid[j*nx+i] && !solid[j*nx+i−1];
```

A face is open only if fluid sits on both sides. The corrector zeroes velocity on every closed face, which enforces no-slip, and the same mask closes the corresponding Poisson coefficient — so the operator and the boundary condition are automatically consistent. One mask, two uses, no way for them to drift apart.

On coarse multigrid levels, a cell is solid only if **all** the fine cells it covers are solid, so a partially-blocked coarse cell still carries fluid and the body stays visible at every level.

---

## 8. The speed layer

Everything above is the algorithm. This part is just making it run fast without changing a single number.

**Branch-free hot loops.** Every velocity on a closed face is held at exactly zero as an *invariant*. So the stencil can be evaluated unconditionally — the worst it ever reads is a legitimate zero — and the result multiplied by a float mask. No branch, no scalar fallback near the body.

**Halo padding.** Each pressure array is allocated with `max(nx,8)` extra floats at *both* ends, with the logical pointer offset into the middle. So `p[id−1]`, `p[id+nx]` etc. are always valid memory, even for the very first and last cell. An 8-wide AVX load at the edge reads into the halo (which is zero, and whose coefficients are zero anyway) instead of segfaulting. This is what makes the branch-free vector code *safe*, not just fast.

**Red/black SOR with masked stores.** Every neighbour of a red cell is black, so all red cells can update simultaneously. The naive version strides by 2, which kills SIMD. Instead: compute the update for all 8 lanes, then store only the current colour with `_mm256_maskstore_ps`. Half the arithmetic is discarded, but every load and store stays contiguous — a big net win on a memory-bound kernel. Since vectors always start at even `i`, the lane mask is one of exactly two constants.

**OpenMP.** Rows go to different threads. Red/black makes this race-free with no locks. One fork/join per `smooth()` call, and levels under 32 rows run serially — they're a few hundred cells visited by every cycle, and barrier traffic there costs more than the arithmetic.

**CUDA.** Same algorithm, one thread per cell. The whole hierarchy is allocated once, the stencil is uploaded once (it's a function of geometry, and geometry is static), and the pressure field **stays resident on the GPU between time steps** — which means last step's solution is a free warm start. Only the RHS crosses the bus each step. Kernels run on the default stream, which serialises them, so the chain `smooth → residual → restrict → recurse → prolongate → smooth` needs no explicit synchronisation.

---

## 9. Output

`saveVTK` byte-swaps values into a 16 KB stack buffer and writes binary legacy VTK straight out — no temporary arrays, no per-cell copies. Pressure is stored internally as `p/ρ` (kinematic), so it's multiplied by `ro` on the way out to give Pascals. Btw kinematic pressure is much easier to use cause if u divide regular pressure by density u get m^2/s^2, not some kg/(m*s^2)
