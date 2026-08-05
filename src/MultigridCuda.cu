#include "Multigrid.hpp"
#ifdef USE_CUDA
#include "MultigridCuda.cuh"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static const int BLOCK_X = 32;
static const int BLOCK_Y = 8;
static const int NORM_BLOCK = 256;
static const int MAX_NORM_BLOCKS = 1024;
static const int PRE_SMOOTH_SWEEPS = 2;
static const int POST_SMOOTH_SWEEPS = 2;
static const int COARSE_SMOOTH_SWEEPS = 50;

namespace {
// Same rule as on the CPU: the coarsest level needs enough sweeps to act as a
// solver rather than a smoother
int coarseSweeps(int nx, int ny) {
    const int wanted = 2 * (nx > ny ? nx : ny);
    if (wanted < COARSE_SMOOTH_SWEEPS) return COARSE_SMOOTH_SWEEPS;
    return (wanted > 400) ? 400 : wanted;
}

void checkCuda(cudaError_t status, const char* what, const char* file, int line) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "CUDA error at %s:%d during %s: %s\n",
                     file, line, what, cudaGetErrorString(status));
        std::abort();
    }
}

void checkLaunch(const char* what, const char* file, int line) {
    checkCuda(cudaGetLastError(), what, file, line);
}
}

#define CUDA_CHECK(call) checkCuda((call), #call, __FILE__, __LINE__)
#define CUDA_CHECK_LAUNCH(name) checkLaunch(name, __FILE__, __LINE__)

__global__ void smoothSORKernel(
    int nx,
    int ny,
    float* __restrict__ pressure,
    const float* __restrict__ rhs,
    const float* __restrict__ coefW,
    const float* __restrict__ coefE,
    const float* __restrict__ coefS,
    const float* __restrict__ coefN,
    const float* __restrict__ invDiag,
    float omega,
    int color)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= nx || j >= ny)
        return;
    if (((i + j) & 1) != color)
        return;

    const int id = j * nx + i;
    const float sum =
        coefW[id] * pressure[id - 1] +
        coefE[id] * pressure[id + 1] +
        coefS[id] * pressure[id - nx] +
        coefN[id] * pressure[id + nx];

    const float pNew = (sum - rhs[id]) * invDiag[id];
    pressure[id] += omega * (pNew - pressure[id]);
}

__global__ void computeResidualKernel(
    int nx,
    int ny,
    const float* __restrict__ pressure,
    const float* __restrict__ rhs,
    const float* __restrict__ coefW,
    const float* __restrict__ coefE,
    const float* __restrict__ coefS,
    const float* __restrict__ coefN,
    const float* __restrict__ diag,
    float* __restrict__ residual)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= nx || j >= ny)
        return;

    const int id = j * nx + i;
    const float sum =
        coefW[id] * pressure[id - 1] +
        coefE[id] * pressure[id + 1] +
        coefS[id] * pressure[id - nx] +
        coefN[id] * pressure[id + nx];

    residual[id] = rhs[id] - (sum - diag[id] * pressure[id]);
}

// The two coarse cells a fine cell interpolates from, and their weights
struct Stencil1D {
    int coarse0, coarse1;
    float weight0, weight1;
};

static __device__ inline Stencil1D transferStencil(int i, int refine, int coarseN) {
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

__global__ void restrictKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    int refineX,
    int refineY,
    const float* __restrict__ fineSrc,
    const float* __restrict__ fineProlongWeight,
    const uint8_t* __restrict__ coarseSolid,
    const float* __restrict__ coarseDiag,
    float* __restrict__ coarseRhs)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= coarseNx || j >= coarseNy)
        return;

    const int coarseId = j * coarseNx + i;

    if (coarseSolid[coarseId] || coarseDiag[coarseId] == 0.0f) {
        coarseRhs[coarseId] = 0.0f;
        return;
    }

    // Fine cells that can carry a non-zero weight into this coarse cell
    const int i0 = (refineX == 1) ? i : max(0, 2 * i - 1);
    const int i1 = (refineX == 1) ? i : min(fineNx - 1, 2 * i + 2);
    const int j0 = (refineY == 1) ? j : max(0, 2 * j - 1);
    const int j1 = (refineY == 1) ? j : min(fineNy - 1, 2 * j + 2);

    float sum = 0.0f;
    for (int jj = j0; jj <= j1; ++jj) {
        const Stencil1D sy = transferStencil(jj, refineY, coarseNy);
        float wy = 0.0f;
        if (sy.coarse0 == j) wy += sy.weight0;
        if (sy.coarse1 == j) wy += sy.weight1;
        if (wy == 0.0f)
            continue;

        const int fineRow = jj * fineNx;
        for (int ii = i0; ii <= i1; ++ii) {
            const int fineId = fineRow + ii;
            const float norm = fineProlongWeight[fineId];
            if (norm <= 0.0f)
                continue;

            const Stencil1D sx = transferStencil(ii, refineX, coarseNx);
            float wx = 0.0f;
            if (sx.coarse0 == i) wx += sx.weight0;
            if (sx.coarse1 == i) wx += sx.weight1;
            if (wx == 0.0f)
                continue;

            sum += (wx * wy / norm) * fineSrc[fineId];
        }
    }

    coarseRhs[coarseId] = sum / static_cast<float>(refineX * refineY);
}

