# Release guide

How a Fluid Solver release is cut, what each file in it is for, and which
command produces it.

---

## What a finished release contains

```
release/0.2/
  Fluid Solver 0.2 <platform>-<arch> <feature>.zip        34 portable rows
  Fluid Solver 0.2 <platform>-<arch> <feature>-ui.zip     34 the same, with the UI
  Fluid Solver 0.2 windows setup.exe                      the three installers
  Fluid-Solver-0.2-linux.run
  Fluid Solver 0.2 macos.pkg
  Fluid-Solver-Source-Code.zip                            what it was built from
  Fluid-Solver-UI-Source-Code.zip                         the UI repository
  README.md
  SHA256SUMS.txt                                          every file above
```

The 34 rows:

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

AVX2 is an x86 instruction set, so ARM has none. 32-bit CUDA has not existed
since CUDA 9, there is no CUDA toolkit for Windows on ARM, and macOS lost CUDA
in 10.14 — so those columns are empty rather than skipped.

---

## Cutting a release

### 0. Set the version

`CMakeLists.txt` is the only place it lives:

```cmake
project(CFD-Solver-2D VERSION 0.2.0 LANGUAGES CXX)
```

Everything else follows from it. `CFD_RELEASE_VERSION` is derived as
`major.minor` (`0.2`), which is what the archive names, the release folder, the
git tag `v0.2` and the update check all use. `include/Version.hpp.in` turns it
into a header the program reads, so nothing is pasted anywhere by hand.

Bump the minor for a release with new behaviour in it, the patch for a rebuild
that changes nothing a user would notice.

### 1. Build the rows

Everything at once, in CI — this is the normal path:

> **Actions → Build every binary → Run workflow**, version `0.2`
>
> or push a tag `v0.2`, which uses `0.2` as the version and attaches the files
> to that tag's release.

Four runners, one per platform family, and the arm64 rows cross-built beside
their native siblings. The result is `release/0.2/` as an artifact.

By hand, on each machine you own:

```bash
# Linux and macOS
bash scripts/make-release.sh 0.2

# Linux, but built in an old-glibc container so the binaries also start
# on distributions older than the one you are on
bash scripts/make-release.sh 0.2 --docker
```

```powershell
# Windows
pwsh -File scripts\make-release.ps1 -Version 0.2
```

Each script builds what its host can and reports what it could not. Drop the
other platforms' `dist\` output in beside yours and rerun with
`--only=package` / `-Only Package` to assemble one release folder out of all of
them.

### 2. Publish the portable release

At this point `release/0.2/` holds the 34 solver rows, the two source archives,
the README and the checksums. That is a complete, publishable release on its
own — it just has no UI in it yet.

```bash
gh release create v0.2 --title "Fluid Solver 0.2" --notes-file release/RELEASE-NOTES-0.2.md
gh release upload v0.2 release/0.2/*
```

### 3. Add the UI

The UI is a separate repository with its own build matrix. Build it there:

```bash
python3 scripts/build-ui-release.py --version 0.2          # in the UI repo
```

That produces `dist-ui/Fluid Solver 0.2 <platform>-<arch> <feature>-ui.zip` —
the UI executable on its own, not a complete install.

Bring those files over, put them in `release/0.2/` (or anywhere, and point
`--ui-dir` at it), and:

```bash
python3 scripts/merge-ui-release.py 0.2
```

For every solver row it finds a UI build and writes
`Fluid Solver 0.2 <platform>-<arch> <feature>-ui.zip` — the solver archive with
the UI laid into it, the UI's own `README.md` and `BUILD_INFO.md` renamed so
they do not overwrite the solver's, `output/` present, Unix permissions
correct. On Windows it also stamps the project icon onto `Fluid Solver UI.exe`,
which the UI build does not carry one of. Then it rewrites `SHA256SUMS.txt`.

It accepts three shapes of input, whichever is convenient:

| what you have | where to put it |
|---|---|
| the UI repo's `-ui.zip` archives | `release/0.2/`, or `--ui-dir <folder>` |
| a folder per row | `<ui-dir>/<platform>-<arch>/<feature>/` |
| one folder per architecture | `<ui-dir>/<platform>-<arch>/` |

