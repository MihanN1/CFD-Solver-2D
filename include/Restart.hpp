#pragma once
#include "Config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

// Windows hands out narrow strings in one code page and reads them back in
// another: the console speaks 866, argv speaks 1251, and filesystem::path
// assumes 1251 for both. Anything with Cyrillic in it therefore quietly stops
// existing. These two are the only sane way in and out.
// Elsewhere they are a plain path <-> string conversion.
std::filesystem::path narrowToPath(const std::string& text);
std::string pathToConsole(const std::filesystem::path& path);

std::filesystem::path resolveRestartPath(const std::string& path,
                                         std::string& error);

bool loadRestart(const std::filesystem::path& file,
                 RestartData& out,
                 std::string& error);