__global__ void prolongateKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    int refineX,
    int refineY,
    float* __restrict__ finePressure,
    const float* __restrict__ coarsePressure,
    const float* __restrict__ fineProlongWeight,
    const uint8_t* __restrict__ coarseSolid,
    int addMode)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= fineNx || j >= fineNy)
        return;

    const int fineId = j * fineNx + i;
    const float norm = fineProlongWeight[fineId];
    if (norm <= 0.0f) {
        if (!addMode)
            finePressure[fineId] = 0.0f;
        return;
    }

    const Stencil1D sx = transferStencil(i, refineX, coarseNx);
    const Stencil1D sy = transferStencil(j, refineY, coarseNy);

    const int coarseX[4] = {sx.coarse0, sx.coarse1, sx.coarse0, sx.coarse1};
    const int coarseY[4] = {sy.coarse0, sy.coarse0, sy.coarse1, sy.coarse1};
    const float weights[4] = {
        sx.weight0 * sy.weight0, sx.weight1 * sy.weight0,
        sx.weight0 * sy.weight1, sx.weight1 * sy.weight1};

    float value = 0.0f;
    #pragma unroll
    for (int k = 0; k < 4; ++k) {
        if (weights[k] == 0.0f)
            continue;
        const int coarseId = coarseY[k] * coarseNx + coarseX[k];
        if (!coarseSolid[coarseId])
            value += weights[k] * coarsePressure[coarseId];
    }

    if (addMode)
        finePressure[fineId] += value / norm;
    else
        finePressure[fineId] = value / norm;
}

__global__ void zeroSolidPressureKernel(
    int cellCount,
    float* __restrict__ pressure,
    float* __restrict__ rhs,
    const float* __restrict__ invDiag)
{
    const int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= cellCount)
        return;
    if (invDiag[id] == 0.0f) {
        pressure[id] = 0.0f;
        rhs[id] = 0.0f;
    }
}

__global__ void computeNormKernel(
    int cellCount,
    const float* __restrict__ values,
    float* __restrict__ partial)
{
    __shared__ float shared[NORM_BLOCK];

    float sum = 0.0f;
    for (int id = blockIdx.x * blockDim.x + threadIdx.x;
         id < cellCount;
         id += blockDim.x * gridDim.x) {
        const float x = values[id];
        sum += x * x;
    }

    shared[threadIdx.x] = sum;
    __syncthreads();

    for (unsigned stride = blockDim.x / 2u; stride > 0u; stride >>= 1) {
        if (threadIdx.x < stride)
            shared[threadIdx.x] += shared[threadIdx.x + stride];
        __syncthreads();
    }

    if (threadIdx.x == 0)
        partial[blockIdx.x] = shared[0];
}

// Device memory. The previous version called cudaMalloc/cudaFree for every
// level on every pressure solve, i.e. tens of allocations per time step, and
// that dominated the GPU path completely. Here the whole hierarchy is allocated
// once when the geometry is set and released in the destructor. MUCH better.

void Multigrid::freeDevice() {
    for (DeviceLevel& d : deviceLevels) {
        if (d.pressureAlloc) cudaFree(d.pressureAlloc);
        if (d.residualAlloc) cudaFree(d.residualAlloc);
        if (d.rhs)           cudaFree(d.rhs);
        if (d.coefW)         cudaFree(d.coefW);
        if (d.coefE)         cudaFree(d.coefE);
        if (d.coefS)         cudaFree(d.coefS);
        if (d.coefN)         cudaFree(d.coefN);
        if (d.diag)          cudaFree(d.diag);
        if (d.invDiag)       cudaFree(d.invDiag);
        if (d.solid)         cudaFree(d.solid);
        if (d.prolongWeight) cudaFree(d.prolongWeight);
        d = DeviceLevel{};
    }
    deviceLevels.clear();

    if (deviceReduceBuffer) {
        cudaFree(deviceReduceBuffer);
        deviceReduceBuffer = nullptr;
    }
    if (hostReduceBuffer) {
        cudaFreeHost(hostReduceBuffer);
        hostReduceBuffer = nullptr;
    }
    deviceReady = false;
}

