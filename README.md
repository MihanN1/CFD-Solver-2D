# CFD Mask UI Optimized

This package builds the GUI independently of the CFD solver source tree.
`CFD-Solver-2D-main` is not a build dependency and must not be modified for GUI work.

## Requirements

- CMake 3.28 or newer.
- Visual Studio 2022 or newer with **Desktop development with C++**.
- Windows SDK.

On Linux: `build-essential`, `libx11-dev`, `libxrandr-dev`, `libxcursor-dev`,
`libxi-dev`, `libudev-dev`, `libgl1-mesa-dev`. On macOS: the Xcode command line
tools, plus `brew install libomp` for the OpenMP rows.

The package includes the GUI's direct source dependencies:

- `third_party/tinyobjloader/tiny_obj_loader.h`
- `third_party/sfml/` (SFML source)

Therefore `CFD_ROOT_DIR` is no longer required and the first configure does not
need a solver checkout. With the release-default static dependency policy, SFML
may download its pinned FreeType/HarfBuzz/SheenBidi sources on a clean build.

## Build with CMake GUI

1. Extract the ZIP.
2. Open **CMake GUI**.
3. **Where is the source code:** select the extracted `Source` folder.
4. **Where to build the binaries:** select a separate sibling folder, for example
   `CFD-Mask-UI-Optimized_build`.
5. If you previously configured an older package in that build folder, use
   **File -> Delete Cache** first.
6. Press **Configure**.
7. Generator: **Visual Studio 17 2022** (or newer); platform: **x64**.
8. Leave `CFD_SOLVER_EXE` empty unless you explicitly want CMake to copy a built
   `Fluid Solver.exe` beside the GUI.
9. Press **Configure** again until there are no red unresolved entries.
10. Press **Generate**.
11. Press **Open Project** and build `cfd_mask_ui_optimized` in **Release | x64**.

Typical executable location for a Visual Studio multi-config build:

```text
<build-folder>/Release/Fluid Solver UI.exe
```

`CFD_UI_STATIC_RUNTIME=ON` is the default. On Windows this selects the static
MSVC CRT (`/MT`). SFML and its bundled FreeType/HarfBuzz/SheenBidi dependencies
are also built static. Linux additionally links `libgcc`/`libstdc++` statically
when GCC is used; X11/OpenGL/OS libraries remain dynamic because they are system
interfaces. macOS system runtimes/frameworks remain dynamic by platform design.


## Release matrix

The UI now has its own release builder and GitHub Actions workflow:

```text
scripts/build-ui-release.py
.github/workflows/build-ui-all.yml
```

The matrix matches the finished Fluid Solver 0.1 binary matrix exactly: 30 UI
archives across Windows/Linux/macOS, x64/x86/arm64, AVX2/non-AVX2, and the same
OpenMP/CUDA feature-name combinations. Every archive name is the matching solver
archive name with `-ui` appended before `.zip`, for example:

```text
Fluid Solver 0.1 windows-x64 cuda-ui.zip
Fluid Solver 0.1 windows-x64 avx2-omp-cuda-ui.zip
Fluid Solver 0.1 macos-arm64 plain-ui.zip
```

List the exact 30 names without building:

```text
python scripts/build-ui-release.py --version 0.1 --list
```

Build all Windows rows locally:

```text
python scripts/build-ui-release.py --version 0.1 --arch x64 --arch x86
```

On Linux the same command builds x64/x86; the 32-bit development libraries must
be installed. On macOS, CI builds arm64 and x64 on native runners. The workflow
merges all four native-platform jobs, verifies that all 30 expected archives are
present, creates `Fluid-Solver-UI-Source-Code.zip`, and writes `SHA256SUMS.txt`.

AVX2 and OpenMP are both real compile-time switches for this UI: AVX2 vectorises
the byte swap that decodes a VTK frame, OpenMP spreads that decode and the colour
map across every core. CUDA is not, and deliberately so - decoding a frame is a
read plus a byte swap, and the colour map has to land in host memory for the
texture upload anyway, so a round trip to the GPU would cost more in transfers
than the arithmetic is worth. The `-cuda` suffix therefore selects a name and not
a build, which is what lets 22 builds cover the 30 archive names the solver
release pairs against.

A row whose name promises OpenMP is configured with
`CFD_UI_ENABLE_OPENMP_EXPLICIT=ON`, so a build machine without an OpenMP runtime
fails the row instead of quietly publishing a single-threaded binary under a name
that says otherwise.

