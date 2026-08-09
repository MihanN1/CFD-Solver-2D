#include "FriendRun.hpp"

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

bool validateGrid(const FriendRunConfig& config, std::string& error) {
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

bool validateFriendRunConfig(const FriendRunConfig& config,
                             std::string& error) {
    error.clear();

    if (!validateGrid(config, error) ||
        !requirePositive("Lx", config.Lx, error) ||
        !requirePositive("Ly", config.Ly, error) ||
        !requireNonNegative("U0", config.U0, error) ||
        !requirePositive("nu", config.nu, error) ||
        !requirePositive("ro", config.ro, error) ||
        !requirePositive("CFL", config.CFL, error) ||
        !requireNonNegative("totalTime", config.totalTime, error) ||
        !requirePositive("omega", config.omega, error) ||
        !requireFinite("sliceAngleX", config.sliceAngleX, error) ||
        !requireFinite("sliceAngleZ", config.sliceAngleZ, error) ||
        !requireFinite("sliceRotation", config.sliceRotation, error)) {
        return false;
    }

    if (config.CFL > 1.0) {
        return fail(error, "CFL must be in (0, 1]");
    }
    if (config.omega >= 2.0) {
        return fail(error, "omega must be in (0, 2)");
    }
    return validateGeometry(config.geometryFile, error);
}

bool buildFriendSolverArguments(
    const FriendRunConfig& config,
    const std::filesystem::path& outputDirectory,
    std::vector<std::string>& arguments,
    std::string& error) {
    error.clear();
    if (!validateFriendRunConfig(config, error)) {
        return false;
    }
    if (!validateArgumentPath("outputDirectory", outputDirectory, error)) {
        return false;
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
        "omega=" + serializeDouble(config.omega),
        "outputDir=" + outputDirectory.u8string(),
        "geometryFile=" + config.geometryFile.u8string(),
        "sliceAngleX=" + serializeDouble(config.sliceAngleX),
        "sliceAngleZ=" + serializeDouble(config.sliceAngleZ),
        "sliceRotation=" + serializeDouble(config.sliceRotation),
        "invertSection=" + std::string(config.invertSection ? "1" : "0")
    };
    return true;
}

bool writeFriendSolverArguments(
    const std::filesystem::path& filename,
    const std::vector<std::string>& arguments,
    std::string& error) {
    error.clear();

    try {
        std::ofstream output(filename, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return fail(
                error,
                "cannot open friend argument file for writing: " +
                    filename.string());
        }

        for (const std::string& argument : arguments) {
            if (argument.empty() ||
                argument.find_first_of("\r\n") != std::string::npos) {
                return fail(
                    error,
                    "friend solver arguments must be non-empty single lines");
            }
            output << argument << '\n';
        }
        output.flush();

        if (!output) {
            return fail(
                error,
                "failed while writing friend argument file: " +
                    filename.string());
        }
        return true;
    } catch (const std::exception& exception) {
        return fail(
            error,
            "friend argument write exception: " +
                std::string(exception.what()));
    }
}

std::filesystem::path resolveFriendSolverExecutable(
    const std::filesystem::path& uiExecutable,
    const std::filesystem::path& configuredExecutable) {
    const std::filesystem::path adjacent =
        uiExecutable.parent_path() / "cfd_app.exe";
    if (isRegularFile(adjacent)) {
        return adjacent;
    }
    if (isRegularFile(configuredExecutable)) {
        return configuredExecutable;
    }
    return adjacent.empty() ? configuredExecutable : adjacent;
}

bool validateFriendSolverExecutable(
    const std::filesystem::path& executable,
    std::string& error) {
    if (!isRegularFile(executable)) {
        return fail(
            error,
            "friend solver executable is not a regular file: " +
                executable.string());
    }
    std::string extension = executable.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (extension != ".exe") {
        return fail(error, "friend solver must be a .exe file");
    }
    error.clear();
    return true;
}

bool writeFriendSolverSelection(
    const std::filesystem::path& selectionFile,
    const std::filesystem::path& executable,
    std::string& error) {
    if (!validateFriendSolverExecutable(executable, error)) {
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

std::optional<std::filesystem::path> readFriendSolverSelection(
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
        if (!validateFriendSolverExecutable(executable, error)) {
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
