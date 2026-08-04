#include "Solver.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <iomanip>
#include <iostream>

namespace {

inline float horizontalMax(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_max_ps(lo, hi);
    lo = _mm_max_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_max_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

inline __m256 absMask() {
    return _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
}

}
Solver::Solver(const Config& cfg, const Mesh& mesh)
    : cfg(cfg),
      mesh(mesh),
      multigrid(cfg.nx, cfg.ny, mesh.dx, mesh.dy, cfg.mgMinCoarseSize)
{
    const int nx = cfg.nx;
    const int ny = cfg.ny;

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
}

void Solver::buildFaceMasks() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    uOpen.assign(static_cast<size_t>(nx + 1) * ny, 0u);
    vOpen.assign(static_cast<size_t>(nx) * (ny + 1), 0u);
    uOpenF.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    vOpenF.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 1; i < nx; ++i) {
            const bool open = !solidMask[row + i] && !solidMask[row + i - 1];
            uOpen[idxU(i, j)]  = open ? 1u : 0u;
            uOpenF[idxU(i, j)] = open ? 1.0f : 0.0f;
        }
    }
    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            const bool open = !solidMask[row + i] && !solidMask[rowBot + i];
            vOpen[idxV(i, j)]  = open ? 1u : 0u;
            vOpenF[idxV(i, j)] = open ? 1.0f : 0.0f;
        }
    }
}

void Solver::initFields() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    solidMask.assign(static_cast<size_t>(nx) * ny, 0u);
    for (int id = 0; id < nx * ny; ++id)
        solidMask[id] = mesh.solid[id] ? 1u : 0u;

    buildFaceMasks();
    multigrid.setUseCuda(cfg.useCuda);
    multigrid.setGeometry(solidMask);

    std::fill(p.begin(), p.end(), 0.0f);
    std::fill(rhs.begin(), rhs.end(), 0.0f);
    std::fill(u.begin(), u.end(), 0.0f);
    std::fill(v.begin(), v.end(), 0.0f);
    std::fill(u_star.begin(), u_star.end(), 0.0f);
    std::fill(v_star.begin(), v_star.end(), 0.0f);

    for (int j = 0; j < ny; ++j) {
        for (int i = 1; i < nx; ++i)
            if (uOpen[idxU(i, j)])
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

    // An axis is only coarsened while its cell count is even, because an odd
    // count would leave the last coarse cell covering a single fine cell and
    // break the consistency between restriction and prolongation. A grid that
    // is odd in both directions therefore gets no hierarchy at all and the
    // pressure solve degrades to plain SOR, well, thats shitty cause we tryna optimize shit
    if (multigrid.levelCount() < 3 && (nx > 32 || ny > 32)) {
        std::cout << "  note: few multigrid levels for " << nx << "x" << ny
                  << ". Cell counts divisible by a high power of two "
                     "(e.g. 256x128) give the deepest hierarchy and the "
                     "fastest pressure solve.\n";
    }
}

