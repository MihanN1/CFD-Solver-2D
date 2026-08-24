#include "Multigrid.hpp"
#include "Runtime.hpp"
#include <cmath>
#include <cstdio>
#include <algorithm>
#ifdef __AVX2__
#include <immintrin.h>
#endif

static const int PRE_SMOOTH_SWEEPS = 2;
static const int POST_SMOOTH_SWEEPS = 2;
static const int COARSE_SMOOTH_SWEEPS = 50;
// Only spawn OpenMP threads when the level is tall enough to pay for them
static const int PARALLEL_ROWS_MIN = 32;

namespace {
// On the coarsest level the smoother acts as a solver, so it needs enough
// sweeps to push information across the whole grid
int coarseSweeps(int nx, int ny) {
    const int wanted = 2 * (nx > ny ? nx : ny);
    if (wanted < COARSE_SMOOTH_SWEEPS) return COARSE_SMOOTH_SWEEPS;
    return (wanted > 400) ? 400 : wanted;
}

#ifdef __AVX2__
// Sum of the 8 lanes
float horizontalSum(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}
#endif
}

Multigrid::Multigrid(int nx, int ny, float dx, float dy, int minCoarseSize)
    :
    nx(nx),
    ny(ny),
    dx(dx),
    dy(dy),
    minCoarseSize(minCoarseSize < 4 ? 4 : minCoarseSize)
{
}

Multigrid::~Multigrid() {
#ifdef USE_CUDA
    freeDevice();
#endif
}

void Multigrid::buildHierarchy() {
    gridLevels.clear();
    int curNx = nx;
    int curNy = ny;
    float curDx = dx;
    float curDy = dy;

    while (true) {
        Level grid;
        grid.nx = curNx;
        grid.ny = curNy;
        grid.cellCount = curNx * curNy;
        grid.dx = curDx;
        grid.dy = curDy;

        const int halo = std::max(curNx, 8);
        grid.pressure.init(grid.cellCount, halo);
        grid.residual.init(grid.cellCount, halo);

        const size_t padded = static_cast<size_t>(grid.cellCount) + 16u;
        grid.rhs.assign(padded, 0.0f);
        grid.coefW.assign(padded, 0.0f);
        grid.coefE.assign(padded, 0.0f);
        grid.coefS.assign(padded, 0.0f);
        grid.coefN.assign(padded, 0.0f);
        grid.diag.assign(padded, 0.0f);
        grid.invDiag.assign(padded, 0.0f);
        grid.solid.assign(static_cast<size_t>(grid.cellCount), 0);
        gridLevels.push_back(std::move(grid));

        // Semi-coarsening: a point smoother only damps the error along the axis
        // it is strongly coupled to, so on a grid with dx << dy we coarsen the
        // over-resolved axis alone to drive the coarse grids towards isotropy.
        // Only an even count is coarsened, otherwise the last coarse cell would
        // cover a single fine cell and restriction would stop being the
        // transpose of prolongation, which makes the V-cycle diverge.
        bool canX = (curNx > minCoarseSize) && (curNx % 2 == 0);
        bool canY = (curNy > minCoarseSize) && (curNy % 2 == 0);

        // Not a strict comparison: a ratio of exactly two is the case the rule
        // exists for, and Lx=2, Ly=1 on a square cell count lands on it every
        // time. Letting it through as isotropic carried the anisotropy down
        // the whole hierarchy and cost a level and most of the convergence.
        if (canX && canY) {
            if (curDx <= 0.5f * curDy)      canY = false;
            else if (curDy <= 0.5f * curDx) canX = false;
        }
        if (!canX && !canY)
            break;

        const int nextNx = canX ? (curNx + 1) / 2 : curNx;
        const int nextNy = canY ? (curNy + 1) / 2 : curNy;
        curDx *= static_cast<float>(curNx) / static_cast<float>(nextNx);
        curDy *= static_cast<float>(curNy) / static_cast<float>(nextNy);
        gridLevels.back().refineX = canX ? 2 : 1;
        gridLevels.back().refineY = canY ? 2 : 1;
        curNx = nextNx;
        curNy = nextNy;
    }

    levels = static_cast<int>(gridLevels.size());
}

