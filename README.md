(lets just forget that we pushed 12 commits just for things to build normally and files to look at least alright 'cause we're stooooopid as shi)
(from Kuzya: i dunno how it works, i have a feeling it's something alive, and changes by itself <3. TOTALLY NOT ME COMMITING 12 TIMES)
# CFD-Solver-2D

**A 2D incompressible Navier‑Stokes solver for external flows around arbitrary profiles.**

CFD‑Solver‑2D is an educational/research project that implements a finite‑difference CFD solver for unsteady viscous incompressible flow. It uses the **Chorin projection method** on a **staggered MAC grid** with an **immersed boundary** technique to handle complex geometries. The code is written in C++17 and features:
- Interactive console parameter input with confirmation and on-the-fly editing.
- Full numerical solver with VTK output for post-processing in ParaView.
- STL/OBJ loading, central plane section extraction, geometry masking, profile rotation, mirroring, and robust contour reconstruction.
- Optional gravity as a uniform body force, pointing in any direction.
- Optional wall behaviour: every body in the mask is found and numbered on its own, and each one can spin, drag its surface, or be made frictionless, independently of the rest.

The project is designed to simulate external incompressible flow around arbitrary 2D profiles such as cylinders, airfoils, valves, turbine blades, and similar engineering geometries.

---

# Future Work

Possible future extensions include:

- Adaptive mesh refinement (AMR)
- Turbulence models
- Compressible flow solver
- Cavity flow
- Moving objects (NO idea how to do it for now, but ill figure that out)
- Flow start coordinates and width of the flow
- Multiphase(multiple liquids/gases)(painting with them too and making profile OPTIONAL)
- Surface tension
- MAY add several other solvers(deforming solver + electronic solver + thermal solver) and merge all of them into one
- MAY make a full on website where u would download it all, but only if the previous point is done

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
- ✅ Optional gravity / uniform body force, direction free
- ✅ Moving walls: rotation and sliding, set per object
- ✅ Free-slip walls, set per object
- ✅ Restarting sim from a given save

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
- ✅ Automatic detection and numbering of separate bodies

---

## Output

- ✅ VTK export
- ✅ Physical pressure (Pa)
- ✅ Velocity vectors
- ✅ Solid mask
- ✅ ParaView compatible
- ✅ Full run state embedded in every frame
- ✅ Continue a stopped simulation from any frame
- ✅ Compact frames, about 19 bytes a cell, with nothing in them stored twice

---

## Mathematical Model (brief)

We solve the 2D incompressible Navier–Stokes equations (kinematic pressure, ρ = 1):

**Momentum (X):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20t}+u\frac{\partial%20u}{\partial%20x}+v\frac{\partial%20u}{\partial%20y}=-\frac{\partial%20p}{\partial%20x}+\nu\nabla^{2}u+g_{x})

**Momentum (Y):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20v}{\partial%20t}+u\frac{\partial%20v}{\partial%20x}+v\frac{\partial%20v}{\partial%20y}=-\frac{\partial%20p}{\partial%20y}+\nu\nabla^{2}v+g_{y})

where the body force **g** is zero unless gravity is enabled — and even then the solver never discretizes it, because at constant density it is exactly a pressure offset. See *Gravity* under §4.

**Continuity (incompressibility):**

