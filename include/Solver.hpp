#pragma once
#include "Boundary.hpp"
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
#include "Phase.hpp"
#include "RigidBody.hpp"
#include "Restart.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>

class Solver {
public:
    Solver(const Config& cfg, Mesh& mesh);
    void run();

    bool setInitialState(RestartData&& state, const std::string& framePrefix);

private:
    const Config& cfg;
    Mesh& mesh;
    Multigrid multigrid;

    // Fields on staggered grid
    // Pressure (cell centres): size nx * ny
    std::vector<float> p;
    // Right-hand side of Poisson equation
    std::vector<float> rhs;
    // u on vertical faces: size (nx+1) * ny
    std::vector<float> u, u_star;
    std::vector<float> uPrev, vPrev;

    std::vector<float> uSteady, vSteady;
    double steadyStamp = 0.0;
    float steadyRate = 0.0f;
    // v on horizontal faces: size nx * (ny+1)
    std::vector<float> v, v_star;

    double currentTime = 0.0;
    int step = 0;
    float dt = 0.0f;  // current time step
    float lastResidual = 0.0f;  // relative residual of the last pressure solve

    // The velocity field stopped being a number, which no step size can be
    // recovered from. run() stops on it instead of grinding out frames of NaN.
    bool fieldBroken = false;
    // Steps whose pressure solve ran out of V-cycles before reaching
    // mgTolerance. The velocity field keeps a divergence of that order, and
    // nothing said so before: mg res simply sat in the step line.
    bool capillaryReported = false;
    bool poissonShortReported = false;
    int poissonShortSteps = 0;
    float poissonWorstResidual = 0.0f;

    bool hasRestartState = false;
    bool needsProjection = false;   // u/v were rebuilt from cell averages
    float restartDt = 0.0f;         // dt that was in flight when the frame was written
    std::string framePrefix = "solution";   // <framePrefix>_<step>.vtk
    std::filesystem::path outputPath;
    std::string configHeader;

    std::vector<uint8_t> uFluidMask;
    std::vector<uint8_t> vFluidMask;
    std::vector<float> uFluidMaskF;
    std::vector<float> vFluidMaskF;
    std::vector<uint8_t> solidMask;
    std::vector<float> fluidCellMaskF;

    // Velocity prescribed on the closed faces, resolved once because neither
    // the geometry nor the motion changes during a run. Zero everywhere when
    // no wall moves, so every expression below degenerates to the old one.
    std::vector<float> uWall;
    std::vector<float> vWall;

    // Rigid surface velocity of one object: u = slide + omega x (x - centre).
    struct WallField {
        float omega = 0.0f;   // rad/s, counter-clockwise
        float cx = 0.0f, cy = 0.0f;
        float slideX = 0.0f, slideY = 0.0f;
        bool slip = false;
    };
    std::vector<WallField> wallField;   // indexed by mesh object id, 0 unused
    bool wallsMove = false;
    bool wallsSlip = false;

    std::vector<RigidBody> bodies;
    bool bodiesMove = false;
    bool bodiesFree = false;
    std::vector<uint8_t> prevSolidMask;
    std::vector<uint8_t> prevUFluidMask, prevVFluidMask;
    std::vector<int> prevObjectId;
    std::vector<float> bodyForceScratch;
    int freshCells = 0;
    int bodyPasses = 0;
    bool renumberReported = false;

    void resolveBodyMotion();
    std::vector<RestartData::BodyState> restartBodies;
    void refreshGeometry();
    void fillFreshCells();
    void bodyForces();
    void advanceBodies(float stepDt);
    void applyBodyPoses();
    void resolveCollisions(float stepDt);
    bool bodyCollisions = false;
    int contactsReported = 0;
    void reportBodies() const;

    // One buried face of a free-slip wall and the open faces it mirrors, so
    // that the difference the viscous stencil sees across the wall is zero.
    // second == first when the wall has fluid on one side only, which is the
    // usual case; then the average is that face exactly.
    struct SlipFace {
        int face;
        int first;
        int second;
    };
    std::vector<SlipFace> uSlipFaces;
    std::vector<SlipFace> vSlipFaces;

    // The same pairing for a no-slip wall, which needs the opposite value on
    // the buried face. The wall lies halfway between it and the open one, so
    // leaving the wall velocity on the buried face puts the average of the two
    // on the wall rather than the wall's own velocity, and every stencil
    // reading across it sees half the shear there really is. wall is what the
    // wall imposes at its own position, which is neither face's position.
    struct MirrorFace {
        int face;
        int first;
        int second;
        float wall;
    };
    std::vector<MirrorFace> uMirrorFaces;
    std::vector<MirrorFace> vMirrorFaces;