void Solver::computeDt() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    const __m256 invDxV = _mm256_set1_ps(invDx);
    const __m256 invDyV = _mm256_set1_ps(invDy);
    const __m256 kAbsMask = absMask();

    float maxCourant = 0.0f;

    #pragma omp parallel
    {
        __m256 localVec = _mm256_setzero_ps();
        float localScalar = 0.0f;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j) {
            const int rowU = j * (nx + 1);
            const int rowV = j * nx;
            const int rowVTop = (j + 1) * nx;

            int i = 0;
            for (; i + 8 <= nx; i += 8) {
                const __m256 uL = _mm256_and_ps(kAbsMask, _mm256_loadu_ps(u.data() + rowU + i));
                const __m256 uR = _mm256_and_ps(kAbsMask, _mm256_loadu_ps(u.data() + rowU + i + 1));
                const __m256 vB = _mm256_and_ps(kAbsMask, _mm256_loadu_ps(v.data() + rowV + i));
                const __m256 vT = _mm256_and_ps(kAbsMask, _mm256_loadu_ps(v.data() + rowVTop + i));

                const __m256 c = _mm256_add_ps(
                    _mm256_mul_ps(_mm256_max_ps(uL, uR), invDxV),
                    _mm256_mul_ps(_mm256_max_ps(vB, vT), invDyV));

                localVec = _mm256_max_ps(localVec, c);
            }

            for (; i < nx; ++i) {
                const float uu = std::max(std::fabs(u[rowU + i]),
                                          std::fabs(u[rowU + i + 1]));
                const float vv = std::max(std::fabs(v[rowV + i]),
                                          std::fabs(v[rowVTop + i]));
                localScalar = std::max(localScalar, uu * invDx + vv * invDy);
            }
        }

        const float localMax = std::max(horizontalMax(localVec), localScalar);
        #pragma omp critical
        {
            maxCourant = std::max(maxCourant, localMax);
        }
    }

    const float dtAdv = (maxCourant < 1e-12f) ? 1e9f : (cfg.CFL / maxCourant);
    const float dtDiff = (cfg.nu > 0.0f)
        ? 1.0f / (2.0f * cfg.nu * (invDx2 + invDy2))
        : 1e9f;
    // safety factor covers the velocity growth in between.
    dt = cfg.dtSafety * std::min(dtAdv, dtDiff);

    if (!(dt > 0.0f) || std::isnan(dt) || std::isinf(dt))
        dt = 1e-6f;
}

