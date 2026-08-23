# Release guide

How a Fluid Solver release is cut, what each file in it is for, and which
command produces it.

---

## The two kinds of download, and why there are two

Every release ships the same program twice. They are not two editions - the
binary inside is identical. What differs is who does the work of putting it
where it belongs.

### Portable — the project's files, handed over directly

A portable archive **is** the folder the program runs from. Unzip it anywhere -
a USB stick, a network share, `D:\tools\`, `~/Downloads` - and the executable
inside is ready to run. Nothing is written outside that folder, nothing is
registered with the system, and deleting the folder is the uninstall.

```
Fluid Solver 0.2 windows-x64 avx2-omp-cuda.zip
└── Fluid Solver 0.2 windows-x64 avx2-omp-cuda/
    ├── Fluid Solver.exe        the solver
    ├── vcomp140.dll            the OpenMP runtime, on the OpenMP rows only
    ├── README.md
    ├── LICENSE
    └── output/                 the frames land here
```

The consequence of "the files, directly" is that **you** choose which one to
download. There is one archive per row of the build matrix, and a row names
exactly what is compiled into it:

- the platform and architecture — `windows-x64`, `linux-arm64`, `macos-x64`, …
- the accelerators — `avx2`, `omp`, `cuda`, in that order, or `plain` for none

`Fluid Solver 0.2 linux-x64 avx2-omp-cuda.zip` is the fastest thing this
project produces on a 64-bit Linux box with a recent Intel or AMD CPU and an
NVIDIA card. `Fluid Solver 0.2 linux-x64 plain.zip` runs on anything with that
architecture and is the one to fall back to. Downloading the wrong one is the
price of the format: an AVX2 build on a CPU without AVX2 does not warn, it dies
with an illegal instruction on the first step.

A `-ui` archive is the same thing with the desktop UI in the folder beside the
solver. It replaces the plain archive rather than accompanying it — the UI is a
shell that launches the solver, so the two have to live in one directory.

### Installers — the same files, put in place for you

An installer is a program whose job is to make those choices and do that
copying. There are three, one per platform, and each carries **every**
architecture and **every** accelerator variant that was built:

```
Fluid Solver 0.2 windows setup.exe    x64 + x86 + arm64
Fluid-Solver-0.2-linux.run            x64 + x86 + arm64
Fluid Solver 0.2 macos.pkg            arm64 + x64
```

Running one of them:

1. reads the machine — architecture, AVX2, core count, NVIDIA driver;
2. picks the fastest variant that machine can actually execute, and drops back
   a switch at a time if that exact combination was not built;
3. copies it to a real install location under the plain name
   `Fluid Solver.exe` / `Fluid Solver`, so shortcuts and scripts never have to
   know which variant it is;
4. offers the desktop UI as a component, and the shortcuts as tick boxes;
5. leaves an uninstaller behind.

Nothing about the accelerators is asked. Windows has a "Let me pick AVX2,
OpenMP and CUDA myself" option on the same page for people who want it, and
Linux has `--choose`; both are opt-in.

**So:** portable is for "I want the files and I will manage them myself" —
no install, no registry, no admin, several machines off one stick. Installers
are for "just put it on this computer" — one download whatever the hardware,
shortcuts, file associations, an entry in Apps & Features.

Both are published for every release. Neither is the "real" one.

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
  README.md                                               how to use the program
  RELEASE-GUIDE.md                                        this file
  RELEASE-NOTES-0.2.md                                    what changed in it
  SHA256SUMS.txt                                          every file above
```

The last three are copied in by the packaging step, so the folder explains
itself: what the program is, what changed, and how the folder was produced.
`RELEASE-NOTES-<version>.md` is only copied when one exists for that version.

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
git tag `v0.2` and the update check all use. `include/VersionGenerated.hpp.in`
is configured into `VersionGenerated.hpp` at build time and
`include/Version.hpp` includes it, so nothing is pasted anywhere by hand.
(`Version.hpp` also carries fallbacks for every macro, which is what keeps an
IDE that has not run CMake yet from underlining them all in red.)

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

### 2. Build the UI rows

The UI is a separate repository with its own matrix, and it does not know this
one exists. Build it at the same time as step 1 — neither waits for the other:

> in the UI repository: **Actions → Build every UI binary → Run workflow**,
> version `0.2`
>
> or push a tag `ui-v0.2`, which builds the same matrix and publishes the
> archives to that tag's release.

By hand instead:

```bash
python3 scripts/build-ui-release.py --version 0.2          # in the UI repo
```

Either way the result is `dist-ui/Fluid Solver 0.2 <platform>-<arch>
<feature>-ui.zip` — **the UI executable on its own, not a complete install.**
34 of them, the same 34 names the solver produces.

**The version is not decoration.** It is the key `merge-ui-release.py` pairs the
two halves by: a UI archive that says `0.1` and a release that says `0.2` do not
pair, and the merge stops with "no UI builds found" rather than quietly writing
34 archives with no UI in them. So build the UI with the same version as the
solver. `python3 scripts/build-ui-release.py --list | wc -l` should print `34`
before you start; anything else means the two repositories have drifted.

