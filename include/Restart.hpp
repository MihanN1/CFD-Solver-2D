#pragma once
#include "Config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// State pulled back out of a frame written by Solver::saveVTK().
// The cell-centred VECTORS block in a VTK frame is an average of two face
// values, and averaging is not invertible, so it cannot seed a staggered run
// on its own. saveVTK therefore appends a FIELD block holding the raw face
// arrays plus the configuration text; that block is what this reads.
// Frames written before the block existed still load, but only approximately
// (exactState stays false and the solver projects the field once).
struct RestartData {
    Config cfg;                    // configuration the source run was started with
    double currentTime = 0.0;
    int step = 0;
    float dt = 0.0f;

    int nx = 0, ny = 0;
    float dx = 0.0f, dy = 0.0f;    // from SPACING, used to sanity-check Lx/Ly

    std::vector<uint8_t> solid;    // nx * ny
    std::vector<float> u;          // (nx + 1) * ny, face values
    std::vector<float> v;          // nx * (ny + 1), face values
    std::vector<float> p;          // nx * ny, kinematic (no ro scaling)

    bool exactState = false;       // false when u/v were rebuilt from cell data
    bool hasConfigText = false;    // false for frames of an older format
};

// Takes a .vtk path or a directory. For a directory the most recently written
// .vtk in it wins, which is what "continue where it stopped" means in practice
// and works for both naming schemes. Returns an empty path on failure.
std::filesystem::path resolveRestartPath(const std::string& path,
                                         std::string& error);

bool loadRestart(const std::filesystem::path& file,
                 RestartData& out,
                 std::string& error);
