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

    config.saveInterval = 3;

    // Gravity and wallMotion are held back exactly like the 0.2 switches, and
    // appear once the executable has been found to understand them.
    config.supportsGravity = true;
    config.gravityEnabled = true;
    config.gravityAccel = 9.81;
    config.gravityAngle = 30.0;
    config.supportsWallMotion = true;
    config.wallMotion = "1:rot=90;2:slip=1";
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("gravity/wallMotion arguments failed: " + error);
    }
    if (!contains(arguments, "gravityEnabled=1") ||
        !hasPrefix(arguments, "gravityAccel=") ||
        !hasPrefix(arguments, "gravityAngle=") ||
        !contains(arguments, "wallMotion=1:rot=90;2:slip=1")) {
        return fail("gravity/wallMotion arguments are incomplete");
    }
    config.wallMotion = "1:rot=90\n2:slip=1";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a multi-line wallMotion was accepted");
    }
    config.wallMotion.clear();
    config.gravityAccel = -1.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a negative gravityAccel was accepted");
    }
    config.gravityAccel = 9.81;

    // Sides, then the case preset that replaces all of them.
    config.supportsBoundaries = true;
    config.boundaryKind[0] = "inlet";
    config.boundaryKind[3] = "movingWall";
    config.boundarySpeed[3] = 2.0;
    config.inletFrom = 0.25;
    config.inletTo = 0.75;
    config.inletProfile = "parabolic";
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("boundary arguments failed: " + error);
    }
    if (!contains(arguments, "bcLeft=inlet") ||
        !contains(arguments, "bcTop=movingWall") ||
        !contains(arguments, "bcTopSpeed=2") ||
        !contains(arguments, "inletProfile=parabolic") ||
        !hasPrefix(arguments, "inletFrom=")) {
        return fail("boundary arguments are incomplete");
    }
    // The trap from the last branch: a zero speed written out for an inlet
    // tells the solver a standstill was asked for.
    if (hasPrefix(arguments, "bcLeftSpeed=")) {
        return fail("a zero inlet speed was written out again");
    }

    config.supportsCase = true;
    config.steadyTolerance = 1e-5;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("case arguments failed: " + error);
    }
    if (!contains(arguments, "caseType=channel") ||
        !hasPrefix(arguments, "steadyTolerance=") ||
        !contains(arguments, "bcLeft=inlet")) {
        return fail("a channel case must still send its own sides");
    }
    if (hasPrefix(arguments, "lidSpeed=")) {
        return fail("a channel has no lid to give a speed to");
    }

    config.caseType = "cavity";
    config.lidSpeed = 1.5;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("cavity arguments failed: " + error);
    }
    if (!contains(arguments, "caseType=cavity") ||
        !contains(arguments, "lidSpeed=1.5")) {
        return fail("cavity arguments are incomplete");
    }
    // The preset owns all four sides. Sending the boundary rows after it would
    // undo it and quietly give back a channel.
    if (hasPrefix(arguments, "bcLeft=") || hasPrefix(arguments, "bcTop=") ||
        hasPrefix(arguments, "inletFrom=")) {
        return fail("the cavity preset was overwritten by the boundary rows");
    }

    // Four walls and no outlet is what a cavity is, and only a channel has to
    // give the fluid somewhere to go.
    for (int side = 0; side < 4; ++side) {
        config.boundaryKind[side] = "wall";
    }
    config.boundaryKind[0] = "inlet";
    if (!maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a cavity was refused over its sides: " + error);
    }
    config.caseType = "channel";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("an inlet with no outlet was accepted");
    }
    config.caseType = "cavity";

    config.steadyTolerance = -1.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a negative steadyTolerance was accepted");
    }
    config.steadyTolerance = 0.0;
    config.caseType = "vortex street please";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("an unknown caseType was accepted");
    }
    config.caseType = "cavity";

    // An empty domain is a real answer to "what geometry", and the only way
    // the UI can ask for a cavity with nothing floating in the middle of it.
    config.geometryFile = "empty";
    if (!maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("geometryFile=empty was refused: " + error);
    }
    config.geometryFile = root / "not-here.obj";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a geometry file that does not exist was accepted");
    }
    config.geometryFile = geometry;
    config.caseType = "channel";
    for (int side = 0; side < 4; ++side) {
        config.boundaryKind[side] = "slip";
    }
    config.boundaryKind[0] = "inlet";
    config.boundaryKind[1] = "outlet";

    // Two fluids, and everything that only exists at two fluids.
    config.supportsPhases = true;
    config.phases = 2;
    config.gravityEnabled = true;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("two phase arguments failed: " + error);
    }
    if (!contains(arguments, "phases=2") ||
        !contains(arguments, "rho1=1000") ||
        !contains(arguments, "vofScheme=hric") ||
        !contains(arguments, "phaseInit=layer") ||
        !hasPrefix(arguments, "nu1=") ||
        !hasPrefix(arguments, "phaseLevel=")) {
        return fail("two phase arguments are incomplete");
    }

    // A painted field is not one of the three built in shapes and has to win
    // over whichever one the row happens to be showing.
    config.initialPhaseFile = root / "initial-phase.txt";
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("painted phase arguments failed: " + error);
    }
    if (!contains(arguments, "phaseInit=file") ||
        !hasPrefix(arguments, "initialPhaseFile=")) {
        return fail("the painted field was not sent");
    }
    if (hasPrefix(arguments, "phaseLevel=")) {
        return fail("a built in shape was sent alongside the painted field");
    }
    config.initialPhaseFile.clear();

    config.gravityEnabled = false;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("two fluids with gravity off were accepted, and they never "
                    "separate");
    }
    config.gravityEnabled = true;
    config.phases = 3;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("three phases were accepted");
    }
    config.phases = 2;
    config.vofScheme = "plic";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a VOF scheme this solver does not have was accepted");
    }
    config.vofScheme = "hric";

    // A source pushes fluid in from inside and needs an outlet exactly as an
    // inlet does.
    config.sources = "x=0.5,y=0.2,r=0.05,rate=2,angle=90";
    if (!maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a source with an outlet was refused: " + error);
    }
    config.boundaryKind[1] = "wall";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a source with nowhere for the fluid to go was accepted");
    }
    config.boundaryKind[1] = "outlet";
    config.sources.clear();
    config.phases = 1;

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
