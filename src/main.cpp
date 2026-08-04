#include "Config.hpp"
#include "Mesh.hpp"
#include "Solver.hpp"
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage(const char* exe) {
    std::cout <<
        "Usage:\n"
        "  " << exe << "                       interactive configuration\n"
        "  " << exe << " key=value [key=value] non-interactive run\n"
        "\n"
        "Keys: Lx Ly nx ny U0 nu Re CFL totalTime dtUpdateInterval dtSafety\n"
        "      omega smootherOmega mgIterations mgTolerance mgMinCoarseSize\n"
        "      saveInterval outputDir geometryFile sliceAngleX sliceAngleZ\n"
        "      sliceRotation invertSection ro\n"
        "\n"
        "Example:\n"
        "  " << exe << " nx=256 ny=128 Lx=2 Ly=1 U0=1 nu=0.002 "
                       "totalTime=2 saveInterval=25\n";
}

}

int main(int argc, char** argv) {
    std::cout << "=== CFD-Solver-2D ===\n\n";

    Config cfg;
    if (argc > 1) {
        for (int a = 1; a < argc; ++a) {
            const std::string arg = argv[a];
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            }
            const size_t eq = arg.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "Malformed argument: " << arg << "\n";
                printUsage(argv[0]);
                return 1;
            }
            if (!cfg.setParam(arg.substr(0, eq), arg.substr(eq + 1))) {
                std::cerr << "Unknown parameter: " << arg.substr(0, eq) << "\n";
                printUsage(argv[0]);
                return 1;
            }
        }
        cfg.print();
    } else {
        cfg.readFromConsole();

        while (!cfg.confirm()) {
            // loop will repeat until user presses Enter without text
        }

        std::cout << "\n--- Final Configuration ---\n";
        cfg.print();

        std::cout << "\nNote: This version supports STL and OBJ models.\n";
        std::cout << "      The mask is generated from a central plane section of the model.\n";
        std::cout << "      Slice angles, in-plane rotation, and optional mirroring are applied.\n";
        std::cout << "      Enter 'none' to use the circle verification geometry.\n";
        std::cout << "      Total simulation time: " << cfg.totalTime << " s.\n";
    }

    if (cfg.nx < 8 || cfg.ny < 8) {
        std::cerr << "nx and ny must be at least 8.\n";
        return 1;
    }

    Mesh mesh(cfg);
    mesh.printInfo();

    Solver solver(cfg, mesh);
    const auto t0 = std::chrono::steady_clock::now();
    solver.run();
    const auto t1 = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Wall clock: " << seconds << " s\n";

    if (argc <= 1) {
        std::cout << "\nSimulation complete. Press Enter to exit...";
        std::cin.get();
    }
    return 0;
}