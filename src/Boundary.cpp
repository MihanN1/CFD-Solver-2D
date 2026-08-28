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

bool parseCaseType(const std::string& text, CaseType& out, std::string& error) {
    const std::string key = lower(text);
    if (key == "channel") { out = CaseType::Channel; return true; }
    if (key == "cavity")  { out = CaseType::Cavity;  return true; }
    error = "'" + text + "' is not a case type. Use channel or cavity.";
    return false;
}

const char* caseTypeName(CaseType type) {
    return type == CaseType::Cavity ? "cavity" : "channel";
}

BoundarySet cavityBoundaries(float lidSpeed) {
    BoundarySet set;
    for (int side = 0; side < 4; ++side)
        set.side[side].kind = BoundaryKind::Wall;
    set[BoundarySide::Top].kind = BoundaryKind::MovingWall;
    set[BoundarySide::Top].speed = lidSpeed;
    set[BoundarySide::Top].speedSet = true;
    return set;
}

BoundarySet defaultChannelBoundaries() {
    BoundarySet set;
    set[BoundarySide::Left].kind = BoundaryKind::Inlet;
    set[BoundarySide::Right].kind = BoundaryKind::Outlet;
    set[BoundarySide::Bottom].kind = BoundaryKind::Slip;
    set[BoundarySide::Top].kind = BoundaryKind::Slip;
    return set;
}

bool checkBoundaryMassBalance(const BoundarySet& sides,
                              float defaultSpeed,
                              const DomainExtent& domain,
                              const std::vector<int>& solid,
                              std::string& error,
                              double extraInflow) {
    const int nx = domain.nx;
    const int ny = domain.ny;
    if (nx < 1 || ny < 1 ||
        solid.size() != static_cast<size_t>(nx) * static_cast<size_t>(ny))
        return true;

    const double dx = static_cast<double>(domain.Lx) / nx;
    const double dy = static_cast<double>(domain.Ly) / ny;

    double net = 0.0;
    double gross = 0.0;
    double outletArea = 0.0;
    double openInlet[4] = {0.0, 0.0, 0.0, 0.0};
    bool anyOutlet = false;

    const auto walk = [&](BoundarySide which, int cells, double face,
                          int first, int stride) {
        const BoundarySpec& spec = sides[which];
        const int index = static_cast<int>(which);
        if (spec.kind == BoundaryKind::Outlet) {
            anyOutlet = true;
            for (int k = 0; k < cells; ++k)
                if (!solid[first + k * stride])
                    outletArea += face;
            return;
        }
        if (spec.kind != BoundaryKind::Inlet)
            return;

        BoundarySpec resolved = spec;
        if (!resolved.speedSet)
            resolved.speed = defaultSpeed;
        for (int k = 0; k < cells; ++k) {
            if (solid[first + k * stride])
                continue;
            const double value = inletVelocityAt(
                resolved, (k + 0.5f) / static_cast<float>(cells));
            if (value == 0.0)
                continue;
            openInlet[index] += face;
            net += value * face;
            gross += std::fabs(value) * face;
        }
    };

    net += extraInflow;
    gross += std::fabs(extraInflow);

    walk(BoundarySide::Left, ny, dy, 0, nx);
    walk(BoundarySide::Right, ny, dy, nx - 1, nx);
    walk(BoundarySide::Bottom, nx, dx, 0, 1);
    walk(BoundarySide::Top, nx, dx, (ny - 1) * nx, 1);

    for (int index = 0; index < 4; ++index) {
        const BoundarySpec& spec = sides.side[index];
        if (spec.kind != BoundaryKind::Inlet || openInlet[index] > 0.0)
            continue;
        const char* name = boundarySideName(static_cast<BoundarySide>(index));
        const float lo = std::min(spec.from, spec.to);
        const float hi = std::max(spec.from, spec.to);
        const float speed = spec.speedSet ? spec.speed : defaultSpeed;
        error = std::string("the inlet on the ") + name + " lets nothing in";
        if (speed == 0.0f)
            error += ", because its speed is 0. An inlet standing still is a "
                     "wall with extra steps - say bc" + std::string(name) +
                     "=wall if that is what it is meant to be.";
        else if (!(hi > lo))
            error += ": inletFrom and inletTo are the same number, so the band "
                     "has no width at all. They are fractions of the side, "
                     "0..1 being the whole of it.";
        else
            error += ": the band " + std::to_string(lo) + ".." +
                     std::to_string(hi) + " of that side misses the centre of "
                     "every cell on it, or every cell it covers is buried in "
                     "solid. Widen the band, or put more cells across that "
                     "side.";
        return false;
    }

    if (gross <= 0.0 || std::fabs(net) <= 1e-4 * gross || outletArea > 0.0)
        return true;

    error = (extraInflow != 0.0 ? std::string("the sources and inlets push ")
                                : std::string("the inlets push ")) +
            std::to_string(std::fabs(net)) + " m^2/s of fluid in and ";
    error += anyOutlet ? "every outlet face is buried in solid, so none of it "
                         "has anywhere to go."
                       : "no side lets any of it out.";
    error += " An incompressible fluid does not compress: no pressure field "
             "can take it, the projection cannot remove the divergence it "
             "makes, and the run would report the same leftover on every "
             "single step to the end.\n    Open a side with bcRight=outlet "
             "(or whichever one the flow should leave by), or make the inlet "
             "a wall.";
    return false;
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
        "  same flow rate as the flat one.\n"
        "  caseType=cavity sets all four of them at once: walls everywhere and\n"
        "  the top one sliding at lidSpeed. There is no inlet and no outlet in\n"
        "  that case, so the pressure is only defined up to a constant and the\n"
        "  solve says so rather than drifting.\n";
}
