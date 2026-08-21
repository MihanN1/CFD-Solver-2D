#include "FluidSolverRun.hpp"
#include "GeometryProcessor.hpp"
#include "SectionAdapter.hpp"
#include "VtkFrame.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

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

int launch(const std::filesystem::path& executable,
           const std::vector<std::string>& arguments) {
#ifdef _WIN32
    std::vector<std::wstring> encoded;
    encoded.reserve(arguments.size() + 1u);
    encoded.push_back(L"\"" + executable.wstring() + L"\"");
    for (const std::string& argument : arguments) {
        encoded.push_back(std::filesystem::u8path(argument).wstring());
    }
    std::vector<const wchar_t*> argv;
    argv.reserve(encoded.size() + 1u);
    for (const std::wstring& argument : encoded) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    return static_cast<int>(_wspawnv(
        _P_WAIT, executable.c_str(), argv.data()));
#else
    (void)executable;
    (void)arguments;
    return 0;
#endif
}

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
#ifndef _WIN32
    return 0;
#else
    const std::filesystem::path solver =
        std::filesystem::u8path(CFD_SOLVER_EXE);
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("mask-ui-solver-test-" + std::to_string(unique));
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
        return fail("geometry load failed: " + error);
    }
    maskui::MaskParameters maskParameters;
    maskParameters.Lx = 4.0;
    maskParameters.Ly = 2.0;
    maskParameters.nx = 32;
    maskParameters.ny = 16;
    const maskui::MaskResult mask = geometry.generateMask(maskParameters);
    if (!mask.success || mask.contours.size() != 2) {
        return fail("two-object mask failed: " + mask.error);
    }
    const std::filesystem::path adapter = root / "section-adapter.obj";
    if (!maskui::writeSectionAdapterOBJ(adapter, mask.contours, error)) {
        return fail("adapter write failed: " + error);
    }

    const std::filesystem::path freshOutput = root / "fresh";
    maskui::FluidSolverRunConfig fresh;
    fresh.Lx = maskParameters.Lx;
    fresh.Ly = maskParameters.Ly;
    fresh.nx = maskParameters.nx;
    fresh.ny = maskParameters.ny;
    fresh.totalTime = 0.01;
    fresh.saveInterval = 1;
    fresh.geometryFile = adapter;
    std::vector<std::string> arguments;
    if (!maskui::buildFluidSolverArguments(
            fresh, freshOutput, arguments, error)) {
        return fail("fresh current-solver arguments failed: " + error);
    }
    if (launch(solver, arguments) != 0) {
        return fail("fresh Fluid Solver process failed; files retained at " +
                    root.string());
    }
    const std::vector<std::filesystem::path> freshFrames =
        maskui::VtkFrameParser::discoverFrames(freshOutput);
    if (freshFrames.empty()) {
        return fail("current Fluid Solver produced no VTK frames");
    }
    const maskui::VtkFrame result =
        maskui::VtkFrameParser::parse(freshFrames.back());
    if (result.nx == 0 || result.ny == 0 || result.pressure.empty()) {
        return fail("current Fluid Solver VTK could not be parsed");
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
#endif
}
