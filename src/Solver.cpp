#include "Solver.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
#include <immintrin.h>
#include <array>
#include <limits>
#include <sstream>
#include <utility>

namespace {
// Largest of the 8 lanes
float horizontalMax(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_max_ps(lo, hi);
    lo = _mm_max_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_max_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

// Sum of the 8 lanes
float horizontalSum(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

// Clears the sign bit, i.e. fabs for a whole vector
__m256 absMask() {
    return _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
}
}

Solver::Solver(const Config& cfg, const Mesh& mesh)
    :
    cfg(cfg),
    mesh(mesh),
    multigrid(cfg.nx, cfg.ny, mesh.dx, mesh.dy, cfg.mgMinCoarseSize)
{
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    // Allocate arrays
    p.assign(static_cast<size_t>(nx) * ny, 0.0f);
    rhs.assign(static_cast<size_t>(nx) * ny, 0.0f);
    u.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    v.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    u_star.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    v_star.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);

    dx = mesh.dx;
    dy = mesh.dy;
    invDx = 1.0f / dx;
    invDy = 1.0f / dy;
    invDx2 = invDx * invDx;
    invDy2 = invDy * invDy;
    if (cfg.gravityEnabled) {
        constexpr float degToRad = 3.14159265358979f / 180.0f;
        const float rad = cfg.gravityAngle * degToRad;
        gx = -cfg.gravityAccel * std::sin(rad) + 0.f;
        gy = -cfg.gravityAccel * std::cos(rad) + 0.f;
    }

    outputPath =
        cfg.outputDir.empty() ?
        std::filesystem::path(".") :
        narrowToPath(cfg.outputDir);

    configHeader = "formatVersion=1\n" + cfg.serialize();
}

bool Solver::setInitialState(RestartData&& state,
                             const std::string& prefix) {
    if (state.u.size() != u.size() ||
        state.v.size() != v.size() ||
        state.p.size() != p.size())
        return false;

    u = std::move(state.u);
    v = std::move(state.v);
    p = std::move(state.p);

    // Frames carry the total pressure, the solver works with the reduced one,
    // so the hydrostatic field of the run that wrote the frame comes back off
    // here. Gravity may differ from that run, or be gone; either way what is
    // subtracted is the head the frame was written with.
    if (state.cfg.gravityEnabled) {
        constexpr float degToRad = 3.14159265358979f / 180.0f;
        const float rad = state.cfg.gravityAngle * degToRad;
        const float oldGx = -state.cfg.gravityAccel * std::sin(rad) + 0.f;
        const float oldGy = -state.cfg.gravityAccel * std::cos(rad) + 0.f;
        for (int j = 0; j < cfg.ny; ++j)
            for (int i = 0; i < cfg.nx; ++i)
                p[idxP(i, j)] -=
                    oldGx * ((i + 0.5f - cfg.nx) * dx) +
                    oldGy * ((j + 0.5f - 0.5f * cfg.ny) * dy);
    }

    currentTime = state.currentTime;
    step = state.step;
    restartDt = state.dt;
    needsProjection = !state.exactState;
    hasRestartState = true;
    if (!prefix.empty())
        framePrefix = prefix;

    return true;
}

void Solver::buildFaceMasks(){
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    uFluidMask.assign(static_cast<size_t>(nx + 1) * ny, 0);
    vFluidMask.assign(static_cast<size_t>(nx) * (ny + 1), 0);
    uFluidMaskF.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    vFluidMaskF.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);

    // u faces
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;
        for (int i = 1; i < nx; ++i){
            const bool fluid = !solidMask[row + i] && !solidMask[row + i - 1];
            uFluidMask[idxU(i, j)] = fluid ? 1 : 0;
            uFluidMaskF[idxU(i, j)] = fluid ? 1.0f : 0.0f;
        }
    }

    // v faces
    for (int j = 1; j < ny; ++j){
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i){
            const bool fluid = !solidMask[row + i] && !solidMask[rowBot + i];
            vFluidMask[idxV(i, j)] = fluid ? 1 : 0;
            vFluidMaskF[idxV(i, j)] = fluid ? 1.0f : 0.0f;
        }
    }
}