namespace {
// The two coarse cells a fine cell interpolates from, and their weights
struct Stencil1D {
    int coarse0, coarse1;
    float weight0, weight1;
};

Stencil1D transferStencil(int i, int refine, int coarseN) {
    Stencil1D s;
    if (refine == 1) {
        s.coarse0 = s.coarse1 = i;
        s.weight0 = 1.0f;
        s.weight1 = 0.0f;
        return s;
    }

    s.coarse0 = i >> 1;
    s.coarse1 =
        ((i & 1) == 0) ?
        (s.coarse0 > 0 ? s.coarse0 - 1 : s.coarse0) :
        (s.coarse0 + 1 < coarseN ? s.coarse0 + 1 : s.coarse0);

    if (s.coarse1 == s.coarse0) {
        s.weight0 = 1.0f;
        s.weight1 = 0.0f;
    } else {
        s.weight0 = 0.75f;
        s.weight1 = 0.25f;
    }
    return s;
}
}

void Multigrid::buildTransferWeights(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    Level& fine = gridLevels[fineLevel];

    fine.prolongWeight.assign(static_cast<size_t>(fine.cellCount), 0.0f);
    if (coarseLevel >= levels)
        return;

    const Level& coarse = gridLevels[coarseLevel];
    for (int j = 0; j < fine.ny; ++j) {
        const Stencil1D sy = transferStencil(j, fine.refineY, coarse.ny);
        for (int i = 0; i < fine.nx; ++i) {
            const int fineId = j * fine.nx + i;
            if (fine.solid[fineId] || fine.diag[fineId] == 0.0f)
                continue;
            const Stencil1D sx = transferStencil(i, fine.refineX, coarse.nx);

            const int coarseX[4] = {sx.coarse0, sx.coarse1, sx.coarse0, sx.coarse1};
            const int coarseY[4] = {sy.coarse0, sy.coarse0, sy.coarse1, sy.coarse1};
            const float weights[4] = {
                sx.weight0 * sy.weight0, sx.weight1 * sy.weight0,
                sx.weight0 * sy.weight1, sx.weight1 * sy.weight1};

            float weight = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (weights[k] == 0.0f)
                    continue;
                if (!coarse.solid[coarseY[k] * coarse.nx + coarseX[k]])
                    weight += weights[k];
            }
            fine.prolongWeight[fineId] = weight;
        }
    }
}

void Multigrid::buildCoefficients(Level& grid) {
    const int nx = grid.nx;
    const int ny = grid.ny;
    const float invDx2 = 1.0f / (grid.dx * grid.dx);
    const float invDy2 = 1.0f / (grid.dy * grid.dy);

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i) {
            const int id = row + i;

            if (grid.solid[id]) {
                grid.coefW[id] = grid.coefE[id] = 0.0f;
                grid.coefS[id] = grid.coefN[id] = 0.0f;
                grid.diag[id] = 0.0f;
                grid.invDiag[id] = 0.0f;
                continue;
            }
            // A link is only opened towards a fluid neighbour, so the solid
            // walls are baked into the stencil instead of patched afterwards
            const float coefW = (i > 0      && !grid.solid[id - 1])  ? invDx2 : 0.0f;
            const float coefE = (i < nx - 1 && !grid.solid[id + 1])  ? invDx2 : 0.0f;
            const float coefS = (j > 0      && !grid.solid[id - nx]) ? invDy2 : 0.0f;
            const float coefN = (j < ny - 1 && !grid.solid[id + nx]) ? invDy2 : 0.0f;

            float diag = coefW + coefE + coefS + coefN;

            // Outlet: p = 0 half a cell outside, hence the extra 2/dx^2
            if (i == nx - 1)
                diag += 2.0f * invDx2;

            grid.coefW[id] = coefW;
            grid.coefE[id] = coefE;
            grid.coefS[id] = coefS;
            grid.coefN[id] = coefN;

            if (diag > 0.0f) {
                grid.diag[id] = diag;
                grid.invDiag[id] = 1.0f / diag;
            } else {
                grid.diag[id] = 0.0f;
                grid.invDiag[id] = 0.0f;
            }
        }
    }
}

