#include "Boundary.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

std::string lower(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}   // namespace

bool parseBoundaryKind(const std::string& text,
                       BoundaryKind& out,
                       std::string& error) {
    const std::string key = lower(text);
    if (key == "inlet")      { out = BoundaryKind::Inlet;      return true; }
    if (key == "outlet")     { out = BoundaryKind::Outlet;     return true; }
    if (key == "wall")       { out = BoundaryKind::Wall;       return true; }
    if (key == "movingwall" || key == "moving" || key == "lid")
                             { out = BoundaryKind::MovingWall; return true; }
    if (key == "slip")       { out = BoundaryKind::Slip;       return true; }
    error = "'" + text +
            "' is not a boundary kind. Use inlet, outlet, wall, movingWall "
            "or slip.";
    return false;
}

bool parseInletProfile(const std::string& text,
                       InletProfile& out,
                       std::string& error) {
    const std::string key = lower(text);
    if (key == "uniform")   { out = InletProfile::Uniform;   return true; }
    if (key == "parabolic") { out = InletProfile::Parabolic; return true; }
    error = "'" + text + "' is not an inlet profile. Use uniform or parabolic.";
    return false;
}

const char* boundaryKindName(BoundaryKind kind) {
    switch (kind) {
    case BoundaryKind::Inlet:      return "inlet";
    case BoundaryKind::Outlet:     return "outlet";
    case BoundaryKind::Wall:       return "wall";
    case BoundaryKind::MovingWall: return "movingWall";
    case BoundaryKind::Slip:       return "slip";
    }
    return "slip";
}

const char* inletProfileName(InletProfile profile) {
    return profile == InletProfile::Parabolic ? "parabolic" : "uniform";
}

const char* boundarySideName(BoundarySide side) {
    switch (side) {
    case BoundarySide::Left:   return "left";
    case BoundarySide::Right:  return "right";
    case BoundarySide::Bottom: return "bottom";
    case BoundarySide::Top:    return "top";
    }
    return "left";
}

bool sideIsOpen(const BoundarySpec& spec) {
    return spec.kind == BoundaryKind::Outlet;
}

void inletBandCells(const BoundarySpec& spec,
                    int cellsAlongSide,
                    int& first,
                    int& last) {
    if (spec.kind != BoundaryKind::Inlet) {
        first = 0;
        last = cellsAlongSide;
        return;
    }
    const float lo = std::min(spec.from, spec.to);
    const float hi = std::max(spec.from, spec.to);
    first = static_cast<int>(std::floor(lo * cellsAlongSide + 0.5f));
    last = static_cast<int>(std::floor(hi * cellsAlongSide + 0.5f));
    first = std::max(0, std::min(first, cellsAlongSide));
    last = std::max(first, std::min(last, cellsAlongSide));
    // A band thinner than one cell still has to let something through, or the
    // case silently becomes a closed box and the pressure problem goes bad.
    if (last == first && cellsAlongSide > 0) {
        last = std::min(cellsAlongSide, first + 1);
        if (last == first)
            first = std::max(0, last - 1);
    }
}

float inletVelocityAt(const BoundarySpec& spec, float t) {
    if (spec.kind != BoundaryKind::Inlet)
        return 0.0f;

    const float lo = std::min(spec.from, spec.to);
    const float hi = std::max(spec.from, spec.to);
    if (t < lo || t > hi)
        return 0.0f;
    if (spec.profile == InletProfile::Uniform)
        return spec.speed;

    const float width = hi - lo;
    if (!(width > 0.0f))
        return spec.speed;

    // Peak of 1.5 * speed, so the parabola carries the same flow rate as the
    // flat profile of the same speed and nobody has to convert by hand.
    const float s = (t - lo) / width;
    return 6.0f * spec.speed * s * (1.0f - s);
}

BoundarySet defaultChannelBoundaries() {
    BoundarySet set;
    set[BoundarySide::Left].kind = BoundaryKind::Inlet;
    set[BoundarySide::Right].kind = BoundaryKind::Outlet;
    set[BoundarySide::Bottom].kind = BoundaryKind::Slip;
    set[BoundarySide::Top].kind = BoundaryKind::Slip;
    return set;
}

std::string boundaryHelp() {
    return
        "  Boundary kinds, one per side (bcLeft, bcRight, bcBottom, bcTop):\n"
        "        inlet       fluid enters at inletSpeed, or U0 when that is 0\n"
        "        outlet      fluid leaves; pressure is fixed here\n"
        "        wall        no-slip: the fluid sticks to it\n"
        "        movingWall  no-slip, but the wall itself slides at\n"
        "                    bc<Side>Speed - this is the lid of a cavity\n"
        "        slip        free-slip: no flow through it, no friction along\n"
        "  A run needs at least one outlet, or the pressure has no level to\n"
        "  sit at. The default is inlet on the left, outlet on the right and\n"
        "  slip above and below, which is the channel every earlier version\n"
        "  solved.\n"
        "  inletFrom and inletTo cut the inlet down to a band of the side,\n"
        "  measured from its low end as fractions: inletFrom=0.25 inletTo=0.75\n"
        "  is the middle half, and the rest of that side becomes a wall.\n"
        "  inletProfile=parabolic bends the band into a parabola carrying the\n"
        "  same flow rate as the flat one.\n";
}
