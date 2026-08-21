#include "Application.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<std::filesystem::path> systemArguments(
    int argc,
    char* argv[]) {
#ifdef _WIN32
    int wideArgumentCount = 0;
    LPWSTR* wideArguments =
        CommandLineToArgvW(GetCommandLineW(), &wideArgumentCount);
    if (wideArguments != nullptr) {
        std::vector<std::filesystem::path> arguments;
        arguments.reserve(static_cast<std::size_t>(wideArgumentCount));
        for (int index = 0; index < wideArgumentCount; ++index) {
            arguments.emplace_back(wideArguments[index]);
        }
        LocalFree(wideArguments);
        return arguments;
    }
#endif
    std::vector<std::filesystem::path> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return arguments;
}

std::filesystem::path executablePath(
    const std::vector<std::filesystem::path>& arguments) {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length > 0 &&
        static_cast<std::size_t>(length) < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer);
    }
#elif defined(__linux__)
    // Everything the UI keeps beside itself - the solver, output/, the font,
    // the saved preferences - hangs off this path, and argv[0] is not it. A
    // launcher symlink or a .desktop entry hands over a bare name or a link,
    // and resolving that against the working directory aimed the whole lot at
    // wherever the program happened to be started from.
    {
        std::error_code linkError;
        const std::filesystem::path self =
            std::filesystem::read_symlink("/proc/self/exe", linkError);
        if (!linkError && !self.empty()) {
            return self;
        }
    }
#elif defined(__APPLE__)
    {
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        if (size > 0) {
            std::string pathBuffer(size, '\0');
            if (_NSGetExecutablePath(pathBuffer.data(), &size) == 0) {
                pathBuffer.resize(std::char_traits<char>::length(
                    pathBuffer.c_str()));
                std::error_code realError;
                const std::filesystem::path resolved =
                    std::filesystem::canonical(pathBuffer, realError);
                return realError
                    ? std::filesystem::path(pathBuffer)
                    : resolved;
            }
        }
    }
#endif
    std::error_code error;
    std::filesystem::path executable =
        arguments.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(arguments.front(), error);
    if (error && !arguments.empty()) {
        executable = arguments.front();
    }
    return executable;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::filesystem::path> arguments =
        systemArguments(argc, argv);
    if (arguments.size() > 2) {
        std::cerr
            << "Usage: \"Fluid Solver UI\" [model.stl | model.obj | folder of solution_*.vtk]\n";
        return 1;
    }

    const std::filesystem::path initialModel =
        arguments.size() == 2
            ? arguments[1]
            : std::filesystem::path{};
    maskui::Application application(
        executablePath(arguments),
        initialModel);
    return application.run();
}
