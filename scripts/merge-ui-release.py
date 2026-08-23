#!/usr/bin/env python3
"""Build the "-ui" half of a release out of the UI builds, without doing it by hand.

The portable release has two archives for every row: the solver on its own, and
the same solver with the desktop UI beside it. The second one used to be
assembled by hand - unzip a solver row, unzip a UI build, drop one into the
other, zip it back up, thirty times, and then redo the checksums. This does
that.

    # everything: release/<ver>/ holds the solver rows AND the UI builds
    python3 scripts/merge-ui-release.py 0.2

    # the UI builds live somewhere else
    python3 scripts/merge-ui-release.py 0.2 --ui-dir dist-ui

    # say what would happen and write nothing
    python3 scripts/merge-ui-release.py 0.2 --check

What counts as a UI build, in the order they are looked for:

  1. "Fluid Solver <ver> <platform>-<arch> <feature>-ui.zip"
     - what scripts/build-ui-release.py in the UI repository produces, and what
       an earlier run of this script produced. Either is fine: the solver files
       are written last, so re-merging an already-merged archive picks up a
       rebuilt solver rather than keeping the old one.
  2. "<ui-dir>/<platform>-<arch>/<feature>/..." - a folder per row.
  3. "<ui-dir>/<platform>-<arch>/..." - one folder per architecture, used for
     every feature of it. The UI's own build script does exactly this: AVX2 and
     OpenMP change its machine code and CUDA does not, so one binary serves the
     "-cuda" row and the row without it.

A row with no UI build of its own falls back to one with the same AVX2 and
OpenMP state - which is what makes 22 UI builds cover 34 archive names - and
is reported and skipped if there is not even that.

The result for each row is the solver archive with the UI laid into it: the
UI's own README.md and BUILD_INFO.md come across renamed, because the solver's
documentation is already in there under those names, and everything the solver
archive holds wins over anything of the same name in the UI build. Unix
permissions are written explicitly, so an archive that came off Windows - which
carries no permission bits at all - still unpacks into something that runs.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def project_version(default: str = "0.0") -> str:
    """major.minor from CMakeLists.txt, so no version is typed twice."""
    cmake = ROOT / "CMakeLists.txt"
    try:
        text = cmake.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return default
    match = re.search(r"project\s*\([^)]*?VERSION\s+([0-9]+)\.([0-9]+)", text)
    return f"{match.group(1)}.{match.group(2)}" if match else default


# Same shape as scripts/unpack-release.py matches, and for the same reason:
# GitHub rewrites spaces to dots in release asset names, so an archive that came
# back from a tag is dotted rather than spaced.
NAME = re.compile(
    r"^Fluid[ .]Solver[ .](?P<ver>[0-9][0-9A-Za-z.]*?)[ .]"
    r"(?P<platform>windows|linux|macos)-(?P<arch>x64|x86|arm64)[ .]"
    r"(?P<feature>[a-z0-9-]+?)(?P<ui>-ui)?$"
)

SOLVER_NAMES = {"Fluid Solver", "Fluid Solver.exe"}
UI_NAMES = {"Fluid Solver UI", "Fluid Solver UI.exe"}
EXECUTABLES = SOLVER_NAMES | UI_NAMES

# The UI repository ships these two; the solver archive already has files of
# those names and they are not the same documents.
RENAME_IN_UI = {"README.md": "README-UI.md", "BUILD_INFO.md": "BUILD_INFO-UI.md"}
# The licence is the same project's, so the copy already in the solver archive
# is the one that stays.
DROP_FROM_UI = {"LICENSE", "LICENSE.txt"}


def parse(stem: str):
    match = NAME.match(stem)
    if not match:
        return None
    info = match.groupdict()
    info["ui"] = bool(info["ui"])
    return info


def row_name(info, ui: bool) -> str:
    """The spaced form, which is what everything downstream indexes by."""
    return (f"Fluid Solver {info['ver']} {info['platform']}-{info['arch']} "
            f"{info['feature']}" + ("-ui" if ui else ""))


def feature_key(feature: str):
    """What actually decides the UI binary: AVX2 and OpenMP, never CUDA.

    A frame is a read plus a byte swap and the colour map has to land in host
    memory for the texture upload anyway, so the UI has no CUDA path to build
    two of. One binary therefore serves "avx2-omp-cuda" and "avx2-omp" alike.
    """
    parts = feature.split("-")
    return ("avx2" in parts, "omp" in parts)


# --------------------------------------------------------------- reading ---

def read_zip(path: Path) -> dict:
    """{relative path -> (bytes, mode)}, with the single top folder stripped."""
    entries = {}
    with zipfile.ZipFile(path) as archive:
        roots = {Path(name).parts[0] for name in archive.namelist() if name.strip()}
        if len(roots) != 1:
            raise SystemExit(f"{path.name}: expected one folder inside, found {sorted(roots)}")
        root = roots.pop()
        for item in archive.infolist():
            relative = Path(item.filename)
            if relative.parts[0] != root:
                continue
            inner = "/".join(relative.parts[1:])
            if not inner:
                continue
            mode = (item.external_attr >> 16) & 0o7777
            if item.is_dir():
                entries[inner.rstrip("/") + "/"] = (b"", mode or 0o755)
                continue
            if not mode:
                # A zip written on Windows carries DOS attributes and nothing
                # else, so the mode has to be invented rather than read - and
                # 0 would unpack as 0000.
                mode = 0o755 if relative.name in EXECUTABLES else 0o644
            entries[inner] = (archive.read(item), mode)
    return entries


def read_dir(path: Path) -> dict:
    entries = {}
    for item in sorted(path.rglob("*")):
        inner = item.relative_to(path).as_posix()
        if item.is_dir():
            entries[inner + "/"] = (b"", 0o755)
        elif item.is_file():
            mode = 0o755 if item.name in EXECUTABLES else 0o644
            entries[inner] = (item.read_bytes(), mode)
    return entries


def looks_like_ui(entries: dict) -> bool:
    return any(Path(name).name in UI_NAMES for name in entries)


# --------------------------------------------------------------- writing ---

def write_zip(path: Path, top: str, entries: dict) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        # Directory entries are written explicitly rather than left implicit:
        # output/ is empty and has to survive the round trip, and every
        # unpacker in this project reads the top folder's name off the archive.
        directories = {top + "/"}
        for name in entries:
            parts = name.rstrip("/").split("/")
            branch = parts if name.endswith("/") else parts[:-1]
            for depth in range(1, len(branch) + 1):
                directories.add(f"{top}/" + "/".join(branch[:depth]) + "/")
        for name in sorted(directories):
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = (stat.S_IFDIR | 0o755) << 16
            info.compress_type = zipfile.ZIP_STORED
            archive.writestr(info, b"")
        for name in sorted(n for n in entries if not n.endswith("/")):
            payload, mode = entries[name]
            info = zipfile.ZipInfo(f"{top}/{name}")
            # create_system 3 is Unix, which is what makes the mode below
            # survive the round trip at all.
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, payload, compresslevel=9)


def merge(solver: dict, ui: dict) -> dict:
    merged = {}
    for name, value in ui.items():
        base = Path(name).name
        if base in DROP_FROM_UI:
            continue
        if base in RENAME_IN_UI:
            name = "/".join(Path(name).parts[:-1] + (RENAME_IN_UI[base],))
        merged[name] = value
    # Written second on purpose: the solver archive is the authority on every
    # name it carries, so re-merging an archive that already holds an older
    # solver replaces it rather than keeping it.
    merged.update(solver)
    merged.setdefault("output/", (b"", 0o755))
    return merged


# ------------------------------------------------------------- gathering ---

def collect_ui_sources(release: Path, ui_dir: Path | None, version: str) -> dict:
    """{(platform, arch, feature) -> (label, entries)} for everything usable."""
    sources: dict = {}

    def note(key, label, entries):
        if looks_like_ui(entries):
            sources.setdefault(key, (label, entries))

    for folder in filter(None, [release, ui_dir]):
        if not folder.is_dir():
            continue
        for archive in sorted(folder.glob("*.zip")):
            info = parse(archive.stem)
            if info is None or not info["ui"] or info["ver"] != version:
                continue
            note((info["platform"], info["arch"], info["feature"]),
                 archive.name, read_zip(archive))

    if ui_dir and ui_dir.is_dir():
        # "<ui-dir>/<platform>-<arch>[/<feature>]/..."
        for entry in sorted(ui_dir.iterdir()):
            if not entry.is_dir():
                continue
            match = re.fullmatch(r"(windows|linux|macos)-(x64|x86|arm64)", entry.name)
            if not match:
                continue
            platform, arch = match.group(1), match.group(2)
            children = [c for c in sorted(entry.iterdir()) if c.is_dir()]
            feature_dirs = [c for c in children
                            if re.fullmatch(r"[a-z0-9-]+", c.name) and c.name != "output"]
            if feature_dirs and not any(f.name in UI_NAMES for f in entry.iterdir()):
                for feature_dir in feature_dirs:
                    note((platform, arch, feature_dir.name),
                         f"{entry.name}/{feature_dir.name}", read_dir(feature_dir))
            else:
                # One folder for the whole architecture: it answers for every
                # feature, and the fallback below picks it up.
                note((platform, arch, "*"), entry.name, read_dir(entry))
    return sources


def pick_ui(sources: dict, platform: str, arch: str, feature: str):
    exact = sources.get((platform, arch, feature))
    if exact:
        return exact
    wanted = feature_key(feature)
    for (p, a, f), value in sources.items():
        if p == platform and a == arch and f != "*" and feature_key(f) == wanted:
            return value
    return sources.get((platform, arch, "*"))


def refresh_checksums(release: Path) -> None:
    sums = release / "SHA256SUMS.txt"
    rows = []
    for path in sorted(p for p in release.iterdir()
                       if p.is_file() and p.name != sums.name):
        rows.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    sums.write_text("".join(rows), encoding="utf-8")


def stamp_icons(release: Path, version: str) -> bool:
    """Put the project icon on the UI executables. Windows only, by nature."""
    script = ROOT / "scripts" / "stamp-ui-icon.py"
    if not script.is_file():
        return False
    result = subprocess.run(
        [sys.executable, str(script), version, "--release", str(release)],
        check=False)
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Assemble the '-ui' release archives from the UI builds.")
    parser.add_argument("version", nargs="?", default=project_version())
    parser.add_argument("--release", help="folder holding the solver archives "
                                          "(default release/<version>)")
    parser.add_argument("--ui-dir", help="where the UI builds are, if not in "
                                         "the release folder")
    parser.add_argument("--check", action="store_true",
                        help="report what would be built and write nothing")
    parser.add_argument("--no-stamp", action="store_true",
                        help="skip putting the icon on the UI executable")
    arguments = parser.parse_args()

    release = Path(arguments.release) if arguments.release \
        else ROOT / "release" / arguments.version
    ui_dir = Path(arguments.ui_dir) if arguments.ui_dir else None

    if not release.is_dir():
        print(f"no such folder: {release}", file=sys.stderr)
        return 1

    solver_rows = {}
    for archive in sorted(release.glob("*.zip")):
        info = parse(archive.stem)
        if info is None or info["ui"] or info["ver"] != arguments.version:
            continue
        solver_rows[(info["platform"], info["arch"], info["feature"])] = archive

    if not solver_rows:
        print(f"no solver archives for {arguments.version} in {release}",
              file=sys.stderr)
        return 1

    sources = collect_ui_sources(release, ui_dir, arguments.version)
    if not sources:
        print("no UI builds found. Put them in " + str(release) +
              ", or point --ui-dir at them.", file=sys.stderr)
        return 1

    print(f"{len(solver_rows)} solver row(s), {len(sources)} UI build(s)")

    written, skipped = 0, []
    for key in sorted(solver_rows):
        platform, arch, feature = key
        chosen = pick_ui(sources, platform, arch, feature)
        target = release / (row_name({"ver": arguments.version,
                                      "platform": platform, "arch": arch,
                                      "feature": feature}, True) + ".zip")
        if chosen is None:
            skipped.append(f"{platform}-{arch} {feature}")
            continue
        label, ui_entries = chosen
        if arguments.check:
            print(f"  {target.name}  <-  {label}")
            written += 1
            continue

        merged = merge(read_zip(solver_rows[key]), ui_entries)
        write_zip(target, target.stem, merged)
        print(f"  {target.name}  <-  {label}")
        written += 1

    if skipped:
        print(f"\n{len(skipped)} row(s) have no UI build:")
        for line in skipped:
            print("  " + line)

    if arguments.check:
        print(f"\nwould write {written} archive(s)")
        return 0 if written else 1

    if written and not arguments.no_stamp:
        print()
        stamp_icons(release, arguments.version)

    if written:
        refresh_checksums(release)
        print(f"\n{written} archive(s) written, SHA256SUMS.txt rewritten")
    return 0 if written else 1


if __name__ == "__main__":
    raise SystemExit(main())
