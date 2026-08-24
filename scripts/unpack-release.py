#!/usr/bin/env python3
"""Rebuild dist/ out of an already published release, so the installers can be
built without building a single binary again.

    python3 scripts/unpack-release.py 0.1 release/0.1 dist
    python3 scripts/unpack-release.py 0.1 downloaded dist --platform windows

A release holds one zip per row:

    Fluid Solver 0.1 windows-x64 avx2-omp-cuda.zip
    Fluid Solver 0.1 linux-x64 plain.zip
    Fluid Solver 0.1 macos-arm64 omp.zip

and each of those holds a folder of the same name with the executable inside.
The three installer builders want two different shapes out of that, which is
the whole reason this script exists:

    windows   a folder, because vcomp140.dll sits beside the .exe
    linux     a single file named "Fluid Solver <ver> linux-<arch> <feature>"
    macos     the same single file

Python rather than shell because it runs the same on all four runners, and
because every name in this release has spaces in it.

Archives whose name ends in "-ui" are the desktop UI. The UI is built per
variant exactly like the solver, so each one keeps its own name and the
installers pair "<variant>-ui" with "<variant>" by that name alone.
"""

import argparse
import re
import shutil
import sys
import zipfile
from pathlib import Path

# A space or a dot separates the four parts of the name. The dot is not a
# stylistic choice: GitHub rewrites spaces to dots in release asset filenames,
# so the same archive comes back from "gh release download" as
# "Fluid.Solver.0.1.linux-x64.avx2.zip". Matching only spaces is why a release
# downloaded from a tag looked to this script like a folder full of files it had
# never seen.
NAME = re.compile(
    r"^Fluid[ .]Solver[ .](?P<ver>[0-9][0-9A-Za-z.]*?)[ .]"
    r"(?P<platform>windows|linux|macos)-(?P<arch>x64|x86|arm64)[ .]"
    r"(?P<feature>[a-z0-9-]+?)(?P<ui>-ui)?$"
)


def parse(stem):
    m = NAME.match(stem)
    if not m:
        return None
    d = m.groupdict()
    d["ui"] = bool(d["ui"])
    return d


def canonical_name(info):
    """The name with spaces, whatever the file on disk happened to be called.

    Everything downstream - the .iss variant folders, install.sh's payload scan,
    build-pkg.sh - looks for the spaced form, so that is what dist/ must hold
    even when the archive arrived with dots in its name.
    """
    return (f"Fluid Solver {info['ver']} {info['platform']}-{info['arch']} "
            f"{info['feature']}" + ("-ui" if info["ui"] else ""))


# The two programs an archive may hold, on every platform. Anything named like
# one of these has to come out executable; everything else is data.
EXECUTABLES = {"Fluid Solver", "Fluid Solver.exe", "Fluid Solver UI", "Fluid Solver UI.exe"}


def extract(zip_path, into):
    """Unpack and return the single top-level directory inside.

    extractall() drops the Unix mode, so everything would come out 0644 and the
    executables inside a UI archive would not run. The mode is in the upper 16
    bits of external_attr on any zip written by a Unix tool; when it is there,
    it is put back.

    A zip written on Windows - which is how the "<variant>-ui" archives are
    assembled by hand - has no Unix bits to put back at all: external_attr holds
    DOS attributes and the upper half is zero. Python then extracts those
    entries 0600 and every later step copies that mode along, so the install
    ends up with a UI that will not run, a README and LICENSE readable only by
    whoever ran the installer, and an output/ at 0700 that a system-wide install
    cannot write frames into. A missing mode is therefore filled in rather than
    left alone: 0755 for directories and for the two executables, 0644 for the
    rest.
    """
    with zipfile.ZipFile(zip_path) as z:
        roots = {Path(n).parts[0] for n in z.namelist() if n.strip()}
        z.extractall(into)
        for item in z.infolist():
            mode = (item.external_attr >> 16) & 0o7777
            if not mode:
                if item.is_dir():
                    mode = 0o755
                else:
                    mode = 0o755 if Path(item.filename).name in EXECUTABLES else 0o644
            (into / item.filename).chmod(mode)
    if len(roots) != 1:
        raise SystemExit(f"{zip_path.name}: expected one folder inside, found {sorted(roots)}")
    return into / roots.pop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("source", help="folder holding the release zips")
    ap.add_argument("dist", help="folder to rebuild")
    ap.add_argument("--platform", action="append", default=[],
                    choices=["windows", "linux", "macos"],
                    help="only unpack these; may be repeated")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    source = Path(args.source)
    dist = Path(args.dist)
    if not source.is_dir():
        raise SystemExit(f"no such folder: {source}")
    dist.mkdir(parents=True, exist_ok=True)
    scratch = dist / ".unpack"
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir()

    wanted = set(args.platform) or {"windows", "linux", "macos"}
    taken, skipped = 0, 0
    seen_versions = set()

    for zip_path in sorted(source.glob("*.zip")):
        info = parse(zip_path.stem)
        if info is not None:
            seen_versions.add(info["ver"])
        if info is None or info["ver"] != args.version:
            skipped += 1
            continue
        if info["platform"] not in wanted:
            continue

        name = canonical_name(info)
        inner = extract(zip_path, scratch / zip_path.stem)
        target = dist / name

        # Windows rows and every UI stay folders; a Linux or macOS solver row
        # collapses back to the single file the installers index by name.
        if info["platform"] == "windows" or info["ui"]:
            if target.exists():
                shutil.rmtree(target)
            shutil.move(str(inner), str(target))
        else:
            binary = inner / "Fluid Solver"
            if not binary.is_file():
                raise SystemExit(f"{zip_path.name}: no 'Fluid Solver' inside")
            if target.exists():
                target.unlink()
            shutil.move(str(binary), str(target))
            target.chmod(0o755)

        taken += 1
        if not args.quiet:
            print(f"  {target.name}")

    shutil.rmtree(scratch, ignore_errors=True)

    if taken == 0:
        message = [
            f"nothing matched 'Fluid Solver {args.version} <platform>-<arch> "
            f"<feature>.zip' in {source}"
        ]
        others = sorted(v for v in seen_versions if v != args.version)
        if others:
            message.append(
                "the archives there are version " + ", ".join(others)
                + f" - not {args.version}. Pass that as the version instead; a "
                  "release tag that reads differently is a separate thing.")
        elif not seen_versions:
            message.append(
                "no file there is named like a release row at all - "
                f"{source} holds {len(list(source.glob('*.zip')))} zip(s).")
        raise SystemExit("\n".join(message))
    print(f"{taken} row(s) unpacked into {dist}"
          + (f", {skipped} file(s) ignored" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