On Windows an OpenMP row needs `vcomp140.dll` beside it, because MSVC has no
static OpenMP runtime. The solver's own `omp` archives already ship that DLL, and
a `-ui` archive is that archive plus the UI binary, so the pairing supplies it.

Each binary archive contains one top-level folder with the same stem,
`Fluid Solver UI[.exe]`, `README-UI.md`, `BUILD_INFO-UI.md`, and an empty
`output/` folder. The documentation is renamed on the way in on purpose: these
archives are merged by hand into the solver's `-ui` archive, which has a
`README.md` and a `LICENSE` of its own, and a plain copy would replace them.

**To assemble a `-ui` release archive:** take the solver's archive for that row,
and add `Fluid Solver UI[.exe]` from the UI archive of the same name. Nothing
else from the UI archive is needed - the binary is fully static apart from the
system graphics stack.

## Solver integration

The solver is optional at GUI build time. At runtime the UI looks for the solver
beside its own executable first - `Fluid Solver.exe` on Windows, `Fluid Solver`
with no extension on Linux and macOS - and **Select solver** points it somewhere
else. Selecting or replacing the solver does not require rebuilding the UI.

For a fresh UI with no saved preference, the output root is `output` **beside the
UI executable**, which is where the solver writes its own frames and where the
installers create the folder. It is not the process working directory: a Start
Menu shortcut, a desktop icon or a drag-and-drop each hand the process some other
directory, and frames used to land wherever that happened to be. A saved
`ui-preferences.txt` or a loaded `.cfdui` configuration still overrides it.

What goes on the command line depends on what the selected executable
understands, because a solver exits on the first argument it does not know.
`restart`, `restartFile` and `addTime` need 0.1.1 or newer; `avx2`, `openmp`,
`threads` and `tray` need 0.2. Everything added after 0.2 was published has no
version number to separate it, so the UI looks for the key name in the
executable's own parameter table instead and leaves the whole block off when it
is not there:

| Block | Found by |
|---|---|
| `gravityEnabled` `gravityAccel` `gravityAngle` | `gravityEnabled` |
| `wallMotion` | `wallMotion` |
| `bodyMotion` `bodyCoupling` | `bodyMotion` |
| `profiles` | `profiles` |
| `extraFields` | `extraFields` |
| `convection` `limiter` `timeScheme` `gravityMode` | `timeScheme` |
| `bcLeft` … `inletProfile` | `bcBottom` |
| `caseType` `lidSpeed` `steadyTolerance` | `caseType` |
| `phases` `rho1` `nu1` `rho2` `nu2` `phaseInit` … `sources` | `vofScheme` |
| `mixing` `diffusivity` `surfaceTension` `contactAngle` | `surfaceTension` |
| `turbulence` `Cs` `turbIntensity` `turbLengthScale` | `turbLengthScale` |

`bc<Side>Speed` is only written when that side is a `movingWall` or the speed is
not zero. Writing `bcLeftSpeed=0` for an inlet would tell the solver a standstill
was asked for, and an inlet meant to run at `U0` would come out stopped.

A continuation sends no `wallMotion` and no `profiles` while those two boxes are
empty, so whatever the frame was run with survives.

**Case** is a preset rather than another parameter. `cavity` makes the solver
write all four sides itself - three walls and a lid sliding at **Lid speed** -
so the UI leaves the whole BOUNDARIES block off the command line when it is
picked. Sending both would run the preset and then overwrite it with whatever
the boundary rows happened to say, which is not what picking a preset means.
`channel` sends the rows exactly as before.

**Stop when steady** is `steadyTolerance`: the run ends early once the largest
velocity change per second, measured against whatever drives the case, drops
under it. Zero, the default, runs the whole of **Total time**.

=== BODIES ===

A body selector and a set of rows that edit whichever body it is pointing at,
rather than one row per body per setting - a table of eight settings across
however many bodies the mask happens to have does not fit on a screen, and the
number is not known until the mask is generated.

    BODIES   Body               which one the rows below are about
             Behaviour          static | drag | slip | travel | free
             Surface spin       deg/s      \
             Surface slide X    m/s         > drag: the surface moves, the
             Surface slide Y    m/s        /  body does not
             Body velocity X    m/s        \
             Body velocity Y    m/s         > travel: the body itself moves.
             Body spin          deg/s      /  Under free, what it starts with
             Body mass          kg/m       \
             Body density       kg/m3       > free only
             Pinned             which degrees of freedom are held
             Body motion        text, the solver's grammar
             Coupling           weak | added | strong
             Collisions         off, bodies pass through each other
             Bounciness         how much of the closing speed survives
             Report forces      work the force out for set paths too