![](https://latex.codecogs.com/svg.image?\frac{\partial%20u}{\partial%20x}+\frac{\partial%20v}{\partial%20y}=0)

The **Chorin projection** splits each time step into:

1. **Predictor** – compute intermediate velocities \(u^*, v^*\) without pressure.
2. **Poisson equation** – solve

![](https://latex.codecogs.com/svg.image?\nabla^{2}p=\frac{1}{\Delta%20t}\left(\frac{\partial%20u^{*}}{\partial%20x}+\frac{\partial%20v^{*}}{\partial%20y}\right))

using a multigrid V-cycle with red/black SOR as its smoother.

3. **Corrector** – update velocities with the pressure gradient.

Boundary conditions: no-slip on solid walls, constant velocity at inlet, zero-gradient at outlet, free-slip at top/bottom. No-slip means "the fluid matches the wall", and the wall is allowed to be moving — or to be frictionless, see *Walls* below and §7. The outlet also carries the only Dirichlet condition on pressure, and it holds `p = 0` whatever gravity is doing — see *Gravity* under §4.

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
- Precomputed wall velocities on the closed faces
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
- One residual sweep per pressure solve, not two
- Frames packed against their own contents instead of repeating them
- Semi-coarsening down to isotropy, including a two-to-one aspect ratio

The implementation prioritizes computational performance without changing the numerical formulation.

---

# Architecture

```text
CFD-Solver-2D/
├── .vscode/
├── out/
│   ├── build/
│   │   ├── x64Debug(or Release)
│   │   │   ├── bin
│   │   │   │   ├── Debug <-build here
│   │   │   │   │   ├── output/ <-vtk here
│   │   │   │   │   └── cfd_app.exe <-will be renamed to fluid_solver.exe, as there will be other solvers. other files arent really necessary to explain
├── logo/
│   ├── toxic-mark-32.png
│   ├── toxic-mark-64.png
│   ├── toxic-mark-128.png
│   ├── toxic-mark-256.png
│   ├── toxic-mark-512.png
│   └── toxic-mark-1024.png
├── src/
│   ├── main.cpp
│   ├── Config.cpp
│   ├── Mesh.cpp
│   ├── Restart.cpp
│   ├── Solver.cpp
│   ├── Multigrid.cpp
│   ├── MultigridCuda.cu
│   └── tiny_obj_loader_impl.cpp
├── include/
│   ├── Config.hpp
│   ├── Mesh.hpp
│   ├── Restart.hpp
│   ├── Solver.hpp
│   ├── Multigrid.hpp
│   ├── MultigridCuda.cuh
│   └── tiny_obj_loader.h
├── models/ <- actially not really needed, but the models could be stored here. we store them here for tests.
├── lib/
│   └── stl_reader/
├── CMakeLists.txt
├── LICENSE
└── README.md
```

---

# Requirements

- C++17 compatible compiler
- CMake 3.28+
- CUDA Toolkit (optional, for the GPU pressure solver)
- OpenMP (optional, picked up automatically when present)
- A CPU with AVX2 (optional, the vector kernels; without it every one of them
  falls back to the scalar loop it already carries)
- ParaView (optional, for looking at the output)

There are no other dependencies. `tiny_obj_loader` and `stl_reader` are
header-only and vendored, so a plain configure-and-build works out of the box.

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

Three switches, all on by default, all safe to turn off — the build works with
any combination of them:

| Option | Off means |
|---|---|
| `-DCFD_ENABLE_CUDA=OFF` | CPU multigrid only, no toolkit needed |
| `-DCFD_ENABLE_OPENMP=OFF` | single-threaded, bit-identical to the threaded build |
| `-DCFD_ENABLE_AVX2=OFF` | scalar kernels instead of the vector ones |

CUDA is also dropped automatically when no toolkit is found, unless
`-DCFD_ENABLE_CUDA_EXPLICIT=ON` says to treat that as an error instead.

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
- Gravity (optional: magnitude and direction)
- Time parameters
- Pressure solver parameters
- Geometry
- Slice orientation
- Walls (optional: rotation, sliding and free-slip, per object)

After confirmation the simulation starts immediately.

Every answer is read as a whole line and checked before it is accepted: a value
that does not fit is refused with the reason and the same question is asked
again, so one bad answer can no longer zero out everything after it. Pressing
Enter keeps the value shown in brackets, and `nx=256` typed straight into the
confirmation screen works too.

## Non-interactive mode (`key=value`)

Pass at least one argument and the program stops asking questions: it takes the
whole configuration from the command line, prints it once and starts. That is
the only switch there is - there is no `--batch` flag, `argc > 1` *is* the flag.

```powershell
.\install\bin\cfd_app.exe                        # interactive, as above
.\install\bin\cfd_app.exe --help                 # the keys and the rules
.\install\bin\cfd_app.exe nx=256 ny=128 nu=2e-3  # non-interactive
```

What changes in this mode:

- no questions and no confirmation screen, the run starts immediately;
- nothing is waited for at the end, the process just exits, so the whole run
  fits in a loop or a batch file;
- anything not given on the line keeps its default from `Config.hpp`;
- on a continuation a rejected value is fatal instead of being asked again.

### How to write the arguments

| Rule | Right | Wrong |
|---|---|---|
| no spaces around `=` | `nx=256` | `nx = 256`, `nx 256` |
| keys are case insensitive, leading dashes allowed | `nx=256`, `NX=256`, `--nx=256` | - |
| decimal separator is a dot | `nu=0.002`, `nu=2e-3` | `nu=0,002` |
| switches take 1 or 0 (`true/false`, `yes/no`, `on/off` also work) | `useCuda=1` | `useCuda=yeah` |
| no units inside the value | `totalTime=2` | `totalTime=2s` |
| counts are whole numbers | `saveInterval=25` | `saveInterval=25.5` |
| quote the whole token when the path has spaces | `"geometryFile=C:\my models\wing.stl"` | `geometryFile=C:\my models\wing.stl` |

Nothing is parsed leniently any more. A value that does not fit is refused by
name, every bad argument on the line is reported in one go, and the run does not
start:

```text
2 arguments are wrong:
  not a right way to write nu=0,002: the decimal separator is a dot, write nu=0.002
  not a right way to write useCuda=yeah: useCuda is a switch, write useCuda=1 or useCuda=0 (true/false, yes/no and on/off work too)

Nothing has been started. Fix the line and run it again.
```

A misspelled key gets the nearest real one: `saveinterwal=5` answers
*did you mean saveInterval?*

Values that are legal but suspicious are kept and warned about instead:
`CFL` above 1, `dtSafety` above 1, `omega` within 0.05 of 2, `mgTolerance`
above 0.1, `nu=0`.

### Keys and defaults

| Key | Type | Default | Accepted |
|---|---|---|---|
| `Lx` `Ly` | float, m | 1.0 | > 0 |
| `nx` `ny` | int | 50 | >= 8 |
| `U0` | float, m/s | 1.0 | any finite |
| `nu` | float, m^2/s | 0.01 | >= 0 |
| `ro` | float, kg/m^3 | 1.225 | > 0 |
| `gravityEnabled` | switch | 0 | 1 / 0 |
| `gravityAccel` | float, m/s^2 | 9.81 | >= 0 |
| `gravityAngle` | float, deg CW from down | 0 | any finite |
| `CFL` | float | 0.5 | > 0, warns above 1 |
| `totalTime` | double, s | 10.0 | > 0 |
| `dtUpdateInterval` | int, steps | 5 | >= 1 |
| `dtSafety` | float | 0.9 | > 0, warns above 1 |
| `omega` | float | 1.85 | 0 < omega < 2 |
| `smootherOmega` | float | 1.15 | 0 < omega < 2 |
| `mgIterations` | int, V-cycles/step | 2 | >= 1 |
| `mgTolerance` | float, relative | 1e-4 | 0 < tol <= 1 |
| `mgMinCoarseSize` | int, cells/axis | 8 | >= 2 |
| `useCuda` | switch | 1 | 1 / 0, ignored on a CPU-only build |
| `saveInterval` | int, steps | 20 | >= 1 |
| `outputDir` | path | `output` | created on the first frame, empty = current directory |
| `geometryFile` | path or `none` | `none` | `none` is the verification circle |
| `sliceAngleX` `sliceAngleZ` `sliceRotation` | float, deg | 0 | any finite |
| `invertSection` | switch | 0 | 1 / 0 |
| `wallMotion` | list | empty | `<object>:rot=90,slideX=0.5;<object>:slip=1` — an object either moves or slips, see below |
| `restart` | switch | 0 | 1 / 0 |
| `restartFile` | path | empty | a `.vtk` frame or the folder holding them |
| `addTime` | double, s | 0.0 | counts forward from the frame |

Duplicated keys are applied in order, so the last one on the line wins. On a
continuation every key given on the line is applied on top of the frame,
whatever its position - see *Continuing a run*.

### Parameter sweeps

The point of the mode: no keypress anywhere in here.

```powershell
foreach ($n in 0.01, 0.005, 0.002, 0.001) {
    .\install\bin\cfd_app.exe nx=256 ny=128 nu=$n totalTime=5 outputDir="out_nu_$n"
}
```

The exit code is 0 on a finished run and 1 on anything refused, so a sweep can
be stopped on the first bad line.

## Gravity

Off by default. Turning it on asks for two more numbers:

```text
Enable gravity? (0 = no, 1 = yes): 1
Enter gravitational acceleration (m/s^2, 9.81 on Earth): 9.81
Enter gravity direction (degrees clockwise from straight down: 0 = down, 90 = towards the inlet, 180 = up): 0
```

The direction is an angle rather than a vector for one reason: the flow
direction is the one thing in this domain that cannot be turned — the inlet is
always the left edge and the outlet the right one — so everything else turns
around it. `0` is straight down, and the angle runs clockwise from there, so
`90` points into the inlet, `180` is up, `270` runs along the flow. On the
command line it is the same three keys:

```powershell
.\install\bin\cfd_app.exe gravityEnabled=1 gravityAccel=9.81 gravityAngle=30
```

**Read the *Gravity* part of §4 before expecting it to do something.** The
short version: at constant density it cannot change the velocity field, only
the pressure. That is not a limitation of this implementation, it is what the
equations say.

## Walls: moving and slipping

Off by default. A moving wall is a wall whose *surface* has a velocity while
the body itself stays exactly where it is: a spinning cylinder, a conveyor
belt, the lid of a driven cavity. Gravity is the term that cannot change the
flow; this is the opposite — it is the cheapest way there is to put
circulation into one, and it is what the Magnus effect is made of. A rotating
cylinder in a stream develops real lift, a rotating valve or blade drags its
boundary layer around with it, and a belt drives a shear layer with no inlet
involved at all.

Every separate body in the mask is found and numbered on its own, and each one
takes its own motion. One body is one entry, the entry opens with that body's
number and a colon, and everything the body does goes inside it:

```text
<object>:<setting>=<value>,<setting>=<value>;<object>:<setting>=<value>
```

| Setting | Unit | Means |
|---|---|---|
| `rot` | degrees/s, counter-clockwise | the surface turns about that body's own centroid |
| `slideX` `slideY` | m/s | the surface is dragged in a straight line |
| `slip` | switch | free-slip instead of no-slip: the fluid slides along the wall and the wall exerts no drag |

The first two rows are one group and the third is the other. `rot` and `slide`
keep the no-slip wall and give its surface a velocity, so the wall holds the
fluid and now carries it somewhere; `slip` does the opposite and stops the wall
from holding the fluid at all. Nothing about the geometry changes either way —
a body never moves, only the velocity its surface hands to the fluid does.

`rot` and the two `slide` components add up, because together they are just the
rigid-body velocity field **v = slide + ω × (x − centre)**. Counter-clockwise
means what it looks like in ParaView: with `rot=720` the top of the body runs
towards the inlet at two turns a second.

`slip` is exclusive with all three, and asking for both is refused rather than
half-applied. A free-slip wall carries no tangential stress *by definition* —
that is what free-slip means — and tangential stress is the only thing a
spinning or sliding surface has to push the fluid with. `1:rot=90,slip=1` is
not a combination, it is a contradiction, so it stops the run and says so.
Different objects can of course do different things.

```powershell
.\install\bin\cfd_app.exe "wallMotion=1:rot=720"
.\install\bin\cfd_app.exe "wallMotion=1:rot=720;2:slideX=-1.5,slideY=0.4"
.\install\bin\cfd_app.exe "wallMotion=1:slip=1;2:rot=-45"
.\install\bin\cfd_app.exe wallMotion=1:rot=720,2:slideX=-1.5
```

Objects are separated by `;` and settings by `,` — and either separator opens a
new object as long as the number and its colon come first, which is what the
third line uses. PowerShell eats a bare `;`, so the semicolon form has to be
quoted and the comma form does not. Everything else is the same as any other
key: no spaces around `=`, dots for decimals, names case insensitive.

**Where the numbers come from.** The mask is flood-filled into numbered bodies
before the run starts, and the list is printed with the rest of the mesh
information:

```text
  Number of solid cells = 117
  Number of objects = 2 (these numbers are what wallMotion takes)
    object 1: 76 cells, centre (0.5, 0.346628) m, rim 0.109486 m
    object 2: 41 cells, centre (1.04992, 0.679688) m, rim 0.0781845 m
```

Numbering follows the scan order of the grid — bottom row first, left to
right, a body taking its number from the first of its cells the scan reaches —
so the same mask always produces the same numbers, and *the mask comes out of
the frame on a continuation*, which means the
numbers therefore still mean the same bodies after a restart. The usual way to
use this is to let the first run print the list, then continue with the motion
you want. Two cells that touch only at a corner count as one body, since the
flow cannot squeeze through that corner either.

> **Known issue.** All of the above is what the solver does with a mask that
> already has several bodies in it, and it is what a continuation gets, since
> the mask then comes out of the frame. Getting there from a *model* does not
> work yet: the section keeps only its largest contour, so a model that cuts
> into two shapes loses one, and a model with a hole gets the hole filled.
> Object numbering and contour generation are both being reworked, and this is
> fixed closer to the 1.3 release.

`rim` is the distance from the centroid to the farthest cell, i.e. the radius
the rim speed is computed at, because degrees per second is not a number you
can compare to `U0` on sight. The solver does that comparison for you:

```text
Walls:
  object 1 at (0.5, 0.346628) m: rot = 720 deg/s -> rim speed 1.37584 m/s, slide = (0, 0) m/s
    surface moves at 1.37584x the inlet velocity
  object 2 at (1.04992, 0.679688) m: rot = 0 deg/s -> rim speed 0 m/s, slide = (-1.5, 0.4) m/s
    surface moves at 1.55242x the inlet velocity
```

**What it does and does not do.** The tangential part of the surface velocity
drives the flow. The normal part is dropped, and it has to be: the mask does
not move, so a surface pushing into its own body would be creating mass on one
side and destroying it on the other. For rotation that costs nothing at all —
spin about the centroid is tangential everywhere by definition. For sliding it
is the entire difference between *the surface slides* and *the body moves*,
and the second one is `Moving objects`, still on the list at the top of this
file. §7 has the discrete version of the same argument.

**Measured.** 128×64, `Lx=2 Ly=1 nu=0.005 U0=1`, the verification circle,
mean pressure over the ring of fluid just outside the body:

```text
                       above     below     below - above
static cylinder      -0.15518  -0.15517       0.00001 Pa
rot = 1440 deg/s     +0.02670  -0.52826      -0.55496 Pa
```

The static body is symmetric to the fifth decimal, as it has to be. Spinning
counter-clockwise, the top surface runs against the stream and the bottom runs
with it, so the flow is slowed above and sped up below, and the pressure
follows: half a Pascal more of it on top than underneath, pressing the body
down. That is the Magnus force, on the correct side, at a believable size.
Divergence over the same runs stayed at `2·10⁻⁵` against `1·10⁻⁵` for the
static body, i.e. nothing leaked.

**One side effect worth knowing.** The wall velocity is a velocity like any
other, so it goes into the CFL limit. On that run `|u|max` went from 1.40 to
2.16, the wall now being the fastest thing in the domain, and `dt` halved from
`4.2·10⁻³` to `2.0·10⁻³` s. (2.16 is below the 2.50 m/s rim speed printed at
startup, and should be: the faces that carry the motion are the ones buried
inside the body, a cell in from the outermost one the rim is measured to.) A
wall spun much faster than the flow buys its physics with step count, and the
solver says so at startup when the surface is more than a few times `U0`.

**Free-slip, and what it is for.** No-slip is the physically right condition
for a viscous fluid on a real wall, and it is the default. Free-slip is what
you want when the wall is not really a wall: a symmetry plane, a fluid-fluid
interface standing in for a free surface, or a body you want present as an
*obstacle* without the boundary layer it would really grow. It is also the
cheap way to ask how much of a wake is displacement and how much is friction —
run the same geometry twice and difference them.

Measured on a flat, grid-aligned wall (a rectangular block, `nu=0.005`), the
tangential velocity in the first five fluid cells above it:

```text
no-slip     +0.0540  +0.1250  +0.2116  +0.3121  +0.4247
slip        +0.4218  +0.4471  +0.4958  +0.5644  +0.6484
```

No-slip pins the fluid to the wall and grows a boundary layer out of it. Free
slip lets it past at 0.42 and the profile is nearly flat across the first two
cells — what is left is the outer shear of the channel, not wall friction.

On a *staircase* body the effect is real but partial: the same measurement on
the crown of the circle gives `0.311 → 0.592`, a large reduction and not a
removal. The steps either side of the crown present vertical faces, and those
are no-penetration faces, which free-slip does not and must not touch. That
residual is the staircase approximating a smooth circle, not the slip
condition; it goes away as the mask gets finer, and it is the same error the
immersed boundary already has without slip.

A body one cell thick can only be driven along itself. The tangential value
lives on the faces *inside* the body, and a one-cell-tall bar has buried `u`
faces but no buried `v` faces at all — so it drags in `x` and ignores `slideY`
and, being flat, very nearly ignores `rot` too. A single isolated cell has
neither kind and cannot drive anything. Both are found, numbered and reported
like any other object; they just have nothing to push with. §7 explains which
face is which.

# Continuing a run

The first thing the configuration asks is whether this is a new simulation or a
continuation of an old one:

```text
Start a new simulation or continue an old one?
  0 = new simulation
  1 = continue from a saved .vtk
>
```

Answer `1` and give it either a frame or the folder the frames live in — a
folder means "take the newest one". Every parameter of the old run comes back
out of that file, and the usual confirmation screen opens on top of it, so
anything can still be changed before the run starts. The typical reason to
continue is precisely that: the flow looked interesting somewhere in the middle
and you want denser output from there on, so you continue with a smaller
`saveInterval` and a longer `totalTime`.

The same thing from the command line, where anything after `restartFile`
overrides what the frame remembers:

```powershell
.\install\bin\cfd_app.exe restart=1 restartFile=output totalTime=30 saveInterval=5
```

**What a continuation writes.** Frames are named after the file they were
started from, and carry their own step number, so the name says both where the
run came from and how far it got. Continuing from step 200 with
`saveInterval = 100`:

```text
output/solution_200.vtk          <- what you continued from
output/solution_200_300.vtk      <- step 300
output/solution_200_400.vtk
output/solution_200_522.vtk      <- final step
```

Nothing is ever overwritten and it stays obvious which run produced what.
Continue from `solution_200_400.vtk` and the next series is
`solution_200_400_450.vtk` and so on. A fresh run is unchanged:
`solution_<step>.vtk`.

**What can and cannot change.**

| Parameter | On a continuation |
|---|---|
| `nx`, `ny`, `Lx`, `Ly` | fixed by the frame, changing them is refused |
| geometry (`geometryFile`, slice angles, `invertSection`) | ignored — the solid mask comes out of the frame, the model file is not needed any more |
| `totalTime` | must be larger than the time already reached, otherwise there is nothing to compute |
| `saveInterval`, `outputDir`, `CFL`, `dtSafety`, `dtUpdateInterval`, `omega`, `smootherOmega`, `mgIterations`, `mgTolerance`, `mgMinCoarseSize`, `useCuda` | free |
| `U0`, `nu` | allowed, but it is a discontinuity in the physics, not a continuation of the same problem |
| `ro` | free — it only scales the pressure on the way out to Pa, the frame stores the kinematic field |
| `gravityEnabled`, `gravityAccel`, `gravityAngle` | free — changing them shifts the hydrostatic part of the pressure and leaves the velocity where it was |
| `wallMotion` | free — the mask comes out of the frame, so the object numbers still mean the same bodies. Spinning a wall up mid-run is a step change in the boundary condition, not a discontinuity in the state |

Continuing from the last frame of a run and letting it go further produces
**bit-identical** results to never having stopped at all.

Frames written before gravity existed simply do not carry its three keys, and
the reader skips keys it does not know, so they load with gravity off. Frames
written now stay readable by builds that predate it, for the same reason.

## Continuing a run, in detail

Every frame is also a checkpoint. That is less obvious than it sounds, because
what a frame shows and what the solver needs are not the same thing.

**Why the visible arrays are not enough.** The solver lives on a staggered
grid: `u` sits on `(nx+1)×ny` vertical faces, `v` on `nx×(ny+1)` horizontal
ones. A VTK frame is cell centred, so what gets written is the average of the
two faces around each cell — `0.5*(u[i] + u[i+1])`. Averaging throws away
exactly one degree of freedom per row, and no amount of cleverness gets it
back. Reading a frame and interpolating back onto the faces gives a field that
looks right and is not divergence free, which the projection then has to repair
with a visible kick.

So every frame carries a `FIELD RestartData` block at the very end, after the
arrays ParaView cares about:

```text
FIELD RestartData 2
configText 1 <n> char           the whole configuration as key=value lines,
                                plus restartTime, restartStep and restartDt
facePack   1 <n> unsigned_char  the raw face velocities, stored as the
                                difference from what the cell averages
                                predict — see "What a frame is made of"
```

A frame without face velocities — one written before the `RestartData` block existed, or one whose packed block fails its checksum — has them rebuilt from the cell averages and the state projected once. The reconstruction is masked before that projection: interpolation fills *every* face, the ones held shut against a wall included, and a projection of a field that flows through walls is not a projection of anything. Masking first is what the predictor does on every ordinary step.

Pointing `restartFile` at a folder takes the newest frame in it, by file timestamp, with the step in the file name breaking ties — a run short enough to write its whole output inside one second gives every frame the same timestamp.

`FIELD` is the only legacy VTK block that lets each array declare its own tuple
count, which is the whole reason it is used — `(nx+1)*ny` simply does not fit
in a `CELL_DATA` section. The pressure the restart needs is not in here at all:
the `SCALARS pressure` array ParaView reads is the same field times `ro`, so
the reader divides it back out rather than the frame carrying it twice. What is
left costs about 12% more file size, and it is the difference between a frame
you can look at and a frame you can resume.

The configuration is serialized in the same `key=value` form the command line
already speaks, so reading it back is `setParam()` on each line, and unknown
keys are skipped instead of rejected — old frames stay loadable and new frames
do not break old builds.

**The two things that make it exact.** Restoring `u`, `v`, `p` and the clock is
not quite enough, and both leftovers are easy to miss:

*The time step in flight.* `dt` is only recomputed every `dtUpdateInterval`
steps. A continuation that recomputes it immediately would shift that cadence
by a fraction of a step and drift away from the original trajectory, so the
frame stores the `dt` that was live when it was written and the solver takes it
back instead of recomputing.

*The nested iteration.* The first pressure solve of a run does one full
multigrid pass to build a field out of nothing. A continuation is not starting
from nothing — it has the converged pressure of the step it stopped at, which is
a better guess than that pass produces — so the pass is skipped. Leave it in and
the field gets nudged, and the two trajectories separate within a few steps.

With both pinned, continuing from frame *k* and running on gives byte-for-byte
the same frames as the uninterrupted run would have.

**Frames written before all this existed** still load. The face velocities get
rebuilt from the cell averages, the state is projected once before the first
step, and it says so loudly. It is a restart, not *the* restart: use it to
rescue an old run, not to claim continuity.

**The mask comes from the frame**, not from the model. `Mesh` takes an optional
preset mask and skips loading, slicing and rasterizing entirely when it gets
one. The STL does not have to still exist, and no rasterizer change can ever
move a boundary in the middle of a run.

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

Pressure has no equation of its own; it's whatever makes the flow divergence-free. So each step you advance momentum ignoring pressure, measure the divergence you created, solve a Poisson equation for the pressure that cancels it, and subtract its gradient. The grid is staggered so pressure gradients and divergences land exactly where they're needed and the chessboard mode can't survive. The Poisson operator encodes every boundary condition in its coefficients, which guarantees it's exactly `div ∘ grad` and therefore that the projection actually projects. Multigrid solves it in `O(N)` by exploiting the fact that SOR smooths error fast but converges slowly — so you smooth on every grid size at once. Everything else is SIMD, threads, and not allocating memory in the inner loop. Gravity, if you switch it on, never enters the solve at all: at constant density it is exactly a pressure offset, so the solver works in the reduced pressure and puts the hydrostatic part back on the way out — §4 explains why that is the correct answer and not a shortcut. Wall behaviour is the mirror image: the faces buried inside a body are handed the surface velocity instead of zero — or the neighbouring fluid value, which is free-slip — and the operator does not change one coefficient either way. §7.

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

### When the estimate is not an estimate

Both limits are *upper bounds from stability*. Neither says the step is a good idea, only that the scheme will not explode at it — and a bound can be vacuous. A slow flow in a thin fluid, `U0 = 0.001 m/s` and `ν = 1e-6 m²/s`, puts the CFL limit at 15 s and the viscous one at 97 s, so a one second run is a single step. At a Courant number of 0.03 that step is perfectly accurate; it is just not a film. The startup line prints both limits and the step count they imply, and says so when the run fits in a couple of steps — what fixes that is more `totalTime` or a finer grid, not a smaller `dt`.

Two cases are not estimates at all, and both stop the run rather than substituting a number nobody chose:

**The field is no longer finite.** `maxps` returns its second operand when either input is a NaN and `std::max` drops it the same way, so a `NaN` sails straight through a maximum. The Courant reduction carries a `_CMP_UNORD_Q` accumulator beside the max to catch it.

**`dt` comes out zero or infinite.** That means the grid spacing or the viscosity are outside what a float can express — `Lx=1e-20` overflows `1/dx²` and sends the viscous limit to zero. The run stops and prints `dx`, `dy` and `ν`, which is the actual problem.

One rule at the other end of the run: **no step is ever shorter than half of `dt`.** The last stretch up to `totalTime` rarely divides evenly, and the naive thing — take full steps, then whatever is left — can leave a final step of nanoseconds. That step is not wrong, but the Poisson right-hand side is `div/dt`, so the frame it writes has a pressure map scaled by a factor of millions, and the last frame of a run is one people open. So when the remainder is between one and two `dt`, it is split in half instead. The leftover after the final step is then set to zero outright rather than accumulated, because `currentTime` adds up single-precision steps and lands a few ulps short of `totalTime` — which used to be enough on its own to trigger one more absurd step, on roughly half of all runs.

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

There is no gravity term in there, and that is deliberate — the next heading is
about why. What the real line does carry is the mask multiply and the wall
value that rides with it, so a closed face leaves the predictor holding exactly
its boundary velocity; §7.

Then `v` gets the mirror-image treatment.

### Gravity — and why it does nothing

This one deserves its own heading, because the honest answer is unintuitive
enough that people assume the code is broken when they see it.

**At constant density, gravity cannot change the velocity field.** Not
approximately — exactly. Gravity is uniform, so it is the gradient of a
potential:

```
g = ∇Φ        with        Φ = gx·x + gy·y
```

which means `−∇p + g = −∇(p − Φ)`. Substitute `P = p − Φ` and the momentum
equation is *literally* the gravity-free one. So the flow is whatever it was,
and the pressure gains a hydrostatic term. This is why a swimming pool doesn't
develop currents.

**So the solver solves for `P` and never touches `g`.** That is the entire
implementation. `p` in the code *is* the reduced pressure: the predictor has no
body-force term, the Poisson operator has no gravity in it, the outlet keeps
its plain `p = 0`, and the multigrid never learns that gravity exists.
`phiCell()` adds `Φ` back in the one place that writes pressure out — the VTK
scalar, which is also what the restart reads back — and `setInitialState()`
takes it off again when a frame is read back in. `gx` and `gy` appear nowhere
else.

Measured. 128×64, `nu=0.005`, `g = 9.81` straight down, 98 steps:

```text
uFace and vFace, gravity on vs off     bit-identical
p(on) − p(off) against ρΦ              max error 1.05·10⁻⁶ Pa
```

Not "small" — *identical*, the same bytes, because gravity does not enter a
single arithmetic operation of the solve. And the pressure differs by exactly
the hydrostatic head, to the last digit a float of order 6 Pa has.

**Why not the obvious way.** The first version did the obvious thing: a `dt·gx`
term in the predictor for the projection to cancel. It works, and it drags two
problems in behind it.

The outlet is the only Dirichlet condition in the operator and it holds
`p = 0` on the face, while the hydrostatic field wants `Φ` there. Those are
different demands. Left alone the outlet is pinned to a pressure the field
cannot reach, and the mismatch drives a jet of order `√(g·Ly)` — with `g = 9.81`
and `Ly = 1`, several times the inlet velocity. It doesn't look like a bug, it
looks like gravity working, which is the worst way for a bug to look:

```
|u|max, gravity off                    1.386   (steady)
|u|max, gravity on, outlet left alone  3.671   (still climbing at t = 0.38)
|u|max, gravity on, outlet corrected   1.386   (identical to gravity off)
```

Correcting it took a `2Φ/dx²` term on the right-hand side and a matching
`+2·dt·(p − Φ)/dx` in the corrector. And even corrected, `Φ` was still sitting
inside `‖rhs‖`, which is what the multigrid measures its *relative* residual
against — so the same `mgTolerance` bought a looser solve with gravity on than
without, and divergence went from `8·10⁻⁵` to `6·10⁻⁴` purely from stopping a
cycle early.

Working in `P` from the start deletes all of it. No predictor term, no outlet
special case, no inflated `‖rhs‖`, and no large number added at one end of the
step and subtracted at the other in single precision. Six sites in the
predictor and one boundary condition became one inline function called twice on
the way out, and the CUDA path never had anything to say about it either way.

**So why implement it at all?** Because what survives is the part that will
still be needed when the cancellation stops working. Gravity starts driving
flow the moment density stops being constant — a second phase, or a Boussinesq
buoyancy term from a thermal solver, both of which are on the list at the top
of this file. Then the force is `g·(ρ(x) − ρ₀)/ρ₀`, `Φ` is no longer a potential
for it, `P = p − Φ` no longer removes it, and it has to come back into the
predictor as a real body force — that time earning its place. The direction
convention, the config plumbing, the restart handling and the output path are
already right and stay put. And in the meantime the pressure field in ParaView
is a real pressure field, hydrostatic head included, rather than one with
gravity quietly left out of it.

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

The outlet face is the exception, since it is the one place with a prescribed pressure rather than a prescribed velocity — see §5, and *Gravity* above for what changes there when gravity is on.

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

Being the only Dirichlet condition also makes it the only place a body force would have to be told anything — which is exactly why gravity is not implemented as one. See *Gravity* in §4.

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

### Numbering the bodies

Nothing above cares how many obstacles there are — the mask is just cells. Wall motion does, because two bodies can spin differently, so the mask is flood-filled once into numbered components before the run starts, 8-connected. Diagonal connectivity is the right choice here for the same reason the mask works at all: two cells meeting at a corner leave no face for the flow to pass through, so calling them two obstacles would be a lie about the geometry as well as a nuisance to configure. The fill is an explicit stack, not recursion, because a body can be the whole grid.

Each component keeps its cell count, its centroid — the axis rotation turns about — and the distance to its farthest cell, which is the radius `rot` is turned into a rim speed at.

### Wall behaviour, discretely

Closed faces come in two kinds, and this is the whole trick:

| Closed `u` face | Solid on | Is | Holds |
|---|---|---|---|
| `i` | one side | a vertical wall, and this face is *normal* to it | **0** |
| `i` | both sides | a face buried in the body | `2·u_wall − u_fluid` |

A face normal to a wall stays shut, exactly as before, because the body is not going anywhere. A face inside the body looks useless — no fluid cell owns it — but it is not: the fluid `u` face one row above reads it as `u_bot` in `d²u/dy²` and in the upwind term. That read *is* the no-slip condition along that stretch of horizontal wall. `v` faces mirror it: the ones inside the body are what the fluid to the left and right of a vertical wall shears against.

**And the value it has to hold is not the wall's velocity.** This is worth being careful about, because the obvious choice is wrong and it is wrong by exactly a factor of two. The wall is a *cell edge*. The buried face sits half a cell inside it, the fluid face half a cell outside, and the wall is the midpoint of the two. Whatever a stencil reading across the pair sees at the wall is therefore the **average** of the two faces. Put `u_wall` on the buried face and the wall ends up holding `(u_fluid + u_wall)/2`, which is not the boundary condition anybody asked for: for a stationary wall the fluid slides along it at half its own near-wall speed, the shear `∂u/∂y` comes out as `(u_fluid − u_wall)/dy` instead of `(u_fluid − u_wall)/(dy/2)`, and **every body in the domain has half the drag it should**. A moving wall has the same problem from the other end — it drags the fluid at half strength.

The value that puts `u_wall` on the wall is the mirrored one, `2·u_wall − u_fluid`. It is the standard ghost-cell reflection, and here it costs nothing extra: the pairing machinery already exists for free-slip (below), the no-slip version is the same list with a different expression on it, and the wall velocity is evaluated at the cell edge the wall actually is rather than at either face's own position. On a body thin enough to have fluid on both sides, one buried face serves two walls and takes the average of the two, which is the same code path with the same index twice.

It costs no stability. The mirror raises the diagonal of the viscous operator at a near-wall face from `2/dy²` to `3/dy²` and drops the corresponding off-diagonal by the same amount, so the Gershgorin bound on the spectrum — and with it the diffusive `dt` limit — does not move. The convective term picks up the same half-cell distance, which is the distance it should be differencing over.

The predictor and corrector write `uMask*(...) + uWall`, which resets the buried faces to the plain wall value every step, and the mirror is re-applied at the top of the next one from the field as it then stands. Same lifecycle as free-slip, same place in the loop.

So a horizontal stretch of wall is driven through the `u` faces inside the body, a vertical stretch through the `v` faces, and every staircase in between gets both. The predictor and corrector change by one term:

```cpp
uStar = uMask*( ... ) + uWall;     // uWall is nonzero only on the buried faces
u     = uMask*( ... ) + uWall;
```

`uWall` and `vWall` are built once, next to the masks, because neither the geometry nor the motion changes during a run.

**Why the Poisson operator does not change.** A coefficient is zero when the corrector does not own the face — and it still does not own it, whatever value that face now holds. So `L` is the same matrix, the identity `L = div ∘ grad` is untouched, the projection still projects, and the CUDA path needed no changes at all. The RHS in *fluid* cells does not move either: every face a fluid cell owns that touches solid is a face normal to a wall, and those are still zero.

**Why nothing leaks.** A rigid-body velocity is discretely divergence-free, exactly, with no truncation error. `u = slideX − ω(y − cy)` depends only on `y`, and both vertical faces of a cell sit at the same `y`, so `∂u/∂x` differences to a hard zero; `v` depends only on `x` and the same happens vertically. Filling a body with its own motion therefore creates no mass anywhere inside it, and by the discrete divergence theorem none crosses its boundary either. This is also the discrete version of *why the normal component is dropped*: keeping it would be the statement that the mask moves, and the mask does not.

**Free-slip is the same table with one entry changed.** No-slip mirrors the buried face about the wall's own velocity. Free-slip wants the opposite — no tangential stress, `∂u_t/∂n = 0` — which discretely means the buried face has to hold *whatever the fluid face across the wall currently holds*, so the difference the viscous stencil takes across the wall is zero and the upwind term through it is zero too. It is exactly the treatment the domain's own top and bottom walls have always had (`u_bot = u_ij` at `j = 0`), applied to a body.

That value is not a constant, so it cannot live in `uWall`. What is constant is *which* face mirrors *which*, so that pairing is what gets precomputed: one list of `(buried face, the open faces beside it)` per axis, built once after the masks, refreshed at the top of every step. A buried face with fluid on one side takes it exactly; on a body thin enough to have fluid on both sides it takes the average, which is the same expression with the same index twice, so there is one code path and no branch.

The mirrored values never reach the pressure solve. The predictor writes `uMask*(...) + uWall` into `u*`, and on a buried face the mask is zero — so `u*` holds the wall value, not the mirrored one, and the divergence, the right-hand side and the operator are exactly as they were. Free-slip is purely a change to what the *viscous and convective* stencils read across a wall, which is precisely what it means physically. And no-penetration is untouched: the faces normal to a wall are still shut, because free-slip removes friction, not the wall.

**The one thing that does need cleaning.** Solid cells have a zero diagonal and are never solved, so their residual is just their right-hand side — and `‖rhs‖`, which the whole grid's relative tolerance is measured against, is summed over every cell. A boundary solid cell has one face at zero and another at the wall velocity, so its divergence is not zero, and left alone a spinning body would inflate `‖rhs‖` and quietly loosen the tolerance everywhere (the same trap gravity fell into, one section up). So the RHS is multiplied by a fluid-cell mask on the way out. Without moving walls that multiply changes nothing — the divergence in a solid cell is already exactly zero — which is why it can be there unconditionally.

---

## 8. The speed layer

Everything above is the algorithm. This part is just making it run fast without changing a single number.

**Branch-free hot loops.** Every velocity on a closed face is held at a value that does not change during the run — zero, or the wall's own velocity if that wall moves. So the stencil can be evaluated unconditionally, the worst it ever reads being a legitimate boundary value, and the result multiplied by a float mask and offset by a float wall array. No branch, no scalar fallback near the body, and no `ω × r` evaluated per face per step — the two arrays are built once, next to the masks.

That add is unconditional, which is the deliberate part: one extra load and one extra `_mm256_add_ps` per face beats a branch, and with nothing moving the array is all zeros and the arithmetic is exact. On 256×128 the cost does not come out of the run-to-run noise (0.561–0.564 s before, 0.560–0.565 s after), and the fields are identical — literally, except that a closed face now holds `+0` where it used to hold `−0`, which nothing in the solver can tell apart.

**Halo padding.** Each pressure array is allocated with `max(nx,8)` extra floats at *both* ends, with the logical pointer offset into the middle. So `p[id−1]`, `p[id+nx]` etc. are always valid memory, even for the very first and last cell. An 8-wide AVX load at the edge reads into the halo (which is zero, and whose coefficients are zero anyway) instead of segfaulting. This is what makes the branch-free vector code *safe*, not just fast.

**Red/black SOR with masked stores.** Every neighbour of a red cell is black, so all red cells can update simultaneously. The naive version strides by 2, which kills SIMD. Instead: compute the update for all 8 lanes, then store only the current colour with `_mm256_maskstore_ps`. Half the arithmetic is discarded, but every load and store stays contiguous — a big net win on a memory-bound kernel. Since vectors always start at even `i`, the lane mask is one of exactly two constants.

**OpenMP.** Rows go to different threads. Red/black makes this race-free with no locks. One fork/join per `smooth()` call, and levels under 32 rows run serially — they're a few hundred cells visited by every cycle, and barrier traffic there costs more than the arithmetic.

**And all of it is optional.** Every vector kernel in here already had a scalar
loop after it, for the last `nx % 8` cells of a row — so the fallback for a CPU
without AVX2 is not a second implementation, it is the same loop starting at
`i = 0` instead of where the vector one stopped. The red/black smoother is the
neat case: its remainder loop already strides by 2 from the right parity, so
skipping the vector part turns it into a plain red/black sweep with nothing
else changed. Guarded on `__AVX2__`, so `-march=native` picks the fast path up
by itself. Measured against the vector build on a moving-wall run, the two
agree to `2·10⁻⁷` relative on velocity and `6·10⁻⁶` on pressure — float
rounding from a different summation order, not a different algorithm.

**CUDA.** Same algorithm, one thread per cell. The whole hierarchy is allocated once, the stencil is uploaded once (it's a function of geometry, and geometry is static), and the pressure field **stays resident on the GPU between time steps** — which means last step's solution is a free warm start. Only the RHS crosses the bus each step. Kernels run on the default stream, which serialises them, so the chain `smooth → residual → restrict → recurse → prolongate → smooth` needs no explicit synchronisation.

---

## 9. Output

`saveVTK` byte-swaps values into a 16 KB stack buffer and writes binary legacy VTK straight out — no temporary arrays, no per-cell copies. Pressure is stored internally as `p/ρ` (kinematic), so it's multiplied by `ro` on the way out to give Pascals. Btw kinematic pressure is much easier to use cause if u divide regular pressure by density u get m^2/s^2, not some kg/(m*s^2)

One cell class gets special treatment on the way out. Solid cells have a zero diagonal and never take part in the solve, so their pressure sits at a permanent zero. Without gravity that zero is somewhere in the middle of the fluid range and nobody notices. With it the fluid is offset by the hydrostatic head, and the body would punch a visible hole straight through the pressure map — so solid cells are written with the hydrostatic value they would have had. Nothing ever reads them back; this is purely what ParaView sees.

The velocity of the same cells gets the same treatment for the same reason. A cell centre value is the average of its two faces, and inside a body those faces hold the mirrored value the no-slip condition needs — an artefact of the stencil, not a speed anything moves at — while on the rim one of them is normal to the wall and held shut. Either way the average is not a velocity. Solid cells are written with the surface velocity they actually impose instead: zero for a body that does not move, and for one that does, the thing that makes a spinning body look like a spinning body and a sliding one like a belt. Again: output only, never read back.

### What a frame is made of

About 19 bytes a cell, and nothing in it is stored twice.

| Array | Bytes/cell | |
|---|---|---|
| `pressure`, float, Pa | 4 | what ParaView colours |
| `velocity`, 3×float | 12 | what ParaView glyphs |
| `solid`, `unsigned_char` | 1 | the mask holds 0 or 1, and `bit` is not a type ParaView reads reliably |
| `facePack` | ~2.2 | the staggered face velocities |
| `configText` | ~0.1 | the run's own settings |

There is no pressure array for the restart to read: `SCALARS pressure` is the same field multiplied by `ro`, bit for bit, so the reader divides `ro` back out — taking the density from the frame's own configuration text rather than from the run being started, in case that changed.

The face velocities cannot be dropped the same way. The cell velocity is an average and averages do not invert. But they are *nearly* determined by it: along a row, `u_cell[i] = (u[i] + u[i+1])/2` gives `u[i+1] = 2·u_cell[i] − u[i]`, and marching that from the inlet face reproduces the whole row to about 3e-6 m/s on a grid 256 wide. Close, and not close enough — the error accumulates along the march and lands as a divergence blip of the same order as the solver's own convergence.

So the frame stores the march's **mistake** rather than the answer. For each face: predict it, subtract the prediction's bit pattern from the true one, write the difference as a zigzag varint. An exact prediction costs one zero byte, one ulp out either way costs one byte, a real miss costs five. About one face in eight is predicted exactly, the block comes out at 2.2 bytes a cell against the 8.1 the raw arrays need, and it is **lossless** — the reader adds each delta back before predicting the next face from the corrected value, so nothing accumulates and there is no drift to bound.

Three things make it safe to rely on:

- The prediction is `2.0*cell − previous` in double, and nothing else. In float it would overflow above 1.7e38 unless the compiler contracted it into an FMA, and whether it does depends on `-mfma`; doubling a float is exact in double whatever the compiler does, and the single conversion back rounds once. Writer and reader call the same inline function.
- The two lines the march starts from, `u` at `i = 0` and `v` at `j = 0`, are written out in full rather than derived. That is one column and one row, a fraction of a percent, and the reader never has to reproduce how the inlet or the bottom wall were set on the run that wrote the frame.
- A 32-bit FNV-1a over the faces goes in the block header. On a mismatch — a truncated file, a bad byte — the reader says so and falls back to rebuilding the faces from the cell averages with one projection. It cannot silently restart from something subtly wrong.

**Frames carry a `formatVersion`, currently 2.** A build reads every frame at or below its own version: version 1 spelled `uFace`, `vFace` and `pRaw` out as plain float arrays and had `solid` as `int32`, and those load as they always did. The reverse does not hold — a version 1 reader takes four bytes a cell for `solid`, desynchronises inside the file, and cannot be rescued after the fact, because the configuration text that carries the version sits at the *end* of the frame. The version is there so that a reader can name the problem instead of reporting whatever binary garbage it lands on.