void Multigrid::setGeometry(const std::vector<uint8_t>& solid) {
    if (gridLevels.empty())
        buildHierarchy();

    std::copy(solid.begin(),
              solid.begin() + gridLevels[0].cellCount,
              gridLevels[0].solid.begin());

    // A coarse cell is solid only if every fine cell under it is solid
    for (int l = 1; l < levels; ++l) {
        const Level& fine = gridLevels[l - 1];
        Level& coarse = gridLevels[l];

        for (int j = 0; j < coarse.ny; ++j) {
            for (int i = 0; i < coarse.nx; ++i) {
                const int i0 = i * fine.refineX;
                const int i1 = std::min(i0 + fine.refineX - 1, fine.nx - 1);
                const int j0 = j * fine.refineY;
                const int j1 = std::min(j0 + fine.refineY - 1, fine.ny - 1);

                bool allSolid = true;
                for (int jj = j0; jj <= j1 && allSolid; ++jj)
                    for (int ii = i0; ii <= i1 && allSolid; ++ii)
                        if (!fine.solid[jj * fine.nx + ii])
                            allSolid = false;

                coarse.solid[j * coarse.nx + i] = allSolid ? 1 : 0;
            }
        }
    }

    for (int l = 0; l < levels; ++l)
        buildCoefficients(gridLevels[l]);

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
    // A build with the toolkit in it still runs on machines with no device
    // behind it, and every allocation on that path ends in abort(). Asking
    // first costs one call and turns a dead process into a CPU run.
    useCuda = enable && cudaDeviceAvailable();
    if (enable && !useCuda)
        std::fprintf(stderr,
                     "No usable CUDA device; the pressure solve runs on the "
                     "CPU.\n");
    if (useCuda && geometryReady)
        setGeometryCuda();
#else
    (void)enable;
    useCuda = false;
#endif
}

void Multigrid::smoothSOR(
    int level,
    float omega,
    int sweeps)
{
    Level& grid = gridLevels[level];
    const int nx = grid.nx;
    const int ny = grid.ny;

    float* const       pressure = grid.pressure.data();
    const float* const rhs      = grid.rhs.data();
    const float* const coefW    = grid.coefW.data();
    const float* const coefE    = grid.coefE.data();
    const float* const coefS    = grid.coefS.data();
    const float* const coefN    = grid.coefN.data();
    const float* const invDiag  = grid.invDiag.data();

#ifdef __AVX2__
    // Red-black ordering: only every second lane of a vector is written back
    const __m256i laneEven = _mm256_setr_epi32(-1, 0, -1, 0, -1, 0, -1, 0);
    const __m256i laneOdd  = _mm256_setr_epi32(0, -1, 0, -1, 0, -1, 0, -1);
    const __m256 omegaVec  = _mm256_set1_ps(omega);
#endif

    #pragma omp parallel if (ny >= PARALLEL_ROWS_MIN)
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        for (int color = 0; color < 2; ++color) {
            #pragma omp for schedule(static)
            for (int j = 0; j < ny; ++j) {
                const int row = j * nx;
                const int parity = color ^ (j & 1);

                int i = 0;
#ifdef __AVX2__
                const __m256i lane = parity ? laneOdd : laneEven;
                for (; runtime::avx2 && i + 8 <= nx; i += 8) {
                    const int id = row + i;

                    const __m256 pCentre =
                        _mm256_loadu_ps(pressure + id);
                    const __m256 pLeft =
                        _mm256_loadu_ps(pressure + id - 1);
                    const __m256 pRight =
                        _mm256_loadu_ps(pressure + id + 1);
                    const __m256 pBot =
                        _mm256_loadu_ps(pressure + id - nx);
                    const __m256 pTop =
                        _mm256_loadu_ps(pressure + id + nx);

                    __m256 sum =
                        _mm256_mul_ps(_mm256_loadu_ps(coefW + id), pLeft);
                    sum = _mm256_add_ps(sum,
                        _mm256_mul_ps(_mm256_loadu_ps(coefE + id), pRight));
                    sum = _mm256_add_ps(sum,
                        _mm256_mul_ps(_mm256_loadu_ps(coefS + id), pBot));
                    sum = _mm256_add_ps(sum,
                        _mm256_mul_ps(_mm256_loadu_ps(coefN + id), pTop));

                    const __m256 pNew =
                        _mm256_mul_ps(
                            _mm256_sub_ps(sum, _mm256_loadu_ps(rhs + id)),
                            _mm256_loadu_ps(invDiag + id));

                    const __m256 relaxed =
                        _mm256_add_ps(
                            pCentre,
                            _mm256_mul_ps(
                                omegaVec,
                                _mm256_sub_ps(pNew, pCentre)));

                    _mm256_maskstore_ps(pressure + id, lane, relaxed);
                }
#endif
                // Whatever the vector loop left, and on a build without AVX2
                // the whole row: same red-black stride, same arithmetic.
                for (int ii = i + parity; ii < nx; ii += 2) {
                    const int id = row + ii;
                    const float sum =
                        coefW[id] * pressure[id - 1] +
                        coefE[id] * pressure[id + 1] +
                        coefS[id] * pressure[id - nx] +
                        coefN[id] * pressure[id + nx];
                    const float pNew = (sum - rhs[id]) * invDiag[id];
                    pressure[id] += omega * (pNew - pressure[id]);
                }
            }
        }
    }
}

