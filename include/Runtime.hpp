#pragma once
#include <filesystem>
#include <string>

// The three accelerators, at runtime.
//
// Which of them a binary CAN use is decided when it is compiled: AVX2 is a
// compiler flag, OpenMP is a runtime linked in, CUDA is a whole second
// backend. Which of them it DOES use is decided here, per machine, and the
// answer is remembered - so a choice made once survives into every later run
// instead of having to be retyped, or worse, having to be reinstalled.
//
// Turning one off changes how long a run takes, not what it is solving. The
// AVX2 kernels and the scalar tail evaluate the same expressions, OpenMP only
// splits the same loops across cores, and the CUDA backend solves the same
// system. What does move is the last few digits: the velocity field agrees to
// float rounding, and the pressure - which the multigrid stops refining once
// the residual is under mgTolerance - lands on a slightly different iterate,
// about a thousandth of its peak at the default tolerance. That is the same
// difference the separate AVX2 and non-AVX2 release rows have always had
// between them, not something these switches introduce.

namespace runtime {

struct Settings {
    // Each of these is "use it" and defaults to yes. A switch for something
    // this build has no code for is kept - so moving the settings file to a
    // richer build does not silently lose the choice - but does nothing.
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

// The file the answers live in: settings.ini beside the executable when that
// directory can be written to - which is what a portable unpack gives - and in
// the per-user data directory otherwise, which is what an install under
// Program Files gives. Same rule the frames follow, for the same reason.
std::filesystem::path settingsPath();

// Read once at startup. A missing file is not an error: it means the defaults.
void load();

// Write the current settings back. Returns false when the location refuses,
// which is worth saying out loud rather than pretending the choice stuck.
bool save();

const Settings& settings();
Settings& mutableSettings();

// Applies what the settings say to the process: caps the OpenMP thread count
// when OpenMP is off, and nothing else - AVX2 and CUDA are read per call site.
void apply();

// What this binary was built with. A switch for something not in here is shown
// as unavailable rather than hidden, so the menu explains why a machine is
// slower than the one next to it.
bool builtWithAvx2();
bool builtWithOpenMp();
bool builtWithCuda();

// What this machine can run, which is a different question again: a binary
// built with CUDA still needs a driver, and one built with AVX2 still needs a
// CPU that has it.
bool machineHasAvx2();
bool machineHasNvidia();

// Built with it, turned on, and the machine can take it. This is what the
// kernels ask.
bool avx2Enabled();
bool openMpEnabled();
bool cudaEnabled();

// The same answer as avx2Enabled(), cached in a plain global because the
// vector kernels read it once per row inside OpenMP regions: a function with a
// thread-safe local static costs a guard load per read, which is more than the
// branch it is protecting. apply() sets it, and main() calls apply() before
// anything starts solving. It defaults to what the build can do, so a code
// path that somehow skipped apply() behaves exactly as it did before this
// switch existed.
extern bool avx2;

// Threads OpenMP is allowed to use right now, for printing.
int threadCount();

// One line per accelerator: what it is, whether it is on, and if not, why.
std::string summary();

// What THIS MACHINE can run, and which release row to download for it. Answers
// the same question from a "plain" build as from any other, which is the point:
// plain runs anywhere, so it is the one to grab when you do not know yet.
std::string hardwareReport();

// The interactive menu. Returns true when something was changed and saved.
bool configureInteractively();

}   // namespace runtime
