# How to cut a release

From a clean checkout to published assets. Everything here is on this branch
already unless the step says otherwise.

---

## 0. Before anything: three decisions and one correction

### 0.1 Version numbers do not currently agree with themselves

You want releases named `0.1`, `0.2`, `0.3`, `0.4`. The four branches carry
CMake versions `0.1.0`, `0.1.1`, `0.1.2`, `0.1.3`. So the mapping is:

| Release | Branch | CMake `VERSION` |
|---|---|---|
| **0.1** | `develop-math` | 0.1.0 |
| **0.2** | `continue-simulation-update` | 0.1.1 |
| **0.3** | `optional-gravity-update` | 0.1.2 |
| **0.4** | `optional-wall-rotation-update` | 0.1.3 |

That is fine for filenames — the build scripts take the version as a parameter,
so passing `0.1` names everything `Fluid Solver 0.1 …` regardless of CMake. But
it is **not** invisible: `VERSIONINFO` in the `.exe`, the Properties dialog and
the installer's version field all read `PROJECT_VERSION`, so a user who
right-clicks `Fluid Solver.exe` from release 0.1 sees `0.1.0`, and from release
0.4 sees `0.1.3`.

Two ways out, pick one:

- **Make CMake match.** Set `VERSION 0.1.0` / `0.2.0` / `0.3.0` / `0.4.0` on the
  four branches. One line each, and everything agrees everywhere.
- **Leave it.** `0.1` and `0.1.0` are close enough that nobody will file a bug,
  and you renumber at 1.0 anyway.

I'd take the first: it costs one line per branch and removes a question you'd
otherwise have to answer every release.

### 0.2 The README ships as a release asset, and it currently claims things that are not true

You asked for the version's README to sit in the release folder. It's in the
packaging script. But on `develop-math` it still says:

- **"Hole detection ✅"** — false. `Mesh.cpp` keeps only the largest-area contour.
- **"Multiple contour support ✅"** — false, same line of code.
- **"Real-time visualization with SFML"** as a solver feature — false. SFML is
  not linked into this `CMakeLists.txt` at all; the visualisation is the separate
  UI.
- The architecture tree lists a `build/` directory that isn't how it's laid out,
  and omits `AppPaths`, `Multigrid`, `MultigridCuda`.

Fix those before the release, or you're shipping a feature list that the first
user with a two-part model will disprove in ten minutes. The release notes I
wrote already own these under *Known limitations*; the README should match.

### 0.3 The UI needs one flag it didn't need before

The GUI project on `GUI-part_2` looks for a file called `cfd_app.exe` at fixed
paths. We renamed the output to `Fluid Solver.exe`, so that search now fails.
Its CMake already has the escape hatch — pass the path explicitly:

```powershell
cmake -S . -B build -DCFD_SOLVER_EXE="C:\path\to\dist\Fluid Solver 0.1 windows-x64 avx2-omp\Fluid Solver.exe"
```

Two other things about that project, so they don't surprise you mid-release:

- It hard-fails at configure time if it can't find `models/Car.obj` (or `../CAR.obj`).
  Put the file there first.
- It builds SFML from `lib/sfml`, which is the vendored checkout in this repo.
  That's why `lib/sfml` exists even though the solver never links it.

---

## 1. Generate the icons — done, files attached

`logo/` now has, alongside your originals:

```
fluid-solver.ico              16,24,32,48,64,128,256 in one file
fluid-solver-{16..1024}.png   for Linux hicolor
wizard-164x314.bmp            Inno Setup wizard image
wizard-410x797.bmp            the HiDPI one
wizard-small-55x55.bmp        the small wizard image
wizard-small-138x140.bmp      its HiDPI pair
```

The flask is on a dark rounded plate at 84% scale. Your original is a white
outline on transparency, so on any light background — Explorer in light mode, the
Inno wizard, a Finder window — it vanished and only the green bubbles showed. The
plate fixes that, and the padding fixes the other problem: the artwork was
602×1024 inside a 1024 square, so at 16 px it was a 9-pixel sliver. It's legible
at 16 now.

Copy the whole `logo/` folder in. The wizard images **must** stay BMP; Inno
Setup does not read PNG for those.

---

## 2. Wire the icon into the executable

Two files, both attached: `src/app.rc.in` and the `CMakeLists.txt` hunk that
configures it. The resource carries the icon *and* a `VERSIONINFO` block —
product name, version, copyright. That block is what Explorer's Properties
dialog and the SmartScreen prompt read, and its absence is one of the things
that makes an unsigned download look like malware.

Windows only. On Linux the icon belongs to the `.desktop` file, on macOS to the
app bundle; there is nothing to compile in.

