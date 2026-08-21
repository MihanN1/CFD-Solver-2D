#!/usr/bin/env python3
"""Build and package the desktop UI using the Fluid Solver release matrix.

The UI itself does not execute OpenMP or CUDA solver kernels. Therefore the
OpenMP/CUDA parts of a release feature name are compatibility/package identity,
while AVX2 changes the generated UI machine code. This avoids rebuilding the
same UI binary four times while still producing the exact 30 archive names that
the Fluid Solver release/installers pair by name.
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
    ("linux", "x64"): [
        "avx2", "avx2-cuda", "avx2-omp", "avx2-omp-cuda",
        "cuda", "omp-cuda", "omp", "plain",
    ],
    ("linux", "x86"): ["avx2", "avx2-omp", "omp", "plain"],
    ("macos", "arm64"): ["omp", "plain"],
    ("macos", "x64"): ["avx2", "avx2-omp", "omp", "plain"],
}

PLATFORM_ARCHES = {
    "windows": ["x64", "x86"],
    "linux": ["x64", "x86"],
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


def cmake_args(system: str, arch: str, avx2: bool, build_dir: Path,
               generator: str) -> list[str]:
    args = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir),
        "-DBUILD_TESTING=OFF",
        "-DCFD_UI_STATIC_RUNTIME=ON",
        f"-DCFD_UI_ENABLE_AVX2={'ON' if avx2 else 'OFF'}",
    ]

    if system == "windows":
        args += ["-G", generator, "-A", "x64" if arch == "x64" else "Win32"]
    else:
        args += ["-DCMAKE_BUILD_TYPE=Release"]
        if system == "linux" and arch == "x86":
            # These must be present during compiler detection so CMake chooses
            # i386 multiarch libraries rather than absolute amd64 libraries.
            args += ["-DCMAKE_C_FLAGS=-m32", "-DCMAKE_CXX_FLAGS=-m32"]
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


def build_binary(system: str, arch: str, avx2: bool, work: Path,
                 generator: str, keep_builds: bool) -> Path:
    isa = "avx2" if avx2 else "plain"
    build_dir = work / f"build-{system}-{arch}-{isa}"
    if build_dir.exists() and not keep_builds:
        shutil.rmtree(build_dir)
    build_dir.parent.mkdir(parents=True, exist_ok=True)

    run(cmake_args(system, arch, avx2, build_dir, generator))
    command = ["cmake", "--build", str(build_dir), "--parallel"]
    if system == "windows":
        command += ["--config", "Release"]
    run(command)
    return find_executable(build_dir, system)


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
        for filename in ("README.md", "LICENSE", "BUILD_INFO.md"):
            source = ROOT / filename
            if source.is_file():
                zip_add_file(zf, source, f"{name}/{filename}")
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

    for arch in arches:
        binaries: dict[bool, Path] = {}
        for feature in FEATURES[(system, arch)]:
            avx2 = "avx2" in feature.split("-")
            if avx2 not in binaries:
                binaries[avx2] = build_binary(
                    system, arch, avx2, work, args.generator, args.keep_builds)
            package_variant(
                binaries[avx2], output, args.version, system, arch, feature)


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
    parser.add_argument("--version", default="0.1")
    parser.add_argument("--arch", action="append", choices=["x64", "x86", "arm64"])
    parser.add_argument("--output", default=str(ROOT / "dist-ui"))
    parser.add_argument("--work", default=str(ROOT / ".release-build"))
    parser.add_argument("--generator", default="Visual Studio 18 2026")
    parser.add_argument("--keep-builds", action="store_true")
    parser.add_argument("--list", action="store_true", help="print the exact 30 archive names and exit")
    parser.add_argument("--finalize", metavar="DIR", help="verify all 30 archives and add source/checksums")
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
