#include "FluidSolverRun.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace maskui {
namespace {

constexpr int kMinimumGridDimension = 8;
constexpr int kMaximumGridDimension = 1'000'000;
constexpr std::size_t kMaximumGridCells = 100'000'000;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

bool requireFinite(const char* name, double value, std::string& error) {
    if (!std::isfinite(value)) {
        return fail(error, std::string(name) + " must be finite");
    }
    return true;
}

bool requirePositive(const char* name, double value, std::string& error) {
    if (!requireFinite(name, value, error)) {
        return false;
    }
    if (value <= 0.0) {
        return fail(error, std::string(name) + " must be positive");
    }
    return true;
}

bool requireNonNegative(const char* name,
                        double value,
                        std::string& error) {
    if (!requireFinite(name, value, error)) {
        return false;
    }
    if (value < 0.0) {
        return fail(error, std::string(name) + " must be non-negative");
    }
    return true;
}

bool validateGrid(const FluidSolverRunConfig& config, std::string& error) {
    if (config.nx < kMinimumGridDimension ||
        config.nx > kMaximumGridDimension) {
        return fail(error, "nx must be in [8, 1000000]");
    }
    if (config.ny < kMinimumGridDimension ||
        config.ny > kMaximumGridDimension) {
        return fail(error, "ny must be in [8, 1000000]");
    }

    const std::size_t width = static_cast<std::size_t>(config.nx);
    const std::size_t height = static_cast<std::size_t>(config.ny);
    if (width > std::numeric_limits<std::size_t>::max() / height ||
        width * height > kMaximumGridCells) {
        return fail(error, "nx * ny exceeds the 100000000-cell limit");
    }
    return true;
}

bool validateGeometry(const std::filesystem::path& filename,
                      std::string& error) {
    const std::string pathText = filename.string();
    if (pathText.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "geometryFile must not contain CR or LF");
    }

    {
        std::string lowered = pathText;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (lowered == "empty" || lowered == "none")
            return true;
    }
    if (filename.empty()) {
        return fail(error, "geometryFile must not be empty");
    }

    std::error_code filesystemError;
    const bool isFile =
        std::filesystem::is_regular_file(filename, filesystemError);
    if (filesystemError || !isFile) {
        return fail(
            error,
            "geometryFile must name an existing regular file: " + pathText);
    }

    std::string extension = filename.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (extension != ".obj" && extension != ".stl") {
        return fail(error, "geometryFile extension must be .obj or .stl");
    }
    return true;
}

bool isRegularFile(const std::filesystem::path& filename) {
    std::error_code error;
    const bool regular =
        std::filesystem::is_regular_file(filename, error);
    return !error && regular;
}

bool validateArgumentPath(const char* name,
                          const std::filesystem::path& path,
                          std::string& error);

