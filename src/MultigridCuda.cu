#include "Multigrid.hpp"
#ifdef USE_CUDA
#include "MultigridCuda.cuh"
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace {

constexpr int kBlockX = 32;
constexpr int kBlockY = 8;
constexpr int kNormBlock = 256;
constexpr int kMaxNormBlocks = 1024;
constexpr int kPreSmooth    = 2;
constexpr int kPostSmooth   = 2;
constexpr int kCoarseSmooth = 50;

inline int coarseSweeps(int nx, int ny) {
    const int wanted = 2 * (nx > ny ? nx : ny);
    if (wanted < kCoarseSmooth) return kCoarseSmooth;
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
__global__ void mgSmoothKernel(int nx, int ny,
                               float* __restrict__ p,
                               const float* __restrict__ rhs,
                               const float* __restrict__ cW,
                               const float* __restrict__ cE,
                               const float* __restrict__ cS,
                               const float* __restrict__ cN,
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
    const float num =
        cW[id] * p[id - 1] +
        cE[id] * p[id + 1] +
        cS[id] * p[id - nx] +
        cN[id] * p[id + nx];

    const float pNew = (num - rhs[id]) * invDiag[id];
    p[id] += omega * (pNew - p[id]);
}
__global__ void mgResidualKernel(int nx, int ny,
                                 const float* __restrict__ p,
                                 const float* __restrict__ rhs,
                                 const float* __restrict__ cW,
                                 const float* __restrict__ cE,
                                 const float* __restrict__ cS,
                                 const float* __restrict__ cN,
                                 const float* __restrict__ diag,
                                 float* __restrict__ res)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= nx || j >= ny)
        return;
    const int id = j * nx + i;
    const float num =
        cW[id] * p[id - 1] +
        cE[id] * p[id + 1] +
        cS[id] * p[id - nx] +
        cN[id] * p[id + nx];

    res[id] = rhs[id] - (num - diag[id] * p[id]);
}

struct MgStencil1D {
    int   c0, c1;
    float w0, w1;
};

__device__ inline MgStencil1D mgTransferStencil(int i, int ref, int coarseN) {
    MgStencil1D s;
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

__global__ void mgRestrictKernel(int fineNx, int fineNy,
                                 int coarseNx, int coarseNy,
                                 int refX, int refY,
                                 const float* __restrict__ fineSrc,
                                 const float* __restrict__ finePWeight,
                                 const uint8_t* __restrict__ coarseSolid,
                                 const float* __restrict__ coarseDiag,
                                 float* __restrict__ coarseRhs)
{
    const int I = blockIdx.x * blockDim.x + threadIdx.x;
    const int J = blockIdx.y * blockDim.y + threadIdx.y;
    if (I >= coarseNx || J >= coarseNy)
        return;

    const int cid = J * coarseNx + I;

    if (coarseSolid[cid] || coarseDiag[cid] == 0.0f) {
        coarseRhs[cid] = 0.0f;
        return;
    }

    const int i0 = (refX == 1) ? I : max(0, 2 * I - 1);
    const int i1 = (refX == 1) ? I : min(fineNx - 1, 2 * I + 2);
    const int j0 = (refY == 1) ? J : max(0, 2 * J - 1);
    const int j1 = (refY == 1) ? J : min(fineNy - 1, 2 * J + 2);

    float sum = 0.0f;
    for (int jj = j0; jj <= j1; ++jj) {
        const MgStencil1D sy = mgTransferStencil(jj, refY, coarseNy);
        float wy = 0.0f;
        if (sy.c0 == J) wy += sy.w0;
        if (sy.c1 == J) wy += sy.w1;
        if (wy == 0.0f)
            continue;
        const int frow = jj * fineNx;
        for (int ii = i0; ii <= i1; ++ii) {
            const int fid = frow + ii;
            const float norm = finePWeight[fid];
            if (norm <= 0.0f)
                continue;

            const MgStencil1D sx = mgTransferStencil(ii, refX, coarseNx);
            float wx = 0.0f;
            if (sx.c0 == I) wx += sx.w0;
            if (sx.c1 == I) wx += sx.w1;
            if (wx == 0.0f)
                continue;

            sum += (wx * wy / norm) * fineSrc[fid];
        }
    }

    coarseRhs[cid] = sum / static_cast<float>(refX * refY);
}

__global__ void mgProlongateKernel(int fineNx, int fineNy,
                                   int coarseNx, int coarseNy,
                                   int refX, int refY,
                                   float* __restrict__ fineP,
                                   const float* __restrict__ coarseP,
                                   const float* __restrict__ finePWeight,
                                   const uint8_t* __restrict__ coarseSolid,
                                   int addMode)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= fineNx || j >= fineNy)
        return;

    const int fid = j * fineNx + i;
    const float norm = finePWeight[fid];
    if (norm <= 0.0f) {
        if (!addMode)
            fineP[fid] = 0.0f;
        return;
    }

    const MgStencil1D sx = mgTransferStencil(i, refX, coarseNx);
    const MgStencil1D sy = mgTransferStencil(j, refY, coarseNy);

    const int   cx[4] = {sx.c0, sx.c1, sx.c0, sx.c1};
    const int   cy[4] = {sy.c0, sy.c0, sy.c1, sy.c1};
    const float w[4]  = {sx.w0 * sy.w0, sx.w1 * sy.w0,
                         sx.w0 * sy.w1, sx.w1 * sy.w1};

    float value = 0.0f;
    #pragma unroll
    for (int k = 0; k < 4; ++k) {
        if (w[k] == 0.0f)
            continue;
        const int cid = cy[k] * coarseNx + cx[k];
        if (!coarseSolid[cid])
            value += w[k] * coarseP[cid];
    }

    if (addMode)
        fineP[fid] += value / norm;
    else
        fineP[fid] = value / norm;
}