void Solver::initFields()
{
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    solidMask.assign(static_cast<size_t>(nx) * ny, 0);
    for (int id = 0; id < nx * ny; ++id)
        solidMask[id] = mesh.solid[id] ? 1 : 0;

    buildFaceMasks();
    multigrid.setUseCuda(cfg.useCuda);
    multigrid.setGeometry(solidMask);

    if (hasRestartState && !needsProjection)
        multigrid.skipInitialFullMultigrid();

    std::fill(rhs.begin(), rhs.end(), 0.0f);
    std::fill(u_star.begin(), u_star.end(), 0.0f);
    std::fill(v_star.begin(), v_star.end(), 0.0f);

    if (!hasRestartState) {
        std::fill(p.begin(), p.end(), 0.0f);
        std::fill(u.begin(), u.end(), 0.0f);
        std::fill(v.begin(), v.end(), 0.0f);

        for (int j = 0; j < ny; ++j) {
            for (int i = 1; i < nx; ++i)
                if (uFluidMask[idxU(i, j)])
                    u[idxU(i, j)] = cfg.U0;

            if (!solidMask[idxP(0, j)])
                u[idxU(0, j)] = cfg.U0;
            if (!solidMask[idxP(nx - 1, j)])
                u[idxU(nx, j)] = cfg.U0;
        }
    }

    // Runs in both cases: on a restart this is what applies a changed U0
    applyBC();

    std::cout << "Fields initialized. Multigrid levels: "
              << multigrid.levelCount()
              << ", backend: " << (multigrid.usingCuda() ? "CUDA" : "CPU")
              << "\n";
    if (cfg.U0 > 0.0f && cfg.nu > 0.0f) {
        const float nuNum = 0.5f * cfg.U0 * dx;   // upwind
        const float Lref  = 0.2f * std::min(cfg.Lx, cfg.Ly);
        const float ReSet = cfg.U0 * Lref / cfg.nu;
        const float ReEff = cfg.U0 * Lref / (cfg.nu + nuNum);

        std::cout << "Fluid: rho = " << cfg.ro << " kg/m^3, nu = " << cfg.nu
                  << " m^2/s\n  Re(set) = " << ReSet
                  << ", nu_numerical = " << nuNum
                  << ", Re(effective) = " << ReEff << "\n";

        if (nuNum > cfg.nu)
            std::cout << "  note: upwind adds " << nuNum / cfg.nu
                      << "x more viscosity than the fluid has. The run behaves "
                         "like Re " << ReEff << ", not " << ReSet
                      << ".\n         dx < 2*nu/U0 = " << 2.f * cfg.nu / cfg.U0
                      << " m would fix it, i.e. nx > "
                      << cfg.Lx * cfg.U0 / (2.f * cfg.nu) << ".\n";

        if (ReEff > 3000.f)
            std::cout << "  note: Re(effective) is past the laminar range and "
                         "there is no turbulence model here.\n";
    }
    if (cfg.gravityEnabled) {
        const float head = std::fabs(gx) * cfg.Lx + std::fabs(gy) * cfg.Ly;
        std::cout << "Gravity: " << cfg.gravityAccel << " m/s^2 at "
                  << cfg.gravityAngle << " deg -> g = (" << gx << ", " << gy
                  << ") m/s^2.\n"
                  << "  At constant density this cannot change the velocity "
                     "field; it is absorbed\n"
                     "  into the pressure as a hydrostatic head of "
                  << head * cfg.ro << " Pa across the domain.\n";

        const float dynamic = cfg.U0 * cfg.U0;
        if (dynamic > 0.0f && head > 5.0f * dynamic) {
            std::cout << "  note: that head is " << (head / dynamic)
                      << "x the dynamic scale U0^2, so it dominates the "
                         "pressure map in\n"
                         "  ParaView. It is added on output only and never "
                         "enters the solve, so it\n"
                         "  costs no accuracy; rescale the colour map to see "
                         "the dynamic part.\n";
        }
    }

    if (multigrid.levelCount() < 3 && (nx > 32 || ny > 32)) {
        std::cout << "  note: few multigrid levels for " << nx << "x" << ny
                  << ". Cell counts divisible by a high power of two "
                     "(e.g. 256x128) give the deepest hierarchy and the "
                     "fastest pressure solve.\n";
    }
}