void Solver::predictor() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    const float dtNu = dt * cfg.nu;
    const float dtConv = dt;
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float* __restrict uStar = u_star.data();
    float* __restrict vStar = v_star.data();
    const __m256 zero    = _mm256_setzero_ps();
    const __m256 two     = _mm256_set1_ps(2.0f);
    const __m256 quarter = _mm256_set1_ps(0.25f);
    const __m256 invDxV  = _mm256_set1_ps(invDx);
    const __m256 invDyV  = _mm256_set1_ps(invDy);
    const __m256 invDx2V = _mm256_set1_ps(invDx2);
    const __m256 invDy2V = _mm256_set1_ps(invDy2);
    const __m256 dtConvV = _mm256_set1_ps(dtConv);
    const __m256 dtNuV   = _mm256_set1_ps(dtNu);
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny - 1; ++j) {
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uTop = uPtr + (j + 1) * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict maskRow = uOpenF.data() + rowU;
        float* __restrict outRow = uStar + rowU;
        int i = 1;
        for (; i + 8 <= nx; i += 8) {
            const __m256 uij   = _mm256_loadu_ps(uRow + i);
            const __m256 utop  = _mm256_loadu_ps(uTop + i);
            const __m256 ubot  = _mm256_loadu_ps(uBot + i);
            const __m256 uleft = _mm256_loadu_ps(uRow + i - 1);
            const __m256 urght = _mm256_loadu_ps(uRow + i + 1);
            const __m256 vn = _mm256_mul_ps(
                _mm256_add_ps(
                    _mm256_add_ps(_mm256_loadu_ps(vTop + i - 1), _mm256_loadu_ps(vTop + i)),
                    _mm256_add_ps(_mm256_loadu_ps(vRow + i - 1), _mm256_loadu_ps(vRow + i))),
                quarter);
            const __m256 dudx = _mm256_blendv_ps(
                _mm256_mul_ps(_mm256_sub_ps(urght, uij), invDxV),
                _mm256_mul_ps(_mm256_sub_ps(uij, uleft), invDxV),
                _mm256_cmp_ps(uij, zero, _CMP_GT_OS));
            const __m256 dudy = _mm256_blendv_ps(
                _mm256_mul_ps(_mm256_sub_ps(utop, uij), invDyV),
                _mm256_mul_ps(_mm256_sub_ps(uij, ubot), invDyV),
                _mm256_cmp_ps(vn, zero, _CMP_GT_OS));
            const __m256 d2x = _mm256_mul_ps(
                _mm256_add_ps(urght, _mm256_sub_ps(uleft, _mm256_mul_ps(two, uij))), invDx2V);
            const __m256 d2y = _mm256_mul_ps(
                _mm256_add_ps(utop, _mm256_sub_ps(ubot, _mm256_mul_ps(two, uij))), invDy2V);
            const __m256 conv = _mm256_add_ps(_mm256_mul_ps(uij, dudx),
                                              _mm256_mul_ps(vn, dudy));
            const __m256 res = _mm256_add_ps(
                _mm256_sub_ps(uij, _mm256_mul_ps(dtConvV, conv)),
                _mm256_mul_ps(dtNuV, _mm256_add_ps(d2x, d2y)));
            _mm256_storeu_ps(outRow + i,
                             _mm256_mul_ps(res, _mm256_loadu_ps(maskRow + i)));
        }
        for (; i < nx; ++i) {
            const float uij = uRow[i];
            const float vn = 0.25f * (vTop[i - 1] + vTop[i] + vRow[i - 1] + vRow[i]);
            const float ul = uRow[i - 1];
            const float ur = uRow[i + 1];
            const float ub = uBot[i];
            const float ut = uTop[i];
            const float dudx = (uij > 0.0f) ? (uij - ul) * invDx : (ur - uij) * invDx;
            const float dudy = (vn > 0.0f) ? (uij - ub) * invDy : (ut - uij) * invDy;
            const float d2x = (ur - 2.0f * uij + ul) * invDx2;
            const float d2y = (ut - 2.0f * uij + ub) * invDy2;
            outRow[i] = maskRow[i] *
                (uij - dtConv * (uij * dudx + vn * dudy) + dtNu * (d2x + d2y));
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        if (ny < 2)
            break;
        const int j = (pass == 0) ? 0 : ny - 1;
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uOther = uPtr + ((pass == 0) ? (nx + 1) : (j - 1) * (nx + 1));
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict maskRow = uOpenF.data() + rowU;
        float* __restrict outRow = uStar + rowU;
        for (int i = 1; i < nx; ++i) {
            const float uij = uRow[i];
            const float vn = 0.25f * (vTop[i - 1] + vTop[i] + vRow[i - 1] + vRow[i]);
            const float ul = uRow[i - 1];
            const float ur = uRow[i + 1];
            const float ub = (pass == 0) ? uij : uOther[i];
            const float ut = (pass == 0) ? uOther[i] : uij;
            const float dudx = (uij > 0.0f) ? (uij - ul) * invDx : (ur - uij) * invDx;
            const float dudy = (vn > 0.0f) ? (uij - ub) * invDy : (ut - uij) * invDy;
            const float d2x = (ur - 2.0f * uij + ul) * invDx2;
            const float d2y = (ut - 2.0f * uij + ub) * invDy2;
            outRow[i] = maskRow[i] *
                (uij - dtConv * (uij * dudx + vn * dudy) + dtNu * (d2x + d2y));
        }
    }
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int rowV = j * nx;
        const float* __restrict vRow = vPtr + rowV;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict vBot = vPtr + (j - 1) * nx;
        const float* __restrict uRow = uPtr + j * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict maskRow = vOpenF.data() + rowV;
        float* __restrict outRow = vStar + rowV;
        int i = 1;
        for (; i + 8 <= nx - 1; i += 8) {
            const __m256 vij   = _mm256_loadu_ps(vRow + i);
            const __m256 vtop  = _mm256_loadu_ps(vTop + i);
            const __m256 vbot  = _mm256_loadu_ps(vBot + i);
            const __m256 vleft = _mm256_loadu_ps(vRow + i - 1);
            const __m256 vrght = _mm256_loadu_ps(vRow + i + 1);
            const __m256 ue = _mm256_mul_ps(
                _mm256_add_ps(
                    _mm256_add_ps(_mm256_loadu_ps(uRow + i), _mm256_loadu_ps(uRow + i + 1)),
                    _mm256_add_ps(_mm256_loadu_ps(uBot + i), _mm256_loadu_ps(uBot + i + 1))),
                quarter);
            const __m256 dvdx = _mm256_blendv_ps(
                _mm256_mul_ps(_mm256_sub_ps(vrght, vij), invDxV),
                _mm256_mul_ps(_mm256_sub_ps(vij, vleft), invDxV),
                _mm256_cmp_ps(ue, zero, _CMP_GT_OS));

            const __m256 dvdy = _mm256_blendv_ps(
                _mm256_mul_ps(_mm256_sub_ps(vtop, vij), invDyV),
                _mm256_mul_ps(_mm256_sub_ps(vij, vbot), invDyV),
                _mm256_cmp_ps(vij, zero, _CMP_GT_OS));
            const __m256 d2x = _mm256_mul_ps(
                _mm256_add_ps(vrght, _mm256_sub_ps(vleft, _mm256_mul_ps(two, vij))), invDx2V);
            const __m256 d2y = _mm256_mul_ps(
                _mm256_add_ps(vtop, _mm256_sub_ps(vbot, _mm256_mul_ps(two, vij))), invDy2V);
            const __m256 conv = _mm256_add_ps(_mm256_mul_ps(ue, dvdx),
                                              _mm256_mul_ps(vij, dvdy));
            const __m256 res = _mm256_add_ps(
                _mm256_sub_ps(vij, _mm256_mul_ps(dtConvV, conv)),
                _mm256_mul_ps(dtNuV, _mm256_add_ps(d2x, d2y)));
            _mm256_storeu_ps(outRow + i,
                             _mm256_mul_ps(res, _mm256_loadu_ps(maskRow + i)));
        }
        for (; i < nx - 1; ++i) {
            const float vij = vRow[i];
            const float ue = 0.25f * (uRow[i] + uRow[i + 1] + uBot[i] + uBot[i + 1]);
            const float vl = vRow[i - 1];
            const float vr = vRow[i + 1];
            const float vb = vBot[i];
            const float vt = vTop[i];
            const float dvdx = (ue > 0.0f) ? (vij - vl) * invDx : (vr - vij) * invDx;
            const float dvdy = (vij > 0.0f) ? (vij - vb) * invDy : (vt - vij) * invDy;
            const float d2x = (vr - 2.0f * vij + vl) * invDx2;
            const float d2y = (vt - 2.0f * vij + vb) * invDy2;

            outRow[i] = maskRow[i] *
                (vij - dtConv * (ue * dvdx + vij * dvdy) + dtNu * (d2x + d2y));
        }
        for (int pass = 0; pass < 2; ++pass) {
            const int i2 = (pass == 0) ? 0 : nx - 1;
            if (i2 < 0 || (pass == 1 && nx < 2))
                continue;
            const float vij = vRow[i2];
            const float ue = 0.25f * (uRow[i2] + uRow[i2 + 1] + uBot[i2] + uBot[i2 + 1]);
            const float vl = (i2 == 0) ? vij : vRow[i2 - 1];
            const float vr = (i2 == nx - 1) ? vij : vRow[i2 + 1];
            const float vb = vBot[i2];
            const float vt = vTop[i2];
            const float dvdx = (ue > 0.0f) ? (vij - vl) * invDx : (vr - vij) * invDx;
            const float dvdy = (vij > 0.0f) ? (vij - vb) * invDy : (vt - vij) * invDy;
            const float d2x = (vr - 2.0f * vij + vl) * invDx2;
            const float d2y = (vt - 2.0f * vij + vb) * invDy2;
            outRow[i2] = maskRow[i2] *
                (vij - dtConv * (ue * dvdx + vij * dvdy) + dtNu * (d2x + d2y));
        }
    }
    for (int j = 0; j < ny; ++j) {
        uStar[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        uStar[idxU(nx, j)] = solidMask[idxP(nx - 1, j)]
            ? 0.0f
            : uStar[idxU(nx - 1, j)];
    }
    for (int i = 0; i < nx; ++i) {
        vStar[idxV(i, 0)] = 0.0f;
        vStar[idxV(i, ny)] = 0.0f;
    }
}