The third one is the usual case: AVX2 and OpenMP change the UI's machine code
and CUDA does not, so 22 UI builds cover all 34 names. A row with no UI build
of its own falls back to the one with the same AVX2 and OpenMP state, and rows
with nothing at all are listed at the end rather than silently missing.

> The UI repository's own `FEATURES` table still lists only x64, x86 and the
> two Macs — the arm64 rows were added to the solver in 0.2 and have no UI
> counterpart yet. Until they do, `merge-ui-release.py` reports
> `windows-arm64` and `linux-arm64` as having no UI build and writes the other
> rows anyway, and the installers offer no UI on ARM. That is a hole to fill in
> the UI repository, not here.

`--check` prints what it would do and writes nothing.

Then upload the new archives:

```bash
gh release upload v0.2 release/0.2/*-ui.zip release/0.2/SHA256SUMS.txt --clobber
```

### 4. Build the installers

The installers are cut **from the published release**, not from a build. That
is deliberate: they have to contain exactly what was published, UI archives
included, and no build produces those.

In CI:

> **Actions → Build the installers → Run workflow**, version `0.2`
>
> Leave `run_id` empty to pull the assets of tag `v0.2`. Tick `attach` to
> upload the finished installers back to that release.

By hand:

```bash
# Linux: unpacks the release into dist/ and cuts the .run
bash scripts/make-release.sh 0.2 --only=installers

# macOS: the same, then the .pkg
bash scripts/make-release.sh 0.2 --only=installers
```

```powershell
# Windows: unpacks the release into dist\ and cuts the setup.exe
pwsh -File scripts\make-release.ps1 -Version 0.2 -Only Installers

# ...and the three single-architecture ones as well, for a smaller download
pwsh -File scripts\make-release.ps1 -Version 0.2 -Only Installers -PerArch
```

Upload them the same way:

```bash
gh release upload v0.2 "release/0.2/Fluid Solver 0.2 windows setup.exe" \
                       "release/0.2/Fluid-Solver-0.2-linux.run" \
                       "release/0.2/Fluid Solver 0.2 macos.pkg" --clobber
```

### 5. Release notes

`release/RELEASE-NOTES-0.2.md`, and it is what `gh release create --notes-file`
publishes. The 0.1 notes were a guide to using the program; from 0.2 on they
are a record of what changed and how the new things work. The README is the
guide.

---

## What runs where

| tool | needed for | platform |
|---|---|---|
| CMake ≥ 3.28 | everything | all |
| MSVC (VS 2022 or newer) | Windows rows | Windows |
| CUDA Toolkit 12.x + VS integration | the CUDA rows | Windows, Linux |
| `gcc-multilib g++-multilib` | linux-x86 rows | Linux |
| `crossbuild-essential-arm64` | linux-arm64 rows | Linux |
| MSVC ARM64 build tools | windows-arm64 rows | Windows |
| Xcode command line tools + Homebrew `libomp` | macOS rows | macOS |
| Inno Setup 6.3 or newer | `setup.exe` | Windows |
| `makeself` | `.run` | Linux |
| `pkgbuild` / `productbuild` | `.pkg` | macOS |
| Python 3 | `merge-ui-release.py`, `unpack-release.py`, `stamp-ui-icon.py` | all |

`make-release.sh` installs the Linux ones itself where it can (`--no-deps`
stops it). CUDA 12.x rather than 13.x is not a typo: CUDA 13 dropped Maxwell,
Pascal and Volta, and building the release rows against 12.x is what keeps
`sm_50`–`sm_70` reachable.

---

## Checks worth doing before publishing

- `SHA256SUMS.txt` covers every file in the folder and nothing that is not
  there. `merge-ui-release.py` and `stamp-ui-icon.py` both rewrite it; the
  release scripts write it last.
- `python3 scripts/merge-ui-release.py 0.2 --check` lists any row that has no
  UI build. An incomplete set is fine to publish on purpose, but not by
  accident.
- `python3 scripts/stamp-ui-icon.py 0.2 --check` reports any `-ui` archive
  whose UI executable has no icon.
- Unzip one portable row per platform and run it. The banner prints the version
  and the feature tag it was actually compiled with — `=== CFD-Solver-2D 0.2
  (avx2-omp-cuda) ===` — so a row published under the wrong name is one line
  away from being caught.
- Run the Windows installer on a machine with no AVX2 if you have one, or at
  least confirm the build it picks matches what the summary page claims.
