#include "Application.hpp"

#include "ExplorerTarget.hpp"
#include "FluidSolverRun.hpp"
#include "NumericInput.hpp"
#include "GeometryProcessor.hpp"
#include "SectionAdapter.hpp"
#include "VtkFrame.hpp"
#include "VelocityOverlay.hpp"

#include <SFML/Graphics.hpp>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef CFD_MASK_UI_VERSION
#define CFD_MASK_UI_VERSION "dev"
#endif

namespace maskui {
namespace {

constexpr float LEFT_PANEL_WIDTH = 330.0f;
constexpr float PARAMETER_TOP = 122.0f;
constexpr float PARAMETER_BOTTOM_MARGIN = 126.0f;
constexpr float PARAMETER_ROW_HEIGHT = 44.0f;
constexpr float PARAMETER_GROUP_HEIGHT = 25.0f;
constexpr float PARAMETER_SCROLL_STEP = 88.0f;
constexpr double PI = 3.14159265358979323846;
constexpr std::size_t DECODED_FRAME_CACHE_BYTES = 1024ull * 1024ull * 1024ull;
constexpr std::size_t MAX_ADAPTIVE_RESIDENT_FRAMES = 16;

const sf::Color BACKGROUND{5, 7, 6};
const sf::Color PANEL{13, 15, 14};
const sf::Color VIEW_BACKGROUND{4, 6, 5};
const sf::Color TEXT{222, 225, 223};
const sf::Color MUTED{128, 135, 131};
const sf::Color ACCENT{68, 214, 44};
const sf::Color ACCENT_DARK{32, 112, 28};
const sf::Color SOLID_COLOR{35, 38, 36};
const sf::Color CONTROL_BACKGROUND{9, 11, 10};
const sf::Color CONTROL_RAIL{48, 54, 50};
const sf::Color BUTTON_DISABLED{34, 37, 35};
const sf::Color BUTTON_BACKGROUND{28, 33, 30};
const sf::Color BORDER{55, 63, 58};
const sf::Color OVERLAY_BACKGROUND{8, 10, 9, 245};
const sf::Color SECTION_PLANE{68, 214, 44, 14};
const sf::Color SECTION_PLANE_OUTLINE{86, 220, 62, 190};
const sf::Color INVALID_COLOR{255, 0, 180};
const sf::Color CUT_COLOR{255, 157, 46};
const sf::Color CUT_GLOW{68, 32, 8, 220};
const sf::Color WARNING_BACKGROUND{92, 35, 24, 235};
const sf::Color WARNING_OUTLINE{255, 137, 74};
const sf::Color WARNING_TEXT{255, 225, 205};

float clampFloat(float value, float minimum, float maximum) {
    return std::clamp(value, minimum, maximum);
}

double wrapDegrees(double value) {
    while (value > 180.0) {
        value -= 360.0;
    }
    while (value < -180.0) {
        value += 360.0;
    }
    return value;
}

double snapCardinalDegrees(double value) {
    value = wrapDegrees(value);
    const double cardinal = 90.0 * std::round(value / 90.0);
    return std::abs(value - cardinal) <= 0.25
               ? wrapDegrees(cardinal)
               : value;
}

std::string formatValue(double value, bool integer, const std::string& unit) {
    std::ostringstream output;
    if (integer) {
        output << static_cast<long long>(std::llround(value));
    } else if (std::abs(value) > 0.0 && std::abs(value) < 0.001) {
        output << std::scientific << std::setprecision(2) << value;
    } else {
        output << std::fixed << std::setprecision(2) << value;
    }
    if (!unit.empty()) {
        output << ' ' << unit;
    }
    return output.str();
}

sf::Text makeText(
    const sf::Font& font,
    const std::string& value,
    unsigned int size,
    sf::Vector2f position,
    sf::Color color = TEXT) {
    sf::Text text(font, value, size);
    text.setPosition(position);
    text.setFillColor(color);
    return text;
}

void drawPanel(sf::RenderTarget& target, const sf::FloatRect& bounds) {
    sf::RectangleShape panel(bounds.size);
    panel.setPosition(bounds.position);
    panel.setFillColor(PANEL);
    target.draw(panel);
}

void drawThickLine(
    sf::RenderTarget& target,
    sf::Vector2f first,
    sf::Vector2f second,
    float thickness,
    sf::Color color) {
    const sf::Vector2f direction = second - first;
    const float segmentLength = std::hypot(direction.x, direction.y);
    if (segmentLength <= 0.0f) {
        return;
    }
    const sf::Vector2f perpendicular{
        -direction.y / segmentLength * thickness * 0.5f,
        direction.x / segmentLength * thickness * 0.5f
    };
    sf::ConvexShape segment(4);
    segment.setPoint(0, first + perpendicular);
    segment.setPoint(1, second + perpendicular);
    segment.setPoint(2, second - perpendicular);
    segment.setPoint(3, first - perpendicular);
    segment.setFillColor(color);
    target.draw(segment);
}

struct Slider {
    std::string label;
    std::string unit;
    double minimum = 0.0;
    double maximum = 1.0;
    double value = 0.0;
    bool integer = false;
    bool logarithmic = false;
    bool boolean = false;
    double defaultValue = 0.0;
    sf::FloatRect track{{0.0f, 0.0f}, {1.0f, 1.0f}};
    bool dragging = false;

    double normalized() const {
        if (logarithmic) {
            const double low = std::log10(minimum);
            const double high = std::log10(maximum);
            return (std::log10(value) - low) / (high - low);
        }
        return (value - minimum) / (maximum - minimum);
    }

    void setNormalized(double normalizedValue) {
        normalizedValue = std::clamp(normalizedValue, 0.0, 1.0);
        if (logarithmic) {
            const double low = std::log10(minimum);
            const double high = std::log10(maximum);
            value = std::pow(10.0, low + normalizedValue * (high - low));
        } else {
            value = minimum + normalizedValue * (maximum - minimum);
        }
        if (integer) {
            value = std::round(value);
        }
    }

    void setFromX(float mouseX) {
        if (boolean) {
            value = mouseX >= track.position.x + track.size.x * 0.5f
                        ? 1.0
                        : 0.0;
            return;
        }
        setNormalized(
            static_cast<double>((mouseX - track.position.x) / track.size.x));
    }

    bool hit(sf::Vector2f point) const {
        const sf::FloatRect hitBounds{
            {track.position.x - 8.0f, track.position.y - 10.0f},
            {track.size.x + 16.0f, track.size.y + 20.0f}
        };
        return hitBounds.contains(point);
    }

    sf::FloatRect editBounds() const {
        return {
            {track.position.x + track.size.x - 132.0f,
             track.position.y - 28.0f},
            {132.0f, 23.0f}
        };
    }

    sf::FloatRect stepMinusBounds() const {
        return {
            {track.position.x + track.size.x - 184.0f,
             track.position.y - 28.0f},
            {20.0f, 20.0f}
        };
    }

    sf::FloatRect stepPlusBounds() const {
        return {
            {track.position.x + track.size.x - 158.0f,
             track.position.y - 28.0f},
            {20.0f, 20.0f}
        };
    }

    bool valueHit(sf::Vector2f point) const {
        return editBounds().contains(point);
    }

    bool setFromText(const std::string& text, std::string& error) {
        double parsed = value;
        if (!parseNumericInput(
                text,
                NumericInputRules{integer, logarithmic},
                parsed,
                error)) {
            return false;
        }
        if (boolean && parsed != 0.0 && parsed != 1.0) {
            error = "boolean value must be 0 or 1";
            return false;
        }
        value = parsed;
        return true;
    }

    void draw(sf::RenderTarget& target,
              const sf::Font& font,
              bool editing,
              const std::string& inputText,
              bool invalid = false) const {
        target.draw(makeText(
            font,
            label,
            13,
            {track.position.x, track.position.y - 24.0f},
            invalid ? WARNING_OUTLINE : TEXT));
        const std::string display = editing
            ? inputText + "|"
            : boolean
                  ? (value >= 0.5 ? "Requested" : "Off")
                  : formatValue(value, integer, unit);
        if (editing) {
            const sf::FloatRect bounds = editBounds();
            sf::RectangleShape editor(bounds.size);
            editor.setPosition(bounds.position);
            editor.setFillColor(CONTROL_BACKGROUND);
            editor.setOutlineColor(ACCENT);
            editor.setOutlineThickness(1.0f);
            target.draw(editor);
        }
        sf::Text valueText = makeText(
            font,
            display,
            12,
            {track.position.x + track.size.x, track.position.y - 23.0f},
            editing ? TEXT : MUTED);
        if (invalid) {
            valueText.setFillColor(WARNING_TEXT);
        }
        valueText.setOrigin({valueText.getLocalBounds().size.x, 0.0f});
        target.draw(valueText);

        if (integer && !boolean && !editing) {
            for (const auto& control :
                 std::array<std::pair<sf::FloatRect, const char*>, 2>{{
                     {stepMinusBounds(), "-"},
                     {stepPlusBounds(), "+"}
                 }}) {
                sf::RectangleShape box(control.first.size);
                box.setPosition(control.first.position);
                box.setFillColor(CONTROL_BACKGROUND);
                box.setOutlineColor(BORDER);
                box.setOutlineThickness(1.0f);
                target.draw(box);
                target.draw(makeText(
                    font,
                    control.second,
                    12,
                    control.first.position + sf::Vector2f{6.0f, 1.0f},
                    MUTED));
            }
        }

        sf::RectangleShape rail(track.size);
        rail.setPosition(track.position);
        rail.setFillColor(CONTROL_RAIL);
        target.draw(rail);

        const float fraction =
            static_cast<float>(std::clamp(normalized(), 0.0, 1.0));
        sf::RectangleShape fill({track.size.x * fraction, track.size.y});
        fill.setPosition(track.position);
        fill.setFillColor(ACCENT_DARK);
        target.draw(fill);

        sf::CircleShape handle(boolean ? 8.0f : 7.0f);
        const float handleRadius = handle.getRadius();
        handle.setOrigin({handleRadius, handleRadius});
        handle.setPosition({
            track.position.x + track.size.x * fraction,
            track.position.y + track.size.y / 2.0f
        });
        handle.setFillColor(dragging ? sf::Color::White : ACCENT);
        target.draw(handle);
    }
};

struct Button {
    std::string label;
    sf::FloatRect bounds{{0.0f, 0.0f}, {1.0f, 1.0f}};
    bool selected = false;
    bool enabled = true;

    bool hit(sf::Vector2f point) const {
        return enabled && bounds.contains(point);
    }

    void draw(sf::RenderTarget& target, const sf::Font& font) const {
        sf::RectangleShape rectangle(bounds.size);
        rectangle.setPosition(bounds.position);
        rectangle.setFillColor(
            !enabled
                ? BUTTON_DISABLED
                : selected
                      ? ACCENT_DARK
                      : BUTTON_BACKGROUND);
        rectangle.setOutlineColor(selected ? ACCENT : BORDER);
        rectangle.setOutlineThickness(1.0f);
        target.draw(rectangle);

        sf::Text text = makeText(font, label, 14, {0.0f, 0.0f});
        const sf::FloatRect textBounds = text.getLocalBounds();
        text.setPosition({
            bounds.position.x +
                (bounds.size.x - textBounds.size.x) / 2.0f,
            bounds.position.y +
                (bounds.size.y - textBounds.size.y) / 2.0f - 2.0f
        });
        text.setFillColor(enabled ? TEXT : MUTED);
        target.draw(text);
    }
};

#ifdef _WIN32
std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

struct ChildProcess {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
#endif
    bool active = false;

    ~ChildProcess() {
        if (!terminate()) {
            closeHandles();
        }
    }

    bool terminate(std::string* error = nullptr) {
#ifdef _WIN32
        DWORD exitCode = 0;
        if (active && process != nullptr) {
            if (!GetExitCodeProcess(process, &exitCode)) {
                if (error != nullptr) {
                    *error = "Cannot inspect Fluid Solver. Windows error " +
                        std::to_string(GetLastError()) + ".";
                }
                return false;
            }
            if (exitCode == STILL_ACTIVE) {
                if (!TerminateProcess(process, ERROR_CANCELLED)) {
                    if (error != nullptr) {
                        *error = "Cannot stop Fluid Solver. Windows error " +
                            std::to_string(GetLastError()) + ".";
                    }
                    return false;
                }
                if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
                    if (error != nullptr) {
                        *error = "Fluid Solver did not stop within 5 seconds.";
                    }
                    return false;
                }
            }
        }
#else
        (void)error;
#endif
        closeHandles();
        return true;
    }

    void closeHandles() {
#ifdef _WIN32
        if (thread != nullptr) {
            CloseHandle(thread);
            thread = nullptr;
        }
        if (process != nullptr) {
            CloseHandle(process);
            process = nullptr;
        }
#endif
        active = false;
    }

    bool start(
        const std::filesystem::path& solverExecutable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& runDirectory,
        std::string& error) {
        if (active) {
            error = "A simulation is already running.";
            return false;
        }
#ifdef _WIN32
        std::wstring command;
        try {
            command = quoteWindowsArgument(solverExecutable.wstring());
            for (const std::string& argument : arguments) {
                command.push_back(L' ');
                command += quoteWindowsArgument(
                    std::filesystem::u8path(argument).wstring());
            }
        } catch (const std::exception& exception) {
            error = "Cannot encode Fluid Solver arguments: " +
                    std::string(exception.what());
            return false;
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        const std::filesystem::path outputFile =
            runDirectory / "solver-output.txt";
        const std::filesystem::path errorFile =
            runDirectory / "solver-error.txt";
        HANDLE childInput = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        HANDLE childOutput = CreateFileW(
            outputFile.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        HANDLE childError = CreateFileW(
            errorFile.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            &security,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (childInput == INVALID_HANDLE_VALUE ||
            childOutput == INVALID_HANDLE_VALUE ||
            childError == INVALID_HANDLE_VALUE) {
            const DWORD windowsError = GetLastError();
            if (childInput != INVALID_HANDLE_VALUE) {
                CloseHandle(childInput);
            }
            if (childOutput != INVALID_HANDLE_VALUE) {
                CloseHandle(childOutput);
            }
            if (childError != INVALID_HANDLE_VALUE) {
                CloseHandle(childError);
            }
            error = "Cannot open solver log files. Windows error " +
                    std::to_string(windowsError) + ".";
            return false;
        }

        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput;
        startup.hStdOutput = childOutput;
        startup.hStdError = childError;
        PROCESS_INFORMATION information{};
        const BOOL created = CreateProcessW(
            solverExecutable.c_str(),
            commandBuffer.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            runDirectory.c_str(),
            &startup,
            &information);
        const DWORD windowsError = created ? ERROR_SUCCESS : GetLastError();
        CloseHandle(childInput);
        CloseHandle(childOutput);
        CloseHandle(childError);
        if (!created) {
            error = "Cannot start Fluid Solver. Windows error " +
                    std::to_string(windowsError) + ".";
            return false;
        }
        process = information.hProcess;
        thread = information.hThread;
        active = true;
        return true;
#else
        (void)solverExecutable;
        (void)arguments;
        (void)runDirectory;
        error = "External Fluid Solver launch is implemented for Windows only.";
        return false;
#endif
    }

    std::optional<unsigned long> poll() {
        if (!active) {
            return std::nullopt;
        }
#ifdef _WIN32
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(process, &exitCode)) {
            closeHandles();
            return static_cast<unsigned long>(-1);
        }
        if (exitCode == STILL_ACTIVE) {
            return std::nullopt;
        }
        const unsigned long completedCode = exitCode;
        closeHandles();
        return completedCode;
#else
        return std::nullopt;
#endif
    }
};

enum class DisplayMode {
    Setup,
    Results
};

enum class ResultQuantity {
    Pressure,
    Velocity
};

enum class ResultOrigin {
    FluidSolverRun,
    ContinuedFluidSolverRun,
    StoppedFluidSolverRun,
    ImportedFiles
};

enum ParameterIndex : std::size_t {
    WindSpeed,
    Viscosity,
    Density,
    SliceX,
    SliceZ,
    SliceRotation,
    DomainX,
    DomainY,
    CellsX,
    CellsY,
    Cfl,
    TotalTime,
    DtUpdateInterval,
    DtSafety,
    CoarseSorOmega,
    SmootherOmega,
    MgIterations,
    MgTolerance,
    MgMinCoarseSize,
    SaveInterval,
    UseCuda,
    CacheMegabytes,
    ParameterCount
};

struct ParameterGroupInfo {
    std::size_t firstIndex;
    const char* label;
};

constexpr std::array<ParameterGroupInfo, 6> PARAMETER_GROUPS{{
    {WindSpeed, "FLOW"},
    {SliceX, "GEOMETRY"},
    {DomainX, "DOMAIN / GRID"},
    {Cfl, "TIME"},
    {CoarseSorOmega, "PRESSURE / MULTIGRID"},
    {SaveInterval, "OUTPUT / VIEW"}
}};

const char* parameterKey(std::size_t index) {
    static constexpr std::array<const char*, ParameterCount> keys{{
        "U0", "nu", "ro",
        "sliceAngleX", "sliceAngleZ", "sliceRotation",
        "Lx", "Ly", "nx", "ny",
        "CFL", "totalTime", "dtUpdateInterval", "dtSafety",
        "omega", "smootherOmega", "mgIterations", "mgTolerance",
        "mgMinCoarseSize", "saveInterval", "useCuda", "uiCacheMB"
    }};
    return index < keys.size() ? keys[index] : "unknown";
}

std::string parameterHelp(std::size_t index) {
    switch (index) {
    case WindSpeed:
        return "U0: inlet speed used by the Fluid Solver.";
    case Viscosity:
        return "nu: kinematic viscosity; affects Reynolds number and diffusion timestep.";
    case Density:
        return "rho / CLI key ro: physical density used by the solver pressure output.";
    case SliceX:
    case SliceZ:
    case SliceRotation:
        return "Geometry section transform. The UI bakes this into section-adapter.obj.";
    case DomainX:
    case DomainY:
        return "Physical domain size. dx=Lx/nx and dy=Ly/ny.";
    case CellsX:
    case CellsY:
        return "Grid resolution. Typed values may exceed the slider range, subject to validation.";
    case Cfl:
        return "CFL controls the advective timestep restriction; valid range is (0, 1].";
    case TotalTime:
        return "Simulation duration in seconds for a new run.";
    case DtUpdateInterval:
        return "How many solver steps elapse between adaptive dt recalculations.";
    case DtSafety:
        return "Safety multiplier applied to the timestep; valid range is (0, 1].";
    case CoarseSorOmega:
        return "Relaxation omega for the coarsest pressure solve; must be in (0, 2).";
    case SmootherOmega:
        return "Relaxation omega used by multigrid smoothing; must be in (0, 2).";
    case MgIterations:
        return "Maximum/target multigrid V-cycles per pressure solve.";
    case MgTolerance:
        return "Relative residual tolerance for the pressure solve.";
    case MgMinCoarseSize:
        return "Minimum coarse-grid size used when building the multigrid hierarchy.";
    case SaveInterval:
        return "Write one VTK result every N solver steps.";
    case UseCuda:
        return "Requests CUDA in a CUDA-capable build; CPU-only builds force this off.";
    case CacheMegabytes:
        return "Maximum decoded VTK frame cache owned by the UI, in MiB.";
    default:
        return {};
    }
}

struct ProjectedPoint {
    sf::Vector2f position;
    double depth = 0.0;
};

struct PreviewTriangle {
    std::array<sf::Vector2f, 3> points;
    double depth = 0.0;
    sf::Color color;
};

Vec3 add(const Vec3& first, const Vec3& second) {
    return {
        first.x + second.x,
        first.y + second.y,
        first.z + second.z
    };
}

Vec3 multiply(const Vec3& value, double scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

Vec3 subtract(const Vec3& first, const Vec3& second) {
    return {
        first.x - second.x,
        first.y - second.y,
        first.z - second.z
    };
}

Vec3 cross(const Vec3& first, const Vec3& second) {
    return {
        first.y * second.z - first.z * second.y,
        first.z * second.x - first.x * second.z,
        first.x * second.y - first.y * second.x
    };
}

double length(const Vec3& value) {
    return std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z);
}

sf::Color scalarColor(double value, double minimum, double maximum) {
    if (!std::isfinite(value)) {
        return INVALID_COLOR;
    }
    double normalized = 0.5;
    if (maximum > minimum) {
        normalized = std::clamp(
            (value - minimum) / (maximum - minimum),
            0.0,
            1.0);
    }

    struct Stop {
        double position;
        sf::Color color;
    };
    const std::array<Stop, 5> stops{{
        {0.00, {91, 33, 182}},
        {0.25, {38, 92, 214}},
        {0.50, {34, 201, 173}},
        {0.75, {247, 177, 48}},
        {1.00, {220, 43, 43}}
    }};

    for (std::size_t index = 1; index < stops.size(); ++index) {
        if (normalized <= stops[index].position) {
            const Stop& first = stops[index - 1];
            const Stop& second = stops[index];
            const double local =
                (normalized - first.position) /
                (second.position - first.position);
            const auto interpolate = [local](std::uint8_t a, std::uint8_t b) {
                return static_cast<std::uint8_t>(
                    std::lround(
                        static_cast<double>(a) +
                        local *
                            (static_cast<double>(b) -
                             static_cast<double>(a))));
            };
            return {
                interpolate(first.color.r, second.color.r),
                interpolate(first.color.g, second.color.g),
                interpolate(first.color.b, second.color.b)
            };
        }
    }
    return stops.back().color;
}

#ifdef _WIN32
class ScopedFileDropTarget {
public:
    explicit ScopedFileDropTarget(sf::WindowHandle owner)
        : window_(reinterpret_cast<HWND>(owner)) {
        if (window_ != nullptr &&
            SetWindowSubclass(
                window_,
                &ScopedFileDropTarget::windowProcedure,
                SUBCLASS_ID,
                reinterpret_cast<DWORD_PTR>(this)) != FALSE) {
            attached_ = true;
            DragAcceptFiles(window_, TRUE);
        } else {
            window_ = nullptr;
        }
    }

    ~ScopedFileDropTarget() {
        detach();
    }

    ScopedFileDropTarget(const ScopedFileDropTarget&) = delete;
    ScopedFileDropTarget& operator=(const ScopedFileDropTarget&) = delete;

    bool available() const {
        return attached_;
    }

    std::vector<std::vector<std::filesystem::path>> takeBatches() {
        std::vector<std::vector<std::filesystem::path>> result =
            std::move(batches_);
        batches_.clear();
        return result;
    }

    bool takeReadFailure() {
        const bool result = readFailure_;
        readFailure_ = false;
        return result;
    }

private:
    struct DropHandleGuard {
        HDROP handle = nullptr;

        ~DropHandleGuard() {
            if (handle != nullptr) {
                DragFinish(handle);
            }
        }
    };

    static constexpr UINT_PTR SUBCLASS_ID = 0x4346444Du;

    void capture(HDROP drop) noexcept {
        DropHandleGuard guard{drop};
        try {
            const UINT count =
                DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            std::vector<std::filesystem::path> paths;
            paths.reserve(static_cast<std::size_t>(count));
            for (UINT index = 0; index < count; ++index) {
                const UINT length =
                    DragQueryFileW(drop, index, nullptr, 0);
                if (length == 0) {
                    readFailure_ = true;
                    return;
                }
                std::vector<wchar_t> buffer(
                    static_cast<std::size_t>(length) + 1u,
                    L'\0');
                if (DragQueryFileW(
                        drop,
                        index,
                        buffer.data(),
                        length + 1u) != length) {
                    readFailure_ = true;
                    return;
                }
                paths.emplace_back(buffer.data());
            }
            if (!paths.empty()) {
                batches_.push_back(std::move(paths));
            }
        } catch (...) {
            readFailure_ = true;
        }
    }

    void detach() noexcept {
        if (!attached_ || window_ == nullptr) {
            return;
        }
        DragAcceptFiles(window_, FALSE);
        RemoveWindowSubclass(
            window_,
            &ScopedFileDropTarget::windowProcedure,
            SUBCLASS_ID);
        attached_ = false;
        window_ = nullptr;
    }

    static LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData) noexcept {
        auto* self =
            reinterpret_cast<ScopedFileDropTarget*>(referenceData);
        if (message == WM_DROPFILES && self != nullptr) {
            self->capture(reinterpret_cast<HDROP>(wParam));
            return 0;
        }
        if (message == WM_NCDESTROY && self != nullptr) {
            self->attached_ = false;
            self->window_ = nullptr;
            RemoveWindowSubclass(
                window,
                &ScopedFileDropTarget::windowProcedure,
                subclassId);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    HWND window_ = nullptr;
    bool attached_ = false;
    bool readFailure_ = false;
    std::vector<std::vector<std::filesystem::path>> batches_;
};
#endif

std::filesystem::path chooseGeometryFile(sf::WindowHandle owner) {
#ifdef _WIN32
    std::array<wchar_t, 32768> filename{};
    const wchar_t filter[] =
        L"3D models (*.stl;*.obj)\0*.stl;*.obj\0"
        L"STL files (*.stl)\0*.stl\0"
        L"OBJ files (*.obj)\0*.obj\0"
        L"All files (*.*)\0*.*\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = reinterpret_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags =
        OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog) != FALSE) {
        return std::filesystem::path(filename.data());
    }
#else
    (void)owner;
#endif
    return {};
}

std::filesystem::path chooseSolverExecutable(
    sf::WindowHandle owner,
    std::string& error) {
#ifdef _WIN32
    std::array<wchar_t, 32768> filename{};
    const wchar_t filter[] =
        L"Windows executables (*.exe)\0*.exe\0"
        L"All files (*.*)\0*.*\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = reinterpret_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrTitle = L"Select Fluid Solver executable";
    dialog.lpstrDefExt = L"exe";
    dialog.Flags =
        OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog) != FALSE) {
        return std::filesystem::path(filename.data());
    }
    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError != 0) {
        error = "Windows solver file dialog failed with error " +
            std::to_string(dialogError) + ".";
    }
