# Fluid Solver 0.1

First release. A 2D incompressible Navier–Stokes solver for external flow around
an arbitrary profile — a cylinder, an airfoil, a valve, a turbine blade — writing
binary VTK frames you open in ParaView.

Chorin projection on a staggered MAC grid, geometric multigrid pressure solve,
immersed-boundary solid mask. Single executable, no runtime dependencies, no
installer required if you don't want one.

---

## Which file do I download?

**If you want it installed with shortcuts:** take the setup for your system and
let it pick the build for you.

| Your system | File |
|---|---|
| Windows 64-bit | `Fluid Solver 0.1 windows-x64 setup.exe` |
| Windows 32-bit | `Fluid Solver 0.1 windows-x86 setup.exe` |
| Linux 64-bit | `Fluid-Solver-0.1-linux-x64.run` |
| Linux 32-bit | `Fluid-Solver-0.1-linux-x86.run` |
| macOS Apple Silicon | `Fluid Solver 0.1 macos-arm64.pkg` |
| macOS Intel | `Fluid Solver 0.1 macos-x64.pkg` |

The installer detects your CPU and GPU, recommends the right build, and lets you
override it. It also asks whether to install the desktop UI and whether to make
shortcuts.

**If you just want the executable:** take the `.zip` that matches your system and
the features you want. Unzip it anywhere and run it — `output/` appears next to
the executable and the frames go there.

Feature suffixes, fastest first:

| Suffix | Needs | What it buys |
|---|---|---|
| `avx2-omp-cuda` | AVX2 CPU + NVIDIA GPU | everything on |
| `avx2-omp` | AVX2 CPU (Intel/AMD from ~2013) | vector kernels and all your cores |
| `avx2` | AVX2 CPU | vector kernels, one thread |
| `omp` | any x86 CPU | all your cores, scalar kernels |
| `plain` | any CPU | runs anywhere, slowest |
| `…-cuda` | NVIDIA GPU + driver | the pressure solve runs on the GPU |

**Not sure? Take `avx2-omp`.** Any CPU newer than about 2013 has AVX2, and every
build produces the same numbers — they differ only in speed.

A CUDA build on a machine with no NVIDIA card does not fail: it says so and runs
the pressure solve on the CPU.

Also in this release:

- `Fluid-Solver-Source-Code.zip` — the full source this release was built from
- `README.md` — the documentation for exactly this version
- `SHA256SUMS.txt` — checksums for everything above

---

## Quick start

```
Fluid Solver
```

with no arguments walks you through the configuration and starts. Or pass
`key=value` pairs and it runs without asking anything:

```
Fluid Solver nx=256 ny=128 Lx=2 Ly=1 U0=1 nu=0.002 totalTime=5 saveInterval=25
```

That's flow past a cylinder at Re = 100. Frames land in `output/solution_*.vtk`.
Open the whole series in ParaView — it groups them into an animation on its own —
colour by `pressure`, add a Glyph filter on `velocity`.

Your own geometry instead of the cylinder:

```
Fluid Solver geometryFile=wing.obj sliceRotation=-5 nx=256 ny=128 totalTime=5
```

`Fluid Solver --help` lists every key. The README explains all 24 of them.

Three things worth knowing before your first run:

- **Cell counts divisible by a high power of two are much faster.** `256x128`,
  not `250x130`. The multigrid only coarsens even counts, and the startup line
  tells you how many levels it got.
- **`div` in the step line is the correctness number.** It should stay small and
  stay small.
- **No wake?** Upwind convection adds numerical viscosity of about `u·dx/2`. On a
  coarse grid that can exceed your physical `nu`. Raise `nx`.

---

## What's in it

**Solver.** Chorin projection; staggered MAC grid; first-order upwind convection;
central-difference diffusion; CFL- and diffusion-limited adaptive timestep.

**Pressure.** Geometric multigrid with red/black SOR smoothing, semi-coarsening
for anisotropic grids, and full-multigrid nested iteration for the first solve.
The Poisson operator is exactly `div ∘ grad` face by face, boundary conditions
included, so the projection provably projects. Measured mass error at the outlet:
0.00000%.

**Geometry.** STL and OBJ import, arbitrary slicing plane, in-plane rotation,
mirroring, even–odd rasterisation into the cell mask. `geometryFile=none` gives a
verification circle.

**Speed.** AVX2 hot loops with masked stores, OpenMP row parallelism, and a CUDA
multigrid backend that keeps the pressure field resident on the GPU between
timesteps so each step starts from a free warm guess. All three are optional at
build time; CUDA is also switchable at runtime with `useCuda=0`.

**Output.** Binary legacy VTK: pressure in Pascals, velocity vectors, solid mask.
20 bytes per cell, written straight to the stream with no temporary arrays.

**Interfaces.** Interactive console with a confirm-and-edit loop; a
non-interactive `key=value` command line for sweeps and scripting; and a separate
SFML desktop UI (Windows) that configures runs, launches the solver and renders
the frames.

---

## Known limitations

Worth knowing before you file an issue about them.

- **The section keeps only its largest contour.** A model that cuts into two
  separate shapes loses one, and a model with a hole gets the hole filled. The
  contour pipeline is being reworked.
- **Interactive input is not validated.** A malformed answer — `0,002` instead of
  `0.002` — is silently taken as 0 and confuses every prompt after it. Use the
  command line if you want your mistakes reported. Fixed in the next release.
- **A run that goes unstable is only noticed every 10 steps**, and it keeps
  writing frames until then.
- **`useCuda` is missing from `--help`** and cannot be changed from the
  confirmation screen, though it works on the command line.
- **No restart.** A stopped run starts over. This is the headline feature of the
  next release.
- Upwind convection is first-order, so a coarse grid is more viscous than you
  asked for. The README explains how to tell and what to do.

Windows will show a SmartScreen warning on first run — the installer is not code
signed. macOS packages are not notarised, so right-click → Open the first time.

---

## Next release

**Restart a simulation from a frame you choose.** Every frame becomes a
checkpoint carrying the full solver state, so a run can be continued from any
point, with denser output or a longer clock, producing byte-for-byte what an
uninterrupted run would have. Plus the fixes listed above.

After that: optional gravity, and moving/free-slip walls per object.

---

## Building it yourself

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Three switches, all on by default, all safe to turn off in any combination:
`-DCFD_ENABLE_AVX2=OFF`, `-DCFD_ENABLE_OPENMP=OFF`, `-DCFD_ENABLE_CUDA=OFF`.
CUDA is dropped automatically when no toolkit is found. `CFD_STATIC=ON`, the
default, links the runtime in so the result is one file.

C++17 and CMake 3.28 are the only requirements. `tiny_obj_loader` and
`stl_reader` are vendored.

`scripts/build-linux.sh` and `scripts/build-windows.ps1` build every variant for
their platform in one go.

---

MIT licensed. Issues and pull requests welcome.