std::string serializeDouble(double value) {
    std::ostringstream output;
    output << std::setprecision(
        std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

bool validateArgumentPath(const char* name,
                          const std::filesystem::path& path,
                          std::string& error) {
    const std::string text = path.u8string();
    if (text.empty()) {
        return fail(error, std::string(name) + " must not be empty");
    }
    if (text.find_first_of("\r\n") != std::string::npos) {
        return fail(error, std::string(name) + " must not contain CR or LF");
    }
    return true;
}

} // namespace

bool validateFluidSolverRunConfig(const FluidSolverRunConfig& config,
                             std::string& error) {
    error.clear();

    if (!validateGrid(config, error) ||
        !requirePositive("Lx", config.Lx, error) ||
        !requirePositive("Ly", config.Ly, error) ||
        !requireNonNegative("U0", config.U0, error) ||
        !requirePositive("nu", config.nu, error) ||
        !requirePositive("ro", config.ro, error) ||
        !requirePositive("CFL", config.CFL, error) ||
        !requirePositive("totalTime", config.totalTime, error) ||
        !requirePositive("dtSafety", config.dtSafety, error) ||
        !requirePositive("omega", config.omega, error) ||
        !requirePositive("smootherOmega", config.smootherOmega, error) ||
        !requirePositive("mgTolerance", config.mgTolerance, error) ||
        !requireFinite("sliceAngleX", config.sliceAngleX, error) ||
        !requireFinite("sliceAngleZ", config.sliceAngleZ, error) ||
        !requireFinite("sliceRotation", config.sliceRotation, error)) {
        return false;
    }

    if (config.dtUpdateInterval <= 0) {
        return fail(error, "dtUpdateInterval must be positive");
    }
    if (config.mgIterations <= 0) {
        return fail(error, "mgIterations must be positive");
    }
    if (config.mgMinCoarseSize <= 0) {
        return fail(error, "mgMinCoarseSize must be positive");
    }
    if (config.saveInterval <= 0) {
        return fail(error, "saveInterval must be positive");
    }
    if (config.CFL > 1.0) {
        return fail(error, "CFL must be in (0, 1]");
    }
    if (config.dtSafety > 1.0) {
        return fail(error, "dtSafety must be in (0, 1]");
    }
    if (config.omega >= 2.0) {
        return fail(error, "omega must be in (0, 2)");
    }
    if (config.smootherOmega >= 2.0) {
        return fail(error, "smootherOmega must be in (0, 2)");
    }
    if (!requireNonNegative("addTime", config.addTime, error)) {
        return false;
    }
    if (config.threads < 0) {
        return fail(error, "threads must be zero (all cores) or positive");
    }
    if (!requireNonNegative("gravityAccel", config.gravityAccel, error) ||
        !requireFinite("gravityAngle", config.gravityAngle, error)) {
        return false;
    }
    if (config.wallMotion.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "wallMotion must not contain CR or LF");
    }
    if (config.bodyMotion.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "bodyMotion must not contain CR or LF");
    }
    if (config.supportsBodyMotion && !config.bodyMotion.empty()) {
        if (config.bodyCoupling != "weak" && config.bodyCoupling != "added" &&
            config.bodyCoupling != "strong") {
            return fail(error, "bodyCoupling is weak, added or strong");
        }
        if (!(config.bodyRestitution >= 0.0) ||
            !(config.bodyRestitution <= 1.0)) {
            return fail(error, "bodyRestitution is how much of the closing "
                               "speed survives a bounce, so it lives between "
                               "0 and 1");
        }

        if (config.geometryFile == "empty" && config.profiles.empty()) {
            return fail(error, "bodyMotion needs a model to move: an empty "
                               "domain has no body in it, and a body has to "
                               "have its outline cut again wherever it goes");
        }
    }
    if (config.supportsTurbulence && config.turbulence != "none") {
        if (config.turbulence != "smagorinsky" &&
            config.turbulence != "kOmegaSST") {
            return fail(error, "turbulence is none, smagorinsky or kOmegaSST");
        }
        if (!(config.turbulenceCs > 0.0) || !(config.turbulenceCs <= 1.0)) {
            return fail(error, "Cs is the Smagorinsky constant and lives "
                               "between 0 and 1; 0.17 is the value it was "
                               "derived at and 0.1 is what channels want");
        }
        if (!(config.turbIntensity >= 0.0) ||
            !(config.turbIntensity <= 1.0)) {
            return fail(error, "turbIntensity is a fraction of the inlet "
                               "speed, so it lives between 0 and 1: 0.05 is "
                               "five percent");
        }
        if (!(config.turbLengthScale >= 0.0) ||
            !std::isfinite(config.turbLengthScale)) {
            return fail(error, "turbLengthScale cannot be negative; zero lets "
                               "the solver take a tenth of the domain height");
        }
    }
    if (config.profiles.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "profiles must not contain CR or LF");
    }
    if (config.extraFields.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "extraFields must not contain CR or LF");
    }
    if (config.phases != 1 && config.phases != 2) {
        return fail(error, "phases is 1 or 2: a second phase field would need "
                           "a rule for what happens where three fluids meet, "
                           "and there is not one");
    }
    if (config.phases > 1) {
        if (!(config.rho1 > 0.0) || !(config.rho2 > 0.0) ||
            !std::isfinite(config.rho1) || !std::isfinite(config.rho2)) {
            return fail(error, "both densities have to be positive numbers");
        }
        if (!(config.nu1 >= 0.0) || !(config.nu2 >= 0.0) ||
            !std::isfinite(config.nu1) || !std::isfinite(config.nu2)) {
            return fail(error, "neither viscosity can be negative");
        }
        if (config.vofScheme != "upwind" && config.vofScheme != "hric" &&
            config.vofScheme != "cicsam") {
            return fail(error, "vofScheme is upwind, hric or cicsam");
        }
        if (config.phaseInit != "layer" && config.phaseInit != "drop" &&
            config.phaseInit != "column" && config.phaseInit != "file") {
            return fail(error, "phaseInit is layer, drop, column or file");
        }
        for (double fraction : {config.phaseLevel, config.phaseX,
                                config.phaseY}) {
            if (!(fraction >= 0.0 && fraction <= 1.0))
                return fail(error, "phaseLevel, phaseX and phaseY are "
                                   "fractions of the domain, so they live "
                                   "between 0 and 1");
        }
        if (config.supportsTension) {
            if (config.mixing != "immiscible" && config.mixing != "miscible")
                return fail(error, "mixing is immiscible or miscible");
            if (!(config.diffusivity >= 0.0) ||
                !std::isfinite(config.diffusivity))
                return fail(error, "diffusivity cannot be negative");
            if (!(config.surfaceTension >= 0.0) ||
                !std::isfinite(config.surfaceTension))
                return fail(error, "surfaceTension cannot be negative");
            if (!(config.contactAngle >= 0.0) ||
                !(config.contactAngle <= 180.0))
                return fail(error, "contactAngle is measured inside fluid 1 "
                                   "and lives between 0 and 180 degrees");
            if (config.mixing == "miscible" && config.surfaceTension > 0.0)
                return fail(error, "fluids that mix have no surface for "
                                   "tension to pull on: set surfaceTension to "
                                   "zero, or make them immiscible");
        }
        if (!config.gravityEnabled)
            return fail(error, "two fluids with gravity off never separate: "
                               "the density difference is the only thing that "
                               "would move them, and nothing here reads it. "
                               "Turn gravity on, or drop back to one phase");
    }
    if (config.sources.find_first_of("\r\n") != std::string::npos) {
        return fail(error, "sources must not contain CR or LF");
    }
    if (!std::isfinite(config.lidSpeed)) {
        return fail(error, "lidSpeed must be finite");
    }
    if (!std::isfinite(config.steadyTolerance) ||
        config.steadyTolerance < 0.0) {
        return fail(error, "steadyTolerance is a fraction and cannot be "
                           "negative; zero means the run goes the whole way");
    }
    if (config.caseType != "channel" && config.caseType != "cavity") {
        return fail(error, "caseType must be channel or cavity");
    }
    if (!(config.inletFrom >= 0.0 && config.inletFrom <= 1.0) ||
        !(config.inletTo >= 0.0 && config.inletTo <= 1.0)) {
        return fail(error, "inletFrom and inletTo are fractions of a side, so "
                           "they live between 0 and 1");
    }
    if (config.inletFrom >= config.inletTo) {
        return fail(error, "inletFrom must be below inletTo, or the band the "
                           "inlet occupies is empty");
    }
    {
        bool anyOutlet = false;
        bool anyInlet = false;
        for (int side = 0; side < 4; ++side) {
            if (config.boundaryKind[side] == "outlet")
                anyOutlet = true;
            if (config.boundaryKind[side] == "inlet")
                anyInlet = true;
            if (!std::isfinite(config.boundarySpeed[side]))
                return fail(error, "boundary speeds must be finite");
        }

        if (!config.sources.empty())
            anyInlet = true;
        if (config.supportsBoundaries && config.caseType != "cavity" &&
            anyInlet && !anyOutlet && !config.restart)
            return fail(error,
                        "there is an inlet but no outlet, so fluid is pushed "
                        "into a box it cannot leave. Give one side outlet, or "
                        "make every side a wall for a closed case");
    }

    if (config.restart) {
        // The mask, the grid and the geometry all come out of the frame, so
        // the model file is not needed and is not asked for. What has to be
        // there is the frame.
        if (config.restartFile.empty()) {
            return fail(error, "restartFile must name a frame or its folder");
        }
        std::error_code filesystemError;
        if (!std::filesystem::exists(config.restartFile, filesystemError) ||
            filesystemError) {
            return fail(
                error,
                "restartFile does not exist: " + config.restartFile.string());
        }
        return validateArgumentPath("restartFile", config.restartFile, error);
    }

    return validateGeometry(config.geometryFile, error);
}