void Multigrid::allocateDevice() {
    freeDevice();
    deviceLevels.resize(levels);

    for (int l = 0; l < levels; ++l) {
        const Level& grid = gridLevels[l];
        DeviceLevel& d = deviceLevels[l];

        d.halo = (grid.nx > 8) ? grid.nx : 8;
        const size_t padded =
            static_cast<size_t>(grid.cellCount) + 2u * d.halo + 16u;
        const size_t plain = static_cast<size_t>(grid.cellCount) + 16u;

        CUDA_CHECK(cudaMalloc(&d.pressureAlloc, padded * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.residualAlloc, padded * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.pressureAlloc, 0, padded * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.residualAlloc, 0, padded * sizeof(float)));
        d.pressure = d.pressureAlloc + d.halo;
        d.residual = d.residualAlloc + d.halo;

        CUDA_CHECK(cudaMalloc(&d.rhs, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.coefW, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.coefE, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.coefS, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.coefN, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.diag, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.invDiag, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.solid,
                              static_cast<size_t>(grid.cellCount) * sizeof(uint8_t)));
        CUDA_CHECK(cudaMalloc(&d.prolongWeight, plain * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.rhs, 0, plain * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.prolongWeight, 0, plain * sizeof(float)));
    }

    int blocks = (gridLevels[0].cellCount + NORM_BLOCK - 1) / NORM_BLOCK;
    if (blocks > MAX_NORM_BLOCKS)
        blocks = MAX_NORM_BLOCKS;
    if (blocks < 1)
        blocks = 1;
    reduceBlocks = blocks;

    CUDA_CHECK(cudaMalloc(&deviceReduceBuffer,
                          static_cast<size_t>(reduceBlocks) * sizeof(float)));
    CUDA_CHECK(cudaMallocHost(&hostReduceBuffer,
                              static_cast<size_t>(reduceBlocks) * sizeof(float)));
}