(The version also goes into the executable's own Windows version block, which is
what Explorer's Properties dialog and the SmartScreen prompt read. An unsigned
download with no product name and no version is one of the things that makes
SmartScreen shout the loudest.)

### 3. Merge the UI into the solver rows

This is the step that used to be thirty manual unzip-drop-rezip cycles.
Bring the UI archives over, put them in a folder of **their own**, and:

```bash
# in the solver repository. release/0.2/ holds the 34 solver rows.
python3 scripts/merge-ui-release.py 0.2 --ui-dir dist-ui --check   # always first
python3 scripts/merge-ui-release.py 0.2 --ui-dir dist-ui
```

For every solver row it finds a UI build and writes
`Fluid Solver 0.2 <platform>-<arch> <feature>-ui.zip` — the solver archive with
the UI laid into it, the UI's own `README.md` and `BUILD_INFO.md` renamed so
they do not overwrite the solver's, `output/` present, Unix permissions written
explicitly (a zip built on Windows carries no permission bits at all, so without
this the Linux rows unpack into a UI that will not run). On Windows it also
stamps the project icon onto `Fluid Solver UI.exe`, which the UI build does not
carry one of. Then it rewrites `SHA256SUMS.txt`.

It accepts three shapes of input, whichever is convenient:

| what you have | where to put it |
|---|---|
| the UI repo's `-ui.zip` archives | `--ui-dir <folder>` |
| a folder per row | `<ui-dir>/<platform>-<arch>/<feature>/` |
| one folder per architecture | `<ui-dir>/<platform>-<arch>/` |

The third one is the usual case: AVX2 and OpenMP change the UI's machine code
and CUDA does not, so 22 UI builds cover all 34 names. A row with no UI build of
its own falls back to the one with the same AVX2 and OpenMP state, and rows with
nothing at all are listed at the end rather than silently missing.

**Use `--ui-dir`, not the release folder.** The UI archives are named exactly
what the merged archives are named. Merging in place does work — the solver
files are written last, so re-running picks up a rebuilt solver rather than
keeping a stale one — but a merge that dies halfway has then overwritten the
UI-only archives, and they have to be rebuilt.

`release/0.2/` now holds 68 archives: 34 solver, 34 solver-plus-UI.

### 4. Publish the portable release

```bash
gh release create v0.2 --title "Fluid Solver 0.2" \
   --notes-file release/RELEASE-NOTES-0.2.md
gh release upload v0.2 release/0.2/*
```

> **The other order.** Steps 2-4 can be run as publish-then-add instead: put the
> 34 solver rows up as soon as step 1 finishes, and upload the `-ui.zip`
> archives with `--clobber` when the UI is ready. That gets something
> downloadable out earlier, at the cost of a window where the release exists
> with no UI in it and `SHA256SUMS.txt` covers half of what is there. Pick one;
> do not half-do both.

### 5. Build the installers

The installers are cut **from the published release**, not from a build. That is
deliberate: they have to contain exactly what was published, UI archives
included, and no build produces those. So step 4 has to have happened, `-ui`
archives and all — an installer built before them comes out with no UI
component, and the workflow says so with a warning rather than an error, because
building without the UI on purpose is a real thing to want.

In CI:

> **Actions → Build the installers → Run workflow**, version `0.2`
>
> Leave `run_id` empty to pull the assets of tag `v0.2`.
>
> `installer_tag` is where the finished installers go. Leave it empty to publish
> nothing and just collect the artifact; set it to a tag of its own —
> `v0.2-installers` — to keep the installers as a separate release; set it to
> `v0.2` to put them alongside the portable files. The tag is created if it does
> not exist.

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

### 6. Publish the installers

As their own release, which is what `installer_tag` does for you in CI. By hand,
or from a downloaded artifact:

```bash
gh release create v0.2-installers --title "Fluid Solver 0.2 installers" \
   --notes "Installers for Fluid Solver 0.2. The portable archives are in the v0.2 release."
gh release upload v0.2-installers \
   "release/0.2/Fluid Solver 0.2 windows setup.exe" \
   "release/0.2/Fluid-Solver-0.2-linux.run" \
   "release/0.2/Fluid Solver 0.2 macos.pkg" --clobber
```

Two releases per version is a deliberate split, not an accident: the portable
one is 68 files and changes whenever a row is rebuilt, the installer one is
three files cut from it. Say in each one's notes where the other is, because
nothing on the GitHub releases page does that for you.

### Where the release notes come from

`release/RELEASE-NOTES-0.2.md`, written before step 4, because that is what
`gh release create --notes-file` publishes. The 0.1 notes were a guide to using
the program; from 0.2 on they are a record of what changed and how the new
things work. The README is the guide.

Both packaging scripts copy it, and `RELEASE-GUIDE.md`, into `release/<version>/`
so the folder explains itself later.

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

## Signing

Signing is wired into the release scripts. Set the variables below and every
build signs itself; leave them unset and every build says on one line that it
did not, and carries on. Nothing has to be run by hand either way.

