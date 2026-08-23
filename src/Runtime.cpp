#include "Runtime.hpp"
#include "AppPaths.hpp"
#include "Restart.hpp"
#include "Version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_OPENMP)
#include <omp.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#endif

namespace runtime {

// Defaults to what the build carries, so a path that never reaches apply()
// runs exactly as it did before the switch existed.
bool avx2 = CFD_BUILT_WITH_AVX2 != 0;

namespace {

Settings g_settings;
bool g_loaded = false;

// ---------------------------------------------------------------- machine ---

bool detectAvx2() {
#if defined(_WIN32)
    // PF_AVX2_INSTRUCTIONS_AVAILABLE. Present since Windows 10 1709; on
    // anything older the call returns false and the CPUID path below is what
    // would have to answer - but no AVX2 build targets those.
    return IsProcessorFeaturePresent(40) != FALSE;
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid_max(0, nullptr))
        return false;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0)
        return false;
    return (ebx & (1u << 5)) != 0;   // CPUID.7.0:EBX[5] = AVX2
#else
    // ARM and everything else: there is no AVX2 to have, and no AVX2 build
    // either, so the honest answer is no rather than "unknown".
    return false;
#endif
}

bool detectNvidia() {
#if defined(_WIN32)
    // nvcuda.dll is put in System32 by the display driver, so its presence
    // means a driver that can talk to a card. LoadLibrary rather than
    // FileExists: on WOW64 a 32-bit process gets the 32-bit copy, which is the
    // one it would actually have to load.
    HMODULE lib = LoadLibraryW(L"nvcuda.dll");
    if (lib) {
        FreeLibrary(lib);
        return true;
    }
    return false;
#elif defined(__APPLE__)
    return false;   // No CUDA on macOS since 10.14, and no CUDA build either.
#else
    const char* const candidates[] = {
        "/proc/driver/nvidia/version",
        "/dev/nvidiactl",
        "/dev/nvidia0",
    };
    std::error_code ec;
    for (const char* path : candidates)
        if (std::filesystem::exists(path, ec))
            return true;
    return false;
#endif
}

// ------------------------------------------------------------------ file ---

std::string trim(const std::string& text) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(text.begin(), text.end(), notSpace);
    auto last = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    return (first < last) ? std::string(first, last) : std::string();
}

bool parseBool(const std::string& value, bool fallback) {
    const std::string v = trim(value);
    if (v.empty())
        return fallback;
    switch (std::tolower(static_cast<unsigned char>(v[0]))) {
        case '1': case 'y': case 't': case 'o':
            // "off" also starts with 'o', so it is checked rather than assumed.
            return !(v.size() >= 3 &&
                     std::tolower(static_cast<unsigned char>(v[1])) == 'f');
        case '0': case 'n': case 'f':
            return false;
        default:
            return fallback;
    }
}

// The environment wins over the file, so a single run can be forced without
// editing anything: FLUID_SOLVER_NO_AVX2=1, _NO_OPENMP, _NO_CUDA,
// _NO_UPDATE_CHECK, _NO_TRAY.
bool envOff(const char* name) {
    const char* value = std::getenv(name);
    return value && *value && !(value[0] == '0' && value[1] == '\0');
}

}   // namespace

std::filesystem::path settingsPath() {
    static std::filesystem::path cached;
    if (!cached.empty())
        return cached;

    const std::filesystem::path beside = executableDir() / "settings.ini";
    std::error_code ec;
    // Writable? An unpacked portable release is; an install under Program
    // Files is not, and there the per-user location is the only one that
    // survives a reboot.
    std::ofstream probe(beside, std::ios::app);
    if (probe) {
        probe.close();
        // Only leave the file behind if load() or save() puts something in it.
        if (std::filesystem::file_size(beside, ec) == 0 && !ec)
            std::filesystem::remove(beside, ec);
        cached = beside;
        return cached;
    }

    const std::filesystem::path user = userDataDir();
    std::filesystem::create_directories(user, ec);
    cached = user / "settings.ini";
    return cached;
}