The two text rows are the truth and the rows above them are a way of writing
into one entry of each - exactly as the brush is a way of writing into the
sources row. Pick a body and the rows read that body's entry back out of the
text; change a row and it writes that entry back. Editing the text by hand does
the same thing, and `@<seconds>` keyframes can only be written there, because a
timetable is not a slider.

**Collisions** is off by default and that is deliberate: turning it on changes
the answer, and every run written before this branch had bodies passing
through each other. On, a body that would run into another one or into the
domain edge bounces instead, and **Bounciness** is how much of the closing
speed comes back - 0 stops dead, 1 rebounds at the speed it arrived.

**Report forces** works the fluid force out for bodies whose path you set as
well. It never changes where they go; a set path is a set path. It only puts
the force in the step line so you can read what the fluid was doing to the
body - useful just before you release it, and for a drag coefficient. Free
bodies always have it computed, because that is the thing that moves them.

=== TURBULENCE ===

    TURBULENCE  Turbulence model        none | smagorinsky | kOmegaSST
                Smagorinsky Cs          the one constant smagorinsky has
                Turbulence intensity    fraction of the inlet speed
                Turbulence length       m, the biggest eddy coming in

Four rows, and three of them are read by one model each. `Cs` is only sent for
`smagorinsky`; the two inlet rows are only sent for `kOmegaSST`; `none` sends
the model name and nothing else, so a run with the model off puts exactly one
extra key on the command line and changes nothing about what the solver does.

The whole group is held back on finding `turbLengthScale` in the executable,
like every block since 0.2. Point it at a 0.7 solver and none of these four
rows reach the command line.

Refused before the run rather than after: a `Cs` of zero or above one, an
intensity above one - an inlet more turbulent than it is moving - and a
negative length scale.

`nuT`, `wallDistance` and `strain` are `extraFields` like any other and show up
in the **Field** button once the solver has been asked to write them. `k` and
`omega` arrive on their own in every frame of a `kOmegaSST` run, because the
solver needs them back to continue.

One trap the UI cannot fix for you: **object numbers come from the mask, not
from the order the models are listed in.** The flood fill walks the grid in
scan order, so of two shapes side by side either can end up as number 1. The
solver prints the mapping with the mesh, before anything runs, and that is what
the Body row is pointing at.

**Behaviour** is the one row that decides which of the two strings an entry
goes into. `drag` and `slip` are `wallMotion` - the surface moves and the body
stays put. `travel` and `free` are `bodyMotion` - the body itself goes
somewhere, and the solver cuts its outline again every step.

Held back on finding `bodyMotion` in the executable, like every block since
0.2. Point it at a 0.6 solver and the whole group is left off the command line.

The validator refuses a body told to travel through an empty domain, because
moving a body means cutting its outline again and an empty domain has no
outline to cut. Better a sentence now than a run that starts, prints a refusal
of its own and then sits perfectly still for ten minutes.

**Phases** turns the FLUIDS group into two fluids with their own densities and
viscosities, and the setup view into something you can paint on. `phases=1`
sends none of it.

**Surface tension** (in mN/m, because that is how everybody quotes it - water
against air is 72) and **Contact angle** are the last two rows of the FLUIDS
group, held back on the solver having the keys at all - a 0.5 solver is asked
exactly what it was asked before. Zero tension is off. The angle is measured
inside fluid 1 at a wall: under 90 it wets the wall and climbs, over 90 it
beads off.

**Mixing** decides whether there is a surface at all. `immiscible` is oil and
water and is what every run so far did. `miscible` is ink and water: no
interface, the interface scheme is not read, the composition spreads by
**Diffusivity** instead - and a surface tension on top of that is refused
rather than quietly ignored, because there is nothing for it to pull on.

Surface tension is not free in wall clock either. The step size it forces is
`sqrt((rho1+rho2)*d^3/(4*pi*sigma))`, it usually binds well before the CFL
number does, and it falls as `d^1.5`, so halving the cell size costs about
three times the steps. The solver says so on the first step rather than
appearing to hang.