void Solver::computeDt(){
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 signMask = absMask();

    float maxCourant = 0.0f;

    #pragma omp parallel
    {
        __m256 localVec = _mm256_setzero_ps();
        float localScalar = 0.0f;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j)
        {
            const int rowU = j * (nx + 1);
            const int rowV = j * nx;
            const int rowVTop = (j + 1) * nx;

            int i = 0;
            for (; i + 8 <= nx; i += 8)
            {
                const __m256 uL = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(u.data() + rowU + i));
                const __m256 uR = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(u.data() + rowU + i + 1));
                const __m256 vB = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(v.data() + rowV + i));
                const __m256 vT = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(v.data() + rowVTop + i));

                const __m256 courant =
                    _mm256_add_ps(
                        _mm256_mul_ps(_mm256_max_ps(uL, uR), invDxVec),
                        _mm256_mul_ps(_mm256_max_ps(vB, vT), invDyVec));

                localVec = _mm256_max_ps(localVec, courant);
            }

            for (; i < nx; ++i)
            {
                const float maxU = std::max(std::fabs(u[rowU + i]),
                                            std::fabs(u[rowU + i + 1]));
                const float maxV = std::max(std::fabs(v[rowV + i]),
                                            std::fabs(v[rowVTop + i]));
                localScalar = std::max(localScalar,
                                       maxU * invDx + maxV * invDy);
            }
        }

        const float localMax = std::max(horizontalMax(localVec), localScalar);
        #pragma omp critical
        {
            maxCourant = std::max(maxCourant, localMax);
        }
    }

    const float dtAdv =
        (maxCourant < 1e-12f) ?
        1e9f :
        cfg.CFL / maxCourant;

    const float dtDiff =
        (cfg.nu > 0.0f) ?
        1.f / (2.f * cfg.nu * (invDx2 + invDy2)) :
        1e9f;

    dt = cfg.dtSafety * std::min(dtAdv, dtDiff);

    if (!(dt > 0.f) || std::isnan(dt) || std::isinf(dt))
        dt = 1e-6f;
}

