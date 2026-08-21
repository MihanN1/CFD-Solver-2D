#pragma once

#include "GeometryProcessor.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace maskui {

bool writeSectionAdapterOBJ(const std::filesystem::path& filename,
                            const std::vector<std::vector<Vec2>>& contours,
                            std::string& error);

} // namespace maskui
