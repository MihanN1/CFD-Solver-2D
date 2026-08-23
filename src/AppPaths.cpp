#include "AppPaths.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <vector>
#endif

namespace {

// The application's own name, used for the fallback directory only. Kept here
// rather than in Config, because it names the install and not the simulation.
const char* const kAppName = "Fluid Solver";

std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0')
        return {};
    return std::filesystem::path(value);
}

// A directory is only usable if a file can actually be created in it. Checking
// the permission bits is not enough on Windows, where an install under Program
// Files reads as writable and then is not.
bool directoryAcceptsFiles(const std::filesystem::path& dir) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (!std::filesystem::is_directory(dir, error))
        return false;

    const std::filesystem::path probe = dir / ".fluid-solver-write-test";
    std::error_code removeFirst;
    std::filesystem::remove(probe, removeFirst);

    std::FILE* handle = std::fopen(probe.string().c_str(), "wb");
    if (handle == nullptr)
        return false;
    std::fclose(handle);
    std::error_code cleanup;
    std::filesystem::remove(probe, cleanup);
    return true;
}

}

std::filesystem::path executableDir() {
    std::error_code error;

#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(),
                           static_cast<DWORD>(buffer.size()));
    if (length > 0 && static_cast<size_t>(length) < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        const std::filesystem::path self =
            std::filesystem::weakly_canonical(
                std::filesystem::path(buffer.data()), error);
        if (!error)
            return self.parent_path();
    }
#else
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
        return self.parent_path();
#endif

    return std::filesystem::current_path(error);
}

std::filesystem::path userDataDir() {
#if defined(_WIN32)
    std::filesystem::path base = environmentPath("LOCALAPPDATA");
    if (base.empty())
        base = environmentPath("APPDATA");
#elif defined(__APPLE__)
    std::filesystem::path base = environmentPath("HOME");
    if (!base.empty())
        base /= "Library/Application Support";
#else
    std::filesystem::path base = environmentPath("XDG_DATA_HOME");
    if (base.empty()) {
        base = environmentPath("HOME");
        if (!base.empty())
            base /= ".local/share";
    }
#endif

    if (base.empty())
        return executableDir();
    return base / kAppName;
}

std::filesystem::path resolveOutputDir(const std::filesystem::path& outputDir) {
    std::filesystem::path target;
    if (outputDir.empty())
        target = executableDir();
    else if (outputDir.is_absolute())
        target = outputDir;
    else
        target = executableDir() / outputDir;

    if (directoryAcceptsFiles(target))
        return target;

    // Read-only install directory. Anywhere else is a surprise, so the path
    // that actually gets used is printed rather than left to be discovered.
    const std::filesystem::path fallback =
        userDataDir() /
        (outputDir.empty() ? std::filesystem::path("output") : outputDir);
    if (directoryAcceptsFiles(fallback)) {
        std::cout << "Cannot write to " << target.string()
                  << "\n  frames go to " << fallback.string()
                  << " instead.\n";
        return fallback;
    }

    std::cerr << "Cannot write to " << target.string() << " or "
              << fallback.string() << "; frames will not be saved.\n";
    return target;
}
