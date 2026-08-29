#pragma once
#include "Config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Bumped whenever the binary layout of a frame changes in a way an older reader
// cannot follow. Version 2 writes solid as one byte a cell and replaced the
// uFace/vFace/pRaw arrays with the packed block below, so a version 1 reader
// desynchronises on the solid array and cannot be rescued - the version is
// there so that the next change can be reported instead of guessed at.
constexpr int FRAME_FORMAT_VERSION = 2;

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

    std::vector<float> phase;

    std::vector<std::pair<std::string, std::vector<float>>> extras;

    struct BodyState {
        int object = 0;
        double x = 0.0, y = 0.0, theta = 0.0;
        float vx = 0.0f, vy = 0.0f, omega = 0.0f;
    };
    std::vector<BodyState> bodies;

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

// A face velocity is almost exactly the running average the cell velocity array
// already carries, so writing it out again spends four bytes a cell on
// something the frame nearly holds twice over. These two store what is left of
// it instead: the bit pattern of every face minus the bit pattern that average
// predicts, zigzag varint encoded. Lossless to the bit and about a quarter of
// the size, and the reconstruction cannot drift, because every face is
// corrected by its own delta before the next one is predicted from it.
// Both directions have to agree on the prediction down to the last ulp, so it
// is written once here and used from both sides.
std::string packFaceVelocities(int nx, int ny,
                               const std::vector<float>& u,
                               const std::vector<float>& v,
                               const std::vector<float>& uCell,
                               const std::vector<float>& vCell);

// False when the block is truncated or its checksum says the faces did not come
// back exactly. The caller then falls back to the cell averages, which is the
// path frames without the block already take.
bool unpackFaceVelocities(int nx, int ny,
                          const std::string& packed,
                          const std::vector<float>& uCell,
                          const std::vector<float>& vCell,
                          std::vector<float>& u,
                          std::vector<float>& v);

bool loadRestart(const std::filesystem::path& file,
                 RestartData& out,
                 std::string& error);