**Paint** replaces the 3D preview with the solver's own grid, one pixel per
cell, and paints the initial volume fraction straight onto it. Left button
lays down fluid 1, right button takes it back to fluid 2, the wheel resizes the
brush, and Fill, Clear and Undo do what they say. What is painted is written as
`initial-phase.txt` beside the run and passed as `initialPhaseFile`, so the
folder holding the frames also holds the thing they started from - and a
painted field overrides whichever of layer, drop and column the Start shape row
happens to be showing, because the point of painting one is that it is none of
those.

The brush also places **sources**, and it does it by writing a line in the
solver's own grammar into the Flow sources row rather than inventing a second
way of saying the same thing. Drop one, then edit the row for rate and angle.

Fluid 1 and Fluid 2 are which of the two the left button lays down; the right
button always lays down the other, so a stroke can be taken back without
reaching for anything.

Changing nx or ny throws the painting away rather than stretching it into
something nobody drew.

**Run simulation** is no longer greyed out without a model. That was the last
place where "the profile is optional" was not actually true: an empty domain is
a case in its own right and a painted phase field is a whole initial condition,
and the button stayed dead through both.

In the results view the `phase` field gets a colour map of its own, fixed to
0..1 rather than to whatever turned up in this frame: a domain of pure water
should look like pure water and not like half of it. Everything else the solver
writes still goes through the general ramp.

When nothing has been drawn, the UI now sends `geometryFile=empty` instead of a
section adapter with no contours in it. An empty adapter used to make the solver
fall back to its verification circle, which is how a lid driven cavity ended up
with a cylinder sitting in the middle of it.

## Result fields

A frame carries pressure, the solid mask and velocity. Anything else the solver
was asked to write - `vorticity`, `divergence`, `speed`, `objectId`, `phase`,
`nuT`, `k`, `omega`, `wallDistance`, `strain` - is read into a registry keyed by
the name the frame used, and the **Field** button walks whatever turned up and
back round to pressure. Nothing in the UI has a list of which fields exist, so a
field the solver learns to write later shows up without this project changing.

Where a scalar sits in the file is the writer's business, and it took a while to
admit it. The reader used to refuse any array it did not recognise until it had
seen all three of pressure, mask and velocity - and the solver writes the phase
fraction between the pressure and the mask, with `k` and `omega` beside it. So
every two-fluid frame ever written was rejected on the way in with *Unknown
scalar array before required arrays: phase*. The order check is gone; a frame
that never provides the three is still rejected, at the end of the parse where
that can actually be known.

The three that were always there stay named members rather than map entries:
they are read on every pixel of every redraw and have no business going through
a hash lookup to get there.

### The preview is on the solver's grid, not on a better one

The UI cuts the section itself, writes it out as `section-adapter.obj`, and
then compares the mask the solver rasterised out of that file against the one
it drew for the preview. They used to disagree, by three cells out of 2500, on
a cube.

The solver keeps its grid spacing in `float`: `dx = float(Lx)/nx`. The preview
computed it in `double`. For `Lx = 1, nx = 50` that is 0.019999999552965164
against 0.02, and by `i = 30` the two disagree about where the cell centre is
by a part in 10^8.

That is nothing at all right up until a face of the model lands on a cell
boundary, which for anything axis aligned it does on purpose. Then the four
corner cells of the rectangle sit at exactly one cell circumradius from the
outline - the distance at which the rasteriser decides a cell is on the
boundary - and a part in 10^8 is the whole difference between a solid cell and
an empty one.

So the preview now divides in `float` too. Being more accurate than the thing
you are previewing is not an improvement.

The answer this produces is not symmetrical: `float(1)/50` is a hair UNDER
0.02, so a cell centre drifts towards the origin as its index grows, and the
far corners land just inside the circumradius while the near ones land just
outside. Three corners solid, one not. It looks like a bug and it is the
solver's own answer, which is the only answer a preview is allowed to have.
GeometryProcessorTests pins all four.

## Frame loading

A VTK frame is read in one go and decoded from memory. The legacy binary payload
is big endian, so every 32-bit word is swapped in place - eight at a time through
one AVX2 shuffle where that is compiled in - rather than pulled through the
stream a value at a time, which is what the reader used to do. On a 600x300 grid
that is 24.8 ms down to 2.6 ms per frame.

Frames also decode on several threads at once, one per spare core up to eight.
There used to be a single loader, and a request while it was busy simply waited,
which is why flipping quickly past the few cached steps stalled on every one. The
decoded-frame cache holds up to 256 frames within its byte budget instead of 16,
so an ordinary series ends up entirely resident after one pass.

