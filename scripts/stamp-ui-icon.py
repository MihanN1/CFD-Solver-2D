#!/usr/bin/env python3
"""Put the project icon on the UI executable inside every "-ui" release archive.

The UI was built without a resource script, so "Fluid Solver UI.exe" carries no
icon at all and Explorer draws it as a blank binary. The solver's own executable
gets its icon from src/app.rc.in at compile time; this does the same thing after
the fact, so archives that are already published can be fixed without rebuilding
anything.

    python3 scripts/stamp-ui-icon.py 0.1                 # release/0.1, in place
    python3 scripts/stamp-ui-icon.py 0.1 --check         # report, change nothing
    python3 scripts/stamp-ui-icon.py 0.1 --release DIR --icon logo/fluid-solver.ico

Only a Windows executable can carry an icon inside itself. ELF and Mach-O have
no such concept: on Linux the icon comes from the .desktop entry and the hicolor
theme, which installer/linux/install.sh already installs, and on macOS it comes
from the .app bundle, which installer/macos/build-pkg.sh now gives an .icns. So
those rows are reported and left alone rather than silently "succeeding".

Writing the resource uses Windows' own BeginUpdateResource/UpdateResource, so
this half has to run on Windows. Reading it back to check the result is pure
Python and runs anywhere.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

RT_ICON = 3
RT_GROUP_ICON = 14
LANG_NEUTRAL_EN_US = 0x0409

UI_NAMES = ("Fluid Solver UI.exe", "Fluid Solver UI")
ROOT = Path(__file__).resolve().parents[1]


# ---------------------------------------------------------------- reading ---
# A read-only PE resource walker. It is how this script checks its own work and
# how --check tells "no icon" apart from "an icon Explorer is not showing".

class PEError(RuntimeError):
    pass


def _pe_layout(data: bytes):
    if data[:2] != b"MZ":
        raise PEError("not a PE file: no MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise PEError("not a PE file: no PE signature")
    coff = pe + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x10B:
        directories = optional + 96
    elif magic == 0x20B:
        directories = optional + 112
    else:
        raise PEError(f"unknown optional header magic 0x{magic:x}")
    resource_rva, resource_size = struct.unpack_from("<II", data, directories + 16)
    sections = []
    table = optional + optional_size
    for index in range(section_count):
        entry = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, entry + 8)
        sections.append((virtual_address, virtual_size, raw_pointer, raw_size))
    return sections, resource_rva, resource_size


def _offset_of(sections, rva: int):
    for virtual_address, virtual_size, raw_pointer, raw_size in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_pointer + (rva - virtual_address)
    return None


def _walk(data: bytes, table: int, base: int, path: tuple):
    named, ident = struct.unpack_from("<HH", data, table + 12)
    for index in range(named + ident):
        key, child = struct.unpack_from("<II", data, table + 16 + index * 8)
        name = f"<{key & 0x7FFFFFFF}>" if key & 0x80000000 else key
        if child & 0x80000000:
            yield from _walk(data, base + (child & 0x7FFFFFFF), base, path + (name,))
        else:
            rva, size = struct.unpack_from("<II", data, base + child)
            yield path + (name,), rva, size


def icon_resources(path: Path) -> dict:
    data = Path(path).read_bytes()
    sections, resource_rva, resource_size = _pe_layout(data)
    found = {"icons": {}, "groups": {}, "resources": bool(resource_rva and resource_size)}
    if not found["resources"]:
        return found
    base = _offset_of(sections, resource_rva)
    if base is None:
        raise PEError("resource directory lies outside every section")
    for keys, rva, size in _walk(data, base, base, ()):
        if not keys or keys[0] not in (RT_ICON, RT_GROUP_ICON):
            continue
        offset = _offset_of(sections, rva)
        if offset is None:
            continue
        bucket = "icons" if keys[0] == RT_ICON else "groups"
        found[bucket][keys[1] if len(keys) > 1 else 0] = data[offset:offset + size]
    return found


def icon_summary(path: Path) -> str:
    try:
        found = icon_resources(path)
    except PEError as error:
        return str(error)
    if not found["groups"]:
        return "no icon"
    sizes = []
    for blob in found["icons"].values():
        if blob[:8] == b"\x89PNG\r\n\x1a\n":
            width, height = struct.unpack_from(">II", blob, 16)
            sizes.append(f"{width}x{height}")
        elif len(blob) >= 8:
            width, height = struct.unpack_from("<ii", blob, 4)
            sizes.append(f"{width}x{height // 2}")
    return f"{len(found['icons'])} image(s): " + ", ".join(sizes)


# ---------------------------------------------------------------- writing ---

def split_icon_file(data: bytes):
    """(entries, images) from an .ico, in the order the file lists them."""
    if len(data) < 6:
        raise SystemExit("icon file is too short")
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or count == 0:
        raise SystemExit("not a Windows .ico file")
    entries, images = [], []
    for index in range(count):
        width, height, colors, pad, planes, bits, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + index * 16)
        if offset + size > len(data):
            raise SystemExit(f"icon image {index} runs past the end of the file")
        # Windows wants at least one plane; .ico files often leave it at zero.
        entries.append((width, height, colors, pad, planes or 1, bits, size))
        images.append(data[offset:offset + size])
    return entries, images


def group_icon_blob(entries, first_id: int = 1) -> bytes:
    """The GRPICONDIR that ties the RT_ICON images together.

    Byte for byte what rc.exe writes for the same .ico - checked against the
    solver's own executable, which gets its icon that way.
    """
    blob = struct.pack("<HHH", 0, 1, len(entries))
    for index, (width, height, colors, pad, planes, bits, size) in enumerate(entries):
        blob += struct.pack("<BBBBHHIH", width, height, colors, pad,
                            planes, bits, size, first_id + index)
    return blob


def stamp_windows_executable(exe: Path, icon: Path) -> None:
    """Write the icon into a PE using Windows' own resource updater."""
    if os.name != "nt":
        raise RuntimeError("writing PE resources needs Windows")

    import ctypes
    from ctypes import wintypes

    entries, images = split_icon_file(Path(icon).read_bytes())

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    kernel32.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel32.UpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR,
                                         wintypes.LPCWSTR, wintypes.WORD,
                                         wintypes.LPVOID, wintypes.DWORD]
    kernel32.UpdateResourceW.restype = wintypes.BOOL
    kernel32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    kernel32.EndUpdateResourceW.restype = wintypes.BOOL

    def resource_id(number: int):
        # MAKEINTRESOURCE: the integer is passed where a string pointer goes.
        return ctypes.cast(ctypes.c_void_p(number), wintypes.LPCWSTR)

    handle = kernel32.BeginUpdateResourceW(str(exe), False)
    if not handle:
        raise OSError(ctypes.get_last_error(),
                      f"BeginUpdateResource failed for {exe}")

    def update(kind: int, name: int, blob: bytes) -> None:
        buffer = ctypes.create_string_buffer(blob, len(blob))
        ok = kernel32.UpdateResourceW(handle, resource_id(kind), resource_id(name),
                                      LANG_NEUTRAL_EN_US, buffer, len(blob))
        if not ok:
            error = ctypes.get_last_error()
            kernel32.EndUpdateResourceW(handle, True)   # discard
            raise OSError(error, f"UpdateResource failed for type {kind} id {name}")

    for index, image in enumerate(images):
        update(RT_ICON, index + 1, image)
    update(RT_GROUP_ICON, 1, group_icon_blob(entries))

    if not kernel32.EndUpdateResourceW(handle, False):
        raise OSError(ctypes.get_last_error(), f"EndUpdateResource failed for {exe}")


