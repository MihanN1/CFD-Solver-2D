#pragma once
#include <filesystem>
#include <string>

namespace runtime {

struct Settings {
    bool useAvx2 = true;
    bool useOpenMp = true;
    bool useCuda = true;

    // Ask GitHub whether a newer release exists, once per start.
    bool checkForUpdates = true;

    // Tray icon and taskbar progress while a simulation runs.
    bool tray = true;

    // How many threads OpenMP may use. 0 means "whatever the runtime picked",
    // which is one per core.
    int threads = 0;
};

std::filesystem::path settingsPath();

void load();

bool save();

const Settings& settings();
Settings& mutableSettings();

void apply();

bool builtWithAvx2();
bool builtWithOpenMp();
bool builtWithCuda();

bool machineHasAvx2();
bool machineHasNvidia();

bool avx2Enabled();
bool openMpEnabled();
bool cudaEnabled();

extern bool avx2;

// Threads OpenMP is allowed to use right now, for printing.
int threadCount();

// One line per accelerator: what it is, whether it is on, and if not, why.
std::string summary();

// The interactive menu. Returns true when something was changed and saved.
bool configureInteractively();

}