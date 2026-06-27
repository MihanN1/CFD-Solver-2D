# CFD-Solver-2D

## Архитектура

```text
CFD-Solver-2D/
├── .vscode/
├── src/
│   ├── main.cpp
│   └── tiny_obj_loader_impl.cpp
├── include/
│   └── tiny_obj_loader.h       # скачивается CMake, если отсутствует
├── models/
├── lib/
│   ├── sfml/                   # скачивается CMake, если отсутствует
│   └── stl_reader/             # скачивается CMake, если отсутствует
├── build/
├── CMakeLists.txt
├── .gitignore
└── README.md
```

## Build — PowerShell

```powershell
cd "C:\Users\alans\3D Objects\.git\CFD-Solver-2D-full-fixed"

if (Test-Path build) { Remove-Item build -Recurse -Force }
if (Test-Path install) { Remove-Item install -Recurse -Force }

cmake -S . -B build -G "Visual Studio 18 2026" -A x64

cmake --build build --config Release

cmake --install build --config Release --prefix install
```

## Run

```powershell
.\install\bin\cfd_app.exe
```