void Solver::solvePoisson() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    const float invDt = 1.0f / dt;
    const __m256 invDxV = _mm256_set1_ps(invDx);
    const __m256 invDyV = _mm256_set1_ps(invDy);
    const __m256 invDtV = _mm256_set1_ps(invDt);
    const float* __restrict us = u_star.data();
    const float* __restrict vs = v_star.data();
    float* __restrict rhsPtr = rhs.data();
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        int i = 0;
        for (; i + 8 <= nx; i += 8) {
            const __m256 uR = _mm256_loadu_ps(us + rowU + i + 1);
            const __m256 uL = _mm256_loadu_ps(us + rowU + i);
            const __m256 vT = _mm256_loadu_ps(vs + rowVTop + i);
            const __m256 vB = _mm256_loadu_ps(vs + rowV + i);
            const __m256 div = _mm256_add_ps(
                _mm256_mul_ps(_mm256_sub_ps(uR, uL), invDxV),
                _mm256_mul_ps(_mm256_sub_ps(vT, vB), invDyV));
            _mm256_storeu_ps(rhsPtr + rowP + i, _mm256_mul_ps(div, invDtV));
        }

        for (; i < nx; ++i) {
            const float div =
                (us[rowU + i + 1] - us[rowU + i]) * invDx +
                (vs[rowVTop + i] - vs[rowV + i]) * invDy;
            rhsPtr[rowP + i] = div * invDt;
        }
    }
    const int cycles = std::max(1, cfg.mgIterations);
    lastResidual = multigrid.solve(p, rhs,
                                   cfg.smootherOmega,
                                   cfg.omega,
                                   cycles,
                                   cfg.mgTolerance);
}