The colour map that turns a frame into the displayed texture runs across cores
too, and its buffer is kept between frames rather than reallocated per step.

## Tests

Enable `BUILD_TESTING` in CMake GUI if wanted, then build the test targets and run
CTest. `SolverCompatibilityTests` is added only when `CFD_SOLVER_EXE` points to an
existing solver executable.

## Parameter panel

The setup parameter panel is one scrollable list. It contains all current
solver-facing numerical controls rather than hiding the multigrid/timestep
controls on a separate Basic/Advanced page.

- Mouse wheel over the left parameter panel scrolls the controls.
- The visible scrollbar can be dragged or clicked.
- Mouse wheel over the 3D preview still controls preview zoom.
- A row is a number, a choice or a line of text. The panel is one list and
  everything about it works by index, so a dropdown and a text box are kinds of
  row rather than separate widgets: the layout, the scrolling, the group
  headers and the `.cfdui` file did not have to learn they exist.
- A choice row is dragged or clicked across its options like any other slider,
  and double-clicking its value lets the name be typed instead.
- A text row holds the solver's own grammar and is handed over untouched. The
  UI only refuses a line break; what a line means is the solver's opinion, and
  having two opinions about the same string is how they end up disagreeing.
- The WALLS group is one such text row, so every body can be addressed
  individually - `1:rot=90,slideX=0.5;2:slip=1` - rather than one setting being
  applied to all of them.
- GEOMETRY has a second text row for `profiles`, which places several models at
  once, and BOUNDARIES has a choice row per side under a **Case** preset.
- `outputDir`, `geometryFile`, transformed slice arguments, and `invertSection`
  are generated from the GUI workflow rather than exposed as ordinary sliders.


## Current UI revision — 2026-08-21

This source revision is UI-only. It does not modify the Fluid Solver numerical
code or either finished solver distribution.

Implemented UI behavior:

- warns before launching when the slice contains multiple disconnected contours;
  the user may cancel or explicitly reduce the run to the largest contour, and
  the preview/adapter are updated to match that choice;
- identifies the selected `Fluid Solver.exe` and displays its version/build;
- distinguishes CPU-only and CUDA-capable solver builds and disables CUDA requests
  when the selected solver cannot provide CUDA;
- renames the process-stop action to **Stop simulation** and confirms before
  closing the application while a simulation is active;
- keeps all numerical controls visible in one scrollable panel, grouped by
  physical, geometry, grid, timestep, multigrid, output, and backend purpose;
- provides integer +/- adjustment, inline invalid-field highlighting, and
  parameter help text;
- displays derived grid/runtime information including `dx`, `dy`, cell count,
  Reynolds number, approximate timestep, approximate VTK count, and estimated
  multigrid levels;
- persists the output-root preference and supports Save/Load of `.cfdui`
  configuration files;
- writes UI-owned run metadata with solver identity and requested parameters;
- exposes a configurable decoded-VTK cache budget;
- shows `u`, `v`, speed, and pressure for result inspection;
- adds result-frame keyboard navigation, playback, series/current-frame range
  selection, and a Run details viewer;
- retains VTK restart parsing infrastructure but does not expose unsupported
  restart/continuation launch arguments.

Current fresh-run CLI compatibility remains the 24-key Fluid Solver contract.
Gravity and continuation keys are not emitted.

## Validation status of this source revision

Performed in the available Linux validation environment:

- Full Linux Release link of `Fluid Solver UI`, with and without AVX2/OpenMP.
- `FluidSolverRunTests`, `GeometryProcessorTests`, `VtkFrameTests`: passed.
- Reader equivalence: the same data written as BINARY and as ASCII decodes to
  bit-identical pressure, solid, velocity, speed, finite masks and ranges,
  including frames carrying NaN and infinity.
- Reader robustness: truncated payload, truncated header, empty file and a solid
  value outside {0,1} all raise `VtkParseError` rather than misbehaving.
- Ranges and speeds checked against independently computed reference values.
- Solver discovery: the extension-less solver is found beside the UI, a
  non-executable one is refused with a message that says so, and a configured
  path is used when nothing sits beside the UI.
- Generated 30-name release matrix compared exactly against the solver
  `release/0.1` archive set: 30/30, no missing or extra rows.
- Linux x64 release rows built and packaged end to end through
  `scripts/build-ui-release.py`.

Not performed here:

- Windows and macOS builds;
- GUI launch and interactive visual verification (the container has no display).
