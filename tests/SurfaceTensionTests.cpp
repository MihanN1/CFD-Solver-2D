#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

Config dropCase(const std::filesystem::path& out) {
    Config cfg = baseConfig(out);
    cfg.geometryFile = "empty";
    cfg.Lx = 0.02f;
    cfg.Ly = 0.02f;
    cfg.nx = 64;
    cfg.ny = 64;
    cfg.U0 = 0.0f;
    cfg.phases = 2;
    cfg.rho1 = 1000.0f;
    cfg.rho2 = 100.0f;
    cfg.nu1 = 1e-6f;
    cfg.nu2 = 1.5e-5f;
    cfg.gravityEnabled = false;
    cfg.gravityMode = GravityMode::Body;
    cfg.surfaceTension = 0.072f;
    cfg.mgIterations = 80;
    cfg.mgTolerance = 1e-7f;
    for (int side = 0; side < 4; ++side)
        cfg.boundaries.side[side].kind = BoundaryKind::Wall;
    return cfg;
}

double interfaceLength(const RestartData& frame) {
    double total = 0.0;
    for (int j = 1; j < frame.ny - 1; ++j)
        for (int i = 1; i < frame.nx - 1; ++i) {
            const float gx = (frame.phase[j * frame.nx + i + 1] -
                              frame.phase[j * frame.nx + i - 1]) /
                             (2.0f * frame.dx);
            const float gy = (frame.phase[(j + 1) * frame.nx + i] -
                              frame.phase[(j - 1) * frame.nx + i]) /
                             (2.0f * frame.dy);
            total += std::sqrt(gx * gx + gy * gy);
        }
    return total * frame.dx * frame.dy;
}

}

int main() {
    const std::filesystem::path root = scratchDir("tension");
    std::string error;

    {
        Config cfg = dropCase(root / "laplace");
        cfg.phaseInit = PhaseInit::Drop;
        cfg.phaseLevel = 0.5f;
        cfg.totalTime = 0.05;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("laplace: " + error);
        if (!allFinite(frame.u) || !allFinite(frame.p))
            return fail("laplace: the field stopped being a number");

        const double radius = 0.5 * cfg.phaseLevel * cfg.Ly;
        const double exact = cfg.surfaceTension / radius;

        double inside = 0.0, outside = 0.0;
        int insideCells = 0, outsideCells = 0;
        for (size_t id = 0; id < frame.phase.size(); ++id) {
            if (frame.phase[id] > 0.99f) { inside += frame.p[id]; ++insideCells; }
            else if (frame.phase[id] < 0.01f) { outside += frame.p[id]; ++outsideCells; }
        }
        if (insideCells == 0 || outsideCells == 0)
            return fail("laplace: the drop is not there any more");
        const double jump = inside / insideCells - outside / outsideCells;

        const float worst = std::max(magnitude(frame.u), magnitude(frame.v));
        const double capillary =
            std::sqrt(cfg.surfaceTension / (cfg.rho1 * radius));

        std::printf("  laplace     jump %.4f Pa against sigma/R = %.4f "
                    "(%.2f%% off)\n",
                    jump, exact, 100.0 * std::fabs(jump - exact) / exact);
        std::printf("  spurious    %.3e m/s against %.4f m/s of capillary "
                    "wave (%.4f of it)\n",
                    worst, capillary, worst / capillary);

        if (!(std::fabs(jump - exact) < 0.05 * exact))
            return fail("the pressure inside the drop is not sigma/R, so the "
                        "curvature or the force built from it is wrong");
        if (!(worst < 8.0 * capillary))
            return fail("a drop that should be sitting still is boiling on "
                        "the spot");
    }

    {
        Config cfg = dropCase(root / "rounding");
        cfg.phaseInit = PhaseInit::Column;
        cfg.phaseX = 0.45f;
        cfg.phaseLevel = 0.45f;
        cfg.totalTime = 0.02;
        cfg.saveInterval = 100000;

        RestartData first;
        Config start = cfg;
        start.totalTime = 1e-6;
        start.outputDir = (root / "rounding-start").string();
        if (!runCase(start, first, error))
            return fail("rounding start: " + error);

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("rounding: " + error);
        if (!allFinite(frame.phase))
            return fail("rounding: the field stopped being a number");

        const double before = interfaceLength(first);
        const double after = interfaceLength(frame);
        std::printf("  rounding    interface %.5f m -> %.5f m (%.1f%% less)\n",
                    before, after, 100.0 * (before - after) / before);
        if (!(after < 0.97 * before))
            return fail("a square blob under surface tension did not get any "
                        "rounder, so the force is not pulling the way it "
                        "should");
    }

    removeDir(root);
    std::printf("SurfaceTensionTests OK\n");
    return 0;
}