# ---------------------------------------------------------------- archives ---

def ui_member(archive: zipfile.ZipFile):
    """The UI executable inside a release archive, or None."""
    for info in archive.infolist():
        if info.is_dir():
            continue
        if Path(info.filename).name in UI_NAMES:
            return info
    return None


def rewrite_archive(path: Path, member: str, replacement: bytes) -> None:
    """Replace one member, keeping every other entry's bytes, order and mode."""
    handle, temporary = tempfile.mkstemp(dir=str(path.parent), suffix=".zip")
    os.close(handle)
    temporary_path = Path(temporary)
    try:
        with zipfile.ZipFile(path) as source, \
             zipfile.ZipFile(temporary_path, "w", zipfile.ZIP_DEFLATED) as target:
            for info in source.infolist():
                payload = replacement if info.filename == member else source.read(info)
                copy = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                copy.compress_type = info.compress_type
                copy.external_attr = info.external_attr
                copy.internal_attr = info.internal_attr
                copy.create_system = info.create_system
                target.writestr(copy, payload, compresslevel=9)
        temporary_path.replace(path)
    finally:
        temporary_path.unlink(missing_ok=True)


def platform_of(name: str) -> str:
    for system in ("windows", "linux", "macos"):
        if f" {system}-" in name or f".{system}-" in name:
            return system
    return "unknown"


