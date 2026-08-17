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
        "      gravityEnabled gravityAccel gravityAngle wallMotion\n"
        "\n"
        "Rules:\n"
        "  key=value, no spaces around '='  nx=256      not  nx = 256\n"
        "  keys are case insensitive        NX=256, --nx=256 also work\n"
        "  decimal separator is a dot       nu=0.002    not  nu=0,002\n"
        "  switches take 1 or 0             useCuda=1   (true/false, yes/no too)\n"
        "  no units inside the value        totalTime=2 not  totalTime=2s\n"
        "  quote paths with spaces          \"geometryFile=C:\\my models\\a.stl\"\n"
        "  wallMotion has a grammar of its own:\n"
        "        <object>:<setting>=<value>,<setting>=<value>;<next object>:...\n"
        "        settings are rot=<deg/s>, slideX=<m/s>, slideY=<m/s>, slip=1\n"
        "        \"wallMotion=1:rot=90,slideX=0.5;2:slip=1\"\n"
        "        an object either moves (rot/slide) or slips, never both\n"
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

// argv arrives already split by the shell, so "nx 256" shows up as a bare
// "nx" and a bare "256". Say which of the two happened instead of one
// "Malformed argument" for everything.
static std::string describeMalformed(const std::string& arg) {
    if (arg.empty())
        return "not a right way to write an empty argument: every argument is "
               "key=value, e.g. nx=256.";
    if (arg.front() == '=')
        return "not a right way to write '" + arg +
               "': the key in front of '=' is missing, e.g. nx=256.";

    const std::string known = Config::canonicalKey(arg);
    if (!known.empty())
        return "not a right way to write '" + arg + "': " + known +
               " needs its value glued to it, with no spaces around '='. "
               "Write " + known + "=<value>.";

    const std::string guess = Config::suggestKey(arg);
    if (!guess.empty())
        return "not a right way to write '" + arg + "': arguments are key=value "
               "with no spaces. Did you mean " + guess + "=<value>?";

    return "not a right way to write '" + arg + "': arguments are key=value "
           "with no spaces, e.g. nx=256. Run with --help for the list of keys.";
}

int main(int argc, char** argv) {
    std::cout << "=== CFD-Solver-2D ===\n\n";

    Config cfg;
    std::vector<std::pair<std::string, std::string>> overrides;

    if (argc > 1) {
        // --help wins wherever it sits on the line, even behind a broken
        // argument, which is exactly when it tends to be needed.
        for (int a = 1; a < argc; ++a) {
            const std::string arg = argv[a];
            if (arg == "-h" || arg == "--help" || arg == "/?") {
                printUsage(argv[0]);
                return 0;
            }
        }

        // Every bad argument is collected, so one run reports all of them
        // instead of one per attempt.
        std::vector<std::string> problems;
        bool wallMotionRefused = false;

        for (int a = 1; a < argc; ++a) {
            const std::string arg = argv[a];
            const size_t eq = arg.find('=');
            if (eq == std::string::npos || eq == 0) {
                problems.push_back(describeMalformed(arg));
                continue;
            }
            const std::string key = arg.substr(0, eq);
            const std::string value = arg.substr(eq + 1);

            std::string error, warning;
            if (!cfg.setParam(key, value, error, &warning)) {
                problems.push_back(error);
                if (Config::canonicalKey(key) == "wallMotion")
                    wallMotionRefused = true;
                continue;
            }
            if (!warning.empty())
                std::cout << "Warning: " << warning << "\n";
            overrides.emplace_back(key, value);
        }

        if (!problems.empty()) {
            std::cerr << "\n" << problems.size()
                      << (problems.size() == 1 ? " argument is wrong:\n"
                                               : " arguments are wrong:\n");
            for (const std::string& problem : problems)
                std::cerr << "  " << problem << "\n";
            std::cerr << "\nNothing has been started. Fix the line and run it "
                         "again.\n\n";
            printUsage(argv[0]);
            // The one key with a grammar the usage block cannot hold in a
            // single line, so it gets its own explanation when it is the one
            // that failed.
            if (wallMotionRefused)
                std::cout << wallMotionHelp();
            return 1;
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
            } else if (cfg.addTime < 0.0) {
                std::cout << "\n!!! addTime " << cfg.addTime
                          << " s is negative and does nothing. It counts "
                             "forward from the\n    time this frame stopped at ("
                          << restart.currentTime
                          << " s). To stop earlier, set totalTime itself,\n"
                             "    and it still has to be past "
                          << restart.currentTime << " s.\n";
                cfg.addTime = 0.0;
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