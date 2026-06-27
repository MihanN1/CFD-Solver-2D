# CFD-Solver-2D

**A 2D incompressible Navier‑Stokes solver for external flows around arbitrary profiles.**

CFD‑Solver‑2D is an educational/research project that implements a finite‑difference CFD solver for unsteady viscous incompressible flow. It uses the **Chorin projection method** on a **staggered MAC grid** with an **immersed boundary** technique to handle complex geometries. The code is written in C++17 and features:
- Interactive console parameter input.
- Real‑time visualisation with **SFML** (pressure, velocity magnitude, streamlines, vector field).
- Import of 3D models (STL/OBJ) with automatic slicing to obtain a 2D cross‑section profile.

The ultimate goal is to simulate the **Kármán vortex street** behind a cylinder or an airfoil at moderate Reynolds numbers.

---

## Project Structure

**Architecture**
CFD-Solver-2D/<br>
├── .vscode/               (VS Code settings, optional)<br>
├── src/                   (source files)<br>
│   └── main.cpp           (entry point)<br>
├── include/               (header files)<br>
│   └──tiny_obj_loader.h   (for now no CMake connect and no vcpkg installed, also no STL-reader, only obj works)<br>
├── models/                (place 3D STL/OBJ files here)<br>
├── lib/                   (external libraries, if manually installed)<br>
│   └── sfml/              (SFML SDK)<br>
├── build/                 (build output, ignored by Git)<br>
├── CMakeLists.txt         (build configuration)<br>
├── .gitignore
└── README.md

**Commit messages**
feat: new stable things
fix: fixes
docs: documentation updates
refactor: code improving

## Current Status (Sprint 0)

This repository contains the **initial project skeleton**:

- CMake build system configured.
- Basic `main.cpp` with a "Hello, World!" placeholder.
- Git repository structure with `develop` branch.
- README and `.gitignore` prepared.

**No numerical solver or visualisation has been implemented yet.**  
The project is ready for development to begin.

---

## Planned Features (Roadmap)

| Sprint |Focus                                                                         |
|--------|------------------------------------------------------------------------------|
| **1**  | Config parser, structured grid generation, circle mask (immersed boundary)   |
| **2**  | Predictor‑corrector solver (Chorin), SOR Poisson solver, CFL check           |
| **3**  | STL/OBJ import, 2D profile extraction by slicing, geometry masking           |
| **4**  | SFML‑based real‑time rendering, interactive controls (pause, mode switching) |

---

## Requirements

- **C++17** compatible compiler (MSVC 2019/2022, GCC 9+, Clang 10+)
- **CMake** 3.10 or higher
- **Optional dependencies** (only for visualisation and import):
  - [SFML](https://www.sfml-dev.org/) (≥ 2.6 or 3.0)
  - [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) (header‑only)
  - OpenGL (system library)

---

## Building the Project

### 1. Clone the repository

```bash
git clone git@github.com:yourusername/CFD-Solver-2D.git
cd CFD-Solver-2D