void Multigrid::setGeometryCuda() {
    allocateDevice();
    for (int l = 0; l < levels; ++l) {
        const Level& grid = gridLevels[l];
        DeviceLevel& d = deviceLevels[l];
        const size_t plain = static_cast<size_t>(grid.cellCount) + 16u;

        CUDA_CHECK(cudaMemcpy(d.coefW, grid.coefW.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.coefE, grid.coefE.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.coefS, grid.coefS.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.coefN, grid.coefN.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.diag, grid.diag.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.invDiag, grid.invDiag.data(),
                              plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.solid, grid.solid.data(),
                              static_cast<size_t>(grid.cellCount) * sizeof(uint8_t),
                              cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.prolongWeight, grid.prolongWeight.data(),
                              static_cast<size_t>(grid.cellCount) * sizeof(float),
                              cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemset(d.pressureAlloc, 0,
                              (static_cast<size_t>(grid.cellCount)
                               + 2u * d.halo + 16u) * sizeof(float)));
    }

    deviceReady = true;
}

void Multigrid::smoothSORCuda(
    int level,
    float omega,
    int sweeps)
{
    const Level& grid = gridLevels[level];
    const DeviceLevel& d = deviceLevels[level];

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((grid.nx + BLOCK_X - 1) / BLOCK_X,
                          (grid.ny + BLOCK_Y - 1) / BLOCK_Y);

    for (int sweep = 0; sweep < sweeps; ++sweep) {
        for (int color = 0; color < 2; ++color) {
            // Consecutive launches on the same stream are ordered, so the black
            // half sweep is guaranteed to observe every red update. The old code
            // relied on the same property but also ran a separate applyBC kernel
            // that wrote the cells its neighbours were reading, which is exactly
            // where the GPU and CPU results used to diverge.
            smoothSORKernel<<<launchGrid, block>>>(
                grid.nx, grid.ny, d.pressure, d.rhs,
                d.coefW, d.coefE, d.coefS, d.coefN, d.invDiag,
                omega, color);
            CUDA_CHECK_LAUNCH("smoothSORKernel");
        }
    }
}

void Multigrid::computeResidualCuda(int level) {
    const Level& grid = gridLevels[level];
    const DeviceLevel& d = deviceLevels[level];

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((grid.nx + BLOCK_X - 1) / BLOCK_X,
                          (grid.ny + BLOCK_Y - 1) / BLOCK_Y);

    computeResidualKernel<<<launchGrid, block>>>(
        grid.nx, grid.ny, d.pressure, d.rhs,
        d.coefW, d.coefE, d.coefS, d.coefN, d.diag, d.residual);
    CUDA_CHECK_LAUNCH("computeResidualKernel");
}

float Multigrid::computeResidualNormCuda(int level) {
    const Level& grid = gridLevels[level];
    const DeviceLevel& d = deviceLevels[level];

    computeNormKernel<<<reduceBlocks, NORM_BLOCK>>>(
        grid.cellCount, d.residual, deviceReduceBuffer);
    CUDA_CHECK_LAUNCH("computeNormKernel");

    CUDA_CHECK(cudaMemcpy(hostReduceBuffer, deviceReduceBuffer,
                          static_cast<size_t>(reduceBlocks) * sizeof(float),
                          cudaMemcpyDeviceToHost));

    double total = 0.0;
    for (int b = 0; b < reduceBlocks; ++b)
        total += hostReduceBuffer[b];
    return static_cast<float>(std::sqrt(total));
}

float Multigrid::computeRhsNormCuda(int level) {
    const Level& grid = gridLevels[level];
    const DeviceLevel& d = deviceLevels[level];

    computeNormKernel<<<reduceBlocks, NORM_BLOCK>>>(
        grid.cellCount, d.rhs, deviceReduceBuffer);
    CUDA_CHECK_LAUNCH("computeNormKernel");

    CUDA_CHECK(cudaMemcpy(hostReduceBuffer, deviceReduceBuffer,
                          static_cast<size_t>(reduceBlocks) * sizeof(float),
                          cudaMemcpyDeviceToHost));

    double total = 0.0;
    for (int b = 0; b < reduceBlocks; ++b)
        total += hostReduceBuffer[b];
    return static_cast<float>(std::sqrt(total));
}

void Multigrid::restrictResidualCuda(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;

    const Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];
    const DeviceLevel& dFine = deviceLevels[fineLevel];
    const DeviceLevel& dCoarse = deviceLevels[coarseLevel];

    // Clear the coarse solution: the V-cycle solves for a correction there
    CUDA_CHECK(cudaMemset(dCoarse.pressure, 0,
                          static_cast<size_t>(coarse.cellCount) * sizeof(float)));

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((coarse.nx + BLOCK_X - 1) / BLOCK_X,
                          (coarse.ny + BLOCK_Y - 1) / BLOCK_Y);

    restrictKernel<<<launchGrid, block>>>(
        fine.nx, fine.ny, coarse.nx, coarse.ny, fine.refineX, fine.refineY,
        dFine.residual, dFine.prolongWeight,
        dCoarse.solid, dCoarse.diag, dCoarse.rhs);
    CUDA_CHECK_LAUNCH("restrictKernel(residual)");
}

void Multigrid::restrictRHSCuda(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;

    const Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];
    const DeviceLevel& dFine = deviceLevels[fineLevel];
    const DeviceLevel& dCoarse = deviceLevels[coarseLevel];

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((coarse.nx + BLOCK_X - 1) / BLOCK_X,
                          (coarse.ny + BLOCK_Y - 1) / BLOCK_Y);

    restrictKernel<<<launchGrid, block>>>(
        fine.nx, fine.ny, coarse.nx, coarse.ny, fine.refineX, fine.refineY,
        dFine.rhs, dFine.prolongWeight,
        dCoarse.solid, dCoarse.diag, dCoarse.rhs);
    CUDA_CHECK_LAUNCH("restrictKernel(rhs)");
}

void Multigrid::prolongateCorrectionCuda(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;

    const int fineLevel = coarseLevel - 1;
    const Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];
    const DeviceLevel& dFine = deviceLevels[fineLevel];
    const DeviceLevel& dCoarse = deviceLevels[coarseLevel];

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((fine.nx + BLOCK_X - 1) / BLOCK_X,
                          (fine.ny + BLOCK_Y - 1) / BLOCK_Y);

    prolongateKernel<<<launchGrid, block>>>(
        fine.nx, fine.ny, coarse.nx, coarse.ny, fine.refineX, fine.refineY,
        dFine.pressure, dCoarse.pressure, dFine.prolongWeight,
        dCoarse.solid, 1);
    CUDA_CHECK_LAUNCH("prolongateKernel(correction)");
}

