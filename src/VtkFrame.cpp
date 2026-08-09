#include "VtkFrame.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

namespace maskui {
namespace {

class TokenCursor {
public:
    explicit TokenCursor(std::istream& input)
        : input_(input) {}

    bool empty() {
        fill();
        return !token_.has_value();
    }

    const std::string& peek() {
        fill();
        if (empty()) {
            throw VtkParseError("Unexpected end of VTK file");
        }
        return *token_;
    }

    std::string take() {
        const std::string token = peek();
        token_.reset();
        return token;
    }

    void expect(const std::string& expected) {
        const std::string actual = take();
        if (actual != expected) {
            throw VtkParseError(
                "Expected token '" + expected + "', found '" + actual + "'");
        }
    }

    void beginBinaryPayload(const std::string& context) {
        if (token_.has_value()) {
            throw VtkParseError(
                "Internal token state precedes binary " + context);
        }
        char character = '\0';
        while (input_.get(character)) {
            if (character == '\n') {
                return;
            }
            if (character == '\r') {
                if (input_.peek() == '\n') {
                    input_.get();
                }
                return;
            }
            if (character != ' ' && character != '\t') {
                throw VtkParseError(
                    "Binary " + context +
                    " must start after its declaration line");
            }
        }
        throw VtkParseError(
            "Unexpected end of VTK file before binary " + context);
    }

