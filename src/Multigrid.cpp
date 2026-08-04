#include "Multigrid.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <immintrin.h>

namespace {
constexpr int kPreSmooth    = 2;
constexpr int kPostSmooth   = 2;
constexpr int kCoarseSmooth = 50;
inline int coarseSweeps(int nx, int ny) {
    const int wanted = 2 * (nx > ny ? nx : ny);
    if (wanted < kCoarseSmooth) return kCoarseSmooth;
    return (wanted > 400) ? 400 : wanted;
}

constexpr int kParallelRows = 32;

inline float horizontalSum(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

}
Multigrid::Multigrid(int nx, int ny, float dx, float dy, int minCoarseSize)
    : nx0(nx), ny0(ny), dx0(dx), dy0(dy),
      minCoarseSize(minCoarseSize < 4 ? 4 : minCoarseSize)
{
}

Multigrid::~Multigrid() {
#ifdef USE_CUDA
    freeDevice();
#endif
}

void Multigrid::buildHierarchy() {
    lv.clear();
    int   cnx = nx0;
    int   cny = ny0;
    float cdx = dx0;
    float cdy = dy0;
    while (true) {
        Level L;
        L.nx = cnx;
        L.ny = cny;
        L.n  = cnx * cny;
        L.dx = cdx;
        L.dy = cdy;
        const int halo = std::max(cnx, 8);
        L.p.init(L.n, halo);
        L.res.init(L.n, halo);
        L.rhs.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.cW.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.cE.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.cS.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.cN.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.diag.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.invDiag.assign(static_cast<size_t>(L.n) + 16u, 0.0f);
        L.solid.assign(static_cast<size_t>(L.n), 0u);
        lv.push_back(std::move(L));

        // Semi-coarsening. A point smoother only damps the error in the
        // direction it is strongly coupled to, so on a grid with dx << dy the
        // error along y is barely touched and standard full coarsening makes a
        // V-cycle stall or even diverge. Coarsening the over-resolved axis
        // alone drives the coarse grids towards isotropy, where the smoother
        // works properly again.
        // Only an even count is coarsened. With an odd count the last coarse
        // cell would cover a single fine cell instead of two, its column of the
        // prolongation would carry half the weight of every other column, and
        // the restriction - being the transpose - would hand the coarse solver
        // a residual that is half as large as it should be there. That
        // inconsistency is enough to make the V-cycle diverge.
        bool canX = (cnx > minCoarseSize) && (cnx % 2 == 0);
        bool canY = (cny > minCoarseSize) && (cny % 2 == 0);

        if (canX && canY) {
            if (cdx < 0.5f * cdy)      canY = false;
            else if (cdy < 0.5f * cdx) canX = false;
        }
        if (!canX && !canY)
            break;

        const int nnx = canX ? (cnx + 1) / 2 : cnx;
        const int nny = canY ? (cny + 1) / 2 : cny;
        cdx *= static_cast<float>(cnx) / static_cast<float>(nnx);
        cdy *= static_cast<float>(cny) / static_cast<float>(nny);
        lv.back().refX = canX ? 2 : 1;
        lv.back().refY = canY ? 2 : 1;
        cnx = nnx;
        cny = nny;
    }

    levels = static_cast<int>(lv.size());
}

namespace {

struct Stencil1D {
    int   c0, c1;
    float w0, w1;
};

inline Stencil1D transferStencil(int i, int ref, int coarseN) {
    Stencil1D s;
    if (ref == 1) {
        s.c0 = s.c1 = i;
        s.w0 = 1.0f;
        s.w1 = 0.0f;
        return s;
    }

    s.c0 = i >> 1;
    s.c1 = ((i & 1) == 0) ? (s.c0 > 0 ? s.c0 - 1 : s.c0)
                          : (s.c0 + 1 < coarseN ? s.c0 + 1 : s.c0);
    if (s.c1 == s.c0) {
        s.w0 = 1.0f;
        s.w1 = 0.0f;
    } else {
        s.w0 = 0.75f;
        s.w1 = 0.25f;
    }
    return s;
}

}
void Multigrid::buildTransferWeights(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    Level& F = lv[fineLevel];

    F.pWeight.assign(static_cast<size_t>(F.n), 0.0f);
    if (coarseLevel >= levels)
        return;
    const Level& C = lv[coarseLevel];
    for (int j = 0; j < F.ny; ++j) {
        const Stencil1D sy = transferStencil(j, F.refY, C.ny);
        for (int i = 0; i < F.nx; ++i) {
            const int fid = j * F.nx + i;
            if (F.solid[fid] || F.diag[fid] == 0.0f)
                continue;
            const Stencil1D sx = transferStencil(i, F.refX, C.nx);

            float weight = 0.0f;
            const int   cx[4] = {sx.c0, sx.c1, sx.c0, sx.c1};
            const int   cy[4] = {sy.c0, sy.c0, sy.c1, sy.c1};
            const float w[4]  = {sx.w0 * sy.w0, sx.w1 * sy.w0,
                                 sx.w0 * sy.w1, sx.w1 * sy.w1};
            for (int k = 0; k < 4; ++k) {
                if (w[k] == 0.0f)
                    continue;
                if (!C.solid[cy[k] * C.nx + cx[k]])
                    weight += w[k];
            }
            F.pWeight[fid] = weight;
        }
    }
}
void Multigrid::buildCoefficients(Level& L) {
    const int nx = L.nx;
    const int ny = L.ny;
    const float invDx2 = 1.0f / (L.dx * L.dx);
    const float invDy2 = 1.0f / (L.dy * L.dy);

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i) {
            const int id = row + i;

            if (L.solid[id]) {
                L.cW[id] = L.cE[id] = L.cS[id] = L.cN[id] = 0.0f;
                L.diag[id] = 0.0f;
                L.invDiag[id] = 0.0f;
                continue;
            }
            const float cw = (i > 0      && !L.solid[id - 1])  ? invDx2 : 0.0f;
            const float ce = (i < nx - 1 && !L.solid[id + 1])  ? invDx2 : 0.0f;
            const float cs = (j > 0      && !L.solid[id - nx]) ? invDy2 : 0.0f;
            const float cn = (j < ny - 1 && !L.solid[id + nx]) ? invDy2 : 0.0f;

            float diag = cw + ce + cs + cn;

            if (i == nx - 1)
                diag += 2.0f * invDx2;

            L.cW[id] = cw;
            L.cE[id] = ce;
            L.cS[id] = cs;
            L.cN[id] = cn;

            if (diag > 0.0f) {
                L.diag[id] = diag;
                L.invDiag[id] = 1.0f / diag;
            } else {
                L.diag[id] = 0.0f;
                L.invDiag[id] = 0.0f;
            }
        }
    }
}

