#include "Config.hpp"
#include "Mesh.hpp"
#include "Progress.hpp"
#include "Restart.hpp"
#include "Runtime.hpp"
#include "Solver.hpp"
#include "UpdateCheck.hpp"
#include "Version.hpp"
#include <cmath>
#include <cstdlib>
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
        "  " << exe << " --settings             change AVX2/OpenMP/CUDA and exit\n"
        "  " << exe << " --check-updates        ask GitHub for a newer release\n"
        "\n"
        "Keys: Lx Ly nx ny U0 nu CFL totalTime dtUpdateInterval dtSafety\n"
        "      omega smootherOmega mgIterations mgTolerance mgMinCoarseSize\n"
        "      saveInterval outputDir geometryFile sliceAngleX sliceAngleZ\n"
        "      sliceRotation invertSection ro useCuda restart restartFile addTime\n"
        "\n"
        "Acceleration, for this run only (the remembered defaults come from\n"
        "settings.ini, which --settings writes):\n"
        "      avx2=0|1 openmp=0|1 useCuda=0|1 threads=N tray=0|1\n"
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

// The three accelerators can be steered per run without touching the file the
// choice is remembered in. Handled before Config sees the argument, because
// only "useCuda" is a simulation parameter - the other three are not.
static bool applyRuntimeArg(const std::string& key, const std::string& value) {
    const bool on = std::atoi(value.c_str()) != 0;
    if (key == "avx2")          { runtime::mutableSettings().useAvx2 = on; return true; }
    if (key == "openmp" || key == "omp")
                                { runtime::mutableSettings().useOpenMp = on; return true; }
    if (key == "tray")          { runtime::mutableSettings().tray = on; return true; }
    if (key == "threads")       {
        const int count = std::atoi(value.c_str());
        runtime::mutableSettings().threads = (count > 0) ? count : 0;
        return true;
    }
    return false;
}

// Referenced through a volatile pointer so no compiler decides the strings are
// unused and drops them: the UI finds this build's version and feature set by
// searching the executable's bytes for them, without having to run it.
namespace {
const char* const kBuildMarkers[] = {CFD_VERSION_MARKER, CFD_FEATURES_MARKER};
const char* const* const kBuildMarkersKeepAlive = kBuildMarkers;
}   // namespace

int main(int argc, char** argv) {
    (void)kBuildMarkersKeepAlive;
    std::cout << "=== CFD-Solver-2D " << CFD_RELEASE_VERSION << " ("
              << CFD_BUILD_FEATURES << ") ===\n\n";

    // Read before anything asks a question, so the answers the user gave last
    // time are already in place - and so --settings has something to edit.
    runtime::load();

    Config cfg;
    // The remembered choice is the default; "useCuda=" on the command line and
    // the interactive prompt both still override it, and a build with no CUDA
    // in it cannot be talked into having some.
    cfg.useCuda = runtime::cudaEnabled();
    std::vector<std::pair<std::string, std::string>> overrides;

    if (argc > 1) {
        for (int a = 1; a < argc; ++a) {
            const std::string arg = argv[a];
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            }
            if (arg == "--version" || arg == "-v") {
                // Two machine-readable lines first, then the sentence. The UI
                // reads the same two strings straight out of the file, so what
                // is printed here and what is found there cannot disagree.
                std::cout << CFD_VERSION_MARKER << "\n"
                          << CFD_FEATURES_MARKER << "\n"
                          << CFD_APP_NAME << " " << CFD_RELEASE_VERSION
                          << " (build " << CFD_APP_VERSION << ", "
                          << CFD_BUILD_FEATURES << ")\n";
                return 0;
            }
            if (arg == "--settings" || arg == "--accel") {
                runtime::configureInteractively();
                return 0;
            }
            if (arg == "--check-updates") {
                const update::Result result = update::check();
                if (!result.checked) {
                    std::cout << "Could not check: " << result.error << "\n";
                    return 1;
                }
                std::cout << "This build: " << CFD_RELEASE_VERSION
                          << "\nNewest published: " << result.latest << "\n"
                          << (result.newer ? result.url
                                           : std::string("Already up to date."))
                          << "\n";
                return 0;
            }
            const size_t eq = arg.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::cerr << "Malformed argument: " << arg << "\n";
                printUsage(argv[0]);
                return 1;
            }
            if (applyRuntimeArg(arg.substr(0, eq), arg.substr(eq + 1)))
                continue;
            if (!cfg.setParam(arg.substr(0, eq), arg.substr(eq + 1))) {
                std::cerr << "Unknown parameter: " << arg.substr(0, eq) << "\n";
                printUsage(argv[0]);
                return 1;
            }
            overrides.emplace_back(arg.substr(0, eq), arg.substr(eq + 1));
        }
        runtime::apply();
        update::runStartupCheck(false);
        std::cout << "Acceleration:\n" << runtime::summary();
        cfg.print();
    } else {
        runtime::apply();
        update::runStartupCheck(true);

        std::cout << "Acceleration (remembered from last time):\n"
                  << runtime::summary()
                  << "Change it? [y/N] ";
        std::cout.flush();
        std::string answer;
        if (std::getline(std::cin, answer) && !answer.empty() &&
            (answer[0] == 'y' || answer[0] == 'Y')) {
            runtime::configureInteractively();
            cfg.useCuda = runtime::cudaEnabled();
        }
        std::cout << "\n";

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

        // The configuration asks about CUDA as well, and that answer is the
        // later of the two, so it becomes the remembered default rather than
        // being forgotten the moment this run ends. Only here: a "useCuda=" on
        // the command line is an override for one run and has no business
        // rewriting a file.
        if (runtime::builtWithCuda() &&
            cfg.useCuda != runtime::settings().useCuda) {
            runtime::mutableSettings().useCuda = cfg.useCuda;
            runtime::apply();
            runtime::save();
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
                // Ignoring this quietly was worse than being wrong out loud.
                // addTime walks forward from where the frame stopped, it does
                // not subtract from totalTime, and time does not run backwards.
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

    // Takes the tray icon down and clears the taskbar progress. Before the
    // "press Enter" below, or the icon sits there for as long as the window
    // stays open with nothing behind it.
    progress::shutdown();

    if (argc <= 1) {
        std::cout << "\nSimulation complete. Press Enter to exit...";
        std::cin.get();
    }
    return 0;
}