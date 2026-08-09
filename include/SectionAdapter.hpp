#pragma once

#include "GeometryProcessor.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace maskui {

bool writeSectionAdapterOBJ(const std::filesystem::path& filename,
                            const std::vector<Vec2>& contour,
                            std::string& error);

} // namespace maskui
