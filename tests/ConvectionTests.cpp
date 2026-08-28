#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

struct Row {
    const char* label;
    ConvectionScheme scheme;
    LimiterKind limiter;
    TimeScheme time;
    float vorticity;
};

}

int main() {
    const std::filesystem::path root = scratchDir("convection");
    std::string error;

    Row rows[] = {
        {"upwind", ConvectionScheme::Upwind, LimiterKind::VanLeer,
         TimeScheme::Euler, 0.0f},
        {"muscl minmod", ConvectionScheme::Muscl, LimiterKind::Minmod,
         TimeScheme::RK2, 0.0f},
        {"muscl vanLeer", ConvectionScheme::Muscl, LimiterKind::VanLeer,
         TimeScheme::RK2, 0.0f},
        {"muscl superbee", ConvectionScheme::Muscl, LimiterKind::Superbee,
         TimeScheme::RK2, 0.0f},
        {"central", ConvectionScheme::Central, LimiterKind::VanLeer,
         TimeScheme::RK3, 0.0f},
    };

    int index = 0;
    for (Row& row : rows) {
        Config cfg = baseConfig(root / std::to_string(index++));
        cfg.Lx = 4.0f;
        cfg.Ly = 2.0f;
        cfg.nx = 128;
        cfg.ny = 64;
        cfg.nu = 0.002f;
        cfg.totalTime = 0.6;
        cfg.mgIterations = 10;
        cfg.geometryFile = "";
        cfg.convection = row.scheme;
        cfg.limiter = row.limiter;
        cfg.timeScheme = row.time;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail(std::string(row.label) + ": " + error);
        if (!allFinite(frame.u) || !allFinite(frame.v) || !allFinite(frame.p))
            return fail(std::string(row.label) +
                        ": the field stopped being a number");
        row.vorticity = peakVorticity(frame);
        std::printf("  %-16s peak vorticity %.4f\n", row.label, row.vorticity);
    }

    const float upwind = rows[0].vorticity;
    const float minmod = rows[1].vorticity;
    const float vanLeer = rows[2].vorticity;
    const float central = rows[4].vorticity;

    if (!(upwind > 0.0f))
        return fail("the reference run produced no vorticity at all, so this "
                    "case cannot tell the schemes apart");

    if (!(minmod > upwind))
        return fail("muscl with minmod is not less dissipative than upwind");
    if (!(vanLeer > minmod))
        return fail("van Leer is not less dissipative than minmod");
    if (!(central > vanLeer))
        return fail("central is not less dissipative than a limited scheme");

    std::printf("  upwind -> central keeps %.0f%% more vorticity\n",
                100.0 * (central / upwind - 1.0));

    removeDir(root);
    std::printf("ConvectionTests OK\n");
    return 0;
}