| script | signs | with |
|---|---|---|
| `scripts/sign-windows.ps1` | every `Fluid Solver.exe` and every `setup.exe` | `signtool` |
| `scripts/sign-macos.sh` | every Mach-O binary, then the `.pkg`, then notarises it | `codesign`, `productsign`, `notarytool`, `stapler` |

`make-release.ps1` calls the first after each row and after each installer.
`installer/macos/build-pkg.sh` calls the second at the three points Apple
requires, in the only order it accepts: binaries → package → notarise → staple.

### Windows

One certificate, from any CA that sells code signing (DigiCert, Sectigo,
SSL.com, GlobalSign). Two grades, and the difference is what the first few
hundred downloads look like:

- **standard (OV)** — the SmartScreen prompt still appears at first and says
  your name instead of "Unknown publisher". It stops appearing once the
  signature has accumulated enough reputation.
- **EV** — no prompt from the first download. Since June 2023 CAs must keep
  the key on a hardware token or an HSM, so an EV certificate cannot be
  exported to a file and cannot be used from a hosted CI runner. Sign those
  locally.

```powershell
# EV or hardware token: the certificate is already in the machine's store
$env:CFD_SIGN_THUMBPRINT = "A1B2C3..."        # certmgr.msc → Details → Thumbprint

# ...or a .pfx file
$env:CFD_SIGN_PFX = "C:\certs\fluid-solver.pfx"
$env:CFD_SIGN_PFX_PASSWORD = "..."

# optional; the default is DigiCert's
$env:CFD_SIGN_TIMESTAMP_URL = "http://timestamp.digicert.com"

pwsh -File scripts\make-release.ps1 -Version 0.2 -WithInstallers
```

Timestamping is not optional in practice: without it every signature stops
validating the day the certificate expires, so a release published today would
start warning users the moment the certificate lapses. With it the signature
outlives the certificate.

### macOS

An Apple Developer Program membership, and **two** certificates from it —
Apple issues them separately and they are not interchangeable. A `.pkg` signed
with the Application certificate is rejected outright.

```bash
export CFD_SIGN_MACOS_APP_IDENTITY="Developer ID Application: NAME (TEAMID)"
export CFD_SIGN_MACOS_INSTALLER_IDENTITY="Developer ID Installer: NAME (TEAMID)"

# notarisation - separate from signing, and the half that actually removes the
# "Apple cannot check it for malicious software" dialog on 10.15 and later
xcrun notarytool store-credentials cfd-notary \
      --apple-id you@example.com --team-id TEAMID \
      --password <app-specific-password>
export CFD_NOTARY_PROFILE=cfd-notary

bash scripts/make-release.sh 0.2 --only=installers
```

`security find-identity -v` prints the two identity strings exactly as they
must be typed. The app-specific password comes from appleid.apple.com →
Sign-In and Security → App-Specific Passwords; it is not the account password.

In CI, where there is no keychain to store a profile in, the three parts go in
separately instead: `CFD_NOTARY_APPLE_ID`, `CFD_NOTARY_TEAM_ID`,
`CFD_NOTARY_PASSWORD`.

### In GitHub Actions

**Actions → Build the installers** reads these repository secrets. Every one of
them is optional; the workflow goes green without any of them and produces
unsigned installers.

| secret | for |
|---|---|
| `CFD_SIGN_PFX_BASE64` | the Windows `.pfx`, base64 (`certutil -encode cert.pfx cert.txt`) |
| `CFD_SIGN_PFX_PASSWORD` | its password |
| `CFD_SIGN_TIMESTAMP_URL` | optional override |
| `CFD_MACOS_CERTS_BASE64` | a `.p12` holding **both** Developer ID certificates with their keys, base64 |
| `CFD_MACOS_CERTS_PASSWORD` | its password |
| `CFD_SIGN_MACOS_APP_IDENTITY` | `Developer ID Application: NAME (TEAMID)` |
| `CFD_SIGN_MACOS_INSTALLER_IDENTITY` | `Developer ID Installer: NAME (TEAMID)` |
| `CFD_NOTARY_APPLE_ID` / `CFD_NOTARY_TEAM_ID` / `CFD_NOTARY_PASSWORD` | notarisation |

The macOS job imports the certificates into a throwaway keychain it creates,
unlocks with a password it invents, and deletes at the end of the job; the
Windows job writes the `.pfx` to `RUNNER_TEMP` and removes it the same way.
Neither ever lands in the workspace, so no artifact upload can pick one up.

### Making it mandatory

For the run that actually publishes, an unsigned release should be a failure
rather than a note:

```bash
CFD_SIGN_REQUIRE=1 bash scripts/make-release.sh 0.2 --only=installers
```

```powershell
pwsh -File scripts\sign-windows.ps1 -Require "release\0.2\*setup.exe"
```

A certificate that **is** configured and then fails to sign is always an error,
with or without that switch — half a release signed is worse than none of it.

### With nothing configured

Which is a legitimate way to publish, and what 0.1 did:

- **Windows** — SmartScreen shows "Windows protected your PC" on a download
  with no reputation. More → Run anyway.
- **macOS** — Gatekeeper refuses the `.pkg` on any machine but the one that
  built it. Right-click → Open, then confirm.

Say so in the release notes rather than letting people find out.

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
