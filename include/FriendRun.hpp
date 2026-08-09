#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maskui {

struct FriendRunConfig {
    double Lx = 1.0;
    double Ly = 1.0;
    int nx = 50;
    int ny = 50;

    double U0 = 1.0;
    double nu = 0.01;
    double ro = 1.225;

    double CFL = 0.5;
    double totalTime = 0.0;
    double omega = 1.85;

    std::filesystem::path geometryFile;
    double sliceAngleX = 0.0;
    double sliceAngleZ = 0.0;
    double sliceRotation = 0.0;
    bool invertSection = false;
};

bool validateFriendRunConfig(const FriendRunConfig& config,
                             std::string& error);

bool buildFriendSolverArguments(
    const FriendRunConfig& config,
    const std::filesystem::path& outputDirectory,
    std::vector<std::string>& arguments,
    std::string& error);

bool writeFriendSolverArguments(
    const std::filesystem::path& filename,
    const std::vector<std::string>& arguments,
    std::string& error);

std::filesystem::path resolveFriendSolverExecutable(
    const std::filesystem::path& uiExecutable,
    const std::filesystem::path& configuredExecutable);

bool validateFriendSolverExecutable(
    const std::filesystem::path& executable,
    std::string& error);

bool writeFriendSolverSelection(
    const std::filesystem::path& selectionFile,
    const std::filesystem::path& executable,
    std::string& error);

std::optional<std::filesystem::path> readFriendSolverSelection(
    const std::filesystem::path& selectionFile,
    std::string& error);

} // namespace maskui
