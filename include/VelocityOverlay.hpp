#pragma once

#include "VtkFrame.hpp"

#include <cstddef>
#include <vector>

namespace maskui {

struct VelocityOverlayConfig {
    double targetSpacingPixels = 36.0;
    double zeroTolerance = 1.0e-12;
    std::size_t maxArrows = 2000;
};

struct VelocityArrow {
    std::size_t i = 0;
    std::size_t j = 0;
    double unitX = 0.0;
    double unitY = 0.0;
    double relativeMagnitude = 0.0;
};

class VelocityOverlayPlanner {
public:
    explicit VelocityOverlayPlanner(VelocityOverlayConfig config = {});

    std::vector<VelocityArrow> plan(
        const VtkFrame& frame,
        double cellWidthPixels,
        double cellHeightPixels,
        double referenceMagnitude) const;

    const VelocityOverlayConfig& config() const noexcept;

private:
    VelocityOverlayConfig config_;
};

} // namespace maskui