bool buildFluidSolverArguments(
    const FluidSolverRunConfig& config,
    const std::filesystem::path& outputDirectory,
    std::vector<std::string>& arguments,
    std::string& error) {
    error.clear();
    if (!validateFluidSolverRunConfig(config, error)) {
        return false;
    }
    if (!validateArgumentPath("outputDirectory", outputDirectory, error)) {
        return false;
    }
    if (!outputDirectory.is_absolute()) {
        return fail(
            error,
            "outputDirectory must be absolute for the current Fluid Solver");
    }

    arguments = {
        "Lx=" + serializeDouble(config.Lx),
        "Ly=" + serializeDouble(config.Ly),
        "nx=" + std::to_string(config.nx),
        "ny=" + std::to_string(config.ny),
        "U0=" + serializeDouble(config.U0),
        "nu=" + serializeDouble(config.nu),
        "ro=" + serializeDouble(config.ro),
        "CFL=" + serializeDouble(config.CFL),
        "totalTime=" + serializeDouble(config.totalTime),
        "dtUpdateInterval=" + std::to_string(config.dtUpdateInterval),
        "dtSafety=" + serializeDouble(config.dtSafety),
        "omega=" + serializeDouble(config.omega),
        "smootherOmega=" + serializeDouble(config.smootherOmega),
        "mgIterations=" + std::to_string(config.mgIterations),
        "mgTolerance=" + serializeDouble(config.mgTolerance),
        "mgMinCoarseSize=" + std::to_string(config.mgMinCoarseSize),
        "saveInterval=" + std::to_string(config.saveInterval),
        "useCuda=" + std::string(config.useCuda ? "1" : "0"),
        "outputDir=" + outputDirectory.u8string()
    };

    if (config.restart) {
        // The solver reads the frame first and then applies whatever follows,
        // so restart= and restartFile= go in before the overrides. The
        // geometry keys are left out entirely: the mask comes out of the frame
        // and the model file may not even exist any more.
        arguments.insert(
            arguments.begin(),
            {"restart=1", "restartFile=" + config.restartFile.u8string()});
        if (config.addTime > 0.0) {
            arguments.push_back("addTime=" + serializeDouble(config.addTime));
        }
    } else {
        arguments.push_back("geometryFile=" + config.geometryFile.u8string());
        arguments.push_back("sliceAngleX=" + serializeDouble(config.sliceAngleX));
        arguments.push_back("sliceAngleZ=" + serializeDouble(config.sliceAngleZ));
        arguments.push_back(
            "sliceRotation=" + serializeDouble(config.sliceRotation));
        arguments.push_back(
            "invertSection=" + std::string(config.invertSection ? "1" : "0"));
    }

    if (config.supportsRuntimeSwitches) {
        // A 0.1 solver stops on the first argument it does not recognise, so
        // these only exist once the executable has been read and found to be
        // 0.2 or newer.
        arguments.push_back("avx2=" + std::string(config.useAvx2 ? "1" : "0"));
        arguments.push_back(
            "openmp=" + std::string(config.useOpenMp ? "1" : "0"));
        arguments.push_back("threads=" + std::to_string(config.threads));
        arguments.push_back(
            "tray=" + std::string(config.solverTray ? "1" : "0"));
    }
    if (config.supportsGravity) {
        arguments.push_back(
            "gravityEnabled=" + std::string(config.gravityEnabled ? "1" : "0"));
        arguments.push_back(
            "gravityAccel=" + serializeDouble(config.gravityAccel));
        arguments.push_back(
            "gravityAngle=" + serializeDouble(config.gravityAngle));
    }
    if (config.supportsWallMotion) {
        arguments.push_back("wallMotion=" + config.wallMotion);
    }
    if (config.supportsBodyMotion) {
        arguments.push_back("bodyMotion=" + config.bodyMotion);
        if (!config.bodyMotion.empty()) {
            arguments.push_back("bodyCoupling=" + config.bodyCoupling);
            arguments.push_back(
                "bodyCollisions=" + std::string(config.bodyCollisions ? "1" : "0"));
            if (config.bodyCollisions)
                arguments.push_back("bodyRestitution=" +
                                    serializeDouble(config.bodyRestitution));
            arguments.push_back(
                "bodyForceReport=" +
                std::string(config.bodyForceReport ? "1" : "0"));
        }
    }
    if (config.supportsTurbulence) {
        arguments.push_back("turbulence=" + config.turbulence);
        if (config.turbulence == "smagorinsky")
            arguments.push_back("Cs=" + serializeDouble(config.turbulenceCs));
        if (config.turbulence == "kOmegaSST") {
            arguments.push_back("turbIntensity=" +
                                serializeDouble(config.turbIntensity));
            arguments.push_back("turbLengthScale=" +
                                serializeDouble(config.turbLengthScale));
        }
    }
    if (config.supportsProfiles && !config.profiles.empty()) {
        arguments.push_back("profiles=" + config.profiles);
    }
    if (config.supportsExtraFields) {
        arguments.push_back("extraFields=" + config.extraFields);
    }
    if (config.supportsSchemes) {
        arguments.push_back("convection=" + config.convection);
        arguments.push_back("limiter=" + config.limiter);
        arguments.push_back("timeScheme=" + config.timeScheme);
        arguments.push_back("gravityMode=" + config.gravityMode);
    }
    if (config.supportsPhases) {
        arguments.push_back("phases=" + std::to_string(config.phases));
        if (config.phases > 1) {
            arguments.push_back("rho1=" + serializeDouble(config.rho1));
            arguments.push_back("rho2=" + serializeDouble(config.rho2));
            arguments.push_back("nu1=" + serializeDouble(config.nu1));
            arguments.push_back("nu2=" + serializeDouble(config.nu2));
            arguments.push_back("vofScheme=" + config.vofScheme);
            if (config.supportsTension) {
                arguments.push_back("mixing=" + config.mixing);
                arguments.push_back("diffusivity=" +
                                    serializeDouble(config.diffusivity));
                arguments.push_back("surfaceTension=" +
                                    serializeDouble(config.surfaceTension));
                arguments.push_back("contactAngle=" +
                                    serializeDouble(config.contactAngle));
            }

            if (!config.initialPhaseFile.empty()) {
                arguments.push_back("phaseInit=file");
                arguments.push_back("initialPhaseFile=" +
                                    config.initialPhaseFile.u8string());
            } else {
                arguments.push_back("phaseInit=" + config.phaseInit);
                arguments.push_back("phaseLevel=" +
                                    serializeDouble(config.phaseLevel));
                arguments.push_back("phaseX=" + serializeDouble(config.phaseX));
                arguments.push_back("phaseY=" + serializeDouble(config.phaseY));
            }
        }
        arguments.push_back("sources=" + config.sources);
    }
    if (config.supportsCase) {
        arguments.push_back("caseType=" + config.caseType);
        if (config.caseType == "cavity")
            arguments.push_back("lidSpeed=" + serializeDouble(config.lidSpeed));
        arguments.push_back(
            "steadyTolerance=" + serializeDouble(config.steadyTolerance));
    }
    if (config.supportsBoundaries && config.caseType != "cavity") {
        static const char* const kKind[4] = {
            "bcLeft", "bcRight", "bcBottom", "bcTop"};
        static const char* const kSpeed[4] = {
            "bcLeftSpeed", "bcRightSpeed", "bcBottomSpeed", "bcTopSpeed"};
        for (int side = 0; side < 4; ++side) {
            arguments.push_back(std::string(kKind[side]) + "=" +
                                config.boundaryKind[side]);

            const bool movingWall = config.boundaryKind[side] == "movingWall";
            if (movingWall || config.boundarySpeed[side] != 0.0)
                arguments.push_back(std::string(kSpeed[side]) + "=" +
                                    serializeDouble(config.boundarySpeed[side]));
        }
        arguments.push_back("inletFrom=" + serializeDouble(config.inletFrom));
        arguments.push_back("inletTo=" + serializeDouble(config.inletTo));
        arguments.push_back("inletProfile=" + config.inletProfile);
    }
    return true;
}