void Multigrid::setGeometry(const std::vector<uint8_t>& solid) {
    if (lv.empty())
        buildHierarchy();

    std::copy(solid.begin(), solid.begin() + lv[0].n, lv[0].solid.begin());
    for (int l = 1; l < levels; ++l) {
        const Level& F = lv[l - 1];
        Level& C = lv[l];

        for (int j = 0; j < C.ny; ++j) {
            for (int i = 0; i < C.nx; ++i) {
                const int i0 = i * F.refX;
                const int i1 = std::min(i0 + F.refX - 1, F.nx - 1);
                const int j0 = j * F.refY;
                const int j1 = std::min(j0 + F.refY - 1, F.ny - 1);

                bool allSolid = true;
                for (int jj = j0; jj <= j1 && allSolid; ++jj)
                    for (int ii = i0; ii <= i1 && allSolid; ++ii)
                        if (!F.solid[jj * F.nx + ii])
                            allSolid = false;

                C.solid[j * C.nx + i] = allSolid ? 1u : 0u;
            }
        }
    }

    for (int l = 0; l < levels; ++l)
        buildCoefficients(lv[l]);

    for (int l = 0; l < levels; ++l)
        buildTransferWeights(l);

    geometryReady = true;
    firstSolve = true;

#ifdef USE_CUDA
    if (useCuda)
        setGeometryCuda();
#endif
}

void Multigrid::setUseCuda(bool enable) {
#ifdef USE_CUDA
    useCuda = enable;
    if (useCuda && geometryReady)
        setGeometryCuda();
#else
    (void)enable;
    useCuda = false;
#endif
}