void Solver::predictor() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float dtNu = dt * cfg.nu;
    const float dtConv = dt;
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float* __restrict uStar = u_star.data();
    float* __restrict vStar = v_star.data();

    const __m256 zero    = _mm256_setzero_ps();
    const __m256 two     = _mm256_set1_ps(2.f);
    const __m256 quarter = _mm256_set1_ps(0.25f);
    const __m256 invDxVec  = _mm256_set1_ps(invDx);
    const __m256 invDyVec  = _mm256_set1_ps(invDy);
    const __m256 invDx2Vec = _mm256_set1_ps(invDx2);
    const __m256 invDy2Vec = _mm256_set1_ps(invDy2);
    const __m256 dtConvVec = _mm256_set1_ps(dtConv);
    const __m256 dtNuVec   = _mm256_set1_ps(dtNu);

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny - 1; ++j) {
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uTop = uPtr + (j + 1) * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict uMask = uFluidMaskF.data() + rowU;
        float* __restrict uStarRow = uStar + rowU;
        int i = 1;
        for (; i + 8 <= nx; i += 8) {
            const __m256 uij =
                _mm256_loadu_ps(uRow + i);
            const __m256 utop =
                _mm256_loadu_ps(uTop + i);
            const __m256 ubot =
                _mm256_loadu_ps(uBot + i);
            const __m256 uleft =
                _mm256_loadu_ps(uRow + i - 1);
            const __m256 uright =
                _mm256_loadu_ps(uRow + i + 1);
            // Convection: upwind for u
            const __m256 vn =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(
                            _mm256_loadu_ps(vTop + i - 1),
                            _mm256_loadu_ps(vTop + i)),
                        _mm256_add_ps(
                            _mm256_loadu_ps(vRow + i - 1),
                            _mm256_loadu_ps(vRow + i))),
                    quarter);
            const __m256 dudx =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(uright, uij), invDxVec),
                    _mm256_mul_ps(_mm256_sub_ps(uij, uleft), invDxVec),
                    _mm256_cmp_ps(uij, zero, _CMP_GT_OS));
            const __m256 dudy =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(utop, uij), invDyVec),
                    _mm256_mul_ps(_mm256_sub_ps(uij, ubot), invDyVec),
                    _mm256_cmp_ps(vn, zero, _CMP_GT_OS));
            // Diffusion: central differences
            const __m256 d2udx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        uright,
                        _mm256_sub_ps(
                            uleft,
                            _mm256_mul_ps(two, uij))),
                    invDx2Vec);
            const __m256 d2udy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        utop,
                        _mm256_sub_ps(
                            ubot,
                            _mm256_mul_ps(two, uij))),
                    invDy2Vec);
            const __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(uij, dudx),
                    _mm256_mul_ps(vn, dudy));
            const __m256 diff =
                _mm256_add_ps(
                    d2udx2,
                    d2udy2);
            const __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        uij,
                        _mm256_mul_ps(dtConvVec, conv)),
                    _mm256_mul_ps(dtNuVec, diff));
            _mm256_storeu_ps(
                uStarRow + i,
                _mm256_mul_ps(res, _mm256_loadu_ps(uMask + i)));
        }
        for (; i < nx; ++i) {
            const float u_ij = uRow[i];
            const float v_n = 0.25f * (
                vTop[i-1] +
                vTop[i] +
                vRow[i-1] +
                vRow[i]);

            const float u_left  = uRow[i-1];
            const float u_right = uRow[i+1];
            const float u_bot   = uBot[i];
            const float u_top   = uTop[i];

            const float dudx =
                (u_ij > 0.f) ?
                (u_ij - u_left) * invDx :
                (u_right - u_ij) * invDx;

            const float dudy =
                (v_n > 0.f) ?
                (u_ij - u_bot) * invDy :
                (u_top - u_ij) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + dtNu * (d2udx2 + d2udy2));
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        if (ny < 2)
            break;
        const int j = (pass == 0) ? 0 : ny - 1;
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uOther =
            uPtr + ((pass == 0) ? (nx + 1) : (j - 1) * (nx + 1));
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict uMask = uFluidMaskF.data() + rowU;
        float* __restrict uStarRow = uStar + rowU;
        for (int i = 1; i < nx; ++i) {
            const float u_ij = uRow[i];
            const float v_n = 0.25f * (
                vTop[i-1] +
                vTop[i] +
                vRow[i-1] +
                vRow[i]);

            const float u_left  = uRow[i-1];
            const float u_right = uRow[i+1];
            const float u_bot = (pass == 0) ? u_ij : uOther[i];
            const float u_top = (pass == 0) ? uOther[i] : u_ij;

            const float dudx =
                (u_ij > 0.f) ?
                (u_ij - u_left) * invDx :
                (u_right - u_ij) * invDx;

            const float dudy =
                (v_n > 0.f) ?
                (u_ij - u_bot) * invDy :
                (u_top - u_ij) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + dtNu * (d2udx2 + d2udy2));
        }
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j){ // internal horizontal faces
        const int rowV = j * nx;
        const float* __restrict vRow = vPtr + rowV;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict vBot = vPtr + (j - 1) * nx;
        const float* __restrict uRow = uPtr + j * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict vMask = vFluidMaskF.data() + rowV;
        float* __restrict vStarRow = vStar + rowV;
        int i = 1;
        for (; i + 8 <= nx - 1; i += 8) {
            const __m256 vij =
                _mm256_loadu_ps(vRow + i);
            const __m256 vtop =
                _mm256_loadu_ps(vTop + i);
            const __m256 vbot =
                _mm256_loadu_ps(vBot + i);
            const __m256 vleft =
                _mm256_loadu_ps(vRow + i - 1);
            const __m256 vright =
                _mm256_loadu_ps(vRow + i + 1);
            const __m256 ue =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(
                            _mm256_loadu_ps(uRow + i),
                            _mm256_loadu_ps(uRow + i + 1)),
                        _mm256_add_ps(
                            _mm256_loadu_ps(uBot + i),
                            _mm256_loadu_ps(uBot + i + 1))),
                    quarter);
            const __m256 dvdx =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vright, vij), invDxVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vleft), invDxVec),
                    _mm256_cmp_ps(ue, zero, _CMP_GT_OS));
            const __m256 dvdy =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vtop, vij), invDyVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vbot), invDyVec),
                    _mm256_cmp_ps(vij, zero, _CMP_GT_OS));
            const __m256 d2vdx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vright,
                        _mm256_sub_ps(
                            vleft,
                            _mm256_mul_ps(two, vij))),
                    invDx2Vec);
            const __m256 d2vdy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vtop,
                        _mm256_sub_ps(
                            vbot,
                            _mm256_mul_ps(two, vij))),
                    invDy2Vec);
            const __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(ue, dvdx),
                    _mm256_mul_ps(vij, dvdy));
            const __m256 diff =
                _mm256_add_ps(
                    d2vdx2,
                    d2vdy2);
            const __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        vij,
                        _mm256_mul_ps(dtConvVec, conv)),
                    _mm256_mul_ps(dtNuVec, diff));
            _mm256_storeu_ps(
                vStarRow + i,
                _mm256_mul_ps(res, _mm256_loadu_ps(vMask + i)));
        }
        for (; i < nx - 1; ++i) {
            const float v_ij = vRow[i];
            const float u_e = 0.25f * (
                uRow[i] +
                uRow[i+1] +
                uBot[i] +
                uBot[i+1]);

            const float v_left  = vRow[i-1];
            const float v_right = vRow[i+1];
            const float v_bot   = vBot[i];
            const float v_top   = vTop[i];

            const float dvdx =
                (u_e > 0.f) ?
                (v_ij - v_left) * invDx :
                (v_right - v_ij) * invDx;

            const float dvdy =
                (v_ij > 0.f) ?
                (v_ij - v_bot) * invDy :
                (v_top - v_ij) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[i] = vMask[i] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + dtNu * (d2vdx2 + d2vdy2));
        }

        for (int pass = 0; pass < 2; ++pass) {
            const int iCol = (pass == 0) ? 0 : nx - 1;
            if (iCol < 0 || (pass == 1 && nx < 2))
                continue;
            const float v_ij = vRow[iCol];
            const float u_e = 0.25f * (
                uRow[iCol] +
                uRow[iCol+1] +
                uBot[iCol] +
                uBot[iCol+1]);

            const float v_left  = (iCol == 0)      ? v_ij : vRow[iCol-1];
            const float v_right = (iCol == nx - 1) ? v_ij : vRow[iCol+1];
            const float v_bot   = vBot[iCol];
            const float v_top   = vTop[iCol];

            const float dvdx =
                (u_e > 0.f) ?
                (v_ij - v_left) * invDx :
                (v_right - v_ij) * invDx;

            const float dvdy =
                (v_ij > 0.f) ?
                (v_ij - v_bot) * invDy :
                (v_top - v_ij) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[iCol] = vMask[iCol] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + dtNu * (d2vdx2 + d2vdy2));
        }
    }

    for (int j = 0; j < ny; ++j) {
        uStar[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        uStar[idxU(nx, j)] =
            solidMask[idxP(nx - 1, j)] ?
            0.0f :
            uStar[idxU(nx - 1, j)];
    }
    for (int i = 0; i < nx; ++i) {
        vStar[idxV(i, 0)] = 0.0f;
        vStar[idxV(i, ny)] = 0.0f;
    }
}