bool writeFluidSolverArguments(
    const std::filesystem::path& filename,
    const std::vector<std::string>& arguments,
    std::string& error) {
    error.clear();

    try {
        std::ofstream output(filename, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return fail(
                error,
                "cannot open Fluid Solver argument file for writing: " +
                    filename.string());
        }

        for (const std::string& argument : arguments) {
            if (argument.empty() ||
                argument.find_first_of("\r\n") != std::string::npos) {
                return fail(
                    error,
                    "Fluid Solver arguments must be non-empty single lines");
            }
            output << argument << '\n';
        }
        output.flush();

        if (!output) {
            return fail(
                error,
                "failed while writing Fluid Solver argument file: " +
                    filename.string());
        }
        return true;
    } catch (const std::exception& exception) {
        return fail(
            error,
            "Fluid Solver argument write exception: " +
                std::string(exception.what()));
    }
}

// The solver ships as "Fluid Solver.exe" on Windows and as "Fluid Solver" with
// no extension everywhere else. Both names are tried on every platform, the
// native one first: the UI ships for Linux and macOS now, and it used to insist
// on a .exe that does not exist there.
const char* const FLUID_SOLVER_FILE_NAMES[] = {
#if defined(_WIN32)
    "Fluid Solver.exe",
    "Fluid Solver",
#else
    "Fluid Solver",
    "Fluid Solver.exe",
#endif
};