__global__ void mgMaskKernel(int n,
                             float* __restrict__ p,
                             float* __restrict__ rhs,
                             const float* __restrict__ invDiag)
{
    const int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= n)
        return;
    if (invDiag[id] == 0.0f) {
        p[id] = 0.0f;
        rhs[id] = 0.0f;
    }
}

__global__ void mgNormKernel(int n,
                             const float* __restrict__ v,
                             float* __restrict__ partial)
{
    __shared__ float shared[kNormBlock];

    float sum = 0.0f;
    for (int id = blockIdx.x * blockDim.x + threadIdx.x;
         id < n;
         id += blockDim.x * gridDim.x) {
        const float x = v[id];
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

// ---------------------------------------------------------------------------
// Device memory
//
// The previous version called cudaMalloc and cudaFree for every level on every
// single pressure solve, i.e. tens of allocations per time step. Allocation is
// a synchronising, driver-level operation and it dominated the GPU path
// completely. Here the whole hierarchy is allocated once, when the geometry is
// set, and released in the destructor. MUCH better.
// ---------------------------------------------------------------------------
void Multigrid::freeDevice() {
    for (DeviceLevel& d : dev) {
        if (d.pAlloc)   cudaFree(d.pAlloc);
        if (d.resAlloc) cudaFree(d.resAlloc);
        if (d.rhs)      cudaFree(d.rhs);
        if (d.cW)       cudaFree(d.cW);
        if (d.cE)       cudaFree(d.cE);
        if (d.cS)       cudaFree(d.cS);
        if (d.cN)       cudaFree(d.cN);
        if (d.diag)     cudaFree(d.diag);
        if (d.invDiag)  cudaFree(d.invDiag);
        if (d.solid)    cudaFree(d.solid);
        if (d.pWeight)  cudaFree(d.pWeight);
        d = DeviceLevel{};
    }
    dev.clear();

    if (dReduce) {
        cudaFree(dReduce);
        dReduce = nullptr;
    }
    if (hReduce) {
        cudaFreeHost(hReduce);
        hReduce = nullptr;
    }
    deviceReady = false;
}
void Multigrid::allocateDevice() {
    freeDevice();
    dev.resize(levels);

    for (int l = 0; l < levels; ++l) {
        const Level& L = lv[l];
        DeviceLevel& d = dev[l];
        d.halo = (L.nx > 8) ? L.nx : 8;
        const size_t padded = static_cast<size_t>(L.n) + 2u * d.halo + 16u;
        const size_t plain  = static_cast<size_t>(L.n) + 16u;

        CUDA_CHECK(cudaMalloc(&d.pAlloc, padded * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.resAlloc, padded * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.pAlloc, 0, padded * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.resAlloc, 0, padded * sizeof(float)));
        d.p = d.pAlloc + d.halo;
        d.res = d.resAlloc + d.halo;
        CUDA_CHECK(cudaMalloc(&d.rhs, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.cW, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.cE, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.cS, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.cN, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.diag, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.invDiag, plain * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d.solid, static_cast<size_t>(L.n) * sizeof(uint8_t)));
        CUDA_CHECK(cudaMalloc(&d.pWeight, plain * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.rhs, 0, plain * sizeof(float)));
        CUDA_CHECK(cudaMemset(d.pWeight, 0, plain * sizeof(float)));
    }
    int blocks = (lv[0].n + kNormBlock - 1) / kNormBlock;
    if (blocks > kMaxNormBlocks)
        blocks = kMaxNormBlocks;
    if (blocks < 1)
        blocks = 1;
    reduceBlocks = blocks;
    CUDA_CHECK(cudaMalloc(&dReduce, static_cast<size_t>(reduceBlocks) * sizeof(float)));
    CUDA_CHECK(cudaMallocHost(&hReduce, static_cast<size_t>(reduceBlocks) * sizeof(float)));
}

void Multigrid::setGeometryCuda() {
    allocateDevice();
    for (int l = 0; l < levels; ++l) {
        const Level& L = lv[l];
        DeviceLevel& d = dev[l];
        const size_t plain = static_cast<size_t>(L.n) + 16u;

        CUDA_CHECK(cudaMemcpy(d.cW, L.cW.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.cE, L.cE.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.cS, L.cS.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.cN, L.cN.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.diag, L.diag.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.invDiag, L.invDiag.data(), plain * sizeof(float), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.solid, L.solid.data(),
                              static_cast<size_t>(L.n) * sizeof(uint8_t),
                              cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d.pWeight, L.pWeight.data(),
                              static_cast<size_t>(L.n) * sizeof(float),
                              cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemset(d.pAlloc, 0,
                              (static_cast<size_t>(L.n) + 2u * d.halo + 16u) * sizeof(float)));
    }

    deviceReady = true;
}

void Multigrid::smoothCuda(int level, float omega, int sweeps) {
    const Level& L = lv[level];
    const DeviceLevel& d = dev[level];

    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((L.nx + kBlockX - 1) / kBlockX,
                    (L.ny + kBlockY - 1) / kBlockY);

    for (int sweep = 0; sweep < sweeps; ++sweep) {
        for (int color = 0; color < 2; ++color) {
            // Consecutive launches on the same stream are ordered, so the black
            // half sweep is guaranteed to observe every red update. The old
            // code relied on the same property but also ran a separate applyBC
            // kernel that wrote the cells its neighbours were reading, which is
            // exactly where the GPU and CPU results used to diverge.
            mgSmoothKernel<<<grid, block>>>(
                L.nx, L.ny, d.p, d.rhs,
                d.cW, d.cE, d.cS, d.cN, d.invDiag,
                omega, color);
            CUDA_CHECK_LAUNCH("mgSmoothKernel");
        }
    }
}
void Multigrid::computeResidualCuda(int level) {
    const Level& L = lv[level];
    const DeviceLevel& d = dev[level];

    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((L.nx + kBlockX - 1) / kBlockX,
                    (L.ny + kBlockY - 1) / kBlockY);

    mgResidualKernel<<<grid, block>>>(
        L.nx, L.ny, d.p, d.rhs,
        d.cW, d.cE, d.cS, d.cN, d.diag, d.res);
    CUDA_CHECK_LAUNCH("mgResidualKernel");
}
float Multigrid::residualNormCuda(int level) {
    const Level& L = lv[level];
    const DeviceLevel& d = dev[level];

    mgNormKernel<<<reduceBlocks, kNormBlock>>>(L.n, d.res, dReduce);
    CUDA_CHECK_LAUNCH("mgNormKernel");

    CUDA_CHECK(cudaMemcpy(hReduce, dReduce,
                          static_cast<size_t>(reduceBlocks) * sizeof(float),
                          cudaMemcpyDeviceToHost));

    double total = 0.0;
    for (int b = 0; b < reduceBlocks; ++b)
        total += hReduce[b];
    return static_cast<float>(std::sqrt(total));
}
float Multigrid::rhsNormCuda(int level) {
    const Level& L = lv[level];
    const DeviceLevel& d = dev[level];

    mgNormKernel<<<reduceBlocks, kNormBlock>>>(L.n, d.rhs, dReduce);
    CUDA_CHECK_LAUNCH("mgNormKernel");

    CUDA_CHECK(cudaMemcpy(hReduce, dReduce,
                          static_cast<size_t>(reduceBlocks) * sizeof(float),
                          cudaMemcpyDeviceToHost));

    double total = 0.0;
    for (int b = 0; b < reduceBlocks; ++b)
        total += hReduce[b];
    return static_cast<float>(std::sqrt(total));
}
void Multigrid::restrictResidualCuda(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;
    const Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];
    const DeviceLevel& df = dev[fineLevel];
    const DeviceLevel& dc = dev[coarseLevel];

    // Clear the coarse solution: the V-cycle solves for a correction there.
    CUDA_CHECK(cudaMemset(dc.p, 0, static_cast<size_t>(C.n) * sizeof(float)));

    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((C.nx + kBlockX - 1) / kBlockX,
                    (C.ny + kBlockY - 1) / kBlockY);

    mgRestrictKernel<<<grid, block>>>(
        F.nx, F.ny, C.nx, C.ny, F.refX, F.refY,
        df.res, df.pWeight, dc.solid, dc.diag, dc.rhs);
    CUDA_CHECK_LAUNCH("mgRestrictKernel(res)");
}
void Multigrid::restrictRHSCuda(int fineLevel) {
    const int coarseLevel = fineLevel + 1;
    if (coarseLevel >= levels)
        return;

    const Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];
    const DeviceLevel& df = dev[fineLevel];
    const DeviceLevel& dc = dev[coarseLevel];

    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((C.nx + kBlockX - 1) / kBlockX,
                    (C.ny + kBlockY - 1) / kBlockY);

    mgRestrictKernel<<<grid, block>>>(
        F.nx, F.ny, C.nx, C.ny, F.refX, F.refY,
        df.rhs, df.pWeight, dc.solid, dc.diag, dc.rhs);
    CUDA_CHECK_LAUNCH("mgRestrictKernel(rhs)");
}
void Multigrid::prolongateAddCuda(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;

    const int fineLevel = coarseLevel - 1;
    const Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];
    const DeviceLevel& df = dev[fineLevel];
    const DeviceLevel& dc = dev[coarseLevel];
    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((F.nx + kBlockX - 1) / kBlockX,
                    (F.ny + kBlockY - 1) / kBlockY);
    mgProlongateKernel<<<grid, block>>>(
        F.nx, F.ny, C.nx, C.ny, F.refX, F.refY,
        df.p, dc.p, df.pWeight, dc.solid, 1);
    CUDA_CHECK_LAUNCH("mgProlongateKernel(add)");
}
void Multigrid::prolongateSetCuda(int coarseLevel) {
    if (coarseLevel <= 0 || coarseLevel >= levels)
        return;

    const int fineLevel = coarseLevel - 1;
    const Level& F = lv[fineLevel];
    const Level& C = lv[coarseLevel];
    const DeviceLevel& df = dev[fineLevel];
    const DeviceLevel& dc = dev[coarseLevel];

    const dim3 block(kBlockX, kBlockY);
    const dim3 grid((F.nx + kBlockX - 1) / kBlockX,
                    (F.ny + kBlockY - 1) / kBlockY);

    mgProlongateKernel<<<grid, block>>>(
        F.nx, F.ny, C.nx, C.ny, F.refX, F.refY,
        df.p, dc.p, df.pWeight, dc.solid, 0);
    CUDA_CHECK_LAUNCH("mgProlongateKernel(set)");
}
void Multigrid::vCycleCuda(int level, float smootherOmega, float coarseOmega) {
    if (level == levels - 1) {
        smoothCuda(level, coarseOmega, coarseSweeps(lv[level].nx, lv[level].ny));
        return;
    }

    smoothCuda(level, smootherOmega, kPreSmooth);
    computeResidualCuda(level);
    restrictResidualCuda(level);
    vCycleCuda(level + 1, smootherOmega, coarseOmega);
    prolongateAddCuda(level + 1);
    smoothCuda(level, smootherOmega, kPostSmooth);
}
void Multigrid::fullMultigridCuda(float smootherOmega, float coarseOmega) {
    const int coarsest = levels - 1;
    if (coarsest == 0) {
        smoothCuda(0, coarseOmega, coarseSweeps(lv[0].nx, lv[0].ny));
        return;
    }

    for (int level = 0; level < coarsest; ++level)
        restrictRHSCuda(level);

    CUDA_CHECK(cudaMemset(dev[coarsest].p, 0,
                          static_cast<size_t>(lv[coarsest].n) * sizeof(float)));
    smoothCuda(coarsest, coarseOmega, coarseSweeps(lv[coarsest].nx, lv[coarsest].ny));

    for (int level = coarsest; level > 0; --level) {
        prolongateSetCuda(level);
        vCycleCuda(level - 1, smootherOmega, coarseOmega);
    }
}

