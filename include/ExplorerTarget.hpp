#pragma once

#include <filesystem>
#include <optional>

namespace maskui {

struct ExplorerTarget {
    std::filesystem::path location;
    bool selectFile = false;
};

std::optional<ExplorerTarget> chooseExplorerTarget(
    const std::filesystem::path& selectedFrame,
    const std::filesystem::path& runDirectory);

} // namespace maskui