void Solver::solvePoisson() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float invDt = 1.f / dt;
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 invDtVec = _mm256_set1_ps(invDt);
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    float* __restrict rhsPtr = rhs.data();

    double rhsSqSum = 0.0;

    #pragma omp parallel for schedule(static) reduction(+ : rhsSqSum)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        __m256 sqAcc = _mm256_setzero_ps();
        int i = 0;
        for (; i + 8 <= nx; i += 8){
            const __m256 uR =
                _mm256_loadu_ps(uStar + rowU + i + 1);
            const __m256 uL =
                _mm256_loadu_ps(uStar + rowU + i);
            const __m256 vT =
                _mm256_loadu_ps(vStar + rowVTop + i);
            const __m256 vB =
                _mm256_loadu_ps(vStar + rowV + i);
            const __m256 div =
                _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_sub_ps(uR, uL),
                        invDxVec),
                    _mm256_mul_ps(
                        _mm256_sub_ps(vT, vB),
                        invDyVec));
            const __m256 val = _mm256_mul_ps(div, invDtVec);
            _mm256_storeu_ps(rhsPtr + rowP + i, val);
            sqAcc = _mm256_add_ps(sqAcc, _mm256_mul_ps(val, val));
        }
        float rowSum = horizontalSum(sqAcc);
        for (; i < nx; ++i){
            const float div =
                (uStar[rowU + i + 1] - uStar[rowU + i]) * invDx +
                (vStar[rowVTop + i] - vStar[rowV + i]) * invDy;

            const float val = div * invDt;
            rhsPtr[rowP + i] = val;
            rowSum += val * val;
        }
        rhsSqSum += double(rowSum);
    }

    const float rhsNorm = float(std::sqrt(rhsSqSum));

    const int cycles = std::max(1, cfg.mgIterations);
    lastResidual = multigrid.solve(
        p,
        rhs,
        cfg.smootherOmega,
        cfg.omega,
        cycles,
        cfg.mgTolerance,
        rhsNorm);
}