std::filesystem::path resolveFluidSolverExecutable(
    const std::filesystem::path& uiExecutable,
    const std::filesystem::path& configuredExecutable) {
    const std::filesystem::path directory = uiExecutable.parent_path();
    for (const char* const name : FLUID_SOLVER_FILE_NAMES) {
        const std::filesystem::path adjacent = directory / name;
        if (isRegularFile(adjacent)) {
            return adjacent;
        }
    }
    if (isRegularFile(configuredExecutable)) {
        return configuredExecutable;
    }
    // Nothing there. Name the file the UI looks for beside itself, so the
    // message the user gets points at the place it has to be put.
    return directory.empty()
        ? configuredExecutable
        : directory / FLUID_SOLVER_FILE_NAMES[0];
}

bool validateFluidSolverExecutable(
    const std::filesystem::path& executable,
    std::string& error) {
    if (!isRegularFile(executable)) {
        return fail(
            error,
            "Fluid Solver executable is not a regular file: " +
                executable.string());
    }
#if defined(_WIN32)
    std::string extension = executable.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (extension != ".exe") {
        return fail(error, "On Windows the Fluid Solver must be a .exe file");
    }
#else
    // Elsewhere the solver carries no extension, so the thing worth checking is
    // whether it can actually be run.
    std::error_code permissionError;
    const std::filesystem::perms permissions =
        std::filesystem::status(executable, permissionError).permissions();
    if (!permissionError &&
        (permissions & (std::filesystem::perms::owner_exec |
                        std::filesystem::perms::group_exec |
                        std::filesystem::perms::others_exec)) ==
            std::filesystem::perms::none) {
        return fail(
            error,
            "Fluid Solver is not executable: " + executable.string() +
                " - chmod +x it");
    }
#endif
    error.clear();
    return true;
}

