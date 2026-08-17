#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
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
    // v on horizontal faces: size nx * (ny+1)
    std::vector<float> v, v_star;

    double currentTime = 0.0;
    int step = 0;
    float dt = 0.0f;  // current time step
    float lastResidual = 0.0f;  // relative residual of the last pressure solve

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

    float dx = 0.0f, dy = 0.0f;
    float invDx = 0.0f, invDy = 0.0f;
    float invDx2 = 0.0f, invDy2 = 0.0f;

    // Gravity as a vector, resolved once in the constructor. Both stay at zero
    // when gravity is off, so every expression below degenerates to the old one.
    float gx = 0.0f, gy = 0.0f;

    void initFields();
    void computeDt();
    void predictor();
    void solvePoisson();
    void corrector();
    void applyBC();
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

    float maxDivergence() const;
    float maxVelocity() const;

    void saveVTK(int stepNum) const;

    // Gravity potential, phi = g . x, i.e. the hydrostatic pressure. At
    // constant density it is an exact solution of the discrete pressure
    // problem for the body force, so p carries only the reduced pressure and
    // this is added on output. Identically zero when gravity is off. The
    // reference point is the outlet at mid-height, which keeps phi small.
    inline float phiCell(int i, int j) const {
        return gx * ((i + 0.5f - cfg.nx) * dx) +
               gy * ((j + 0.5f - 0.5f * cfg.ny) * dy);
    }

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
