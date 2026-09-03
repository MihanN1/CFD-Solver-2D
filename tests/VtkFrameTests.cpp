#include "VtkFrame.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void writeWord(std::ostream& output, std::uint32_t value) {
    output.put(static_cast<char>((value >> 24u) & 0xffu));
    output.put(static_cast<char>((value >> 16u) & 0xffu));
    output.put(static_cast<char>((value >> 8u) & 0xffu));
    output.put(static_cast<char>(value & 0xffu));
}

void writeFloat(std::ostream& output, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    writeWord(output, bits);
}

void writeFrame(const std::filesystem::path& path,
                std::size_t pRawCount,
                bool early = false) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "# vtk DataFile Version 3.0\n"
           << "CFD-Solver-2D output, step 20\n"
           << "BINARY\n"
           << "DATASET STRUCTURED_POINTS\n"
           << "DIMENSIONS 3 3 1\n"
           << "ORIGIN 0 0 0\n"
           << "SPACING 0.5 0.5 1\n"
           << "CELL_DATA 4\n"
           << "SCALARS pressure float 1\n"
           << "LOOKUP_TABLE default\n";
    for (int value = 0; value < 4; ++value) {
        writeFloat(output, static_cast<float>(value));
    }

    if (early) {
        output << "\nSCALARS phase float 1\nLOOKUP_TABLE default\n";
        for (int value = 0; value < 4; ++value) {
            writeFloat(output, 0.25f * static_cast<float>(value));
        }
    }
    output << "\nSCALARS solid int 1\nLOOKUP_TABLE default\n";
    for (int value = 0; value < 4; ++value) {
        writeWord(output, value == 0 ? 1u : 0u);
    }
    output << "\nVECTORS velocity float\n";
    for (int value = 0; value < 12; ++value) {
        writeFloat(output, 0.25f * static_cast<float>(value));
    }

    const std::string config =
        "formatVersion=1\n"
        "Lx=1\nLy=1\nnx=2\nny=2\n"
        "totalTime=10\n"
        "ro=1.225\n"
        "dtSafety=0.9\n"
        "useCuda=1\n"
        "restartTime=4.5\n"
        "restartStep=20\n"
        "restartDt=0.01\n";
    output << "\nFIELD RestartData 4\n"
           << "configText 1 " << config.size() << " char\n";
    output.write(config.data(), static_cast<std::streamsize>(config.size()));
    output << "\nuFace 1 6 float\n";
    for (int value = 0; value < 6; ++value) {
        writeFloat(output, 0.0f);
    }
    output << "\nvFace 1 6 float\n";
    for (int value = 0; value < 6; ++value) {
        writeFloat(output, 0.0f);
    }
    output << "\npRaw 1 " << pRawCount << " float\n";
    for (std::size_t value = 0; value < pRawCount; ++value) {
        writeFloat(output, 0.0f);
    }
    output << '\n';
}

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    if (maskui::VtkFrameParser::frameNumberFromFilename(
            "solution_20.vtk") != 20 ||
        maskui::VtkFrameParser::frameNumberFromFilename(
            "solution_3_6.vtk") != 6) {
        return fail("new or continued solver filename was not recognized");
    }
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("mask-ui-vtk-test-" + std::to_string(unique));
    std::filesystem::create_directories(root);
    const std::filesystem::path exact = root / "solution_20.vtk";
    const std::filesystem::path malformed = root / "solution_21.vtk";
    writeFrame(exact, 4);
    writeFrame(malformed, 3);

    const maskui::VtkFrame frame = maskui::VtkFrameParser::parse(exact);
    if (frame.nx != 2 || frame.ny != 2 ||
        !frame.restart.restartCapable ||
        !frame.restart.currentTime ||
        std::abs(*frame.restart.currentTime - 4.5) > 1e-12 ||
        frame.restart.totalTime != 10.0 ||
        frame.restart.restartStep != 20 ||
        frame.restart.config.at("dtSafety") != "0.9") {
        return fail("exact RestartData metadata was not parsed correctly");
    }

    const auto sample = maskui::sampleVtkPixel(
        frame,
        maskui::ResultImageTransform{0.0, 0.0, 10.0, 10.0},
        5.0,
        5.0);
    if (!sample || std::abs(sample->velocityX - 1.5f) > 1e-6f ||
        std::abs(sample->velocityY - 1.75f) > 1e-6f) {
        return fail("pixel sample did not expose velocity components u/v");
    }

    const maskui::VtkFrame bad = maskui::VtkFrameParser::parse(malformed);
    if (bad.restart.restartCapable) {
        return fail("mismatched RestartData sizes were accepted");
    }

    const std::filesystem::path phased = root / "solution_22.vtk";
    writeFrame(phased, 4, true);
    const maskui::VtkFrame withPhase = maskui::VtkFrameParser::parse(phased);
    if (withPhase.scalars.find("phase") == withPhase.scalars.end()) {
        return fail("a scalar written before the mask was dropped, and the "
                    "solver writes the phase fraction exactly there");
    }
    if (std::abs(withPhase.scalars.at("phase").at(3) - 0.75f) > 1e-6f) {
        return fail("the phase fraction came back with the wrong values");
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return 0;
}
