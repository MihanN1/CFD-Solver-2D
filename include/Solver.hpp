#pragma once
#include "Boundary.hpp"
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
#include "Phase.hpp"
#include "Restart.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>

class Solver {
public:
    Solver(const Config& cfg, const Mesh& mesh);
    void run();

    bool setInitialState(RestartData&& state, const std::string& framePrefix);

private:
    const Config& cfg;
    const Mesh& mesh;
    Multigrid multigrid;

    // Fields on staggered grid
    // Pressure (cell centres): size nx * ny
    std::vector<float> p;
    // Right-hand side of Poisson equation
    std::vector<float> rhs;
    // u on vertical faces: size (nx+1) * ny
    std::vector<float> u, u_star;
    std::vector<float> uPrev, vPrev;

    // Snapshot the steady check compares against, and the time it was taken
    // at. Only allocated when the check is on, because on a channel it is dead
    // weight the size of the velocity field.
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

    // One side of the domain, reduced to what the kernels actually need: the
    // ghost value a tangential stencil reads across it, written as
    // ghost = sign * inner + offset, and whether the normal face is driven,
    // held shut or left to the pressure.
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

    // Two fluids, or one. Everything below is dead weight at one phase: the
    // field is never allocated, the coefficients are never uploaded, and the
    // kernels take the branch they always took.
    PhaseField phase;
    bool multiphase = false;
    std::vector<float> coeffX, coeffY;
    void refreshPhaseCoefficients();
    void advectPhase();

    // Regions pushing fluid in from inside the domain. cellRate is the volume
    // added per unit volume per second in each cell, which is exactly the
    // divergence the projection has to produce there.
    std::vector<float> sourceRate;
    std::vector<float> sourcePhase;
    std::vector<float> sourceU, sourceV;
    bool hasSources = false;
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

    // One whole forward Euler step of the projection method. The Runge-Kutta
    // schemes are convex combinations of these, which is what keeps the result
    // divergence free: every stage ends in a projection.
    void advanceStage();
    void blendWithPrevious(float weightPrevious);

    // Largest velocity change per unit time since the last snapshot, divided by
    // whatever drives this case. Returns false while there is nothing to
    // compare against yet.
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

    // What the pressure array has to be multiplied by to be pascals. At one
    // phase p is the kinematic pressure and this is the density; at two the
    // projection already carries 1/rho on every face, so p is in pascals and
    // this is one.
    float pressureScale() const { return multiphase ? 1.0f : cfg.ro; }

    void saveVTK(int stepNum) const;

    // Named fields the frame carries beyond the three it has always had. The
    // writer walks this list rather than knowing what is in it, so a field
    // added later is one entry here and nothing else.
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
    // The hydrostatic potential itself, whichever mode is running. A solid
    // cell has no solved pressure in it, so this is what goes in its place;
    // leaving it at zero punches a hole through the pressure map that has
    // nothing to do with the flow.
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

    // Same field at the middle of a boundary face rather than a cell centre.
    // With the force in the solve, p carries the head as well, so an open side
    // is held at the head rather than at zero and the outflow stops being
    // driven by a step in pressure that is not physically there.
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

    // The pressure the outside of an open side is held at, in whatever units
    // p is carrying. At one density that is the kinematic head and this is
    // phiFace unchanged; at two, p is in pascals, so the head is weighed by the
    // density actually sitting against that face - the light fluid does not
    // hold up the same column the heavy one does.
    float phiOutside(BoundarySide side, int k) const;

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
