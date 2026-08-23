// What a continuation run puts on the solver's command line, and what it
// refuses to put there.
//
// The rules these check are the ones that used to be wrong by omission: a
// continuation must not be asked to validate a geometry file it does not need,
// must carry restart= before the overrides, and must not hand a 0.1 solver an
// argument it will exit on.

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

maskui::FluidSolverRunConfig baseConfig() {
    maskui::FluidSolverRunConfig config;
    config.Lx = 2.0;
    config.Ly = 1.0;
    config.nx = 64;
    config.ny = 32;
    config.totalTime = 5.0;
    return config;
}

} // namespace

int main() {
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cfd-continuation-tests-" + std::to_string(unique));
    std::filesystem::create_directories(root);
    const std::filesystem::path output = root / "run";
    std::filesystem::create_directories(output);

    const std::filesystem::path frame = root / "solution_120.vtk";
    {
        std::ofstream stream(frame, std::ios::binary);
        stream << "# vtk DataFile Version 3.0\n";
    }

    std::string error;

    // A continuation validates without a geometry file, because the mask comes
    // out of the frame and the model may be long gone.
    maskui::FluidSolverRunConfig continuation = baseConfig();
    continuation.restart = true;
    continuation.restartFile = frame;
    if (!maskui::validateFluidSolverRunConfig(continuation, error)) {
        return fail("a continuation without a geometry file was rejected: " +
                    error);
    }

    // ... but it does need a frame that exists.
    maskui::FluidSolverRunConfig missingFrame = continuation;
    missingFrame.restartFile = root / "solution_999.vtk";
    if (maskui::validateFluidSolverRunConfig(missingFrame, error)) {
        return fail("a continuation from a frame that does not exist was "
                    "accepted");
    }

    std::vector<std::string> arguments;
    if (!maskui::buildFluidSolverArguments(
            continuation, output, arguments, error)) {
        return fail("continuation arguments were not built: " + error);
    }
    if (!contains(arguments, "restart=1")) {
        return fail("restart=1 is missing from a continuation");
    }
    if (!hasPrefix(arguments, "restartFile=")) {
        return fail("restartFile= is missing from a continuation");
    }
    // The solver reads the frame, replaces its configuration with the frame's,
    // and then applies the arguments in order - so restart= has to come before
    // the overrides it is meant to be overridden by nothing.
    if (arguments.front() != "restart=1") {
        return fail("restart=1 must be the first argument, found '" +
                    arguments.front() + "'");
    }
    if (hasPrefix(arguments, "geometryFile=") ||
        hasPrefix(arguments, "sliceAngleX=")) {
        return fail("a continuation must not send geometry arguments");
    }
    if (hasPrefix(arguments, "addTime=")) {
        return fail("addTime= must be left out when it is zero");
    }

    // addTime is what the solver adds to the time the frame stopped at.
    maskui::FluidSolverRunConfig added = continuation;
    added.addTime = 2.5;
    if (!maskui::buildFluidSolverArguments(added, output, arguments, error)) {
        return fail("continuation with addTime was not built: " + error);
    }
    if (!hasPrefix(arguments, "addTime=")) {
        return fail("addTime= is missing when it was asked for");
    }

    // A negative addTime is a mistake, not a shorter run.
    maskui::FluidSolverRunConfig negative = continuation;
    negative.addTime = -1.0;
    if (maskui::validateFluidSolverRunConfig(negative, error)) {
        return fail("a negative addTime was accepted");
    }

    // The 0.2 switches only exist when the solver is known to have them: a 0.1
    // build stops on the first argument it does not recognise.
    maskui::FluidSolverRunConfig fresh = baseConfig();
    fresh.geometryFile = root / "model.obj";
    {
        std::ofstream stream(fresh.geometryFile);
        stream << "v 0 0 0\n";
    }
    if (!maskui::buildFluidSolverArguments(fresh, output, arguments, error)) {
        return fail("a fresh run was not built: " + error);
    }
    if (hasPrefix(arguments, "avx2=") || hasPrefix(arguments, "openmp=") ||
        hasPrefix(arguments, "threads=") || hasPrefix(arguments, "tray=")) {
        return fail("runtime switches were sent to a solver that has none");
    }

    fresh.supportsRuntimeSwitches = true;
    fresh.useAvx2 = false;
    fresh.threads = 4;
    fresh.solverTray = false;
    if (!maskui::buildFluidSolverArguments(fresh, output, arguments, error)) {
        return fail("a fresh run with runtime switches was not built: " +
                    error);
    }
    if (!contains(arguments, "avx2=0")) {
        return fail("avx2=0 is missing");
    }
    if (!contains(arguments, "openmp=1")) {
        return fail("openmp=1 is missing");
    }
    if (!contains(arguments, "threads=4")) {
        return fail("threads=4 is missing");
    }
    if (!contains(arguments, "tray=0")) {
        return fail("tray=0 is missing");
    }
    if (!hasPrefix(arguments, "geometryFile=")) {
        return fail("a fresh run must still send its geometry");
    }

    // A negative thread count is a typo, not a request.
    fresh.threads = -1;
    if (maskui::validateFluidSolverRunConfig(fresh, error)) {
        return fail("a negative thread count was accepted");
    }

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    std::cout << "ContinuationTests passed\n";
    return 0;
}
