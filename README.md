(lets just forget that we pushed 12 commits just for things to build normally and files to look at least alright 'cause we're stooooopid as shi)
(from Kuzya: i dunno how it works, i have a feeling it's something alive, and changes by itself <3. TOTALLY NOT ME COMMITING 12 TIMES)
# CFD-Solver-2D

**A 2D incompressible Navier‑Stokes solver for external flows around arbitrary profiles.**

CFD‑Solver‑2D is an educational/research project that implements a finite‑difference CFD solver for unsteady viscous incompressible flow. It uses the **Chorin projection method** on a **staggered MAC grid** with an **immersed boundary** technique to handle complex geometries. The code is written in C++17 and features:<br>
- Interactive console parameter input.<br>
- Real‑time visualisation with **SFML** (pressure, velocity magnitude, streamlines, vector field).<br>
- Import of 3D models (STL/OBJ) with automatic slicing to obtain a 2D cross‑section profile.<br>

The ultimate goal is to simulate the **Kármán vortex street** behind a cylinder or an airfoil at moderate Reynolds numbers.

---

## General info
**Commit messages guide**<br>
feat: new stable things!<br>
fix: fixes<br>
docs: documentation updates<br>
refactor: code improving<br>
**Planned features**<br>
| Sprint |Focus                                                                         |<br>
|--------|------------------------------------------------------------------------------|<br>
| **1**  | Config parser, structured grid generation, circle mask (immersed boundary)   |<br>
| **2**  | Predictor‑corrector solver (Chorin), SOR Poisson solver, CFL check           |<br>
| **3**  | STL/OBJ import, 2D profile extraction by slicing, geometry masking           |<br>
| **4**  | SFML‑based real‑time rendering, interactive controls (pause, mode switching) |<br>
| **5**  | Optimizations, fixes, compressible liquid features, other extra features.    |<br>

---

## Architecture

```text
CFD-Solver-2D/
├── .git/                       #<- not saved by github
├── .vscode/                    #<- not saved by github
├── .vs/                        #<- not saved by github
├── out/                        #CMake files here <- not saved by github
├── output/                     #Files with results here <- github doesnt save insides
├── src/
│   ├── main.cpp
│   └── tiny_obj_loader_impl.cpp
├── include/                    #<- isnt saved if empty
│   └── tiny_obj_loader.h       #CMake adds this if its not there
├── models/                     #<- github doesnt save insides
├── lib/                        #<- isnt saved if empty
│   ├── sfml/                   #CMake adds this if its not there
│   └── stl_reader/             #CMake adds this if its not there
├── build/                      #main.exe file here <- not saved by github
├── CMakeLists.txt
├── .gitignore
├── LICENSE
└── README.md
```

## Requirements

- **C++17** compatible compiler (MSVC 2019/2022, GCC 9+, Clang 10+)<br>
- **CMake** 3.10 or higher<br>

---

## Build — PowerShell

```powershell
cd "...\CFD-Solver-2D-full-fixed"

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