void Multigrid::smooth(int level, float omega, int sweeps) {
    Level& L = lv[level];
    const int nx = L.nx;
    const int ny = L.ny;

    float* const        pAll  = L.p.data();
    const float* const  rhs   = L.rhs.data();
    const float* const  cW    = L.cW.data();
    const float* const  cE    = L.cE.data();
    const float* const  cS    = L.cS.data();
    const float* const  cN    = L.cN.data();
    const float* const  invD  = L.invDiag.data();

    const __m256i laneEven = _mm256_setr_epi32(-1, 0, -1, 0, -1, 0, -1, 0);
    const __m256i laneOdd  = _mm256_setr_epi32(0, -1, 0, -1, 0, -1, 0, -1);
    const __m256  omegaV   = _mm256_set1_ps(omega);
    #pragma omp parallel if (ny >= kParallelRows)
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        for (int color = 0; color < 2; ++color) {
            #pragma omp for schedule(static)
            for (int j = 0; j < ny; ++j) {
                const int row = j * nx;
                const int parity = color ^ (j & 1);
                const __m256i lane = parity ? laneOdd : laneEven;

                int i = 0;
                for (; i + 8 <= nx; i += 8) {
                    const int id = row + i;

                    const __m256 pc = _mm256_loadu_ps(pAll + id);
                    const __m256 pl = _mm256_loadu_ps(pAll + id - 1);
                    const __m256 pr = _mm256_loadu_ps(pAll + id + 1);
                    const __m256 pd = _mm256_loadu_ps(pAll + id - nx);
                    const __m256 pu = _mm256_loadu_ps(pAll + id + nx);

                    __m256 num = _mm256_mul_ps(_mm256_loadu_ps(cW + id), pl);
                    num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cE + id), pr));
                    num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cS + id), pd));
                    num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cN + id), pu));

                    const __m256 pNew = _mm256_mul_ps(
                        _mm256_sub_ps(num, _mm256_loadu_ps(rhs + id)),
                        _mm256_loadu_ps(invD + id));

                    const __m256 upd = _mm256_add_ps(
                        pc, _mm256_mul_ps(omegaV, _mm256_sub_ps(pNew, pc)));

                    _mm256_maskstore_ps(pAll + id, lane, upd);
                }
                for (int ii = i + parity; ii < nx; ii += 2) {
                    const int id = row + ii;
                    const float num =
                        cW[id] * pAll[id - 1] +
                        cE[id] * pAll[id + 1] +
                        cS[id] * pAll[id - nx] +
                        cN[id] * pAll[id + nx];
                    const float pNew = (num - rhs[id]) * invD[id];
                    pAll[id] += omega * (pNew - pAll[id]);
                }
            }
        }
    }
}
void Multigrid::computeResidual(int level) {
    Level& L = lv[level];
    const int nx = L.nx;
    const int ny = L.ny;

    const float* const pAll = L.p.data();
    const float* const rhs  = L.rhs.data();
    const float* const cW   = L.cW.data();
    const float* const cE   = L.cE.data();
    const float* const cS   = L.cS.data();
    const float* const cN   = L.cN.data();
    const float* const diag = L.diag.data();
    float* const       res  = L.res.data();

    #pragma omp parallel for schedule(static) if (ny >= kParallelRows)
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;

        int i = 0;
        for (; i + 8 <= nx; i += 8) {
            const int id = row + i;
            const __m256 pc = _mm256_loadu_ps(pAll + id);
            const __m256 pl = _mm256_loadu_ps(pAll + id - 1);
            const __m256 pr = _mm256_loadu_ps(pAll + id + 1);
            const __m256 pd = _mm256_loadu_ps(pAll + id - nx);
            const __m256 pu = _mm256_loadu_ps(pAll + id + nx);

            __m256 num = _mm256_mul_ps(_mm256_loadu_ps(cW + id), pl);
            num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cE + id), pr));
            num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cS + id), pd));
            num = _mm256_add_ps(num, _mm256_mul_ps(_mm256_loadu_ps(cN + id), pu));
            const __m256 Ap = _mm256_sub_ps(
                num, _mm256_mul_ps(_mm256_loadu_ps(diag + id), pc));
            _mm256_storeu_ps(res + id,
                             _mm256_sub_ps(_mm256_loadu_ps(rhs + id), Ap));
        }

        for (; i < nx; ++i) {
            const int id = row + i;
            const float num =
                cW[id] * pAll[id - 1] +
                cE[id] * pAll[id + 1] +
                cS[id] * pAll[id - nx] +
                cN[id] * pAll[id + nx];
            res[id] = rhs[id] - (num - diag[id] * pAll[id]);
        }
    }
}
float Multigrid::vectorNorm(const float* v, int n) {
    double total = 0.0;
    #pragma omp parallel reduction(+ : total) if (n >= 8192)
    {
        __m256 acc = _mm256_setzero_ps();

        #pragma omp for schedule(static) nowait
        for (int i = 0; i <= n - 8; i += 8) {
            const __m256 x = _mm256_loadu_ps(v + i);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(x, x));
        }
        total += static_cast<double>(horizontalSum(acc));
    }
    const int start = (n / 8) * 8;
    for (int i = start; i < n; ++i)
        total += static_cast<double>(v[i]) * static_cast<double>(v[i]);

    return static_cast<float>(std::sqrt(total));
}

