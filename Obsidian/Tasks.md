# Tasks

## Release validation

- [ ] Run `.github/workflows/build-ui-all.yml` for version `0.1`.
- [ ] Verify final release contains exactly 30 `Fluid Solver 0.1 ...-ui.zip` binary archives.
- [ ] Verify `Fluid-Solver-UI-Source-Code.zip` and `SHA256SUMS.txt`.
- [ ] Inspect Windows x64/x86 dependencies; confirm static CRT and no SFML DLLs.
- [ ] Inspect Linux x64/x86 dependencies; confirm libstdc++/libgcc are not dynamic and required X11/OpenGL system libraries remain.
- [ ] Launch macOS arm64/x64 builds.
- [ ] Pair each UI feature row with the identically named solver feature row in installer tests.
- [ ] Verify a fresh UI creates VTK runs under `./output`.


## Immediate validation

- [ ] Build with Visual Studio 18 2026, `Release | x64`.
- [ ] Launch the resulting GUI on Windows.
- [ ] Verify parameter-panel scrolling and all grouped controls.
- [ ] Verify multi-contour warning/cancel/largest-contour paths visually.
- [ ] Verify CPU-only solver selection disables CUDA request.
- [ ] Verify CUDA solver selection enables CUDA request.
- [ ] Run a fresh simulation with the AVX2 + OpenMP solver build.
- [ ] Run a fresh simulation with the AVX2 + OpenMP + CUDA solver build.
- [ ] Verify Run details, output directory, metadata, VTK playback, and result hover values.

## Deferred

- [ ] Revisit VTK continuation only after the solver publishes a tested restart interface.
