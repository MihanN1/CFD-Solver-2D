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

    bool speedSet = false;

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

bool sideIsOpen(const BoundarySpec& spec);

void inletBandCells(const BoundarySpec& spec,
                    int cellsAlongSide,
                    int& first,
                    int& last);

float inletVelocityAt(const BoundarySpec& spec, float t);

enum class CaseType {
    Channel,
    Cavity
};

bool parseCaseType(const std::string& text, CaseType& out, std::string& error);
const char* caseTypeName(CaseType type);

BoundarySet defaultChannelBoundaries();

BoundarySet cavityBoundaries(float lidSpeed);

struct DomainExtent {
    float Lx = 1.0f;
    float Ly = 1.0f;
    int nx = 0;
    int ny = 0;
};

bool checkBoundaryMassBalance(const BoundarySet& sides,
                              float defaultSpeed,
                              const DomainExtent& domain,
                              const std::vector<int>& solid,
                              std::string& error,
                              double extraInflow = 0.0);

std::string boundaryHelp();
