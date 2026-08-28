#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

bool runAndKeep(Config cfg, RestartData& out, std::string& error) {
    const std::filesystem::path dir(cfg.outputDir);
    std::filesystem::create_directories(dir);
    {
        Quiet quiet;
        Mesh mesh(cfg, nullptr);
        if (!mesh.valid()) {
            error = mesh.error();
            return false;
        }
        Solver solver(cfg, mesh);
        solver.run();
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".vtk")
            continue;
        const auto stamp = std::filesystem::last_write_time(entry.path(), ec);
        if (newest.empty() || stamp >= newestTime) {
            newest = entry.path();
            newestTime = stamp;
        }
    }
    if (newest.empty()) {
        error = "no frame in " + dir.string();
        return false;
    }
    return loadRestart(newest, out, error);
}

}

int main() {
    const std::filesystem::path root = scratchDir("restart");
    std::string error;

    Config whole = baseConfig(root / "whole");
    whole.totalTime = 0.12;
    whole.mgIterations = 20;
    RestartData wholeFrame;
    if (!runAndKeep(whole, wholeFrame, error))
        return fail("whole run: " + error);

    Config half = baseConfig(root / "half");
    half.totalTime = 0.06;
    half.mgIterations = 20;
    RestartData halfFrame;
    if (!runAndKeep(half, halfFrame, error))
        return fail("first half: " + error);

    if (!halfFrame.exactState)
        return fail("the frame did not carry the face velocities, so a "
                    "continuation cannot be exact and this test is pointless");

    std::filesystem::path source;
    for (const auto& entry :
         std::filesystem::directory_iterator(root / "half")) {
        if (entry.path().extension() == ".vtk")
            source = entry.path();
    }

    Config rest = baseConfig(root / "rest");
    rest.restart = true;
    rest.restartFile = source.string();
    rest.totalTime = 0.12;
    rest.mgIterations = 20;

    RestartData restFrame;
    {
        Quiet quiet;
        RestartData loaded;
        if (!loadRestart(narrowToPath(rest.restartFile), loaded, error))
            return fail("loading the frame back: " + error);

        Config merged = loaded.cfg;
        merged.restart = true;
        merged.restartFile = rest.restartFile;
        merged.outputDir = rest.outputDir;
        merged.totalTime = rest.totalTime;

        std::filesystem::create_directories(rest.outputDir);
        Mesh mesh(merged, &loaded.solid);
        Solver solver(merged, mesh);
        if (!solver.setInitialState(std::move(loaded), "cont"))
            return fail("the solver refused the frame");
        solver.run();
    }
    {
        std::filesystem::path newest;
        std::filesystem::file_time_type newestTime{};
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(root / "rest", ec)) {
            if (entry.path().extension() != ".vtk")
                continue;
            const auto stamp = std::filesystem::last_write_time(entry.path(), ec);
            if (newest.empty() || stamp >= newestTime) {
                newest = entry.path();
                newestTime = stamp;
            }
        }
        if (newest.empty() || !loadRestart(newest, restFrame, error))
            return fail("continuation frame: " + error);
    }

    const float scaleU = magnitude(wholeFrame.u);
    const float scaleV = magnitude(wholeFrame.v);
    const float du = maxDifference(wholeFrame.u, restFrame.u) / scaleU;
    const float dv = maxDifference(wholeFrame.v, restFrame.v) / scaleV;

    std::printf("  straight through t=%.3f step %d, continued t=%.3f step %d\n",
                wholeFrame.currentTime, wholeFrame.step,
                restFrame.currentTime, restFrame.step);
    std::printf("  velocity differs by %.3e (u) and %.3e (v), relative\n", du, dv);

    if (std::fabs(wholeFrame.currentTime - restFrame.currentTime) > 1e-9)
        return fail("the continuation stopped at a different time");
    if (!(du < 5e-3f && dv < 5e-3f))
        return fail("continuing from a frame does not reproduce the run");

    removeDir(root);
    std::printf("RestartTests OK\n");
    return 0;
}