    float takeBigEndianFloat(const std::string& context) {
        const std::uint32_t bits = takeBigEndianWord(context);
        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    int takeBigEndianInt(const std::string& context) {
        const std::uint32_t bits = takeBigEndianWord(context);
        std::int32_t value = 0;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<int>(value);
    }

    void skipBinaryValues(
        std::size_t count,
        const std::string& type,
        const std::string& context) {
        if (type != "float" && type != "int") {
            throw VtkParseError(
                "Unsupported binary VTK value type for " +
                context + ": " + type);
        }
        for (std::size_t index = 0; index < count; ++index) {
            static_cast<void>(takeBigEndianWord(context));
        }
    }

    void endBinaryPayload(const std::string& context) {
        const int character = input_.peek();
        if (character == '\r') {
            input_.get();
            if (input_.peek() == '\n') {
                input_.get();
            }
            return;
        }
        if (character == '\n') {
            input_.get();
            return;
        }
        if (character == std::char_traits<char>::eof()) {
            return;
        }
        throw VtkParseError(
            "Binary " + context +
            " must end before a line boundary or end of file");
    }

private:
    std::uint32_t takeBigEndianWord(const std::string& context) {
        std::array<unsigned char, 4> bytes{};
        input_.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (input_.gcount() !=
            static_cast<std::streamsize>(bytes.size())) {
            throw VtkParseError(
                "Truncated binary VTK payload while reading " + context);
        }
        return
            (static_cast<std::uint32_t>(bytes[0]) << 24u) |
            (static_cast<std::uint32_t>(bytes[1]) << 16u) |
            (static_cast<std::uint32_t>(bytes[2]) << 8u) |
            static_cast<std::uint32_t>(bytes[3]);
    }

    void fill() {
        if (token_.has_value()) {
            return;
        }
        std::string next;
        if (input_ >> next) {
            token_ = std::move(next);
            return;
        }
        if (!input_.eof()) {
            throw VtkParseError("Failed while reading VTK tokens");
        }
    }

    std::istream& input_;
    std::optional<std::string> token_;
};

std::string trimCarriageReturn(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

int parseInteger(const std::string& token, const std::string& context) {
    int value = 0;
    const char* begin = token.data();
    const char* end = begin + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        throw VtkParseError("Invalid integer for " + context + ": " + token);
    }
    return value;
}

double parseNumber(const std::string& token, const std::string& context) {
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size()) {
        throw VtkParseError(
            "Invalid floating-point value for " + context + ": " + token);
    }
    return value;
}

std::size_t checkedGridCount(
    int nx,
    int ny,
    const std::string& context) {
    if (nx <= 0 || ny <= 0) {
        throw VtkParseError(context + " dimensions must be positive");
    }
    const std::size_t x = static_cast<std::size_t>(nx);
    const std::size_t y = static_cast<std::size_t>(ny);
    if (x > std::numeric_limits<std::size_t>::max() / y) {
        throw VtkParseError(context + " dimensions overflow the sample count");
    }
    return x * y;
}

void includeValue(DataRange& range, double value) {
    if (!std::isfinite(value)) {
        return;
    }
    if (!range.available) {
        range.available = true;
        range.minimum = value;
        range.maximum = value;
        return;
    }
    range.minimum = std::min(range.minimum, value);
    range.maximum = std::max(range.maximum, value);
}

bool allRequiredArraysPresent(
    bool hasPressure,
    bool hasSolid,
    bool hasVelocity) {
    return hasPressure && hasSolid && hasVelocity;
}

void skipScalarArray(
    TokenCursor& cursor,
    std::size_t sampleCount,
    int componentCount,
    const std::string& type,
    bool binary,
    const std::string& name) {
    if (componentCount <= 0) {
        throw VtkParseError("SCALARS component count must be positive");
    }
    const std::size_t components = static_cast<std::size_t>(componentCount);
    if (sampleCount > std::numeric_limits<std::size_t>::max() / components) {
        throw VtkParseError("SCALARS array size overflow");
    }
    const std::size_t valueCount = sampleCount * components;
    if (binary) {
        cursor.beginBinaryPayload("SCALARS " + name);
        cursor.skipBinaryValues(
            valueCount,
            type,
            "SCALARS " + name);
        cursor.endBinaryPayload("SCALARS " + name);
        return;
    }
    for (std::size_t index = 0; index < valueCount; ++index) {
        parseNumber(cursor.take(), "SCALARS " + name);
    }
}

void skipVectorArray(
    TokenCursor& cursor,
    std::size_t sampleCount,
    const std::string& type,
    bool binary,
    const std::string& name) {
    if (binary) {
        cursor.beginBinaryPayload("VECTORS " + name);
        cursor.skipBinaryValues(
            3 * sampleCount,
            type,
            "VECTORS " + name);
        cursor.endBinaryPayload("VECTORS " + name);
        return;
    }
    for (std::size_t index = 0; index < 3 * sampleCount; ++index) {
        parseNumber(cursor.take(), "VECTORS " + name);
    }
}

std::optional<int> titleFrameNumber(const std::string& title) {
    static const std::regex pattern(R"(.*\bstep ([0-9]+)\s*$)");
    std::smatch match;
    if (!std::regex_match(title, match, pattern)) {
        return std::nullopt;
    }
    try {
        const long long value = std::stoll(match[1].str());
        if (value > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

std::size_t VtkFrame::cellIndex(std::size_t i, std::size_t j) const {
    if (i >= nx || j >= ny) {
        throw std::out_of_range("VTK cell index is outside the logical grid");
    }
    return j * nx + i;
}

std::size_t VtkFrame::decodedByteSize() const {
    std::size_t bytes = sourcePath.native().capacity() *
        sizeof(std::filesystem::path::value_type);
    bytes += title.capacity();
    bytes += pressure.capacity() * sizeof(float);
    bytes += solid.capacity() * sizeof(std::uint8_t);
    bytes += velocity.capacity() * sizeof(Velocity);
    bytes += velocityMagnitude.capacity() * sizeof(float);
    bytes += pressureFinite.capacity() * sizeof(std::uint8_t);
    bytes += velocityFinite.capacity() * sizeof(std::uint8_t);
    for (const std::string& warning : warnings) {
        bytes += warning.capacity();
    }
    return bytes;
}

std::optional<VtkPixelSample> sampleVtkPixel(
    const VtkFrame& frame,
    const ResultImageTransform& transform,
    double screenX,
    double screenY) {
    if (frame.nx == 0 || frame.ny == 0 ||
        transform.pixelWidth <= 0.0 || transform.pixelHeight <= 0.0) {
        return std::nullopt;
    }

    const double localX = screenX - transform.screenOriginX;
    const double localY = screenY - transform.screenOriginY;
    const double imageWidth =
        static_cast<double>(frame.nx) * transform.pixelWidth;
    const double imageHeight =
        static_cast<double>(frame.ny) * transform.pixelHeight;
    if (localX < 0.0 || localY < 0.0 ||
        localX >= imageWidth || localY >= imageHeight) {
        return std::nullopt;
    }

    const std::size_t x = static_cast<std::size_t>(
        std::floor(localX / transform.pixelWidth));
    const std::size_t displayY = static_cast<std::size_t>(
        std::floor(localY / transform.pixelHeight));
    const std::size_t y = frame.ny - 1u - displayY;
    const std::size_t index = frame.cellIndex(x, y);
    const double sampleOffset =
        frame.association == VtkDataAssociation::Cell ? 0.5 : 0.0;

    VtkPixelSample sample;
    sample.x = x;
    sample.y = y;
    sample.physicalX = frame.originX +
        (static_cast<double>(x) + sampleOffset) * frame.spacingX;
    sample.physicalY = frame.originY +
        (static_cast<double>(y) + sampleOffset) * frame.spacingY;
    sample.pressure = frame.pressure[index];
    sample.speed = frame.velocityMagnitude[index];
    sample.solid = frame.solid[index] != 0;
    sample.pressureFinite = frame.pressureFinite[index] != 0;
    sample.speedFinite = frame.velocityFinite[index] != 0;
    return sample;
}

AdaptiveFrameWindow planAdaptiveFrameWindow(
    std::size_t frameCount,
    std::size_t centerIndex,
    std::size_t maximumResidentFrames) {
    AdaptiveFrameWindow window;
    if (frameCount == 0) {
        return window;
    }

    centerIndex = std::min(centerIndex, frameCount - 1u);
    maximumResidentFrames = std::max<std::size_t>(1, maximumResidentFrames);
    if (frameCount == 1) {
        window.nearestIndices.push_back(0);
        return window;
    }

    window.divisor = 2;
    const auto dividedCount = [frameCount](std::size_t divisor) {
        return frameCount / divisor +
            static_cast<std::size_t>(frameCount % divisor != 0);
    };
    while (dividedCount(window.divisor) > maximumResidentFrames &&
           window.divisor < frameCount) {
        if (window.divisor >
            std::numeric_limits<std::size_t>::max() / 2u) {
            break;
        }
        window.divisor *= 2u;
    }

    const std::size_t targetCount = std::max<std::size_t>(
        1,
        std::min(frameCount, dividedCount(window.divisor)));
    const std::size_t leftRadius = (targetCount - 1u) / 2u;
    std::size_t begin = centerIndex > leftRadius
        ? centerIndex - leftRadius
        : 0u;
    if (begin > frameCount - targetCount) {
        begin = frameCount - targetCount;
    }
    const std::size_t end = begin + targetCount;

    window.nearestIndices.reserve(targetCount);
    window.nearestIndices.push_back(centerIndex);
    for (std::size_t distance = 1;
         window.nearestIndices.size() < targetCount;
         ++distance) {
        if (centerIndex <= std::numeric_limits<std::size_t>::max() - distance) {
            const std::size_t right = centerIndex + distance;
            if (right < end) {
                window.nearestIndices.push_back(right);
            }
        }
        if (window.nearestIndices.size() == targetCount) {
            break;
        }
        if (centerIndex >= distance) {
            const std::size_t left = centerIndex - distance;
            if (left >= begin) {
                window.nearestIndices.push_back(left);
            }
        }
    }
    return window;
}

DecodedFrameCache::DecodedFrameCache(std::size_t byteBudget)
    : byteBudget_(byteBudget) {
}

std::shared_ptr<const VtkFrame> DecodedFrameCache::find(
    std::size_t frameIndex) {
    const auto found = entries_.find(frameIndex);
    if (found == entries_.end()) {
        return {};
    }
    recency_.splice(recency_.begin(), recency_, found->second.recency);
    found->second.recency = recency_.begin();
    return found->second.frame;
}

bool DecodedFrameCache::contains(std::size_t frameIndex) const {
    return entries_.find(frameIndex) != entries_.end();
}

void DecodedFrameCache::insert(
    std::size_t frameIndex,
    std::shared_ptr<const VtkFrame> frame) {
    const auto existing = entries_.find(frameIndex);
    if (existing != entries_.end()) {
        usedBytes_ -= existing->second.bytes;
        recency_.erase(existing->second.recency);
        entries_.erase(existing);
    }
    if (!frame) {
        return;
    }

    const std::size_t bytes = frame->decodedByteSize();
    if (bytes > byteBudget_) {
        return;
    }
    while (!recency_.empty() && usedBytes_ > byteBudget_ - bytes) {
        const std::size_t leastRecent = recency_.back();
        const auto evicted = entries_.find(leastRecent);
        usedBytes_ -= evicted->second.bytes;
        entries_.erase(evicted);
        recency_.pop_back();
    }

    recency_.push_front(frameIndex);
    entries_.emplace(
        frameIndex,
        Entry{std::move(frame), bytes, recency_.begin()});
    usedBytes_ += bytes;
}

void DecodedFrameCache::retainOnly(
    const std::vector<std::size_t>& frameIndices) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (std::find(
                frameIndices.begin(),
                frameIndices.end(),
                entry->first) != frameIndices.end()) {
            ++entry;
            continue;
        }
        usedBytes_ -= entry->second.bytes;
        recency_.erase(entry->second.recency);
        entry = entries_.erase(entry);
    }
}

void DecodedFrameCache::clear() {
    entries_.clear();
    recency_.clear();
    usedBytes_ = 0;
}

std::size_t DecodedFrameCache::entryCount() const {
    return entries_.size();
}

std::size_t DecodedFrameCache::usedBytes() const {
    return usedBytes_;
}

std::size_t DecodedFrameCache::byteBudget() const {
    return byteBudget_;
}

VtkFrame VtkFrameParser::parse(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw VtkParseError("Cannot open VTK file: " + path.string());
    }

    std::string version;
    std::string title;
    std::string encoding;
    if (!std::getline(input, version) ||
        !std::getline(input, title) ||
        !std::getline(input, encoding)) {
        throw VtkParseError("VTK file is missing its three-line header");
    }
    version = trimCarriageReturn(version);
    title = trimCarriageReturn(title);
    encoding = trimCarriageReturn(encoding);
    if (version.rfind("# vtk DataFile Version ", 0) != 0) {
        throw VtkParseError("Unsupported VTK version header");
    }
    const bool binary = encoding == "BINARY";
    if (!binary && encoding != "ASCII") {
        throw VtkParseError(
            "Only legacy ASCII or BINARY VTK files are supported");
    }

    TokenCursor cursor(input);
    cursor.expect("DATASET");
    cursor.expect("STRUCTURED_POINTS");
    cursor.expect("DIMENSIONS");
    const int pointNx = parseInteger(cursor.take(), "DIMENSIONS nx");
    const int pointNy = parseInteger(cursor.take(), "DIMENSIONS ny");
    const int nz = parseInteger(cursor.take(), "DIMENSIONS nz");
    if (nz != 1) {
        throw VtkParseError("Only two-dimensional VTK frames with nz = 1 are supported");
    }
    const std::size_t pointCount =
        checkedGridCount(pointNx, pointNy, "POINT");

    cursor.expect("ORIGIN");
    const double originX = parseNumber(cursor.take(), "ORIGIN x");
    const double originY = parseNumber(cursor.take(), "ORIGIN y");
    const double originZ = parseNumber(cursor.take(), "ORIGIN z");
    if (!std::isfinite(originX) ||
        !std::isfinite(originY) ||
        !std::isfinite(originZ)) {
        throw VtkParseError("ORIGIN values must be finite");
    }

    cursor.expect("SPACING");
    const double spacingX = parseNumber(cursor.take(), "SPACING x");
    const double spacingY = parseNumber(cursor.take(), "SPACING y");
    const double spacingZ = parseNumber(cursor.take(), "SPACING z");
    if (!std::isfinite(spacingX) ||
        !std::isfinite(spacingY) ||
        !std::isfinite(spacingZ) ||
        spacingX <= 0.0 ||
        spacingY <= 0.0) {
        throw VtkParseError(
            "SPACING x and y must be positive and all spacing values must be finite");
    }

    const std::string associationToken = cursor.take();
    VtkDataAssociation association = VtkDataAssociation::Point;
    int logicalNx = pointNx;
    int logicalNy = pointNy;
    std::size_t sampleCount = pointCount;
    if (associationToken == "CELL_DATA") {
        if (pointNx < 2 || pointNy < 2) {
            throw VtkParseError(
                "CELL_DATA requires DIMENSIONS nx and ny to be at least 2");
        }
        association = VtkDataAssociation::Cell;
        logicalNx = pointNx - 1;
        logicalNy = pointNy - 1;
        sampleCount =
            checkedGridCount(logicalNx, logicalNy, "CELL");
    } else if (associationToken != "POINT_DATA") {
        throw VtkParseError(
            "Expected POINT_DATA or CELL_DATA, found '" +
            associationToken + "'");
    }

    const int declaredSampleCount = parseInteger(
        cursor.take(),
        associationToken + " count");
    if (declaredSampleCount < 0 ||
        static_cast<std::size_t>(declaredSampleCount) != sampleCount) {
        throw VtkParseError(
            associationToken +
            " count does not equal the logical sample-grid size");
    }

    VtkFrame frame;
    frame.sourcePath = path;
    frame.title = title;
    frame.association = association;
    frame.nx = static_cast<std::size_t>(logicalNx);
    frame.ny = static_cast<std::size_t>(logicalNy);
    frame.originX = originX;
    frame.originY = originY;
    frame.spacingX = spacingX;
    frame.spacingY = spacingY;
    frame.frameNumber = frameNumberFromFilename(path).value_or(-1);

    bool hasPressure = false;
    bool hasSolid = false;
    bool hasVelocity = false;

    while (!cursor.empty()) {
        const std::string declaration = cursor.take();
        if (declaration == "SCALARS") {
            const std::string name = cursor.take();
            const std::string type = cursor.take();
            int componentCount = 1;
            if (cursor.peek() != "LOOKUP_TABLE") {
                componentCount =
                    parseInteger(cursor.take(), "SCALARS component count");
            }
            cursor.expect("LOOKUP_TABLE");
            cursor.expect("default");

            if (name == "pressure") {
                if (hasPressure) {
                    throw VtkParseError("Duplicate pressure array");
                }
                if (type != "float" || componentCount != 1) {
                    throw VtkParseError(
                        "pressure must be SCALARS pressure float with one component");
                }
                frame.pressure.reserve(sampleCount);
                if (binary) {
                    cursor.beginBinaryPayload("pressure");
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        frame.pressure.push_back(
                            cursor.takeBigEndianFloat("pressure"));
                    }
                    cursor.endBinaryPayload("pressure");
                } else {
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        frame.pressure.push_back(
                            static_cast<float>(
                                parseNumber(cursor.take(), "pressure")));
                    }
                }
                hasPressure = true;
            } else if (name == "solid") {
                if (hasSolid) {
                    throw VtkParseError("Duplicate solid array");
                }
                if (type != "int" || componentCount != 1) {
                    throw VtkParseError(
                        "solid must be SCALARS solid int 1");
                }
                frame.solid.reserve(sampleCount);
                if (binary) {
                    cursor.beginBinaryPayload("solid");
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        const int value =
                            cursor.takeBigEndianInt("solid");
                        if (value != 0 && value != 1) {
                            throw VtkParseError(
                                "solid values must be 0 or 1");
                        }
                        frame.solid.push_back(
                            static_cast<std::uint8_t>(value));
                    }
                    cursor.endBinaryPayload("solid");
                } else {
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        const int value =
                            parseInteger(cursor.take(), "solid");
                        if (value != 0 && value != 1) {
                            throw VtkParseError(
                                "solid values must be 0 or 1");
                        }
                        frame.solid.push_back(
                            static_cast<std::uint8_t>(value));
                    }
                }
                hasSolid = true;
            } else {
                if (!allRequiredArraysPresent(
                        hasPressure, hasSolid, hasVelocity)) {
                    throw VtkParseError(
                        "Unknown scalar array before required arrays: " + name);
                }
                skipScalarArray(
                    cursor,
                    sampleCount,
                    componentCount,
                    type,
                    binary,
                    name);
                frame.warnings.push_back(
                    "Ignored unsupported scalar array: " + name);
            }
        } else if (declaration == "VECTORS") {
            const std::string name = cursor.take();
            const std::string type = cursor.take();
            if (name == "velocity") {
                if (hasVelocity) {
                    throw VtkParseError("Duplicate velocity array");
                }
                if (type != "float") {
                    throw VtkParseError(
                        "velocity must be VECTORS velocity float");
                }
                frame.velocity.reserve(sampleCount);
                if (binary) {
                    cursor.beginBinaryPayload("velocity");
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        frame.velocity.push_back({
                            cursor.takeBigEndianFloat("velocity x"),
                            cursor.takeBigEndianFloat("velocity y"),
                            cursor.takeBigEndianFloat("velocity z")
                        });
                    }
                    cursor.endBinaryPayload("velocity");
                } else {
                    for (std::size_t index = 0;
                         index < sampleCount;
                         ++index) {
                        frame.velocity.push_back({
                            static_cast<float>(
                                parseNumber(cursor.take(), "velocity x")),
                            static_cast<float>(
                                parseNumber(cursor.take(), "velocity y")),
                            static_cast<float>(
                                parseNumber(cursor.take(), "velocity z"))
                        });
                    }
                }
                hasVelocity = true;
            } else {
                if (!allRequiredArraysPresent(
                        hasPressure, hasSolid, hasVelocity)) {
                    throw VtkParseError(
                        "Unknown vector array before required arrays: " + name);
                }
                skipVectorArray(
                    cursor,
                    sampleCount,
                    type,
                    binary,
                    name);
                frame.warnings.push_back(
                    "Ignored unsupported vector array: " + name);
            }
        } else {
            throw VtkParseError(
                "Unsupported or misplaced VTK declaration: " + declaration);
        }
    }

    if (!allRequiredArraysPresent(hasPressure, hasSolid, hasVelocity)) {
        throw VtkParseError(
            "VTK frame is missing pressure, solid, or velocity");
    }

    frame.pressureFinite.resize(sampleCount, 0);
    frame.velocityFinite.resize(sampleCount, 0);
    frame.velocityMagnitude.resize(
        sampleCount,
        std::numeric_limits<float>::quiet_NaN());

    std::size_t nonFinitePressureCount = 0;
    std::size_t nonFiniteVelocityCount = 0;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const bool pressureIsFinite = std::isfinite(frame.pressure[index]);
        frame.pressureFinite[index] =
            static_cast<std::uint8_t>(pressureIsFinite);
        if (!pressureIsFinite) {
            ++nonFinitePressureCount;
        }

        const Velocity& velocity = frame.velocity[index];
        const bool componentsAreFinite =
            std::isfinite(velocity.x) &&
            std::isfinite(velocity.y) &&
            std::isfinite(velocity.z);
        const float magnitude = componentsAreFinite
            ? std::hypot(velocity.x, velocity.y)
            : std::numeric_limits<float>::quiet_NaN();
        const bool magnitudeIsFinite = std::isfinite(magnitude);
        if (magnitudeIsFinite) {
            frame.velocityMagnitude[index] = magnitude;
        }
        frame.velocityFinite[index] = static_cast<std::uint8_t>(
            componentsAreFinite && magnitudeIsFinite);
        if (!componentsAreFinite || !magnitudeIsFinite) {
            ++nonFiniteVelocityCount;
        }

        if (frame.solid[index] != 0) {
            continue;
        }
        if (pressureIsFinite) {
            includeValue(frame.pressureRange, frame.pressure[index]);
        }
        includeValue(frame.velocityXRange, velocity.x);
        includeValue(frame.velocityYRange, velocity.y);
        if (magnitudeIsFinite) {
            includeValue(frame.velocityMagnitudeRange, magnitude);
        }
    }

    if (nonFinitePressureCount != 0) {
        frame.warnings.push_back(
            std::to_string(nonFinitePressureCount) +
            " pressure sample(s) are non-finite");
    }
    if (nonFiniteVelocityCount != 0) {
        frame.warnings.push_back(
            std::to_string(nonFiniteVelocityCount) +
            " velocity sample(s) are non-finite");
    }

    if (frame.frameNumber < 0) {
        frame.warnings.push_back(
            "Filename does not match solution_<step>.vtk");
    } else {
        const std::optional<int> titleStep = titleFrameNumber(title);
        if (titleStep && *titleStep != frame.frameNumber) {
            frame.warnings.push_back(
                "Title step does not match filename step");
        }
    }

    return frame;
}

