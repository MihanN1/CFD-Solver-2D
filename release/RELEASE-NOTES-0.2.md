# Fluid Solver 0.2

The release 0.1 promised: **a run can be continued from any frame**. Around it,
the things 0.1 could not do — choose its own accelerators and remember the
choice, say when it is out of date, report progress somewhere other than a
console nobody is looking at, and cut a body that is not one single closed
shape.

The desktop UI is in this release. So are ARM64 builds, one installer per
platform instead of one per architecture, and code signing.

These notes are a record of **what changed**. The README is the guide to using
the program; `RELEASE-GUIDE.md`, in this folder, is the guide to cutting a
release.

---

## Contents

- [If you are upgrading from 0.1](#if-you-are-upgrading-from-01)
- [New: continue a run from any frame](#new-continue-a-run-from-any-frame)
- [New: accelerators you can switch, and that are remembered](#new-accelerators-you-can-switch-and-that-are-remembered)
- [New: the tray, a progress bar, and a clean stop](#new-the-tray-a-progress-bar-and-a-clean-stop)
- [New: it tells you when there is a newer release](#new-it-tells-you-when-there-is-a-newer-release)
- [New: more than one contour](#new-more-than-one-contour)
- [New in the UI](#new-in-the-ui)
- [Packaging: ARM64, one installer per platform, signing](#packaging-arm64-one-installer-per-platform-signing)
- [Fixed](#fixed)
- [Changed, and it may affect you](#changed-and-it-may-affect-you)
- [Every file that changed](#every-file-that-changed)
- [Known limitations](#known-limitations)
- [Building it yourself](#building-it-yourself)

---

## If you are upgrading from 0.1

Nothing to migrate. Unpack the new archive, or run the installer over the old
install.

Three things to know:

1. **0.1 frames cannot be continued.** Restart data is written into every frame
   from 0.2 on; a 0.1 frame does not carry it. 0.2 still *reads* 0.1 frames
   perfectly well, so old series open in the UI and in ParaView as before.
2. **A `settings.ini` appears** next to the executable the first time you change
   an accelerator setting, or in your user data directory when the install
   folder is read-only. Delete it to go back to defaults.
3. **The Windows installer no longer offers to pin to the taskbar.** It never
   worked reliably; the last page now tells you to do it yourself, which takes
   one right-click. Same on Linux and macOS.

---

## New: continue a run from any frame

This is what 0.1's notes called the headline feature of the next release, and
it is here.

Every frame 0.2 writes carries the full solver state — the face velocities, the
raw pressure and the configuration text — in a `FIELD RestartData` block inside
the VTK file. Any frame is therefore a checkpoint, and a run can be picked up
from it:

```
Fluid Solver restart=1 restartFile=output/solution_120.vtk addTime=5
```

- `restart=1` turns it on.
- `restartFile=` is the frame to continue from.
- `addTime=` extends the clock by that many seconds **past where the frame
  stopped**. Leave it out to run to the original `totalTime`.

Anything else you pass is applied on top of the frame's own configuration, so
the same command can continue a run *and* change `saveInterval`, `nu`,
`mgTolerance` or the output directory. What it does not need is the model file:
the solid mask travels in the frame, so a continuation works even if the `.obj`
is long gone or was never on this machine.

New frames are numbered from the one they started at — continuing from
`solution_33.vtk` writes `solution_33_34.vtk`, `solution_33_35.vtk` and so on —
so a continuation never overwrites the run it came from, and it is obvious
afterwards which frames came from where.

The size cost is about 1.6× per frame — 32 bytes per cell instead of 20, the
extra being the two face-velocity arrays and the raw pressure. That is the whole
state, and it is what makes any frame a checkpoint rather than only the last
one.

---

## New: accelerators you can switch, and that are remembered

In 0.1, which accelerators a build used was fixed when it was compiled: to run
without AVX2 you downloaded a different archive. In 0.2 a build still decides
what it *can* do at compile time, but what it *does* is yours, per machine, and
the answer sticks.

```
Fluid Solver --accel          # or --settings; the same screen
```

opens a short questionnaire — AVX2, OpenMP (and its thread count), CUDA, the
update check, the tray icon — and writes the answers to `settings.ini`. Every
later run reads them. Or set them for one run only:

```
Fluid Solver avx2=0 openmp=1 threads=4 tray=0 nx=256 ny=128 totalTime=5
```

`settings.ini` sits beside the executable when that folder can be written to —
which is what a portable unpack gives you — and in your per-user data directory
otherwise, which is what an install under `Program Files` gives you. It is a
plain `key=value` file:

```ini
useAvx2=1
useOpenMP=1
useCuda=1
threads=0
checkForUpdates=1
tray=1
```

There are environment overrides too, for scripts and CI, which beat the file
without changing it: `FLUID_SOLVER_NO_AVX2`, `FLUID_SOLVER_NO_OPENMP`,
`FLUID_SOLVER_NO_CUDA`, `FLUID_SOLVER_NO_TRAY`,
`FLUID_SOLVER_NO_UPDATE_CHECK`.

A switch for something this build has no code for is kept in the file rather
than dropped, so moving `settings.ini` to a richer build does not silently lose
the choice — but in this build it does nothing, and the screen says so.

**Does turning one off change the answer?** It changes how long the run takes,
not what is being solved. The AVX2 kernels and the scalar tail evaluate the same
expressions, OpenMP only splits the same loops across cores, and the CUDA
backend solves the same system. What does move is the last few digits: velocity
agrees to float rounding, and pressure — which the multigrid stops refining once
the residual is under `mgTolerance` — lands on a slightly different iterate,
about a thousandth of its peak at the default tolerance. That is the same
difference the separate `avx2` and `plain` rows have always had between them.

Also new: `--version` (or `-v`) prints the version and the exact feature set the
binary was compiled with, so a row published under the wrong name is one command
away from being caught.

---

## New: the tray, a progress bar, and a clean stop

A long run used to report only into a console. Now:

**On Windows** the solver puts a real tray icon up for the duration of the run.
Its tooltip says how much of the simulated time is done —
`Fluid Solver - 120 / 500 s (24%)` — and the taskbar button fills up with the
same fraction. Right-clicking it gives:

- **Hide to the tray** / **Show the console** — send the console window away
  and bring it back, so a run can keep going without a window in the way.
- **Open the output folder** — opens the directory the frames are going to.
- **Stop after the current step (saves a frame)** — see below.

**Everywhere else** there is no tray a static console binary can reach without
dragging in a desktop toolkit, so the same numbers go into the terminal's title,
which is what the taskbar or the Dock shows for that window.

**Stopping cleanly** is the part worth knowing. Both the tray menu item and
Ctrl+C now ask the run to *finish the step it is on*, write a frame, and return
normally. A killed process leaves the last frame half written and no restart
data in it; a clean stop leaves a frame you can continue from. Press Ctrl+C a
second time and it dies immediately, as it always did.

`tray=0`, or `FLUID_SOLVER_NO_TRAY=1`, turns the icon off and leaves the console
output exactly as it was.

---

## New: it tells you when there is a newer release

On start the solver asks GitHub for the release list, compares tags numerically
(so 0.10 is correctly newer than 0.9, which a string compare gets wrong), skips
drafts, and if something newer exists asks once whether to open the releases
page. Answer no and it gets on with the run.

It is one HTTPS request — WinHTTP on Windows, `curl` then `wget` on everything
else — and a failure is silent: no network, a proxy in the way, or GitHub being
down never delays or breaks a run.

Turn it off permanently in `--accel`, per run with `FLUID_SOLVER_NO_UPDATE_CHECK=1`,
or ask for it on demand with `--check-updates`.

---

## New: more than one contour

0.1's known limitations said:

> **The section keeps only its largest contour.** A model that cuts into two
> separate shapes loses one, and a model with a hole gets the hole filled.

Fixed. The section plane's intersection with the model is now collected as
*every* closed loop it produces, not just the biggest, and the even-odd fill
runs across the whole set at once. So:

- two aerofoils side by side stay two aerofoils;
- a ring, a duct or a body with a hole through it comes out hollow, because a
  loop drawn inside another loop toggles twice;
- all the loops share one bounding box for scaling, so their relative positions
  and sizes are preserved rather than each being fitted to the domain
  separately.

Loops smaller than 1/10000 of the largest by area are dropped — those are the
slivers a plane grazing a surface produces, not geometry.

The startup banner now reports what it found: `Number of section contours = 2
(16 points)`.

---

## New in the UI

The desktop UI ships with this release. Against the version that existed before
it:

**Tray and progress.** The same tray icon and taskbar progress bar as the
solver, for the UI's own window: the window can be hidden to the tray and
brought back, and the tray menu can stop the running simulation. Inside the
window, a progress bar sits above the status strip during a run, filled by how
much of the simulated time is done — read out of the solver's own output — and
sliding as a marquee while that is not yet known.

**Continue a run from the UI.** Select a frame, press **Continue run**, and the
UI starts a continuation from it. The button is only enabled when the frame
actually carries restart data *and* the solver it is pointed at is new enough to
accept it. There is a new **Add time** field for how much further to run.

**It knows what your solver can do.** The UI reads the solver executable and
finds the version and feature strings 0.2 embeds in it — no need to run it — and
holds back every argument that version does not understand. A 0.1 solver exits
on the first argument it does not recognise, so this is what lets one UI drive
both.

**New fields.** `Add time`, `AVX2`, `OpenMP` and `Threads` join the parameter
list, which is now grouped into eight sections including OUTPUT, ACCELERATION
and UI.

**Colour scales that survive one bad cell.** Pressure and velocity now carry a
trimmed range as well as a full one — the outermost half-percent at each end
left out — and the range control cycles through four states (this frame or the
whole series, trimmed or full). A scale stretched by a handful of cells used to
map everything else onto three shades.

**Several contours.** When the solver is 0.2 or newer, the UI stops reducing a
multi-contour section to its largest loop and passes the whole thing through.
Against an older solver it still reduces, and still asks first.

**The section plane stays inside its panel.** The setup preview is clipped to
its own view, so a plane dragged past the edge no longer draws over the rest of
the window.

---

## Packaging: ARM64, one installer per platform, signing

**ARM64.** `windows-arm64` and `linux-arm64` join the matrix, built the same way
as every other row — cross-compiled beside their native siblings in the same CI
job. That takes the release from 30 portable rows to **34**:

| platform      | AVX2 | OpenMP | CUDA | rows |
|---------------|------|--------|------|------|
| windows-x64   | ×    | ×      | ×    | 8    |
| windows-x86   | ×    | ×      | —    | 4    |
| windows-arm64 | —    | ×      | —    | 2    |
| linux-x64     | ×    | ×      | ×    | 8    |
| linux-x86     | ×    | ×      | —    | 4    |
| linux-arm64   | —    | ×      | —    | 2    |
| macos-arm64   | —    | ×      | —    | 2    |
| macos-x64     | ×    | ×      | —    | 4    |

AVX2 is an x86 instruction set, so ARM has none; there is no CUDA toolkit for
Windows on ARM, no 32-bit CUDA since CUDA 9, and no CUDA on macOS since 10.14.

**One installer per platform.** 0.1 planned six installers, one per platform and
architecture. 0.2 ships three, each carrying every architecture and every
variant that was built:

```
Fluid Solver 0.2 windows setup.exe    x64 + x86 + arm64
Fluid-Solver-0.2-linux.run            x64 + x86 + arm64
Fluid Solver 0.2 macos.pkg            arm64 + x64
```

One download whatever is inside the machine, and nobody picks the wrong file.
Each one reads the machine — architecture, AVX2 support, core count, NVIDIA
driver — picks the fastest variant it can actually execute, and drops back one
switch at a time if that exact combination was not built. Nothing about the
accelerators is asked; Windows has a "let me pick them myself" option on the
same page and Linux has `--choose`, both opt-in. The installed executable is
always called `Fluid Solver`, so shortcuts and scripts never have to know which
variant is underneath.

The single-architecture Windows installers are still available with
`make-release.ps1 -PerArch`, for a smaller download.

**Signing.** 0.1 shipped unsigned and said so. 0.2 signs itself when a
certificate is configured, and says on one line when it is not:

- Windows — `scripts/sign-windows.ps1` signs every executable and every
  installer with `signtool`, timestamped so the signature outlives the
  certificate. Set `CFD_SIGN_THUMBPRINT` (a certificate in the machine store,
  which is what a hardware or EV certificate needs) or `CFD_SIGN_PFX` +
  `CFD_SIGN_PFX_PASSWORD`.
- macOS — `scripts/sign-macos.sh` signs every Mach-O binary with the hardened
  runtime, signs the `.pkg` with the Installer certificate, then submits it to
  Apple and staples the ticket. Set `CFD_SIGN_MACOS_APP_IDENTITY`,
  `CFD_SIGN_MACOS_INSTALLER_IDENTITY` and either `CFD_NOTARY_PROFILE` or the
  Apple ID / team / app-specific-password trio.

`CFD_SIGN_REQUIRE=1` turns "not configured" into a build failure, which is what
the run that actually publishes should use. A certificate that *is* configured
and then fails to sign is always an error — half a release signed is worse than
none of it. The whole procedure, including the GitHub secrets, is in
`RELEASE-GUIDE.md`.

**Assembling the release.** `scripts/merge-ui-release.py` takes the UI builds
and lays each into the matching solver archive, producing the `-ui.zip` rows
without anyone repacking 34 archives by hand. It accepts the UI repo's own
archives, a folder per row, or one folder per architecture — the last is the
usual case, because AVX2 and OpenMP change the UI's machine code and CUDA does
not, so 22 UI builds cover all 34 names. A row with no UI build of its own falls
back to the one with the same AVX2/OpenMP state, and rows with nothing at all
are listed rather than silently missing. It renames the UI's `README.md` and
`BUILD_INFO.md` so they do not overwrite the solver's, fixes Unix permissions,
stamps the project icon onto `Fluid Solver UI.exe`, and rewrites
`SHA256SUMS.txt`. `--check` prints what it would do and writes nothing.

**`RELEASE-GUIDE.md`** is new, and ships inside the release folder: what
portable and installer each mean and why there are both, what every file in the
release is, and the five steps that produce them.

---

## Fixed

**Enormous pressures in the last frame of a run.** The worst bug in 0.1, and the
reason a UI-run simulation looked wrong. `currentTime` is a double accumulating
float-sized steps, so after a few hundred steps it lands a fraction of a
nanosecond short of `totalTime`. The loop then took one more step of that
fraction — and the projection's right-hand side is `ρ·div(u*)/Δt`, so dividing
the leftover divergence by a Δt of 2e-10 inflated it into pressures of order
1e13. Because the UI reads the *last* frame to set the colour scale for the
whole series, that one frame made every other frame look flat and wrong.

Now the loop stops when less than a thousandth of a step remains, splits the
last step in two when what is left is between one and one and a half steps
(rather than leaving a sliver behind), and snaps `currentTime` exactly onto
`totalTime` at the end. Verified across `totalTime` from 0.005 to 10: every run
lands exactly on its target with physically sensible pressure.

**The UI's runner did nothing on Linux and macOS.** `ChildProcess::start` was
implemented for Windows only. It now has a full `fork`/`exec` path with the
solver's output captured to `solver-output.txt` and `solver-error.txt`,
non-blocking status polling through `waitpid`, and a stop that sends `SIGINT`,
waits five seconds and only then sends `SIGKILL` — which is what lets the
solver's own clean-stop handler write a final frame. On Windows the stop now
sends `CTRL_BREAK_EVENT` first for the same reason, and only falls back to
`TerminateProcess`.

**The UI refused a perfectly good solver** because it insisted the file be named
exactly `Fluid Solver.exe`. On Linux and macOS the same program is called
`Fluid Solver`, with no extension. Both names are accepted now.

**CUDA was silently skipped in CI.** The Windows job added
`.../CUDA/v13.2/bin` to `PATH` while the step above it installed 12.6.3, so
`nvcc` was never on `PATH` and every CUDA row quietly fell back. The path now
matches the version that is actually installed.

**Every `CFD_*` macro underlined as an error** in an editor that had not run
CMake yet. The generated header is now `VersionGenerated.hpp`, and a committed
`include/Version.hpp` includes it when it exists and supplies stand-ins for
every macro when it does not. The program compiles either way, and IntelliSense
stops complaining.

**Icons.** The window icon is set explicitly at startup (`WM_SETICON`, both
sizes), the tray icons load at the size the shell actually asks for, the
uninstaller entry has one, both Start Menu and desktop shortcuts name theirs
explicitly rather than relying on the target, and `.vtk` files associated with
the solver get the solver's icon. On macOS an `.icns` is built from the PNG set
and dropped into every `.app` wrapper the installer creates, because a Mach-O
executable carries no icon of its own.

---

## Changed, and it may affect you

- **No taskbar-pin option in the installers.** Pinning from an installer means
  writing into a shell data structure Microsoft has never supported writing to,
  and it silently stopped working. The final page now says to right-click the
  Start Menu entry and choose *Pin to taskbar*. Linux and macOS say the
  equivalent. The Linux uninstaller still removes a GNOME favourite that 0.1
  left behind.
- **Frames are about 1.6× larger** (32 bytes per cell, up from 20), because
  they carry the restart state.
- **Six installers became three.** If you linked to
  `Fluid Solver 0.1 windows-x64 setup.exe`, the 0.2 equivalent is
  `Fluid Solver 0.2 windows setup.exe`.
- **`Version.hpp.in` is now `VersionGenerated.hpp.in`.** Only matters if you
  build from source with your own scripts.
- **CMake 3.28** is the minimum, as in 0.1.

---

## Every file that changed

### Solver

| file | what happened |
|---|---|
| `CMakeLists.txt` | 0.2; ARM64 and OpenMP/CUDA options; version header generation; icon resource; `Fluid Solver` output name; WinHTTP/shell32/ole32/user32 |
| `include/Version.hpp` | **new** — includes the generated header, or stands in for it |
| `include/VersionGenerated.hpp.in` | renamed from `Version.hpp.in`; adds the feature list and the two embedded markers |
| `include/Runtime.hpp`, `src/Runtime.cpp` | **new** — the accelerator settings, `settings.ini`, CPU/GPU detection, env overrides |
| `include/UpdateCheck.hpp`, `src/UpdateCheck.cpp` | **new** — the release check, numeric version comparison, opening the page |
| `include/Progress.hpp`, `src/Progress.cpp` | **new** — tray icon, taskbar progress, terminal title, clean stop |
| `src/main.cpp` | version banner; `--version`, `--accel`/`--settings`, `--check-updates`; runtime switches on the command line; embedded markers |
| `src/Solver.cpp` | the timing fix; progress reporting; stop handling; AVX2 loops behind the runtime switch |
| `src/Multigrid.cpp` | AVX2 loops behind the runtime switch |
| `include/Mesh.hpp`, `src/Mesh.cpp` | every closed loop instead of the largest; even-odd fill across the whole set; shared bounding box; sliver rejection |
| `installer/windows/fluid-solver.iss` | one installer for x64 + x86 + arm64; automatic-or-manual variant choice; no taskbar task; explicit icons |
| `installer/linux/install.sh` | one `.run` for all architectures; `--arch=`, `--choose`, `--auto`; pinning removed |
| `installer/macos/build-pkg.sh` | one `.pkg` for both architectures; Dock choice removed; signing and notarisation |
| `scripts/sign-windows.ps1` | **new** |
| `scripts/sign-macos.sh` | **new** |
| `scripts/merge-ui-release.py` | **new** |
| `scripts/make-release.sh`, `scripts/make-release.ps1` | 0.2; ARM64 rows; combined installers; signing; the guide and notes copied into the release folder |
| `scripts/build-linux.sh`, `scripts/build-windows.ps1` | 0.2; ARM64 rows; `--skip-arm` |
| `.github/workflows/build-all.yml` | 34 rows; ARM64 cross-toolchain; the CUDA `PATH` fix |
| `.github/workflows/build-installers.yml` | one installer per platform; signing secrets and a throwaway keychain |
| `RELEASE-GUIDE.md` | **new** |
| `README.md` | the executable is `Fluid Solver`; acceleration, `settings.ini`, the tray, the update check |

### UI

| file | what happened |
|---|---|
| `include/TrayIcon.hpp`, `src/TrayIcon.cpp` | **new** — tray icon, menu, taskbar progress, command queue |
| `src/Application.cpp` | POSIX `fork`/`exec` runner; clean stop on both platforms; solver capability detection; continuation; the in-window progress bar; the new parameters; trimmed colour ranges; multi-contour; the clipped setup preview; the window icon |
| `include/FluidSolverRun.hpp`, `src/FluidSolverRun.cpp` | restart, `addTime` and the runtime switches on the solver's command line, each held back unless the solver understands it |
| `include/VtkFrame.hpp`, `src/VtkFrame.cpp` | trimmed pressure and velocity ranges, per frame and per series |
| `tests/ContinuationTests.cpp` | **new** — what a continuation puts on the command line, and what it refuses to |
| `CMakeLists.txt` | 0.2; the tray source; platform libraries; the new test |
| `scripts/build-ui-release.py` | ARM64 rows; 34 archives; 0.2 |
| `.github/workflows/build-ui-all.yml` | ARM64 jobs and the aarch64 cross-toolchain |

---

## Known limitations

- **The UI has no ARM64 build yet.** The four ARM64 solver rows are real and
  complete, but there is no UI counterpart, so `merge-ui-release.py` reports
  `windows-arm64` and `linux-arm64` as having no UI and the installers offer no
  UI component on ARM.
- **0.1 frames cannot be continued** — they carry no restart state. They still
  open and animate normally.
- **A run that goes unstable is still only noticed every 10 steps**, and it keeps
  writing frames until then.
- **Upwind convection is first-order**, so a coarse grid is more viscous than you
  asked for. The README explains how to tell and what to do.
- **If this release was published without a certificate configured**, Windows
  will show SmartScreen's "Windows protected your PC" on first run
  (More → Run anyway) and macOS will want a right-click → Open on the package.
  `SHA256SUMS.txt` is what to check against in the meantime.

---

## Building it yourself

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Three switches, all on by default, all safe to turn off in any combination:
`-DCFD_ENABLE_AVX2=OFF`, `-DCFD_ENABLE_OPENMP=OFF`, `-DCFD_ENABLE_CUDA=OFF`.
CUDA is dropped automatically when no toolkit is found. `CFD_STATIC=ON`, the
default, links in what can be linked in.

C++17 and CMake 3.28 are the only requirements. `tiny_obj_loader` and
`stl_reader` are vendored.

The whole 34-row matrix, zipped and checksummed:

```
pwsh -File scripts\make-release.ps1 -Version 0.2
bash scripts/make-release.sh 0.2
```

Rows the machine cannot produce — no CUDA toolkit, no 32-bit multilib, no
aarch64 cross-compiler — are reported and skipped, not failed; `--skip-arm`
leaves the ARM rows out on purpose. `--docker` builds the Linux rows in an
old-glibc container so they also run on distributions older than the one that
built them. `RELEASE-GUIDE.md` has the whole procedure.

---

MIT licensed. Issues and pull requests welcome.
