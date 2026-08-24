# CFD-Solver-2D — the whole thing, explained

This document builds upwards: the physics first, then the numerical scheme that
falls out of it, then the code structure that falls out of the scheme, then
every file and every function, then the build, the variant matrix, the releases
and the installers.

Read it front to back, or jump around the contents.

---

## Contents

1. [The project on one page](#1-the-project-on-one-page)
2. [The physics: what is actually being solved](#2-the-physics-what-is-actually-being-solved)
3. [Chorin's projection method](#3-chorins-projection-method)
4. [The staggered MAC grid and the indices](#4-the-staggered-mac-grid-and-the-indices)
5. [`Config` — every number the run uses](#5-config--every-number-the-run-uses)
6. [`AppPaths` — where the program writes](#6-apppaths--where-the-program-writes)
7. [`main` — the entry point](#7-main--the-entry-point)
8. [`Mesh` — geometry and the solid mask](#8-mesh--geometry-and-the-solid-mask)
9. [`Solver` — one time step](#9-solver--one-time-step)
10. [`Multigrid` — why the pressure solve is fast](#10-multigrid--why-the-pressure-solve-is-fast)
11. [`MultigridCuda` — the same thing on the GPU](#11-multigridcuda--the-same-thing-on-the-gpu)
12. [The speed layer: AVX2, OpenMP, halo, red/black](#12-the-speed-layer-avx2-openmp-halo-redblack)
13. [Output: the VTK format](#13-output-the-vtk-format)
14. [`CMakeLists.txt` line by line](#14-cmakeliststxt-line-by-line)
15. [The build matrix: why thirty rows](#15-the-build-matrix-why-thirty-rows)
16. [The build and release scripts](#16-the-build-and-release-scripts)
17. [GitHub Actions](#17-github-actions)
18. [The installers](#18-the-installers)
19. [Cheat sheet: symptom to file](#19-cheat-sheet-symptom-to-file)

---

## 1. The project on one page

This project is **two separate programs** plus the machinery around them.

```
                       you (a console)                you (a window)
                            |                              |
                            v                              v
                   +-----------------+           +-------------------+
                   | Fluid Solver    |           | Fluid Solver UI   |
                   | (cfd_app)       | <-------- | its own build,    |
                   | computes, has   |  runs it  | SFML, its own     |
                   | no window       |  as a     | branch            |
                   +-----------------+  child    +-------------------+
                            |           process           ^
                            v                             |
                     output/solution_N.vtk ----------------
                            |
                            v
                        ParaView
```

Everything in `src/` and `include/` is **the solver only**. SFML is not linked
into it at all, and no copy of it is vendored here any more: the UI lives on its
own branch and carries its own in `third_party/sfml`.
The solver draws nothing: it writes `.vtk` frames, and either the UI or
ParaView draws them.

Inside the solver there are four bricks:

| Brick | Files | Responsibility |
|---|---|---|
| `Config` | `Config.hpp/.cpp` | every number of the run, keyboard and command-line input |
| `Mesh` | `Mesh.hpp/.cpp` | loads STL/OBJ, sections it with a plane, rasterises it into the `solid` mask |
| `Solver` | `Solver.hpp/.cpp` | the time loop: predictor → pressure → corrector → VTK |
| `Multigrid` | `Multigrid.hpp/.cpp`, `MultigridCuda.cuh/.cu` | solves the pressure Poisson equation |

Plus two helpers: `AppPaths` (where `output` lives on disk) and `main.cpp` (the
glue).

The machinery:

| What | Where | Why |
|---|---|---|
| Build | `CMakeLists.txt` | four switches: AVX2, OpenMP, CUDA, static linking |
| Every variant | `scripts/build-*.{sh,ps1}` | run all combinations on your own machine |
| Release | `scripts/make-release.{sh,ps1}` | build the variants, assemble `release/<version>/` |
| CI | `.github/workflows/build-all.yml` | the same, but on real hardware for each system |
| Installers | `installer/windows|linux|macos/` | Inno Setup, makeself, productbuild |
| Installers in CI | `.github/workflows/build-installers.yml` | rebuild `dist/` out of a published release and package it |

The one fact all of that machinery grows out of: **a binary cannot decide at
run time whether it is allowed to execute an AVX2 instruction.** It just dies
with an illegal instruction. So AVX2 cannot be a flag — it has to be two
different binaries. And once you are splitting anyway, OpenMP and CUDA are
easier to split too, so that `plain` starts on absolutely anything. Hence the
8/4/2/4 matrix, and hence the questions the installers ask.

---

## 2. The physics: what is actually being solved

The incompressible Navier–Stokes equations in 2D:

```
∂u/∂t + (u·∇)u  =  −∇p + ν∇²u        momentum
∇·u = 0                                incompressibility
```

The first one reads as "F = ma for a blob of fluid":

* `(u·∇)u` — **convection**: the blob carries itself along with the flow;
* `−∇p` — **pressure**: the neighbours push;
* `ν∇²u` — **viscosity**: friction against the neighbours smears velocity out.

The second one is where all the trouble comes from. There is no `∂/∂t` in it.
It is not an evolution equation, it is a **constraint**: at every point, at
every instant, whatever flows in flows out. Nothing piles up and nothing
vanishes.

And crucially: **there is no equation for pressure.** No `∂p/∂t`, no
`p = f(ρ, T)` — the fluid is incompressible. Pressure is a Lagrange multiplier:
whatever value the field has to take, right now, to make the constraint hold.
That single fact determines the entire architecture of the solver.

### So what is `ro` doing in the config

Internally the solver works in **kinematic pressure**, `p/ρ`. The units are
m²/s², and the momentum equation then honestly carries `−∇p` with no division
by `ρ`. The density `cfg.ro` is used exactly once — on output, in `saveVTK`,
where `p * ro` gives pascals. See the `writeFloat(p[row + i] * cfg.ro)` line in
`Solver.cpp`.

### Why Reynolds is not an input

The solver is dimensional: it integrates with `nu` directly. The Reynolds
number is a consequence of the other settings, not an input:

```
Re = U0 * D / nu,   where D = 0.2 * min(Lx, Ly)
```

`D` is the obstacle diameter, and it is pinned to the domain size:
`OBSTACLE_DOMAIN_FRACTION = 0.2` in `Mesh.cpp` scales any imported geometry to
that span, and the fallback circle has radius `0.1 * min(Lx, Ly)`, which is the
same diameter. To drive a run by Reynolds, convert on the way in:
`nu = U0 * D / Re`.

`Config.cpp` carries a long comment about this: the old prompt asked for `Re`
with "0 to auto-compute later", but nothing ever consumed the value —
`print()` merely echoed it back. It was removed rather than left there implying
that typing a Reynolds number would change anything.

### Boundary conditions

| Boundary | Condition | How it looks in the code |
|---|---|---|
| Inlet (left, `i=0`) | `u = U0` | `applyBC()`: `u[idxU(0,j)] = U0` |
| Outlet (right, `i=nx`) | pressure `p = 0` on the face | a Dirichlet coefficient in `buildCoefficients`, a matching term in `corrector` |
| Top and bottom (`j=0`, `j=ny`) | free slip: `v = 0`, `∂u/∂y = 0` | `applyBC()` zeroes `v`; `predictor` replaces the missing neighbour with the face itself on the edge rows |
| The body | no slip: face velocity is 0 | face masks, see `buildFaceMasks()` |

---

## 3. Chorin's projection method

If pressure cannot be marched forward in time, you cheat, then fix it.

**Step 1 — cheat.** Advance momentum and pretend pressure does not exist:

```
u* = uⁿ + dt·( −(u·∇)u + ν∇²u )
```

`u*` is a perfectly good velocity field except for one thing: it is not
divergence-free. Mass is appearing and vanishing all over the place.

**Step 2 — measure the damage.** Compute `∇·u*` in every cell. Positive means
mass is being created there, negative means destroyed.

**Step 3 — find the fix.** We want a correction that removes exactly that
divergence. Write it as the gradient of a scalar `p`. That is not arbitrary:
the Helmholtz decomposition says any vector field splits uniquely into a
divergence-free part plus a gradient, and the gradient part is precisely what
we want to throw away.

```
uⁿ⁺¹ = u* − dt·∇p
```

Take the divergence of both sides and demand `∇·uⁿ⁺¹ = 0`:

```
0 = ∇·u* − dt·∇²p        →        ∇²p = ∇·u* / dt
```

That is a **Poisson equation**. Solve it and you get the pressure field whose
gradient exactly cancels the divergence you created.

**Step 4 — apply it.** `uⁿ⁺¹ = u* − dt·∇p`.

Which is literally three calls in `Solver::run`:

```cpp
predictor();      // step 1
solvePoisson();   // steps 2 and 3
corrector();      // step 4
```

The Poisson solve is **about 90% of the run time**. Everything else in this
codebase is either feeding it or making it fast.

---

## 4. The staggered MAC grid and the indices

### Why everything at cell centres is broken

The obvious layout — `u`, `v`, `p` all at cell centres — does not work. Here is
why. To get `∂p/∂x` at a cell centre from centred values you would use
`(p[i+1] − p[i−1]) / 2dx`. Now imagine a pressure field alternating
`+1, −1, +1, −1` like a chessboard. Every `p[i+1] − p[i−1]` is zero. **That
field produces no force at all.** Nothing in the equations damps it, so it
grows out of rounding noise until the pressure plot looks like a chessboard.
This is a real, famous failure mode, not a theoretical worry.

### The fix — stagger

Pressure at cell centres, velocities on cell faces:

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

Now `∂p/∂x` at the `u` face between cells `i−1` and `i` is
`(p[i] − p[i−1]) / dx` — **adjacent** cells, no gap in the middle. A chessboard
produces a huge gradient and is crushed immediately. Same for divergence: it is
the net flux through the four faces of a cell, and the velocities live on
exactly those faces. Everything lands where it is needed. No interpolation.

### The price — three different array shapes

```
p   nx     × ny        row stride = nx        idxP(i,j) = j*nx     + i
u   (nx+1) × ny        row stride = nx+1      idxU(i,j) = j*(nx+1) + i    ← different!
v   nx     × (ny+1)    row stride = nx        idxV(i,j) = j*nx     + i
```

`u` has **`nx+1`** columns because a row of `nx` cells has `nx+1` vertical
faces — fence posts versus fence panels. Mixing up `j*nx` and `j*(nx+1)` is the
single easiest way to destroy this solver, and it is exactly the bug that was
once in there.

The three helpers live in `Solver.hpp` as `inline`:

```cpp
inline int idxP(int i, int j) const { return j * cfg.nx + i; }
inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
inline int idxV(int i, int j) const { return j * cfg.nx + i; }
```

`idxV` has the same formula as `idxP` — but not the same meaning and not the
same legal range of `j` (`v` goes up to `ny` inclusive). Keeping them separate
is the only way to read this code without losing track.

---
## 5. `Config` — every number the run uses

`include/Config.hpp` is a plain struct with fields and five methods. No logic,
no invariants, nothing private: it is a bag of numbers passed by const
reference into `Mesh` and `Solver`.

### The fields and what they actually do

| Field | Default | Consumed by |
|---|---|---|
| `Lx, Ly` | 1.0, 1.0 | `Mesh::createGrid` → `dx = Lx/nx`, `dy = Ly/ny` |
| `nx, ny` | 50, 50 | every array size; `main` requires ≥ 8 |
| `U0` | 1.0 | `applyBC`, `initFields` — the inlet velocity |
| `nu` | 0.01 | the diffusion term in `predictor`, the `dtDiff` limit in `computeDt` |
| `ro` | 1.225 | **only** `saveVTK`: `p * ro` gives pascals |
| `CFL` | 0.5 | `computeDt`: `dtAdv = CFL / maxCourant` |
| `totalTime` | 10.0 | the loop exit condition in `run` |
| `dtUpdateInterval` | 5 | how often `dt` is recomputed |
| `dtSafety` | 0.9 | margin covering the velocity growth in between |
| `omega` | 1.85 | relaxation on the **coarsest** level, where the smoother acts as a solver |
| `smootherOmega` | 1.15 | relaxation **inside** the V-cycle |
| `mgIterations` | 2 | V-cycles per time step |
| `mgTolerance` | 1e-4 | target relative residual `‖r‖/‖rhs‖` |
| `mgMinCoarseSize` | 8 | stop coarsening below this many cells per axis |
| `useCuda` | true | ignored on a build without CUDA |
| `saveInterval` | 20 | write a VTK file every N steps |
| `outputDir` | `"output"` | a relative path hangs off the executable |
| `geometryFile` | `"none"` | `"none"` → the fallback circle |
| `sliceAngleX`, `sliceAngleZ` | 0, 0 | orientation of the section plane, degrees |
| `sliceRotation` | 0 | rotation within the simulation plane |
| `invertSection` | false | mirror the section in X |

### About the two omegas

This is subtle and non-obvious, which is why it has its own comment in
`Config.hpp`. Strong over-relaxation (`omega ≈ 1.85`) is an excellent
**solver** on its own but a **bad smoother**: it kills low frequencies well and
high frequencies badly. Inside a V-cycle you want exactly the opposite — kill
the high frequencies, the low ones will go down to the coarse grid. So inside
the cycle the relaxation is clamped to `smootherOmega ≈ 1.15`, and `omega` only
comes out at the bottom, where the smoother stops being a smoother and acts as
a solver.

### `readFromConsole()`

A flat list of `std::cout << "..."; std::cin >> field;`. Nothing clever, with
two exceptions.

`readGeometryPath()` (anonymous namespace at the top of the file) reads a
**whole line**, not a token:

```cpp
std::getline(std::cin >> std::ws, path);
```

`std::cin >> std::ws` eats leading whitespace and the leftover newline from the
previous read, and `getline` takes everything to the end — because a path to a
model can contain spaces. Then the surrounding quotes are stripped, because
Windows Explorer's "Copy as path" puts `"C:\...\model.stl"` on the clipboard,
quotes included.

At the very end there is `std::cin.ignore(..., '\n')`: after a `>>` the newline
is still in the buffer, and without this the next `getline` in `confirm()`
would immediately return an empty line and silently confirm the configuration.

### `print() const`

Prints every field with its unit. Called by `confirm()` before each question,
and by `main` in non-interactive mode.

### `setParam(key, value)` → `bool`

The non-interactive entry point. The key is lowercased by `toLower`, then a
long `else if` chain. Returns `false` on an unknown key — and `main` stops
there with a message rather than silently ignoring a typo.

Numbers are parsed with `strtof`/`strtod`/`atoi`. Note that `strtof` returns
`0` on garbage with no diagnostic, so `nx=abc` gives `nx = 0` and then trips
the `nx < 8` check in `main` — not the clearest error, but not a crash either.

### `modifyParam(name)` → `bool`

The same, interactively: asks for a new value for one parameter. The
`usedFormattedInput` flag separates two cases: after `std::cin >> x` the
newline has to be cleaned up, after `readGeometryPath()` it does not, because
`getline` already consumed it. The `usecuda` branch used to be missing, so
typing the name of a parameter that plainly exists in `setParam` answered
"Unknown parameter"; it is there now.

### `confirm()` → `bool`

```
print();
"To change a parameter, type its name and press Enter."
"To confirm, just press Enter (empty line)."
```

An empty line → `true`. Otherwise `modifyParam(input)` and `false`. In `main`
this spins in `while (!cfg.confirm()) {}`.

---

## 6. `AppPaths` — where the program writes

Three functions, all three existing because of one problem: **a process's
working directory depends on who started it.** A desktop shortcut, a file
manager and a terminal hand the process three different working directories, so
`"output"` meant three different folders and the frames went wherever the
launcher felt like.

### `executableDir()`

The directory of the **executable itself**, not the working directory.

* Windows: `GetModuleFileNameW` into a 32768-character buffer (the long path
  limit), then `.parent_path()`.
* macOS: `_NSGetExecutablePath` — first with `nullptr` to learn the required
  size, then for real; then `weakly_canonical` to resolve symlinks.
* Linux and everything else: `read_symlink("/proc/self/exe")`.

If the platform refuses to say, it falls back to `current_path()`. No supported
platform does.

### `userDataDir()`

The fallback location that is always writable:

* Windows: `%LOCALAPPDATA%`, or `%APPDATA%` if that is empty;
* macOS: `$HOME/Library/Application Support`;
* everything else: `$XDG_DATA_HOME`, otherwise `$HOME/.local/share`.

Then `base / "Fluid Solver"` — the application name is hard-coded in the
anonymous namespace as `kAppName`, because it names the installation and not
the simulation, and so has no business being in `Config`.

### `resolveOutputDir(outputDir)`

The logic:

1. Empty string → the executable's own directory.
2. Absolute path → taken as given.
3. Relative (the default `"output"`) → `executableDir() / outputDir`.

Then `directoryAcceptsFiles(target)`. And here is the important bit:
**checking the permission bits is not enough.** On Windows an install under
`Program Files` reads as writable to a standard user and then is not. So the
function honestly tries to create a `.fluid-solver-write-test` file, close it
and delete it. If that worked, the directory will do.

If it did not, it tries `userDataDir() / outputDir` and **prints the path the
frames actually went to**. It prints because an unexpected location is
something the user should be told, not something they should discover.

If that fails too — a message on `stderr` and the original path is returned;
frames simply will not be saved, but the run does not die.

`Solver::run()` calls this **once** and stores it in `outputPath`. That is
exactly why `saveVTK()` does not call `resolveOutputDir` itself: it prints, and
that should happen once per run rather than five hundred times.

---

## 7. `main` — the entry point

`src/main.cpp`, 96 lines. It does four things.

**1. Parses the arguments.** If `argc > 1`, every argument is a `key=value`
pair:

```cpp
const size_t eq = arg.find('=');
if (eq == std::string::npos || eq == 0) { /* error + printUsage */ }
cfg.setParam(arg.substr(0, eq), arg.substr(eq + 1));
```

`eq == 0` rejects `=value` with no key. `-h`/`--help` print `printUsage` and
exit.

**2. Or asks.** With no arguments: `readFromConsole()`, then the confirmation
loop, then a final print and the notes about STL/OBJ.

**3. Sanity check and object construction.**

```cpp
if (cfg.nx < 8 || cfg.ny < 8) { ... return 1; }
Mesh mesh(cfg);
mesh.printInfo();
Solver solver(cfg, mesh);
```

The threshold of 8 is not arbitrary: `mgMinCoarseSize` defaults to 8, and on a
grid smaller than that the multigrid hierarchy will not build at all.

**4. Runs it and times it.** `steady_clock` around `solver.run()`,
`std::chrono::duration<double>` in seconds.

And finally: `if (argc <= 1) { "Press Enter to exit..."; std::cin.get(); }` — so
that a console window opened by double-clicking a shortcut does not vanish
before you have read the result. With arguments (that is, from a script or from
the UI) it does not do this, or the process would hang forever.

---

## 8. `Mesh` — geometry and the solid mask

The body in this solver is **not a mesh**, it is a **mask**. `Mesh` rasterises
the geometry into a per-cell `solid` array of 0s and 1s. Immersed boundary,
simplest flavour.

### The constructor

```cpp
Mesh::Mesh(const Config& cfg) : nx(cfg.nx), ny(cfg.ny), cfg(cfg) {
    solid.resize(nx * ny, 0);
    createGrid();
    const bool geometryLoaded = loadGeometry(cfg.geometryFile);
    buildSection();
    rasterizeSection();
    buildSolid();
    if (!geometryLoaded || sectionContour.empty()) {
        // fallback circle, radius 0.1*min(Lx,Ly), centred in the domain
        initCircle(cfg.Lx/2, cfg.Ly/2, 0.1*std::min(cfg.Lx, cfg.Ly));
    }
}
```

Note the order: the geometry is tried honestly first, and **only if that came
to nothing** does the circle appear. The circle is not a "default geometry" —
it is the verification case: flow around a cylinder is a problem with a known
answer, and it is what you run to check the solver is alive at all.

### `createGrid()`

```cpp
dx = cfg.Lx / nx;
dy = cfg.Ly / ny;
x.resize((nx + 1) * (ny + 1));  y.resize((nx + 1) * (ny + 1));
x[j*(nx+1)+i] = i*dx;           y[j*(nx+1)+i] = j*dy;
```

`x`/`y` hold the **node** coordinates (there are `(nx+1)×(ny+1)` of them). The
solver does not use them: it works from `dx`, `dy` and indices. They are there
for completeness and possible output.

### `loadGeometry(filename)`

`"none"` or an empty string → `false` immediately, no message (that is the
normal case).

Otherwise `resolveGeometryPath()` tries three places **in this order**:

1. the path as given (relative to the working directory, or absolute);
2. `executableDir() / "models" / name` — **this is the installed-copy case**;
3. `CFD_MODELS_DIR / name` — a macro baked in at compile time.

That order exists because `CFD_MODELS_DIR` points at the machine the binary was
built on, not the machine it is running on. For an installed copy only step 2
works.

The extension picks the format: `.stl` → `loadSTL`, `.obj` → `loadOBJ`,
anything else → a message and `false`.

### `loadOBJ(filename)`

Through `tinyobjloader`. The material directory is derived from the file path
and gets a trailing separator, or the `.mtl` beside it will not be found.

Then **fan triangulation**: OBJ may contain n-gons, and the solver needs
triangles.

```cpp
for (std::size_t vertex = 1; vertex + 1 < faceVertices.size(); ++vertex)
    triangles.push_back({faceVertices[0], faceVertices[vertex], faceVertices[vertex+1]});
```

Negative `vertex_index` values and indices past the end of the coordinate array
are skipped — broken OBJ files turn up more often than one would like.

### `loadSTL(filename)`

Through `stl_reader` inside a `try/catch`: that library reports errors by
throwing. It just copies `tri_corner_coords` into `Triangle`.

### `buildSection()` — the densest function in the file

It turns a 3D mesh into a closed 2D contour. Six stages.

**(a) Bounding box and centre.** A pass over every triangle collecting min/max
on three axes. The centre is the middle of the box. `characteristicLength` is
the longest extent; if it is `≤ 0` the function returns (degenerate geometry).

**(b) A basis for the section plane.** Two angles build an orthonormal triple,
`Rz(angleZ) · Rx(angleX)`:

```cpp
sectionAxisX = { cosZ,             sinZ,            0    };
sectionAxisY = {-sinZ*cosX,        cosZ*cosX,       sinX };
sectionNormal= { sinZ*sinX,       -cosZ*sinX,       cosX };
```

The plane passes through the centre: `(x − c)·n = 0`.

**(c) Intersecting each triangle with the plane.** For the three vertices,
signed distances `d = (v − c)·n`. Then over the three edges:

* a vertex lying **on** the plane (`|d| ≤ tolerance`) → the vertex itself is
  added;
* opposite signs at the ends of an edge → the crossing point by linear
  interpolation:

```
x_section = a + d_a/(d_a − d_b) · (b − a)
```

Duplicates are rejected by `squaredDistance ≤ tolerance²` — otherwise a vertex
that lies on the plane would be added twice, since it belongs to two edges.

A triangle with exactly **two** intersection points contributes a section
segment. It is projected into 2D by dotting against `sectionAxisX`/
`sectionAxisY` and pushed into `segments`.

`tolerance = max(1e-12, characteristicLength * 1e-9)` is relative, because the
model might be in millimetres or in metres.

**(d) Segments into a graph.** `findOrAddNode` merges coincident endpoints (a
linear search, `O(n²)`; on a typical section that is fine). Edges go into a
`std::set<std::pair<int,int>>` normalised so `first < second`, which also
removes duplicates. Then an adjacency list is built.

**(e) Walking the contours, keeping the largest.** Walk unvisited edges; at
each step take the **first** neighbour not yet walked to. Closing back on the
starting node makes a loop. The `loop.size() <= edges.size()+1` guard stops an
infinite walk on broken geometry.

The loop's area is the shoelace formula:

```cpp
signedAreaTwice += first.x*second.y − second.x*first.y;
area = 0.5 * |signedAreaTwice|;
```

The loop with the largest area is kept and the rest are discarded. **This is
where the project's limitation lives**: geometry that sections into several
separate contours — a cascade of blades, say — loses everything but one.

**(f) Mirror, rotate, scale, centre.**

```cpp
if (invertSection) point.x = -point.x;
// rotation by sliceRotation
x' = cosR*x − sinR*y;   y' = sinR*x + cosR*y;
```

Then a bounding box of the contour, and:

```cpp
targetSpan = 0.2 * min(Lx, Ly);
scale = targetSpan / sectionSpan;
point.x = Lx/2 + scale*(point.x − centreX);
point.y = Ly/2 + scale*(point.y − centreY);
```

So **any** model is brought to a span of `0.2·min(Lx,Ly)` and placed at the
centre of the domain. That preserves the old circle's diameter and makes `Re`
predictable.

### `rasterizeSection()`

The first of two rasterisation passes — the **boundary**. For each cell take
its centre and measure the distance to every contour segment
(`distanceToSegment` is the classic point-on-segment projection with the
parameter clamped to `[0,1]`). If the distance is `≤ boundaryRadius =
0.5·hypot(dx,dy)` — half a cell diagonal — the cell is marked solid.

The cost is honestly `O(nx·ny·|contour|)`. For 256×128 and a couple of hundred
contour points that is a few million operations, once, at start-up.

### `pointInsideSection(x, y)`

The even-odd rule: cast a horizontal ray and count crossings.

```cpp
crossesRay = ((first.y > pointY) != (second.y > pointY)) &&
             (pointX < (second.x−first.x)*(pointY−first.y)/(second.y−first.y) + first.x);
if (crossesRay) inside = !inside;
```

### `buildSolid()`

The second pass — the **fill**. Anything not already marked as boundary and
lying inside the contour becomes solid. Two passes rather than one, because
thin parts of a contour may not catch a single cell centre and the fill alone
would miss them.

### `initCircle(cx, cy, R)` and `printInfo()`

`initCircle` is a direct `dist ≤ R` test on cell centres. `printInfo` prints
`nx`, `ny`, `dx`, `dy`, the solid cell count, the triangle count and the
section point count. This is the first place to look when "the model did not
load": zero triangles means the file was not read; triangles but zero section
points means the plane missed, or the geometry is not closed.

---
## 9. `Solver` — one time step

`Solver.hpp` declares the fields and a dozen methods; `Solver.cpp` is 967 lines,
well over half of which are the AVX2 versions of the very same loops.

### The fields

```cpp
std::vector<float> p, rhs;              // nx*ny
std::vector<float> u, u_star;           // (nx+1)*ny
std::vector<float> v, v_star;           // nx*(ny+1)
std::vector<uint8_t> uFluidMask, vFluidMask, solidMask;
std::vector<float>   uFluidMaskF, vFluidMaskF;   // the same masks as float
float dx, dy, invDx, invDy, invDx2, invDy2;      // cached, never change
std::filesystem::path outputPath;                // resolved once
```

The masks are stored **twice** — as `uint8_t` for branching and as `float` so
that SIMD code can simply **multiply** by the mask instead of testing it.
Duplicating that memory is cheaper than converting in a hot loop.

`invDx`, `invDx2` and friends exist because a division costs about 20 cycles
and a multiply costs one. There is not a single division in the innermost loop
of the program.

### The constructor

Allocates six arrays and caches the grid spacing. `Multigrid` is constructed in
the initialiser list — it needs `nx, ny, dx, dy, mgMinCoarseSize`.

### `buildFaceMasks()`

One line of meaning:

```cpp
uFluidMask[idxU(i,j)] = !solidMask[row+i] && !solidMask[row+i-1];
```

A face is **open only if fluid sits on both sides**. For `u` that is the left
and right neighbours, for `v` the bottom and top. The ranges matter: `u` is
computed for `i = 1..nx-1` (the outer faces are the inlet and outlet, which
have their own conditions) and `v` for `j = 1..ny-1` (the outer ones are the
walls).

One mask, **two uses**: the corrector zeroes velocity on every closed face —
that is the no-slip condition — and the same mask closes the corresponding
Poisson coefficient. The operator and the boundary condition are automatically
consistent, and there is no way for them to drift apart.

### `initFields()`

1. `solidMask` from `mesh.solid`.
2. `buildFaceMasks()`.
3. `multigrid.setUseCuda(cfg.useCuda)` and `multigrid.setGeometry(solidMask)`.
4. Zero every field.
5. **Start from a uniform stream**: every open `u` face gets `U0`. The outer
   faces too, if the cell beside them is not solid.
6. `applyBC()`.
7. Print the multigrid level count and the backend (CUDA or CPU).

And a warning worth reading:

```cpp
if (multigrid.levelCount() < 3 && (nx > 32 || ny > 32))
    "few multigrid levels ... 256x128 give the deepest hierarchy"
```

An axis is only coarsened while its cell count is **even** (why, in the
multigrid section). A grid that is odd in both directions gets no hierarchy at
all and the pressure solve degrades to plain SOR. `100×100` looks like a
respectable number and coarsens exactly once (`50` is even, `25` is not).
`256×128` coarsens all the way down.

### `computeDt()`

The scheme is explicit, so there is a speed limit. Two of them.

**Advective (CFL).** In one step, fluid must not cross more than a fraction of
a cell. If it jumped two cells, the stencil never even looked at the cell it
passed through — information outran the numerics.

```
|u|·dt/dx + |v|·dt/dy ≤ 1
```

**Diffusive.** The explicit viscous term goes unstable if
`dt > 1/(2ν(1/dx² + 1/dy²))`. Physically: momentum must not diffuse more than
about a cell per step.

```cpp
dtAdv  = CFL / maxCourant;
dtDiff = 1 / (2·nu·(invDx2 + invDy2));
dt     = dtSafety · min(dtAdv, dtDiff);
```

The refinement the whole loop exists for: the Courant number is computed
**per cell** from that cell's own faces, and only then maxed. Taking a global
`max|u|` and a global `max|v|` and adding them assumes the worst horizontal and
worst vertical flow happen in the same place. They usually do not, so `dt`
would be shrunk for a cell that does not exist.

The implementation: `#pragma omp parallel` with a per-thread accumulator,
inside it an AVX2 loop over 8 cells (`_mm256_max_ps` of the absolute values of
neighbouring faces) plus a scalar tail, and a `#pragma omp critical` at the end
to reduce the maximum.

`absMask()` is `_mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))` — clearing
the sign bit, i.e. `fabs` for a whole vector at once. `horizontalMax()` folds
the 8 lanes into one in three steps.

At the bottom, a guard: `if (!(dt > 0) || isnan || isinf) dt = 1e-6f;`.

Called once every `dtUpdateInterval` steps (5 by default), because it is a full
sweep over the grid and velocities do not change much in five steps.

### `predictor()` — momentum without pressure

For each `u` face:

```cpp
dudx = (u_ij > 0) ? (u_ij − u_left)*invDx : (u_right − u_ij)*invDx;   // upwind
dudy = (v_n  > 0) ? (u_ij − u_bot )*invDy : (u_top   − u_ij)*invDy;
d2udx2 = (u_right − 2*u_ij + u_left)*invDx2;                          // central
d2udy2 = (u_top   − 2*u_ij + u_bot )*invDy2;

uStar = mask * ( u_ij − dt*(u_ij*dudx + v_n*dudy) + dt*nu*(d2udx2 + d2udy2) );
```

**Convection uses upwind.** The derivative is taken on the side the flow is
coming *from*. If fluid moves right, what arrives at this face came from the
left, so ask the left neighbour. A centred difference here would be unstable:
it lets information propagate against the flow, which is physically wrong and
numerically explosive.

The price: upwind is only first-order accurate and adds artificial diffusion of
roughly `u·dx/2`. That is the answer to "why is there no wake" — on a coarse
grid that numerical viscosity can exceed your physical `nu`.

**Diffusion uses centred differences.** Friction genuinely acts both ways
equally; no upwinding needed.

`v_n` is the vertical velocity **at the u-face**, where it does not exist, so it
is the average of the four surrounding `v` faces. This is the one place
staggering makes you interpolate.

Three pieces of code:

* **The interior rows** `j = 1..ny-2`, parallelised with
  `#pragma omp parallel for`. Inside, AVX2 eight at a time, with
  `_mm256_blendv_ps` instead of branching on the sign
  (`_mm256_cmp_ps(uij, zero, _CMP_GT_OS)` yields the mask, `blendv` selects),
  then a scalar tail.
* **The edge rows** `j = 0` and `j = ny-1` — a separate `for (pass = 0..1)`
  loop. The missing neighbour is replaced by the face itself: `u_bot = u_ij`.
  That is free slip — zero gradient across the wall.
* **`v_star`** — the mirror image of all of the above, plus separate handling
  of the `i = 0` and `i = nx-1` columns.

At the very end, boundary conditions for `u_star`/`v_star`:

```cpp
uStar[idxU(0,j)]  = solid ? 0 : U0;                    // inlet
uStar[idxU(nx,j)] = solid ? 0 : uStar[idxU(nx-1,j)];   // outlet: zero gradient
vStar[idxV(i,0)]  = vStar[idxV(i,ny)] = 0;             // walls
```

Note the `mask *` at the end of every formula. Velocity on a closed face is
held at **exactly zero as an invariant**, so the stencil can be evaluated
unconditionally: the worst it ever reads is a legitimate zero. No branch, no
scalar fallback near the body.

### `solvePoisson()`

The right-hand side is one line of physics per cell:

```cpp
div = (u*[i+1,j] − u*[i,j])·invDx + (v*[i,j+1] − v*[i,j])·invDy;
rhs = div / dt;
```

Flux out the right face minus flux in the left, plus top minus bottom. That is
net mass creation. Divide by `dt`.

Then it all goes to the multigrid:

```cpp
lastResidual = multigrid.solve(p, rhs, cfg.smootherOmega, cfg.omega,
                               max(1, cfg.mgIterations), cfg.mgTolerance);
```

Note that `p` is passed by reference and **is not zeroed**. Last step's
pressure is an excellent initial guess; the pressure field changes little
between steps.

### `corrector()`

```cpp
u[i,j] = mask * ( u*[i,j] − dt·(p[i,j] − p[i−1,j])·invDx );
v[i,j] = mask * ( v*[i,j] − dt·(p[i,j] − p[i,j−1])·invDy );
```

Two adjacent pressures, subtract, scale. Notice how clean this is *because* of
the staggering.

The outlet face separately:

```cpp
const float outletFactor = 2.f * dt * invDx;
u[idxU(nx,j)] = u_star[idxU(nx,j)] + outletFactor * p[idxP(nx-1,j)];
```

The 2 is there because `p = 0` is imposed **on the face** while pressure lives
at cell centres, half a cell away. Details in the Poisson-operator section.

And `applyBC()` again at the end.

### `applyBC()`

Three lines: inlet `u = U0` (or 0 if that cell is solid), `v = 0` on the bottom
and top walls. Everything else is carried by masks and coefficients.

### Diagnostics: `maxDivergence()` and `maxVelocity()`

`maxDivergence` computes `∇·u` in every non-solid cell and takes the maximum
absolute value. This is the health indicator: if the projection is working
these should be around `1e-6` or smaller.

`maxVelocity` is the maximum absolute value over all `u` and `v`. If it turns
`nan`/`inf`, `run()` prints "Solution diverged" and exits.

### `run()`

```cpp
initFields();
outputPath = resolveOutputDir(cfg.outputDir);      // exactly once
computeDt();
saveVTK(0);

while (currentTime < cfg.totalTime) {
    if (step % dtUpdateInterval == 0) computeDt();

    float stepDt = dt;
    if (currentTime + stepDt > cfg.totalTime)
        stepDt = cfg.totalTime − currentTime;      // do not overshoot the end
    if (!(stepDt > 0)) break;

    const float savedDt = dt;
    dt = stepDt;

    predictor();
    solvePoisson();
    corrector();

    currentTime += dt;
    step++;
    dt = savedDt;                                  // put the CFL dt back
    ...
}
saveVTK(step);
```

The `savedDt` dance is for this: the last step is **shortened** to land exactly
on `totalTime`. But that shortened `dt` must not become the `dt` for the next
step, or the run would crawl in tiny steps after every truncation. So the CFL
value is saved and restored.

Every 10 steps a progress line is printed: step, time, `dt`, `|u|max`, `div`,
the relative multigrid residual and the number of V-cycles spent. Those are
exactly the six numbers that tell you what is going on.

Every `saveInterval` steps, `saveVTK`.

---
## 10. `Multigrid` — why the pressure solve is fast

This is the densest part of the project. Correctness lives here.

### 10.1 The Poisson operator: the core requirement

Three operators must agree **exactly, face by face**:

* the **divergence** that builds the right-hand side (`Solver::solvePoisson`);
* the **Laplacian** that gets solved (`Multigrid::buildCoefficients`);
* the **gradient** the corrector applies (`Solver::corrector`).

The requirement:

```
Laplacian = divergence ∘ gradient
```

Not approximately. Not "except at boundaries". If they disagree at even one
face, then at that cell `∇·uⁿ⁺¹ ≠ 0` no matter how perfectly the linear system
is solved. And the error persists into the next step. Forever.

### 10.2 How that is achieved: boundary conditions inside the coefficients

The operator for a cell:

```
(L p)ᵢⱼ = cW·p(i−1,j) + cE·p(i+1,j) + cS·p(i,j−1) + cN·p(i,j+1) − diag·pᵢⱼ
```

Each coefficient answers **one** question: *is this face something the
corrector will update?*

| Face | Is velocity there prescribed? | Coefficient |
|---|---|---|
| between two fluid cells | no, the corrector owns it | `1/dx²` or `1/dy²` |
| touching a solid cell | yes — it is 0 | **0** |
| inlet, `i=0` | yes — it is `U0` | **0** |
| walls, `j=0` and `j=ny` | yes — it is 0 | **0** |
| outlet, `i=nx` | no — free to adjust | Dirichlet, see below |

The rule is almost embarrassingly simple: **if the corrector cannot change the
velocity on a face, that face contributes nothing to the Laplacian.** A closed
coefficient and a prescribed velocity are the same statement written twice.

The code (`buildCoefficients`):

```cpp
const float coefW = (i > 0      && !solid[id-1])  ? invDx2 : 0;
const float coefE = (i < nx-1   && !solid[id+1])  ? invDx2 : 0;
const float coefS = (j > 0      && !solid[id-nx]) ? invDy2 : 0;
const float coefN = (j < ny-1   && !solid[id+nx]) ? invDy2 : 0;
float diag = coefW + coefE + coefS + coefN;
if (i == nx-1) diag += 2.0f * invDx2;             // outlet
```

### 10.3 The outlet and the factor of 2

We want `p = 0` **on the face**, but pressure lives at cell centres, half a cell
away. So use a ghost value `p_ghost = −p(nx−1,j)` — then the average of the
two, which is the face value, is exactly zero.

Its contribution to the Laplacian is `(p_ghost − p)/dx² = −2p/dx²`: nothing in
the numerator, `2/dx²` added to the diagonal. And the corrector applies the
matching gradient:

```cpp
u[nx,j] = u*[nx,j] + 2·dt·p[nx−1,j]·invDx;
```

Two payoffs:

1. With one real Dirichlet condition the matrix is **non-singular**. No pinning
   of a cell, no drift of the mean pressure.
2. The outlet velocity is **corrected by the pressure**, so the solver balances
   outflow against inflow by itself. Measured mass error: 0.00000%.

### 10.4 The identity

Substitute the corrector into the divergence and everything telescopes:

```
∇·uⁿ⁺¹ = ∇·u* − dt·(L p)
```

with exactly that `L`. Set `rhs = ∇·u*/dt`, solve `L p = rhs`, and you get
`∇·u* − dt·(∇·u*/dt) = 0`. Guaranteed by construction, not by carefulness.

### 10.5 What is actually stored

Six float arrays per level: `coefW, coefE, coefS, coefN, diag, invDiag`. Built
once in `buildCoefficients()`, because the geometry never changes during a run.
The solver loop becomes:

```cpp
num  = cW[id]*p[id−1] + cE[id]*p[id+1] + cS[id]*p[id−nx] + cN[id]*p[id+nx];
pNew = (num − rhs[id]) * invDiag[id];
p[id] += omega * (pNew − p[id]);
```

No branches. No "is this solid?" tests. No division.

Cells that should not be solved (solid, or fluid but completely walled in) get
`invDiag = 0`, so `pNew = 0` and the update does nothing. The masking handles
solids for free.

### 10.6 Why plain iteration is hopeless

Gauss-Seidel/SOR updates each cell from its four neighbours. Information moves
**one cell per sweep**. On a 256-wide grid, a pressure change at the inlet needs
256 sweeps just to *reach* the outlet, and thousands to converge. That is
`O(N²)` work per time step, and it is exactly what the old solver was doing —
hence 70 ms/step.

### 10.7 The multigrid insight

Watch what SOR actually does to the **error**. After two or three sweeps the
error is **smooth** — all the jagged, cell-to-cell wiggle is gone. What remains
is a broad, gentle shape spanning the whole domain, and SOR barely touches it,
because a smooth error looks locally like a constant and a constant is already
"solved" for a Laplacian.

So SOR is excellent at high-frequency error and useless at low-frequency error.

But: **smooth is defined relative to the grid spacing.** Put the same error on
a grid with twice the spacing and it is no longer smooth — it is now
high-frequency, and SOR eats it. Recurse.

### 10.8 The V-cycle

```
at level L:
  smooth 2×                    kill the high frequencies here
  r = rhs − L·p                what is left over
  restrict r to level L+1      it is smooth, a coarser grid can hold it
  recurse                      solve L·e = r there
  p += prolongate(e)           bring the correction back up
  smooth 2×                    clean up interpolation artifacts
```

At the coarsest level the grid is tiny, so just hammer it with 50+ sweeps until
it is solved.

Cost: each level is 4× smaller, and `1 + ¼ + ¹⁄₁₆ + … = ⁴⁄₃`, so a whole
V-cycle costs about the same as ~3 fine-grid sweeps. And each cycle cuts the
error by **about 10×, independent of grid size**. That is the whole point:
`O(N²)` becomes `O(N)`, and the iteration count stops caring how big the grid
is.

Two V-cycles per time step is usually enough — hence `mgIterations = 2`.

### 10.9 `buildHierarchy()` — two non-obvious rules

```cpp
bool canX = (curNx > minCoarseSize) && (curNx % 2 == 0);
bool canY = (curNy > minCoarseSize) && (curNy % 2 == 0);
if (canX && canY) {
    if (curDx <= 0.5f * curDy)      canY = false;
    else if (curDy <= 0.5f * curDx) canX = false;
}
if (!canX && !canY) break;
```

**Rule 1: only coarsen even cell counts.** An odd count leaves the last coarse
cell covering one fine cell instead of two, its column of `P` carries half the
weight of the others, and the coarse grid gets a residual that is half as big
as it should be. Inconsistent → divergence.

**Rule 2: semi-coarsening.** A point smoother only damps error in the direction
it is strongly coupled to. On a grid with `dx ≪ dy` the `y` coupling (`1/dy²`)
is tiny, so `y` error survives and the cycle stalls. So when the aspect ratio is
worse than 2:1, only the over-resolved axis gets coarsened, driving the coarse
grids toward isotropy.

The comparison is **not strict** (`<=`) on purpose: a ratio of exactly two is
the case the rule exists for, and `Lx=2, Ly=1` on a square cell count lands on
it every time. Letting it through as isotropic carried the anisotropy down the
whole hierarchy and cost a level and most of the convergence.

`refineX`/`refineY` are recorded on the **fine** level and say by how much it is
coarsened towards the next (1 = that axis was not coarsened).

### 10.10 Restriction and prolongation must match

Going down (**restriction**) and coming back up (**prolongation**) cannot be
designed independently. The requirement is:

```
R = Pᵀ / (fine cells per coarse cell)
```

When that holds, the coarse-grid correction is an orthogonal projection in the
energy norm — it provably **cannot make the error worse**. When it does not
hold, there is no guarantee, and the two-grid operator can have spectral radius
above 1, meaning each V-cycle *amplifies* error. This was the bug that made
100×100 explode while 128×128 worked fine.

So `P` is cell-centred bilinear: a fine cell sits a quarter of a coarse cell
off-centre, giving weights `¾` and `¼` per axis.

`transferStencil(i, refine, coarseN)`:

```cpp
s.coarse0 = i >> 1;
s.coarse1 = ((i & 1) == 0) ? (coarse0 > 0 ? coarse0−1 : coarse0)
                           : (coarse0+1 < coarseN ? coarse0+1 : coarse0);
weights = (coarse1 == coarse0) ? {1, 0} : {0.75, 0.25};
```

An even fine cell leans towards the **left** coarse neighbour, an odd one
towards the right. At an edge, where there is no neighbour, the weight collapses
to 1.0.

And `R` is computed as the **literal transpose** — implemented as a **gather**
rather than a scatter, so OpenMP needs no atomics. Look at `restrictField`: the
outer loops run over **coarse** cells and collect the fine contributions. That
is why `i0 = 2*i−1`, `i1 = 2*i+2` are there — the window of fine cells that can
carry any weight into this coarse cell.

### 10.11 `prolongWeight` — normalisation near the body

```cpp
float weight = 0;
for (k = 0..3)
    if (!coarse.solid[...]) weight += weights[k];
fine.prolongWeight[fineId] = weight;
```

For a fine cell, only the weights of coarse cells that are **fluid** are summed.
Then both prolongation and restriction divide by that:

```cpp
finePressure[fineId] += value / norm;          // prolongateCorrection
sum += (wx * wy / norm) * fineSrc[fineId];     // restrictField
```

Without it a fine cell right at the body would get a correction weighted over
three coarse cells instead of four — understated — and `R = Pᵀ` would break
exactly where the geometry is most complicated.

`prolongWeight <= 0` means "this cell takes no part", and that is the only test
in either transfer.

### 10.12 `setGeometry(solid)`

1. If there is no hierarchy — `buildHierarchy()`.
2. Copy the mask onto level zero.
3. **Coarsen the mask upwards**: a coarse cell is solid only if **all** the fine
   cells under it are solid. Not "most", not "any" — all. A partially blocked
   coarse cell stays fluid, and the body remains visible at every level.
4. `buildCoefficients` on every level.
5. `buildTransferWeights` on every level (after the coefficients — it looks at
   `diag`).
6. `geometryReady = true; firstSolve = true;`
7. If CUDA is on — `setGeometryCuda()`.

### 10.13 `smoothSOR(level, omega, sweeps)` — red/black SOR

Every neighbour of a red cell is black, so all red cells can update
simultaneously. The naive version strides by 2 — and kills SIMD.

Instead: compute the update for **all 8 lanes**, then store only the current
colour with `_mm256_maskstore_ps`. Half the arithmetic is discarded, but
**every load and store stays contiguous** — on a memory-bound kernel that is a
big net win.

```cpp
const __m256i laneEven = _mm256_setr_epi32(-1,0,-1,0,-1,0,-1,0);
const __m256i laneOdd  = _mm256_setr_epi32(0,-1,0,-1,0,-1,0,-1);
...
const int parity = color ^ (j & 1);
const __m256i lane = parity ? laneOdd : laneEven;
...
_mm256_maskstore_ps(pressure + id, lane, relaxed);
```

Since vectors always start at even `i`, the lane mask is one of exactly two
constants. `parity = color ^ (j & 1)` accounts for the chessboard being shifted
in adjacent rows.

The scalar tail strides `ii += 2` from `i + parity` — same colouring, same
arithmetic. On a build without AVX2 it handles the whole row.

`#pragma omp parallel if (ny >= PARALLEL_ROWS_MIN)` — levels under 32 rows run
serially: they are a few hundred cells visited by every cycle, and barrier
traffic there costs more than the arithmetic. One fork/join per `smooth()` call,
not per sweep.

### 10.14 `computeResidual` and the norms

```cpp
Ap = (neighbour sum) − diag*p;
residual = rhs − Ap;
```

The `−diag·p` rather than `+` is because the diagonal is stored **positive**
while the operator carries it with a minus (see the `(L p)ᵢⱼ` formula above).

`computeVectorNorm` is a sum of squares with an AVX2 accumulator and an
`omp reduction`, and it accumulates in `double`, not `float`: at 256×128 =
32768 cells a float sum of squares is already losing precision.

### 10.15 `vCycle`, `fullMultigrid` and `solve`

```cpp
void Multigrid::vCycle(int level, float smootherOmega, float coarseOmega) {
    if (level == levels − 1) {                       // the bottom
        smoothSOR(level, coarseOmega, coarseSweeps(nx, ny));
        return;
    }
    smoothSOR(level, smootherOmega, PRE_SMOOTH_SWEEPS);   // 2
    computeResidual(level);
    restrictResidual(level);
    vCycle(level + 1, smootherOmega, coarseOmega);
    prolongateCorrection(level + 1);
    smoothSOR(level, smootherOmega, POST_SMOOTH_SWEEPS);  // 2
}
```

`coarseSweeps(nx, ny) = clamp(2*max(nx,ny), 50, 400)` — at the bottom the
smoother acts as a solver and needs enough sweeps for information to cross the
whole grid.

`fullMultigrid` (nested iteration) is a different thing: it **restricts the
right-hand side itself** all the way to the bottom, solves there, and comes back
up doing `prolongateSolution` (the solution, not a correction) followed by a
full V-cycle at each level. That builds a good field out of nothing.

`solve()`:

```cpp
// solid cells have a zero diagonal, they take no part in the solve
finestPressure[id] = active ? pressure[id] : 0;
finestRhs[id]      = active ? rhs[id]      : 0;

rhsNorm = computeVectorNorm(finestRhs, cellCount);
scale = (rhsNorm > 1e-20f) ? rhsNorm : 1.0f;

if (firstSolve) { fullMultigrid(...); firstSolve = false; }

for (cycle = 0; cycle < maxCycles; ++cycle) {
    computeResidual(0);
    relative = computeResidualNorm(0) / scale;
    if (relative < tolerance) break;
    vCycle(0, smootherOmega, coarseOmega);
    ++lastCycles;
}
```

`firstSolve` means FMG runs **exactly once per run**. Later steps start from the
previous pressure field, which is far better than anything a fresh FMG pass
produces.

The residual check sits **before** the cycle: if the field is already good
enough — and on settled flow it often is — not a single V-cycle is spent, and
the progress line shows `0 cycles`.

Normalising by `‖rhs‖` is what makes `mgTolerance = 1e-4` a meaningful number
that does not depend on the scale of the problem.

---
## 11. `MultigridCuda` — the same thing on the GPU

`MultigridCuda.cu` is 682 lines, all of it wrapped in `#ifdef USE_CUDA`. If
CMake built without CUDA the file is not even in the source list
(`CMakeLists.txt`: `if(CFD_ENABLE_CUDA) list(APPEND CFD_SOURCES src/MultigridCuda.cu)`),
and the declarations inside `Multigrid.hpp` are hidden behind the same
`#ifdef`.

The algorithm is **identical**. One thread per cell.

### 11.1 What lives on the device

`struct DeviceLevel` mirrors `Level`: pointers to pressure, residual, `rhs`, the
five coefficients, `invDiag`, `solid`, `prolongWeight`.

The key decision has its own comment in the file:

> The previous version called `cudaMalloc`/`cudaFree` for every level on every
> pressure solve, i.e. tens of allocations per time step, and that dominated
> the GPU path completely.

Now: **the whole hierarchy is allocated once** in `allocateDevice()` when the
geometry is set, and released in the destructor. The stencil is uploaded once
(it is a function of geometry, and geometry is static). The pressure field
**stays resident on the GPU between time steps** — which means last step's
solution is a free warm start. Only the RHS crosses the bus each step.

### 11.2 `cudaDeviceAvailable()` — why this matters

```cpp
bool Multigrid::cudaDeviceAvailable() {
    int count = 0;
    const cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess || count < 1) {
        cudaGetLastError();     // clear the latched error
        return false;
    }
    return true;
}
```

`cudaMalloc` **aborts the process** when the toolkit is there but no device is
behind it — and that is a whole class of machines: built with CUDA, run on a
laptop with integrated graphics. Asking first costs one call and turns a dead
process into a CPU run.

`cudaGetLastError()` is not decoration: a failed query **latches** the error,
and the next unrelated CUDA call would be the one to report it.

`setUseCuda(enable)` in `Multigrid.cpp`:

```cpp
useCuda = enable && cudaDeviceAvailable();
if (enable && !useCuda)
    fprintf(stderr, "No usable CUDA device; the pressure solve runs on the CPU.\n");
```

### 11.3 The kernels

**`smoothSORKernel`** — red/black SOR. Each thread takes its `(i, j)`, returns
if `((i+j) & 1) != color`, otherwise does the exact same formula as the CPU.

The subtle point, which has its own comment:

> Consecutive launches on the same stream are ordered, so the black half sweep
> is guaranteed to observe every red update. The old code relied on the same
> property but also ran a separate `applyBC` kernel that wrote the cells its
> neighbours were reading, which is exactly where the GPU and CPU results used
> to diverge.

Now the boundary conditions live entirely in the coefficients, there is no
separate kernel, and there is nothing left to diverge.

**`computeResidualKernel`** — a line-by-line port of the CPU version.

**`restrictKernel`** — one thread per **coarse** cell, gathering the fine
contributions. Inside it is the same `transferStencil`, duplicated as a
`__device__` function (a stencil cannot be shared between a `.cpp` and a `.cu`
without moving it to a common header, so it is written twice — and that is the
place that must be edited **in lockstep**).

`addMode` in `prolongateKernel`: `1` adds a correction
(`prolongateCorrection`), `0` overwrites the solution (`prolongateSolution`).
One piece of code for both cases.

**`zeroSolidPressureKernel`** — zeroes `pressure` and `rhs` where
`invDiag == 0`. The analogue of the CPU loop at the top of `solve()`.

**`computeNormKernel`** — a classic reduction: a grid-stride loop accumulates
squares in a register, then a 256-element `__shared__` array folds in half with
`__syncthreads()`, and thread 0 writes one value per block. The host sums the
partials **in `double`** and takes the root.

`reduceBlocks` is capped at `MAX_NORM_BLOCKS = 1024` — more is not needed, the
grid-stride loop covers any size anyway.

### 11.4 The halo on the device

```cpp
d.halo = (grid.nx > 8) ? grid.nx : 8;
padded = cellCount + 2*halo + 16;
cudaMalloc(&d.pressureAlloc, padded * sizeof(float));
d.pressure = d.pressureAlloc + d.halo;
```

Exactly the same trick as on the CPU: the pointer is offset into the middle of
the allocation, so `pressure[id − nx]` and `pressure[id + nx]` are always valid
memory for the very first and very last cell. `cudaMemset` to zero guarantees
that what is read there is a zero.

Note that `pressureAlloc` is what gets freed, not `pressure` — the second
pointer is offset and `cudaFree` on it would be an error. That is why the struct
carries both.

### 11.5 `solveCuda()`

```cpp
cudaMemcpy(dFinest.rhs, rhs.data(), bytes, H2D);
cudaMemcpy(dFinest.pressure, pressure.data(), bytes, H2D);
zeroSolidPressureKernel<<<...>>>(...);
rhsNorm = computeRhsNormCuda(0);
if (firstSolve) { fullMultigridCuda(...); firstSolve = false; }
for (...) { computeResidualCuda(0); ... vCycleCuda(0, ...); }
cudaMemcpy(pressure.data(), dFinest.pressure, bytes, D2H);
```

The structure matches the CPU version one for one. Everything runs on the
default stream, so the chain `smooth → residual → restrict → recurse →
prolongate → smooth` needs no explicit synchronisation.

`CUDA_CHECK` / `CUDA_CHECK_LAUNCH` are macros that print the file, line, call
name and error text, then `std::abort()`. Harsh, but the error does not travel
ten calls downstream before surfacing.

### 11.6 The practical consequence

The CPU and GPU paths must produce **the same numbers**, up to float summation
order. That is exactly what `useCuda = 0` in `Config` is for: a way to compare
the two implementations on one machine. If they disagree, one of them has a
bug, and most often it is the duplicated `transferStencil`.

---

## 12. The speed layer: AVX2, OpenMP, halo, red/black

Everything above is the algorithm. This section is about making it run without
changing a single number.

### Branch-free hot loops

Every velocity on a closed face is held at exactly zero as an **invariant**. So
the stencil can be evaluated unconditionally — the worst it ever reads is a
legitimate zero — and the result multiplied by a float mask. No branch, no
scalar fallback near the body.

That is also why the masks are stored as float: `res * mask` is one
instruction, while `if (mask) ...` is a branch predictor that mispredicts every
other cell along the body's outline.

### Halo padding

```cpp
void init(int cellCount, int haloWidth) {
    halo = haloWidth;
    storage.assign(cellCount + 2*haloWidth + 16, 0.0f);
    base = storage.data() + haloWidth;
}
```

Each pressure array is allocated with `max(nx, 8)` extra floats at **both** ends,
with the logical pointer offset into the middle. So `p[id−1]`, `p[id+nx]` and so
on are always valid memory, even for the very first and last cell. An 8-wide
AVX load at the edge reads into the halo — which is zero, and whose coefficients
are zero anyway — instead of segfaulting.

**This is what makes the branch-free vector code safe, not just fast.** The
extra `+16` at the end is slack so the last 8-float vector load cannot run off
the buffer.

### Red/black SOR with masked stores

Covered in §10.13. The essence: throw away half the arithmetic, keep the memory
access contiguous.

### OpenMP

Rows go to different threads. Red/black colouring makes that race-free with no
locks. One fork/join per `smooth()` call, and levels under 32 rows run serially.

The important part: **an OpenMP build is bit-identical to a single-threaded
one.** No reduction in the hot path depends on thread order — the maxima in
`computeDt` come together through `omp critical`, and the sum of squares
accumulates in `double` with `reduction(+)`.

### CUDA

See §11. Same algorithm, one thread per cell, the whole hierarchy resident.

### What buys what (order of magnitude)

| Layer | What it speeds up | Rough scale |
|---|---|---|
| multigrid instead of SOR | the pressure solve | `O(N²)` → `O(N)`, tens of times at 256×128 |
| AVX2 | every per-cell loop | 2–4× on memory-bound kernels |
| OpenMP | the same | roughly the physical core count, saturating on memory |
| CUDA | the pressure solve only | depends on the card; predictor and corrector stay on the CPU |

Note that last row: **CUDA only accelerates Poisson.** The predictor, the
corrector, `computeDt` and the VTK output are always CPU. So on small grids the
GPU can lose: shipping `rhs` over and `p` back every step eats the win.

---

## 13. Output: the VTK format

`Solver::saveVTK(stepNum)` writes **binary legacy VTK** straight out, with no
temporary arrays and no per-cell copies.

### The header

```
# vtk DataFile Version 3.0
CFD-Solver-2D output, step N
BINARY
DATASET STRUCTURED_POINTS
DIMENSIONS (nx+1) (ny+1) 1
ORIGIN 0 0 0
SPACING dx dy 1
CELL_DATA nx*ny
```

`DIMENSIONS` counts **points**, hence `nx+1`, while `CELL_DATA` counts **cells**,
hence `nx*ny`. Our data is per-cell (pressure at centres), which is why the two
differ.

### The three data blocks

1. `SCALARS pressure float 1` — `p[i] * cfg.ro`, i.e. pascals;
2. `SCALARS solid int 1` — the body mask;
3. `VECTORS velocity float` — velocity **interpolated to the cell centre**:

```cpp
uu = 0.5f * (u[rowU+i] + u[rowU+i+1]);
vv = 0.5f * (v[rowV+i] + v[rowVTop+i]);
writeFloat(uu); writeFloat(vv); writeFloat(0.0f);
```

The half-sum of two faces is where the staggered grid collapses into something
ParaView understands.

### Byte swapping

Legacy VTK binary data is **big endian**, and x86 and ARM normally are not. So
every 32-bit word is reversed:

```cpp
buffer[bufferPos++] = ((x & 0x000000FF) << 24) | ((x & 0x0000FF00) << 8)
                    | ((x & 0x00FF0000) >> 8 ) | ((x & 0xFF000000) >> 24);
```

Words accumulate in a `std::array<uint32_t, 4096>` on the **stack** (16 KB) and
are flushed in batches. No `new`, no `vector` per frame.

`writeFloat` moves a `float` into a `uint32_t` through `memcpy` — not through a
`reinterpret_cast`, because that is the only way not to break strict aliasing.

### The printing

```cpp
if (stepNum % (max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
    std::cout << "Saved " << filename << std::endl;
```

The "Saved" line is printed for **every tenth** saved frame, or the console
turns into a waterfall. `run()` says so in its own line at the start.

---
## 14. `CMakeLists.txt` line by line

The file does three things: parses four switches, catches missing toolkits
gracefully, and links statically in the right way on three platforms.

### The options

```cmake
option(CFD_ENABLE_OPENMP "..." ON)
option(CFD_ENABLE_AVX2   "..." ON)
option(CFD_ENABLE_CUDA   "..." ON)
option(CFD_ENABLE_CUDA_EXPLICIT   "Treat a missing CUDA toolkit as a hard error" OFF)
option(CFD_ENABLE_OPENMP_EXPLICIT "Treat a missing OpenMP runtime as a hard error" OFF)
option(CFD_STATIC "Link the runtime into the executable" ON)
option(CFD_ENABLE_ICON "Embed the icon and version block on Windows" ON)
```

The `*_EXPLICIT` pair is the most important thing here for releases and the
least obvious on first reading.

By default a missing CUDA toolkit is **not** an error: CMake warns and builds
the CPU version. For a developer that is right. For a release script it is a
disaster: the `avx2-omp-cuda` row would quietly build without CUDA and ship
under a name promising a feature it does not have. So `make-release.*` passes
`-DCFD_ENABLE_CUDA_EXPLICIT=ON` for CUDA rows and
`-DCFD_ENABLE_OPENMP_EXPLICIT=ON` for OpenMP rows — then a missing toolkit fails
the configure, the row is recorded as failed, and it never reaches the release.

`CFD_OPENMP_ROOT` is the path to libomp on macOS. Empty means "let
`find_package` look", and on macOS it finds nothing and produces an OpenMP-less
binary under an OpenMP name. Which is why the release script never leaves it
empty for an OpenMP row.

### `CFD_STATIC` — what it costs on each platform

From the comment in the file:

* **MSVC** — static CRT (`/MT`) via `CMAKE_MSVC_RUNTIME_LIBRARY`. The OpenMP
  runtime has no static form at all, so an OpenMP build still needs
  `vcomp140.dll` beside it. That is exactly why `make-release.ps1` hunts for it
  and copies it (`Find-Vcomp`).
* **GCC/Clang** — `-static`, plus a separate trick for `libgomp.a`:

```cmake
execute_process(COMMAND ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_FLAGS} -print-file-name=libgomp.a
                OUTPUT_VARIABLE CFD_LIBGOMP_A ...)
if(EXISTS "${CFD_LIBGOMP_A}")
    set(OpenMP_CXX_FLAGS "-fopenmp")
    set(OpenMP_CXX_LIB_NAMES "gomp")
    set(OpenMP_gomp_LIBRARY "${CFD_LIBGOMP_A}")
endif()
```

CMake's `OpenMP::OpenMP_CXX` target records the path of the **shared** libgomp,
and `-static` then dies with "attempted static link of dynamic object". Point
the search at the archive **before** `find_package` runs and the imported target
comes out static without anything downstream noticing.

* **with CUDA** — no full static: `libcudart_static` wants `dlopen` and
  `pthread`, and the driver is dynamic by definition. What remains is
  `-static-libgcc -static-libstdc++`, which leaves `libc`, `libm` and
  `libcuda.so.1` external.
* **macOS** — there is no static `libSystem`, so this only means "no
  third-party dylibs". That is as static as an Apple binary is allowed to be.
  Hence `-static-libstdc++`.

### CUDA architectures

```cmake
set(CFD_CUDA_ARCHITECTURES "50;60;61;70;75;80;86;89;90" ...)
```

CMake emits SASS for each of these plus PTX for the last, so newer cards JIT and
work. Without the older entries nothing before Turing runs at all.

The trap: **CUDA 13 dropped Maxwell, Pascal and Volta**, and `nvcc` errors out
rather than warning when asked for one of them. So the release scripts detect
the toolkit version and trim the list:

```powershell
if ([int]$Matches[1] -ge 13) { return "75;80;86;89;90" }
else { return "50;60;61;70;75;80;86;89;90" }
```

There is also a hard check:

```cmake
if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950 AND CUDAToolkit_VERSION VERSION_LESS 13.2)
    message(FATAL_ERROR "Visual Studio 2026 requires CUDA Toolkit 13.2 or newer.")
```

### The Windows icon resource

```cmake
file(COPY "${CFD_ICON_SOURCE}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
set(CFD_ICON_PATH "fluid-solver.ico")
configure_file("src/app.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
```

The comment explains what was actually tripped over here: `rc.exe` reads the
`.rc` file in the system ANSI code page, **not** UTF-8. And this project lives
under a path with Cyrillic in it. An absolute path arrives at the resource
compiler as mojibake and the icon "does not exist". So the icon is copied next
to the generated `.rc` and referenced by a bare name, which `rc.exe` resolves
relative to the `.rc` itself. Nothing in the `.rc` may be non-ASCII.

### Target name versus file name

```cmake
set_target_properties(cfd_app PROPERTIES OUTPUT_NAME "${CFD_APP_NAME}")
```

The target keeps its old name so the GUI project, which looks for `cfd_app`,
still resolves. Only the file on disk is renamed — "cfd_app" says nothing about
which solver it is, and there will be others next to it.

### The AVX2 flags

```cmake
if(MSVC)   /arch:AVX2
elseif(GNU|Clang)   -mavx2 -mfma
```

The comment matters: `/arch:AVX2` already implies FMA on MSVC; `-mavx2` alone
does not on GCC/Clang. Without `-mfma` the predictor never contracts into FMA
instructions and the vector code is slower than it should be.

---

## 15. The build matrix: why thirty rows

```
windows-x64    AVX2 {on,off} × OpenMP {on,off} × CUDA {on,off} = 8
windows-x86    AVX2 {on,off} × OpenMP {on,off}                 = 4
linux-x64      AVX2 {on,off} × OpenMP {on,off} × CUDA {on,off} = 8
linux-x86      AVX2 {on,off} × OpenMP {on,off}                 = 4
macos-arm64    OpenMP {on,off}                                 = 2
macos-x64      AVX2 {on,off} × OpenMP {on,off}                 = 4
                                                             ----
                                                               30
```

Why exactly that:

* **There is no 32-bit CUDA**, and has not been since CUDA 9. So the `*-x86`
  rows have no CUDA axis at all.
* **There is no CUDA on macOS at all.** Apple and NVIDIA parted ways.
* **arm64 has no AVX2** — that is an x86 extension. Apple Silicon has NEON, but
  there is no NEON code in this project, so only the OpenMP axis remains there.
* **`plain`** — no AVX2, no OpenMP, no CUDA — is the row that runs on anything,
  the only one with no `vcomp140.dll` beside it on Windows, and the only fully
  static one on Linux.

And the reason this is a matrix at all rather than flags:

> A binary cannot decide at run time whether it is allowed to execute an AVX2
> instruction. It just dies.

You can check `CPUID`, but the code is already compiled with `-mavx2` and the
compiler is free to put AVX2 anywhere, including the prologue of `main`.
Splitting into two builds is the only honest way. And once one axis is split,
splitting the other two is easier, because an installer can only offer what is
in its payload.

Each Mac builds **both** architectures (the second one as a cross build), but in
CI each machine **keeps only what it produced natively** and the artifacts are
merged at the end. That way no libomp for a foreign architecture ever has to be
dragged in.

---

## 16. The build and release scripts

### `scripts/build-linux.sh` and `scripts/build-windows.ps1`

Simple: run every combination for their platform and drop the results. This is
what you run when you just want to rebuild everything locally.

### `scripts/make-release.sh` — Linux and macOS

One command: check the toolchain, install what is missing, build everything this
machine can, assemble `release/<version>/`.

```
bash scripts/make-release.sh 0.1
bash scripts/make-release.sh 0.1 --docker --with-installers
bash scripts/make-release.sh 0.1 --only=package
```

Flags: `--docker`, `--with-installers`, `--no-deps`, `--skip-32`, `--skip-cuda`,
`--only=all|build|installers|package`, `--cuda-archs=`, `--docker-image=`.

**Toolchain checks** (`ensure_tools_linux` / `ensure_tools_macos`):

* compiler, `cmake ≥ 3.28`, `zip/file/binutils`;
* 32-bit multilib — probed by actually compiling a `-m32` test file
  (`probe_m32`), not by asking the package manager;
* `nvcc` — `probe_nvcc` also looks in `/usr/local/cuda/bin` and `/opt/cuda/bin`,
  because installers put it there and leave `PATH` alone;
* `makeself` — only when installers were asked for.

**`ensure_cmake`** deserves its own paragraph. CMake 3.28 is what the project
asks for and what distributions older than the binaries do not have. The script
installs a newer one **into a venv inside the repository**
(`.toolchain/<os>-<arch>/venv`) rather than on top of the system copy. And it
checks the version it prints, not whether the file exists:

> Being executable is not the same as working: a venv left behind by another
> machine still has a cmake in it, and its shebang points at an interpreter
> that is not on this one.

The toolchain directory is one **per system**, not per repository, and the
distribution and its version are part of the name — because the host and the
container are both `linux-x86_64` and would otherwise collide.

**`prepare_macos_openmp`** is how one Mac builds both architectures. The host
libomp is installed normally through Homebrew; the libomp for the **other**
architecture is fetched as a bottle (`brew fetch --bottle-tag=...`) and unpacked
into a scratch directory. That is all a cross build needs, and it avoids a
second Homebrew under Rosetta. The static archive is preferred so the result
does not depend on Homebrew still being on the machine that runs it.

Note that `OMP_SCRATCH="$(mktemp -d)"` is created **in** `prepare_macos_openmp`
and not in `fetch_libomp_bottle`: that one runs in a command substitution, so
anything it assigns is lost with the subshell and the trap would never clean it
up.

**`build_row`** is one row of the matrix. It composes the feature name from the
flags (`avx2-omp-cuda` … `plain`), wipes the build directory, configures,
builds, finds the executable, copies it to `dist/<name>`, strips it and prints
the size and the link type. The full log goes to `logs/<label>-<tags>.log`.

**`package()`** turns `dist/` into `release/<version>/`. For each entry it makes
a temporary folder `<name>/` holding the binary (renamed to `Fluid Solver`),
`README.md`, `LICENSE` and an empty `output/`, and zips it. Finished `.run`,
`.pkg` and `*setup.exe` files are simply copied. Then
`Fluid-Solver-Source-Code.zip` is built and `SHA256SUMS.txt` computed.

A small thing with a large point: the checksum file is written **outside and
moved in**. Created in place, it already exists when the glob runs and ends up
listing a checksum of itself, taken halfway through being written.

**`run_in_docker`** — why it exists at all:

> A statically linked glibc binary still records the kernel it was built
> against, and the CUDA rows cannot be static at all, so rows built on a current
> distribution refuse to start on an older one.

One old image with the CUDA toolkit already in it covers every Linux row at
once. After the container, the script **checks that rows actually appeared** —
the script inside the container reports failures but still exits 0, so an empty
`dist` is the only signal that everything died.

### `scripts/make-release.ps1` — Windows

The same idea. The substantive differences:

* `Resolve-CudaArchs` — toolkit version from `nvcc --version`;
* `Find-Vcomp` — hunts `vcomp140.dll` in the Visual Studio redistributables,
  because MSVC has no static OpenMP runtime and the build does not start without
  that DLL;
* `Find-Iscc` — locates the Inno Setup compiler;
* `Show-Tail` — prints the log lines that actually say what broke
  (`error|fatal|LNK\d|unresolved|Unsupported`), not the last 12 lines whatever
  they are:

  > Hiding a compiler error behind a `-Verbose` nobody passes is not a summary,
  > it is a dead end.

* `Package` filters on **both shapes**: a Windows row is a directory because of
  the DLL beside the exe, while a Linux or macOS row dropped in from the other
  script is a single file. Filtering on `-Directory` is why those used to vanish
  from the release without a word.
* `_to_delete`, `.toolchain`, `lib\sfml`, `logs` and `build*` are excluded from
  the source archive. `.toolchain` because it holds a Linux venv whose symlinks
  are unreadable from Windows, and `Compress-Archive` stops the whole run on the
  first one.

### `scripts/unpack-release.py`

The inverse of `package()`: it turns an already published release back into a
`dist/` the installer builders can use, without compiling anything.

```
python3 scripts/unpack-release.py 0.1 release/0.1 dist
python3 scripts/unpack-release.py 0.1 downloaded dist --platform windows
python3 scripts/unpack-release.py 0.1 downloaded dist --ui-from avx2-omp
```

The reason it exists is that the three builders want two different shapes out of
the same zips:

| Platform | Shape in `dist/` | Why |
|---|---|---|
| windows | a folder | `vcomp140.dll` sits beside the `.exe` |
| linux | a single file `Fluid Solver <ver> linux-<arch> <feature>` | that is the name `install.sh` scans for |
| macos | the same single file | `build-pkg.sh` indexes by that exact name |

It is Python rather than shell because it runs identically on all four runner
operating systems, and because every name in this release has spaces in it.

Archives whose name ends in `-ui` are the desktop UI. They are unpacked as
folders under their own name, and `--ui-from <feature>` additionally copies one
of them to `ui-<platform>-<arch>`, which is where the installers look.

---

## 17. GitHub Actions

There are two workflows, and they are deliberately separate.

### `build-all.yml` — "Build every binary"

Triggered by hand (`workflow_dispatch` with a `version` field) or by pushing a
`v*` tag.

```
version ──┬── linux        (container nvidia/cuda:12.6.3-devel-ubuntu20.04)
          ├── windows      (windows-2022 + Jimver/cuda-toolkit)
          ├── macos-arm64  (macos-latest)
          └── macos-intel  (macos-15-intel)
                              │
                              v
                           package  (ubuntu-latest)
```

Each machine runs `make-release --only=build` and uploads its `dist/` as an
artifact. Both Macs then **delete what they cross-built** — each keeps only its
native rows. Then `package` downloads every `dist-*` with
`merge-multiple: true`, runs `--only=package`, and, if this was a tag, uploads
the files to the release with `gh` (already on the runner, so no third-party
action is needed).

Linux builds **inside an old Ubuntu 20.04 container** that already has `nvcc`.
Same reason as `--docker`: so the binaries start on more than the distribution
that built them. The first step installs `git` into the container and runs
`git config --global --add safe.directory '*'`, or `checkout` complains about
the directory's owner.

This workflow **does not build installers** — the first lines of the file say so.

### `build-installers.yml` — "Build the installers"

The second workflow, and the one that closes that gap. It compiles nothing: it
takes a release that already exists, unpacks the zips back into `dist/`, and
hands them to the three installer builders.

```
fetch ──┬── windows   (windows-2022, choco install innosetup, iscc ×2)
        ├── linux     (ubuntu-latest, makeself, make-release.sh --only=installers)
        └── macos     (macos-latest, build-pkg.sh ×2)
                          │
                          v
                       collect  (SHA256, optional upload to the release)
```

Inputs:

| Input | Meaning |
|---|---|
| `version` | which release the installers are built from |
| `run_id` | optional: take the zips from a run of "Build every binary" instead of from the tag's release |
| `ui_variant` | optional: which `<feature>-ui` archive to install as the desktop UI |
| `attach` | upload the finished installers to the tag's release |

Three details worth knowing:

* **The download happens once**, in the `fetch` job, and is passed on as an
  artifact. Four jobs downloading four times with four sets of credentials is
  the thing this avoids.
* **One macOS runner builds both `.pkg` files.** `pkgbuild` and `productbuild`
  only repackage binaries that are already built, so the host architecture does
  not come into it. Windows and Linux do need their own runners: `iscc` is a
  Windows program and `makeself` wants a Linux shell.
* **The Windows step is inline pwsh, not `make-release.ps1 -Only Installers`,**
  on purpose. That script collects failures into a summary and still exits 0,
  which is exactly what a CI job must not do. Here a failed `iscc` throws.

---

## 18. The installers

Three platforms, three entirely different technologies, but they ask **the same
questions**:

1. Which features to enable — but only from what can **in principle** be
   installed.
2. Whether to install the desktop UI.
3. Where to put shortcuts: the menu / Start / Launchpad, the desktop, the
   taskbar / Dock.

### 18.1 The "in principle installable" rule

A switch is shown **only when the payload holds builds on both sides of it**.
Otherwise there is nothing to choose between, and the question is a lie.

Examples:

* the 32-bit installer: not one CUDA build exists → no CUDA box at all;
* macOS arm64: no AVX2 builds → no AVX2 box;
* a build machine without `nvcc` failed every CUDA row → the resulting installer
  again has no CUDA box, even though the platform supports it.

And symmetrically: if the payload held **only** AVX2 rows, the AVX2 box should
not be shown either — the axis is pinned to the one value that exists. The
Windows script used to check one side only, so unticking AVX2 could name a build
that was not in the file; both sides are checked now.

### 18.2 Windows — `installer/windows/fluid-solver.iss`

Inno Setup. Built like this:

```
iscc /DAppVersion=0.1 /DArch=x64 /DDistDir=..\..\dist installer\windows\fluid-solver.iss
```

**The preprocessor** works out what is there:

```
#define HaveAnyAvx2 (HaveVariant('avx2-omp-cuda') || ... )   // exists with AVX2
#define HaveNoAvx2  (HaveVariant('omp-cuda') || ... )        // exists without AVX2
#define ShowAvx2    (HaveAnyAvx2 && HaveNoAvx2)              // show the box
#if HaveAnyAvx2
  #define PinAvx2Value "True"                                // if not shown
#else
  #define PinAvx2Value "False"
#endif
```

The same for OpenMP and CUDA. `#if !HaveAny` → `#error`, so building an
installer out of an empty `dist/` stops at compile time instead of producing an
empty one.

**The `[Code]` section.** The key functions:

| Function | What it does |
|---|---|
| `HasAvx2` | `IsProcessorFeaturePresent(PF_AVX2)` — 40 |
| `HasNvidia` | does `{sys}\nvcuda.dll` exist (the display driver puts it there) |
| `Checked(i)` | the value of box `i`, `False` if that box does not exist |
| `AxisValue(i, pinned)` | the box if there is one, otherwise the pinned value |
| `ComposeFeature(a,o,c)` | three booleans → `avx2-omp-cuda` … `plain` |
| `SelectedFeature` | `ComposeFeature` of the three `AxisValue`s |
| `WantVariant(name)` | the `Check:` condition on every `[Files]` line |

`NextButtonClick` on the selection page tests three things: AVX2 ticked on a CPU
without AVX2 (a hard error — otherwise "illegal instruction" on the first run);
whether that build exists in `Available` at all; and, if CUDA is ticked without
a driver, a warning that can be dismissed (such a build does start and runs on
the CPU, the user just ticked the box for a different reason).

`ShouldSkipPage` hides the page entirely when no boxes are left.

**Shortcuts** — three tasks in `[Tasks]`:

```
Name: "startmenu";   Description: "Create a Start Menu shortcut"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"
Name: "taskbar";     Description: "Pin to the taskbar"
```

The first two are declarative: `[Icons]` creates `.lnk` files in `{group}` and
`{autodesktop}` with `Tasks: startmenu` / `Tasks: desktopicon`.

The third cannot be. **Windows has no supported API for pinning to the
taskbar.** Up to Windows 10 the shell exposed a "Pin to taskbar" verb on the
file, and driving that verb is what every installer that manages it does.
Windows 11 removed the verb.

So:

```pascal
ShellString(5386)          // the localised verb name, out of shell32.dll
  → ShellVerb(file, ...)   // walk Item.Verbs, match, DoIt
    → PinToTaskbar(file)
```

Resource 5386 in `shell32.dll` is "Pin to taskbar" in the system's language,
5387 is "Unpin from taskbar". Reading it is what lets the verb be found on a
Windows that is not in English. If the resource cannot be read there is a
fallback comparison against the substring `pin to taskbar`, and the whole
lookup is inside a `try/except` so nothing there can produce an error dialog.

What gets pinned is decided by `TaskbarTarget`: **the UI if it is installed,
otherwise the console solver.** The UI is the windowed program; a taskbar button
is for it.

If pinning fails — that is, on Windows 11 — a message says that this is blocked
system-wide for every program, that you should open Start, right-click and
choose "Pin to taskbar", and that `Fluid Solver.exe` was installed beside it as
the console version where parameters are typed in at the prompt. The note about
the console version is printed on success too.

`CurUninstallStepChanged(usUninstall)` unpins **before** the files are deleted —
afterwards there is nothing left for the shell to unpin.

One more thing that is easy to miss: a line whose first non-blank character is
`#` is a preprocessor directive to ISPP, and `#13` is not one of the directives
it knows. So a wrapped Pascal string constant must put the `+` at the start of
the continuation line rather than at the end of the previous one. There is a
comment saying so at the one place it matters.

The rest: `PrivilegesRequired=lowest` (because the solver writes `output` beside
itself and a standard user cannot write in `Program Files` — and `AppPaths`
covers the case where somebody installs there anyway), `[Dirs]` with
`Flags: uninsneveruninstall` on `{app}\output` so an uninstall does not take the
user's results with it, an optional `PATH` entry and an optional `.vtk`
association with the UI.

### 18.3 Linux — `installer/linux/install.sh`

This is the payload script of a self-extracting archive (`makeself`). It runs
from inside the unpacked archive, so every variant file is right there in the
working directory.

```
./Fluid-Solver-0.1-linux-x64.run                       # interactive
./Fluid-Solver-0.1-linux-x64.run -- --avx2 --openmp \
    --prefix=/opt/fluid-solver --menu --desktop --taskbar --yes
```

**What the machine can do** is read directly:

```bash
HAS_AVX2=0;  grep -qm1 '\bavx2\b' /proc/cpuinfo && HAS_AVX2=1
HAS_NVIDIA=0; command -v nvidia-smi || [ -e /dev/nvidia0 ] || [ -e /proc/driver/nvidia/version ]
CORES=$(nproc)
```

**What the installer carries** is read by trying all eight names:

```bash
for a in 1 0; do for o in 1 0; do for c in 1 0; do
    f="$(feature_of "$a" "$o" "$c")"
    [ -f "$APP $VERSION linux-$ARCH $f" ] || continue
    COMBOS+=("$a$o$c"); FEATURES+=("$f")
done; done; done
```

Then `varies(pos)` answers whether both 0 and 1 occur on that axis. If not,
`only_value(pos)` pins it. The question is asked **only** for axes that vary.
Exactly the same rule as the Windows script.

Defaults follow the machine: AVX2 = does this CPU have it, OpenMP = more than
one core, CUDA = is there a driver. Command-line arguments win.

Then the hard check: AVX2 on a CPU without AVX2 exits with an error. CUDA with
no driver warns but does not refuse.

**Install location:** root installs to `/opt/fluid-solver`, anyone else lands in
`~/.local/share/fluid-solver`. That is what keeps the "output beside the
executable" rule working. Symlinks go into `/usr/local/bin` or `~/.local/bin`,
icons into `hicolor`.

**Three shortcut questions:**

```bash
[ -z "$WANT_MENU" ]    && WANT_MENU=$(ask "Create an application menu entry?" 1)
[ -z "$WANT_DESKTOP" ] && WANT_DESKTOP=$(ask "Put a shortcut on the desktop?" 1)
[ -z "$WANT_TASKBAR" ] && WANT_TASKBAR=$(ask "Add it to the taskbar (dock favourites)?" 0)
```

The `.desktop` text is written by a single `write_entry <path> <solver|ui>`
function, because it is needed in two places: the menu directory and the desktop
itself. The solver gets `Terminal=true`, the UI `Terminal=false` — the UI is a
window and does not want a console.

The menu entry is written if **either** the menu or the taskbar was asked for:
dock favourites reference a `.desktop` id, and without the file there is nothing
to reference.

**The desktop** path comes from `xdg-user-dir DESKTOP` (on a localised system
that may be "Рабочий стол" rather than "Desktop"), falling back to
`$HOME/Desktop`. The file is made executable and marked trusted:

```bash
as_target_user gio set "$DESKTOP_SHORTCUT" metadata::trusted true
```

Without that GNOME shows it as an untrusted text file and refuses to launch it
until the user right-clicks "Allow launching".

**The taskbar** has no cross-desktop answer. GNOME keeps its dock in a gsettings
list of `.desktop` ids, which is scriptable and stable:

```bash
current="$(as_target_user gsettings get org.gnome.shell favorite-apps)"
as_target_user gsettings set org.gnome.shell favorite-apps "${current%]}, '$PRIMARY_ENTRY']"
```

Plasma keeps its task manager's launchers inside one
`plasma-org.kde.plasma.desktop-appletsrc` whose layout changes between releases,
so the script touches nothing there and instead prints that a right-click and
"Add to Favourites" does the same thing in one click.

**Under sudo** a desktop file and a dock favourite belong to a person, and root
has no session, so:

```bash
TARGET_USER="${SUDO_USER:-$(id -un)}"
as_target_user() { sudo -u "$TARGET_USER" DBUS_SESSION_BUS_ADDRESS=... "$@"; }
```

**What gets pinned** follows the same rule: the UI if installed, otherwise the
solver (`PRIMARY_ENTRY` / `PRIMARY_NAME`).

`uninstall.sh` is generated in place and knows about everything that was
created: the symlinks, the menu `.desktop`, the desktop shortcut, the dock
favourite, the icons, the binaries and `models`. It leaves `output` alone —
those are the user's results, not ours.

### 18.4 macOS — `installer/macos/build-pkg.sh`

This file is not an installer, it is a **builder** of one. It has to run on a
Mac: `pkgbuild` and `productbuild` are Apple tools with no Linux equivalent.

```
./installer/macos/build-pkg.sh 0.1 arm64
./installer/macos/build-pkg.sh 0.1 x64
```

A Distribution XML is what gives real checkbox selection; a `.dmg` drag-install
cannot do that.

**One payload package per variant, all of them hidden.** Each carries a
`selected` expression that reads the visible tick boxes, so exactly one is ever
installed:

```bash
variant_condition() {
    case "$f" in *avx2*) e="choices['choice-avx2'].selected" ;;
                 *)      e="!choices['choice-avx2'].selected" ;; esac
    ...
}
```

The boxes appear by the same rule: `SHOW_AVX2=1` only if the found variants
include both some with `avx2` and some without. On arm64 there are no AVX2
builds at all, so there is no box. CUDA does not exist on macOS, so that axis is
not there in principle.

Each variant gets a `postinstall` that hands the `output` folder to whoever is
actually logged in: a system-wide install would leave it owned by root, and the
solver writes its frames there.

```sh
who="$(stat -f%Su /dev/console)"
chown -R "$who" "$target"
```

**Three shortcuts**, as three payload-less packages (`pkgbuild --nopayload`)
that only run their `postinstall`. A shared preamble (`common.sh`) is pasted in
front of each.

* **Launchpad.** The payload is two Unix executables, and neither Launchpad nor
  the Dock nor Finder will treat one of those as an application. `make_app`
  wraps them in a minimal `.app`: `Contents/Info.plist` plus
  `Contents/MacOS/launcher`. The UI's launcher just `exec`s the binary; the
  solver's hands it to Terminal:

  ```sh
  open -a Terminal "$base/Fluid Solver"
  ```

  That is the macOS equivalent of a Start-menu entry: the app shows up in
  Launchpad and in the Applications folder. Untick it and the binaries are still
  installed, just only startable from a terminal.

* **Desktop.** A symlink to the `.app` in the logged-in user's `~/Desktop`.

* **Dock.** `defaults write com.apple.dock persistent-apps -array-add` with a
  `<dict>` entry pointing at the `.app`, then `killall Dock` — the Dock restarts
  once, and that is what makes the icon appear. Both commands run **as the
  logged-in user** through `launchctl asuser <uid> sudo -u <who>`: the Dock is a
  per-user thing and root has no Dock.

All three come **last** in `choices-outline`, so they run after the binaries
they point at have landed.

**Gatekeeper.** The script prints the truth at the end: without signing and
notarisation the package will not open on any machine but the one that built it.
With an Apple Developer account:

```
productsign --sign "Developer ID Installer: <name> (<team>)" in.pkg out.pkg
xcrun notarytool submit out.pkg --apple-id <id> --team-id <team> --password <pw> --wait
xcrun stapler staple out.pkg
```

Without one, users have to right-click and choose Open and confirm a warning.
Better said in the release notes than discovered.

---

## 19. Cheat sheet: symptom to file

| What you see | Where to look |
|---|---|
| "illegal instruction" on the first run | an AVX2 build on a CPU without AVX2. Take `plain` or `omp` |
| Run diverged, `\|u\|max = nan` | `computeDt` (is CFL too big?); `Solver::run` prints and exits |
| `div` never falls below `1e-3` | `Multigrid::buildCoefficients` — the operator disagrees with the corrector |
| "few multigrid levels" at start-up | `nx`/`ny` are not divisible by 2 enough times. `256×128`, not `100×100` |
| V-cycles amplify the error | `transferStencil` / `prolongWeight` — `R ≠ Pᵀ` |
| No wake behind the body | first-order upwind's numerical viscosity exceeds `nu`. Finer grid, or higher `Re` |
| The model did not load | `Mesh::printInfo` — 0 triangles means the file was not read; 0 section points means the plane missed |
| The geometry got clipped | `buildSection` keeps **one** loop, the largest by area |
| Frames are not where you expected | `AppPaths::resolveOutputDir` prints the path it settled on |
| No frames at all | the directory is not writable; there is a message on `stderr` |
| A CUDA build dies at start-up | it should not — `cudaDeviceAvailable()` catches that. If it does, look at `allocateDevice` |
| CUDA and CPU give different numbers | the duplicated `transferStencil` in the `.cu` has drifted from the `.cpp` |
| A release row did not build | `logs/<label>-<feature>.log`; the tail is printed to the screen |
| `vcomp140.dll not found` | `Find-Vcomp` in `make-release.ps1` — needs the Visual Studio C++ workload |
| The icon is not embedded in the exe | Cyrillic in the path; see the `.ico`-copy trick in `CMakeLists.txt` |
| The installer shows no CUDA box | there are no CUDA rows in `dist/`. Not a bug, that is the rule |
| The installer shows no AVX2 box | `dist/` holds only AVX2 rows **or** only non-AVX2 rows |
| "Pin to taskbar" did nothing | Windows 11 removed the verb. The message explains what to do |
| Nothing appeared in the Linux dock | not GNOME. Right-click → "Add to Favourites" |
| The macOS shortcut will not launch | Gatekeeper. Right-click → Open |
| `iscc` fails on a `#13#10` line | a line starting with `#` is an ISPP directive; lead with the `+` instead |

---

## What to read first if you come back in six months

1. `Solver::run()` — the whole loop in 40 lines.
2. `Multigrid::buildCoefficients()` — where all the correctness lives.
3. `Multigrid::buildHierarchy()` — the two rules that break everything if
   violated.
4. Everything else is either input/output or a speed-up of what those three do.