std::optional<int> VtkFrameParser::frameNumberFromFilename(
    const std::filesystem::path& path) {
    static const std::regex pattern(R"(^solution_([0-9]+)\.vtk$)");
    std::smatch match;
    const std::string filename = path.filename().string();
    if (!std::regex_match(filename, match, pattern)) {
        return std::nullopt;
    }
    try {
        const long long value = std::stoll(match[1].str());
        if (value > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::filesystem::path> VtkFrameParser::discoverFrames(
    const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        throw VtkParseError(
            "Frame directory does not exist: " + directory.string());
    }

    std::vector<std::pair<int, std::filesystem::path>> numberedPaths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::optional<int> number =
            frameNumberFromFilename(entry.path());
        if (number) {
            numberedPaths.emplace_back(*number, entry.path());
        }
    }

    std::sort(
        numberedPaths.begin(),
        numberedPaths.end(),
        [](const auto& first, const auto& second) {
            if (first.first != second.first) {
                return first.first < second.first;
            }
            return first.second.string() < second.second.string();
        });

    for (std::size_t index = 1; index < numberedPaths.size(); ++index) {
        if (numberedPaths[index - 1].first == numberedPaths[index].first) {
            throw VtkParseError(
                "Duplicate VTK frame number: " +
                std::to_string(numberedPaths[index].first));
        }
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(numberedPaths.size());
    for (const auto& numberedPath : numberedPaths) {
        paths.push_back(numberedPath.second);
    }
    return paths;
}

namespace {

bool sameSeriesLayout(
    const VtkFrame& reference,
    const VtkFrame& frame) {
    return frame.association == reference.association &&
        frame.nx == reference.nx &&
        frame.ny == reference.ny &&
        frame.originX == reference.originX &&
        frame.originY == reference.originY &&
        frame.spacingX == reference.spacingX &&
        frame.spacingY == reference.spacingY &&
        frame.solid == reference.solid;
}

void includeRange(DataRange& combined, const DataRange& range) {
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

VtkFrame layoutOf(const VtkFrame& frame) {
    VtkFrame layout;
    layout.association = frame.association;
    layout.nx = frame.nx;
    layout.ny = frame.ny;
    layout.originX = frame.originX;
    layout.originY = frame.originY;
    layout.spacingX = frame.spacingX;
    layout.spacingY = frame.spacingY;
    layout.solid = frame.solid;
    return layout;
}

} // namespace


std::vector<VtkFrame> VtkFrameParser::parseSeries(
    const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        throw VtkParseError("No VTK frame files were selected");
    }

    std::vector<std::pair<int, std::filesystem::path>> numberedPaths;
    numberedPaths.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            throw VtkParseError(
                "VTK frame file does not exist: " + path.string());
        }
        const std::optional<int> number = frameNumberFromFilename(path);
        if (!number) {
            throw VtkParseError(
                "Expected filename solution_<step>.vtk: " +
                path.filename().string());
        }
        numberedPaths.emplace_back(*number, path);
    }

    std::sort(
        numberedPaths.begin(),
        numberedPaths.end(),
        [](const auto& first, const auto& second) {
            if (first.first != second.first) {
                return first.first < second.first;
            }
            return first.second.string() < second.second.string();
        });

    for (std::size_t index = 1; index < numberedPaths.size(); ++index) {
        if (numberedPaths[index - 1].first == numberedPaths[index].first) {
            throw VtkParseError(
                "Duplicate VTK frame number: " +
                std::to_string(numberedPaths[index].first));
        }
    }

    std::vector<VtkFrame> frames;
    frames.reserve(numberedPaths.size());
    for (const auto& numberedPath : numberedPaths) {
        frames.push_back(parse(numberedPath.second));
    }
    validateSeries(frames);
    return frames;
}

