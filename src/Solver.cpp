#include "Solver.hpp"
#include "AppPaths.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include <array>

namespace {
#ifdef __AVX2__
// Largest of the 8 lanes
float horizontalMax(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_max_ps(lo, hi);
    lo = _mm_max_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_max_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

// Clears the sign bit, i.e. fabs for a whole vector
__m256 absMask() {
    return _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
}
#endif
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

    // Cache the spacing, it never changes during a run
    dx = mesh.dx;
    dy = mesh.dy;
    invDx = 1.0f / dx;
    invDy = 1.0f / dy;
    invDx2 = invDx * invDx;
    invDy2 = invDy * invDy;
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

    std::fill(p.begin(), p.end(), 0.0f);
    std::fill(rhs.begin(), rhs.end(), 0.0f);
    std::fill(u.begin(), u.end(), 0.0f);
    std::fill(v.begin(), v.end(), 0.0f);
    std::fill(u_star.begin(), u_star.end(), 0.0f);
    std::fill(v_star.begin(), v_star.end(), 0.0f);

    // Start every open face at the inlet velocity
    for (int j = 0; j < ny; ++j) {
        for (int i = 1; i < nx; ++i)
            if (uFluidMask[idxU(i, j)])
                u[idxU(i, j)] = cfg.U0;

        if (!solidMask[idxP(0, j)])
            u[idxU(0, j)] = cfg.U0;
        if (!solidMask[idxP(nx - 1, j)])
            u[idxU(nx, j)] = cfg.U0;
    }

    applyBC();

    std::cout << "Fields initialized. Multigrid levels: "
              << multigrid.levelCount()
              << ", backend: " << (multigrid.usingCuda() ? "CUDA" : "CPU")
              << "\n";

    // An axis is only coarsened while its cell count is even, so a grid that is
    // odd in both directions gets no hierarchy and the pressure solve degrades
    // to plain SOR, well, thats shitty cause we tryna optimize shit
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

#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 signMask = absMask();
#endif

    float maxCourant = 0.0f;

    #pragma omp parallel
    {
#ifdef __AVX2__
        __m256 localVec = _mm256_setzero_ps();
#endif
        float localScalar = 0.0f;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j)
        {
            const int rowU = j * (nx + 1);
            const int rowV = j * nx;
            const int rowVTop = (j + 1) * nx;

            int i = 0;
#ifdef __AVX2__
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
#endif

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

#ifdef __AVX2__
        const float localMax = std::max(horizontalMax(localVec), localScalar);
#else
        const float localMax = localScalar;
#endif
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

    // Safety factor covers the velocity growth in between the recomputations
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

#ifdef __AVX2__
    const __m256 zero    = _mm256_setzero_ps();
    const __m256 two     = _mm256_set1_ps(2.f);
    const __m256 quarter = _mm256_set1_ps(0.25f);
    const __m256 invDxVec  = _mm256_set1_ps(invDx);
    const __m256 invDyVec  = _mm256_set1_ps(invDy);
    const __m256 invDx2Vec = _mm256_set1_ps(invDx2);
    const __m256 invDy2Vec = _mm256_set1_ps(invDy2);
    const __m256 dtConvVec = _mm256_set1_ps(dtConv);
    const __m256 dtNuVec   = _mm256_set1_ps(dtNu);
#endif

    // Compute u_star for internal fluid cells (i = 1..nx-1, j = 1..ny-2)
    // u is on vertical faces, so we need to compute convection and diffusion at those points
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
#ifdef __AVX2__
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
            // Multiplying by the mask zeroes the closed faces without a branch
            _mm256_storeu_ps(
                uStarRow + i,
                _mm256_mul_ps(res, _mm256_loadu_ps(uMask + i)));
        }
#endif
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

    // Bottom (j = 0) and top (j = ny-1) rows: the missing neighbour is replaced
    // by the face itself, which is the free-slip condition for u
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

    // Compute v_star similarly
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
#ifdef __AVX2__
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
            // dv/dx with upwind in x
            const __m256 dvdx =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vright, vij), invDxVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vleft), invDxVec),
                    _mm256_cmp_ps(ue, zero, _CMP_GT_OS));
            // dv/dy with upwind in y
            const __m256 dvdy =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vtop, vij), invDyVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vbot), invDyVec),
                    _mm256_cmp_ps(vij, zero, _CMP_GT_OS));
            // Diffusion
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
#endif
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

        // Left (i = 0) and right (i = nx-1) columns, same trick as for u
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

    // Apply BC to u_star and v_star
    // Inlet (left): u_star = U0. Outlet (right): zero gradient (neumann)
    for (int j = 0; j < ny; ++j) {
        uStar[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        uStar[idxU(nx, j)] =
            solidMask[idxP(nx - 1, j)] ?
            0.0f :
            uStar[idxU(nx - 1, j)];
    }
    // Top/Bottom: v = 0 (no vertical flow through the walls)
    for (int i = 0; i < nx; ++i) {
        vStar[idxV(i, 0)] = 0.0f;
        vStar[idxV(i, ny)] = 0.0f;
    }
}

