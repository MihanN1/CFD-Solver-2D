#include "SectionAdapter.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>

namespace maskui {
namespace {

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

} // namespace

bool writeSectionAdapterOBJ(const std::filesystem::path& filename,
                            const std::vector<std::vector<Vec2>>& contours,
                            std::string& error) {
    error.clear();
    if (contours.empty()) {
        return fail(error, "section adapter requires at least one contour");
    }

    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (std::size_t component = 0;
         component < contours.size();
         ++component) {
        const std::vector<Vec2>& contour = contours[component];
        if (contour.size() < 3) {
            return fail(
                error,
                "section adapter component " +
                    std::to_string(component) +
                    " requires at least three points");
        }
        double signedAreaTwice = 0.0;
        for (std::size_t index = 0; index < contour.size(); ++index) {
            const Vec2& point = contour[index];
            const Vec2& next = contour[(index + 1) % contour.size()];
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                return fail(error, "section adapter points must be finite");
            }
            if (point.x == next.x && point.y == next.y) {
                return fail(
                    error,
                    "section adapter contains consecutive duplicate points");
            }
            minimumX = std::min(minimumX, point.x);
            minimumY = std::min(minimumY, point.y);
            maximumX = std::max(maximumX, point.x);
            maximumY = std::max(maximumY, point.y);
            signedAreaTwice += point.x * next.y - next.x * point.y;
        }
        if (std::abs(signedAreaTwice) <=
            std::numeric_limits<double>::epsilon()) {
            return fail(error, "section adapter contour area is degenerate");
        }
    }

    const double span =
        std::max(maximumX - minimumX, maximumY - minimumY);
    if (!std::isfinite(span) ||
        span <= std::numeric_limits<double>::epsilon()) {
        return fail(error, "section adapter contour span is degenerate");
    }

    const double centreX = 0.5 * (minimumX + maximumX);
    const double centreY = 0.5 * (minimumY + maximumY);
    constexpr double halfDepth = 5e-4;

    try {
        std::ofstream output(filename, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return fail(
                error,
                "cannot open section adapter for writing: " +
                    filename.string());
        }

        output << "# CFD Mask UI section adapter\n";
        output << "# Derived from all selected GUI 2D contours; guarded repairs may apply.\n";
        output << std::setprecision(
            std::numeric_limits<double>::max_digits10);
        std::size_t vertexBase = 0;
        for (std::size_t component = 0;
             component < contours.size();
             ++component) {
            const std::vector<Vec2>& contour = contours[component];
            output << "o section_component_" << component << '\n';
            for (const Vec2& point : contour) {
                output << "v "
                       << (point.x - centreX) / span << ' '
                       << (point.y - centreY) / span << ' '
                       << -halfDepth << '\n';
            }
            for (const Vec2& point : contour) {
                output << "v "
                       << (point.x - centreX) / span << ' '
                       << (point.y - centreY) / span << ' '
                       << halfDepth << '\n';
            }

            output << "g section_component_" << component << '\n';
            const std::size_t pointCount = contour.size();
            for (std::size_t index = 0; index < pointCount; ++index) {
                const std::size_t next = (index + 1) % pointCount;
                const std::size_t bottomCurrent = vertexBase + index + 1;
                const std::size_t bottomNext = vertexBase + next + 1;
                const std::size_t topCurrent =
                    vertexBase + pointCount + index + 1;
                const std::size_t topNext =
                    vertexBase + pointCount + next + 1;
                output << "f "
                       << bottomCurrent << ' '
                       << bottomNext << ' '
                       << topNext << '\n';
                output << "f "
                       << bottomCurrent << ' '
                       << topNext << ' '
                       << topCurrent << '\n';
            }
            vertexBase += 2u * pointCount;
        }
        output.flush();
        if (!output) {
            return fail(
                error,
                "failed while writing section adapter: " +
                    filename.string());
        }
        return true;
    } catch (const std::exception& exception) {
        return fail(
            error,
            "section adapter write exception: " +
                std::string(exception.what()));
    }
}

} // namespace maskui
