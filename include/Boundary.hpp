#pragma once
#include <string>
#include <vector>

enum class BoundaryKind {
    Inlet,
    Outlet,
    Wall,
    MovingWall,
    Slip
};

enum class InletProfile {
    Uniform,
    Parabolic
};

enum class BoundarySide {
    Left = 0,
    Right = 1,
    Bottom = 2,
    Top = 3
};

struct BoundarySpec {
    BoundaryKind kind = BoundaryKind::Slip;
    InletProfile profile = InletProfile::Uniform;

    float speed = 0.0f;
    // Nobody named a speed for this side, so an inlet runs at U0 and a moving
    // wall stands still. Without the flag "0" and "not given" are the same
    // number and an inlet=0 could never be asked for.
    bool speedSet = false;

    // Fraction of the side the inlet occupies, measured from the low end of
    // the side's own axis. The rest of the side behaves as a wall. 0..1 means
    // the whole side, which is what every case that never mentions them gets.
    float from = 0.0f;
    float to = 1.0f;
};

struct BoundarySet {
    BoundarySpec side[4];

    const BoundarySpec& operator[](BoundarySide s) const {
        return side[static_cast<int>(s)];
    }
    BoundarySpec& operator[](BoundarySide s) {
        return side[static_cast<int>(s)];
    }
};

bool parseBoundaryKind(const std::string& text,
                       BoundaryKind& out,
                       std::string& error);

bool parseInletProfile(const std::string& text,
                       InletProfile& out,
                       std::string& error);

const char* boundaryKindName(BoundaryKind kind);
const char* inletProfileName(InletProfile profile);
const char* boundarySideName(BoundarySide side);

// True when the side lets fluid out of the domain, which is what fixes the
// pressure level. Without one of these the pressure problem is singular and
// the caller has to say so rather than let the solve drift.
bool sideIsOpen(const BoundarySpec& spec);

// Where the inlet band starts and ends along the side, in cell indices, given
// how many cells the side has. Half-open: [first, last).
void inletBandCells(const BoundarySpec& spec,
                    int cellsAlongSide,
                    int& first,
                    int& last);

// Normal velocity the side imposes at position t in 0..1 along its own axis.
// Zero outside the inlet band and for every kind that is not an inlet.
float inletVelocityAt(const BoundarySpec& spec, float t);

enum class CaseType {
    Channel,
    Cavity
};

bool parseCaseType(const std::string& text, CaseType& out, std::string& error);
const char* caseTypeName(CaseType type);

// Inlet on the left, outlet on the right, free slip above and below: the
// channel every version before the boundary layer existed solved, and what a
// run that never mentions a side still gets.
BoundarySet defaultChannelBoundaries();

// Four walls with the top one sliding: a lid driven cavity, the case every
// incompressible solver is measured against because Ghia, Ghia and Shin
// tabulated the answer in 1982 and nobody has had to argue about it since.
BoundarySet cavityBoundaries(float lidSpeed);

struct DomainExtent {
    float Lx = 1.0f;
    float Ly = 1.0f;
    int nx = 0;
    int ny = 0;
};

// Adds up what the sides prescribe against what they leave open. An outlet
// carries whatever the pressure sends through it, so a set with one of those
// balances itself and this only has to catch the sets that cannot: fluid
// pushed into a domain with nothing open, and an inlet band too narrow to
// cover a single cell. Both run to the end producing nonsense otherwise.
bool checkBoundaryMassBalance(const BoundarySet& sides,
                              float defaultSpeed,
                              const DomainExtent& domain,
                              const std::vector<int>& solid,
                              std::string& error);

std::string boundaryHelp();
