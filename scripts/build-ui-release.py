#!/usr/bin/env python3
"""Build and package the desktop UI using the Fluid Solver release matrix.

AVX2 and OpenMP both change the generated UI machine code: AVX2 vectorises the
byte swap that decodes a VTK frame, OpenMP spreads that decode and the colour
map across cores. CUDA does not - a frame is a read plus a swap, and the colour
map has to land in host memory for the texture upload anyway, so a GPU round
trip would cost more than it saves.

So a row's AVX2 and OpenMP state selects a build, and its CUDA state selects
only the name. One binary therefore serves the "-cuda" row and the row without
it, which is what keeps this to 26 builds for the exact 34 archive names the
Fluid Solver release pairs against.

ARM64 was added in 0.2, on Windows and Linux, and carries neither AVX2 - an x86
instruction set - nor CUDA. On Windows it is a cross build (MSVC "-A ARM64");
on Linux it is native on an ARM host and the aarch64 cross toolchain otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FEATURES = {
    ("windows", "x64"): [
        "avx2", "avx2-cuda", "avx2-omp", "avx2-omp-cuda",
        "cuda", "omp-cuda", "omp", "plain",
    ],
    ("windows", "x86"): ["avx2", "avx2-omp", "omp", "plain"],
    # No AVX2 on ARM - it is an x86 instruction set - and no CUDA toolkit for
    # Windows on ARM either, so the two names are all there are.
    ("windows", "arm64"): ["omp", "plain"],
    ("linux", "x64"): [
        "avx2", "avx2-cuda", "avx2-omp", "avx2-omp-cuda",
        "cuda", "omp-cuda", "omp", "plain",
    ],
    ("linux", "x86"): ["avx2", "avx2-omp", "omp", "plain"],
    ("linux", "arm64"): ["omp", "plain"],
    ("macos", "arm64"): ["omp", "plain"],
    ("macos", "x64"): ["avx2", "avx2-omp", "omp", "plain"],
}

PLATFORM_ARCHES = {
    "windows": ["x64", "x86", "arm64"],
    "linux": ["x64", "x86", "arm64"],
    "macos": ["arm64", "x64"],
}


def host_platform() -> str:
    name = platform.system().lower()
    if name == "windows":
        return "windows"
    if name == "linux":
        return "linux"
    if name == "darwin":
        return "macos"
    raise SystemExit(f"unsupported host platform: {platform.system()}")


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command))
    subprocess.run(command, cwd=ROOT, check=True)


def build_name(version: str, system: str, arch: str, feature: str) -> str:
    return f"Fluid Solver {version} {system}-{arch} {feature}-ui"


def expected_archives(version: str) -> list[str]:
    result: list[str] = []
    for system in ("linux", "macos", "windows"):
        for arch in PLATFORM_ARCHES[system]:
            for feature in FEATURES[(system, arch)]:
                result.append(build_name(version, system, arch, feature) + ".zip")
    return result


def macos_libomp_prefix(install: bool = True) -> str | None:
    """Homebrew's libomp, which AppleClang needs to compile any OpenMP at all.

    Installs it when it is missing rather than only reporting it. A build host
    is not required to have been prepared by hand: the OpenMP rows need this and
    nothing else does, so the script that needs it is the place to get it.
    """
    brew = shutil.which("brew")
    if brew is None:
        return None

    def prefix_of() -> str | None:
        try:
            found = subprocess.run(
                [brew, "--prefix", "libomp"],
                capture_output=True, text=True, check=True).stdout.strip()
        except subprocess.CalledProcessError:
            return None
        return found if found and Path(found, "include", "omp.h").is_file() else None

    prefix = prefix_of()
    if prefix is not None or not install:
        return prefix
    print("  libomp is missing; installing it with Homebrew")
    subprocess.run([brew, "install", "libomp"], check=False)
    return prefix_of()


def linux32_libgomp_present() -> bool:
    """Whether a -m32 link can find a 32-bit libgomp.a."""
    compiler = os.environ.get("CXX", "g++")
    try:
        found = subprocess.run(
            [compiler, "-m32", "-print-file-name=libgomp.a"],
            capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False
    return Path(found).is_file()


def aarch64_toolchain() -> tuple[str, str] | None:
    """(cc, cxx) for aarch64: the native pair on an ARM host, the cross pair
    otherwise. None when neither is installed."""
    if platform.machine().lower() in {"aarch64", "arm64"}:
        cc = shutil.which("gcc") or shutil.which("cc")
        cxx = shutil.which("g++") or shutil.which("c++")
        return (cc, cxx) if cc and cxx else None
    for triple in ("aarch64-linux-gnu", "aarch64-none-linux-gnu"):
        cc = shutil.which(f"{triple}-gcc")
        cxx = shutil.which(f"{triple}-g++")
        if cc and cxx:
            return cc, cxx
    return None


class RowUnavailable(RuntimeError):
    """This row cannot be built here. Others still can."""


def cmake_args(system: str, arch: str, avx2: bool, openmp: bool,
               build_dir: Path, generator: str) -> list[str]:
    args = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir),
        "-DBUILD_TESTING=OFF",
        # SFML fetches FreeType, HarfBuzz and SheenBidi when it builds their
        # static forms. Pointing every build at one cache clones them once for
        # the whole run instead of once per build directory.
        f"-DFETCHCONTENT_BASE_DIR={build_dir.parent / '_deps'}",
        "-DCFD_UI_STATIC_RUNTIME=ON",
        f"-DCFD_UI_ENABLE_AVX2={'ON' if avx2 else 'OFF'}",
        f"-DCFD_UI_ENABLE_OPENMP={'ON' if openmp else 'OFF'}",
    ]
    if openmp:
        # A row named "omp" must have OpenMP in it. Without this a build machine
        # missing the runtime would quietly publish a single-threaded binary
        # under a name promising otherwise - the same trap the solver's release
        # script guards against.
        args.append("-DCFD_UI_ENABLE_OPENMP_EXPLICIT=ON")
        if system == "macos":
            prefix = macos_libomp_prefix()
            if prefix is None:
                raise RowUnavailable(
                    "no Homebrew libomp on this host, so an OpenMP row cannot be "
                    "built here; install Homebrew, or run: brew install libomp")
            args.append(f"-DCFD_UI_OPENMP_ROOT={prefix}")
        if system == "linux" and arch == "x86" and not linux32_libgomp_present():
            raise RowUnavailable(
                "no 32-bit libgomp.a for a -m32 link; install it, for example: "
                "sudo apt-get install lib32gomp-14-dev")

    if system == "windows":
        # CMake's spelling of the architecture is not the release's: the
        # generator wants Win32 and ARM64, the file names want x86 and arm64.
        generator_arch = {"x64": "x64", "arm64": "ARM64"}.get(arch, "Win32")
        args += ["-G", generator, "-A", generator_arch]
    else:
        args += ["-DCMAKE_BUILD_TYPE=Release"]
        if system == "linux" and arch == "x86":
            # These must be present during compiler detection so CMake chooses
            # i386 multiarch libraries rather than absolute amd64 libraries.
            args += ["-DCMAKE_C_FLAGS=-m32", "-DCMAKE_CXX_FLAGS=-m32"]
        elif system == "linux" and arch == "arm64":
            compiler = aarch64_toolchain()
            if compiler is None:
                raise RowUnavailable(
                    "no aarch64 compiler; install it, for example: "
                    "sudo apt-get install crossbuild-essential-arm64")
            # A cross build has to be told, or CMake probes the host compiler
            # and produces an x86-64 binary under an arm64 name. On an ARM host
            # this is the native compiler and the whole block is a no-op.
            args += [
                "-DCMAKE_SYSTEM_NAME=Linux",
                "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
                f"-DCMAKE_C_COMPILER={compiler[0]}",
                f"-DCMAKE_CXX_COMPILER={compiler[1]}",
            ]
        elif system == "macos":
            args += [
                "-DCMAKE_OSX_ARCHITECTURES=" +
                ("arm64" if arch == "arm64" else "x86_64")
            ]
    return args


def find_executable(build_dir: Path, system: str) -> Path:
    wanted = "Fluid Solver UI.exe" if system == "windows" else "Fluid Solver UI"
    candidates = [p for p in build_dir.rglob(wanted) if p.is_file()]
    # Ignore CMake compiler probes or copies under dependency directories.
    candidates = [
        p for p in candidates
        if "CMakeFiles" not in p.parts and "_deps" not in p.parts
    ]
    if len(candidates) != 1:
        listed = "\n  ".join(str(p) for p in candidates) or "<none>"
        raise RuntimeError(f"expected one {wanted!r} under {build_dir}, found:\n  {listed}")
    return candidates[0]


def build_binary(system: str, arch: str, avx2: bool, openmp: bool, work: Path,
                 generator: str, keep_builds: bool) -> Path:
    # One directory per architecture, reconfigured for each variant rather than
    # one directory per variant. AVX2 and OpenMP are applied to mask_ui_core and
    # the executable only, so SFML, FreeType, HarfBuzz and SheenBidi are
    # identical across the variants of an architecture - and building them four
    # times, plus cloning the three of them four times over, was most of the
    # wall clock this script used to spend.
    build_dir = work / f"build-{system}-{arch}"
    build_dir.parent.mkdir(parents=True, exist_ok=True)

    run(cmake_args(system, arch, avx2, openmp, build_dir, generator))
    command = ["cmake", "--build", str(build_dir), "--parallel"]
    if system == "windows":
        command += ["--config", "Release"]
    run(command)

    binary = find_executable(build_dir, system)
    # A release binary carries no symbol table. MSVC already keeps its debug
    # information in a separate .pdb, so this is for the other two - it takes
    # about an eighth off the file the user downloads.
    if system != "windows":
        strip = shutil.which("strip")
        if (system == "linux" and arch == "arm64" and
                platform.machine().lower() not in {"aarch64", "arm64"}):
            # The host strip does not know this object format; its own does,
            # and where there is none the binary simply keeps its symbols.
            strip = shutil.which("aarch64-linux-gnu-strip")
        if strip is not None:
            before = binary.stat().st_size
            flags = ["-x"] if system == "macos" else []
            subprocess.run([strip, *flags, str(binary)], check=False)
            after = binary.stat().st_size
            print(f"  stripped {before // 1024} KiB -> {after // 1024} KiB")
    return binary


def zip_add_file(zf: zipfile.ZipFile, source: Path, arcname: str,
                 executable: bool = False) -> None:
    info = zipfile.ZipInfo.from_file(source, arcname)
    info.create_system = 3
    mode = 0o755 if executable else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    with source.open("rb") as handle:
        zf.writestr(info, handle.read(), compresslevel=9)


def zip_add_directory(zf: zipfile.ZipFile, arcname: str) -> None:
    if not arcname.endswith("/"):
        arcname += "/"
    info = zipfile.ZipInfo(arcname)
    info.create_system = 3
    info.external_attr = (stat.S_IFDIR | 0o755) << 16
    info.compress_type = zipfile.ZIP_STORED
    zf.writestr(info, b"")


def package_variant(binary: Path, output: Path, version: str,
                    system: str, arch: str, feature: str) -> Path:
    name = build_name(version, system, arch, feature)
    archive = output / f"{name}.zip"
    output.mkdir(parents=True, exist_ok=True)
    executable_name = "Fluid Solver UI.exe" if system == "windows" else "Fluid Solver UI"

    with zipfile.ZipFile(archive, "w") as zf:
        zip_add_directory(zf, name)
        zip_add_file(zf, binary, f"{name}/{executable_name}", executable=True)
        # Renamed on the way in. These archives get merged by hand into the
        # solver's "-ui" archive, which already has a README.md and a LICENSE of
        # its own; copying these across under the same names would replace the
        # solver's documentation with the UI's.
        for filename, packaged in (
            ("README.md", "README-UI.md"),
            ("BUILD_INFO.md", "BUILD_INFO-UI.md"),
        ):
            source = ROOT / filename
            if source.is_file():
                zip_add_file(zf, source, f"{name}/{packaged}")
        zip_add_directory(zf, f"{name}/output")
    print("  wrote", archive.name)
    return archive


def build_platform(args: argparse.Namespace) -> None:
    system = host_platform()
    arches = args.arch or PLATFORM_ARCHES[system]
    invalid = [arch for arch in arches if (system, arch) not in FEATURES]
    if invalid:
        raise SystemExit(f"invalid {system} architecture(s): {', '.join(invalid)}")

    output = Path(args.output).resolve()
    work = Path(args.work).resolve()

    skipped: list[str] = []
    for arch in arches:
        if not args.keep_builds:
            shutil.rmtree(work / f"build-{system}-{arch}", ignore_errors=True)
        # Keyed on what actually changes the machine code. The CUDA half of a
        # feature name does not, so "avx2-omp-cuda" and "avx2-omp" ship the same
        # binary under their two names.
        binaries: dict[tuple[bool, bool], Path] = {}
        unavailable: dict[tuple[bool, bool], str] = {}
        for feature in FEATURES[(system, arch)]:
            parts = feature.split("-")
            key = ("avx2" in parts, "omp" in parts)
            if key in unavailable:
                skipped.append(f"{system}-{arch} {feature}: {unavailable[key]}")
                continue
            if key not in binaries:
                try:
                    binaries[key] = build_binary(
                        system, arch, key[0], key[1], work,
                        args.generator, args.keep_builds)
                except RowUnavailable as reason:
                    # One missing prerequisite must not take the rows that do
                    # not need it down with it. The run still fails at the end,
                    # and --finalize refuses a release with a hole in it.
                    unavailable[key] = str(reason)
                    print(f"  SKIPPED {system}-{arch} {feature}: {reason}")
                    skipped.append(f"{system}-{arch} {feature}: {reason}")
                    continue
            package_variant(
                binaries[key], output, args.version, system, arch, feature)

    if skipped:
        print(f"\n{len(skipped)} row(s) were not built:")
        for line in skipped:
            print("  " + line)
        raise SystemExit(1)


def should_skip_source(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    parts = rel.parts
    if not parts:
        return False
    first = parts[0]
    if first in {".git", ".vs", ".vscode", ".release-build", "dist-ui", "release-ui"}:
        return True
    if first.startswith("build") or first.startswith("out"):
        return True
    if "__pycache__" in parts:
        return True
    if path.is_file() and path.suffix.lower() == ".vtk" and "output" in parts:
        return True
    return False


def create_source_archive(release_dir: Path) -> Path:
    name = "Fluid-Solver-UI-Source-Code"
    archive = release_dir / f"{name}.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zip_add_directory(zf, name)
        for path in sorted(ROOT.rglob("*")):
            if should_skip_source(path):
                continue
            rel = path.relative_to(ROOT)
            arc = f"{name}/{rel.as_posix()}"
            if path.is_dir():
                zip_add_directory(zf, arc)
            elif path.is_file():
                executable = path.suffix in {".sh", ".py"}
                zip_add_file(zf, path, arc, executable=executable)
    return archive


def finalize_release(args: argparse.Namespace) -> None:
    release_dir = Path(args.finalize).resolve()
    release_dir.mkdir(parents=True, exist_ok=True)
    expected = expected_archives(args.version)
    missing = [name for name in expected if not (release_dir / name).is_file()]
    if missing:
        raise SystemExit("missing UI release archive(s):\n  " + "\n  ".join(missing))

    # Reject typoed/partial binary row names while allowing metadata/source files.
    actual_binary = sorted(p.name for p in release_dir.glob("Fluid Solver *.zip"))
    unexpected = [name for name in actual_binary if name not in expected]
    if unexpected:
        raise SystemExit("unexpected UI release archive(s):\n  " + "\n  ".join(unexpected))

    source_archive = create_source_archive(release_dir)
    for filename in ("README.md", "LICENSE"):
        shutil.copy2(ROOT / filename, release_dir / filename)

    checksum_path = release_dir / "SHA256SUMS.txt"
    rows = []
    for path in sorted(p for p in release_dir.iterdir()
                       if p.is_file() and p.name != checksum_path.name):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        rows.append(f"{digest}  {path.name}\n")
    checksum_path.write_text("".join(rows), encoding="utf-8")

    print(f"verified {len(expected)} binary UI archives")
    print("wrote", source_archive.name)
    print("wrote", checksum_path.name)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", default="0.2")
    parser.add_argument("--arch", action="append", choices=["x64", "x86", "arm64"])
    parser.add_argument("--output", default=str(ROOT / "dist-ui"))
    parser.add_argument("--work", default=str(ROOT / ".release-build"))
    # The solver's own release is cut with Visual Studio 2022, and the Windows
    # runner in .github is windows-2022, which has no 2026 to find.
    parser.add_argument("--generator", default="Visual Studio 17 2022")
    parser.add_argument("--keep-builds", action="store_true")
    parser.add_argument("--list", action="store_true", help="print every archive name and exit")
    parser.add_argument("--finalize", metavar="DIR", help="verify every archive and add source/checksums")
    args = parser.parse_args()

    if args.list:
        print("\n".join(expected_archives(args.version)))
        return 0
    if args.finalize:
        finalize_release(args)
        return 0
    build_platform(args)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        raise SystemExit(exc.returncode) from exc