void Solver::corrector() {
    const int nx = cfg.nx, ny = cfg.ny;
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 dtVec    = _mm256_set1_ps(dt);
    const float* __restrict pPtr = p.data();
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    float* __restrict uPtr = u.data();
    float* __restrict vPtr = v.data();

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        int i = 1;
        for (; i + 8 <= nx; i += 8){
            const __m256 pRight =
                _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pLeft =
                _mm256_loadu_ps(pPtr + rowP + i - 1);
            const __m256 uS =
                _mm256_loadu_ps(uStar + rowU + i);
            const __m256 dpdx =
                _mm256_mul_ps(
                    _mm256_sub_ps(pRight, pLeft),
                    invDxVec);
            const __m256 res =
                _mm256_sub_ps(
                    uS,
                    _mm256_mul_ps(dtVec, dpdx));
            _mm256_storeu_ps(
                uPtr + rowU + i,
                _mm256_mul_ps(
                    res,
                    _mm256_loadu_ps(uFluidMaskF.data() + rowU + i)));
        }
        for (; i < nx; ++i){
            const float res =
                uStar[rowU + i]
                - dt * (pPtr[rowP + i] - pPtr[rowP + i - 1]) * invDx;

            uPtr[rowU + i] = uFluidMaskF[rowU + i] * res;
        }
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowPBot = (j - 1) * nx;
        const int rowV = j * nx;
        int i = 0;
        for (; i + 8 <= nx; i += 8){
            const __m256 pTop =
                _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pBot =
                _mm256_loadu_ps(pPtr + rowPBot + i);
            const __m256 vS =
                _mm256_loadu_ps(vStar + rowV + i);
            const __m256 dpdy =
                _mm256_mul_ps(
                    _mm256_sub_ps(pTop, pBot),
                    invDyVec);
            const __m256 res =
                _mm256_sub_ps(
                    vS,
                    _mm256_mul_ps(dtVec, dpdy));
            _mm256_storeu_ps(
                vPtr + rowV + i,
                _mm256_mul_ps(
                    res,
                    _mm256_loadu_ps(vFluidMaskF.data() + rowV + i)));
        }
        for (; i < nx; ++i){
            const float res =
                vStar[rowV + i]
                - dt * (pPtr[rowP + i] - pPtr[rowPBot + i]) * invDy;

            vPtr[rowV + i] = vFluidMaskF[rowV + i] * res;
        }
    }

    const float outletFactor = 2.f * dt * invDx;
    for (int j = 0; j < ny; ++j) {
        if (solidMask[idxP(nx - 1, j)]) {
            u[idxU(nx, j)] = 0.0f;
        } else {
            u[idxU(nx, j)] =
                u_star[idxU(nx, j)] +
                outletFactor * p[idxP(nx - 1, j)];
        }
    }

    applyBC();
}