float Multigrid::residualNorm(int level) const {
    const Level& L = lv[level];
    return vectorNorm(L.res.data(), L.n);
}

// ---------------------------------------------------------------------------
// Grid transfer
//
// The restriction is the exact transpose of the prolongation, divided by the
// number of fine cells per coarse cell:  R = P^T / (refX*refY).
//
// This is the part the previous implementation got wrong. It restricted with a
// plain average over the 2x2 block while prolongating bilinearly, so R was not
// P^T and the coarse grid correction was not a projection in the energy norm.
// For some grid sizes and aspect ratios the resulting two-grid operator has a
// spectral radius above one and the V-cycle *amplifies* the error instead of
// reducing it - which is exactly what made the solver blow up on a 100x100 or a
// 64x128 domain while working fine on 128x128(during the tests, u know ehehe).
// ---------------------------------------------------------------------------

void Multigrid::restrict(int fineLevel, const float* fineSrc) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;

    const Level& F = lv[fineLevel];
    Level& C = lv[coarseLevel];

    const int refX = F.refX;
    const int refY = F.refY;

    const float scale = 1.0f / static_cast<float>(refX * refY);

    float* const coarseRhs = C.rhs.data();
    const float* const pWeight = F.pWeight.data();

    #pragma omp parallel for schedule(static) if (C.ny >= kParallelRows)
    for (int J = 0; J < C.ny; ++J) {
        for (int I = 0; I < C.nx; ++I) {
            const int cid = J * C.nx + I;

            if (C.solid[cid] || C.diag[cid] == 0.0f) {
                coarseRhs[cid] = 0.0f;
                continue;
            }
            const int i0 = (refX == 1) ? I : std::max(0, 2 * I - 1);
            const int i1 = (refX == 1) ? I : std::min(F.nx - 1, 2 * I + 2);
            const int j0 = (refY == 1) ? J : std::max(0, 2 * J - 1);
            const int j1 = (refY == 1) ? J : std::min(F.ny - 1, 2 * J + 2);
            float sum = 0.0f;
            for (int jj = j0; jj <= j1; ++jj) {
                const Stencil1D sy = transferStencil(jj, refY, C.ny);
                float wy = 0.0f;
                if (sy.c0 == J) wy += sy.w0;
                if (sy.c1 == J) wy += sy.w1;
                if (wy == 0.0f)
                    continue;
                const int frow = jj * F.nx;
                for (int ii = i0; ii <= i1; ++ii) {
                    const int fid = frow + ii;
                    const float norm = pWeight[fid];
                    if (norm <= 0.0f)
                        continue;
                    const Stencil1D sx = transferStencil(ii, refX, C.nx);
                    float wx = 0.0f;
                    if (sx.c0 == I) wx += sx.w0;
                    if (sx.c1 == I) wx += sx.w1;
                    if (wx == 0.0f)
                        continue;
                    sum += (wx * wy / norm) * fineSrc[fid];
                }
            }
            coarseRhs[cid] = sum * scale;
        }
    }
}

void Multigrid::restrictResidual(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;
    lv[coarseLevel].p.zero();
    restrict(fineLevel, lv[fineLevel].res.data());
}

void Multigrid::restrictRHS(int fineLevel) {
    restrict(fineLevel, lv[fineLevel].rhs.data());
}
void Multigrid::prolongateAdd(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;
    const int fineLevel = coarseLevel - 1;
    Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];

    float* const fineP = F.p.data();
    const float* const coarseP = C.p.data();

    #pragma omp parallel for schedule(static) if (F.ny >= kParallelRows)
    for (int j = 0; j < F.ny; ++j) {
        const Stencil1D sy = transferStencil(j, F.refY, C.ny);
        for (int i = 0; i < F.nx; ++i) {
            const int fid = j * F.nx + i;
            const float norm = F.pWeight[fid];
            if (norm <= 0.0f)
                continue;

            const Stencil1D sx = transferStencil(i, F.refX, C.nx);

            const int   cx[4] = {sx.c0, sx.c1, sx.c0, sx.c1};
            const int   cy[4] = {sy.c0, sy.c0, sy.c1, sy.c1};
            const float w[4]  = {sx.w0 * sy.w0, sx.w1 * sy.w0,
                                 sx.w0 * sy.w1, sx.w1 * sy.w1};

            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (w[k] == 0.0f)
                    continue;
                const int cid = cy[k] * C.nx + cx[k];
                if (!C.solid[cid])
                    value += w[k] * coarseP[cid];
            }

            fineP[fid] += value / norm;
        }
    }
}