void Multigrid::computeResidual(int level) {
    Level& grid = gridLevels[level];
    const int nx = grid.nx;
    const int ny = grid.ny;

    const float* const pressure = grid.pressure.data();
    const float* const rhs      = grid.rhs.data();
    const float* const coefW    = grid.coefW.data();
    const float* const coefE    = grid.coefE.data();
    const float* const coefS    = grid.coefS.data();
    const float* const coefN    = grid.coefN.data();
    const float* const diag     = grid.diag.data();
    float* const       residual = grid.residual.data();

    #pragma omp parallel for schedule(static) if (ny >= PARALLEL_ROWS_MIN)
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;

        int i = 0;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= nx; i += 8) {
            const int id = row + i;
            const __m256 pCentre =
                _mm256_loadu_ps(pressure + id);
            const __m256 pLeft =
                _mm256_loadu_ps(pressure + id - 1);
            const __m256 pRight =
                _mm256_loadu_ps(pressure + id + 1);
            const __m256 pBot =
                _mm256_loadu_ps(pressure + id - nx);
            const __m256 pTop =
                _mm256_loadu_ps(pressure + id + nx);

            __m256 sum =
                _mm256_mul_ps(_mm256_loadu_ps(coefW + id), pLeft);
            sum = _mm256_add_ps(sum,
                _mm256_mul_ps(_mm256_loadu_ps(coefE + id), pRight));
            sum = _mm256_add_ps(sum,
                _mm256_mul_ps(_mm256_loadu_ps(coefS + id), pBot));
            sum = _mm256_add_ps(sum,
                _mm256_mul_ps(_mm256_loadu_ps(coefN + id), pTop));

            // Ap = (neighbour sum) - diag * p, residual = rhs - Ap
            const __m256 Ap =
                _mm256_sub_ps(
                    sum,
                    _mm256_mul_ps(_mm256_loadu_ps(diag + id), pCentre));

            _mm256_storeu_ps(
                residual + id,
                _mm256_sub_ps(_mm256_loadu_ps(rhs + id), Ap));
        }
#endif

        for (; i < nx; ++i) {
            const int id = row + i;
            const float sum =
                coefW[id] * pressure[id - 1] +
                coefE[id] * pressure[id + 1] +
                coefS[id] * pressure[id - nx] +
                coefN[id] * pressure[id + nx];
            residual[id] = rhs[id] - (sum - diag[id] * pressure[id]);
        }
    }
}

float Multigrid::computeVectorNorm(const float* values, int count) {
    double total = 0.0;
    int start = 0;
#ifdef __AVX2__
    // Same runtime switch as the two kernels above: with AVX2 turned off the
    // whole vector block is skipped and start stays at zero, so the scalar
    // loop below covers every element rather than only the tail.
    if (runtime::avx2) {
    #pragma omp parallel reduction(+ : total) if (count >= 8192)
    {
        __m256 acc = _mm256_setzero_ps();

        #pragma omp for schedule(static) nowait
        for (int i = 0; i <= count - 8; i += 8) {
            const __m256 x = _mm256_loadu_ps(values + i);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(x, x));
        }
        total += static_cast<double>(horizontalSum(acc));
    }
    start = (count / 8) * 8;
    }
#endif
    for (int i = start; i < count; ++i)
        total += static_cast<double>(values[i]) * static_cast<double>(values[i]);

    return static_cast<float>(std::sqrt(total));
}

float Multigrid::computeResidualNorm(int level) const {
    const Level& grid = gridLevels[level];
    return computeVectorNorm(grid.residual.data(), grid.cellCount);
}

// Grid transfer. The restriction is the exact transpose of the prolongation
// divided by the number of fine cells per coarse cell: R = P^T / (refineX*refineY).
// The previous implementation restricted with a plain 2x2 average while
// prolongating bilinearly, so R was not P^T, and for some grid sizes the
// V-cycle amplified the error instead of reducing it - which is exactly what
// blew the solver up on 100x100 and 64x128 while 128x128 worked fine
// (during the tests, u know ehehe).