void Solver::applyBC() {
    const int nx = cfg.nx, ny = cfg.ny;
    for (int j = 0; j < ny; ++j)
        u[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;

    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0f;
        v[idxV(i, ny)] = 0.0f;
    }
}

float Solver::maxDivergence() const {
    const int nx = cfg.nx, ny = cfg.ny;
    float worst = 0.0f;
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i) {
            if (solidMask[rowP + i])
                continue;
            const float div =
                (u[rowU + i + 1] - u[rowU + i]) * invDx +
                (v[rowVTop + i] - v[rowV + i]) * invDy;
            worst = std::max(worst, std::fabs(div));
        }
    }
    return worst;
}

float Solver::maxVelocity() const {
    float maxVel = 0.0f;
    for (float value : u) maxVel = std::max(maxVel, std::fabs(value));
    for (float value : v) maxVel = std::max(maxVel, std::fabs(value));
    return maxVel;
}

void Solver::projectRestartState() {
    const int nx = cfg.nx, ny = cfg.ny;

    u_star = u;
    v_star = v;

    for (int j = 0; j < ny; ++j) {
        u_star[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        u_star[idxU(nx, j)] =
            solidMask[idxP(nx - 1, j)] ?
            0.0f :
            u_star[idxU(nx - 1, j)];
    }
    for (int i = 0; i < nx; ++i) {
        v_star[idxV(i, 0)] = 0.0f;
        v_star[idxV(i, ny)] = 0.0f;
    }

    solvePoisson();
    corrector();

    std::cout << "  reconstructed state projected, div = "
              << maxDivergence() << "\n";
}

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    if (!hasRestartState) {
        currentTime = 0.0;
        step = 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputPath, ec);

    if (hasRestartState && restartDt > 0.0f && !needsProjection) {
        dt = restartDt;
    } else {
        computeDt();
    }
    std::cout << "Program outputs 'Saved ---' one in ten saves. " << std::endl;
    if (hasRestartState) {
        if (needsProjection)
            projectRestartState();
        std::cout << "Continuing from t = " << currentTime
                  << " s, step " << step
                  << ". Frames go to " << framePrefix << "_<step>.vtk\n";
    } else {
        saveVTK(step);
    }

    const int saveInterval = std::max(1, cfg.saveInterval);
    const int dtUpdateInterval = std::max(1, cfg.dtUpdateInterval);

    while (currentTime < cfg.totalTime) {
        if (step % dtUpdateInterval == 0)
            computeDt();

        float stepDt = dt;
        if (currentTime + stepDt > cfg.totalTime)
            stepDt = static_cast<float>(cfg.totalTime - currentTime);
        if (!(stepDt > 0.f))
            break;
        const float savedDt = dt;
        dt = stepDt;

        predictor();
        solvePoisson();
        corrector();

        currentTime += dt;
        step++;
        dt = savedDt;

        if (step % 10 == 0) {
            const float maxVel = maxVelocity();
            std::cout << "Step " << step
                      << ", t = " << currentTime
                      << " s, dt = " << stepDt
                      << ", |u|max = " << maxVel
                      << ", div = " << maxDivergence()
                      << ", mg res = " << lastResidual
                      << " (" << multigrid.cyclesUsed() << " cycles)"
                      << std::endl;
            if (std::isnan(maxVel) || std::isinf(maxVel)) {
                std::cerr << "Solution diverged at step " << step
                          << "; aborting.\n";
                break;
            }
        }

        if (step % saveInterval == 0)
            saveVTK(step);
    }

    if (step % saveInterval != 0)
        saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s after "
              << step << " steps.\n";
}