VtkSeriesLoadResult VtkFrameParser::parseRecoverableSeries(
    const std::vector<std::filesystem::path>& paths) {
    VtkSeriesLoadResult result;
    std::vector<std::pair<int, std::filesystem::path>> numberedPaths;
    numberedPaths.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            result.rejected.push_back(
                path.filename().string() +
                ": file does not exist or is not regular");
            continue;
        }
        const std::optional<int> number = frameNumberFromFilename(path);
        if (!number) {
            result.rejected.push_back(
                path.filename().string() +
                ": expected filename solution_<step>.vtk");
            continue;
        }
        numberedPaths.emplace_back(*number, path);
    }

    std::sort(
        numberedPaths.begin(),
        numberedPaths.end(),
        [](const auto& first, const auto& second) {
            if (first.first != second.first) {
                return first.first < second.first;
            }
            return first.second.string() < second.second.string();
        });

    result.frames.reserve(numberedPaths.size());
    for (std::size_t index = 0; index < numberedPaths.size(); ++index) {
        const auto& numberedPath = numberedPaths[index];
        if (index != 0 &&
            numberedPaths[index - 1].first == numberedPath.first) {
            result.rejected.push_back(
                numberedPath.second.filename().string() +
                ": duplicate solver step " +
                std::to_string(numberedPath.first));
            continue;
        }
        try {
            VtkFrame frame = parse(numberedPath.second);
            if (!result.frames.empty() &&
                !sameSeriesLayout(result.frames.front(), frame)) {
                result.rejected.push_back(
                    numberedPath.second.filename().string() +
                    ": VTK frame series changes association, grid, or mask");
                continue;
            }
            result.frames.push_back(std::move(frame));
        } catch (const std::exception& exception) {
            result.rejected.push_back(
                numberedPath.second.filename().string() + ": " +
                exception.what());
        }
    }
    return result;
}