float Multigrid::solveCuda(std::vector<float>& pressure,
                           const std::vector<float>& rhs,
                           float smootherOmega,
                           float coarseOmega,
                           int   maxCycles,
                           float tolerance)
{
    if (!deviceReady) {
        std::fprintf(stderr, "Multigrid::solveCuda called before setGeometry\n");
        return 0.0f;
    }
    const Level& L0 = lv[0];
    const DeviceLevel& d0 = dev[0];
    const size_t bytes = static_cast<size_t>(L0.n) * sizeof(float);
    CUDA_CHECK(cudaMemcpy(d0.rhs, rhs.data(), bytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d0.p, pressure.data(), bytes, cudaMemcpyHostToDevice));

    {
        const int block = 256;
        const int grid = (L0.n + block - 1) / block;
        mgMaskKernel<<<grid, block>>>(L0.n, d0.p, d0.rhs, d0.invDiag);
        CUDA_CHECK_LAUNCH("mgMaskKernel");
    }

    const float rhsNorm = rhsNormCuda(0);
    const float scale = (rhsNorm > 1e-20f) ? rhsNorm : 1.0f;

    if (firstSolve) {
        fullMultigridCuda(smootherOmega, coarseOmega);
        firstSolve = false;
    }

    lastCycles = 0;
    float relative = 1.0f;

    for (int cycle = 0; cycle < maxCycles; ++cycle) {
        computeResidualCuda(0);
        relative = residualNormCuda(0) / scale;
        if (relative < tolerance)
            break;

        vCycleCuda(0, smootherOmega, coarseOmega);
        ++lastCycles;
    }

    if (lastCycles > 0) {
        computeResidualCuda(0);
        relative = residualNormCuda(0) / scale;
    }

    CUDA_CHECK(cudaMemcpy(pressure.data(), d0.p, bytes, cudaMemcpyDeviceToHost));
    return relative;
}

#endif // USE_CUDA
