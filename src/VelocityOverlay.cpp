#include "VelocityOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace maskui {
namespace {

std::size_t initialStride(double targetSpacing, double cellSize) {
    const double requested = std::ceil(targetSpacing / cellSize);
    if (requested >=
        static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return std::max<std::size_t>(
        1,
        static_cast<std::size_t>(requested));
}

std::size_t sampleStart(std::size_t extent, std::size_t stride) {
    return stride >= extent ? extent / 2 : stride / 2;
}

std::size_t sampleCount(
    std::size_t extent,
    std::size_t stride,
    std::size_t start) {
    if (extent == 0 || start >= extent) {
        return 0;
    }
    return 1 + (extent - 1 - start) / stride;
}

} // namespace

VelocityOverlayPlanner::VelocityOverlayPlanner(
    VelocityOverlayConfig config)
    : config_(config) {
    if (!std::isfinite(config_.targetSpacingPixels) ||
        config_.targetSpacingPixels <= 0.0) {
        throw std::invalid_argument(
            "Velocity overlay spacing must be finite and positive");
    }
    if (!std::isfinite(config_.zeroTolerance) ||
        config_.zeroTolerance < 0.0) {
        throw std::invalid_argument(
            "Velocity overlay zero tolerance must be finite and non-negative");
    }
    if (config_.maxArrows == 0) {
        throw std::invalid_argument(
            "Velocity overlay maximum arrow count must be positive");
    }
}

std::vector<VelocityArrow> VelocityOverlayPlanner::plan(
    const VtkFrame& frame,
    double cellWidthPixels,
    double cellHeightPixels,
    double referenceMagnitude) const {
    std::vector<VelocityArrow> arrows;
    if (frame.nx == 0 || frame.ny == 0 ||
        !std::isfinite(cellWidthPixels) || cellWidthPixels <= 0.0 ||
        !std::isfinite(cellHeightPixels) || cellHeightPixels <= 0.0 ||
        frame.nx > std::numeric_limits<std::size_t>::max() / frame.ny) {
        return arrows;
    }

    const std::size_t cellCount = frame.nx * frame.ny;
    if (frame.solid.size() != cellCount ||
        frame.velocity.size() != cellCount ||
        frame.velocityFinite.size() != cellCount) {
        return arrows;
    }

    std::size_t strideX =
        initialStride(config_.targetSpacingPixels, cellWidthPixels);
    std::size_t strideY =
        initialStride(config_.targetSpacingPixels, cellHeightPixels);
    std::size_t startX = sampleStart(frame.nx, strideX);
    std::size_t startY = sampleStart(frame.ny, strideY);
    std::size_t countX = sampleCount(frame.nx, strideX, startX);
    std::size_t countY = sampleCount(frame.ny, strideY, startY);

    if (countX != 0 &&
        countY > config_.maxArrows / countX) {
        const double estimated =
            static_cast<double>(countX) * static_cast<double>(countY);
        const double scale =
            std::sqrt(estimated / static_cast<double>(config_.maxArrows));
        strideX = std::max<std::size_t>(
            strideX + 1,
            static_cast<std::size_t>(
                std::ceil(static_cast<double>(strideX) * scale)));
        strideY = std::max<std::size_t>(
            strideY + 1,
            static_cast<std::size_t>(
                std::ceil(static_cast<double>(strideY) * scale)));
        startX = sampleStart(frame.nx, strideX);
        startY = sampleStart(frame.ny, strideY);
        countX = sampleCount(frame.nx, strideX, startX);
        countY = sampleCount(frame.ny, strideY, startY);
    }

    arrows.reserve(std::min(
        config_.maxArrows,
        countX * countY));
    const bool hasReference =
        std::isfinite(referenceMagnitude) &&
        referenceMagnitude > config_.zeroTolerance;

    for (std::size_t j = startY;
         j < frame.ny && arrows.size() < config_.maxArrows;
         j += strideY) {
        for (std::size_t i = startX;
             i < frame.nx && arrows.size() < config_.maxArrows;
             i += strideX) {
            const std::size_t index = frame.cellIndex(i, j);
            if (frame.solid[index] != 0 ||
                frame.velocityFinite[index] == 0) {
                continue;
            }

            const Velocity& velocity = frame.velocity[index];
            if (!std::isfinite(velocity.x) ||
                !std::isfinite(velocity.y)) {
                continue;
            }
            const double magnitude =
                std::hypot(velocity.x, velocity.y);
            if (!std::isfinite(magnitude) ||
                magnitude <= config_.zeroTolerance) {
                continue;
            }

            arrows.push_back(VelocityArrow{
                i,
                j,
                velocity.x / magnitude,
                velocity.y / magnitude,
                hasReference
                    ? std::clamp(
                          magnitude / referenceMagnitude,
                          0.0,
                          1.0)
                    : 1.0
            });
        }
    }
    return arrows;
}

const VelocityOverlayConfig&
VelocityOverlayPlanner::config() const noexcept {
    return config_;
}

} // namespace maskui
