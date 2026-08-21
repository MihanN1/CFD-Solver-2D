# Decisions

## 2026-08-21 — Release UI by solver row name, not fake solver dependencies

Decision:
- Produce one `*-ui.zip` for every published solver row.
- AVX2 is a real UI compile variant.
- OpenMP/CUDA suffixes are package identity only and do not add OpenMP/CUDA libraries to the UI.
- Reuse one built UI executable for rows that share architecture and AVX2 state.

Reason:
- The existing installer pairs `<feature>-ui` with `<feature>` by name.
- OpenMP and CUDA are executed by `Fluid Solver`, not by the desktop UI; linking those runtimes into the UI would add dependencies without changing UI behavior.

Effect:
- Exact 30-row compatibility with the solver release naming contract without redundant compilation or false runtime requirements.

## 2026-08-21 — Maximize static linkage without breaking OS interfaces

Decision:
- Static-link UI-owned and vendored libraries/runtimes where supported.
- Do not attempt unsupported fully-static desktop binaries on Linux/macOS.

Reason:
- Windows CRT and GCC C++ runtimes can be made static. SFML's bundled dependencies can be static. Linux X11/OpenGL and macOS system frameworks are operating-system interfaces and remain dynamic.

Effect:
- Release binaries minimize redistributable runtime files while remaining buildable/valid for their platform.


## 2026-08-21 — Treat Fluid Solver as read-only

Decision:
- All compatibility work is implemented on the GUI side.
- The finished solver source/binaries are not patched, renamed, rebuilt, or repackaged by UI work.

Reason:
- Solver ownership and numerical behavior belong to the user's friend and are outside the UI modification boundary.

Effect:
- UI must detect, validate, explain, or gate unsupported solver behavior rather than silently changing solver code.

## 2026-08-21 — Gate multiple contours in the UI

Decision:
- When multiple disconnected contours are detected, require an explicit user choice before launch.
- If largest-contour mode is selected, update both preview and adapter geometry to that contour.

Reason:
- The current solver consumes one closed contour; silently passing several can make UI preview and solved geometry disagree.

Effect:
- The UI cannot silently claim that the solver is solving geometry it will discard.

## 2026-08-21 — Keep continuation disabled until solver support exists

Decision:
- Retain VTK restart parsing infrastructure, but do not emit `restart`, `restartFile`, or `addTime` for the current finished solver builds.

Reason:
- Current binaries do not expose the restart contract.

Effect:
- No fake or stale continuation hook is sent to the current solver.
