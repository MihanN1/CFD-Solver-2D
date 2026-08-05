#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
#include <vector>
#include <string>
#include <cstdint>

// Projection (fractional step) method on a staggered MAC grid:
//   u*      = u^n + dt * (-(u.grad)u + nu*lap u)   predictor
//   lap p   = div u* / dt                          pressure Poisson
//   u^{n+1} = u* - dt * grad p                     corrector

class Solver {
public:
    Solver(const Config& cfg, const Mesh& mesh);
    // Main loop: run simulation until totalTime
    void run();

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

    // Faces the corrector owns: interior faces with fluid on both sides.
    // Every other face keeps its prescribed value (0 on walls/solids, U0 at the
    // inlet), so the hot loops can multiply by the mask instead of branching.
    std::vector<uint8_t> uFluidMask;
    std::vector<uint8_t> vFluidMask;
    // Same masks as float, so SIMD code can multiply by them directly
    std::vector<float> uFluidMaskF;
    std::vector<float> vFluidMaskF;
    std::vector<uint8_t> solidMask;

    // Cached mesh spacing (mesh.dx/dy never change during a run)
    float dx = 0.0f, dy = 0.0f;
    float invDx = 0.0f, invDy = 0.0f;
    float invDx2 = 0.0f, invDy2 = 0.0f;

    // Helper methods
    void initFields();
    void computeDt();
    void predictor();
    void solvePoisson();
    void corrector();
    void applyBC();
    void buildFaceMasks(); // build masks for u and v to identify fluid faces

    // Diagnostics printed in the progress line
    float maxDivergence() const;
    float maxVelocity() const;

    // VTK output
    void saveVTK(int stepNum) const;

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};
