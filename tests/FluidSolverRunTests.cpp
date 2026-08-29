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

    if (hasPrefix(arguments, "bcLeft=") || hasPrefix(arguments, "bcTop=") ||
        hasPrefix(arguments, "inletFrom=")) {
        return fail("the cavity preset was overwritten by the boundary rows");
    }

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

    if (hasPrefix(arguments, "surfaceTension=") ||
        hasPrefix(arguments, "mixing=")) {
        return fail("tension keys were sent to a solver that has no idea what "
                    "they are");
    }
    config.supportsTension = true;
    config.surfaceTension = 0.0625;
    config.contactAngle = 40.0;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("tension arguments failed: " + error);
    }
    if (!contains(arguments, "surfaceTension=0.0625") ||
        !contains(arguments, "contactAngle=40") ||
        !contains(arguments, "mixing=immiscible") ||
        !hasPrefix(arguments, "diffusivity=")) {
        return fail("the tension block is incomplete");
    }
    config.surfaceTension = -1.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a negative surface tension was accepted");
    }
    config.surfaceTension = 0.0625;
    config.contactAngle = 200.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a contact angle past 180 degrees was accepted");
    }
    config.contactAngle = 90.0;
    config.mixing = "miscible";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("fluids that mix were given a surface to pull on");
    }
    config.surfaceTension = 0.0;
    if (!maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("miscible with no tension was refused: " + error);
    }
    config.diffusivity = -1.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a negative diffusivity was accepted");
    }
    config.diffusivity = 1e-6;
    config.mixing = "immiscible";
    config.supportsTension = false;

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

    if (hasPrefix(arguments, "bodyMotion=") ||
        hasPrefix(arguments, "bodyCoupling=")) {
        return fail("body keys were sent to a solver that has never heard of "
                    "them");
    }
    config.supportsBodyMotion = true;
    config.bodyMotion = "1:vx=0.3,omega=45";
    config.bodyCoupling = "strong";
    config.bodyCollisions = true;
    config.bodyRestitution = 0.5;
    config.bodyForceReport = true;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("body arguments failed: " + error);
    }
    if (!contains(arguments, "bodyMotion=1:vx=0.3,omega=45") ||
        !contains(arguments, "bodyCoupling=strong") ||
        !contains(arguments, "bodyCollisions=1") ||
        !contains(arguments, "bodyRestitution=0.5") ||
        !contains(arguments, "bodyForceReport=1")) {
        return fail("the body block is incomplete");
    }
    config.bodyCollisions = false;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("body arguments without collisions failed: " + error);
    }
    if (hasPrefix(arguments, "bodyRestitution=")) {
        return fail("a bounciness was sent for bodies that cannot collide");
    }
    config.bodyCoupling = "telepathic";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a coupling this solver does not have was accepted");
    }
    config.bodyCoupling = "added";
    config.bodyCollisions = true;
    config.bodyRestitution = 1.5;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a bounciness above one was accepted, and it would add "
                    "energy on every bounce");
    }
    config.bodyRestitution = 0.2;
    config.bodyMotion = "1:vx=0.3\n2:free=1";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a multi-line bodyMotion was accepted");
    }
    config.bodyMotion = "1:vx=0.3";
    config.geometryFile = "empty";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a body was told to travel through an empty domain");
    }
    config.geometryFile = geometry;
    config.bodyMotion.clear();
    config.bodyCollisions = false;
    config.bodyForceReport = false;

    if (hasPrefix(arguments, "turbulence=") || hasPrefix(arguments, "Cs=")) {
        return fail("turbulence keys were sent to a solver that has no model "
                    "in it");
    }
    config.supportsTurbulence = true;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("turbulence off arguments failed: " + error);
    }
    if (!contains(arguments, "turbulence=none") ||
        hasPrefix(arguments, "Cs=") ||
        hasPrefix(arguments, "turbIntensity=")) {
        return fail("turbulence off still sent the model's own settings");
    }
    config.turbulence = "smagorinsky";
    config.turbulenceCs = 0.1;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("smagorinsky arguments failed: " + error);
    }
    if (!contains(arguments, "turbulence=smagorinsky") ||
        !contains(arguments, "Cs=0.10000000000000001") ||
        hasPrefix(arguments, "turbIntensity=")) {
        return fail("smagorinsky was sent the inlet settings only kOmegaSST "
                    "reads");
    }
    config.turbulence = "kOmegaSST";
    config.turbIntensity = 0.03;
    config.turbLengthScale = 0.005;
    if (!maskui::buildFluidSolverArguments(config, output, arguments, error)) {
        return fail("k-omega arguments failed: " + error);
    }
    if (!contains(arguments, "turbulence=kOmegaSST") ||
        !contains(arguments, "turbIntensity=0.029999999999999999") ||
        !contains(arguments, "turbLengthScale=0.0050000000000000001") ||
        hasPrefix(arguments, "Cs=")) {
        return fail("the k-omega block is incomplete");
    }
    config.turbulence = "les please";
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a turbulence model this solver does not have was "
                    "accepted");
    }
    config.turbulence = "smagorinsky";
    config.turbulenceCs = 0.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a zero Cs was accepted, and it turns the model off by "
                    "the back door");
    }
    config.turbulenceCs = 0.17;
    config.turbIntensity = 1.5;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("an inlet more turbulent than it is moving was accepted");
    }
    config.turbIntensity = 0.05;
    config.turbLengthScale = -1.0;
    if (maskui::validateFluidSolverRunConfig(config, error)) {
        return fail("a negative eddy was accepted");
    }
    config.turbLengthScale = 0.0;
    config.turbulence = "none";

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