void Multigrid::prolongateSet(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;
    const int fineLevel = coarseLevel - 1;
    Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];

    F.p.zero();

    float* const fineP = F.p.data();
    const float* const coarseP = C.p.data();

    #pragma omp parallel for schedule(static) if (F.ny >= kParallelRows)
    for (int j = 0; j < F.ny; ++j) {
        const Stencil1D sy = transferStencil(j, F.refY, C.ny);
        for (int i = 0; i < F.nx; ++i) {
            const int fid = j * F.nx + i;
            const float norm = F.pWeight[fid];
            if (norm <= 0.0f)
                continue;
            const Stencil1D sx = transferStencil(i, F.refX, C.nx);
            const int   cx[4] = {sx.c0, sx.c1, sx.c0, sx.c1};
            const int   cy[4] = {sy.c0, sy.c0, sy.c1, sy.c1};
            const float w[4]  = {sx.w0 * sy.w0, sx.w1 * sy.w0,
                                 sx.w0 * sy.w1, sx.w1 * sy.w1};
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (w[k] == 0.0f)
                    continue;
                const int cid = cy[k] * C.nx + cx[k];
                if (!C.solid[cid])
                    value += w[k] * coarseP[cid];
            }

            fineP[fid] = value / norm;
        }
    }
}
void Multigrid::vCycle(int level, float smootherOmega, float coarseOmega) {
    if (level == levels - 1) {
        smooth(level, coarseOmega, coarseSweeps(lv[level].nx, lv[level].ny));
        return;
    }
    smooth(level, smootherOmega, kPreSmooth);
    computeResidual(level);
    restrictResidual(level);
    vCycle(level + 1, smootherOmega, coarseOmega);
    prolongateAdd(level + 1);
    smooth(level, smootherOmega, kPostSmooth);
}
void Multigrid::fullMultigrid(float smootherOmega, float coarseOmega) {
    const int coarsest = levels - 1;
    if (coarsest == 0) {
        smooth(0, coarseOmega, coarseSweeps(lv[0].nx, lv[0].ny));
        return;
    }
    for (int level = 0; level < coarsest; ++level)
        restrictRHS(level);
    lv[coarsest].p.zero();
    smooth(coarsest, coarseOmega, coarseSweeps(lv[coarsest].nx, lv[coarsest].ny));
    for (int level = coarsest; level > 0; --level) {
        prolongateSet(level);
        vCycle(level - 1, smootherOmega, coarseOmega);
    }
}
float Multigrid::solve(std::vector<float>& pressure,
                       const std::vector<float>& rhs,
                       float smootherOmega,
                       float coarseOmega,
                       int   maxCycles,
                       float tolerance)
{
    if (!geometryReady) {
        std::fprintf(stderr, "Multigrid::solve called before setGeometry\n");
        return 0.0f;
    }

#ifdef USE_CUDA
    if (useCuda)
        return solveCuda(pressure, rhs, smootherOmega, coarseOmega,
                         maxCycles, tolerance);
#endif

    Level& L0 = lv[0];
    const int n = L0.n;

    float* const p0 = L0.p.data();
    float* const rhs0 = L0.rhs.data();
    #pragma omp parallel for schedule(static)
    for (int id = 0; id < n; ++id) {
        const bool active = (L0.diag[id] > 0.0f);
        p0[id]   = active ? pressure[id] : 0.0f;
        rhs0[id] = active ? rhs[id] : 0.0f;
    }

    const float rhsNorm = vectorNorm(rhs0, n);
    const float scale = (rhsNorm > 1e-20f) ? rhsNorm : 1.0f;

    if (firstSolve) {
        // Nested iteration once, to build a good field from nothing. Later
        // steps start from the previous pressure, which is a far better guess
        // than anything a fresh FMG pass produces.
        fullMultigrid(smootherOmega, coarseOmega);
        firstSolve = false;
    }

    lastCycles = 0;
    float relative = 1.0f;

    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        computeResidual(0);
        relative = residualNorm(0) / scale;
        if (relative < tolerance)
            break;
        vCycle(0, smootherOmega, coarseOmega);
        ++lastCycles;
    }

    if (lastCycles > 0) {
        computeResidual(0);
        relative = residualNorm(0) / scale;
    }
    std::copy(p0, p0 + n, pressure.begin());
    return relative;
}