void Solver::solvePoisson() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float invDt = 1.f / dt;
#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 invDtVec = _mm256_set1_ps(invDt);
#endif
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    float* __restrict rhsPtr = rhs.data();

    // rhs = div(u*) / dt
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        int i = 0;
#ifdef __AVX2__
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
            _mm256_storeu_ps(
                rhsPtr + rowP + i,
                _mm256_mul_ps(div, invDtVec));
        }
#endif
        for (; i < nx; ++i){
            const float div =
                (uStar[rowU + i + 1] - uStar[rowU + i]) * invDx +
                (vStar[rowVTop + i] - vStar[rowV + i]) * invDy;

            rhsPtr[rowP + i] = div * invDt;
        }
    }

    const int cycles = std::max(1, cfg.mgIterations);
    lastResidual = multigrid.solve(
        p,
        rhs,
        cfg.smootherOmega,
        cfg.omega,
        cycles,
        cfg.mgTolerance);
}

void Solver::corrector() {
    const int nx = cfg.nx, ny = cfg.ny;
#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 dtVec    = _mm256_set1_ps(dt);
#endif
    const float* __restrict pPtr = p.data();
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    float* __restrict uPtr = u.data();
    float* __restrict vPtr = v.data();

    // Update u: u_new = u_star - dt * (p(i) - p(i-1)) / dx
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        int i = 1;
#ifdef __AVX2__
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
#endif
        for (; i < nx; ++i){
            const float res =
                uStar[rowU + i]
                - dt * (pPtr[rowP + i] - pPtr[rowP + i - 1]) * invDx;

            uPtr[rowU + i] = uFluidMaskF[rowU + i] * res;
        }
    }

    // Update v: v_new = v_star - dt * (p(j) - p(j-1)) / dy
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowPBot = (j - 1) * nx;
        const int rowV = j * nx;
        int i = 0;
#ifdef __AVX2__
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
#endif
        for (; i < nx; ++i){
            const float res =
                vStar[rowV + i]
                - dt * (pPtr[rowP + i] - pPtr[rowPBot + i]) * invDy;

            vPtr[rowV + i] = vFluidMaskF[rowV + i] * res;
        }
    }

    // Outlet face: p = 0 sits half a cell outside, hence the factor 2
    const float outletFactor = 2.f * dt * invDx;
    for (int j = 0; j < ny; ++j) {
        if (solidMask[idxP(nx - 1, j)]) {
            u[idxU(nx, j)] = 0.0f;
        } else {
            u[idxU(nx, j)] =
                u_star[idxU(nx, j)] + outletFactor * p[idxP(nx - 1, j)];
        }
    }

    // Apply boundary conditions again
    applyBC();
}

void Solver::applyBC() {
    const int nx = cfg.nx, ny = cfg.ny;
    // Inlet (left): u = U0
    for (int j = 0; j < ny; ++j)
        u[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;

    // Top/Bottom: free slip (u gradient zero, v = 0)
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

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    currentTime = 0.0;
    step = 0;

    outputPath = resolveOutputDir(cfg.outputDir);
    std::cout << "Writing frames to " << outputPath.string() << "\n";

    // Save initial state
    computeDt();
    std::cout << "Program outputs 'Saved ---' one in ten saves. " << std::endl;
    saveVTK(step);

    const int saveInterval = std::max(1, cfg.saveInterval);
    const int dtUpdateInterval = std::max(1, cfg.dtUpdateInterval);

    while (currentTime < cfg.totalTime) {
        if (step % dtUpdateInterval == 0)
            computeDt();

        // Avoid overshooting totalTime, but keep the CFL dt for the next step
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

        // Progress output
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

        // Save VTK periodically
        if (step % saveInterval == 0)
            saveVTK(step);
    }

    // Final save
    saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s after "
              << step << " steps.\n";
    // Not sure about this, if it works its so cool
}

void Solver::saveVTK(int stepNum) const {
    const int nx = cfg.nx, ny = cfg.ny;
    constexpr size_t BUFFER_WORDS = 4096;
    std::array<uint32_t, BUFFER_WORDS> buffer{};
    size_t bufferPos = 0;

    std::filesystem::path filename =
        outputPath.empty() ? std::filesystem::path(".") : outputPath;
    filename /= "solution_" + std::to_string(stepNum) + ".vtk";

    std::ofstream fout(filename, std::ios::binary);
    if (!fout){
        std::cerr << "Cannot open " << filename.string() << " for writing.\n";
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
            writeFloat(p[row + i] * cfg.ro);
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
    if (stepNum % (std::max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
        std::cout << "Saved " << filename.string() << std::endl;
}