    struct SideData {
        BoundaryKind kind = BoundaryKind::Slip;
        float ghostSign = 1.0f;
        float ghostOffset = 0.0f;
        bool outlet = false;
        bool inlet = false;
    };

    SideData sideLeft, sideRight, sideBottom, sideTop;
    std::vector<float> uInletLeft, uInletRight;
    std::vector<float> vInletBottom, vInletTop;

    void resolveBoundaries();
    void applyOutletFaces();

    PhaseField phase;
    bool multiphase = false;
    std::vector<float> coeffX, coeffY;
    void refreshPhaseCoefficients();
    void advectPhase();

    bool hasTension = false;
    std::vector<float> tensionX, tensionY;
    void refreshSurfaceTension();

    std::vector<float> sourceRate;
    std::vector<float> sourcePhase;
    std::vector<float> sourceU, sourceV;

    std::vector<int> sourceCells;
    bool hasSources = false;
    bool sourcesRide = false;
    std::vector<uint8_t> sourceLive;
    bool sourcesReported = false;
    double sourceInflow = 0.0;
    void buildSources();
    void applySources();

    float dx = 0.0f, dy = 0.0f;
    float invDx = 0.0f, invDy = 0.0f;
    float invDx2 = 0.0f, invDy2 = 0.0f;

    // Gravity as a vector, resolved once in the constructor. Both stay at zero
    // when gravity is off, so every expression below degenerates to the old one.
    float gx = 0.0f, gy = 0.0f;
    bool bodyGravity = false;

    void initFields();
    void computeDt();

    void predictor();
    template <bool TwoPhase> void predictorByScheme();
    template <int Phi, bool TwoPhase> void predictorImpl();
    template <bool TwoPhase> void correctorImpl();

    void advanceStage();
    void blendWithPrevious(float weightPrevious);

    bool steadyReached();
    void solvePoisson();
    void corrector();
    void applyBC();
    void applyBoundaryVelocities(std::vector<float>& uf,
                                 std::vector<float>& vf,
                                 bool extrapolateOutlet) const;
    void buildFaceMasks();
    void projectRestartState(); // one projection after an approximate restart

    // Turns cfg.wallMotion into one rigid velocity field per object, checks
    // the numbers against the geometry and reports what will actually move.
    void resolveWallMotion();

    // Collects the buried faces of every free-slip object together with the
    // open faces they mirror. Needs the face masks finished, so it runs after
    // buildFaceMasks rather than inside it.
    void buildSlipFaces();

    // Refreshes those faces from the current field, which is what makes the
    // wall frictionless: the stencil reading across it sees no difference.
    void applySlipFaces();

    // Same two steps for every wall that is not free-slip, and the wall
    // velocity each pair has to reproduce at the wall itself.
    void buildMirrorFaces();
    void applyMirrorFaces();

    float maxDivergence() const;
    float maxVelocity() const;

    float pressureScale() const { return multiphase ? 1.0f : cfg.ro; }

    void saveVTK(int stepNum) const;

    struct ExtraField {
        std::string name;
        std::vector<float> values;
    };
    std::vector<ExtraField> buildExtraFields(
        const std::vector<float>& uCell,
        const std::vector<float>& vCell) const;

    // Gravity potential, phi = g . x, i.e. the hydrostatic pressure. At
    // constant density it is an exact solution of the discrete pressure
    // problem for the body force, so p carries only the reduced pressure and
    // this is added on output. Identically zero when gravity is off. The
    // reference point is the outlet at mid-height, which keeps phi small.

    inline float headCell(int i, int j) const {
        return gx * ((i + 0.5f - cfg.nx) * dx) +
               gy * ((j + 0.5f - 0.5f * cfg.ny) * dy);
    }

    inline float phiCell(int i, int j) const {
        if (bodyGravity)
            return 0.0f;
        return gx * ((i + 0.5f - cfg.nx) * dx) +
               gy * ((j + 0.5f - 0.5f * cfg.ny) * dy);
    }

    inline float phiFace(BoundarySide side, int k) const {
        if (!bodyGravity)
            return 0.0f;
        switch (side) {
        case BoundarySide::Left:
            return gx * (-static_cast<float>(cfg.nx) * dx) +
                   gy * ((k + 0.5f - 0.5f * cfg.ny) * dy);
        case BoundarySide::Right:
            return gy * ((k + 0.5f - 0.5f * cfg.ny) * dy);
        case BoundarySide::Bottom:
            return gx * ((k + 0.5f - cfg.nx) * dx) +
                   gy * (-0.5f * static_cast<float>(cfg.ny) * dy);
        case BoundarySide::Top:
            return gx * ((k + 0.5f - cfg.nx) * dx) +
                   gy * (0.5f * static_cast<float>(cfg.ny) * dy);
        }
        return 0.0f;
    }

    float phiOutside(BoundarySide side, int k) const;

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
