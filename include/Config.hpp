#pragma once
#include "Boundary.hpp"
#include <string>
#include <vector>

// Where the weight of the fluid goes. reduced keeps the solve untouched and
// adds the hydrostatic head on output, which is exact while the density is one
// number for the whole domain. body puts the force in the predictor and solves
// for the total pressure, which is what a domain holding two densities needs.
enum class GravityMode {
    Reduced,
    Body
};

// How the convective term reads the field. upwind is the first order scheme
// this solver has always used. central is second order and keeps nothing back,
// which needs rk2 or rk3 under it. muscl is the two blended by a limiter, so
// it is second order where the field is smooth and falls back to upwind where
// it is not, which is what stops it overshooting at a front.
enum class ConvectionScheme {
    Upwind,
    Central,
    Muscl
};

enum class LimiterKind {
    Minmod,
    VanLeer,
    Superbee
};

// Forward Euler is one projection per step. The two SSP Runge-Kutta schemes
// project on every stage, which is what keeps the result divergence free, and
// cost that many times more per step.
enum class TimeScheme {
    Euler,
    RK2,
    RK3
};

// One geometry file and where it sits. An unplaced profile keeps the old
// behaviour: centred in the domain at a fifth of its smaller side.
struct Profile {
    std::string file;
    float x = 0.0f;
    float y = 0.0f;
    bool placed = false;
    float size = 0.0f;
    float rotation = 0.0f;
    float angleX = 0.0f;
    float angleZ = 0.0f;
    bool angleSet = false;
    bool invert = false;
    bool invertSet = false;
};

// Reads the profiles string: "<file>@x=..,y=..;<file>@x=..". The separator is
// '@' rather than ':' because a Windows path already owns the colon. A token
// with no '@' is a file with no placement. Returns false and fills error in
// exactly the shape setParam uses.
bool parseProfiles(const std::string& text,
                   std::vector<Profile>& out,
                   std::string& error);

std::string profilesHelp();

// What one solid object does to the fluid touching it. rotation turns the
// surface about the object's own centroid, slideX/slideY drag it in a straight
// line; the object itself never moves, only the velocity its walls impose on
// the fluid. slip replaces no-slip with free-slip and is exclusive with the
// other three, since a wall that carries no tangential stress cannot drag.
struct WallMotion {
    int object = 0;
    float rotation = 0.0f;   // degrees/s, counter-clockwise
    float slideX = 0.0f;     // m/s
    float slideY = 0.0f;     // m/s
    bool slip = false;
};

// Reads the wallMotion string. Settings are separated by ';' or ',' and each
// object opens with "<number>:", so "1:rot=90;2:slideX=0.5" and the same line
// written with commas both parse. An empty string means nothing moves.
// Returns false and fills error in exactly the shape setParam uses.
bool parseWallMotion(const std::string& text,
                     std::vector<WallMotion>& out,
                     std::string& error);

// The whole syntax as a block ready to print: the shape of the line, what each
// setting does, why slip and the moving settings exclude each other, and lines
// that can be copied as they are. One sentence inside a prompt was not enough
// for anybody who had not read the parser, so the prompt prints this instead.
std::string wallMotionHelp();

struct Config {
    bool restart = false;
    std::string restartFile = "";
    double addTime = 0.0;

    // Domain
    float Lx = 1.0, Ly = 1.0;
    int nx = 50, ny = 50;

    // Flow
    float U0 = 1.0;
    float nu = 0.01;

    // Density
    float ro = 1.225;   // kg/m^3

    bool gravityEnabled = false;
    float gravityAccel = 9.81f;   // m/s^2
    float gravityAngle = 0.0f;    // degrees, clockwise, 0 = down
    GravityMode gravityMode = GravityMode::Reduced;

    // Numerics
    ConvectionScheme convection = ConvectionScheme::Upwind;
    LimiterKind limiter = LimiterKind::VanLeer;
    TimeScheme timeScheme = TimeScheme::Euler;

    // Boundaries
    BoundarySet boundaries = defaultChannelBoundaries();

    // Time
    float CFL = 0.5;
    double totalTime = 10.0;
    int dtUpdateInterval = 5;   // steps between dt recomputations
    float dtSafety = 0.9f;      // covers the velocity growth in between

    float omega = 1.85f;
    float smootherOmega = 1.15f;

    // Multigrid controls
    int mgIterations = 2;        // V-cycles per pressure solve
    float mgTolerance = 1e-4f;   // relative residual ||r|| / ||rhs||
    int mgMinCoarseSize = 8;     // stop coarsening below this many cells per axis

    bool useCuda = true;

    // Output
    // Extra named fields written into every frame beside pressure, solid and
    // velocity. Comma separated, empty by default so a frame stays exactly the
    // size it used to be unless somebody asks for more.
    std::string extraFields = "";
    int saveInterval = 20;              // write a VTK file every N steps
    std::string outputDir = "output";   // directory for solution_*.vtk

    // Geometry
    std::string geometryFile = "none";
    // Several models at once, each with its own place in the domain. Empty
    // means geometryFile alone, which is every configuration written so far.
    std::string profiles = "";
    float sliceAngleX = 0.0;   // degrees
    float sliceAngleZ = 0.0;   // degrees
    float sliceRotation = 0.0;   // degrees
    bool invertSection = false; // doesn't allow to invert the model by default

    // Wall behaviour
    std::string wallMotion = "";   // "1:rot=90,slideX=0.5;2:slip=1", empty = static no-slip

    // Methods
    void readFromConsole();

    // One prompt = one whole line = one setParam, so a bad answer cannot
    // poison the stream for every prompt after it. Enter keeps the current
    // value. Returns false only when the input stream is dead.
    bool ask(const std::string& key, const std::string& prompt);

    // Current value of a key as text, for the [default] shown in the prompt.
    std::string currentValue(const std::string& key) const;

    void print() const;
    bool modifyParam(const std::string& name);
    bool confirm();   // returns true if confirmed, false if modified

    bool setParam(const std::string& key, const std::string& value);

    // Same, but says what exactly is wrong with the value instead of quietly
    // turning it into a zero. *warning is filled when the value is legal but
    // almost certainly not what was meant; the assignment still happens then.
    bool setParam(const std::string& key,
                  const std::string& value,
                  std::string& error,
                  std::string* warning = nullptr);

    // "nx" for "NX", "--nx", "'nx'". Empty when it is not a parameter at all.
    static std::string canonicalKey(const std::string& key);
    // Nearest parameter name for a typo, empty when nothing is close enough.
    static std::string suggestKey(const std::string& key);

    // geometryFile and profiles say the same thing in two shapes; this is the
    // one the mesh actually reads.
    std::vector<Profile> resolvedProfiles() const;

    std::string serialize() const;
};
