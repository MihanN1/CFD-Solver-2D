#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Restart.hpp"
#include "Runtime.hpp"
#include "Solver.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

inline int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

inline void report(const std::string& message) {
    std::cout << "  " << message << "\n";
}

// The solver talks a lot and a test that prints a whole run is a test nobody
// reads. Everything it says goes into a string that is only printed when
// something fails.
class Quiet {
public:
    Quiet() : previous(std::cout.rdbuf(captured.rdbuf())) {}
    ~Quiet() { std::cout.rdbuf(previous); }
    std::string text() const { return captured.str(); }

private:
    std::ostringstream captured;
    std::streambuf* previous;
};

inline std::filesystem::path scratchDir(const std::string& name) {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cfd-test-" + name + "-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    return root;
}

inline void removeDir(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// A configuration that runs in a fraction of a second and has nothing in it
// that a test did not ask for.
inline Config baseConfig(const std::filesystem::path& out) {
    Config cfg;
    cfg.Lx = 2.0f;
    cfg.Ly = 1.0f;
    cfg.nx = 64;
    cfg.ny = 32;
    cfg.U0 = 1.0f;
    cfg.nu = 0.01f;
    cfg.totalTime = 0.1;
    cfg.saveInterval = 1000000;
    cfg.useCuda = false;
    cfg.geometryFile = "none";
    cfg.outputDir = out.string();
    return cfg;
}

// Runs a whole simulation and hands back the last frame it wrote. Every test
// below works from that rather than reaching inside the solver, so what is
// checked is what a user would actually get.
inline bool runCase(Config cfg,
                    RestartData& out,
                    std::string& error,
                    const std::string& framePrefix = "") {
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
        if (!framePrefix.empty()) {
            RestartData empty;
            (void)empty;
        }
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
        error = "the run wrote no frame into " + dir.string();
        return false;
    }
    return loadRestart(newest, out, error);
}

inline float maxDivergence(const RestartData& frame) {
    const int nx = frame.nx, ny = frame.ny;
    const float invDx = 1.0f / frame.dx;
    const float invDy = 1.0f / frame.dy;
    float worst = 0.0f;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            if (frame.solid[j * nx + i])
                continue;
            const float div =
                (frame.u[j * (nx + 1) + i + 1] - frame.u[j * (nx + 1) + i]) * invDx +
                (frame.v[(j + 1) * nx + i] - frame.v[j * nx + i]) * invDy;
            worst = std::max(worst, std::fabs(div));
        }
    return worst;
}

inline float peakVorticity(const RestartData& frame) {
    const int nx = frame.nx, ny = frame.ny;
    float worst = 0.0f;
    for (int j = 1; j < ny - 1; ++j)
        for (int i = 1; i < nx - 1; ++i) {
            const float dvdx =
                (frame.v[j * nx + i + 1] - frame.v[j * nx + i - 1]) /
                (2.0f * frame.dx);
            const float dudy =
                (frame.u[(j + 1) * (nx + 1) + i] -
                 frame.u[(j - 1) * (nx + 1) + i]) /
                (2.0f * frame.dy);
            worst = std::max(worst, std::fabs(dvdx - dudy));
        }
    return worst;
}

inline bool allFinite(const std::vector<float>& values) {
    for (float value : values)
        if (!std::isfinite(value))
            return false;
    return true;
}

inline float maxDifference(const std::vector<float>& a,
                           const std::vector<float>& b) {
    if (a.size() != b.size())
        return std::numeric_limits<float>::infinity();
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    return worst;
}

inline float magnitude(const std::vector<float>& values) {
    float worst = 0.0f;
    for (float value : values)
        worst = std::max(worst, std::fabs(value));
    return worst > 0.0f ? worst : 1.0f;
}

}   // namespace testing
