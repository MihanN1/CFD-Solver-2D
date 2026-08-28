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

// How the phase fraction is carried. None of these reconstruct the interface
// the way PLIC does; they are algebraic, which is a fraction of the cost and
// the reason a two-phase run is not ten times slower than a single-phase one.
// upwind smears the interface and is here as the thing the other two are
// measured against, hric compresses it by steering towards downwind where the
// interface is aligned with the flow, cicsam does the same with a smooth
// weighting that behaves better when it is not.
enum class VofScheme {
    Upwind,
    Hric,
    Cicsam
};

// What is in the domain when the run starts. layer fills everything below
// phaseLevel with fluid 1, drop puts a circle of fluid 1 in a domain of fluid
// 2, column is the dam break: a block of fluid 1 in the low corner. file reads
// one value per cell from a text file, which is what the UI paints.
enum class PhaseInit {
    Layer,
    Drop,
    Column,
    File
};

bool parseVofScheme(const std::string& text, VofScheme& out, std::string& error);
bool parsePhaseInit(const std::string& text, PhaseInit& out, std::string& error);
const char* vofSchemeName(VofScheme scheme);
const char* phaseInitName(PhaseInit init);

// One region that pushes fluid into the domain from the inside rather than
// through a side. rate is the volume flow per unit area of the region per
// second, so it does not change meaning when the region is resized; the
// direction is where the fluid is aimed, and phase is which fluid comes out.
struct FlowSource {
    float x = 0.0f, y = 0.0f;
    float radius = 0.0f;
    float rate = 0.0f;
    float angle = 0.0f;   // degrees, 0 = +x, counter-clockwise
    float phase = 1.0f;
};

bool parseSources(const std::string& text,
                  std::vector<FlowSource>& out,
                  std::string& error);

std::string sourcesHelp();

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

    // Phases. 1 is every run written before this existed: one density, one
    // viscosity, ro and nu, and not one line of the phase code runs. 2 turns on
    // the phase fraction, and then rho1/nu1 and rho2/nu2 are what the two
    // fluids are and ro/nu are ignored.
    int phases = 1;
    float rho1 = 1000.0f;   // kg/m^3, the fluid the initial shape is made of
    float rho2 = 1.225f;    // kg/m^3, what fills the rest of the domain
    float nu1 = 1e-6f;      // m^2/s
    float nu2 = 1.5e-5f;    // m^2/s
    PhaseInit phaseInit = PhaseInit::Layer;
    float phaseLevel = 0.5f;    // layer height / drop radius, fraction of the domain
    float phaseX = 0.5f;        // drop or column centre, fraction of Lx
    float phaseY = 0.5f;        // drop centre, fraction of Ly
    std::string initialPhaseFile = "";
    VofScheme vofScheme = VofScheme::Hric;

    // Fluid pushed in from inside the domain rather than through a side.
    // Empty is every run so far.
    std::string sources = "";

    // Numerics
    ConvectionScheme convection = ConvectionScheme::Upwind;
    LimiterKind limiter = LimiterKind::VanLeer;
    TimeScheme timeScheme = TimeScheme::Euler;

    // Boundaries
    // caseType is a preset over the four sides below rather than a mode of its
    // own: setting it rewrites them, and naming a side afterwards overrides
    // whatever the preset put there.
    CaseType caseType = CaseType::Channel;
    float lidSpeed = 1.0f;
    BoundarySet boundaries = defaultChannelBoundaries();

    // Ends the run when the field stops changing, measured as the largest
    // velocity change per unit time against U0 or the lid speed. Zero is off,
    // which is what every run before this one did. A cavity has no natural end
    // time and this is the only sensible one it has.
    float steadyTolerance = 0.0f;

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

    // "none" has always meant the verification circle rather than nothing at
    // all, and every configuration written so far relies on that. "empty" is
    // the way to ask for a domain with no body in it, which is what a cavity
    // wants and what there was no way to say before.
    bool emptyDomain() const;

    // Two fluids share the domain, which is what turns on the phase field, the
    // variable coefficients in the projection and the body force formulation
    // of gravity. Written out once here so nothing has to remember that
    // "phases > 1" is the condition.
    bool multiphase() const { return phases > 1; }

    std::vector<FlowSource> resolvedSources() const;

    std::string serialize() const;
};