void load() {
    g_loaded = true;
    std::ifstream file(settingsPath());
    if (!file)
        return;

    std::string line;
    while (std::getline(file, line)) {
        const std::string clean = trim(line);
        if (clean.empty() || clean[0] == '#' || clean[0] == ';')
            continue;
        const size_t eq = clean.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(clean.substr(0, eq));
        const std::string value = trim(clean.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (key == "useavx2")             g_settings.useAvx2 = parseBool(value, true);
        else if (key == "useopenmp")      g_settings.useOpenMp = parseBool(value, true);
        else if (key == "usecuda")        g_settings.useCuda = parseBool(value, true);
        else if (key == "checkforupdates") g_settings.checkForUpdates = parseBool(value, true);
        else if (key == "tray")           g_settings.tray = parseBool(value, true);
        else if (key == "threads")        g_settings.threads = std::atoi(value.c_str());
    }
}

bool save() {
    const std::filesystem::path path = settingsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;

    file << "# " << CFD_APP_NAME << " " << CFD_RELEASE_VERSION
         << " - remembered settings.\n"
            "# Deleting this file restores the defaults. These switches change\n"
            "# how fast a run is, not what it solves.\n"
            "\n"
         << "useAvx2=" << (g_settings.useAvx2 ? 1 : 0) << "\n"
         << "useOpenMP=" << (g_settings.useOpenMp ? 1 : 0) << "\n"
         << "useCuda=" << (g_settings.useCuda ? 1 : 0) << "\n"
         << "threads=" << g_settings.threads << "\n"
         << "checkForUpdates=" << (g_settings.checkForUpdates ? 1 : 0) << "\n"
         << "tray=" << (g_settings.tray ? 1 : 0) << "\n";
    return static_cast<bool>(file);
}

const Settings& settings() {
    if (!g_loaded)
        load();
    return g_settings;
}

Settings& mutableSettings() {
    if (!g_loaded)
        load();
    return g_settings;
}

bool builtWithAvx2() { return CFD_BUILT_WITH_AVX2 != 0; }
bool builtWithOpenMp() { return CFD_BUILT_WITH_OPENMP != 0; }
bool builtWithCuda() { return CFD_BUILT_WITH_CUDA != 0; }

bool machineHasAvx2() {
    static const bool value = detectAvx2();
    return value;
}

bool machineHasNvidia() {
    static const bool value = detectNvidia();
    return value;
}

bool avx2Enabled() {
    // The machine check is not redundant: a binary compiled with /arch:AVX2
    // that reached this line at all is running on a CPU that has it, but a
    // build where only the kernels are guarded (any future runtime dispatch)
    // would not be, and answering honestly here costs one cached bool.
    return builtWithAvx2() && settings().useAvx2 &&
           !envOff("FLUID_SOLVER_NO_AVX2") && machineHasAvx2();
}

bool openMpEnabled() {
    return builtWithOpenMp() && settings().useOpenMp &&
           !envOff("FLUID_SOLVER_NO_OPENMP");
}

bool cudaEnabled() {
    return builtWithCuda() && settings().useCuda &&
           !envOff("FLUID_SOLVER_NO_CUDA");
}

int threadCount() {
#if defined(_OPENMP)
    if (!openMpEnabled())
        return 1;
    return omp_get_max_threads();
#else
    return 1;
#endif
}

void apply() {
    avx2 = avx2Enabled();
#if defined(_OPENMP)
    if (!openMpEnabled()) {
        // One thread is how OpenMP is turned off without touching a single
        // pragma: every parallel region still runs, on the calling thread.
        omp_set_dynamic(0);
        omp_set_num_threads(1);
    } else if (settings().threads > 0) {
        omp_set_dynamic(0);
        omp_set_num_threads(settings().threads);
    }
#endif
}

namespace {

const char* platformTag() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char* archTag() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x64";
#else
    return "x86";
#endif
}

bool archIsX86() {
    const std::string arch = archTag();
    return arch == "x64" || arch == "x86";
}

// Which rows exist: no AVX2 on ARM, and CUDA only on 64-bit Windows and Linux.
std::string bestRowFor(bool avx2, bool omp, bool cuda) {
    std::string tag;
    const auto add = [&tag](const char* part) {
        if (!tag.empty())
            tag += '-';
        tag += part;
    };
    if (avx2 && archIsX86())
        add("avx2");
    if (omp)
        add("omp");
    if (cuda && std::string(archTag()) == "x64" &&
        std::string(platformTag()) != "macos")
        add("cuda");
    return tag.empty() ? "plain" : tag;
}

}   // namespace

std::string hardwareReport() {
    const bool avx2 = archIsX86() && machineHasAvx2();
    const bool nvidia = machineHasNvidia();
    // The machine's cores, not threadCount(): that one answers "how many
    // threads will OpenMP use", which is 1 in a build that has no OpenMP - and
    // a plain build is exactly the one someone runs this from.
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0)
        cores = 1;

    std::ostringstream out;
    out << "This machine\n"
        << "  system     : " << platformTag() << "-" << archTag() << "\n"
        << "  cores      : " << cores << "\n"
        << "  AVX2       : ";
    if (!archIsX86())
        out << "no - AVX2 is an x86 instruction set, this is ARM\n";
    else
        out << (avx2 ? "yes\n" : "no - this CPU is too old for it\n");
    out << "  NVIDIA GPU : ";
    if (std::string(platformTag()) == "macos")
        out << "not usable - macOS has had no CUDA since 10.14\n";
    else if (std::string(archTag()) != "x64")
        out << "not usable - no CUDA toolkit targets this architecture\n";
    else
        out << (nvidia ? "yes\n" : "no driver found\n");

    out << "\nDownload this row:\n\n    Fluid Solver " << CFD_RELEASE_VERSION
        << " " << platformTag() << "-" << archTag() << " "
        << bestRowFor(avx2, cores > 1, nvidia) << "\n\n"
        << "Add \"-ui\" to that name for the same thing with the desktop UI in\n"
           "it. Or take the installer, which reads all of the above itself.\n";
    return out.str();
}