void Multigrid::restrictField(int fineLevel, const float* fineSrc) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;

    const Level& fine = gridLevels[fineLevel];
    Level& coarse = gridLevels[coarseLevel];

    const int refineX = fine.refineX;
    const int refineY = fine.refineY;
    const float scale = 1.0f / static_cast<float>(refineX * refineY);

    float* const coarseRhs = coarse.rhs.data();
    const float* const prolongWeight = fine.prolongWeight.data();

    #pragma omp parallel for schedule(static) if (coarse.ny >= PARALLEL_ROWS_MIN)
    for (int j = 0; j < coarse.ny; ++j) {
        for (int i = 0; i < coarse.nx; ++i) {
            const int coarseId = j * coarse.nx + i;

            if (coarse.solid[coarseId] || coarse.diag[coarseId] == 0.0f) {
                coarseRhs[coarseId] = 0.0f;
                continue;
            }
            // Fine cells that can carry a non-zero weight into this coarse cell
            const int i0 = (refineX == 1) ? i : std::max(0, 2 * i - 1);
            const int i1 = (refineX == 1) ? i : std::min(fine.nx - 1, 2 * i + 2);
            const int j0 = (refineY == 1) ? j : std::max(0, 2 * j - 1);
            const int j1 = (refineY == 1) ? j : std::min(fine.ny - 1, 2 * j + 2);

            float sum = 0.0f;
            for (int jj = j0; jj <= j1; ++jj) {
                const Stencil1D sy = transferStencil(jj, refineY, coarse.ny);
                float wy = 0.0f;
                if (sy.coarse0 == j) wy += sy.weight0;
                if (sy.coarse1 == j) wy += sy.weight1;
                if (wy == 0.0f)
                    continue;

                const int fineRow = jj * fine.nx;
                for (int ii = i0; ii <= i1; ++ii) {
                    const int fineId = fineRow + ii;
                    const float norm = prolongWeight[fineId];
                    if (norm <= 0.0f)
                        continue;

                    const Stencil1D sx = transferStencil(ii, refineX, coarse.nx);
                    float wx = 0.0f;
                    if (sx.coarse0 == i) wx += sx.weight0;
                    if (sx.coarse1 == i) wx += sx.weight1;
                    if (wx == 0.0f)
                        continue;

                    sum += (wx * wy / norm) * fineSrc[fineId];
                }
            }
            coarseRhs[coarseId] = sum * scale;
        }
    }
}

void Multigrid::restrictResidual(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;
    gridLevels[coarseLevel].pressure.zero();
    restrictField(fineLevel, gridLevels[fineLevel].residual.data());
}

void Multigrid::restrictRHS(int fineLevel) {
    restrictField(fineLevel, gridLevels[fineLevel].rhs.data());
}

void Multigrid::prolongateCorrection(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;
    const int fineLevel = coarseLevel - 1;
    Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];

    float* const finePressure = fine.pressure.data();
    const float* const coarsePressure = coarse.pressure.data();

    #pragma omp parallel for schedule(static) if (fine.ny >= PARALLEL_ROWS_MIN)
    for (int j = 0; j < fine.ny; ++j) {
        const Stencil1D sy = transferStencil(j, fine.refineY, coarse.ny);
        for (int i = 0; i < fine.nx; ++i) {
            const int fineId = j * fine.nx + i;
            const float norm = fine.prolongWeight[fineId];
            if (norm <= 0.0f)
                continue;

            const Stencil1D sx = transferStencil(i, fine.refineX, coarse.nx);

            const int coarseX[4] = {sx.coarse0, sx.coarse1, sx.coarse0, sx.coarse1};
            const int coarseY[4] = {sy.coarse0, sy.coarse0, sy.coarse1, sy.coarse1};
            const float weights[4] = {
                sx.weight0 * sy.weight0, sx.weight1 * sy.weight0,
                sx.weight0 * sy.weight1, sx.weight1 * sy.weight1};

            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (weights[k] == 0.0f)
                    continue;
                const int coarseId = coarseY[k] * coarse.nx + coarseX[k];
                if (!coarse.solid[coarseId])
                    value += weights[k] * coarsePressure[coarseId];
            }

            finePressure[fineId] += value / norm;
        }
    }
}

