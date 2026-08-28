#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

int main() {
    const std::filesystem::path root = scratchDir("channel");
    std::string error;

    Config cfg = baseConfig(root / "poiseuille");

    cfg.Lx = 2.0f;
    cfg.Ly = 1.0f;
    cfg.nx = 48;
    cfg.ny = 24;
    cfg.U0 = 1.0f;
    cfg.nu = 0.2f;
    cfg.totalTime = 8.0;
    cfg.CFL = 0.4f;
    cfg.mgIterations = 20;
    cfg.mgTolerance = 1e-6f;
    cfg.geometryFile = "none";
    cfg.boundaries[BoundarySide::Bottom].kind = BoundaryKind::Wall;
    cfg.boundaries[BoundarySide::Top].kind = BoundaryKind::Wall;

    RestartData frame;
    if (!runCase(cfg, frame, error))
        return fail(error);

    const int nx = frame.nx, ny = frame.ny;
    const int column = static_cast<int>(nx * 0.9);

    double worst = 0.0;
    double peak = 0.0;
    for (int j = 0; j < ny; ++j) {
        const double y = (j + 0.5) / ny;
        const double exact = 6.0 * cfg.U0 * y * (1.0 - y);
        const double value = frame.u[j * (nx + 1) + column];
        peak = std::max(peak, value);
        worst = std::max(worst, std::fabs(value - exact));
    }

    const double relative = worst / (1.5 * cfg.U0);
    std::printf("  peak %.4f against the exact 1.5000, worst miss %.2f%%\n",
                peak, 100.0 * relative);

    if (!allFinite(frame.u))
        return fail("the channel run stopped being a number");
    if (!(relative < 0.05))
        return fail("the developed profile is not the Poiseuille parabola");
    if (!(std::fabs(peak - 1.5) < 0.06))
        return fail("the peak of the profile is in the wrong place");

    Config slip = cfg;
    slip.outputDir = (root / "slip").string();
    slip.totalTime = 1.5;
    slip.boundaries[BoundarySide::Bottom].kind = BoundaryKind::Slip;
    slip.boundaries[BoundarySide::Top].kind = BoundaryKind::Slip;

    RestartData slipFrame;
    if (!runCase(slip, slipFrame, error))
        return fail("free-slip channel: " + error);

    const double wallNoSlip = frame.u[column];
    const double wallSlip = slipFrame.u[column];
    std::printf("  row nearest the wall: no-slip %.3f, free-slip %.3f\n",
                wallNoSlip, wallSlip);
    if (!(wallNoSlip < 0.45 * cfg.U0))
        return fail("a no-slip wall is not slowing the fluid beside it");
    if (!(wallSlip > 0.8 * cfg.U0))
        return fail("a free-slip wall is dragging on the flow");

    removeDir(root);
    std::printf("ChannelTests OK\n");
    return 0;
}