void Multigrid::prolongateSolutionCuda(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;

    const int fineLevel = coarseLevel - 1;
    const Level& fine = gridLevels[fineLevel];
    const Level& coarse = gridLevels[coarseLevel];
    const DeviceLevel& dFine = deviceLevels[fineLevel];
    const DeviceLevel& dCoarse = deviceLevels[coarseLevel];

    const dim3 block(BLOCK_X, BLOCK_Y);
    const dim3 launchGrid((fine.nx + BLOCK_X - 1) / BLOCK_X,
                          (fine.ny + BLOCK_Y - 1) / BLOCK_Y);

    prolongateKernel<<<launchGrid, block>>>(
        fine.nx, fine.ny, coarse.nx, coarse.ny, fine.refineX, fine.refineY,
        dFine.pressure, dCoarse.pressure, dFine.prolongWeight,
        dCoarse.solid, 0);
    CUDA_CHECK_LAUNCH("prolongateKernel(solution)");
}

void Multigrid::vCycleCuda(
    int level,
    float smootherOmega,
    float coarseOmega)
{
    if (level == levels - 1) {
        smoothSORCuda(level, coarseOmega,
                      coarseSweeps(gridLevels[level].nx, gridLevels[level].ny));
        return;
    }

    smoothSORCuda(level, smootherOmega, PRE_SMOOTH_SWEEPS);
    computeResidualCuda(level);
    restrictResidualCuda(level);
    vCycleCuda(level + 1, smootherOmega, coarseOmega);
    prolongateCorrectionCuda(level + 1);
    smoothSORCuda(level, smootherOmega, POST_SMOOTH_SWEEPS);
}

void Multigrid::fullMultigridCuda(float smootherOmega, float coarseOmega) {
    const int coarsest = levels - 1;
    if (coarsest == 0) {
        smoothSORCuda(0, coarseOmega,
                      coarseSweeps(gridLevels[0].nx, gridLevels[0].ny));
        return;
    }

    for (int level = 0; level < coarsest; ++level)
        restrictRHSCuda(level);

    CUDA_CHECK(cudaMemset(
        deviceLevels[coarsest].pressure, 0,
        static_cast<size_t>(gridLevels[coarsest].cellCount) * sizeof(float)));
    smoothSORCuda(coarsest, coarseOmega,
                  coarseSweeps(gridLevels[coarsest].nx,
                               gridLevels[coarsest].ny));

    for (int level = coarsest; level > 0; --level) {
        prolongateSolutionCuda(level);
        vCycleCuda(level - 1, smootherOmega, coarseOmega);
    }
}

float Multigrid::solveCuda(
    std::vector<float>& pressure,
    const std::vector<float>& rhs,
    float smootherOmega,
    float coarseOmega,
    int maxCycles,
    float tolerance)
{
    if (!deviceReady) {
        std::fprintf(stderr, "Multigrid::solveCuda called before setGeometry\n");
        return 0.0f;
    }

    const Level& finest = gridLevels[0];
    const DeviceLevel& dFinest = deviceLevels[0];
    const size_t bytes = static_cast<size_t>(finest.cellCount) * sizeof(float);

    CUDA_CHECK(cudaMemcpy(dFinest.rhs, rhs.data(), bytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(dFinest.pressure, pressure.data(), bytes,
                          cudaMemcpyHostToDevice));

    // Solid cells have a zero diagonal, they take no part in the solve
    {
        const int block = 256;
        const int launchGrid = (finest.cellCount + block - 1) / block;
        zeroSolidPressureKernel<<<launchGrid, block>>>(
            finest.cellCount, dFinest.pressure, dFinest.rhs, dFinest.invDiag);
        CUDA_CHECK_LAUNCH("zeroSolidPressureKernel");
    }

    const float rhsNorm = computeRhsNormCuda(0);
    const float scale = (rhsNorm > 1e-20f) ? rhsNorm : 1.0f;

    if (firstSolve) {
        // Nested iteration once, to build a good field from nothing
        fullMultigridCuda(smootherOmega, coarseOmega);
        firstSolve = false;
    }

    lastCycles = 0;
    float relative = 1.0f;

    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        computeResidualCuda(0);
        relative = computeResidualNormCuda(0) / scale;
        if (relative < tolerance)
            break;

        vCycleCuda(0, smootherOmega, coarseOmega);
        ++lastCycles;
    }

    if (lastCycles > 0) {
        computeResidualCuda(0);
        relative = computeResidualNormCuda(0) / scale;
    }

    CUDA_CHECK(cudaMemcpy(pressure.data(), dFinest.pressure, bytes,
                          cudaMemcpyDeviceToHost));
    return relative;
}

#endif // USE_CUDA
