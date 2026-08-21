#include "GeometryProcessor.hpp"
#include "SectionAdapter.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void writeCube(std::ostream& output,
               double x0,
               double x1,
               std::size_t base) {
    output << "v " << x0 << " -0.5 -0.5\n"
           << "v " << x1 << " -0.5 -0.5\n"
           << "v " << x1 << " 0.5 -0.5\n"
           << "v " << x0 << " 0.5 -0.5\n"
           << "v " << x0 << " -0.5 0.5\n"
           << "v " << x1 << " -0.5 0.5\n"
           << "v " << x1 << " 0.5 0.5\n"
           << "v " << x0 << " 0.5 0.5\n";
    const auto index = [base](std::size_t local) {
        return base + local;
    };
    output << "f " << index(1) << ' ' << index(4) << ' ' << index(3) << ' '
           << index(2) << "\n"
           << "f " << index(5) << ' ' << index(6) << ' ' << index(7) << ' '
           << index(8) << "\n"
           << "f " << index(1) << ' ' << index(2) << ' ' << index(6) << ' '
           << index(5) << "\n"
           << "f " << index(2) << ' ' << index(3) << ' ' << index(7) << ' '
           << index(6) << "\n"
           << "f " << index(3) << ' ' << index(4) << ' ' << index(8) << ' '
           << index(7) << "\n"
           << "f " << index(4) << ' ' << index(1) << ' ' << index(5) << ' '
           << index(8) << "\n";
}

std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("mask-ui-geometry-test-" + std::to_string(unique));
    std::filesystem::create_directories(root);
    const std::filesystem::path model = root / "two-cubes.obj";
    {
        std::ofstream output(model);
        writeCube(output, -1.5, -0.5, 0);
        writeCube(output, 0.5, 1.5, 8);
    }

    maskui::GeometryProcessor geometry;
    std::string error;
    if (!geometry.load(model, error)) {
        return fail("two-cube load failed: " + error);
    }
    maskui::MaskParameters parameters;
    parameters.Lx = 4.0;
    parameters.Ly = 2.0;
    parameters.nx = 200;
    parameters.ny = 100;
    parameters.sliceAngleX = 0.0;
    parameters.sliceAngleZ = 0.0;
    const maskui::MaskResult mask = geometry.generateMask(parameters);
    if (!mask.success || mask.contours.size() != 2) {
        return fail(
            "expected two disconnected contours: " + mask.error);
    }

    bool leftSolid = false;
    bool rightSolid = false;
    for (int j = 0; j < parameters.ny; ++j) {
        for (int i = 0; i < parameters.nx; ++i) {
            const std::size_t cell =
                static_cast<std::size_t>(j) * parameters.nx + i;
            if (mask.cells[cell] == 0) {
                continue;
            }
            leftSolid = leftSolid || i < parameters.nx / 2 - 2;
            rightSolid = rightSolid || i > parameters.nx / 2 + 2;
        }
    }
    if (!leftSolid || !rightSolid) {
        return fail("the union mask did not retain both separated objects");
    }

    const maskui::MaskResult single = geometry.rasterizeContours(
        parameters,
        {mask.contours.front()});
    if (!single.success || single.contours.size() != 1 ||
        single.solidCellCount <= 0 ||
        single.solidCellCount >= mask.solidCellCount) {
        return fail(
            "single-contour rerasterization did not reduce the preview mask");
    }

    const std::filesystem::path adapter = root / "section-adapter.obj";
    if (!maskui::writeSectionAdapterOBJ(adapter, mask.contours, error)) {
        return fail("multi-component adapter failed: " + error);
    }
    std::ifstream input(adapter, std::ios::binary);
    const std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (occurrences(text, "o section_component_") != 2 ||
        occurrences(text, "g section_component_") != 2) {
        return fail("adapter OBJ does not contain two component groups");
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
