#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace maskui {

struct FluidSolverRunConfig {
    double Lx = 1.0;
    double Ly = 1.0;
    int nx = 50;
    int ny = 50;

    double U0 = 1.0;
    double nu = 0.01;
    double ro = 1.225;

    double CFL = 0.5;
    double totalTime = 10.0;
    int dtUpdateInterval = 5;
    double dtSafety = 0.9;

    double omega = 1.85;
    double smootherOmega = 1.15;
    int mgIterations = 2;
    double mgTolerance = 1e-4;
    int mgMinCoarseSize = 8;

    int saveInterval = 20;
    bool useCuda = true;

    std::filesystem::path geometryFile;
    double sliceAngleX = 0.0;
    double sliceAngleZ = 0.0;
    double sliceRotation = 0.0;
    bool invertSection = false;
};

bool validateFluidSolverRunConfig(const FluidSolverRunConfig& config,
                             std::string& error);

bool buildFluidSolverArguments(
    const FluidSolverRunConfig& config,
    const std::filesystem::path& outputDirectory,
    std::vector<std::string>& arguments,
    std::string& error);

bool writeFluidSolverArguments(
    const std::filesystem::path& filename,
    const std::vector<std::string>& arguments,
    std::string& error);

std::filesystem::path resolveFluidSolverExecutable(
    const std::filesystem::path& uiExecutable,
    const std::filesystem::path& configuredExecutable);

bool validateFluidSolverExecutable(
    const std::filesystem::path& executable,
    std::string& error);

bool writeFluidSolverSelection(
    const std::filesystem::path& selectionFile,
    const std::filesystem::path& executable,
    std::string& error);

std::optional<std::filesystem::path> readFluidSolverSelection(
    const std::filesystem::path& selectionFile,
    std::string& error);

} // namespace maskui