---

## 3. Build the binaries

### 3.1 On your Windows machine — 12 of the 30

```powershell
cd C:\...\CFD-Solver-2D
pwsh -File scripts\build-windows.ps1 -Version 0.1
```

Produces `dist\Fluid Solver 0.1 windows-x64 <feature>\` for all 8 x64 rows and
all 4 Win32 rows. It finds `vcomp140.dll` and copies it beside the OpenMP builds
— MSVC has no static OpenMP runtime, so those builds do not start without it.

Add `-SkipCuda` to skip the 4 CUDA rows, `-Generator "Visual Studio 17 2022"` if
you're not on VS2026.

**Set the CUDA architectures.** The default is still `75` — a GTX 1660 Ti and
nothing older. The script passes `-CudaArchs "75;80;86;89;90"`. Build the CUDA
rows against **CUDA 12.x**, not 13: 13 dropped Maxwell, Pascal and Volta, so a
13-built binary won't run on anything before Turing.

### 3.2 On a Linux machine — 12 more

```bash
./scripts/build-linux.sh                 # VERSION=0.1 ./scripts/build-linux.sh
```

Skips CUDA rows without `nvcc` and 32-bit rows without multilib, and says which.
For the full 12: `apt install gcc-multilib g++-multilib` and a CUDA toolkit.

**Build these in a Debian 11 or manylinux2014 container**, not on a current
distro. A statically linked glibc binary still records the kernel version it was
built against, so one built on Ubuntu 24 refuses to start on an older system for
no reason the user can act on.

### 3.3 macOS — the remaining 6

Needs a Mac. Same CMake invocation with `-DCMAKE_OSX_ARCHITECTURES=arm64` or
`x86_64`, and `brew install libomp` for the OpenMP rows.

No CUDA exists for macOS at all, and arm64 has no AVX2, so arm64 has 2 rows and
Intel has 4.

### 3.4 Or let CI do all 30

`.github/workflows/release.yml` is attached — **you have to add this one by
hand**, my tools are not allowed to write into `.github/workflows/`. It builds
the full matrix on GitHub runners and fails the job if the count isn't exactly
30. I could not run it from here, so expect the first run to need a fix or two;
`libomp` on macOS and the CUDA action on Win32 are the likely spots.

### 3.5 What I built and verified here

Linux x86-64, all four CPU rows, fully static (`ldd` → *not a dynamic
executable*), 2.5–2.8 MB stripped, each one runs and writes correct frames.
The other 26 need machines this session can't reach — no CUDA toolkit, no
MinGW, no multilib, and the package repositories are blocked.

---

## 4. Build the UI (Windows only)

```powershell
git checkout GUI-part_2
cmake -S . -B build-ui -G "Visual Studio 18 2026" -A x64 `
      -DCFD_ROOT_DIR="C:\...\CFD-Solver-2D" `
      -DCFD_SOLVER_EXE="C:\...\dist\Fluid Solver 0.1 windows-x64 avx2-omp\Fluid Solver.exe"
