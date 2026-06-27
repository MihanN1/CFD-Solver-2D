# CFD-Solver-2D

## Structure

```text
CFD-Solver-2D/
├── .vscode/
├── src/
│   ├── main.cpp
│   └── tiny_obj_loader_impl.cpp
├── include/
│   └── tiny_obj_loader.h       # auto-downloaded if missing
├── models/
│   └── .gitkeep
├── lib/
│   ├── sfml/                   # auto-downloaded if missing
│   └── stl_reader/             # auto-downloaded if missing
├── build/
├── CMakeLists.txt
├── .gitignore
└── README.md
```

## What CMake does

- If `include/tiny_obj_loader.h` is missing, it downloads it.
- If `lib/stl_reader/stl_reader.h` is missing, it downloads it.
- If `lib/sfml/` is missing, it downloads SFML source into `lib/sfml`.
- It builds `cfd_app`.
- It installs executable/header/model folders with `cmake --install`.

## Build — Visual Studio 2022

```bat
rmdir /s /q build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix install
```

Run:

```bat
install\bin\cfd_app.exe
```

## Build — Ninja

Open Developer Command Prompt for VS.

```bat
rmdir /s /q build
cmake -S . -B build -G Ninja
cmake --build build
cmake --install build --prefix install
```

Run:

```bat
install\bin\cfd_app.exe
```

## Models

Put `.obj` files into:

```text
models/
```

At start the app tries to load the first `.obj` file and prints info to console.

## Notes

- OBJ loader: `tiny_obj_loader.h`
- STL loader: `stl_reader.h`
- Window/render: SFML
- No vcpkg needed
