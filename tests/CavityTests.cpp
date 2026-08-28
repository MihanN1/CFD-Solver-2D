#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

constexpr int kPoints = 17;

constexpr double kY[kPoints] = {
    1.0000, 0.9766, 0.9688, 0.9609, 0.9531, 0.8516, 0.7344, 0.6172, 0.5000,
    0.4531, 0.2813, 0.1719, 0.1016, 0.0703, 0.0625, 0.0547, 0.0000};

constexpr double kU100[kPoints] = {
    1.00000, 0.84123, 0.78871, 0.73722, 0.68717, 0.23151, 0.00332, -0.13641,
    -0.20581, -0.21090, -0.15662, -0.10150, -0.06434, -0.04775, -0.04192,
    -0.03717, 0.00000};

constexpr double kX[kPoints] = {
    1.0000, 0.9688, 0.9609, 0.9531, 0.9453, 0.9063, 0.8594, 0.8047, 0.5000,
    0.2344, 0.2266, 0.1563, 0.0938, 0.0781, 0.0703, 0.0625, 0.0000};

constexpr double kV100[kPoints] = {
    0.00000, -0.05906, -0.07391, -0.08864, -0.10313, -0.16914, -0.22445,
    -0.24533, 0.05454, 0.17527, 0.17507, 0.16077, 0.12317, 0.10890, 0.10091,
    0.09233, 0.00000};

double sampleU(const RestartData& frame, double y, int column) {
    const double t = y * frame.ny - 0.5;
    const int j = std::max(0, std::min(frame.ny - 2, static_cast<int>(t)));
    const double w = std::min(1.0, std::max(0.0, t - j));
    const auto cell = [&](int row) {
        const int left = row * (frame.nx + 1) + column;
        return 0.5 * (frame.u[left] + frame.u[left + 1]);
    };
    return cell(j) * (1.0 - w) + cell(j + 1) * w;
}

double sampleV(const RestartData& frame, double x, int row) {
    const double t = x * frame.nx - 0.5;
    const int i = std::max(0, std::min(frame.nx - 2, static_cast<int>(t)));
    const double w = std::min(1.0, std::max(0.0, t - i));
    const auto cell = [&](int column) {
        return 0.5 * (frame.v[row * frame.nx + column] +
                      frame.v[(row + 1) * frame.nx + column]);
    };
    return cell(i) * (1.0 - w) + cell(i + 1) * w;
}

}

int main() {
    const std::filesystem::path root = scratchDir("cavity");
    std::string error;

    Config cfg = baseConfig(root / "re100");
    cfg.Lx = 1.0f;
    cfg.Ly = 1.0f;
    cfg.nx = 64;
    cfg.ny = 64;
    cfg.U0 = 0.0f;
    cfg.nu = 0.01f;
    cfg.caseType = CaseType::Cavity;
    cfg.lidSpeed = 1.0f;
    cfg.boundaries = cavityBoundaries(cfg.lidSpeed);
    cfg.geometryFile = "empty";
    cfg.totalTime = 200.0;
    cfg.steadyTolerance = 1e-5f;
    cfg.mgIterations = 20;
    cfg.mgTolerance = 1e-6f;

    cfg.convection = ConvectionScheme::Muscl;
    cfg.limiter = LimiterKind::VanLeer;
    cfg.timeScheme = TimeScheme::RK2;

    RestartData frame;
    if (!runCase(cfg, frame, error))
        return fail(error);

    if (!allFinite(frame.u) || !allFinite(frame.v))
        return fail("the cavity run stopped being a number");

    if (!(frame.currentTime < cfg.totalTime - 1.0))
        return fail("the cavity never reached a steady state, so there is "
                    "nothing to compare against Ghia");

    int solidCells = 0;
    for (std::uint8_t value : frame.solid)
        solidCells += value ? 1 : 0;
    if (solidCells != 0)
        return fail("the cavity has " + std::to_string(solidCells) +
                    " solid cells in it - geometryFile=empty is not being "
                    "honoured and the verification circle is back");

    std::printf("  steady at t = %.2f s, %d steps, %dx%d\n",
                frame.currentTime, frame.step, frame.nx, frame.ny);
    std::printf("       y   Ghia u    solver u |      x   Ghia v    solver v\n");

    double worstU = 0.0;
    double worstV = 0.0;
    for (int k = 0; k < kPoints; ++k) {
        const double u = sampleU(frame, kY[k], frame.nx / 2);
        const double v = sampleV(frame, kX[k], frame.ny / 2);
        const bool onWall = k == 0 || k == kPoints - 1;
        if (!onWall) {
            worstU = std::max(worstU, std::fabs(u - kU100[k]));
            worstV = std::max(worstV, std::fabs(v - kV100[k]));
        }
        std::printf("  %6.4f %8.5f %10.5f |%s %6.4f %8.5f %10.5f\n",
                    kY[k], kU100[k], u, onWall ? " *" : "  ",
                    kX[k], kV100[k], v);
    }
    std::printf("  worst interior miss: u %.4f, v %.4f   "
                "(* = on the wall, not judged)\n",
                worstU, worstV);

    if (!(worstU < 0.025))
        return fail("the centreline u profile has drifted away from Ghia");
    if (!(worstV < 0.025))
        return fail("the centreline v profile has drifted away from Ghia");

    removeDir(root);
    std::printf("CavityTests OK\n");
    return 0;
}