cmake --build build-ui --config Release
```

Copy the result into `dist\ui-windows-x64\` — that's where the installer script
looks for it.

---

## 5. Build the installers

### Windows

Install Inno Setup 6, then:

```powershell
iscc /DAppVersion=0.1 /DArch=x64   /DDistDir=..\..\dist installer\windows\fluid-solver.iss
iscc /DAppVersion=0.1 /DArch=x86   /DDistDir=..\..\dist installer\windows\fluid-solver.iss
```

The installer carries every variant for that architecture and puts one on disk as
`Fluid Solver.exe`. It reads the CPU with `IsProcessorFeaturePresent(PF_AVX2)`
and looks for `nvcuda.dll` in System32 to detect the driver, recommends a build
from that, and refuses an AVX2 build on a CPU that would crash on it. Components
for the UI and the example models, tasks for desktop and Start Menu shortcuts,
PATH, and a `.vtk` association.

It installs **per-user by default, without a UAC prompt**. That is deliberate and
it matters: you want `output/` beside the executable, and a standard user cannot
write inside `Program Files`. Per-user keeps that promise. If someone does choose
Program Files, the solver notices the directory is read-only and falls back to
their per-user data directory, printing the path it used.

### Linux

```bash
sudo apt install makeself
# stage what the payload needs
mkdir -p pkg && cp -r dist/"Fluid Solver 0.1 linux-x64"* pkg/
cp README.md LICENSE pkg/ && mkdir -p pkg/icons && cp logo/fluid-solver-*.png pkg/icons/
sed -e 's/__VERSION__/0.1/' -e 's/__ARCH__/x64/' installer/linux/install.sh > pkg/install.sh
chmod +x pkg/install.sh
makeself pkg "dist/Fluid-Solver-0.1-linux-x64.run" "Fluid Solver 0.1" ./install.sh
```

Interactive by default; `--avx2 --openmp --cuda --prefix=… --shortcut --yes` for
unattended. Root installs to `/opt`, anyone else to `~/.local/share`. Writes a
`.desktop` entry, hicolor icons, a `fluid-solver` symlink and an uninstaller that
deliberately leaves `output/` alone.

### macOS

```bash
./installer/macos/build-pkg.sh 0.1 arm64
./installer/macos/build-pkg.sh 0.1 x86_64
```

Must run on a Mac. Uses a Distribution XML so the variants become radio buttons —
a `.dmg` drag-install cannot do component selection.

---

## 6. Assemble the release folder

```bash
./scripts/package-release.sh 0.1
```

Reads `dist/`, writes `release/0.1/`:

```
Fluid Solver 0.1 windows-x64 avx2-omp-cuda.zip     one per variant, each with
Fluid Solver 0.1 linux-x64 avx2-omp.zip            "Fluid Solver", README,
…                                                  LICENSE and an empty output/
Fluid Solver 0.1 windows-x64 setup.exe             the installers
Fluid-Solver-0.1-linux-x64.run
Fluid Solver 0.1 macos-arm64.pkg
Fluid-Solver-Source-Code.zip                       the two standalone files
README.md
SHA256SUMS.txt
```

**Verified end to end here** with the real binaries: the variant zips are laid
out right, and the source archive unzips and builds clean. It excludes `.git`,
build output and `lib/sfml` — 268 KB instead of 100+ MB. Note that this means the
source archive builds the *solver*, not the UI; the UI needs the SFML checkout.

You asked for a `.rar`. The script makes a `.zip` instead, on purpose: RAR needs
proprietary software to create and many people can't open it without installing
something. If you want RAR anyway, swap the `zip -qr9` line for `rar a`.

---

## 7. Publish

```bash
git tag -a v0.1 -m "Fluid Solver 0.1"
git push origin v0.1
gh release create v0.1 release/0.1/* \
   --title "Fluid Solver 0.1" \
   --notes-file release/RELEASE-NOTES-0.1.md
```

`RELEASE-NOTES-0.1.md` is attached and paste-ready: what to download for which
system, a quick start, what's in it, the known limitations stated honestly, and
what's coming next.

Two things to say in the release text rather than let people discover:

- **Windows SmartScreen** will warn on first run. An OV code-signing certificate
  (~$200–400/year) removes it; an EV one removes it immediately rather than after
  building reputation.
- **macOS Gatekeeper** blocks unnotarised packages outright. Without a $99/year
  Apple Developer account, users must right-click → Open. If that's not
  acceptable, drop macOS from 0.1 and add it once the account exists.

Do **not** UPX-compress the binaries. It saves a couple of MB and reliably trips
antivirus heuristics.

---

## 8. Then repeat for 0.2, 0.3, 0.4

The other three branches already have every fix. Only two things change per
release: the version you pass to the build scripts, and the release notes.

Their headline features, for the notes:

- **0.2** — restart from a saved frame. Every frame is a checkpoint; continuing
  from the last frame of a run gives byte-for-byte what an uninterrupted run
  would have produced.
- **0.3** — optional gravity, any direction. Implemented in the reduced pressure,
  so the velocity field is bit-identical with it on or off, which at constant
  density is exactly what the equations say rather than an approximation.
- **0.4** — moving and free-slip walls, per object. Bodies are found and numbered
  automatically; each can spin, drag its surface, or be frictionless.

One caveat when you merge forward: `AppPaths` differs by one line between 0.1 and
the rest — the parameter is `std::string` on 0.1 and `std::filesystem::path` on
the others, because those branches convert the text themselves with
`narrowToPath` and must not have it done twice. **Keep the later version**; the
0.1 call site compiles against it unchanged.

---

## Checklist

- [ ] Decide the CMake version question (§0.1)
- [ ] Fix the three false claims in the README (§0.2)
- [ ] Copy in `logo/`, `src/app.rc.in`, the `CMakeLists.txt` hunk
- [ ] Add `.github/workflows/release.yml` by hand
- [ ] Set the CUDA architecture list, build CUDA rows on CUDA 12.x
- [ ] Build Windows (12), Linux (12), macOS (6)
- [ ] Build the UI with `-DCFD_SOLVER_EXE=…`
- [ ] Build the three installers
- [ ] `./scripts/package-release.sh 0.1`
- [ ] Tag, upload, paste the notes
