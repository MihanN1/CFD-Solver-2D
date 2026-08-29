#pragma once
#include "Boundary.hpp"
#include <string>
#include <vector>

enum class GravityMode {
    Reduced,
    Body
};

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

enum class TimeScheme {
    Euler,
    RK2,
    RK3
};

enum class VofScheme {
    Upwind,
    Hric,
    Cicsam
};

enum class PhaseInit {
    Layer,
    Drop,
    Column,
    File
};

enum class MixingKind {
    Immiscible,
    Miscible
};

bool parseMixingKind(const std::string& text, MixingKind& out,
                     std::string& error);
const char* mixingKindName(MixingKind kind);

bool parseVofScheme(const std::string& text, VofScheme& out, std::string& error);
bool parsePhaseInit(const std::string& text, PhaseInit& out, std::string& error);
const char* vofSchemeName(VofScheme scheme);
const char* phaseInitName(PhaseInit init);

struct FlowSource {
    float x = 0.0f, y = 0.0f;
    float radius = 0.0f;
    float rate = 0.0f;
    float angle = 0.0f;
    float phase = 1.0f;

    int body = 0;
};

bool parseSources(const std::string& text,
                  std::vector<FlowSource>& out,
                  std::string& error);

std::string sourcesHelp();

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

struct BodyKeyframe {
    float time = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float omega = 0.0f;   // degrees/s, counter-clockwise
    bool free = false;
};

struct BodyMotion {
    int object = 0;
    bool free = false;
    float vx = 0.0f;
    float vy = 0.0f;
    float omega = 0.0f;
    float mass = 0.0f;
    float inertia = 0.0f;
    float density = 0.0f;
    bool pinX = false, pinY = false, pinRot = false;
    std::vector<BodyKeyframe> keys;
};

bool parseBodyMotion(const std::string& text,
                     std::vector<BodyMotion>& out,
                     std::string& error);

std::string bodyMotionHelp();

enum class BodyCoupling {
    Weak,
    Added,
    Strong
};

bool parseBodyCoupling(const std::string& text, BodyCoupling& out,
                       std::string& error);
const char* bodyCouplingName(BodyCoupling coupling);

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

    int phases = 1;
    float rho1 = 1000.0f;
    float rho2 = 1.225f;
    float nu1 = 1e-6f;
    float nu2 = 1.5e-5f;
    PhaseInit phaseInit = PhaseInit::Layer;
    float phaseLevel = 0.5f;
    float phaseX = 0.5f;
    float phaseY = 0.5f;
    std::string initialPhaseFile = "";
    VofScheme vofScheme = VofScheme::Hric;

    MixingKind mixing = MixingKind::Immiscible;

    float diffusivity = 1e-6f;

    float surfaceTension = 0.0f;

    float contactAngle = 90.0f;

    std::string sources = "";

    ConvectionScheme convection = ConvectionScheme::Upwind;
    LimiterKind limiter = LimiterKind::VanLeer;
    TimeScheme timeScheme = TimeScheme::Euler;

    CaseType caseType = CaseType::Channel;
    float lidSpeed = 1.0f;
    BoundarySet boundaries = defaultChannelBoundaries();

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

    std::string extraFields = "";
    int saveInterval = 20;              // write a VTK file every N steps
    std::string outputDir = "output";   // directory for solution_*.vtk

    // Geometry
    std::string geometryFile = "none";

    std::string profiles = "";
    float sliceAngleX = 0.0;   // degrees
    float sliceAngleZ = 0.0;   // degrees
    float sliceRotation = 0.0;   // degrees
    bool invertSection = false; // doesn't allow to invert the model by default

    // Wall behaviour
    std::string wallMotion = "";   // "1:rot=90,slideX=0.5;2:slip=1", empty = static no-slip

    std::string bodyMotion = "";
    BodyCoupling bodyCoupling = BodyCoupling::Added;
    int bodyIterations = 4;
    bool bodyCollisions = false;
    float bodyRestitution = 0.2f;
    bool bodyForceReport = false;

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

    std::vector<Profile> resolvedProfiles() const;

    bool emptyDomain() const;

    bool multiphase() const { return phases > 1; }

    bool bodiesMove() const { return !bodyMotion.empty(); }

    bool hasSurfaceTension() const {
        return multiphase() && mixing == MixingKind::Immiscible &&
               surfaceTension > 0.0f;
    }
    bool miscible() const {
        return multiphase() && mixing == MixingKind::Miscible;
    }

    std::vector<FlowSource> resolvedSources() const;

    std::string serialize() const;
};
