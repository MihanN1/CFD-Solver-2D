#include "FluidSolverRun.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool contains(const std::vector<std::string>& arguments,
              const std::string& expected) {
    return std::find(arguments.begin(), arguments.end(), expected) !=
        arguments.end();
}

bool hasPrefix(const std::vector<std::string>& arguments,
               const std::string& prefix) {
    return std::any_of(
        arguments.begin(),
        arguments.end(),
        [&prefix](const std::string& argument) {
            return argument.rfind(prefix, 0) == 0;
        });
}

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path root =
        std::filesystem::absolute(
            std::filesystem::temp_directory_path() /
            ("mask-ui-fluid-solver-run-test-" + std::to_string(unique)));
    std::filesystem::create_directories(root);
    const std::filesystem::path geometry = root / "shape.obj";
    const std::filesystem::path output = root / "output";
    std::ofstream(geometry) << "# test\n";

    maskui::FluidSolverRunConfig config;
    config.geometryFile = geometry;
    config.dtUpdateInterval = 7;
    config.dtSafety = 0.8;
    config.smootherOmega = 1.1;
    config.mgIterations = 4;
    config.mgTolerance = 1e-5;
    config.mgMinCoarseSize = 6;
    config.saveInterval = 3;
    config.useCuda = false;

    std::vector<std::string> arguments;
    std::string error;
    if (!maskui::buildFluidSolverArguments(
            config, output, arguments, error)) {
        return fail("current-solver arguments failed: " + error);
    }
    if (arguments.size() != 24) {
        return fail(
            "current Fluid Solver contract must emit exactly 24 arguments");
    }
    if (!contains(arguments, "dtUpdateInterval=7") ||
        !contains(arguments, "dtSafety=0.80000000000000004") ||
        !contains(arguments, "smootherOmega=1.1000000000000001") ||
        !contains(arguments, "mgIterations=4") ||
        !contains(arguments, "mgTolerance=1.0000000000000001e-05") ||
        !contains(arguments, "mgMinCoarseSize=6") ||
        !contains(arguments, "saveInterval=3") ||
        !contains(arguments, "useCuda=0") ||
        !contains(arguments, "totalTime=10") ||
        !contains(arguments, "outputDir=" + output.u8string())) {
        return fail("current Fluid Solver arguments are incomplete");
    }
    if (hasPrefix(arguments, "gravity") ||
        hasPrefix(arguments, "restart=") ||
        hasPrefix(arguments, "restartFile=") ||
        hasPrefix(arguments, "addTime=")) {
        return fail("obsolete optional-gravity/restart hooks were emitted");
    }

    if (maskui::buildFluidSolverArguments(
            config, ".", arguments, error)) {
        return fail("relative outputDir was accepted for current Fluid Solver");
    }

    config.dtUpdateInterval = 0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("zero dtUpdateInterval was accepted");
    }
    config.dtUpdateInterval = 5;
    config.dtSafety = 1.01;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("dtSafety above one was accepted");
    }
    config.dtSafety = 0.9;
    config.saveInterval = 0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("zero saveInterval was accepted");
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
