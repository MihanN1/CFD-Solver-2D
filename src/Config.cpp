#include "Config.hpp"
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
std::string readGeometryPath() {
    std::string path;
    std::getline(std::cin >> std::ws, path);

    if (path.size() >= 2 &&
        ((path.front() == '"' && path.back() == '"') ||
         (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
    }

    return path;
}

std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
}

void Config::readFromConsole() {
    std::cout << "=== CFD-Solver-2D Configuration ===\n";
    std::cout << "Start a new simulation or continue an old one?\n";
    std::cout << "  0 = new simulation\n";
    std::cout << "  1 = continue from a saved .vtk\n> ";
    std::cin >> restart;
    if (restart) {
        // Everything else comes out of the file, so there is nothing left to
        // ask here. main() restores the configuration and then drops into the
        // usual confirm() loop, so some parameters can be changed for some
        // experiments or something, i dont really care but its cool ahhaha
        std::cout << "Enter path to the .vtk to continue from"
                     " (or the folder with the frames, newest one wins): ";
        restartFile = readGeometryPath();
        std::cout << "Configuration will be restored from that frame.\n";
        return;
    }
    std::cout << "Enter domain width Lx (m): ";
    std::cin >> Lx;
    std::cout << "Enter domain height Ly (m): ";
    std::cin >> Ly;
    std::cout << "Enter number of cells in x-direction nx: ";
    std::cin >> nx;
    std::cout << "Enter number of cells in y-direction ny: ";
    std::cin >> ny;
    std::cout << "Enter inlet velocity U0 (m/s): ";
    std::cin >> U0;
    std::cout << "Enter kinematic viscosity nu (m^2/s): ";
    std::cin >> nu;
    std::cout << "Enter density ro. Make sure that the gas/liquid is incompressible(meaning for air speed its less than 0.3M)(kg/m^3): ";
    std::cin >> ro;
    std::cout << "Enter CFL number (recommended 0.3-0.5): ";
    std::cin >> CFL;
    std::cout << "Enter total simulation time(seconds): ";
    std::cin >> totalTime;
    std::cout << "Enter steps between dt recomputations (recommended 5): ";
    std::cin >> dtUpdateInterval;
    std::cout << "Enter SOR relaxation parameter omega (1.6-1.85): ";
    std::cin >> omega;
    std::cout << "Enter SOR relaxation parameter smootherOmega (for the coarsest multigrid level, 1.0-1.3 recommended): ";
    std::cin >> smootherOmega;
    std::cout << "Enter multigrid V-cycles per step (2 by default, 4-10 max recommended): ";
    std::cin >> mgIterations;
    std::cout << "Enter multigrid relative residual tolerance (1e-4 HEAVILY recommended): ";
    std::cin >> mgTolerance;
    std::cout << "Enter minimum coarse grid size (8 recommended): ";
    std::cin >> mgMinCoarseSize;
    std::cout << "Enter VTK save interval in steps (1 = every step, 20 recommended): ";
    std::cin >> saveInterval;
    std::cout << "Enter path to 3D model (or 'none' for circle): ";
    geometryFile = readGeometryPath();
    std::cout << "Enter around the axis going towards the observer (degrees, default 0): ";
    std::cin >> sliceAngleX;
    std::cout << "Enter around a vertical axis (degrees, default 0): ";
    std::cin >> sliceAngleZ;
    std::cout << "Enter rotation in the simulation plane (degrees, default 0): ";
    std::cin >> sliceRotation;
    std::cout << "Mirror the section? (0 = no, 1 = yes): ";
    std::cin >> invertSection;
    std::cout << "Configuration read.\n";
    std::cout << "Use cuda? (0 = no, 1 = yes, ignored on a CPU-only build): ";
    std::cin >> useCuda;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}
void Config::print() const {
    std::cout << "\n--- Current Configuration ---\n";
    std::cout << "  mode             = " << (restart ? "CONTINUE" : "NEW") << "\n";
    if (restart) {
        std::cout << "  restartFile      = " << restartFile << "\n";
        std::cout << "  addTime          = " << addTime
                  << " s (0 = totalTime is used as is)\n";
    }
    std::cout << "  Lx               = " << Lx << " m\n";
    std::cout << "  Ly               = " << Ly << " m\n";
    std::cout << "  nx               = " << nx << "\n";
    std::cout << "  ny               = " << ny << "\n";
    std::cout << "  U0               = " << U0 << " m/s\n";
    std::cout << "  nu               = " << nu << " m^2/s\n";
    std::cout << "  ro               = " << ro << " kg/m^3\n";
    std::cout << "  CFL              = " << CFL << "\n";
    std::cout << "  totalTime        = " << totalTime << " s\n";
    std::cout << "  dtUpdateInterval = " << dtUpdateInterval << " steps\n";
    std::cout << "  omega            = " << omega << " (coarsest level)\n";
    std::cout << "  smootherOmega    = " << smootherOmega << " (V-cycle smoother)\n";
    std::cout << "  mgIterations     = " << mgIterations << " V-cycles/step\n";
    std::cout << "  mgTolerance      = " << mgTolerance << " (relative)\n";
    std::cout << "  mgMinCoarseSize  = " << mgMinCoarseSize << " cells/axis\n";
    std::cout << "  saveInterval     = " << saveInterval << " steps\n";
    std::cout << "  outputDir        = " << outputDir << "\n";
    std::cout << "  geometryFile     = " << geometryFile << "\n";
    std::cout << "  sliceAngleX      = " << sliceAngleX << " deg\n";
    std::cout << "  sliceAngleZ      = " << sliceAngleZ << " deg\n";
    std::cout << "  invertSection    = " << invertSection << "\n";
    std::cout << "  sliceRotation    = " << sliceRotation << " deg\n";
    std::cout << " CUDA? Yes/No:       " << (useCuda ? "Yes" : "No") << "\n";
    std::cout << "--------------------------------\n";
}

std::string Config::serialize() const {
    std::ostringstream out;

    // max_digits10 is the shortest decimal form that round-trips back to the
    // exact same binary value, so a continuation resumes from bit-identical
    // parameters instead of something 1e-7 off
    out << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "Lx=" << Lx << "\n"
        << "Ly=" << Ly << "\n"
        << "nx=" << nx << "\n"
        << "ny=" << ny << "\n"
        << "U0=" << U0 << "\n"
        << "nu=" << nu << "\n"
        << "ro=" << ro << "\n"
        << "CFL=" << CFL << "\n"
        << "dtUpdateInterval=" << dtUpdateInterval << "\n"
        << "dtSafety=" << dtSafety << "\n"
        << "omega=" << omega << "\n"
        << "smootherOmega=" << smootherOmega << "\n"
        << "mgIterations=" << mgIterations << "\n"
        << "mgTolerance=" << mgTolerance << "\n"
        << "mgMinCoarseSize=" << mgMinCoarseSize << "\n"
        << "saveInterval=" << saveInterval << "\n"
        << "useCuda=" << (useCuda ? 1 : 0) << "\n"
        << "sliceAngleX=" << sliceAngleX << "\n"
        << "sliceAngleZ=" << sliceAngleZ << "\n"
        << "sliceRotation=" << sliceRotation << "\n"
        << "invertSection=" << (invertSection ? 1 : 0) << "\n";

    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "totalTime=" << totalTime << "\n";

    // Paths go last and unquoted: the reader splits on the first '=' only, so
    // spaces and drive letters survive. A newline inside a path would not, but
    // neither would it survive the command line.
    out << "outputDir=" << outputDir << "\n"
        << "geometryFile=" << geometryFile << "\n";

    return out.str();
}

bool Config::setParam(const std::string& key, const std::string& value) {
    const std::string lower = toLower(key);

    if (lower == "lx")                    Lx = std::strtof(value.c_str(), nullptr);
    else if (lower == "ly")               Ly = std::strtof(value.c_str(), nullptr);
    else if (lower == "nx")               nx = std::atoi(value.c_str());
    else if (lower == "ny")               ny = std::atoi(value.c_str());
    else if (lower == "u0")               U0 = std::strtof(value.c_str(), nullptr);
    else if (lower == "nu")               nu = std::strtof(value.c_str(), nullptr);
    else if (lower == "cfl")              CFL = std::strtof(value.c_str(), nullptr);
    else if (lower == "totaltime")        totalTime = std::strtod(value.c_str(), nullptr);
    else if (lower == "dtupdateinterval") dtUpdateInterval = std::atoi(value.c_str());
    else if (lower == "dtsafety")         dtSafety = std::strtof(value.c_str(), nullptr);
    else if (lower == "omega")            omega = std::strtof(value.c_str(), nullptr);
    else if (lower == "smootheromega")    smootherOmega = std::strtof(value.c_str(), nullptr);
    else if (lower == "mgiterations")     mgIterations = std::atoi(value.c_str());
    else if (lower == "mgtolerance")      mgTolerance = std::strtof(value.c_str(), nullptr);
    else if (lower == "mgmincoarsesize")  mgMinCoarseSize = std::atoi(value.c_str());
    else if (lower == "saveinterval")     saveInterval = std::atoi(value.c_str());
    else if (lower == "outputdir")        outputDir = value;
    else if (lower == "geometryfile")     geometryFile = value;
    else if (lower == "sliceanglex")      sliceAngleX = std::strtof(value.c_str(), nullptr);
    else if (lower == "sliceanglez")      sliceAngleZ = std::strtof(value.c_str(), nullptr);
    else if (lower == "slicerotation")    sliceRotation = std::strtof(value.c_str(), nullptr);
    else if (lower == "invertsection")    invertSection = (std::atoi(value.c_str()) != 0);
    else if (lower == "ro")               ro = std::strtof(value.c_str(), nullptr);
    else if (lower == "usecuda")          useCuda = (std::atoi(value.c_str()) != 0);
    else if (lower == "restart")          restart = (std::atoi(value.c_str()) != 0);
    else if (lower == "restartfile")      restartFile = value;
    else if (lower == "addtime")          addTime = std::strtod(value.c_str(), nullptr);
    else return false;

    return true;
}

bool Config::modifyParam(const std::string& name) {
    const std::string lower = toLower(name);
    bool usedFormattedInput = true;

    if (lower == "lx") {
        std::cout << "New Lx: ";
        std::cin >> Lx;
    } else if (lower == "ly") {
        std::cout << "New Ly: ";
        std::cin >> Ly;
    } else if (lower == "nx") {
        std::cout << "New nx: ";
        std::cin >> nx;
    } else if (lower == "ny") {
        std::cout << "New ny: ";
        std::cin >> ny;
    } else if (lower == "u0") {
        std::cout << "New U0: ";
        std::cin >> U0;
    } else if (lower == "nu") {
        std::cout << "New nu: ";
        std::cin >> nu;
    } else if (lower == "cfl") {
        std::cout << "New CFL: ";
        std::cin >> CFL;
    } else if (lower == "totaltime") {
        std::cout << "New totalTime: ";
        std::cin >> totalTime;
    } else if (lower == "dtupdateinterval") {
        std::cout << "New dtUpdateInterval (steps between dt recomputations): ";
        std::cin >> dtUpdateInterval;
    } else if (lower == "dtsafety") {
        std::cout << "New dtSafety (0..1): ";
        std::cin >> dtSafety;
    } else if (lower == "omega") {
        std::cout << "New omega: ";
        std::cin >> omega;
    } else if (lower == "smootheromega") {
        std::cout << "New smootherOmega (1.0-1.3 recommended): ";
        std::cin >> smootherOmega;
    } else if (lower == "mgiterations") {
        std::cout << "New mgIterations: ";
        std::cin >> mgIterations;
    } else if (lower == "mgtolerance") {
        std::cout << "New mgTolerance (relative residual): ";
        std::cin >> mgTolerance;
    } else if (lower == "mgmincoarsesize") {
        std::cout << "New mgMinCoarseSize: ";
        std::cin >> mgMinCoarseSize;
    } else if (lower == "saveinterval") {
        std::cout << "New saveInterval (steps): ";
        std::cin >> saveInterval;
    } else if (lower == "outputdir") {
        std::cout << "New outputDir: ";
        outputDir = readGeometryPath();
        usedFormattedInput = false;
    } else if (lower == "geometryfile") {
        std::cout << "New geometryFile: ";
        geometryFile = readGeometryPath();
        usedFormattedInput = false;
    } else if (lower == "sliceanglex") {
        std::cout << "New sliceAngleX (deg): ";
        std::cin >> sliceAngleX;
    } else if (lower == "sliceanglez") {
        std::cout << "New sliceAngleZ (deg): ";
        std::cin >> sliceAngleZ;
    } else if (lower == "invertsection") {
        std::cout << "New invertSection: ";
        std::cin >> invertSection;
    } else if (lower == "slicerotation") {
        std::cout << "New sliceRotation (deg): ";
        std::cin >> sliceRotation;
    } else if (lower == "ro") {
        std::cout << "New ro(kg/m^3): ";
        std::cin >> ro;
    } else if (lower == "usecuda") {
        std::cout << "New useCuda (0 = no, 1 = yes): ";
        std::cin >> useCuda;
    } else if (lower == "restartfile") {
        std::cout << "New restartFile: ";
        restartFile = readGeometryPath();
        usedFormattedInput = false;
    } else if (lower == "addtime") {
        std::cout << "New addTime (seconds to add on top of the frame): ";
        std::cin >> addTime;
    } else {
        std::cout << "Unknown parameter: " << name << "\n";
        return false;
    }
    if (usedFormattedInput) {
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    std::cout << "Parameter updated.\n";
    return true;
}
bool Config::confirm() {
    print();
    std::cout << "\nTo change a parameter, type its name (e.g., 'nx') and press Enter.\n";
    std::cout << "To confirm all parameters and proceed, just press Enter (empty line).\n";
    std::cout << "> ";

    std::string input;
    std::getline(std::cin, input);

    if (input.empty()) {
        return true;   // confirmed
    } else {
        modifyParam(input);
        return false;  // not confirmed yet, loop again
    }
}
