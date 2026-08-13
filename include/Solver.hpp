#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
#include "Restart.hpp"
#include <vector>
#include <string>
#include <cstdint>

class Solver {
public:
    Solver(const Config& cfg, const Mesh& mesh);
    void run();

    // Seeds the run with a state read from a VTK frame instead of the inlet
    // profile. Must be called before run(). framePrefix is the stem of that
    // frame, so a continuation writes <framePrefix>_1.vtk, _2.vtk and so on
    // and never collides with the series it came from.
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

    // Continuation state. framePrefix is "solution" for a fresh run, so the
    // file names stay solution_<step>.vtk exactly as before.
    bool hasRestartState = false;
    bool needsProjection = false;   // u/v were rebuilt from cell averages
    float restartDt = 0.0f;         // dt that was in flight when the frame was written
    std::string framePrefix = "solution";
    int restartSaveIndex = 0;       // frame counter of the continued run
    // The configuration cannot change mid-run, so its serialized form is
    // built once instead of on every save
    std::string configHeader;

    std::vector<uint8_t> uFluidMask;
    std::vector<uint8_t> vFluidMask;
    std::vector<float> uFluidMaskF;
    std::vector<float> vFluidMaskF;
    std::vector<uint8_t> solidMask;

    float dx = 0.0f, dy = 0.0f;
    float invDx = 0.0f, invDy = 0.0f;
    float invDx2 = 0.0f, invDy2 = 0.0f;

    void initFields();
    void computeDt();
    void predictor();
    void solvePoisson();
    void corrector();
    void applyBC();
    void buildFaceMasks(); // build masks for u and v to identify fluid faces
    void projectRestartState(); // one projection after an approximate restart

    float maxDivergence() const;
    float maxVelocity() const;

    void saveVTK(int stepNum);

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
