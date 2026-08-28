#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

double phaseVolume(const RestartData& frame) {
    double total = 0.0;
    for (float value : frame.phase)
        total += value;
    return total * frame.dx * frame.dy;
}

Config twoFluids(const std::filesystem::path& out) {
    Config cfg = baseConfig(out);
    cfg.geometryFile = "empty";
    cfg.phases = 2;
    cfg.rho1 = 1000.0f;
    cfg.rho2 = 1.225f;
    cfg.nu1 = 1e-6f;
    cfg.nu2 = 1.5e-5f;
    cfg.gravityMode = GravityMode::Body;
    cfg.mgIterations = 60;
    cfg.mgTolerance = 1e-5f;
    return cfg;
}

}   // namespace

int main() {
    const std::filesystem::path root = scratchDir("multiphase");
    std::string error;
    int rc = 0;

    // Nothing leaves a closed box, so however hard the lid stirs it, the amount
    // of fluid 1 in the domain is the amount it started with. This is the one
    // property an algebraic VOF scheme can actually lose, and it loses it
    // quietly: the interface just gets thinner every step until it is gone.
    {
        Config cfg = twoFluids(root / "conserve");
        cfg.Lx = 1.0f;
        cfg.Ly = 1.0f;
        cfg.nx = 48;
        cfg.ny = 48;
        cfg.U0 = 0.0f;
        cfg.totalTime = 3.0;
        cfg.caseType = CaseType::Cavity;
        cfg.lidSpeed = 1.0f;
        cfg.boundaries = cavityBoundaries(cfg.lidSpeed);
        cfg.phaseInit = PhaseInit::Drop;
        cfg.phaseLevel = 0.4f;
        cfg.phaseX = 0.5f;
        cfg.phaseY = 0.6f;
        cfg.gravityEnabled = false;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("stirred drop: " + error);
        if (frame.phase.size() != static_cast<size_t>(frame.nx) * frame.ny)
            return fail("the frame carries no phase field at all");
        if (!allFinite(frame.phase) || !allFinite(frame.u))
            return fail("stirred drop: the field stopped being a number");

        const double r = 0.4f * 0.5f * 1.0f;
        const double started = 3.14159265358979 * r * r;
        const double ended = phaseVolume(frame);
        const double drift = std::fabs(ended - started) / started;
        std::printf("  stirred drop   started %.6f m^2, ended %.6f, "
                    "drift %.3f%%\n", started, ended, 100.0 * drift);

        float lo = 2.0f, hi = -1.0f;
        for (float value : frame.phase) {
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        std::printf("  fraction stays in [%.3f, %.3f]\n", lo, hi);
        if (!(lo >= -1e-6f && hi <= 1.0f + 1e-6f))
            return fail("the volume fraction left [0, 1], which is not a "
                        "fraction of anything");
        if (!(drift < 0.03))
            return fail("the stirred drop lost or gained fluid");
        rc |= 0;
    }

    // Heavy under light in a closed box under real gravity is an exact
    // solution: the pressure gradient balances the weight and nothing moves.
    // Discretely it never quite does, and what is left over is the spurious
    // current every two phase solver has. It has to stay small next to the
    // speed the fluid would reach if it did fall, or the interface tears
    // itself apart while sitting still.
    {
        Config cfg = twoFluids(root / "still");
        cfg.Lx = 0.5f;
        cfg.Ly = 0.5f;
        cfg.nx = 48;
        cfg.ny = 48;
        cfg.U0 = 0.0f;
        cfg.totalTime = 0.5;
        cfg.gravityEnabled = true;
        cfg.gravityAccel = 9.81f;
        cfg.phaseInit = PhaseInit::Layer;
        cfg.phaseLevel = 0.5f;
        for (int side = 0; side < 4; ++side)
            cfg.boundaries.side[side].kind = BoundaryKind::Wall;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("still layer: " + error);
        if (!allFinite(frame.u) || !allFinite(frame.v))
            return fail("still layer: the field stopped being a number");

        const float worst = std::max(magnitude(frame.u), magnitude(frame.v));
        const double falling = std::sqrt(9.81 * cfg.Ly);
        std::printf("  still layer    spurious %.4e m/s against %.3f m/s "
                    "if it fell (%.2f%%)\n",
                    worst, falling, 100.0 * worst / falling);
        if (!(worst < 0.05 * falling))
            return fail("a layer that should be sitting still is moving");
    }

    // The column falls, and the fastest anything in it can be going is the
    // speed of something dropped from the top of it. Approaching that and not
    // passing it is the whole of the energy argument, and it is the one number
    // in a dam break that does not need a table.
    {
        Config cfg = twoFluids(root / "dam");
        cfg.Lx = 0.6f;
        cfg.Ly = 0.4f;
        cfg.nx = 96;
        cfg.ny = 64;
        cfg.U0 = 0.0f;
        cfg.totalTime = 0.25;
        cfg.gravityEnabled = true;
        cfg.gravityAccel = 9.81f;
        cfg.phaseInit = PhaseInit::Column;
        cfg.phaseX = 0.25f;
        cfg.phaseLevel = 0.75f;
        cfg.boundaries.side[0].kind = BoundaryKind::Wall;
        cfg.boundaries.side[1].kind = BoundaryKind::Wall;
        cfg.boundaries.side[2].kind = BoundaryKind::Wall;
        cfg.boundaries.side[3].kind = BoundaryKind::Outlet;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("dam break: " + error);
        if (!allFinite(frame.u) || !allFinite(frame.phase))
            return fail("dam break: the field stopped being a number");

        const double height = 0.75 * cfg.Ly;
        const double limit = std::sqrt(2.0 * 9.81 * height);
        const float worst = std::max(magnitude(frame.u), magnitude(frame.v));

        // Where the water has got to along the floor, as a fraction of the
        // domain. It started at a quarter and can only have moved one way.
        int front = 0;
        for (int i = 0; i < frame.nx; ++i)
            if (frame.phase[i] > 0.5f)
                front = i + 1;
        const double reached = static_cast<double>(front) / frame.nx;

        const double started = 0.25 * cfg.Lx * 0.75 * cfg.Ly;
        const double ended = phaseVolume(frame);
        std::printf("  dam break      |u|max %.3f m/s against sqrt(2gh) = "
                    "%.3f, front at %.2f of the floor\n",
                    worst, limit, reached);
        std::printf("                 volume %.6f m^2 from %.6f, drift %.3f%%\n",
                    ended, started, 100.0 * std::fabs(ended - started) / started);

        if (!(worst < 1.15 * limit))
            return fail("the collapsing column is moving faster than a free "
                        "fall from its own height, which is energy from "
                        "nowhere");
        if (!(worst > 0.5 * limit))
            return fail("the column is barely moving, so whatever is holding "
                        "it up is not physics");
        if (!(reached > 0.4))
            return fail("the water has not left the corner it started in");
        if (!(std::fabs(ended - started) / started < 0.05))
            return fail("the dam break lost or gained water");
    }

    if (rc == 0) {
        removeDir(root);
        std::printf("MultiphaseTests OK\n");
    }
    return rc;
}