def process(release: Path, version: str, icon: Path, check_only: bool) -> int:
    # Both spellings: GitHub rewrites spaces to dots in release asset
    # filenames, so an archive downloaded from a tag is dotted.
    archives = sorted(set(release.glob(f"Fluid Solver {version} *-ui.zip")) |
                      set(release.glob(f"Fluid.Solver.{version}.*-ui.zip")))
    if not archives:
        print(f"no '-ui' archives for {version} in {release}", file=sys.stderr)
        return 1

    stamped = already = skipped = missing = 0
    blocked: list[str] = []

    for archive in archives:
        system = platform_of(archive.name)
        with zipfile.ZipFile(archive) as zf:
            info = ui_member(zf)
            if info is None:
                print(f"  {archive.name}: no UI executable inside")
                missing += 1
                continue
            payload = zf.read(info)

        if system != "windows":
            # Nothing to embed. Said out loud rather than counted as done.
            print(f"  {archive.name}: {Path(info.filename).name} is "
                  f"{'Mach-O' if system == 'macos' else 'ELF'}; the icon comes from "
                  f"{'the .app bundle' if system == 'macos' else 'the .desktop entry'}"
                  f", which the installer builds")
            skipped += 1
            continue

        with tempfile.TemporaryDirectory() as work:
            exe = Path(work) / "ui.exe"
            exe.write_bytes(payload)
            before = icon_summary(exe)
            if before != "no icon":
                print(f"  {archive.name}: already has an icon ({before})")
                already += 1
                continue
            if check_only:
                print(f"  {archive.name}: NO ICON")
                blocked.append(archive.name)
                continue
            if os.name != "nt":
                print(f"  {archive.name}: needs Windows to write the resource")
                blocked.append(archive.name)
                continue

            stamp_windows_executable(exe, icon)
            after = icon_summary(exe)
            if after == "no icon":
                print(f"  {archive.name}: stamping did not take", file=sys.stderr)
                blocked.append(archive.name)
                continue
            rewrite_archive(archive, info.filename, exe.read_bytes())
            print(f"  {archive.name}: icon added ({after})")
            stamped += 1

    print(f"\n{len(archives)} archive(s): {stamped} stamped, {already} already had one, "
          f"{skipped} carry no embeddable icon, {missing} had no UI binary")
    if blocked:
        verb = "have no icon" if check_only else "could not be stamped"
        print(f"{len(blocked)} {verb}:")
        for name in blocked:
            print("  " + name)
        return 1
    if stamped:
        refresh_checksums(release)
    return 0


def refresh_checksums(release: Path) -> None:
    """The archives changed, so the published checksums have to follow."""
    sums = release / "SHA256SUMS.txt"
    if not sums.is_file():
        return
    rows = []
    for path in sorted(p for p in release.iterdir()
                       if p.is_file() and p.name != sums.name):
        rows.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    sums.write_text("".join(rows), encoding="utf-8")
    print(f"rewrote {sums.name}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Put the project icon on the UI executable in every -ui archive.")
    parser.add_argument("version", nargs="?", default="0.1")
    parser.add_argument("--release", help="folder of release archives "
                                          "(default release/<version>)")
    parser.add_argument("--icon", default=str(ROOT / "logo" / "fluid-solver.ico"))
    parser.add_argument("--check", action="store_true",
                        help="report which archives lack an icon and change nothing")
    arguments = parser.parse_args()

    release = Path(arguments.release) if arguments.release \
        else ROOT / "release" / arguments.version
    if not release.is_dir():
        print(f"no such folder: {release}", file=sys.stderr)
        return 1
    icon = Path(arguments.icon)
    if not arguments.check and not icon.is_file():
        print(f"no icon file: {icon}", file=sys.stderr)
        return 1

    print(f"{'Checking' if arguments.check else 'Stamping'} "
          f"{release} with {icon.name}")
    return process(release, arguments.version, icon, arguments.check)


if __name__ == "__main__":
    raise SystemExit(main())