std::string summary() {
    std::ostringstream out;
    const auto line = [&out](const char* name, bool built, bool on,
                             const char* why) {
        out << "  " << name;
        for (size_t i = std::char_traits<char>::length(name); i < 8; ++i)
            out << ' ';
        if (!built)
            out << ": not in this build\n";
        else if (on)
            out << ": on\n";
        else
            out << ": off" << (why && *why ? " - " : "") << (why ? why : "")
                << "\n";
    };

    line("AVX2", builtWithAvx2(), avx2Enabled(),
         machineHasAvx2() ? "turned off in settings"
                          : "this CPU has no AVX2");
    line("OpenMP", builtWithOpenMp(), openMpEnabled(), "turned off in settings");
    line("CUDA", builtWithCuda(), cudaEnabled(),
         machineHasNvidia() ? "turned off in settings"
                            : "no NVIDIA driver on this machine");
    if (openMpEnabled())
        out << "  threads : " << threadCount() << "\n";
    return out.str();
}

namespace {

// Reads one line. Empty means "leave it alone", which is what pressing Enter
// through the whole menu does.
std::string askLine(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string answer;
    if (!std::getline(std::cin, answer))
        return std::string();
    return trim(answer);
}

bool askToggle(const char* name, bool built, bool current, const char* note) {
    if (!built) {
        std::cout << "  " << name << " is not in this build, so there is "
                                     "nothing to switch.\n";
        return current;
    }
    std::ostringstream prompt;
    prompt << "  " << name << " [" << (current ? "on" : "off") << "]";
    if (note && *note)
        prompt << " - " << note;
    prompt << ". on/off (Enter keeps it): ";
    const std::string answer = askLine(prompt.str());
    if (answer.empty())
        return current;
    return parseBool(answer, current);
}

}   // namespace

bool configureInteractively() {
    Settings& s = mutableSettings();
    const Settings before = s;

    std::cout << "\n--- Acceleration ---\n"
                 "These change how fast a run is, not what it solves. The "
                 "velocity field comes\nout the same to the last digit a float "
                 "can hold; the pressure lands on a\nslightly different "
                 "multigrid iterate, the same way the AVX2 and non-AVX2\n"
                 "downloads always have. The choice is remembered for the next "
                 "run.\n\n"
              << summary() << "\n";

    s.useAvx2 = askToggle("AVX2", builtWithAvx2(), s.useAvx2,
                          machineHasAvx2() ? "this CPU supports it"
                                           : "this CPU does NOT support it");
    s.useOpenMp = askToggle("OpenMP", builtWithOpenMp(), s.useOpenMp,
                            "spreads the loops over every core");
    if (builtWithOpenMp() && s.useOpenMp) {
        const std::string answer =
            askLine("  threads [" +
                    (s.threads > 0 ? std::to_string(s.threads)
                                   : std::string("all cores")) +
                    "] (0 = all cores, Enter keeps it): ");
        if (!answer.empty()) {
            const int value = std::atoi(answer.c_str());
            s.threads = (value > 0) ? value : 0;
        }
    }
    s.useCuda = askToggle("CUDA", builtWithCuda(), s.useCuda,
                          machineHasNvidia() ? "an NVIDIA driver is present"
                                             : "no NVIDIA driver found");
    s.checkForUpdates = askToggle("update check", true, s.checkForUpdates,
                                  "asks GitHub once per start");
    s.tray = askToggle("tray icon", true, s.tray,
                       "progress while a simulation runs");

    apply();

    const bool changed = before.useAvx2 != s.useAvx2 ||
                         before.useOpenMp != s.useOpenMp ||
                         before.useCuda != s.useCuda ||
                         before.threads != s.threads ||
                         before.checkForUpdates != s.checkForUpdates ||
                         before.tray != s.tray;
    if (!changed) {
        std::cout << "\nNothing changed.\n";
        return false;
    }
    if (save()) {
        std::cout << "\nSaved to " << pathToConsole(settingsPath()) << "\n"
                  << summary();
        return true;
    }
    std::cout << "\n!!! Could not write " << pathToConsole(settingsPath())
              << ", so this applies to the current run only.\n";
    return false;
}

}   // namespace runtime
