#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

int checkMassBalance(const RestartData& frame, const char* label) {
    const int nx = frame.nx, ny = frame.ny;
    double inflow = 0.0, outflow = 0.0;
    for (int j = 0; j < ny; ++j) {
        inflow += frame.u[j * (nx + 1)] * frame.dy;
        outflow += frame.u[j * (nx + 1) + nx] * frame.dy;
    }
    for (int i = 0; i < nx; ++i) {
        inflow += frame.v[i] * frame.dx;
        outflow += frame.v[ny * nx + i] * frame.dx;
    }

    const double scale = std::max(std::fabs(inflow), 1e-9);
    const double imbalance = std::fabs(inflow - outflow) / scale;
    std::printf("  %-14s in %.6f out %.6f  imbalance %.3e\n",
                label, inflow, outflow, imbalance);
    if (!(imbalance < 2e-3))
        return fail(std::string(label) + ": mass does not balance");
    return 0;
}

int checkCase(Config cfg, const char* label, float divergenceLimit) {
    RestartData frame;
    std::string error;
    if (!runCase(cfg, frame, error))
        return fail(std::string(label) + ": " + error);

    if (!allFinite(frame.u) || !allFinite(frame.v) || !allFinite(frame.p))
        return fail(std::string(label) + ": the field stopped being a number");

    const float divergence = maxDivergence(frame);
    std::printf("  %-14s max divergence %.3e\n", label, divergence);
    if (!(divergence < divergenceLimit))
        return fail(std::string(label) + ": the projection left a divergence of " +
                    std::to_string(divergence));

    return checkMassBalance(frame, label);
}

}

int main() {
    const std::filesystem::path root = scratchDir("conservation");
    int rc = 0;

    {
        Config cfg = baseConfig(root / "plain");
        cfg.mgIterations = 30;
        cfg.mgTolerance = 1e-6f;
        rc |= checkCase(cfg, "empty channel", 1e-3f);
    }
    {
        Config cfg = baseConfig(root / "body");
        cfg.mgIterations = 30;
        cfg.mgTolerance = 1e-6f;
        cfg.geometryFile = "";
        rc |= checkCase(cfg, "with a body", 5e-3f);
    }
    {
        Config cfg = baseConfig(root / "walls");
        cfg.mgIterations = 30;
        cfg.mgTolerance = 1e-6f;
        cfg.boundaries[BoundarySide::Bottom].kind = BoundaryKind::Wall;
        cfg.boundaries[BoundarySide::Top].kind = BoundaryKind::Wall;
        rc |= checkCase(cfg, "no-slip walls", 1e-3f);
    }
    {
        Config cfg = baseConfig(root / "hydrostatic");
        cfg.Lx = 1.0f;
        cfg.Ly = 1.0f;
        cfg.nx = 48;
        cfg.ny = 48;
        cfg.U0 = 0.0f;
        cfg.totalTime = 0.2;
        cfg.mgIterations = 40;
        cfg.mgTolerance = 1e-7f;
        cfg.gravityEnabled = true;
        cfg.gravityMode = GravityMode::Body;
        cfg.gravityAccel = 9.81f;

        RestartData frame;
        std::string error;
        if (!runCase(cfg, frame, error))
            return fail(std::string("hydrostatic: ") + error);

        const float speed = std::max(magnitude(frame.u), magnitude(frame.v));
        std::printf("  %-14s largest speed at rest %.3e\n", "hydrostatic", speed);
        if (!(speed < 1e-3f))
            return fail("gravity as a body force stirred a fluid at rest");
    }

    removeDir(root);
    if (rc)
        return rc;
    std::printf("ConservationTests OK\n");
    return 0;
}