VtkSeriesCatalog VtkFrameParser::catalogSeries(
    const std::vector<std::filesystem::path>& paths,
    bool recoverable) {
    if (paths.empty()) {
        throw VtkParseError("No VTK frame files were selected");
    }

    VtkSeriesCatalog catalog;
    std::vector<std::pair<int, std::filesystem::path>> numberedPaths;
    numberedPaths.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            if (!recoverable) {
                throw VtkParseError(
                    "VTK frame file does not exist: " + path.string());
            }
            catalog.rejected.push_back(
                path.filename().string() +
                ": file does not exist or is not regular");
            continue;
        }
        const std::optional<int> number = frameNumberFromFilename(path);
        if (!number) {
            if (!recoverable) {
                throw VtkParseError(
                    "Expected filename solution_<step>.vtk: " +
                    path.filename().string());
            }
            catalog.rejected.push_back(
                path.filename().string() +
                ": expected filename solution_<step>.vtk");
            continue;
        }
        numberedPaths.emplace_back(*number, path);
    }

    std::sort(
        numberedPaths.begin(),
        numberedPaths.end(),
        [](const auto& first, const auto& second) {
            if (first.first != second.first) {
                return first.first < second.first;
            }
            return first.second.string() < second.second.string();
        });

    std::optional<VtkFrame> referenceLayout;
    for (std::size_t index = 0; index < numberedPaths.size(); ++index) {
        const auto& numberedPath = numberedPaths[index];
        if (index != 0 &&
            numberedPaths[index - 1].first == numberedPath.first) {
            if (!recoverable) {
                throw VtkParseError(
                    "Duplicate VTK frame number: " +
                    std::to_string(numberedPath.first));
            }
            catalog.rejected.push_back(
                numberedPath.second.filename().string() +
                ": duplicate solver step " +
                std::to_string(numberedPath.first));
            continue;
        }
        try {
            VtkFrame frame = parse(numberedPath.second);
            if (!referenceLayout) {
                referenceLayout = layoutOf(frame);
            } else if (!sameSeriesLayout(*referenceLayout, frame)) {
                throw VtkParseError(
                    "VTK frame series changes association, grid, or mask");
            }
            includeRange(catalog.pressureRange, frame.pressureRange);
            includeRange(
                catalog.velocityMagnitudeRange,
                frame.velocityMagnitudeRange);
            catalog.warningCount += frame.warnings.size();
            catalog.frames.push_back({
                frame.sourcePath,
                frame.frameNumber,
                frame.warnings.size()
            });
            if (index + 1u == numberedPaths.size()) {
                catalog.activeFrame = std::move(frame);
            }
        } catch (const std::exception& exception) {
            if (!recoverable) {
                throw;
            }
            catalog.rejected.push_back(
                numberedPath.second.filename().string() + ": " +
                exception.what());
        }
    }

    if (catalog.frames.empty()) {
        return catalog;
    }
    if (catalog.activeFrame.sourcePath != catalog.frames.back().sourcePath) {
        catalog.activeFrame = parse(catalog.frames.back().sourcePath);
        if (!referenceLayout ||
            !sameSeriesLayout(*referenceLayout, catalog.activeFrame)) {
            throw VtkParseError(
                "VTK frame changed while the series was being cataloged");
        }
    }
    return catalog;
}

