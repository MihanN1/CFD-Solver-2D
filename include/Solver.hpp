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
    void buildFaceMasks(); // build masks for u and v to identify fluid faces
    void projectRestartState(); // one projection after an approximate restart

    float maxDivergence() const;
    float maxVelocity() const;

    void saveVTK(int stepNum) const;

    // Gravity potential, phi = g . x, i.e. the hydrostatic pressure. Its
    // reference point sits at the outlet at mid-height, which is what makes
    // phiOutlet independent of gx and keeps the printed numbers small.
    // Both are identically zero when gravity is off.
    inline float phiCell(int i, int j) const {
        return gx * ((i + 0.5f) * dx - cfg.Lx) +
               gy * ((j + 0.5f) * dy - 0.5f * cfg.Ly);
    }
    // The pressure the outlet Dirichlet has to hold instead of zero: phi on
    // the outlet face, where the gx term vanishes by choice of reference.
    inline float phiOutlet(int j) const {
        return gy * ((j + 0.5f) * dy - 0.5f * cfg.Ly);
    }

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