#else
    (void)owner;
    (void)error;
#endif
    return {};
}

std::filesystem::path chooseOutputFolder(
    sf::WindowHandle owner,
    std::string& error) {
#ifdef _WIN32
    BROWSEINFOW dialog{};
    dialog.hwndOwner = reinterpret_cast<HWND>(owner);
    dialog.lpszTitle = L"Select the parent folder for simulation runs";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selection = SHBrowseForFolderW(&dialog);
    if (selection == nullptr) {
        return {};
    }
    std::array<wchar_t, MAX_PATH> path{};
    const BOOL decoded = SHGetPathFromIDListW(selection, path.data());
    CoTaskMemFree(selection);
    if (decoded == FALSE) {
        error = "Windows could not decode the selected output folder.";
        return {};
    }
    return std::filesystem::path(path.data());
#else
    (void)owner;
    error = "Output folder selection is implemented for Windows only.";
    return {};
#endif
}

std::filesystem::path chooseUiConfigFile(
    sf::WindowHandle owner,
    bool save,
    std::string& error) {
#ifdef _WIN32
    std::array<wchar_t, 32768> filename{};
    const wchar_t filter[] =
        L"CFD Mask UI configuration (*.cfdui)\0*.cfdui\0"
        L"Text files (*.txt)\0*.txt\0"
        L"All files (*.*)\0*.*\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = reinterpret_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrTitle = save
        ? L"Save CFD Mask UI configuration"
        : L"Load CFD Mask UI configuration";
    dialog.lpstrDefExt = L"cfdui";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!save) {
        dialog.Flags |= OFN_FILEMUSTEXIST;
    } else {
        dialog.Flags |= OFN_OVERWRITEPROMPT;
    }

    const BOOL accepted = save
        ? GetSaveFileNameW(&dialog)
        : GetOpenFileNameW(&dialog);
    if (accepted != FALSE) {
        return std::filesystem::path(filename.data());
    }
    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError != 0) {
        error = "Windows configuration file dialog failed with error " +
            std::to_string(dialogError) + ".";
    }
#else
    (void)owner;
    (void)save;
    error = "Configuration file dialogs are implemented for Windows only.";
#endif
    return {};
}

std::vector<std::filesystem::path> chooseVtkFiles(
    sf::WindowHandle owner,
    std::string& error) {
#ifdef _WIN32
    std::vector<wchar_t> selectionBuffer(65536u, L'\0');
    const wchar_t filter[] =
        L"VTK solution frames (*.vtk)\0*.vtk\0"
        L"All files (*.*)\0*.*\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = reinterpret_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = selectionBuffer.data();
    dialog.nMaxFile = static_cast<DWORD>(selectionBuffer.size());
    dialog.lpstrTitle = L"Open VTK solution frame(s)";
    dialog.lpstrDefExt = L"vtk";
    dialog.Flags =
        OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        const DWORD dialogError = CommDlgExtendedError();
        if (dialogError != 0) {
            error =
                "Windows VTK file dialog failed with error " +
                std::to_string(dialogError) + ".";
        }
        return {};
    }

    const wchar_t* cursor = selectionBuffer.data();
    const std::filesystem::path first(cursor);
    cursor += std::char_traits<wchar_t>::length(cursor) + 1u;
    if (*cursor == L'\0') {
        return {first};
    }

    std::vector<std::filesystem::path> paths;
    while (*cursor != L'\0') {
        paths.push_back(first / cursor);
        cursor += std::char_traits<wchar_t>::length(cursor) + 1u;
    }
    return paths;
#else
    (void)owner;
    error = "VTK file selection is implemented for Windows only.";
    return {};
#endif
}

void includeDataRange(DataRange& combined, const DataRange& range) {
    if (!range.available) {
        return;
    }
    if (!combined.available) {
        combined = range;
        return;
    }
    combined.minimum = std::min(combined.minimum, range.minimum);
    combined.maximum = std::max(combined.maximum, range.maximum);
}

bool openExplorerTarget(
    sf::WindowHandle owner,
    const ExplorerTarget& target,
    std::string& error) {
#ifdef _WIN32
    if (target.location.empty()) {
        error = "Explorer target is empty.";
        return false;
    }

    std::error_code filesystemError;
    const bool targetExists = target.selectFile
        ? std::filesystem::is_regular_file(
              target.location,
              filesystemError)
        : std::filesystem::is_directory(
              target.location,
              filesystemError);
    if (filesystemError || !targetExists) {
        error =
            (target.selectFile ? "VTK file is unavailable: "
                               : "VTK run directory is unavailable: ") +
            target.location.string();
        return false;
    }

    const std::filesystem::path absolute =
        std::filesystem::absolute(target.location, filesystemError);
    if (filesystemError) {
        error =
            "Cannot resolve Explorer target: " +
            filesystemError.message();
        return false;
    }

    HINSTANCE result = nullptr;
    if (target.selectFile) {
        const std::wstring parameters =
            L"/select,\"" + absolute.wstring() + L"\"";
        result = ShellExecuteW(
            reinterpret_cast<HWND>(owner),
            L"open",
            L"explorer.exe",
            parameters.c_str(),
            nullptr,
            SW_SHOWNORMAL);
    } else {
        result = ShellExecuteW(
            reinterpret_cast<HWND>(owner),
            L"open",
            absolute.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
    }

    const std::intptr_t resultCode =
        reinterpret_cast<std::intptr_t>(result);
    if (resultCode <= 32) {
        error =
            "Windows Explorer failed with code " +
            std::to_string(resultCode) + ".";
        return false;
    }
    return true;
#else
    (void)owner;
    (void)target;
    error = "Opening a VTK location is implemented for Windows only.";
    return false;
#endif
}

std::filesystem::path defaultOutputRoot(
    const std::filesystem::path& executablePath) {
    std::error_code filesystemError;
    const std::filesystem::path current =
        std::filesystem::current_path(filesystemError);
    if (!filesystemError) {
        return (current / "output").lexically_normal();
    }

    // current_path() can fail only in unusual process-environment cases. Keep
    // a usable absolute fallback so the solver's absolute-output contract is
    // still satisfied.
    return (executablePath.parent_path() / "output").lexically_normal();
}

std::filesystem::path createRunDirectory(
    const std::filesystem::path& root,
    std::string& error) {
    if (root.empty()) {
        error = "Output folder is empty.";
        return {};
    }
    const auto timestamp =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        root / ("run-" + std::to_string(timestamp));
    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError) {
        error = "Cannot create run directory: " + filesystemError.message();
        return {};
    }
    return directory;
}

std::string readDiagnosticFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }
    std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    for (char& character : text) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

double contourArea(const std::vector<Vec2>& contour) {
    if (contour.size() < 3) {
        return 0.0;
    }
    double twiceArea = 0.0;
    for (std::size_t index = 0; index < contour.size(); ++index) {
        const Vec2& current = contour[index];
        const Vec2& next = contour[(index + 1) % contour.size()];
        twiceArea += current.x * next.y - next.x * current.y;
    }
    return 0.5 * std::abs(twiceArea);
}

struct SolverExecutableInfo {
    bool valid = false;
    bool recognized = false;
    bool cudaCapable = false;
    std::string version = "unknown";
    std::string build = "Unknown build";
    std::string detail;
};

bool binaryContains(const std::filesystem::path& path,
                    const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    return bytes.find(needle) != std::string::npos;
}

bool binaryContainsUtf16Ascii(const std::filesystem::path& path,
                              const std::string& text) {
    std::string encoded;
    encoded.reserve(text.size() * 2u);
    for (const unsigned char character : text) {
        encoded.push_back(static_cast<char>(character));
        encoded.push_back('\0');
    }
    return binaryContains(path, encoded);
}

SolverExecutableInfo inspectSolverExecutable(
    const std::filesystem::path& executable) {
    SolverExecutableInfo info;
    std::string error;
    info.valid = validateFluidSolverExecutable(executable, error);
    if (!info.valid) {
        info.detail = error;
        return info;
    }

    std::string filename = executable.filename().string();
    std::transform(
        filename.begin(), filename.end(), filename.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    const bool correctName = filename == "fluid solver.exe";
    const bool productMarker =
        binaryContainsUtf16Ascii(executable, "Fluid Solver") ||
        binaryContains(executable, "Fluid Solver");
    info.recognized = correctName && productMarker;
    info.cudaCapable =
        binaryContains(executable, "MultigridCuda.cu") ||
        binaryContains(executable, "CUDA error at");
    if (binaryContainsUtf16Ascii(executable, "0.1.0")) {
        info.version = "0.1.0";
    }
    info.build = info.cudaCapable
        ? "AVX2 + OpenMP + CUDA"
        : "AVX2 + OpenMP";
    if (!info.recognized) {
        info.detail =
            "Executable is not recognized as the expected Fluid Solver build.";
    }
    return info;
}

bool confirmUseLargestContour(sf::WindowHandle owner,
                              std::size_t contourCount) {
#ifdef _WIN32
    const std::wstring message =
        L"The current Fluid Solver uses one closed contour, but the UI "
        L"detected " + std::to_wstring(contourCount) +
        L" disconnected contours.\n\n"
        L"Continue using only the largest contour?\n\n"
        L"The preview and written adapter will be reduced to that contour "
        L"so the UI and solver solve the same geometry.";
    return MessageBoxW(
        reinterpret_cast<HWND>(owner),
        message.c_str(),
        L"Multiple contours detected",
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
#else
    (void)owner;
    (void)contourCount;
    return false;
#endif
}

bool confirmStopAndExit(sf::WindowHandle owner) {
#ifdef _WIN32
    return MessageBoxW(
        reinterpret_cast<HWND>(owner),
        L"A simulation is still running. Stop the Fluid Solver and exit?",
        L"Simulation is running",
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
#else
    (void)owner;
    return false;
#endif
}

} // namespace

class Application::Implementation {
public:
    Implementation(
        std::filesystem::path executablePath,
        std::filesystem::path initialModelPath)
        : executablePath_(std::move(executablePath)),
          initialModelPath_(std::move(initialModelPath)),
          solverSelectionFile_(
              executablePath_.parent_path() / "solver-selection.txt"),
          preferencesFile_(
              executablePath_.parent_path() / "ui-preferences.txt"),
          fluidSolverExecutable_(resolveFluidSolverExecutable(
              executablePath_,
              std::filesystem::path(CFD_SOLVER_EXE))),
          outputRoot_(defaultOutputRoot(executablePath_)),
          sliders_{
              Slider{"Wind speed U0", "m/s", 0.0, 200.0, 1.0, false, false},
              Slider{"Viscosity nu", "m2/s", 1e-8, 10.0, 0.01, false, true},
              Slider{"Density rho (ro)", "kg/m3", 0.01, 5000.0, 1.225, false, true},
              Slider{"Slice X", "deg", -180.0, 180.0, 90.0, true, false},
              Slider{"Slice Z", "deg", -180.0, 180.0, 90.0, true, false},
              Slider{"Slice rotation", "deg", -180.0, 180.0, 0.0, false, false},
              Slider{"Domain Lx", "m", 0.01, 100.0, 1.0, false, true},
              Slider{"Domain Ly", "m", 0.01, 100.0, 1.0, false, true},
              Slider{"Cells nx", "", 8.0, 5000.0, 50.0, true, true},
              Slider{"Cells ny", "", 8.0, 5000.0, 50.0, true, true},
              Slider{"CFL", "", 0.01, 1.0, 0.5, false, false},
              Slider{"Total time", "s", 0.001, 10000.0, 10.0, false, true},
              Slider{"dt update interval", "steps", 1.0, 1000.0, 5.0, true, false},
              Slider{"dt safety", "", 0.01, 1.0, 0.9, false, false},
              Slider{"Coarse SOR omega", "", 0.1, 1.99, 1.85, false, false},
              Slider{"MG smoother omega", "", 0.1, 1.99, 1.15, false, false},
              Slider{"MG V-cycles", "", 1.0, 100.0, 2.0, true, false},
              Slider{"MG tolerance", "", 1e-10, 1e-2, 1e-4, false, true},
              Slider{"MG minimum coarse size", "cells", 1.0, 512.0, 8.0, true, false},
              Slider{"VTK save interval", "steps", 1.0, 100000.0, 20.0, true, true},
              Slider{"Request CUDA", "", 0.0, 1.0, 1.0, true, false, true},
              Slider{"VTK cache", "MB", 128.0, 4096.0, 1024.0, true, true}
          } {
        for (Slider& slider : sliders_) {
            slider.defaultValue = slider.value;
        }
        loadPreferences();
        std::string selectionError;
        const std::optional<std::filesystem::path> selected =
            readFluidSolverSelection(
                solverSelectionFile_,
                selectionError);
        if (selected) {
            fluidSolverExecutable_ = *selected;
        } else if (!selectionError.empty()) {
            status_ =
                "Saved solver selection is unavailable; using fallback. " +
                selectionError;
        }
        std::string solverError;
        if (!validateFluidSolverExecutable(
                fluidSolverExecutable_, solverError)) {
            status_ =
                "Default solver not found; use Select solver EXE before "
                "running. " + solverError;
        }
        refreshSolverInfo();
        if (solverInfo_.valid && !solverInfo_.recognized) {
            status_ =
                "Default EXE is not recognized as Fluid Solver.exe; select "
                "one of the finished Fluid Solver builds before running.";
        }
        applyCacheBudget();
    }

    int run() {
        if (!loadFont()) {
            return 2;
        }

        sf::RenderWindow window(
            sf::VideoMode({1600u, 900u}),
            std::string("CFD Mask UI ") + CFD_MASK_UI_VERSION,
            sf::Style::Default,
            sf::State::Windowed);
        window.setMinimumSize(sf::Vector2u{1000u, 800u});
        window.setFramerateLimit(60);
        window_ = &window;
#ifdef _WIN32
        ScopedFileDropTarget fileDropTarget(window.getNativeHandle());
        if (!fileDropTarget.available()) {
            status_ =
                "VTK drag-and-drop is unavailable; use Open VTK frame(s).";
        }
#endif
        if (!initialModelPath_.empty()) {
            std::error_code pathError;
            if (std::filesystem::is_directory(
                    initialModelPath_,
                    pathError) &&
                !pathError) {
                loadExternalResultInputs({initialModelPath_});
            } else if (initialModelPath_.extension() == ".vtk") {
                loadExternalResultInputs({initialModelPath_});
            } else {
                loadGeometry(initialModelPath_);
            }
        }

        const auto hasContinuousInput = [&]() {
            if (!window.hasFocus()) {
                return false;
            }
            const bool planarMovement =
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::W) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::A) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::S) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::D);
            if (mode_ == DisplayMode::Results) {
                return planarMovement;
            }
            return planarMovement ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Left) ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Right) ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Up) ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Down) ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Q) ||
                   sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::E);
        };
        const auto hasBackgroundWork = [&]() {
            return solverProcess_.active ||
                   resultCatalogFuture_.valid() ||
                   selectedFrameFuture_.valid() ||
                   playingFrames_;
        };

        sf::Clock frameClock;
        bool redrawRequested = true;
        while (window.isOpen()) {
            const bool continuousInputBefore = hasContinuousInput();
            const bool backgroundWorkBefore = hasBackgroundWork();
            const sf::Time waitDuration =
                redrawRequested
                    ? sf::milliseconds(1)
                    : (continuousInputBefore
                           ? sf::milliseconds(16)
                           : (backgroundWorkBefore
                                  ? sf::milliseconds(100)
                                  : sf::Time::Zero));
            bool receivedEvent = false;
            if (const std::optional<sf::Event> event =
                    window.waitEvent(waitDuration)) {
                handleEvent(*event);
                receivedEvent = true;
            }
            while (const std::optional<sf::Event> event = window.pollEvent()) {
                handleEvent(*event);
                receivedEvent = true;
            }
            if (!window.isOpen()) {
                break;
            }
            syncWindowLayout(window.getSize());
#ifdef _WIN32
            bool receivedFileDrop = false;
            if (fileDropTarget.takeReadFailure()) {
                status_ = "VTK drop failed while reading Windows file paths.";
                receivedFileDrop = true;
            }
            for (const auto& batch : fileDropTarget.takeBatches()) {
                loadExternalResultInputs(batch);
                receivedFileDrop = true;
            }
#endif

            float elapsed =
                std::min(frameClock.restart().asSeconds(), 0.1f);
            const bool continuousInputAfterEvents = hasContinuousInput();
            if (!continuousInputBefore && continuousInputAfterEvents) {
                elapsed = 0.0f;
            }
            update(elapsed);

            redrawRequested =
                redrawRequested || receivedEvent || continuousInputBefore ||
                continuousInputAfterEvents || backgroundWorkBefore ||
                hasBackgroundWork();
#ifdef _WIN32
            redrawRequested = redrawRequested || receivedFileDrop;
#endif
            if (!redrawRequested) {
                continue;
            }

            window.clear(BACKGROUND);
            if (mode_ == DisplayMode::Setup) {
                drawSetup();
            } else {
                drawResults();
            }
            drawTopTabs();
            drawLoadingIndicator();
            drawStatus();
            window.display();
            redrawRequested = false;
        }
        window_ = nullptr;
        return 0;
    }