void Solver::corrector() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    const __m256 invDxV = _mm256_set1_ps(invDx);
    const __m256 invDyV = _mm256_set1_ps(invDy);
    const __m256 dtV    = _mm256_set1_ps(dt);
    const float* __restrict pPtr = p.data();
    const float* __restrict us = u_star.data();
    const float* __restrict vs = v_star.data();
    float* __restrict uPtr = u.data();
    float* __restrict vPtr = v.data();
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        int i = 1;
        for (; i + 8 <= nx; i += 8) {
            const __m256 pR = _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pL = _mm256_loadu_ps(pPtr + rowP + i - 1);
            const __m256 uS = _mm256_loadu_ps(us + rowU + i);
            const __m256 res = _mm256_sub_ps(
                uS, _mm256_mul_ps(dtV,
                    _mm256_mul_ps(_mm256_sub_ps(pR, pL), invDxV)));
            _mm256_storeu_ps(uPtr + rowU + i,
                _mm256_mul_ps(res, _mm256_loadu_ps(uOpenF.data() + rowU + i)));
        }
        for (; i < nx; ++i) {
            const float res = us[rowU + i]
                - dt * (pPtr[rowP + i] - pPtr[rowP + i - 1]) * invDx;
            uPtr[rowU + i] = uOpenF[rowU + i] * res;
        }
    }
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowPBot = (j - 1) * nx;
        const int rowV = j * nx;
        int i = 0;
        for (; i + 8 <= nx; i += 8) {
            const __m256 pT = _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pB = _mm256_loadu_ps(pPtr + rowPBot + i);
            const __m256 vS = _mm256_loadu_ps(vs + rowV + i);
            const __m256 res = _mm256_sub_ps(
                vS, _mm256_mul_ps(dtV,
                    _mm256_mul_ps(_mm256_sub_ps(pT, pB), invDyV)));
            _mm256_storeu_ps(vPtr + rowV + i,
                _mm256_mul_ps(res, _mm256_loadu_ps(vOpenF.data() + rowV + i)));
        }
        for (; i < nx; ++i) {
            const float res = vs[rowV + i]
                - dt * (pPtr[rowP + i] - pPtr[rowPBot + i]) * invDy;
            vPtr[rowV + i] = vOpenF[rowV + i] * res;
        }
    }
    const float outletFactor = 2.0f * dt * invDx;
    for (int j = 0; j < ny; ++j) {
        if (solidMask[idxP(nx - 1, j)]) {
            u[idxU(nx, j)] = 0.0f;
        } else {
            u[idxU(nx, j)] = u_star[idxU(nx, j)] + outletFactor * p[idxP(nx - 1, j)];
        }
    }

    applyBC();
}

void Solver::applyBC() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    for (int j = 0; j < ny; ++j)
        u[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;

    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0f;
        v[idxV(i, ny)] = 0.0f;
    }
}

