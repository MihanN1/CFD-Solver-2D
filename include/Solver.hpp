#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Multigrid.hpp"
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Projection (fractional step) solver on a staggered MAC grid.
//
//   u*      = u^n + dt * ( -(u.grad)u + nu * lap u )      predictor
//   lap p   = div u* / dt                                 pressure Poisson
//   u^{n+1} = u* - dt * grad p                            corrector
//
// The three steps share one discrete divergence and one discrete gradient; see
// Multigrid.hpp for why that matters and how the boundary conditions are folded
// into the Poisson stencil instead of being patched on afterwards.
//
// Face bookkeeping
// ----------------
//   u lives on vertical faces,   index i = 0..nx, j = 0..ny-1
//   v lives on horizontal faces, index i = 0..nx-1, j = 0..ny
//
//   i = 0    inlet    u = U0        prescribed, not corrected
//   i = nx   outlet   p = 0 on the face, u IS corrected through the ghost
//   j = 0    wall     v = 0         prescribed, u free slip
//   j = ny   wall     v = 0         prescribed, u free slip
//
// uOpen / vOpen mark the faces the corrector owns: interior faces with fluid on
// both sides. Velocities on every other face are held at their prescribed value
// (0 for solid faces and walls, U0 at the inlet), which is what lets the hot
// loops run completely branch free - a stencil can only ever read a legitimate
// zero, never uninitialised data.
// ---------------------------------------------------------------------------

class Solver {
public:
    Solver(const Config& cfg, const Mesh& mesh);
    void run();

private:
    const Config& cfg;
    const Mesh& mesh;
    Multigrid multigrid;

    std::vector<float> p;
    std::vector<float> rhs;
    std::vector<float> u, u_star;
    std::vector<float> v, v_star;

    double currentTime = 0.0;
    int    step = 0;
    float  dt = 0.0f;
    float  lastResidual = 0.0f;

    std::vector<uint8_t> uOpen, vOpen;
    std::vector<float>   uOpenF, vOpenF;
    std::vector<uint8_t> solidMask;

    float dx = 0.0f, dy = 0.0f;
    float invDx = 0.0f, invDy = 0.0f;
    float invDx2 = 0.0f, invDy2 = 0.0f;

    void initFields();
    void buildFaceMasks();
    void computeDt();
    void predictor();
    void solvePoisson();
    void corrector();
    void applyBC();

    float maxDivergence() const;
    float maxVelocity() const;

    void saveVTK(int stepNum) const;

    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; }
};