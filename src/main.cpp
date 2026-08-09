#include "Application.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <filesystem>
#include <iostream>
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
            << "Usage: cfd_mask_ui_optimized [model.stl|model.obj|result-directory]\n";
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