void Solver::saveVTK(int stepNum) const {
    const int nx = cfg.nx, ny = cfg.ny;
    constexpr size_t BUFFER_WORDS = 4096;
    std::array<uint32_t, BUFFER_WORDS> buffer;
    size_t bufferPos = 0;
    std::filesystem::path filename = outputPath;
    filename /= framePrefix + "_" + std::to_string(stepNum) + ".vtk";

    std::ofstream fout(filename, std::ios::binary);
    if (!fout){
        std::cerr << "Cannot open " << pathToConsole(filename) << " for writing.\n";
        return;
    }
    fout
        << "# vtk DataFile Version 3.0\n"
        << "CFD-Solver-2D output, step " << stepNum << "\n"
        << "BINARY\n"
        << "DATASET STRUCTURED_POINTS\n"
        << "DIMENSIONS "
        << nx + 1 << " "
        << ny + 1 << " 1\n"
        << "ORIGIN 0 0 0\n"
        << "SPACING "
        << dx << " "
        << dy << " 1\n"
        << "CELL_DATA "
        << nx * ny << "\n";

    auto flushFloatBuffer = [&](){
        if (bufferPos == 0)
            return;
        fout.write(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(bufferPos * sizeof(uint32_t)));
        bufferPos = 0;
    };
    // Legacy VTK binary data is big endian, so every word is byte-swapped
    auto writeWord = [&](uint32_t x){
        buffer[bufferPos++] =
            ((x & 0x000000FFu) << 24) |
            ((x & 0x0000FF00u) << 8 ) |
            ((x & 0x00FF0000u) >> 8 ) |
            ((x & 0xFF000000u) >> 24);
        if (bufferPos == BUFFER_WORDS)
            flushFloatBuffer();
    };
    auto writeFloat = [&](float value){
        uint32_t x;
        std::memcpy(&x, &value, sizeof(float));
        writeWord(x);
    };
    auto writeInt = [&](int32_t value){
        uint32_t x;
        std::memcpy(&x, &value, sizeof(int32_t));
        writeWord(x);
    };

    fout << "SCALARS pressure float 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i){
            // p holds the reduced pressure, so the hydrostatic field is put
            // back here and nowhere else. Solid cells have a zero diagonal and
            // never take part in the solve, so they keep the hydrostatic value
            // alone instead of punching a hole through the pressure map.
            const float value =
                phiCell(i, j) + (solidMask[row + i] ? 0.0f : p[row + i]);
            writeFloat(value * cfg.ro);
        }
    }
    flushFloatBuffer();
    fout << "\n";

    fout << "SCALARS solid int 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i)
        {
            writeInt(static_cast<int32_t>(solidMask[row + i]));
        }
    }
    flushFloatBuffer();
    fout << "\n";

    fout << "VECTORS velocity float\n";
    for (int j = 0; j < ny; ++j){
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i){
            float uu =
                0.5f * (u[rowU + i] + u[rowU + i + 1]);
            float vv =
                0.5f * (v[rowV + i] + v[rowVTop + i]);
            writeFloat(uu);
            writeFloat(vv);
            writeFloat(0.0f);
        }
    }
    flushFloatBuffer();
    fout << "\n";

    std::ostringstream state;
    state << std::setprecision(std::numeric_limits<double>::max_digits10)
          << "restartTime=" << currentTime << "\n"
          << "restartStep=" << stepNum << "\n"
          << std::setprecision(std::numeric_limits<float>::max_digits10)
          << "restartDt=" << dt << "\n";
    const std::string configText = configHeader + state.str();

    const size_t uCount = static_cast<size_t>(nx + 1) * ny;
    const size_t vCount = static_cast<size_t>(nx) * (ny + 1);
    const size_t pCount = static_cast<size_t>(nx) * ny;

    fout << "FIELD RestartData 4\n";
    fout << "configText 1 " << configText.size() << " char\n";
    fout.write(configText.data(),
               static_cast<std::streamsize>(configText.size()));
    fout << "\n";

    fout << "uFace 1 " << uCount << " float\n";
    for (size_t id = 0; id < uCount; ++id)
        writeFloat(u[id]);
    flushFloatBuffer();
    fout << "\n";

    fout << "vFace 1 " << vCount << " float\n";
    for (size_t id = 0; id < vCount; ++id)
        writeFloat(v[id]);
    flushFloatBuffer();
    fout << "\n";

    // Written as the total pressure, which is what every frame ever written
    // holds, so old frames keep restarting and setInitialState has one rule.
    fout << "pRaw 1 " << pCount << " float\n";
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            writeFloat(p[j * nx + i] + phiCell(i, j));
    flushFloatBuffer();
    fout << "\n";

    if (stepNum % (std::max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
        std::cout << "Saved " << pathToConsole(filename) << std::endl;
}