private:
    bool loadFont() {
        const std::array<std::filesystem::path, 3> candidates{{
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            executablePath_.parent_path() / "assets/Tuffy.ttf"
        }};
        for (const auto& candidate : candidates) {
            if (font_.openFromFile(candidate)) {
                return true;
            }
        }
        return false;
    }

    void syncWindowLayout(sf::Vector2u size) {
        if (size.x == 0u || size.y == 0u ||
            (size.x == layoutSize_.x && size.y == layoutSize_.y)) {
            return;
        }
        const sf::FloatRect clientBounds{
            {0.0f, 0.0f},
            {
                static_cast<float>(size.x),
                static_cast<float>(size.y)
            }
        };
        window_->setView(sf::View(clientBounds));
        updateLayout(size);
    }

    void updateLayout(sf::Vector2u size) {
        layoutSize_ = size;
        const float width = static_cast<float>(size.x);
        const float height = static_cast<float>(size.y);
        setupTab_.bounds = {{12.0f, 10.0f}, {96.0f, 34.0f}};
        resultsTab_.bounds = {{114.0f, 10.0f}, {96.0f, 34.0f}};
        openVtkButton_.bounds = {{216.0f, 10.0f}, {176.0f, 34.0f}};
        stopSimulationButton_.bounds = {{400.0f, 10.0f}, {132.0f, 34.0f}};
        revealVtkButton_.bounds = {{540.0f, 10.0f}, {190.0f, 34.0f}};
        solverExeButton_.bounds = {{738.0f, 10.0f}, {196.0f, 34.0f}};
        importButton_.bounds = {{18.0f, 60.0f}, {143.0f, 34.0f}};
        outputFolderButton_.bounds = {{169.0f, 60.0f}, {143.0f, 34.0f}};
        resetDefaultsButton_.bounds = {
            {18.0f, height - 106.0f}, {92.0f, 34.0f}};
        saveConfigButton_.bounds = {
            {118.0f, height - 106.0f}, {92.0f, 34.0f}};
        loadConfigButton_.bounds = {
            {218.0f, height - 106.0f}, {94.0f, 34.0f}};
        generateButton_.bounds = {
            {18.0f, height - 62.0f},
            {143.0f, 38.0f}
        };
        continueButton_.bounds = {
            {169.0f, height - 62.0f},
            {143.0f, 38.0f}
        };

        const float parameterBottom =
            std::max(PARAMETER_TOP + 40.0f, height - PARAMETER_BOTTOM_MARGIN);
        const float viewportHeight = parameterBottom - PARAMETER_TOP;
        const float contentHeight =
            static_cast<float>(sliders_.size()) * PARAMETER_ROW_HEIGHT +
            static_cast<float>(PARAMETER_GROUPS.size()) *
                PARAMETER_GROUP_HEIGHT;
        maxParameterScroll_ = std::max(0.0f, contentHeight - viewportHeight);
        parameterScrollOffset_ =
            clampFloat(parameterScrollOffset_, 0.0f, maxParameterScroll_);

        for (std::size_t index = 0; index < sliders_.size(); ++index) {
            const std::size_t groupsBeforeOrAt = static_cast<std::size_t>(
                std::count_if(
                    PARAMETER_GROUPS.begin(),
                    PARAMETER_GROUPS.end(),
                    [index](const ParameterGroupInfo& group) {
                        return group.firstIndex <= index;
                    }));
            sliders_[index].track = {
                {
                    20.0f,
                    PARAMETER_TOP + 24.0f +
                        static_cast<float>(index) * PARAMETER_ROW_HEIGHT -
                        parameterScrollOffset_ +
                        static_cast<float>(groupsBeforeOrAt) *
                            PARAMETER_GROUP_HEIGHT
                },
                {278.0f, 5.0f}
            };
        }

        setupViewport_ = {
            {LEFT_PANEL_WIDTH + 10.0f, 58.0f},
            {
                std::max(320.0f, width - LEFT_PANEL_WIDTH - 28.0f),
                std::max(300.0f, height - 100.0f)
            }
        };

        pressureButton_.bounds = {{20.0f, 72.0f}, {126.0f, 36.0f}};
        velocityButton_.bounds = {{154.0f, 72.0f}, {126.0f, 36.0f}};
        vectorButton_.bounds = {{288.0f, 72.0f}, {144.0f, 36.0f}};
        rangeButton_.bounds = {{440.0f, 72.0f}, {144.0f, 36.0f}};
        playbackButton_.bounds = {{592.0f, 72.0f}, {110.0f, 36.0f}};
        runDetailsButton_.bounds = {{710.0f, 72.0f}, {130.0f, 36.0f}};
        resultViewport_ = {
            {20.0f, 122.0f},
            {
                std::max(320.0f, width - 170.0f),
                std::max(260.0f, height - 220.0f)
            }
        };
        legendBounds_ = {
            {width - 128.0f, 150.0f},
            {34.0f, std::max(180.0f, height - 330.0f)}
        };
        zoomTrack_ = {
            {resultViewport_.position.x + 90.0f, height - 70.0f},
            {std::max(160.0f, resultViewport_.size.x * 0.36f), 5.0f}
        };
        frameTrack_ = {
            {
                resultViewport_.position.x +
                    resultViewport_.size.x * 0.55f,
                height - 70.0f
            },
            {std::max(160.0f, resultViewport_.size.x * 0.35f), 5.0f}
        };

        syncControlState();
    }

    void syncControlState() {
        setupTab_.selected = mode_ == DisplayMode::Setup;
        resultsTab_.selected = mode_ == DisplayMode::Results;
        resultsTab_.enabled = !frames_.empty();
        pressureButton_.selected =
            resultQuantity_ == ResultQuantity::Pressure;
        velocityButton_.selected =
            resultQuantity_ == ResultQuantity::Velocity;
        vectorButton_.label =
            showVelocityVectors_ ? "Vectors: On" : "Vectors: Off";
        vectorButton_.selected = showVelocityVectors_;
        vectorButton_.enabled = !frames_.empty();
        const bool loadingResults =
            resultCatalogFuture_.valid() || selectedFrameFuture_.valid();
        const bool solverAvailable =
            solverInfo_.valid && solverInfo_.recognized;
        generateButton_.enabled =
            solverAvailable && !geometry_.empty() &&
            !solverProcess_.active && !loadingResults;
        continueButton_.enabled = false;
        continueButton_.label = "Continue VTK: later";
        outputFolderButton_.enabled =
            !solverProcess_.active && !loadingResults;
        openVtkButton_.enabled = !solverProcess_.active && !loadingResults;
        stopSimulationButton_.enabled = solverProcess_.active;
        solverExeButton_.enabled = !solverProcess_.active;
        resetDefaultsButton_.enabled = !solverProcess_.active && !loadingResults;
        saveConfigButton_.enabled = !solverProcess_.active && !loadingResults;
        loadConfigButton_.enabled = !solverProcess_.active && !loadingResults;
        revealVtkButton_.enabled = currentExplorerTarget().has_value();
        rangeButton_.enabled = !frames_.empty();
        rangeButton_.label = useSeriesRange_ ? "Range: Series" : "Range: Frame";
        playbackButton_.enabled = frames_.size() > 1;
        playbackButton_.label = playingFrames_ ? "Pause" : "Play";
        runDetailsButton_.enabled =
            !currentRunDirectory_.empty() || !frames_.empty();
        runDetailsButton_.selected = showRunDetails_;
    }

    void handleEvent(const sf::Event& event) {
        if (event.is<sf::Event::Closed>()) {
            if (solverProcess_.active &&
                !confirmStopAndExit(window_->getNativeHandle())) {
                return;
            }
            if (solverProcess_.active) {
                std::string ignored;
                solverProcess_.terminate(&ignored);
            }
            window_->close();
            return;
        }
        if (const auto* resized = event.getIf<sf::Event::Resized>()) {
            endDragging();
            syncWindowLayout(resized->size);
            return;
        }
        if (event.is<sf::Event::FocusLost>()) {
            endDragging();
            cancelSliderEdit(false);
            return;
        }
        if (!window_->hasFocus()) {
            return;
        }

        if (editingSlider_.has_value()) {
            if (const auto* key = event.getIf<sf::Event::KeyPressed>()) {
                handleSliderEditKey(*key);
                return;
            }
            if (const auto* text = event.getIf<sf::Event::TextEntered>()) {
                handleSliderEditText(text->unicode);
                return;
            }
        }

        if (const auto* key = event.getIf<sf::Event::KeyPressed>()) {
            if (mode_ == DisplayMode::Results) {
                if (handleResultsKeyPressed(*key)) {
                    return;
                }
            }
        }

        if (const auto* pressed =
                event.getIf<sf::Event::MouseButtonPressed>()) {
            handleMousePressed(
                pressed->button,
                window_->mapPixelToCoords(pressed->position));
        } else if (const auto* released =
                       event.getIf<sf::Event::MouseButtonReleased>()) {
            handleMouseReleased(released->button);
        } else if (const auto* moved =
                       event.getIf<sf::Event::MouseMoved>()) {
            handleMouseMoved(window_->mapPixelToCoords(moved->position));
        } else if (const auto* wheel =
                       event.getIf<sf::Event::MouseWheelScrolled>()) {
            handleWheel(
                window_->mapPixelToCoords(wheel->position),
                wheel->delta);
        }
    }

    void handleMousePressed(
        sf::Mouse::Button button,
        sf::Vector2f position) {
        lastMouse_ = position;

        if (button == sf::Mouse::Button::Left &&
            editingSlider_.has_value()) {
            const bool insideEditor =
                mode_ == DisplayMode::Setup &&
                sliders_[*editingSlider_].valueHit(position);
            if (!insideEditor && !commitSliderEdit()) {
                return;
            }
        }

        if (button == sf::Mouse::Button::Left &&
            setupTab_.hit(position)) {
            mode_ = DisplayMode::Setup;
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            resultsTab_.hit(position)) {
            mode_ = DisplayMode::Results;
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            openVtkButton_.hit(position)) {
            openVtkFiles();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            revealVtkButton_.hit(position)) {
            revealVtkLocation();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            solverExeButton_.hit(position)) {
            selectFluidSolverExecutable();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            stopSimulationButton_.hit(position)) {
            stopSimulationAndLoadFrames();
            return;
        }

        if (mode_ == DisplayMode::Setup) {
            handleSetupMousePressed(button, position);
        } else {
            handleResultsMousePressed(button, position);
        }
    }

    void handleSetupMousePressed(
        sf::Mouse::Button button,
        sf::Vector2f position) {
        if (button == sf::Mouse::Button::Left &&
            importButton_.hit(position)) {
            importGeometry();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            outputFolderButton_.hit(position)) {
            selectOutputFolder();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            resetDefaultsButton_.hit(position)) {
            resetDefaults();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            saveConfigButton_.hit(position)) {
            saveConfiguration();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            loadConfigButton_.hit(position)) {
            loadConfiguration();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            maxParameterScroll_ > 0.0f) {
            const sf::FloatRect thumb = parameterScrollbarThumb();
            sf::FloatRect hit = parameterScrollbarRail();
            hit.position.x -= 5.0f;
            hit.size.x += 10.0f;
            if (hit.contains(position)) {
                draggingParameterScrollbar_ = true;
                if (thumb.contains(position)) {
                    parameterScrollbarGrabOffset_ =
                        position.y - thumb.position.y;
                } else {
                    parameterScrollbarGrabOffset_ =
                        thumb.size.y * 0.5f;
                    setParameterScrollFromThumb(
                        position.y - parameterScrollbarGrabOffset_);
                }
                return;
            }
        }
        if (button == sf::Mouse::Button::Left &&
            generateButton_.hit(position)) {
            generateAndRun();
            return;
        }
        if (button == sf::Mouse::Button::Left &&
            continueButton_.hit(position)) {
            continueSelectedFrame();
            return;
        }
        if (button == sf::Mouse::Button::Left) {
            for (std::size_t index = 0; index < sliders_.size(); ++index) {
                if (!parameterRowOnScreen(index)) {
                    continue;
                }
                if (sliders_[index].integer && !sliders_[index].boolean &&
                    sliders_[index].stepMinusBounds().contains(position)) {
                    sliders_[index].value = std::max(sliders_[index].minimum, sliders_[index].value - 1.0);
                    invalidSlider_.reset();
                    if (index == CacheMegabytes) {
                        applyCacheBudget();
                        savePreferences();
                    }
                    return;
                }
                if (sliders_[index].integer && !sliders_[index].boolean &&
                    sliders_[index].stepPlusBounds().contains(position)) {
                    sliders_[index].value = std::min(sliders_[index].maximum, sliders_[index].value + 1.0);
                    invalidSlider_.reset();
                    if (index == CacheMegabytes) {
                        applyCacheBudget();
                        savePreferences();
                    }
                    return;
                }
                if (sliders_[index].valueHit(position)) {
                    handleSliderValueClick(index);
                    return;
                }
            }
        }
        if (button == sf::Mouse::Button::Left) {
            for (std::size_t index = 0; index < sliders_.size(); ++index) {
                if (!parameterRowOnScreen(index)) {
                    continue;
                }
                if (sliders_[index].hit(position)) {
                    if (index == UseCuda && !solverInfo_.cudaCapable) {
                        status_ =
                            "CUDA is unavailable in the selected CPU-only "
                            "Fluid Solver build.";
                        sliders_[UseCuda].value = 0.0;
                        return;
                    }
                    activeSlider_ = index;
                    sliders_[index].dragging = true;
                    sliders_[index].setFromX(position.x);
                    return;
                }
            }
        }

        const sf::FloatRect invertBox = invertBounds();
        if (button == sf::Mouse::Button::Left &&
            invertBox.contains(position)) {
            invertSection_ = !invertSection_;
            return;
        }
        if (!setupViewport_.contains(position)) {
            return;
        }
        if (horizontalSliceTrack().contains(position) &&
            button == sf::Mouse::Button::Left) {
            draggingHorizontalSlice_ = true;
            setHorizontalSlice(position.x);
            return;
        }
        if (verticalSliceTrack().contains(position) &&
            button == sf::Mouse::Button::Left) {
            draggingVerticalSlice_ = true;
            setVerticalSlice(position.y);
            return;
        }

        if (button == sf::Mouse::Button::Right) {
            rotatingRoll_ = true;
        } else if (button == sf::Mouse::Button::Left) {
            rotatingObject_ = true;
        }
    }

    void handleResultsMousePressed(
        sf::Mouse::Button button,
        sf::Vector2f position) {
        if (button != sf::Mouse::Button::Left) {
            return;
        }
        if (pressureButton_.hit(position)) {
            resultQuantity_ = ResultQuantity::Pressure;
            resultTextureCacheValid_ = false;
            return;
        }
        if (velocityButton_.hit(position)) {
            resultQuantity_ = ResultQuantity::Velocity;
            resultTextureCacheValid_ = false;
            return;
        }
        if (vectorButton_.hit(position)) {
            showVelocityVectors_ = !showVelocityVectors_;
            status_ = showVelocityVectors_
                ? "Velocity vectors enabled: arrows point with local flow."
                : "Velocity vectors disabled.";
            return;
        }
        if (rangeButton_.hit(position)) {
            useSeriesRange_ = !useSeriesRange_;
            resultTextureCacheValid_ = false;
            status_ = useSeriesRange_
                ? "Result colors use the full VTK series range."
                : "Result colors use the active frame range.";
            return;
        }
        if (playbackButton_.hit(position)) {
            playingFrames_ = !playingFrames_;
            playbackAccumulator_ = 0.0f;
            status_ = playingFrames_
                ? "VTK playback started. Space pauses."
                : "VTK playback paused.";
            return;
        }
        if (runDetailsButton_.hit(position)) {
            showRunDetails_ = !showRunDetails_;
            if (showRunDetails_) {
                refreshRunDetailsText();
            }
            return;
        }
        if (zoomHitBounds().contains(position)) {
            draggingZoom_ = true;
            setZoomFromSlider(position.x);
            return;
        }
        if (frameHitBounds().contains(position) && !frames_.empty()) {
            draggingFrame_ = true;
            setFrameFromSlider(position.x);
            return;
        }
        if (resultViewport_.contains(position)) {
            panningResults_ = true;
        }
    }

    bool handleResultsKeyPressed(const sf::Event::KeyPressed& key) {
        if (frames_.empty()) {
            return false;
        }
        const std::size_t current = desiredFrame_.value_or(selectedFrame_);
        if (key.code == sf::Keyboard::Key::Left) {
            playingFrames_ = false;
            requestSelectedFrame(current == 0 ? 0 : current - 1);
            return true;
        }
        if (key.code == sf::Keyboard::Key::Right) {
            playingFrames_ = false;
            requestSelectedFrame(
                std::min(current + 1, frames_.size() - 1));
            return true;
        }
        if (key.code == sf::Keyboard::Key::Home) {
            playingFrames_ = false;
            requestSelectedFrame(0);
            return true;
        }
        if (key.code == sf::Keyboard::Key::End) {
            playingFrames_ = false;
            requestSelectedFrame(frames_.size() - 1);
            return true;
        }
        if (key.code == sf::Keyboard::Key::Space) {
            playingFrames_ = !playingFrames_;
            playbackAccumulator_ = 0.0f;
            return true;
        }
        return false;
    }

    void handleMouseReleased(sf::Mouse::Button button) {
        if (button == sf::Mouse::Button::Left ||
            button == sf::Mouse::Button::Right) {
            endDragging();
        }
    }

    void handleMouseMoved(sf::Vector2f position) {
        const sf::Vector2f delta = position - lastMouse_;
        lastMouse_ = position;
        if (draggingParameterScrollbar_) {
            setParameterScrollFromThumb(
                position.y - parameterScrollbarGrabOffset_);
        } else if (activeSlider_.has_value()) {
            sliders_[*activeSlider_].setFromX(position.x);
        } else if (draggingHorizontalSlice_) {
            setHorizontalSlice(position.x);
        } else if (draggingVerticalSlice_) {
            setVerticalSlice(position.y);
        } else if (rotatingObject_) {
            const bool shift =
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::LShift) ||
                sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::RShift);
            if (shift) {
                sliders_[SliceRotation].value =
                    wrapDegrees(
                        sliders_[SliceRotation].value +
                        static_cast<double>(delta.x) * 0.4);
            } else {
                sliders_[SliceZ].value =
                    snapCardinalDegrees(
                        sliders_[SliceZ].value +
                        static_cast<double>(delta.x) * 0.35);
                sliders_[SliceX].value =
                    snapCardinalDegrees(
                        sliders_[SliceX].value -
                        static_cast<double>(delta.y) * 0.35);
            }
        } else if (rotatingRoll_) {
            sliders_[SliceRotation].value =
                wrapDegrees(
                    sliders_[SliceRotation].value +
                    static_cast<double>(delta.x) * 0.4);
        } else if (draggingZoom_) {
            setZoomFromSlider(position.x);
        } else if (draggingFrame_) {
            setFrameFromSlider(position.x);
        } else if (panningResults_) {
            resultPan_ += delta;
        }
    }

    void handleWheel(sf::Vector2f position, float delta) {
        if (mode_ == DisplayMode::Setup &&
            parameterViewport().contains(position)) {
            parameterScrollOffset_ = clampFloat(
                parameterScrollOffset_ - delta * PARAMETER_SCROLL_STEP,
                0.0f,
                maxParameterScroll_);
            activeSlider_.reset();
            updateLayout(layoutSize_);
            return;
        }
        if (mode_ == DisplayMode::Setup &&
            setupViewport_.contains(position)) {
            setupZoom_ = clampFloat(
                setupZoom_ * std::pow(1.12f, delta),
                0.35f,
                5.0f);
            return;
        }
        if (mode_ == DisplayMode::Results &&
            resultViewport_.contains(position) &&
            !frames_.empty()) {
            zoomResultsAt(position, std::pow(1.15f, delta));
        }
    }

    void endDragging() {
        const std::optional<std::size_t> releasedSlider = activeSlider_;
        if (activeSlider_.has_value()) {
            sliders_[*activeSlider_].dragging = false;
        }
        activeSlider_.reset();
        if (releasedSlider == CacheMegabytes) {
            applyCacheBudget();
            savePreferences();
        }
        if (releasedSlider.has_value()) {
            invalidSlider_.reset();
        }
        draggingParameterScrollbar_ = false;
        draggingHorizontalSlice_ = false;
        draggingVerticalSlice_ = false;
        rotatingObject_ = false;
        rotatingRoll_ = false;
        draggingZoom_ = false;
        draggingFrame_ = false;
        panningResults_ = false;
    }

    void handleSliderValueClick(std::size_t index) {
        if (editingSlider_ == index) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool doubleClick =
            lastValueClickSlider_ == index &&
            now - lastValueClickTime_ <= std::chrono::milliseconds(500);
        lastValueClickSlider_ = index;
        lastValueClickTime_ = now;
        if (!doubleClick) {
            status_ =
                "Double-click " + sliders_[index].label +
                " value to type an exact number.";
            return;
        }

        editingSlider_ = index;
        sliderEditText_ =
            editableNumber(sliders_[index].value, sliders_[index].integer);
        lastValueClickSlider_.reset();
        status_ =
            "Editing " + sliders_[index].label +
            ": Enter applies; Esc cancels.";
    }

    void handleSliderEditKey(const sf::Event::KeyPressed& key) {
        if (!editingSlider_.has_value()) {
            return;
        }
        if (key.code == sf::Keyboard::Key::Enter) {
            commitSliderEdit();
        } else if (key.code == sf::Keyboard::Key::Escape) {
            cancelSliderEdit(true);
        } else if (key.code == sf::Keyboard::Key::Backspace &&
                   !sliderEditText_.empty()) {
            sliderEditText_.pop_back();
        }
    }

    void handleSliderEditText(char32_t unicode) {
        if (!editingSlider_.has_value() ||
            sliderEditText_.size() >= 64 ||
            unicode > 127) {
            return;
        }
        const char character = static_cast<char>(unicode);
        static const std::string allowed = "0123456789+-.eE";
        if (allowed.find(character) != std::string::npos) {
            sliderEditText_.push_back(character);
        }
    }

    bool commitSliderEdit() {
        if (!editingSlider_.has_value()) {
            return true;
        }
        const std::size_t index = *editingSlider_;
        std::string error;
        if (!sliders_[index].setFromText(sliderEditText_, error)) {
            status_ =
                "Invalid " + sliders_[index].label + ": " + error + ".";
            return false;
        }
        if (index == UseCuda && !solverInfo_.cudaCapable) {
            sliders_[index].value = 0.0;
            status_ =
                "CUDA is unavailable in the selected CPU-only Fluid Solver "
                "build; Request CUDA remains Off.";
        } else {
            status_ =
                sliders_[index].label + " = " +
                formatValue(
                    sliders_[index].value,
                    sliders_[index].integer,
                    sliders_[index].unit) +
                ".";
        }
        if (index == CacheMegabytes) {
            applyCacheBudget();
            savePreferences();
        }
        invalidSlider_.reset();
        editingSlider_.reset();
        sliderEditText_.clear();
        return true;
    }

    void cancelSliderEdit(bool announce) {
        if (!editingSlider_.has_value()) {
            return;
        }
        const std::string label = sliders_[*editingSlider_].label;
        editingSlider_.reset();
        sliderEditText_.clear();
        if (announce) {
            status_ = label + " edit cancelled.";
        }
    }

    void update(float elapsed) {
        pollResultCatalog();
        pollSelectedFrame();
        if (mode_ == DisplayMode::Results && playingFrames_ &&
            frames_.size() > 1 && !selectedFrameFuture_.valid() &&
            !resultCatalogFuture_.valid()) {
            playbackAccumulator_ += elapsed;
            if (playbackAccumulator_ >= 0.12f) {
                playbackAccumulator_ = 0.0f;
                const std::size_t next = (selectedFrame_ + 1) % frames_.size();
                requestSelectedFrame(next);
            }
        }
        if (window_->hasFocus()) {
            const double rotationSpeed = 70.0 * elapsed;
            if (mode_ == DisplayMode::Setup) {
                const bool rotateLeft =
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::A) ||
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Left);
                const bool rotateRight =
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::D) ||
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Right);
                const bool rotateUp =
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::W) ||
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Up);
                const bool rotateDown =
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::S) ||
                    sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Down);
                if (rotateLeft) {
                    sliders_[SliceZ].value =
                        snapCardinalDegrees(
                            sliders_[SliceZ].value - rotationSpeed);
                }
                if (rotateRight) {
                    sliders_[SliceZ].value =
                        snapCardinalDegrees(
                            sliders_[SliceZ].value + rotationSpeed);
                }
                if (rotateUp) {
                    sliders_[SliceX].value =
                        snapCardinalDegrees(
                            sliders_[SliceX].value + rotationSpeed);
                }
                if (rotateDown) {
                    sliders_[SliceX].value =
                        snapCardinalDegrees(
                            sliders_[SliceX].value - rotationSpeed);
                }
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Q)) {
                    sliders_[SliceRotation].value =
                        wrapDegrees(
                            sliders_[SliceRotation].value - rotationSpeed);
                }
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::E)) {
                    sliders_[SliceRotation].value =
                        wrapDegrees(
                            sliders_[SliceRotation].value + rotationSpeed);
                }
            } else {
                const float panSpeed = 190.0f * elapsed;
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::W)) {
                    resultPan_.y -= panSpeed;
                }
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::S)) {
                    resultPan_.y += panSpeed;
                }
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::A)) {
                    resultPan_.x -= panSpeed;
                }
                if (sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::D)) {
                    resultPan_.x += panSpeed;
                }
            }
        }

        if (solverProcess_.active) {
            const std::optional<unsigned long> result =
                solverProcess_.poll();
            if (result.has_value()) {
                if (*result == 0) {
                    loadResultFrames();
                } else {
                    currentRunRequiresComputedFrame_ = false;
                    std::string solverError = readDiagnosticFile(
                        currentRunDirectory_ / "solver-error.txt");
                    if (solverError.empty()) {
                        solverError = readDiagnosticFile(
                            currentRunDirectory_ / "solver-output.txt");
                    }
                    status_ = solverError.empty()
                                  ? "Fluid Solver failed with exit code " +
                                        std::to_string(*result) + "."
                                  : "Fluid Solver failed: " + solverError;
                }
            } else if (std::chrono::steady_clock::now() >=
                       nextSolverProgressUpdate_) {
                refreshSolverProgress();
                nextSolverProgressUpdate_ =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(500);
            }
        }
        syncControlState();
    }

    void refreshSolverProgress() {
        try {
            const std::vector<std::filesystem::path> paths =
                VtkFrameParser::discoverFrames(currentRunDirectory_);
            status_ =
                "Fluid Solver is running. Saved " +
                std::to_string(paths.size()) +
                " VTK frame(s). Closing the GUI cancels this run.";
        } catch (const std::exception& exception) {
            status_ =
                "Fluid Solver is running. Progress scan failed: " +
                std::string(exception.what());
        }
    }

    void stopSimulationAndLoadFrames() {
        if (!solverProcess_.active) {
            status_ = "No Fluid Solver simulation is active.";
            return;
        }
        std::string error;
        if (!solverProcess_.terminate(&error)) {
            status_ = error;
            return;
        }
        currentRunRequiresComputedFrame_ = false;
        try {
            const std::vector<std::filesystem::path> paths =
                VtkFrameParser::discoverFrames(currentRunDirectory_);
            if (paths.empty()) {
                status_ = "Stopped Fluid Solver; no VTK frames were saved.";
                return;
            }
            loadResultPaths(paths, ResultOrigin::StoppedFluidSolverRun);
        } catch (const std::exception& exception) {
            status_ = std::string("Stopped Fluid Solver. VTK load failed: ") +
                exception.what();
        }
    }

    sf::FloatRect parameterViewport() const {
        const float height = static_cast<float>(layoutSize_.y);
        const float bottom =
            std::max(PARAMETER_TOP + 40.0f, height - PARAMETER_BOTTOM_MARGIN);
        return {
            {0.0f, PARAMETER_TOP},
            {LEFT_PANEL_WIDTH, bottom - PARAMETER_TOP}
        };
    }

    bool parameterRowOnScreen(std::size_t index) const {
        if (index >= sliders_.size()) {
            return false;
        }
        const sf::FloatRect viewport = parameterViewport();
        const float visualTop = sliders_[index].track.position.y - 26.0f;
        const float visualBottom = sliders_[index].track.position.y + 12.0f;
        return visualTop >= viewport.position.y &&
               visualBottom <= viewport.position.y + viewport.size.y;
    }

    MaskParameters sectionParameters() const {
        MaskParameters parameters;
        parameters.Lx = sliders_[DomainX].value;
        parameters.Ly = sliders_[DomainY].value;
        parameters.nx =
            static_cast<int>(std::lround(sliders_[CellsX].value));
        parameters.ny =
            static_cast<int>(std::lround(sliders_[CellsY].value));
        parameters.sliceAngleX = sliders_[SliceX].value;
        parameters.sliceAngleZ = sliders_[SliceZ].value;
        parameters.sliceRotation = sliders_[SliceRotation].value;
        parameters.invertSection = invertSection_;
        return parameters;
    }

    FluidSolverRunConfig fluidSolverRunConfig() const {
        FluidSolverRunConfig config;
        const MaskParameters parameters = sectionParameters();
        config.Lx = parameters.Lx;
        config.Ly = parameters.Ly;
        config.nx = parameters.nx;
        config.ny = parameters.ny;
        config.U0 = sliders_[WindSpeed].value;
        config.nu = sliders_[Viscosity].value;
        config.ro = sliders_[Density].value;
        config.CFL = sliders_[Cfl].value;
        config.totalTime = sliders_[TotalTime].value;
        config.dtUpdateInterval =
            static_cast<int>(std::lround(sliders_[DtUpdateInterval].value));
        config.dtSafety = sliders_[DtSafety].value;
        config.omega = sliders_[CoarseSorOmega].value;
        config.smootherOmega = sliders_[SmootherOmega].value;
        config.mgIterations =
            static_cast<int>(std::lround(sliders_[MgIterations].value));
        config.mgTolerance = sliders_[MgTolerance].value;
        config.mgMinCoarseSize =
            static_cast<int>(std::lround(sliders_[MgMinCoarseSize].value));
        config.saveInterval =
            static_cast<int>(std::lround(sliders_[SaveInterval].value));
        config.useCuda = sliders_[UseCuda].value >= 0.5;
        config.geometryFile = geometry_.sourcePath();
        config.sliceAngleX = parameters.sliceAngleX;
        config.sliceAngleZ = parameters.sliceAngleZ;
        config.sliceRotation = parameters.sliceRotation;
        config.invertSection = parameters.invertSection;
        return config;
    }

    std::optional<std::size_t> sliderForValidationError(
        const std::string& error) const {
        const std::array<std::pair<const char*, ParameterIndex>, 18> mappings{{
            {"Lx", DomainX}, {"Ly", DomainY}, {"nx", CellsX}, {"ny", CellsY},
            {"U0", WindSpeed}, {"nu", Viscosity}, {"ro", Density},
            {"CFL", Cfl}, {"totalTime", TotalTime},
            {"dtUpdateInterval", DtUpdateInterval}, {"dtSafety", DtSafety},
            {"omega", CoarseSorOmega}, {"smootherOmega", SmootherOmega},
            {"mgIterations", MgIterations}, {"mgTolerance", MgTolerance},
            {"mgMinCoarseSize", MgMinCoarseSize},
            {"saveInterval", SaveInterval}, {"useCuda", UseCuda}
        }};
        for (const auto& mapping : mappings) {
            if (error.rfind(mapping.first, 0) == 0) {
                return static_cast<std::size_t>(mapping.second);
            }
        }
        if (error.find("nx * ny") != std::string::npos) {
            return CellsX;
        }
        return std::nullopt;
    }

    void applyRestartControlDefaults(const VtkFrame& frame) {
        const auto assignDouble =
            [&frame, this](const char* key, ParameterIndex index) {
                const auto found = frame.restart.config.find(key);
                if (found == frame.restart.config.end()) {
                    return;
                }
                try {
                    std::size_t consumed = 0;
                    const double value = std::stod(found->second, &consumed);
                    if (consumed == found->second.size() &&
                        std::isfinite(value)) {
                        sliders_[index].value = value;
                    }
                } catch (const std::exception&) {
                }
            };
        assignDouble("U0", WindSpeed);
        assignDouble("nu", Viscosity);
        assignDouble("ro", Density);
        assignDouble("CFL", Cfl);
        assignDouble("totalTime", TotalTime);
        assignDouble("dtUpdateInterval", DtUpdateInterval);
        assignDouble("dtSafety", DtSafety);
        assignDouble("omega", CoarseSorOmega);
        assignDouble("smootherOmega", SmootherOmega);
        assignDouble("mgIterations", MgIterations);
        assignDouble("mgTolerance", MgTolerance);
        assignDouble("mgMinCoarseSize", MgMinCoarseSize);
        assignDouble("saveInterval", SaveInterval);
        const auto cuda = frame.restart.config.find("useCuda");
        if (cuda != frame.restart.config.end()) {
            sliders_[UseCuda].value =
                cuda->second == "1" || cuda->second == "true" ? 1.0 : 0.0;
        }
    }

    std::string frameProgressLabel(const VtkFrame& frame) const {
        const int step = frame.restart.restartStep.value_or(frame.frameNumber);
        std::string label = "Solver step " + std::to_string(step);
        if (!frame.restart.currentTime) {
            return label;
        }
        label += " | t " +
            formatValue(*frame.restart.currentTime, false, "s");
        if (frame.restart.totalTime && *frame.restart.totalTime > 0.0) {
            const double percent = std::clamp(
                100.0 * *frame.restart.currentTime /
                    *frame.restart.totalTime,
                0.0,
                100.0);
            label += " / " +
                formatValue(*frame.restart.totalTime, false, "s") +
                " (" + formatValue(percent, false, "%") + ")";
        }
        return label;
    }

    void refreshSolverInfo() {
        solverInfo_ = inspectSolverExecutable(fluidSolverExecutable_);
        if (!solverInfo_.cudaCapable) {
            sliders_[UseCuda].value = 0.0;
            sliders_[UseCuda].label = "Request CUDA (unavailable)";
        } else {
            sliders_[UseCuda].label = "Request CUDA";
        }
    }

    void applyCacheBudget() {
        const double megabytes = std::clamp(
            sliders_[CacheMegabytes].value, 32.0, 16384.0);
        const std::size_t bytes = static_cast<std::size_t>(
            std::llround(megabytes * 1024.0 * 1024.0));
        decodedFrameCache_.setByteBudget(bytes);
    }

    void loadPreferences() {
        std::ifstream input(preferencesFile_, std::ios::binary);
        if (!input.is_open()) {
            return;
        }
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, separator);
            const std::string value = line.substr(separator + 1);
            if (key == "outputRoot" && !value.empty()) {
                outputRoot_ = std::filesystem::u8path(value);
            } else if (key == "uiCacheMB") {
                try {
                    const double parsed = std::stod(value);
                    if (std::isfinite(parsed) && parsed > 0.0) {
                        sliders_[CacheMegabytes].value = parsed;
                    }
                } catch (const std::exception&) {
                }
            }
        }
    }

    void savePreferences() const {
        std::ofstream output(
            preferencesFile_, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "outputRoot=" << outputRoot_.u8string() << '\n';
        output << "uiCacheMB=" <<
            editableNumber(sliders_[CacheMegabytes].value, true) << '\n';
    }

    bool writeConfigurationFile(
        const std::filesystem::path& path,
        std::string& error) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = "Cannot open configuration file for writing: " +
                path.string();
            return false;
        }
        output << "format=CFDMaskUI-1\n";
        output << "model=" << geometry_.sourcePath().u8string() << '\n';
        output << "outputRoot=" << outputRoot_.u8string() << '\n';
        output << "solver=" << fluidSolverExecutable_.u8string() << '\n';
        output << "invertSection=" << (invertSection_ ? 1 : 0) << '\n';
        output << std::setprecision(
            std::numeric_limits<double>::max_digits10);
        for (std::size_t index = 0; index < sliders_.size(); ++index) {
            output << parameterKey(index) << '=' << sliders_[index].value << '\n';
        }
        output.flush();
        if (!output) {
            error = "Failed while writing configuration file: " +
                path.string();
            return false;
        }
        return true;
    }

    bool readConfigurationFile(
        const std::filesystem::path& path,
        std::string& error) {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            error = "Cannot open configuration file: " + path.string();
            return false;
        }
        std::unordered_map<std::string, std::string> values;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t separator = line.find('=');
            if (separator != std::string::npos) {
                values[line.substr(0, separator)] = line.substr(separator + 1);
            }
        }
        if (values["format"] != "CFDMaskUI-1") {
            error = "Unsupported UI configuration format.";
            return false;
        }
        for (std::size_t index = 0; index < sliders_.size(); ++index) {
            const auto found = values.find(parameterKey(index));
            if (found == values.end()) {
                continue;
            }
            try {
                const double parsed = std::stod(found->second);
                if (std::isfinite(parsed)) {
                    sliders_[index].value = parsed;
                }
            } catch (const std::exception&) {
                error = "Invalid value for " +
                    std::string(parameterKey(index)) + ".";
                return false;
            }
        }
        invertSection_ = values["invertSection"] == "1";
        if (const auto found = values.find("outputRoot");
            found != values.end() && !found->second.empty()) {
            outputRoot_ = std::filesystem::u8path(found->second);
        }
        if (const auto found = values.find("solver");
            found != values.end() && !found->second.empty()) {
            const std::filesystem::path solver =
                std::filesystem::u8path(found->second);
            const SolverExecutableInfo info = inspectSolverExecutable(solver);
            if (info.valid && info.recognized) {
                fluidSolverExecutable_ = solver;
                solverInfo_ = info;
            }
        }
        if (const auto found = values.find("model");
            found != values.end() && !found->second.empty()) {
            const std::filesystem::path model =
                std::filesystem::u8path(found->second);
            std::error_code fileError;
            if (std::filesystem::is_regular_file(model, fileError) &&
                !fileError) {
                loadGeometry(model);
            }
        }
        refreshSolverInfo();
        applyCacheBudget();
        savePreferences();
        invalidSlider_.reset();
        return true;
    }

    void saveConfiguration() {
        std::string error;
        const std::filesystem::path path =
            chooseUiConfigFile(window_->getNativeHandle(), true, error);
        if (!error.empty()) {
            status_ = error;
            return;
        }
        if (path.empty()) {
            return;
        }
        if (!writeConfigurationFile(path, error)) {
            status_ = error;
            return;
        }
        status_ = "Saved UI configuration: " + path.string();
    }

    void loadConfiguration() {
        std::string error;
        const std::filesystem::path path =
            chooseUiConfigFile(window_->getNativeHandle(), false, error);
        if (!error.empty()) {
            status_ = error;
            return;
        }
        if (path.empty()) {
            return;
        }
        if (!readConfigurationFile(path, error)) {
            status_ = error;
            return;
        }
        status_ = "Loaded UI configuration: " + path.string();
    }

    void resetDefaults() {
        for (Slider& slider : sliders_) {
            slider.value = slider.defaultValue;
        }
        if (!solverInfo_.cudaCapable) {
            sliders_[UseCuda].value = 0.0;
        }
        invertSection_ = false;
        invalidSlider_.reset();
        applyCacheBudget();
        status_ = "Solver and UI parameters reset to defaults.";
    }

    void importGeometry() {
        const std::filesystem::path selected =
            chooseGeometryFile(window_->getNativeHandle());
        if (selected.empty()) {
            return;
        }
        loadGeometry(selected);
    }

    void selectOutputFolder() {
        std::string error;
        const std::filesystem::path selected =
            chooseOutputFolder(window_->getNativeHandle(), error);
        if (!error.empty()) {
            status_ = error;
            return;
        }
        if (selected.empty()) {
            return;
        }
        std::error_code absoluteError;
        const std::filesystem::path absolute =
            std::filesystem::absolute(selected, absoluteError);
        outputRoot_ = absoluteError ? selected : absolute;
        savePreferences();
        status_ = "Simulation output root: " + outputRoot_.string() +
            ". Each run uses a new child directory.";
    }

    void openVtkFiles() {
        if (solverProcess_.active) {
            status_ =
                "Cannot import VTK frames while the Fluid Solver is running.";
            return;
        }
        std::string error;
        const std::vector<std::filesystem::path> selected =
            chooseVtkFiles(window_->getNativeHandle(), error);
        if (!error.empty()) {
            status_ = error;
            return;
        }
        if (!selected.empty()) {
            loadExternalResultInputs(selected);
        }
    }

    void selectFluidSolverExecutable() {
        if (solverProcess_.active) {
            status_ =
                "Cannot change the Fluid Solver while it is running.";
            return;
        }
        std::string error;
        const std::filesystem::path selected =
            chooseSolverExecutable(window_->getNativeHandle(), error);
        if (!error.empty()) {
            status_ = error;
            return;
        }
        if (selected.empty()) {
            return;
        }

        std::error_code absoluteError;
        const std::filesystem::path absolute =
            std::filesystem::absolute(selected, absoluteError);
        const std::filesystem::path executable =
            absoluteError ? selected : absolute;
        const SolverExecutableInfo info = inspectSolverExecutable(executable);
        if (!info.valid) {
            status_ = "Cannot select Fluid Solver: " + info.detail;
            return;
        }
        if (!info.recognized) {
            status_ =
                "Selected EXE is not recognized as Fluid Solver.exe; "
                "selection was rejected.";
            return;
        }

        fluidSolverExecutable_ = executable;
        solverInfo_ = info;
        if (!solverInfo_.cudaCapable) {
            sliders_[UseCuda].value = 0.0;
        }
        if (!writeFluidSolverSelection(
                solverSelectionFile_,
                fluidSolverExecutable_,
                error)) {
            status_ =
                "Selected Fluid Solver for this session, but could not "
                "persist it: " + error;
            return;
        }
        status_ =
            "Selected Fluid Solver " + solverInfo_.version + " (" +
            solverInfo_.build + "): " + fluidSolverExecutable_.string();
    }

    std::optional<ExplorerTarget> currentExplorerTarget() const {
        const std::filesystem::path selectedFrame =
            !frames_.empty() && selectedFrame_ < frames_.size()
                ? frames_[selectedFrame_].sourcePath
                : std::filesystem::path{};
        return chooseExplorerTarget(
            selectedFrame,
            currentRunDirectory_);
    }

    void revealVtkLocation() {
        const std::optional<ExplorerTarget> target =
            currentExplorerTarget();
        if (!target) {
            status_ = "No VTK file or run directory is available.";
            return;
        }

        std::string error;
        if (!openExplorerTarget(
                window_->getNativeHandle(),
                *target,
                error)) {
            status_ = "Cannot open VTK location: " + error;
            return;
        }

        status_ = target->selectFile
            ? "Selected " + target->location.filename().string() +
                  " in File Explorer."
            : "Opened the VTK run directory in File Explorer.";
    }

    void loadGeometry(const std::filesystem::path& selected) {
        std::string error;
        if (!geometry_.load(selected, error)) {
            status_ = "Import failed: " + error;
            return;
        }
        status_ =
            "Loaded model (" +
            std::to_string(geometry_.triangles().size()) +
            " triangles).";
        sectionSegments_.clear();
        sectionSegmentsSliceX_ = std::numeric_limits<double>::quiet_NaN();
        sectionSegmentsSliceZ_ = std::numeric_limits<double>::quiet_NaN();
        setupZoom_ = 1.0f;
    }

    void generateAndRun() {
        std::string error;
        refreshSolverInfo();
        if (!solverInfo_.valid || !solverInfo_.recognized) {
            status_ =
                "Cannot run: selected executable is not a recognized Fluid "
                "Solver build.";
            return;
        }
        const FluidSolverRunConfig requestedConfig = fluidSolverRunConfig();
        if (!validateFluidSolverRunConfig(requestedConfig, error)) {
            invalidSlider_ = sliderForValidationError(error);
            status_ = "Cannot run Fluid Solver: " + error;
            return;
        }
        invalidSlider_.reset();
        applyCacheBudget();

        MaskResult mask = geometry_.generateMask(sectionParameters());
        if (!mask.success) {
            if (mask.error.find("10000000-cell") != std::string::npos) {
                invalidSlider_ = CellsX;
            }
            status_ = "Mask generation failed: " + mask.error;
            return;
        }

        bool reducedToLargestContour = false;
        const std::size_t originalContourCount = mask.contours.size();
        if (mask.contours.size() > 1) {
            if (!confirmUseLargestContour(
                    window_->getNativeHandle(), mask.contours.size())) {
                status_ =
                    "Run cancelled: current Fluid Solver accepts one closed "
                    "contour, while the UI detected " +
                    std::to_string(mask.contours.size()) + ".";
                return;
            }
            const auto largest = std::max_element(
                mask.contours.begin(),
                mask.contours.end(),
                [](const std::vector<Vec2>& first,
                   const std::vector<Vec2>& second) {
                    return contourArea(first) < contourArea(second);
                });
            std::vector<std::vector<Vec2>> selectedContours{
                *largest
            };
            mask = geometry_.rasterizeContours(
                sectionParameters(), std::move(selectedContours));
            if (!mask.success) {
                status_ =
                    "Largest-contour preview failed: " + mask.error;
                return;
            }
            reducedToLargestContour = true;
        }

        const std::filesystem::path runDirectory =
            createRunDirectory(outputRoot_, error);
        if (runDirectory.empty()) {
            status_ = error;
            return;
        }
        std::vector<std::string> requestedArguments;
        if (!buildFluidSolverArguments(
                requestedConfig,
                runDirectory,
                requestedArguments,
                error)) {
            status_ = "Cannot build requested solver arguments: " + error;
            return;
        }
        const std::filesystem::path requestedArgumentFile =
            runDirectory / "requested-arguments.txt";
        if (!writeFluidSolverArguments(
                requestedArgumentFile,
                requestedArguments,
                error)) {
            status_ = "Cannot write requested run record: " + error;
            return;
        }

        const std::filesystem::path uiRequestFile =
            runDirectory / "ui-request.txt";
        std::ofstream uiRequest(
            uiRequestFile,
            std::ios::out | std::ios::trunc);
        if (!uiRequest.is_open()) {
            status_ =
                "Cannot write UI run record: " + uiRequestFile.string();
            return;
        }
        uiRequest << std::setprecision(
            std::numeric_limits<double>::max_digits10);
        uiRequest << "model=" << geometry_.sourcePath().string() << '\n';
        uiRequest << "uiVersion=" << CFD_MASK_UI_VERSION << '\n';
        uiRequest << "solverPath=" << fluidSolverExecutable_.u8string() << '\n';
        uiRequest << "solverVersion=" << solverInfo_.version << '\n';
        uiRequest << "solverBuild=" << solverInfo_.build << '\n';
        uiRequest << "outputRoot=" << outputRoot_.u8string() << '\n';
        uiRequest << "sourceContourCount=" << originalContourCount << '\n';
        uiRequest << "solverContourCount=" << mask.contours.size() << '\n';
        uiRequest << "reducedToLargestContour="
                  << (reducedToLargestContour ? 1 : 0) << '\n';
        uiRequest << "sliceRotationDegrees="
                  << requestedConfig.sliceRotation << '\n';
        uiRequest.flush();
        if (!uiRequest) {
            status_ =
                "Failed while writing UI run record: " +
                uiRequestFile.string();
            return;
        }

        const std::filesystem::path adapterFile =
            runDirectory / "section-adapter.obj";
        if (!writeSectionAdapterOBJ(adapterFile, mask.contours, error)) {
            status_ = "Cannot write section adapter: " + error;
            return;
        }

        FluidSolverRunConfig solverConfig = requestedConfig;
        solverConfig.geometryFile = adapterFile;
        solverConfig.sliceAngleX = 0.0;
        solverConfig.sliceAngleZ = 0.0;
        solverConfig.sliceRotation = 0.0;
        solverConfig.invertSection = false;
        std::vector<std::string> solverArguments;
        if (!buildFluidSolverArguments(
                solverConfig,
                runDirectory,
                solverArguments,
                error)) {
            status_ = "Cannot build Fluid Solver arguments: " + error;
            return;
        }
        const std::filesystem::path solverArgumentFile =
            runDirectory / "solver-arguments.txt";
        if (!writeFluidSolverArguments(
                solverArgumentFile,
                solverArguments,
                error)) {
            status_ = "Cannot write Fluid Solver arguments: " + error;
            return;
        }
        if (!solverProcess_.start(
                fluidSolverExecutable_,
                solverArguments,
                runDirectory,
                error)) {
            status_ = error;
            return;
        }

        currentRunDirectory_ = runDirectory;
        currentRunIsContinuation_ = false;
        continuationSourceStep_ = -1;
        currentRunRequiresComputedFrame_ =
            requestedConfig.totalTime > 0.0;
        nextSolverProgressUpdate_ =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        previewSolid_.assign(mask.cells.begin(), mask.cells.end());
        previewNx_ = static_cast<std::size_t>(requestedConfig.nx);
        previewNy_ = static_cast<std::size_t>(requestedConfig.ny);
        frames_.clear();
        playingFrames_ = false;
        showRunDetails_ = false;
        runDetailsText_.clear();
        activeFrame_.reset();
        decodedFrameCache_.clear();
        desiredFrame_.reset();
        loadingFrame_.reset();
        resultTextureCacheValid_ = false;
        resultsWarning_.clear();
        selectedFrame_ = 0;
        status_ =
            (reducedToLargestContour
                 ? "Reduced " + std::to_string(originalContourCount) +
                       " disconnected contours to the largest one. "
                 : std::string{}) +
            "Preview contains " + std::to_string(mask.contours.size()) +
            " solver contour(s) and " +
            std::to_string(mask.solidCellCount) +
            " solid cells. Fluid Solver started." +
            (requestedConfig.totalTime > 0.0
                 ? " Projection validation stops the run before saving any "
                   "timestep that exceeds the divergence limit."
                 : "");
    }

    void continueSelectedFrame() {
        status_ =
            "VTK continuation is reserved for a later restart-capable Fluid "
            "Solver. Current CFD-Solver-2D-main has no restart/restartFile/"
            "addTime CLI contract; imported VTK frames remain readable.";
    }

    void loadExternalResultInputs(
        const std::vector<std::filesystem::path>& inputs) {
        if (solverProcess_.active || resultCatalogFuture_.valid() ||
            selectedFrameFuture_.valid()) {
            status_ =
                "Cannot import VTK frames while another load or solver runs.";
            return;
        }
        try {
            std::vector<std::filesystem::path> paths;
            for (const std::filesystem::path& input : inputs) {
                std::error_code pathError;
                const bool isDirectory =
                    std::filesystem::is_directory(input, pathError);
                if (pathError) {
                    throw VtkParseError(
                        "Cannot inspect input path " + input.string() +
                        ": " + pathError.message());
                }
                if (!isDirectory) {
                    paths.push_back(input);
                    continue;
                }
                const std::vector<std::filesystem::path> discovered =
                    VtkFrameParser::discoverFrames(input);
                if (discovered.empty()) {
                    throw VtkParseError(
                        "Directory contains no solver solution VTK frames: " +
                        input.string());
                }
                paths.insert(
                    paths.end(),
                    discovered.begin(),
                    discovered.end());
            }
            loadResultPaths(paths, ResultOrigin::ImportedFiles);
        } catch (const std::exception& exception) {
            status_ = std::string("VTK load failed: ") + exception.what();
        }
    }

    void loadResultFrames() {
        try {
            const std::vector<std::filesystem::path> paths =
                VtkFrameParser::discoverFrames(currentRunDirectory_);
            if (paths.empty()) {
                std::string diagnostic = readDiagnosticFile(
                    currentRunDirectory_ / "solver-error.txt");
                if (diagnostic.empty()) {
                    diagnostic = readDiagnosticFile(
                        currentRunDirectory_ / "solver-output.txt");
                }
                status_ =
                    "Solver completed but produced no VTK frames." +
                    (diagnostic.empty() ? "" : " " + diagnostic);
                return;
            }
            loadResultPaths(
                paths,
                currentRunIsContinuation_
                    ? ResultOrigin::ContinuedFluidSolverRun
                    : ResultOrigin::FluidSolverRun);
        } catch (const std::exception& exception) {
            const std::string diagnostic = readDiagnosticFile(
                currentRunDirectory_ / "solver-error.txt");
            status_ =
                std::string("VTK load failed: ") + exception.what() +
                (diagnostic.empty() ? "" : " " + diagnostic);
        }
    }

    void loadResultPaths(
        const std::vector<std::filesystem::path>& paths,
        ResultOrigin origin) {
        const bool stopped =
            origin == ResultOrigin::StoppedFluidSolverRun;
        if (resultCatalogFuture_.valid() || selectedFrameFuture_.valid()) {
            status_ = "A VTK load is already running.";
            return;
        }
        prefetchQueue_.clear();
        adaptiveWindowIndices_.clear();
        adaptiveWindowDivisor_ = 1;
        pendingResultOrigin_ = origin;
        resultCatalogStarted_ = std::chrono::steady_clock::now();
        resultCatalogFuture_ = std::async(
            std::launch::async,
            [paths, stopped, origin]() {
                VtkSeriesCatalog catalog =
                    VtkFrameParser::indexSeries(paths, stopped);
                if ((origin == ResultOrigin::FluidSolverRun ||
                     origin == ResultOrigin::ContinuedFluidSolverRun) &&
                    catalog.frames.size() > 1) {
                    const VtkFrame initial = VtkFrameParser::parse(
                        catalog.frames.front().sourcePath);
                    VtkFrameParser::validateCompatibility(
                        initial,
                        catalog.activeFrame);
                }
                return catalog;
            });
        status_ = "Indexing " + std::to_string(paths.size()) +
            " VTK frame(s) and decoding only the active frame...";
    }

    void pollResultCatalog() {
        if (!resultCatalogFuture_.valid() ||
            resultCatalogFuture_.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
            return;
        }
        const ResultOrigin origin = *pendingResultOrigin_;
        pendingResultOrigin_.reset();
        try {
            commitResultCatalog(resultCatalogFuture_.get(), origin);
        } catch (const std::exception& exception) {
            if (origin != ResultOrigin::ImportedFiles) {
                currentRunRequiresComputedFrame_ = false;
            }
            status_ = std::string("VTK load failed: ") + exception.what();
        }
    }

    void commitResultCatalog(
        VtkSeriesCatalog catalog,
        ResultOrigin origin) {
        const bool stopped =
            origin == ResultOrigin::StoppedFluidSolverRun;
        const bool continued =
            origin == ResultOrigin::ContinuedFluidSolverRun;
        if (catalog.frames.empty()) {
            throw VtkParseError("No complete compatible VTK frames exist");
        }
        const std::size_t rejectedCount = catalog.rejected.size();
        const std::size_t warningCount = catalog.warningCount;
        const bool fromFluidSolver =
            origin != ResultOrigin::ImportedFiles;
        if (origin == ResultOrigin::FluidSolverRun &&
            currentRunRequiresComputedFrame_ &&
                (catalog.frames.front().frameNumber != 0 ||
                 catalog.frames.size() < 2 ||
                 catalog.frames.back().frameNumber <= 0)) {
            throw VtkParseError(
                "Fluid Solver completed without the required initial "
                "and positive-step VTK frames");
        }
        if (continued && currentRunRequiresComputedFrame_ &&
            catalog.frames.back().frameNumber <= continuationSourceStep_) {
            throw VtkParseError(
                "Continuation completed without a solver step newer than " +
                std::to_string(continuationSourceStep_));
        }
        const bool hasPreview =
            fromFluidSolver && !previewSolid_.empty();
        const bool previewMatches =
            hasPreview &&
            catalog.activeFrame.nx == previewNx_ &&
            catalog.activeFrame.ny == previewNy_ &&
            catalog.activeFrame.solid == previewSolid_;
        const std::string solverDiagnostic =
            fromFluidSolver
                ? readDiagnosticFile(
                      currentRunDirectory_ / "solver-error.txt")
                : std::string{};
        frames_ = std::move(catalog.frames);
        playingFrames_ = false;
        showRunDetails_ = false;
        runDetailsText_.clear();
        activeFrame_ = std::make_shared<VtkFrame>(
            std::move(catalog.activeFrame));
        applyRestartControlDefaults(*activeFrame_);
        currentRunRequiresComputedFrame_ = false;
        currentRunIsContinuation_ = false;
        selectedFrame_ = frames_.size() - 1;
        desiredFrame_.reset();
        loadingFrame_.reset();
        loadingFrameIsPrefetch_ = false;
        decodedFrameCache_.clear();
        decodedFrameCache_.insert(selectedFrame_, activeFrame_);
        planAdaptivePrefetch(selectedFrame_);
        startNextPrefetch();
        pressureRange_ = catalog.pressureRange;
        velocityRange_ = catalog.velocityMagnitudeRange;
        resultZoom_ = 1.0f;
        resultPan_ = {};
        resultTextureCacheValid_ = false;
        resultsWarning_.clear();
        if (stopped) {
            if (!resultsWarning_.empty()) {
                resultsWarning_ += "  ";
            }
            resultsWarning_ += "STOPPED EARLY: complete frames only.";
        }
        if (hasPreview && !previewMatches) {
            if (!resultsWarning_.empty()) {
                resultsWarning_ += "  ";
            }
            resultsWarning_ +=
                "MASK MISMATCH: Fluid Solver VTK differs from GUI preview.";
        }
        if (!solverDiagnostic.empty()) {
            if (!resultsWarning_.empty()) {
                resultsWarning_ += "  ";
            }
            resultsWarning_ += "SOLVER REPORTED STDERR.";
        }
        mode_ = DisplayMode::Results;
        const auto indexMilliseconds = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - resultCatalogStarted_).count();
        status_ = hasPreview && !previewMatches
            ? "Adapter verification failed: Fluid Solver VTK mask differs "
              "from the GUI preview."
            : "Indexed " + std::to_string(frames_.size()) +
                  " VTK frame(s) in " +
                  std::to_string(indexMilliseconds) + " ms; loaded solver step " +
                  std::to_string(activeFrame_->frameNumber) +
                  ". Series steps " +
                  std::to_string(frames_.front().frameNumber) + " to " +
                  std::to_string(frames_.back().frameNumber) + "." +
                  (hasPreview
                       ? " Fluid Solver mask matches the GUI preview."
                       : "");
        if (stopped) {
            status_ = "Stopped Fluid Solver. " + status_;
        } else if (continued) {
            status_ = "Continuation completed. " + status_;
        }
        if (rejectedCount != 0) {
            status_ += " Skipped " + std::to_string(rejectedCount) +
                " incomplete or incompatible frame file(s).";
        }
        status_ +=
            (warningCount == 0
                 ? ""
                 : " Parser warnings: " +
                       std::to_string(warningCount) + ".");
        if (!solverDiagnostic.empty()) {
            status_ += " Solver stderr: " + solverDiagnostic;
        }
    }

    std::size_t adaptiveResidentFrameLimit() const {
        if (!activeFrame_) {
            return 1;
        }
        const std::size_t frameBytes = activeFrame_->decodedByteSize();
        if (frameBytes == 0) {
            return MAX_ADAPTIVE_RESIDENT_FRAMES;
        }
        return std::max<std::size_t>(
            1,
            std::min(
                MAX_ADAPTIVE_RESIDENT_FRAMES,
                decodedFrameCache_.byteBudget() / frameBytes));
    }

    void planAdaptivePrefetch(std::size_t centerIndex) {
        prefetchQueue_.clear();
        if (frames_.empty() || !activeFrame_) {
            adaptiveWindowIndices_.clear();
            adaptiveWindowDivisor_ = 1;
            return;
        }

        AdaptiveFrameWindow window = planAdaptiveFrameWindow(
            frames_.size(),
            centerIndex,
            adaptiveResidentFrameLimit());
        adaptiveWindowDivisor_ = window.divisor;
        adaptiveWindowIndices_ = std::move(window.nearestIndices);
        decodedFrameCache_.retainOnly(adaptiveWindowIndices_);
        for (const std::size_t index : adaptiveWindowIndices_) {
            if (index == centerIndex || loadingFrame_ == index ||
                decodedFrameCache_.contains(index)) {
                continue;
            }
            prefetchQueue_.push_back(index);
        }
    }

    std::string decodedCacheStatus() const {
        return std::to_string(
            decodedFrameCache_.usedBytes() / (1024u * 1024u)) +
            " MiB in " + std::to_string(decodedFrameCache_.entryCount()) +
            "/" + std::to_string(adaptiveWindowIndices_.size()) +
            " nearby frame(s), target 1/" +
            std::to_string(adaptiveWindowDivisor_) + " of series";
    }

    void commitSelectedFrame(
        std::size_t index,
        std::shared_ptr<const VtkFrame> frame,
        const std::string& source) {
        VtkFrameParser::validateCompatibility(*activeFrame_, *frame);
        activeFrame_ = std::move(frame);
        applyRestartControlDefaults(*activeFrame_);
        selectedFrame_ = index;
        if (desiredFrame_ == index) {
            desiredFrame_.reset();
        }
        frames_[index].warningCount = activeFrame_->warnings.size();
        includeDataRange(pressureRange_, activeFrame_->pressureRange);
        includeDataRange(
            velocityRange_,
            activeFrame_->velocityMagnitudeRange);
        resultTextureCacheValid_ = false;
        planAdaptivePrefetch(index);
        status_ = source + " solver step " +
            std::to_string(activeFrame_->frameNumber) + "; " +
            decodedCacheStatus() + ".";
    }

    void startFrameLoad(std::size_t index, bool prefetch) {
        loadingFrame_ = index;
        loadingFrameIsPrefetch_ = prefetch;
        selectedFrameStarted_ = std::chrono::steady_clock::now();
        const std::filesystem::path path = frames_[index].sourcePath;
        selectedFrameFuture_ = std::async(
            std::launch::async,
            [path]() {
                return std::make_shared<VtkFrame>(
                    VtkFrameParser::parse(path));
            });
        if (!prefetch) {
            status_ = "Loading solver step " +
                std::to_string(frames_[index].frameNumber) + "...";
        }
    }

    void startNextPrefetch() {
        if (selectedFrameFuture_.valid() || desiredFrame_) {
            return;
        }
        while (!prefetchQueue_.empty()) {
            const std::size_t index = prefetchQueue_.front();
            prefetchQueue_.pop_front();
            if (index == selectedFrame_ || decodedFrameCache_.contains(index)) {
                continue;
            }
            startFrameLoad(index, true);
            return;
        }
    }

    void requestSelectedFrame(std::size_t index) {
        if (index >= frames_.size() || resultCatalogFuture_.valid()) {
            return;
        }
        desiredFrame_ = index;
        if (index == selectedFrame_) {
            desiredFrame_.reset();
            status_ = "Solver step " +
                std::to_string(frames_[index].frameNumber) +
                " is already displayed.";
            return;
        }
        if (const auto cached = decodedFrameCache_.find(index)) {
            commitSelectedFrame(index, cached, "Cache hit for");
            startNextPrefetch();
            return;
        }
        if (selectedFrameFuture_.valid()) {
            status_ = "Queued latest request: solver step " +
                std::to_string(frames_[index].frameNumber) + ".";
            return;
        }
        startFrameLoad(index, false);
    }

    void pollSelectedFrame() {
        if (!selectedFrameFuture_.valid() ||
            selectedFrameFuture_.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
            return;
        }
        const std::size_t index = *loadingFrame_;
        const bool wasPrefetch = loadingFrameIsPrefetch_;
        try {
            std::shared_ptr<const VtkFrame> frame =
                selectedFrameFuture_.get();
            VtkFrameParser::validateCompatibility(*activeFrame_, *frame);
            frames_[index].warningCount = frame->warnings.size();
            decodedFrameCache_.insert(index, frame);
            decodedFrameCache_.retainOnly(adaptiveWindowIndices_);
            if (desiredFrame_ == index) {
                const auto milliseconds = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    selectedFrameStarted_).count();
                commitSelectedFrame(
                    index,
                    std::move(frame),
                    "Loaded in " + std::to_string(milliseconds) + " ms:");
            }
        } catch (const std::exception& exception) {
            if (desiredFrame_ == index) {
                desiredFrame_.reset();
                status_ = std::string("VTK frame load failed: ") +
                    exception.what();
            }
        }
        loadingFrame_.reset();
        loadingFrameIsPrefetch_ = false;
        if (desiredFrame_ && *desiredFrame_ != selectedFrame_) {
            const std::size_t latest = *desiredFrame_;
            if (const auto cached = decodedFrameCache_.find(latest)) {
                commitSelectedFrame(latest, cached, "Cache hit for");
            } else {
                startFrameLoad(latest, false);
                return;
            }
        } else if (desiredFrame_ == selectedFrame_) {
            desiredFrame_.reset();
        }
        if (wasPrefetch || !desiredFrame_) {
            startNextPrefetch();
        }
    }

    sf::FloatRect horizontalSliceTrack() const {
        return {
            {
                setupViewport_.position.x + 56.0f,
                setupViewport_.position.y +
                    setupViewport_.size.y - 25.0f
            },
            {setupViewport_.size.x - 116.0f, 8.0f}
        };
    }

    sf::FloatRect verticalSliceTrack() const {
        return {
            {
                setupViewport_.position.x +
                    setupViewport_.size.x - 25.0f,
                setupViewport_.position.y + 56.0f
            },
            {8.0f, setupViewport_.size.y - 116.0f}
        };
    }

    sf::FloatRect invertBounds() const {
        return {
            {
                setupViewport_.position.x +
                    setupViewport_.size.x - 47.0f,
                setupViewport_.position.y +
                    setupViewport_.size.y - 47.0f
            },
            {24.0f, 24.0f}
        };
    }

    void setHorizontalSlice(float mouseX) {
        const sf::FloatRect track = horizontalSliceTrack();
        const double normalized = std::clamp(
            static_cast<double>(
                (mouseX - track.position.x) / track.size.x),
            0.0,
            1.0);
        sliders_[SliceZ].value =
            std::round(-180.0 + 360.0 * normalized);
    }

    void setVerticalSlice(float mouseY) {
        const sf::FloatRect track = verticalSliceTrack();
        const double normalized = std::clamp(
            static_cast<double>(
                (mouseY - track.position.y) / track.size.y),
            0.0,
            1.0);
        sliders_[SliceX].value =
            std::round(180.0 - 360.0 * normalized);
    }

    sf::FloatRect zoomHitBounds() const {
        return {
            {zoomTrack_.position.x - 8.0f, zoomTrack_.position.y - 12.0f},
            {zoomTrack_.size.x + 16.0f, 29.0f}
        };
    }

    sf::FloatRect frameHitBounds() const {
        return {
            {frameTrack_.position.x - 8.0f, frameTrack_.position.y - 12.0f},
            {frameTrack_.size.x + 16.0f, 29.0f}
        };
    }

    ResultImageTransform resultImageTransform(const VtkFrame& frame) const {
        const double physicalWidth =
            static_cast<double>(frame.nx) * frame.spacingX;
        const double physicalHeight =
            static_cast<double>(frame.ny) * frame.spacingY;
        if (physicalWidth <= 0.0 || physicalHeight <= 0.0) {
            return {};
        }
        const double fit = std::min(
            static_cast<double>(resultViewport_.size.x) / physicalWidth,
            static_cast<double>(resultViewport_.size.y) / physicalHeight);
        const double pixelWidth = frame.spacingX * fit * resultZoom_;
        const double pixelHeight = frame.spacingY * fit * resultZoom_;
        const double imageWidth = static_cast<double>(frame.nx) * pixelWidth;
        const double imageHeight = static_cast<double>(frame.ny) * pixelHeight;
        return {
            static_cast<double>(resultViewport_.position.x) +
                (static_cast<double>(resultViewport_.size.x) - imageWidth) /
                    2.0 +
                static_cast<double>(resultPan_.x),
            static_cast<double>(resultViewport_.position.y) +
                (static_cast<double>(resultViewport_.size.y) - imageHeight) /
                    2.0 +
                static_cast<double>(resultPan_.y),
            pixelWidth,
            pixelHeight
        };
    }

    void setZoomFromSlider(float mouseX) {
        const double normalized = std::clamp(
            static_cast<double>(
                (mouseX - zoomTrack_.position.x) / zoomTrack_.size.x),
            0.0,
            1.0);
        resultZoom_ =
            static_cast<float>(0.5 * std::pow(16.0, normalized));
        resultPan_ = {};
    }

    void setFrameFromSlider(float mouseX) {
        if (frames_.empty() || resultCatalogFuture_.valid()) {
            return;
        }
        const double normalized = std::clamp(
            static_cast<double>(
                (mouseX - frameTrack_.position.x) / frameTrack_.size.x),
            0.0,
            1.0);
        const std::size_t requestedFrame = static_cast<std::size_t>(
            std::llround(
                normalized *
                static_cast<double>(frames_.size() - 1)));
        if (desiredFrame_ != requestedFrame ||
            (!desiredFrame_ && selectedFrame_ != requestedFrame)) {
            requestSelectedFrame(requestedFrame);
        }
    }

    void zoomResultsAt(sf::Vector2f cursor, float factor) {
        const VtkFrame& frame = *activeFrame_;
        const float physicalWidth =
            static_cast<float>(
                static_cast<double>(frame.nx) * frame.spacingX);
        const float physicalHeight =
            static_cast<float>(
                static_cast<double>(frame.ny) * frame.spacingY);
        const float fit = std::min(
            resultViewport_.size.x / physicalWidth,
            resultViewport_.size.y / physicalHeight);
        const sf::Vector2f centre{
            resultViewport_.position.x + resultViewport_.size.x / 2.0f,
            resultViewport_.position.y + resultViewport_.size.y / 2.0f
        };
        const sf::Vector2f oldSize{
            physicalWidth * fit * resultZoom_,
            physicalHeight * fit * resultZoom_
        };
        const sf::Vector2f oldOrigin =
            centre - oldSize / 2.0f + resultPan_;
        const sf::Vector2f dataCoordinate{
            (cursor.x - oldOrigin.x) / (fit * resultZoom_),
            (cursor.y - oldOrigin.y) / (fit * resultZoom_)
        };

        const float newZoom =
            clampFloat(resultZoom_ * factor, 0.5f, 8.0f);
        const sf::Vector2f newSize{
            physicalWidth * fit * newZoom,
            physicalHeight * fit * newZoom
        };
        resultPan_ =
            cursor - centre + newSize / 2.0f -
            sf::Vector2f{
                dataCoordinate.x * fit * newZoom,
                dataCoordinate.y * fit * newZoom
            };
        resultZoom_ = newZoom;
    }

    ProjectedPoint projectPoint(
        const Vec3& point,
        const sf::FloatRect& viewport) const {
        const GeometryBounds& bounds = geometry_.bounds();
        Vec3 relative = subtract(point, bounds.centre);
        const double inverseScale =
            bounds.characteristicLength > 0.0
                ? 1.0 / bounds.characteristicLength
                : 1.0;
        relative = multiply(relative, inverseScale);

        const double yaw = -35.0 * PI / 180.0;
        const double pitch = 25.0 * PI / 180.0;
        const double yawX =
            std::cos(yaw) * relative.x -
            std::sin(yaw) * relative.z;
        const double yawZ =
            std::sin(yaw) * relative.x +
            std::cos(yaw) * relative.z;
        const double pitchY =
            std::cos(pitch) * relative.y -
            std::sin(pitch) * yawZ;
        const double depth =
            std::sin(pitch) * relative.y +
            std::cos(pitch) * yawZ;

        const float scale =
            0.66f * std::min(viewport.size.x, viewport.size.y) *
            setupZoom_;
        return {
            {
                viewport.position.x + viewport.size.x / 2.0f +
                    static_cast<float>(yawX) * scale,
                viewport.position.y + viewport.size.y / 2.0f -
                    static_cast<float>(pitchY) * scale
            },
            depth
        };
    }

    sf::FloatRect parameterScrollbarRail() const {
        const sf::FloatRect viewport = parameterViewport();
        return {
            {LEFT_PANEL_WIDTH - 12.0f, viewport.position.y},
            {4.0f, viewport.size.y}
        };
    }

    sf::FloatRect parameterScrollbarThumb() const {
        const sf::FloatRect rail = parameterScrollbarRail();
        if (maxParameterScroll_ <= 0.0f) {
            return {rail.position, {rail.size.x, rail.size.y}};
        }

        const float contentHeight =
            static_cast<float>(sliders_.size()) * PARAMETER_ROW_HEIGHT +
            static_cast<float>(PARAMETER_GROUPS.size()) *
                PARAMETER_GROUP_HEIGHT;
        const float thumbHeight = std::max(
            32.0f,
            rail.size.y *
                std::min(1.0f, rail.size.y / contentHeight));
        const float travel = std::max(0.0f, rail.size.y - thumbHeight);
        const float fraction =
            parameterScrollOffset_ / maxParameterScroll_;
        return {
            {rail.position.x, rail.position.y + travel * fraction},
            {rail.size.x, thumbHeight}
        };
    }

    void setParameterScrollFromThumb(float thumbTop) {
        const sf::FloatRect rail = parameterScrollbarRail();
        const sf::FloatRect thumb = parameterScrollbarThumb();
        const float travel = std::max(0.0f, rail.size.y - thumb.size.y);
        if (travel <= 0.0f || maxParameterScroll_ <= 0.0f) {
            parameterScrollOffset_ = 0.0f;
            updateLayout(layoutSize_);
            return;
        }

        const float clampedTop = clampFloat(
            thumbTop,
            rail.position.y,
            rail.position.y + travel);
        const float fraction =
            (clampedTop - rail.position.y) / travel;
        parameterScrollOffset_ = fraction * maxParameterScroll_;
        updateLayout(layoutSize_);
    }

    void drawParameterScrollbar() {
        if (maxParameterScroll_ <= 0.0f) {
            return;
        }

        const sf::FloatRect railBounds = parameterScrollbarRail();
        sf::RectangleShape rail(railBounds.size);
        rail.setPosition(railBounds.position);
        rail.setFillColor(CONTROL_RAIL);
        window_->draw(rail);

        const sf::FloatRect thumbBounds = parameterScrollbarThumb();
        sf::RectangleShape thumb(thumbBounds.size);
        thumb.setPosition(thumbBounds.position);
        thumb.setFillColor(ACCENT);
        window_->draw(thumb);
    }

    void drawParameterGroupHeaders() {
        const sf::FloatRect viewport = parameterViewport();
        for (const ParameterGroupInfo& group : PARAMETER_GROUPS) {
            if (group.firstIndex >= sliders_.size()) {
                continue;
            }
            const float y =
                sliders_[group.firstIndex].track.position.y - 47.0f;
            if (y < viewport.position.y - 18.0f ||
                y > viewport.position.y + viewport.size.y) {
                continue;
            }
            window_->draw(makeText(
                font_, group.label, 11, {20.0f, y}, ACCENT));
            sf::RectangleShape divider({278.0f, 1.0f});
            divider.setPosition({20.0f, y + 17.0f});
            divider.setFillColor(BORDER);
            window_->draw(divider);
        }
    }

    std::string compactPath(
        const std::filesystem::path& path,
        std::size_t maximum = 62) const {
        std::string text = path.string();
        if (text.size() <= maximum) {
            return text;
        }
        return "..." + text.substr(text.size() - (maximum - 3));
    }

    std::string setupSummaryText() const {
        const double lx = sliders_[DomainX].value;
        const double ly = sliders_[DomainY].value;
        const long long nx = std::max<long long>(
            1, std::llround(sliders_[CellsX].value));
        const long long ny = std::max<long long>(
            1, std::llround(sliders_[CellsY].value));
        const double dx = lx / static_cast<double>(nx);
        const double dy = ly / static_cast<double>(ny);
        const long double cells =
            static_cast<long double>(nx) * static_cast<long double>(ny);
        const double nu = sliders_[Viscosity].value;
        const double u0 = sliders_[WindSpeed].value;
        const double reynolds = nu > 0.0 ? u0 * lx / nu : 0.0;

        double dtEstimate = 0.0;
        if (dx > 0.0 && dy > 0.0 && nu > 0.0) {
            const double advectiveRate = u0 > 0.0 ? u0 / dx : 0.0;
            const double advective = advectiveRate > 0.0
                ? sliders_[Cfl].value / advectiveRate
                : std::numeric_limits<double>::infinity();
            const double diffusive = 1.0 /
                (2.0 * nu * (1.0 / (dx * dx) + 1.0 / (dy * dy)));
            dtEstimate = sliders_[DtSafety].value *
                std::min(advective, diffusive);
        }
        long long estimatedSteps = 0;
        long long estimatedVtks = 0;
        if (std::isfinite(dtEstimate) && dtEstimate > 0.0) {
            estimatedSteps = static_cast<long long>(std::ceil(
                sliders_[TotalTime].value / dtEstimate));
            const long long interval = std::max<long long>(
                1, std::llround(sliders_[SaveInterval].value));
            estimatedVtks = 1 + estimatedSteps / interval;
        }

        int coarseX = static_cast<int>(std::min<long long>(nx, 1'000'000));
        int coarseY = static_cast<int>(std::min<long long>(ny, 1'000'000));
        const int minimumCoarse = std::max(
            1, static_cast<int>(std::llround(sliders_[MgMinCoarseSize].value)));
        int levels = 1;
        for (; levels < 32; ++levels) {
            bool changed = false;
            if (coarseX % 2 == 0 && coarseX / 2 >= minimumCoarse) {
                coarseX /= 2;
                changed = true;
            }
            if (coarseY % 2 == 0 && coarseY / 2 >= minimumCoarse) {
                coarseY /= 2;
                changed = true;
            }
            if (!changed) {
                break;
            }
        }

        std::ostringstream text;
        text << "Fluid Solver " << solverInfo_.version << " | "
             << solverInfo_.build << '\n'
             << "Grid " << nx << " x " << ny << " = "
             << std::fixed << std::setprecision(0) << cells << " cells"
             << " | dx " << formatValue(dx, false, "m")
             << " | dy " << formatValue(dy, false, "m") << '\n'
             << "Re(Lx) " << formatValue(reynolds, false, "")
             << " | approx MG levels " << levels;
        if (estimatedSteps > 0) {
            text << " | rough steps " << estimatedSteps
                 << " | VTK ~" << estimatedVtks;
        }
        text << '\n'
             << "Output: " << compactPath(outputRoot_) << '\n'
             << "Solver: " << compactPath(fluidSolverExecutable_);
        if (cells > 10'000'000.0L) {
            text << "\nWARNING: UI preview mask limit is 10,000,000 cells.";
        } else if (cells > 2'000'000.0L) {
            text << "\nWARNING: large grid; preview, RAM and output cost increase sharply.";
        }
        return text.str();
    }

    void drawSetupInfoOverlay() {
        const float width = std::min(560.0f, setupViewport_.size.x - 36.0f);
        if (width < 300.0f) {
            return;
        }
        const sf::Vector2f position{
            setupViewport_.position.x + setupViewport_.size.x - width - 18.0f,
            setupViewport_.position.y + 18.0f
        };
        sf::RectangleShape background({width, 128.0f});
        background.setPosition(position);
        background.setFillColor(OVERLAY_BACKGROUND);
        background.setOutlineColor(BORDER);
        background.setOutlineThickness(1.0f);
        window_->draw(background);
        window_->draw(makeText(
            font_, setupSummaryText(), 11,
            position + sf::Vector2f{10.0f, 8.0f}, TEXT));

        for (std::size_t index = 0; index < sliders_.size(); ++index) {
            if (!parameterRowOnScreen(index)) {
                continue;
            }
            const sf::FloatRect row{
                {12.0f, sliders_[index].track.position.y - 29.0f},
                {292.0f, 42.0f}
            };
            if (!row.contains(lastMouse_)) {
                continue;
            }
            const std::string help = parameterHelp(index);
            if (help.empty()) {
                break;
            }
            sf::RectangleShape helpBox({width, 48.0f});
            helpBox.setPosition({position.x, position.y + 136.0f});
            helpBox.setFillColor(OVERLAY_BACKGROUND);
            helpBox.setOutlineColor(ACCENT_DARK);
            helpBox.setOutlineThickness(1.0f);
            window_->draw(helpBox);
            window_->draw(makeText(
                font_, help, 11,
                {position.x + 10.0f, position.y + 150.0f}, MUTED));
            break;
        }
    }

    void drawSetup() {
        drawPanel(
            *window_,
            {{0.0f, 50.0f},
             {LEFT_PANEL_WIDTH, static_cast<float>(layoutSize_.y) - 50.0f}});
        importButton_.draw(*window_, font_);
        outputFolderButton_.draw(*window_, font_);
        drawParameterGroupHeaders();
        for (std::size_t index = 0; index < sliders_.size(); ++index) {
            if (!parameterRowOnScreen(index)) {
                continue;
            }
            const bool editing = editingSlider_ == index;
            sliders_[index].draw(
                *window_,
                font_,
                editing,
                editing ? sliderEditText_ : std::string{},
                invalidSlider_ == index);
        }
        drawParameterScrollbar();
        resetDefaultsButton_.draw(*window_, font_);
        saveConfigButton_.draw(*window_, font_);
        loadConfigButton_.draw(*window_, font_);
        generateButton_.draw(*window_, font_);
        continueButton_.draw(*window_, font_);

        sf::RectangleShape viewBackground(setupViewport_.size);
        viewBackground.setPosition(setupViewport_.position);
        viewBackground.setFillColor(VIEW_BACKGROUND);
        viewBackground.setOutlineColor(BORDER);
        viewBackground.setOutlineThickness(1.0f);
        window_->draw(viewBackground);

        if (geometry_.empty()) {
            window_->draw(makeText(
                font_,
                "Import an STL or OBJ model",
                24,
                {
                    setupViewport_.position.x + 40.0f,
                    setupViewport_.position.y + 50.0f
                },
                MUTED));
        } else {
            drawGeometryPreview();
        }
        drawSetupInfoOverlay();
        drawSliceControls();
    }

    void drawGeometryPreview() {
        std::vector<PreviewTriangle> projected;
        const auto& source = geometry_.triangles();
        const MaskParameters parameters = sectionParameters();
        const SectionFrame frame = geometry_.sectionFrame(parameters);
        if (!frame.valid) {
            return;
        }
        if (parameters.sliceAngleX != sectionSegmentsSliceX_ ||
            parameters.sliceAngleZ != sectionSegmentsSliceZ_) {
            sectionSegments_ = geometry_.sectionSegments(parameters);
            sectionSegmentsSliceX_ = parameters.sliceAngleX;
            sectionSegmentsSliceZ_ = parameters.sliceAngleZ;
        }
        const double rotation = parameters.sliceRotation * PI / 180.0;
        const double cosineRotation = std::cos(rotation);
        const double sineRotation = std::sin(rotation);
        const auto rotateForPreview =
            [&frame, cosineRotation, sineRotation](const Vec3& point) {
                const Vec3 relative = subtract(point, frame.centre);
                const double axial =
                    relative.x * frame.normal.x +
                    relative.y * frame.normal.y +
                    relative.z * frame.normal.z;
                return add(
                    frame.centre,
                    add(
                        add(
                            multiply(relative, cosineRotation),
                            multiply(
                                cross(frame.normal, relative),
                                sineRotation)),
                        multiply(
                            frame.normal,
                            axial * (1.0 - cosineRotation))));
            };
        const std::size_t stride =
            std::max<std::size_t>(1, source.size() / 12000);
        projected.reserve((source.size() + stride - 1) / stride);

        for (std::size_t index = 0; index < source.size(); index += stride) {
            const Triangle3& triangle = source[index];
            const Vec3 firstVertex = rotateForPreview(triangle.v0);
            const Vec3 secondVertex = rotateForPreview(triangle.v1);
            const Vec3 thirdVertex = rotateForPreview(triangle.v2);
            const ProjectedPoint first =
                projectPoint(firstVertex, setupViewport_);
            const ProjectedPoint second =
                projectPoint(secondVertex, setupViewport_);
            const ProjectedPoint third =
                projectPoint(thirdVertex, setupViewport_);

            const Vec3 normal = cross(
                subtract(secondVertex, firstVertex),
                subtract(thirdVertex, firstVertex));
            const double normalLength = length(normal);
            const double shade =
                normalLength > 0.0
                    ? std::clamp(
                          std::abs(
                              (normal.x * 0.3 +
                               normal.y * 0.6 +
                               normal.z * 0.74) /
                              normalLength),
                          0.0,
                          1.0)
                    : 0.0;
            const std::uint8_t brightness =
                static_cast<std::uint8_t>(75 + 105 * shade);
            projected.push_back({
                {first.position, second.position, third.position},
                (first.depth + second.depth + third.depth) / 3.0,
                 {brightness,
                  static_cast<std::uint8_t>(
                      std::min<int>(255, brightness + 2)),
                  static_cast<std::uint8_t>(
                      std::min<int>(255, brightness + 1))}
            });
        }

        std::sort(
            projected.begin(),
            projected.end(),
            [](const PreviewTriangle& first, const PreviewTriangle& second) {
                return first.depth < second.depth;
            });
        for (const PreviewTriangle& triangle : projected) {
            sf::ConvexShape shape(3);
            for (std::size_t corner = 0; corner < 3; ++corner) {
                shape.setPoint(corner, triangle.points[corner]);
            }
            shape.setFillColor(triangle.color);
            shape.setOutlineColor(sf::Color{38, 42, 40, 150});
            shape.setOutlineThickness(0.5f);
            window_->draw(shape);
        }

        const std::array<Vec3, 4> planeCorners{{
            add(
                add(frame.centre, multiply(frame.axisX, frame.extent)),
                multiply(frame.axisY, frame.extent)),
            add(
                add(frame.centre, multiply(frame.axisX, -frame.extent)),
                multiply(frame.axisY, frame.extent)),
            add(
                add(frame.centre, multiply(frame.axisX, -frame.extent)),
                multiply(frame.axisY, -frame.extent)),
            add(
                add(frame.centre, multiply(frame.axisX, frame.extent)),
                multiply(frame.axisY, -frame.extent))
        }};

        sf::ConvexShape plane(4);
        for (std::size_t corner = 0; corner < planeCorners.size(); ++corner) {
            plane.setPoint(
                corner,
                projectPoint(planeCorners[corner], setupViewport_).position);
        }
        plane.setFillColor(SECTION_PLANE);
        plane.setOutlineColor(SECTION_PLANE_OUTLINE);
        plane.setOutlineThickness(2.0f);
        window_->draw(plane);

        for (const SectionSegment& segment : sectionSegments_) {
            const sf::Vector2f first = projectPoint(
                rotateForPreview(segment.first),
                setupViewport_).position;
            const sf::Vector2f second = projectPoint(
                rotateForPreview(segment.second),
                setupViewport_).position;
            drawThickLine(
                *window_,
                first,
                second,
                8.0f,
                CUT_GLOW);
            drawThickLine(
                *window_,
                first,
                second,
                3.5f,
                CUT_COLOR);
        }

        const sf::Vector2f legendPosition =
            setupViewport_.position + sf::Vector2f{18.0f, 18.0f};
        sf::RectangleShape legendBackground({330.0f, 59.0f});
        legendBackground.setPosition(legendPosition);
        legendBackground.setFillColor(sf::Color{8, 10, 9, 225});
        legendBackground.setOutlineColor(BORDER);
        legendBackground.setOutlineThickness(1.0f);
        window_->draw(legendBackground);
        window_->draw(makeText(
            font_,
            "GREEN = section plane",
            14,
            legendPosition + sf::Vector2f{12.0f, 8.0f},
            SECTION_PLANE_OUTLINE));
        window_->draw(makeText(
            font_,
            sectionSegments_.empty()
                ? "ORANGE = no mesh intersection"
                : "ORANGE = mesh-plane intersection",
            14,
            legendPosition + sf::Vector2f{12.0f, 31.0f},
            CUT_COLOR));
    }

    void drawSliceControls() {
        const sf::FloatRect horizontal = horizontalSliceTrack();
        sf::RectangleShape horizontalRail(horizontal.size);
        horizontalRail.setPosition(horizontal.position);
        horizontalRail.setFillColor(CONTROL_RAIL);
        window_->draw(horizontalRail);
        const float horizontalFraction = static_cast<float>(
            (sliders_[SliceZ].value + 180.0) / 360.0);
        sf::CircleShape horizontalHandle(7.0f);
        horizontalHandle.setOrigin({7.0f, 7.0f});
        horizontalHandle.setPosition({
            horizontal.position.x +
                horizontal.size.x * horizontalFraction,
            horizontal.position.y + horizontal.size.y / 2.0f
        });
        horizontalHandle.setFillColor(ACCENT);
        window_->draw(horizontalHandle);

        const sf::FloatRect vertical = verticalSliceTrack();
        sf::RectangleShape verticalRail(vertical.size);
        verticalRail.setPosition(vertical.position);
        verticalRail.setFillColor(CONTROL_RAIL);
        window_->draw(verticalRail);
        const float verticalFraction = static_cast<float>(
            (180.0 - sliders_[SliceX].value) / 360.0);
        sf::CircleShape verticalHandle(7.0f);
        verticalHandle.setOrigin({7.0f, 7.0f});
        verticalHandle.setPosition({
            vertical.position.x + vertical.size.x / 2.0f,
            vertical.position.y +
                vertical.size.y * verticalFraction
        });
        verticalHandle.setFillColor(ACCENT);
        window_->draw(verticalHandle);

        const sf::FloatRect invert = invertBounds();
        sf::RectangleShape box(invert.size);
        box.setPosition(invert.position);
        box.setFillColor(
            invertSection_ ? ACCENT_DARK : BUTTON_DISABLED);
        box.setOutlineColor(ACCENT);
        box.setOutlineThickness(1.0f);
        window_->draw(box);
        if (invertSection_) {
            window_->draw(makeText(
                font_,
                "x",
                16,
                {invert.position.x + 7.0f, invert.position.y + 1.0f}));
        }
        window_->draw(makeText(
            font_,
            "Slice Z",
            12,
            {horizontal.position.x, horizontal.position.y - 19.0f},
            MUTED));
        window_->draw(makeText(
            font_,
            "Slice X",
            12,
            {vertical.position.x - 28.0f, vertical.position.y - 20.0f},
            MUTED));
        window_->draw(makeText(
            font_,
            "Invert",
            11,
            {invert.position.x - 13.0f, invert.position.y - 18.0f},
            MUTED));
    }

    std::string readRunDetailFile(
        const std::filesystem::path& path,
        const std::string& heading,
        std::size_t maximumLines) const {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return heading + ": unavailable\n";
        }
        std::ostringstream output;
        output << heading << ":\n";
        std::string line;
        std::size_t lines = 0;
        while (lines < maximumLines && std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() > 96) {
                line.resize(93);
                line += "...";
            }
            output << "  " << line << '\n';
            ++lines;
        }
        return output.str();
    }

    void refreshRunDetailsText() {
        std::filesystem::path directory = currentRunDirectory_;
        if (directory.empty() && !frames_.empty()) {
            directory = frames_[selectedFrame_].sourcePath.parent_path();
        }
        if (directory.empty()) {
            runDetailsText_ = "No run directory is available.";
            return;
        }
        std::ostringstream output;
        output << "RUN DIRECTORY\n  " << compactPath(directory, 90) << "\n\n";
        output << readRunDetailFile(
            directory / "ui-request.txt", "UI metadata", 8) << '\n';
        output << readRunDetailFile(
            directory / "solver-arguments.txt", "Fluid Solver arguments", 10);
        const std::string solverError = readDiagnosticFile(
            directory / "solver-error.txt");
        const std::string solverOutput = readDiagnosticFile(
            directory / "solver-output.txt");
        if (!solverError.empty()) {
            output << "\nLast stderr:\n  " << solverError.substr(0, 420) << '\n';
        } else if (!solverOutput.empty()) {
            output << "\nLast stdout:\n  " << solverOutput.substr(0, 420) << '\n';
        }
        runDetailsText_ = output.str();
    }

    void drawRunDetailsOverlay() {
        const float width = std::min(720.0f, resultViewport_.size.x - 40.0f);
        const float height = std::min(500.0f, resultViewport_.size.y - 40.0f);
        if (width < 300.0f || height < 180.0f) {
            return;
        }
        const sf::Vector2f position{
            resultViewport_.position.x + 20.0f,
            resultViewport_.position.y + 20.0f
        };
        sf::RectangleShape background({width, height});
        background.setPosition(position);
        background.setFillColor(OVERLAY_BACKGROUND);
        background.setOutlineColor(ACCENT_DARK);
        background.setOutlineThickness(1.0f);
        window_->draw(background);
        window_->draw(makeText(
            font_, runDetailsText_, 11,
            position + sf::Vector2f{12.0f, 10.0f}, TEXT));
    }

    void drawResults() {
        pressureButton_.draw(*window_, font_);
        velocityButton_.draw(*window_, font_);
        vectorButton_.draw(*window_, font_);
        rangeButton_.draw(*window_, font_);
        playbackButton_.draw(*window_, font_);
        runDetailsButton_.draw(*window_, font_);

        sf::RectangleShape background(resultViewport_.size);
        background.setPosition(resultViewport_.position);
        background.setFillColor(VIEW_BACKGROUND);
        background.setOutlineColor(BORDER);
        background.setOutlineThickness(1.0f);
        window_->draw(background);

        if (!frames_.empty()) {
            drawResultCells();
            drawLegend();
            drawResultSliders();
            drawResultWarning();
            drawResultTooltip();
        }
        if (showRunDetails_) {
            drawRunDetailsOverlay();
        }
    }

    DataRange resultDisplayRange() const {
        if (!activeFrame_) {
            return {};
        }
        if (!useSeriesRange_) {
            return resultQuantity_ == ResultQuantity::Pressure
                ? activeFrame_->pressureRange
                : activeFrame_->velocityMagnitudeRange;
        }
        return resultQuantity_ == ResultQuantity::Pressure
            ? pressureRange_
            : velocityRange_;
    }

    void drawResultCells() {
        const VtkFrame& frame = *activeFrame_;
        const DataRange range = resultDisplayRange();
        const ResultImageTransform transform = resultImageTransform(frame);
        const float cellWidth = static_cast<float>(transform.pixelWidth);
        const float cellHeight = static_cast<float>(transform.pixelHeight);
        const sf::Vector2f origin{
            static_cast<float>(transform.screenOriginX),
            static_cast<float>(transform.screenOriginY)
        };

        if (!resultTextureCacheValid_) {
            const unsigned int maximumTextureSize =
                sf::Texture::getMaximumSize();
            if (frame.nx > maximumTextureSize ||
                frame.ny > maximumTextureSize) {
                status_ = "VTK grid exceeds the GPU texture-size limit.";
                return;
            }
            const sf::Vector2u textureSize{
                static_cast<unsigned int>(frame.nx),
                static_cast<unsigned int>(frame.ny)
            };
            if (resultTexture_.getSize() != textureSize &&
                !resultTexture_.resize(textureSize)) {
                status_ = "Cannot allocate the VTK result texture.";
                return;
            }
            std::vector<std::uint8_t> pixels(frame.nx * frame.ny * 4u);
            for (std::size_t j = 0; j < frame.ny; ++j) {
                const std::size_t sourceRow = j * frame.nx;
                const std::size_t pixelRow =
                    (frame.ny - 1u - j) * frame.nx * 4u;
                for (std::size_t i = 0; i < frame.nx; ++i) {
                    const std::size_t dataIndex = sourceRow + i;
                    sf::Color color = SOLID_COLOR;
                    if (frame.solid[dataIndex] == 0) {
                        const bool finite =
                            resultQuantity_ == ResultQuantity::Pressure
                                ? frame.pressureFinite[dataIndex] != 0
                                : frame.velocityFinite[dataIndex] != 0;
                        if (!finite || !range.available) {
                            color = INVALID_COLOR;
                        } else {
                            const double value =
                                resultQuantity_ == ResultQuantity::Pressure
                                    ? frame.pressure[dataIndex]
                                    : frame.velocityMagnitude[dataIndex];
                            color = scalarColor(
                                value,
                                range.minimum,
                                range.maximum);
                        }
                    }

                    const std::size_t pixel = pixelRow + i * 4u;
                    pixels[pixel] = color.r;
                    pixels[pixel + 1u] = color.g;
                    pixels[pixel + 2u] = color.b;
                    pixels[pixel + 3u] = color.a;
                }
            }
            resultTexture_.update(pixels.data());
            resultTexture_.setSmooth(false);
            resultTextureCacheValid_ = true;
        }

        sf::Sprite resultSprite(resultTexture_);
        resultSprite.setPosition(origin);
        resultSprite.setScale({cellWidth, cellHeight});
        const sf::View previousView = window_->getView();
        sf::View clippedView = previousView;
        const sf::Vector2f windowSize{
            static_cast<float>(window_->getSize().x),
            static_cast<float>(window_->getSize().y)
        };
        clippedView.setScissor({
            {
                resultViewport_.position.x / windowSize.x,
                resultViewport_.position.y / windowSize.y
            },
            {
                resultViewport_.size.x / windowSize.x,
                resultViewport_.size.y / windowSize.y
            }
        });
        window_->setView(clippedView);
        window_->draw(resultSprite);
        window_->setView(previousView);
        if (showVelocityVectors_) {
            drawVelocityVectors(frame, origin, cellWidth, cellHeight);
        }


        window_->draw(makeText(
            font_,
            frameProgressLabel(frame),
            14,
            {
                resultViewport_.position.x + 10.0f,
                resultViewport_.position.y +
                    (resultsWarning_.empty() ? 8.0f : 42.0f)
            },
            TEXT));
    }

    void drawResultTooltip() {
        if (!activeFrame_ || panningResults_ || draggingFrame_ ||
            draggingZoom_ || !resultViewport_.contains(lastMouse_)) {
            return;
        }
        const VtkFrame& frame = *activeFrame_;
        const std::optional<VtkPixelSample> sample = sampleVtkPixel(
            frame,
            resultImageTransform(frame),
            lastMouse_.x,
            lastMouse_.y);
        if (!sample) {
            return;
        }

        const auto scalarText = [](float value, bool finite,
                                   const std::string& unit) {
            return finite ? formatValue(value, false, unit) : "non-finite";
        };
        std::ostringstream value;
        value << "Pixel X: " << sample->x << "   Y: " << sample->y << '\n'
              << "Position x: "
              << formatValue(sample->physicalX, false, "m")
              << "   y: "
              << formatValue(sample->physicalY, false, "m") << '\n';
        if (sample->solid) {
            value << "u: n/a   v: n/a\n"
                  << "Speed: n/a (solid)\nPressure: n/a (solid)";
        } else {
            value << "u: "
                  << scalarText(
                         sample->velocityX,
                         sample->speedFinite,
                         "m/s")
                  << "   v: "
                  << scalarText(
                         sample->velocityY,
                         sample->speedFinite,
                         "m/s")
                  << "\nSpeed: "
                  << scalarText(sample->speed, sample->speedFinite, "m/s")
                  << "\nPressure: "
                  << scalarText(
                         sample->pressure,
                         sample->pressureFinite,
                         "Pa");
        }

        constexpr float tooltipWidth = 270.0f;
        constexpr float tooltipHeight = 110.0f;
        sf::Vector2f position = lastMouse_ + sf::Vector2f{14.0f, 14.0f};
        position.x = std::clamp(
            position.x,
            4.0f,
            static_cast<float>(layoutSize_.x) - tooltipWidth - 4.0f);
        position.y = std::clamp(
            position.y,
            52.0f,
            static_cast<float>(layoutSize_.y) - tooltipHeight - 28.0f);

        sf::RectangleShape background({tooltipWidth, tooltipHeight});
        background.setPosition(position);
        background.setFillColor(OVERLAY_BACKGROUND);
        background.setOutlineColor(ACCENT);
        background.setOutlineThickness(1.0f);
        window_->draw(background);
        window_->draw(makeText(
            font_,
            value.str(),
            12,
            position + sf::Vector2f{9.0f, 7.0f},
            TEXT));
    }

    void drawVelocityVectors(
        const VtkFrame& frame,
        sf::Vector2f origin,
        float cellWidth,
        float cellHeight) {
        const double referenceMagnitude =
            (useSeriesRange_ ? velocityRange_ : frame.velocityMagnitudeRange)
                    .available
                ? std::max(
                      0.0,
                      (useSeriesRange_
                           ? velocityRange_
                           : frame.velocityMagnitudeRange).maximum)
                : 0.0;
        const std::vector<VelocityArrow> arrows =
            velocityOverlayPlanner_.plan(
                frame,
                cellWidth,
                cellHeight,
                referenceMagnitude);
        sf::VertexArray vertices{sf::PrimitiveType::Triangles};
        vertices.resize(arrows.size() * 9u);
        std::size_t vertexIndex = 0;
        const sf::Color color{245, 248, 252, 230};

        for (const VelocityArrow& arrow : arrows) {
            const sf::Vector2f direction{
                static_cast<float>(arrow.unitX),
                static_cast<float>(-arrow.unitY)
            };
            const sf::Vector2f perpendicular{
                -direction.y,
                direction.x
            };
            const sf::Vector2f center{
                origin.x +
                    (static_cast<float>(arrow.i) + 0.5f) * cellWidth,
                origin.y +
                    (static_cast<float>(frame.ny - 1 - arrow.j) + 0.5f) *
                        cellHeight
            };
            const float length =
                10.0f +
                18.0f * std::sqrt(
                    static_cast<float>(arrow.relativeMagnitude));
            const sf::Vector2f tail =
                center - direction * (length * 0.5f);
            const sf::Vector2f tip =
                center + direction * (length * 0.5f);
            const float headLength = std::min(7.0f, length * 0.36f);
            const float headHalfWidth = headLength * 0.62f;
            const sf::Vector2f shaftTip =
                tip - direction * (headLength * 0.58f);
            const sf::Vector2f shaftOffset = perpendicular * 1.25f;
            const sf::Vector2f headLeft =
                tip - direction * headLength +
                perpendicular * headHalfWidth;
            const sf::Vector2f headRight =
                tip - direction * headLength -
                perpendicular * headHalfWidth;
            const std::array<sf::Vector2f, 9> points{{
                tail + shaftOffset,
                tail - shaftOffset,
                shaftTip - shaftOffset,
                tail + shaftOffset,
                shaftTip - shaftOffset,
                shaftTip + shaftOffset,
                tip,
                headLeft,
                headRight
            }};
            const bool inside = std::all_of(
                points.begin(),
                points.end(),
                [this](sf::Vector2f point) {
                    return resultViewport_.contains(point);
                });
            if (!inside) {
                continue;
            }
            for (const sf::Vector2f point : points) {
                vertices[vertexIndex++] = sf::Vertex{point, color};
            }
        }
        vertices.resize(vertexIndex);
        window_->draw(vertices);
    }

    void drawLegend() {
        const DataRange range = resultDisplayRange();
        const int strips = 120;
        for (int strip = 0; strip < strips; ++strip) {
            const double normalized =
                static_cast<double>(strip) /
                static_cast<double>(strips - 1);
            sf::RectangleShape rectangle({
                legendBounds_.size.x,
                legendBounds_.size.y / static_cast<float>(strips) + 1.0f
            });
            rectangle.setPosition({
                legendBounds_.position.x,
                legendBounds_.position.y +
                    legendBounds_.size.y *
                        static_cast<float>(1.0 - normalized)
            });
            rectangle.setFillColor(scalarColor(normalized, 0.0, 1.0));
            window_->draw(rectangle);
        }

        const std::string unit =
            resultQuantity_ == ResultQuantity::Pressure ? "Pa" : "m/s";
        window_->draw(makeText(
            font_,
            resultQuantity_ == ResultQuantity::Pressure
                ? "Pressure"
                : "Velocity",
            14,
            {legendBounds_.position.x - 18.0f,
             legendBounds_.position.y - 28.0f}));
        if (range.available) {
            window_->draw(makeText(
                font_,
                formatValue(range.maximum, false, unit),
                11,
                {legendBounds_.position.x - 18.0f,
                 legendBounds_.position.y - 14.0f},
                MUTED));
            window_->draw(makeText(
                font_,
                formatValue(range.minimum, false, unit),
                11,
                {
                    legendBounds_.position.x - 18.0f,
                    legendBounds_.position.y +
                        legendBounds_.size.y + 5.0f
                },
                MUTED));
        }
    }

    void drawResultSliders() {
        drawSimpleTrack(
            zoomTrack_,
            static_cast<float>(
                std::log(resultZoom_ / 0.5f) / std::log(16.0)),
            "Zoom " + formatValue(resultZoom_, false, "x"));
        const std::size_t displayedFrame =
            desiredFrame_.value_or(selectedFrame_);
        const float frameFraction =
            frames_.size() <= 1
                ? 0.0f
                : static_cast<float>(displayedFrame) /
                      static_cast<float>(frames_.size() - 1);
        drawSimpleTrack(
            frameTrack_,
            frameFraction,
            "Saved frame " +
                std::to_string(displayedFrame + 1) + "/" +
                std::to_string(frames_.size()) + " (solver step " +
                std::to_string(frames_[displayedFrame].frameNumber) + ")");
    }

    void drawResultWarning() {
        if (resultsWarning_.empty()) {
            return;
        }
        sf::RectangleShape banner({
            resultViewport_.size.x,
            34.0f
        });
        banner.setPosition(resultViewport_.position);
        banner.setFillColor(WARNING_BACKGROUND);
        banner.setOutlineColor(WARNING_OUTLINE);
        banner.setOutlineThickness(1.0f);
        window_->draw(banner);

        std::string display = resultsWarning_;
        if (display.size() > 150) {
            display.resize(147);
            display += "...";
        }
        window_->draw(makeText(
            font_,
            display,
            12,
            {
                resultViewport_.position.x + 8.0f,
                resultViewport_.position.y + 8.0f
            },
            WARNING_TEXT));
    }

    void drawSimpleTrack(
        const sf::FloatRect& track,
        float fraction,
        const std::string& label) {
        fraction = clampFloat(fraction, 0.0f, 1.0f);
        window_->draw(makeText(
            font_,
            label,
            12,
            {track.position.x, track.position.y - 22.0f},
            MUTED));
        sf::RectangleShape rail(track.size);
        rail.setPosition(track.position);
        rail.setFillColor(CONTROL_RAIL);
        window_->draw(rail);
        sf::CircleShape handle(7.0f);
        handle.setOrigin({7.0f, 7.0f});
        handle.setPosition({
            track.position.x + track.size.x * fraction,
            track.position.y + track.size.y / 2.0f
        });
        handle.setFillColor(ACCENT);
        window_->draw(handle);
    }

    void drawTopTabs() {
        setupTab_.draw(*window_, font_);
        resultsTab_.draw(*window_, font_);
        openVtkButton_.draw(*window_, font_);
        stopSimulationButton_.draw(*window_, font_);
        revealVtkButton_.draw(*window_, font_);
        solverExeButton_.draw(*window_, font_);
    }

    void drawLoadingIndicator() {
        std::string label;
        if (solverProcess_.active) {
            label = "SIMULATION RUNNING";
        } else if (resultCatalogFuture_.valid()) {
            label = "INDEXING VTK";
        } else if (selectedFrameFuture_.valid()) {
            label = loadingFrameIsPrefetch_
                        ? "PREFETCHING VTK"
                        : "LOADING VTK";
        } else {
            return;
        }

        constexpr float width = 204.0f;
        constexpr float height = 38.0f;
        const sf::Vector2f position{
            std::max(8.0f, static_cast<float>(layoutSize_.x) - width - 12.0f),
            std::max(58.0f, static_cast<float>(layoutSize_.y) - height - 34.0f)
        };
        sf::RectangleShape background({width, height});
        background.setPosition(position);
        background.setFillColor(OVERLAY_BACKGROUND);
        background.setOutlineColor(BORDER);
        background.setOutlineThickness(1.0f);
        window_->draw(background);

        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const std::size_t activeDot =
            static_cast<std::size_t>((elapsed / 100) % 8);
        const sf::Vector2f center = position + sf::Vector2f{22.0f, 19.0f};
        for (std::size_t dot = 0; dot < 8; ++dot) {
            const double angle =
                static_cast<double>(dot) * PI / 4.0 - PI / 2.0;
            sf::CircleShape circle(dot == activeDot ? 2.8f : 2.1f);
            const float radius = circle.getRadius();
            circle.setOrigin({radius, radius});
            circle.setPosition(center + sf::Vector2f{
                static_cast<float>(std::cos(angle) * 9.0),
                static_cast<float>(std::sin(angle) * 9.0)
            });
            circle.setFillColor(
                dot == activeDot ? ACCENT : sf::Color{68, 90, 72, 150});
            window_->draw(circle);
        }
        window_->draw(makeText(
            font_,
            label,
            12,
            position + sf::Vector2f{42.0f, 10.0f},
            TEXT));
    }

    void drawStatus() {
        if (status_.empty()) {
            return;
        }
        const float y =
            static_cast<float>(layoutSize_.y) - 24.0f;
        sf::RectangleShape background({
            static_cast<float>(layoutSize_.x),
            24.0f
        });
        background.setPosition({0.0f, y});
        background.setFillColor(sf::Color{5, 7, 6, 235});
        window_->draw(background);

        std::string display = status_;
        if (display.size() > 180) {
            display.resize(177);
            display += "...";
        }
        window_->draw(makeText(
            font_,
            display,
            12,
            {10.0f, y + 3.0f},
            MUTED));
    }

    std::filesystem::path executablePath_;
    std::filesystem::path initialModelPath_;
    std::filesystem::path solverSelectionFile_;
    std::filesystem::path preferencesFile_;
    std::filesystem::path fluidSolverExecutable_;
    std::filesystem::path outputRoot_;
    SolverExecutableInfo solverInfo_;
    sf::RenderWindow* window_ = nullptr;
    sf::Font font_;
    GeometryProcessor geometry_;
    std::array<Slider, ParameterCount> sliders_;
    std::optional<std::size_t> editingSlider_;
    std::string sliderEditText_;
    std::optional<std::size_t> lastValueClickSlider_;
    std::optional<std::size_t> invalidSlider_;
    std::chrono::steady_clock::time_point lastValueClickTime_{};
    bool invertSection_ = false;

    DisplayMode mode_ = DisplayMode::Setup;
    ResultQuantity resultQuantity_ = ResultQuantity::Pressure;
    Button setupTab_{"Setup"};
    Button resultsTab_{"Results"};
    Button openVtkButton_{"Open VTK frame(s)"};
    Button stopSimulationButton_{"Stop simulation"};
    Button revealVtkButton_{"Show VTK in Explorer"};
    Button solverExeButton_{"Select solver EXE"};
    Button importButton_{"Import STL / OBJ"};
    Button outputFolderButton_{"Output folder"};
    Button resetDefaultsButton_{"Reset"};
    Button saveConfigButton_{"Save cfg"};
    Button loadConfigButton_{"Load cfg"};
    Button generateButton_{"Run new"};
    Button continueButton_{"Continue VTK"};
    Button pressureButton_{"Pressure"};
    Button velocityButton_{"Velocity"};
    Button vectorButton_{"Vectors: Off"};
    Button rangeButton_{"Range: Series"};
    Button playbackButton_{"Play"};
    Button runDetailsButton_{"Run details"};

    sf::FloatRect setupViewport_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    sf::FloatRect resultViewport_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    sf::FloatRect legendBounds_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    sf::FloatRect zoomTrack_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    sf::FloatRect frameTrack_{{0.0f, 0.0f}, {1.0f, 1.0f}};
    sf::Vector2u layoutSize_{0u, 0u};
    float parameterScrollOffset_ = 0.0f;
    float maxParameterScroll_ = 0.0f;
    bool draggingParameterScrollbar_ = false;
    float parameterScrollbarGrabOffset_ = 0.0f;
    std::optional<std::size_t> activeSlider_;
    bool draggingHorizontalSlice_ = false;
    bool draggingVerticalSlice_ = false;
    bool rotatingObject_ = false;
    bool rotatingRoll_ = false;
    bool draggingZoom_ = false;
    bool draggingFrame_ = false;
    bool panningResults_ = false;
    sf::Vector2f lastMouse_{0.0f, 0.0f};

    float setupZoom_ = 1.0f;
    float resultZoom_ = 1.0f;
    sf::Vector2f resultPan_{0.0f, 0.0f};
    std::vector<SectionSegment> sectionSegments_;
    double sectionSegmentsSliceX_ =
        std::numeric_limits<double>::quiet_NaN();
    double sectionSegmentsSliceZ_ =
        std::numeric_limits<double>::quiet_NaN();

    ChildProcess solverProcess_;
    std::filesystem::path currentRunDirectory_;
    std::chrono::steady_clock::time_point nextSolverProgressUpdate_{};
    bool currentRunRequiresComputedFrame_ = false;
    bool currentRunIsContinuation_ = false;
    int continuationSourceStep_ = -1;
    std::vector<std::uint8_t> previewSolid_;
    std::size_t previewNx_ = 0;
    std::size_t previewNy_ = 0;
    std::vector<VtkFrameDescriptor> frames_;
    std::shared_ptr<const VtkFrame> activeFrame_;
    std::size_t selectedFrame_ = 0;
    std::optional<std::size_t> desiredFrame_;
    std::optional<std::size_t> loadingFrame_;
    std::deque<std::size_t> prefetchQueue_;
    std::vector<std::size_t> adaptiveWindowIndices_;
    std::size_t adaptiveWindowDivisor_ = 1;
    bool loadingFrameIsPrefetch_ = false;
    std::future<VtkSeriesCatalog> resultCatalogFuture_;
    std::optional<ResultOrigin> pendingResultOrigin_;
    std::future<std::shared_ptr<VtkFrame>> selectedFrameFuture_;
    std::chrono::steady_clock::time_point resultCatalogStarted_{};
    std::chrono::steady_clock::time_point selectedFrameStarted_{};
    DecodedFrameCache decodedFrameCache_{DECODED_FRAME_CACHE_BYTES};
    DataRange pressureRange_;
    DataRange velocityRange_;
    VelocityOverlayPlanner velocityOverlayPlanner_;
    bool showVelocityVectors_ = false;
    bool useSeriesRange_ = true;
    bool playingFrames_ = false;
    float playbackAccumulator_ = 0.0f;
    bool showRunDetails_ = false;
    std::string runDetailsText_;
    sf::Texture resultTexture_;
    bool resultTextureCacheValid_ = false;
    std::string resultsWarning_;
    std::string status_ =
        "Import a model or drop solver solution VTK frames. Double-click "
        "a parameter value to type it. Input is disabled while unfocused.";
};

Application::Application(
    std::filesystem::path executablePath,
    std::filesystem::path initialModelPath)
    : implementation_(
          std::make_unique<Implementation>(
              std::move(executablePath),
              std::move(initialModelPath))) {
}

Application::~Application() = default;

int Application::run() {
    return implementation_->run();
}

} // namespace maskui