void Multigrid::prolongateSolution(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;
    const int fineLevel = coarseLevel - 1;
    Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];

    fine.pressure.zero();

    float* const finePressure = fine.pressure.data();
    const float* const coarsePressure = coarse.pressure.data();

    #pragma omp parallel for schedule(static) if (fine.ny >= PARALLEL_ROWS_MIN)
    for (int j = 0; j < fine.ny; ++j) {
        const Stencil1D sy = transferStencil(j, fine.refineY, coarse.ny);
        for (int i = 0; i < fine.nx; ++i) {
            const int fineId = j * fine.nx + i;
            const float norm = fine.prolongWeight[fineId];
            if (norm <= 0.0f)
                continue;

            const Stencil1D sx = transferStencil(i, fine.refineX, coarse.nx);

            const int coarseX[4] = {sx.coarse0, sx.coarse1, sx.coarse0, sx.coarse1};
            const int coarseY[4] = {sy.coarse0, sy.coarse0, sy.coarse1, sy.coarse1};
            const float weights[4] = {
                sx.weight0 * sy.weight0, sx.weight1 * sy.weight0,
                sx.weight0 * sy.weight1, sx.weight1 * sy.weight1};

            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                if (weights[k] == 0.0f)
                    continue;
                const int coarseId = coarseY[k] * coarse.nx + coarseX[k];
                if (!coarse.solid[coarseId])
                    value += weights[k] * coarsePressure[coarseId];
            }

            finePressure[fineId] = value / norm;
        }
    }
}

void Multigrid::vCycle(
    int level,
    float smootherOmega,
    float coarseOmega)
{
    if (level == levels - 1) {
        smoothSOR(level, coarseOmega,
                  coarseSweeps(gridLevels[level].nx, gridLevels[level].ny));
        return;
    }
    smoothSOR(level, smootherOmega, PRE_SMOOTH_SWEEPS);
    computeResidual(level);
    restrictResidual(level);
    vCycle(level + 1, smootherOmega, coarseOmega);
    prolongateCorrection(level + 1);
    smoothSOR(level, smootherOmega, POST_SMOOTH_SWEEPS);
}

void Multigrid::fullMultigrid(float smootherOmega, float coarseOmega) {
    const int coarsest = levels - 1;
    if (coarsest == 0) {
        smoothSOR(0, coarseOmega,
                  coarseSweeps(gridLevels[0].nx, gridLevels[0].ny));
        return;
    }
    for (int level = 0; level < coarsest; ++level)
        restrictRHS(level);

    gridLevels[coarsest].pressure.zero();
    smoothSOR(coarsest, coarseOmega,
              coarseSweeps(gridLevels[coarsest].nx, gridLevels[coarsest].ny));

    for (int level = coarsest; level > 0; --level) {
        prolongateSolution(level);
        vCycle(level - 1, smootherOmega, coarseOmega);
    }
}

float Multigrid::solve(
    std::vector<float>& pressure,
    const std::vector<float>& rhs,
    float smootherOmega,
    float coarseOmega,
    int maxCycles,
    float tolerance,
    float rhsScale)
{
    if (!geometryReady) {
        std::fprintf(stderr, "Multigrid::solve called before setGeometry\n");
        return 0.0f;
    }

#ifdef USE_CUDA
    if (useCuda)
        return solveCuda(
            pressure,
            rhs,
            smootherOmega,
            coarseOmega,
            maxCycles,
            tolerance,
            rhsScale);
#endif

    Level& finest = gridLevels[0];
    const int cellCount = finest.cellCount;

    float* const finestPressure = finest.pressure.data();
    float* const finestRhs = finest.rhs.data();

    // Solid cells have a zero diagonal, they take no part in the solve
    #pragma omp parallel for schedule(static)
    for (int id = 0; id < cellCount; ++id) {
        const bool active = (finest.diag[id] > 0.0f);
        finestPressure[id] = active ? pressure[id] : 0.0f;
        finestRhs[id] = active ? rhs[id] : 0.0f;
    }

    const float rhsNorm = (rhsScale > 0.0f) ? rhsScale : computeVectorNorm(finestRhs, cellCount);
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

    // The first cycle runs whatever the residual says, because the field comes
    // from the previous step and one V-cycle costs less than the residual pass
    // that would find out it was not needed. So that pass is not made at all.
    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        if (cycle > 0) {
            computeResidual(0);
            relative = computeResidualNorm(0) / scale;
            if (relative < tolerance)
                break;
        }
        vCycle(0, smootherOmega, coarseOmega);
        ++lastCycles;
    }

    if (lastCycles > 0) {
        computeResidual(0);
        relative = computeResidualNorm(0) / scale;
    }

    std::copy(finestPressure, finestPressure + cellCount, pressure.begin());
    return relative;
}
