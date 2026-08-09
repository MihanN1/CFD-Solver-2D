#include "ExplorerTarget.hpp"

namespace maskui {

std::optional<ExplorerTarget> chooseExplorerTarget(
    const std::filesystem::path& selectedFrame,
    const std::filesystem::path& runDirectory) {
    if (!selectedFrame.empty()) {
        return ExplorerTarget{selectedFrame, true};
    }
    if (!runDirectory.empty()) {
        return ExplorerTarget{runDirectory, false};
    }
    return std::nullopt;
}

} // namespace maskui
