# Errors

## 2026-08-21 — Multi-contour UI/solver capability mismatch

Status:
- Mitigated in UI

Where:
- UI geometry can contain multiple disconnected contours.
- Current Fluid Solver supports one closed contour for the solved section.

Why:
- Without a guard, preview geometry can differ from geometry used by the solver.

Fix:
- UI now blocks silent launch and offers explicit largest-contour-only mode.

Remaining:
- True multiple-object solving requires solver capability and is outside UI ownership.

## 2026-08-21 — Restart/continuation contract absent in current solver

Status:
- Known / gated

Where:
- Existing VTK parser contains restart-oriented infrastructure, but current finished solver binaries do not expose restart launch keys/state.

Fix:
- UI does not emit restart arguments and keeps continuation unavailable.

Remaining:
- Re-enable only after a solver release defines and tests the real restart contract.
