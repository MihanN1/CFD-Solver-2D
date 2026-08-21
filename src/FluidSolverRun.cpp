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
        "outputDir=" + outputDirectory.u8string(),
        "geometryFile=" + config.geometryFile.u8string(),
        "sliceAngleX=" + serializeDouble(config.sliceAngleX),
        "sliceAngleZ=" + serializeDouble(config.sliceAngleZ),
        "sliceRotation=" + serializeDouble(config.sliceRotation),
        "invertSection=" + std::string(config.invertSection ? "1" : "0")
    };
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