float Solver::maxDivergence() const {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
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
    float m = 0.0f;
    for (float value : u) m = std::max(m, std::fabs(value));
    for (float value : v) m = std::max(m, std::fabs(value));
    return m;
}

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    currentTime = 0.0;
    step = 0;

    std::error_code ec;
    std::filesystem::create_directories(cfg.outputDir, ec);
    computeDt();
    saveVTK(step);
    const int saveInterval = std::max(1, cfg.saveInterval);
    const int dtInterval = std::max(1, cfg.dtUpdateInterval);

    while (currentTime < cfg.totalTime) {
        if (step % dtInterval == 0)
            computeDt();

        float stepDt = dt;
        if (currentTime + stepDt > cfg.totalTime)
            stepDt = static_cast<float>(cfg.totalTime - currentTime);
        if (!(stepDt > 0.0f))
            break;
        const float savedDt = dt;
        dt = stepDt;

        predictor();
        solvePoisson();
        corrector();

        currentTime += dt;
        ++step;
        dt = savedDt;

        if (step % 10 == 0) {
            const float vmax = maxVelocity();
            std::cout << "Step " << step
                      << ", t = " << currentTime
                      << " s, dt = " << stepDt
                      << ", |u|max = " << vmax
                      << ", div = " << maxDivergence()
                      << ", mg res = " << lastResidual
                      << " (" << multigrid.cyclesUsed() << " cycles)"
                      << std::endl;
            if (std::isnan(vmax) || std::isinf(vmax)) {
                std::cerr << "Solution diverged at step " << step
                          << "; aborting.\n";
                break;
            }
        }
        if (step % saveInterval == 0)
            saveVTK(step);
    }

    saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s after "
              << step << " steps.\n";
}

void Solver::saveVTK(int stepNum) const {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    std::filesystem::path path = cfg.outputDir.empty()
        ? std::filesystem::path(".")
        : std::filesystem::path(cfg.outputDir);
    path /= "solution_" + std::to_string(stepNum) + ".vtk";
    std::ofstream fout(path, std::ios::binary);
    if (!fout) {
        std::cerr << "Cannot open " << path.string() << " for writing.\n";
        return;
    }
    fout << "# vtk DataFile Version 3.0\n"
         << "CFD-Solver-2D output, step " << stepNum << "\n"
         << "BINARY\n"
         << "DATASET STRUCTURED_POINTS\n"
         << "DIMENSIONS " << nx + 1 << " " << ny + 1 << " 1\n"
         << "ORIGIN 0 0 0\n"
         << "SPACING " << dx << " " << dy << " 1\n"
         << "CELL_DATA " << nx * ny << "\n";

    constexpr size_t kBufferWords = 4096;
    std::array<uint32_t, kBufferWords> buffer{};
    size_t used = 0;
    auto flush = [&]() {
        if (used == 0)
            return;
        fout.write(reinterpret_cast<const char*>(buffer.data()),
                   static_cast<std::streamsize>(used * sizeof(uint32_t)));
        used = 0;
    };
    auto pushWord = [&](uint32_t word) {
        // Legacy VTK binary data is big endian.
        buffer[used++] = ((word & 0x000000FFu) << 24) |
                         ((word & 0x0000FF00u) << 8) |
                         ((word & 0x00FF0000u) >> 8) |
                         ((word & 0xFF000000u) >> 24);
        if (used == kBufferWords)
            flush();
    };
    auto pushFloat = [&](float value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        pushWord(word);
    };
    auto pushInt = [&](int32_t value) {
        uint32_t word;
        std::memcpy(&word, &value, sizeof(word));
        pushWord(word);
    };

    fout << "SCALARS pressure float 1\nLOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i)
            pushFloat(p[row + i] * cfg.ro);
    }
    flush();
    fout << "\n";

    fout << "SCALARS solid int 1\nLOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i)
            pushInt(static_cast<int32_t>(solidMask[row + i]));
    }
    flush();
    fout << "\n";
    fout << "VECTORS velocity float\n";
    for (int j = 0; j < ny; ++j) {
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i) {
            pushFloat(0.5f * (u[rowU + i] + u[rowU + i + 1]));
            pushFloat(0.5f * (v[rowV + i] + v[rowVTop + i]));
            pushFloat(0.0f);
        }
    }
    flush();
    fout << "\n";

    if (stepNum % (std::max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
        std::cout << "Saved " << path.string() << std::endl;
}