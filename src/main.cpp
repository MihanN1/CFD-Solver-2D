#include "Config.hpp"
#include "Mesh.hpp"
#include "Restart.hpp"
#include "Solver.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <chrono>

static void printUsage(const char* exe) {
    std::cout <<
        "Usage:\n"
        "  " << exe << "                       interactive configuration\n"
        "  " << exe << " key=value [key=value] non-interactive run\n"
        "\n"
        "Keys: Lx Ly nx ny U0 nu CFL totalTime dtUpdateInterval dtSafety\n"
        "      omega smootherOmega mgIterations mgTolerance mgMinCoarseSize\n"
        "      saveInterval outputDir geometryFile sliceAngleX sliceAngleZ\n"
        "      sliceRotation invertSection ro useCuda restart restartFile addTime\n"
        "\n"
        "Example:\n"
        "  " << exe << " nx=256 ny=128 Lx=2 Ly=1 U0=1 nu=0.002 "
                       "totalTime=2 saveInterval=25\n"
        "\n"
        "Continuing a run (restartFile takes a frame or the folder holding\n"
        "them; anything given after it overrides the stored configuration):\n"
        "  " << exe << " restart=1 restartFile=output totalTime=30 addTime=10\n"
                       "saveInterval=5\n";
}

int main(int argc, char** argv) {
    std::cout << "=== CFD-Solver-2D ===\n\n";

    Config cfg;
    std::vector<std::pair<std::string, std::string>> overrides;

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
            overrides.emplace_back(arg.substr(0, eq), arg.substr(eq + 1));
        }
        cfg.print();
    } else {
        cfg.readFromConsole();

        if (!cfg.restart) {
            while (!cfg.confirm()) {
            }

            std::cout << "\n--- Final Configuration ---\n";
            cfg.print();

            std::cout << "\nNote: This version supports STL and OBJ models.\n";
            std::cout << "      The mask is generated from a central plane section of the model.\n";
            std::cout << "      Slice angles, in-plane rotation, and optional mirroring are applied.\n";
            std::cout << "      Enter 'none' to use the circle verification geometry.\n";

            std::cout << "      Total simulation time: " << cfg.totalTime << " s.\n";
        }
    }

    RestartData restart;
    std::filesystem::path restartPath;

    if (cfg.restart) {
        std::string error;
        restartPath = resolveRestartPath(cfg.restartFile, error);
        if (restartPath.empty() || !loadRestart(restartPath, restart, error)) {
            std::cerr << "Cannot continue: " << error << "\n";
            return 1;
        }
        std::cout << "\nContinuing from " << pathToConsole(restartPath) << "\n";

        const std::string requested = cfg.restartFile;
        cfg = restart.cfg;
        cfg.restart = true;
        cfg.restartFile = requested;
        for (const auto& override : overrides)
            cfg.setParam(override.first, override.second);
        const auto applyAddTime = [&]() {
            if (cfg.addTime > 0.0) {
                cfg.totalTime = restart.currentTime + cfg.addTime;
                std::cout << "addTime " << cfg.addTime
                          << " s -> totalTime " << cfg.totalTime << " s\n";
            }
        };
        applyAddTime();
        const auto validate = [&](std::string& reason) {
            std::ostringstream why;
            if (cfg.nx != restart.nx || cfg.ny != restart.ny) {
                why << "nx and ny cannot change on a continuation ("
                    << restart.nx << "x" << restart.ny << " in the frame).";
            } else if (std::fabs(cfg.Lx - restart.cfg.Lx) >
                           1e-6f * restart.cfg.Lx ||
                       std::fabs(cfg.Ly - restart.cfg.Ly) >
                           1e-6f * restart.cfg.Ly) {
                why << "Lx and Ly cannot change on a continuation ("
                    << restart.cfg.Lx << " x " << restart.cfg.Ly
                    << " in the frame).";
            } else if (cfg.totalTime <= restart.currentTime) {
                why << "totalTime (" << cfg.totalTime
                    << " s) is not past the time this frame already reached ("
                    << restart.currentTime << " s), there would be nothing to "
                       "compute. Type 'totalTime' and give it more.";
            } else {
                return true;
            }
            reason = why.str();
            return false;
        };

        std::string reason;
        if (argc <= 1) {
            std::cout << "\nThe configuration below came out of that frame. "
                         "Change whatever you want\n"
                         "(totalTime and saveInterval are the usual ones), "
                         "then press Enter to start.\n"
                         "The grid and the geometry are fixed by the frame "
                         "and cannot be changed.\n";

            for (;;) {
                while (!cfg.confirm()) {
                }
                applyAddTime();
                if (validate(reason))
                    break;
                std::cout << "\n!!! " << reason << "\n";
            }
        } else if (!validate(reason)) {
            std::cerr << reason << "\n";
            return 1;
        }
        cfg.print();

        std::cout << "\nNote: the solid mask is taken from the frame, so the "
                     "geometry parameters\n"
                     "      above are only informational and the model file "
                     "is not needed.\n";
    }

    if (cfg.nx < 8 || cfg.ny < 8) {
        std::cerr << "nx and ny must be at least 8.\n";
        return 1;
    }

    Mesh mesh(cfg, cfg.restart ? &restart.solid : nullptr);
    mesh.printInfo();

    Solver solver(cfg, mesh);

    if (cfg.restart &&
        !solver.setInitialState(std::move(restart),
                                restartPath.stem().string())) {
        std::cerr << "The state in the frame does not match the grid.\n";
        return 1;
    }

    const auto startTime = std::chrono::steady_clock::now();
    solver.run();
    const auto endTime = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration<double>(endTime - startTime).count();
    std::cout << "Wall clock: " << seconds << " s\n";

    if (argc <= 1) {
        std::cout << "\nSimulation complete. Press Enter to exit...";
        std::cin.get();
    }
    return 0;
}