VtkSeriesCatalog VtkFrameParser::indexSeries(
    const std::vector<std::filesystem::path>& paths,
    bool recoverable) {
    if (paths.empty()) {
        throw VtkParseError("No VTK frame files were selected");
    }

    VtkSeriesCatalog catalog;
    std::vector<std::pair<int, std::filesystem::path>> numberedPaths;
    numberedPaths.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            if (!recoverable) {
                throw VtkParseError(
                    "VTK frame file does not exist: " + path.string());
            }
            catalog.rejected.push_back(
                path.filename().string() +
                ": file does not exist or is not regular");
            continue;
        }
        const std::optional<int> number = frameNumberFromFilename(path);
        if (!number) {
            if (!recoverable) {
                throw VtkParseError(
                    "Expected filename solution_<step>.vtk: " +
                    path.filename().string());
            }
            catalog.rejected.push_back(
                path.filename().string() +
                ": expected filename solution_<step>.vtk");
            continue;
        }
        numberedPaths.emplace_back(*number, path);
    }

    std::sort(
        numberedPaths.begin(),
        numberedPaths.end(),
        [](const auto& first, const auto& second) {
            if (first.first != second.first) {
                return first.first < second.first;
            }
            return first.second.string() < second.second.string();
        });

    for (std::size_t index = 0; index < numberedPaths.size(); ++index) {
        const auto& numberedPath = numberedPaths[index];
        if (index != 0 &&
            numberedPaths[index - 1].first == numberedPath.first) {
            if (!recoverable) {
                throw VtkParseError(
                    "Duplicate VTK frame number: " +
                    std::to_string(numberedPath.first));
            }
            catalog.rejected.push_back(
                numberedPath.second.filename().string() +
                ": duplicate solver step " +
                std::to_string(numberedPath.first));
            continue;
        }
        catalog.frames.push_back({
            numberedPath.second,
            numberedPath.first,
            0
        });
    }

    // Only the initially displayed frame is decoded. Earlier frames are
    // validated on demand so indexing cost does not scale with dataset bytes.
    while (!catalog.frames.empty()) {
        VtkFrameDescriptor& descriptor = catalog.frames.back();
        try {
            catalog.activeFrame = parse(descriptor.sourcePath);
            descriptor.warningCount = catalog.activeFrame.warnings.size();
            catalog.warningCount = descriptor.warningCount;
            catalog.pressureRange = catalog.activeFrame.pressureRange;
            catalog.velocityMagnitudeRange =
                catalog.activeFrame.velocityMagnitudeRange;
            break;
        } catch (const std::exception& exception) {
            if (!recoverable) {
                throw;
            }
            catalog.rejected.push_back(
                descriptor.sourcePath.filename().string() + ": " +
                exception.what());
            catalog.frames.pop_back();
        }
    }
    return catalog;
}

void VtkFrameParser::validateCompatibility(
    const VtkFrame& reference,
    const VtkFrame& frame) {
    if (!sameSeriesLayout(reference, frame)) {
        throw VtkParseError(
            "VTK frame series changes association, grid, or solid mask");
    }
}

void VtkFrameParser::validateSeries(
    const std::vector<VtkFrame>& frames) {
    if (frames.empty()) {
        return;
    }
    const VtkFrame& reference = frames.front();
    for (std::size_t index = 1; index < frames.size(); ++index) {
        const VtkFrame& frame = frames[index];
        validateCompatibility(reference, frame);
    }
}

} // namespace maskui