bool writeFluidSolverSelection(
    const std::filesystem::path& selectionFile,
    const std::filesystem::path& executable,
    std::string& error) {
    if (!validateFluidSolverExecutable(executable, error)) {
        return false;
    }
    try {
        if (!selectionFile.parent_path().empty()) {
            std::filesystem::create_directories(
                selectionFile.parent_path());
        }
        std::ofstream output(
            selectionFile,
            std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return fail(
                error,
                "cannot write solver selection file: " +
                    selectionFile.string());
        }
        output << executable.u8string() << '\n';
        output.flush();
        if (!output) {
            return fail(
                error,
                "failed while writing solver selection file: " +
                    selectionFile.string());
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        return fail(
            error,
            "solver selection write exception: " +
                std::string(exception.what()));
    }
}

std::optional<std::filesystem::path> readFluidSolverSelection(
    const std::filesystem::path& selectionFile,
    std::string& error) {
    error.clear();
    std::error_code existenceError;
    if (!std::filesystem::exists(selectionFile, existenceError)) {
        if (existenceError) {
            error = "cannot inspect solver selection file: " +
                existenceError.message();
        }
        return std::nullopt;
    }

    try {
        std::ifstream input(selectionFile, std::ios::binary);
        std::string encodedPath;
        if (!input.is_open() || !std::getline(input, encodedPath)) {
            error = "cannot read solver selection file: " +
                selectionFile.string();
            return std::nullopt;
        }
        if (!encodedPath.empty() && encodedPath.back() == '\r') {
            encodedPath.pop_back();
        }
        if (encodedPath.empty()) {
            error = "solver selection file is empty";
            return std::nullopt;
        }
        const std::filesystem::path executable =
            std::filesystem::u8path(encodedPath);
        if (!validateFluidSolverExecutable(executable, error)) {
            return std::nullopt;
        }
        return executable;
    } catch (const std::exception& exception) {
        error = "solver selection read exception: " +
            std::string(exception.what());
        return std::nullopt;
    }
}

} // namespace maskui
