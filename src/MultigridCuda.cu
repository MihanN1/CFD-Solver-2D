#include "Multigrid.hpp"
#include <iomanip>
#include <iostream>
#ifdef USE_CUDA
#include "MultigridCuda.cuh"
#include <algorithm>
#include <cmath>
#include <thrust/device_ptr.h>
#include <thrust/transform_reduce.h>
#include <thrust/functional.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/zip_iterator.h>
#include <thrust/tuple.h>

void Multigrid::solveCuda(
    std::vector<float>& pressure,
    const std::vector<float>& rhs,
    const std::vector<uint8_t>& solid,
    float omega,
    int iterations)
{
    if(levels == 0)
        buildHierarchy();

    dPressureLevels.resize(levels);
    dRhsLevels.resize(levels);
    dResidualLevels.resize(levels);
    dSolidLevels.resize(levels);

    for(int level = 0; level < levels; ++level){
        const size_t bytes =
            static_cast<size_t>(levelNx[level]) *
            levelNy[level];

        cudaMalloc(&dPressureLevels[level], bytes * sizeof(float));
        cudaMalloc(&dRhsLevels[level], bytes * sizeof(float));
        cudaMalloc(&dResidualLevels[level], bytes * sizeof(float));
        cudaMalloc(&dSolidLevels[level], bytes * sizeof(uint8_t));
    }

    {
        const size_t bytes =
            static_cast<size_t>(levelNx[0]) *
            levelNy[0];

        cudaMemcpy(
            dPressureLevels[0],
            pressure.data(),
            bytes * sizeof(float),
            cudaMemcpyHostToDevice);

        cudaMemcpy(
            dRhsLevels[0],
            rhs.data(),
            bytes * sizeof(float),
            cudaMemcpyHostToDevice);

        cudaMemcpy(
            dSolidLevels[0],
            solid.data(),
            bytes * sizeof(uint8_t),
            cudaMemcpyHostToDevice);

        cudaMemset(
            dResidualLevels[0],
            0,
            bytes * sizeof(float));
    }
    for(int level = 1; level < levels; ++level){
        std::fill(
            pressureLevels[level].begin(),
            pressureLevels[level].end(),
            0.0f);

        std::fill(
            rhsLevels[level].begin(),
            rhsLevels[level].end(),
            0.0f);

        std::fill(
            residualLevels[level].begin(),
            residualLevels[level].end(),
            0.0f);

        std::fill(
            solidLevels[level].begin(),
            solidLevels[level].end(),
            static_cast<uint8_t>(0));
    }
    for(int level = 1; level < levels; ++level){
        const size_t bytes =
            static_cast<size_t>(levelNx[level]) *
            levelNy[level];

        cudaMemset(
            dPressureLevels[level],
            0,
            bytes * sizeof(float));

        cudaMemset(
            dRhsLevels[level],
            0,
            bytes * sizeof(float));

        cudaMemset(
            dResidualLevels[level],
            0,
            bytes * sizeof(float));

        cudaMemset(
            dSolidLevels[level],
            0,
            bytes * sizeof(uint8_t));
    }

    constexpr float tolerance = 1e-6f;

    fullMultigridCuda(omega);

    for(int cycle = 0; cycle < iterations; ++cycle){
        computeResidualCuda(0);

        if(computeResidualNormCuda() < tolerance)
            break;

        vCycleCuda(0, omega);
    }

    {
        const size_t bytes =
            static_cast<size_t>(levelNx[0]) *
            levelNy[0];

        cudaMemcpy(
            pressure.data(),
            dPressureLevels[0],
            bytes * sizeof(float),
            cudaMemcpyDeviceToHost);
    }

    for(int level = 0; level < levels; ++level){
        cudaFree(dPressureLevels[level]);
        cudaFree(dRhsLevels[level]);
        cudaFree(dResidualLevels[level]);
        cudaFree(dSolidLevels[level]);

        dPressureLevels[level] = nullptr;
        dRhsLevels[level] = nullptr;
        dResidualLevels[level] = nullptr;
        dSolidLevels[level] = nullptr;
    }
    dPressureLevels.clear();
    dRhsLevels.clear();
    dResidualLevels.clear();
    dSolidLevels.clear();
    pressureLevels[0] = pressure;
    rhsLevels[0] = rhs;
    solidLevels[0] = solid;
}
void Multigrid::smoothSORCuda(
    int level,
    float omega,
    int iterations)
{
    const int nx = levelNx[level];
    const int ny = levelNy[level];

    const float invDx2 = levelInvDx2[level];
    const float invDy2 = levelInvDy2[level];

    dim3 block(16,16);

    dim3 grid(
        (nx + block.x - 1) / block.x,
        (ny + block.y - 1) / block.y);

    for(int iter = 0; iter < iterations; ++iter)
    {
        smoothSORKernel<<<grid, block>>>(
            nx,
            ny,
            dPressureLevels[level],
            dRhsLevels[level],
            dSolidLevels[level],
            invDx2,
            invDy2,
            omega,
            0);

        smoothSORKernel<<<grid, block>>>(
            nx,
            ny,
            dPressureLevels[level],
            dRhsLevels[level],
            dSolidLevels[level],
            invDx2,
            invDy2,
            omega,
            1);
    }
}

void Multigrid::computeResidualCuda(int level){
    const int nx = levelNx[level];
    const int ny = levelNy[level];
    const float invDx2 = levelInvDx2[level];
    const float invDy2 = levelInvDy2[level];
    dim3 block(16, 16);
    dim3 grid(
        (nx + block.x - 1) / block.x,
        (ny + block.y - 1) / block.y);
    computeResidualKernel<<<grid, block>>>(
        nx,
        ny,
        dPressureLevels[level],
        dRhsLevels[level],
        dResidualLevels[level],
        dSolidLevels[level],
        invDx2,
        invDy2);
}

void Multigrid::restrictResidualCuda(int fineLevel){
    if (fineLevel + 1 >= levels)
        return;
    const int coarseLevel = fineLevel + 1;
    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];
    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    cudaMemset(
        dPressureLevels[coarseLevel],
        0,
        static_cast<size_t>(coarseNx) *
        coarseNy *
        sizeof(float));
    dim3 block(16,16);
    dim3 grid(
        (coarseNx + block.x - 1) / block.x,
        (coarseNy + block.y - 1) / block.y);
    restrictResidualKernel<<<grid, block>>>(
        fineNx,
        fineNy,
        coarseNx,
        coarseNy,
        dResidualLevels[fineLevel],
        dSolidLevels[fineLevel],
        dRhsLevels[coarseLevel],
        dPressureLevels[coarseLevel],
        dSolidLevels[coarseLevel]);
}

void Multigrid::restrictRHSCuda(int fineLevel){
    if(fineLevel + 1 >= levels)
        return;

    const int coarseLevel = fineLevel + 1;

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    dim3 block(16,16);

    dim3 grid(
        (coarseNx + block.x - 1) / block.x,
        (coarseNy + block.y - 1) / block.y);

    cudaMemset(
        dPressureLevels[coarseLevel],
        0,
        static_cast<size_t>(coarseNx) *
        coarseNy *
        sizeof(float));

    restrictRHSKernel<<<grid, block>>>(
        fineNx,
        fineNy,
        coarseNx,
        coarseNy,
        dRhsLevels[fineLevel],
        dSolidLevels[fineLevel],
        dRhsLevels[coarseLevel],
        dSolidLevels[coarseLevel]);
}

void Multigrid::prolongateCorrectionCuda(int coarseLevel){
    if(coarseLevel <= 0)
        return;

    const int fineLevel = coarseLevel - 1;

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    dim3 block(16, 16);
    dim3 grid(
        (fineNx + block.x - 1) / block.x,
        (fineNy + block.y - 1) / block.y);

    prolongateCorrectionKernel<<<grid, block>>>(
        fineNx,
        fineNy,
        coarseNx,
        coarseNy,
        dPressureLevels[fineLevel],
        dPressureLevels[coarseLevel],
        dSolidLevels[fineLevel],
        dSolidLevels[coarseLevel]);
}

void Multigrid::prolongateSolutionCuda(int coarseLevel)
{
    if(coarseLevel <= 0)
        return;

    const int fineLevel = coarseLevel - 1;

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    cudaMemset(
        dPressureLevels[fineLevel],
        0,
        static_cast<size_t>(fineNx) * fineNy * sizeof(float));

    dim3 block(16, 16);

    dim3 grid(
        (fineNx + block.x - 1) / block.x,
        (fineNy + block.y - 1) / block.y);

    prolongateSolutionKernel<<<grid, block>>>(
        fineNx,
        fineNy,
        coarseNx,
        coarseNy,
        dPressureLevels[fineLevel],
        dPressureLevels[coarseLevel],
        dSolidLevels[fineLevel],
        dSolidLevels[coarseLevel]);
}

void Multigrid::vCycleCuda(
    int level,
    float omega)
{
    constexpr int preSmooth = 2;
    constexpr int postSmooth = 2;
    constexpr int coarseSmooth = 50;

    if(level == levels - 1){
        smoothSORCuda(
            level,
            omega,
            coarseSmooth);
        zeroSolidPressureCuda(level);
        applyBCCuda(level);
        return;
    }

    smoothSORCuda(
        level,
        omega,
        preSmooth);
    zeroSolidPressureCuda(level);
    applyBCCuda(level);
    computeResidualCuda(level);

    restrictResidualCuda(level);

    cudaMemset(
        dPressureLevels[level + 1],
        0,
        sizeBytes(level + 1));

    vCycleCuda(level + 1, omega);

    prolongateCorrectionCuda(level + 1);
    zeroSolidPressureCuda(level);

    smoothSORCuda(
        level,
        omega,
        postSmooth);
    zeroSolidPressureCuda(level);
    applyBCCuda(level);
}

void Multigrid::fullMultigridCuda(float omega){
    const int coarsest = levels - 1;

    for(int level=0; level<coarsest; ++level)
        restrictRHSCuda(level);
    cudaMemset(
        dPressureLevels[coarsest],
        0,
        sizeBytes(coarsest));

    smoothSORCuda(
        coarsest,
        omega,
        50);
    zeroSolidPressureCuda(coarsest);
    applyBCCuda(coarsest);

    for(int level=coarsest; level>0; --level){
        prolongateSolutionCuda(level);

        vCycleCuda(level-1, omega);
    }
}


struct SquareMasked{
    const uint8_t* solid;

    SquareMasked(const uint8_t* s)
        : solid(s)
    {
    }

    __host__ __device__
    float operator()(const thrust::tuple<float, int>& t) const{
        const float value = thrust::get<0>(t);
        const int index = thrust::get<1>(t);

        return solid[index] ? 0.0f : value * value;
    }
};

float Multigrid::computeResidualNormCuda(){
    const size_t n =
        static_cast<size_t>(levelNx[0]) *
        levelNy[0];

    auto residualBegin =
        thrust::device_pointer_cast(dResidualLevels[0]);

    auto indexBegin =
        thrust::make_counting_iterator<int>(0);

    const float sum =
        thrust::transform_reduce(
            thrust::make_zip_iterator(
                thrust::make_tuple(residualBegin, indexBegin)),
            thrust::make_zip_iterator(
                thrust::make_tuple(residualBegin + n,
                                   indexBegin + n)),
            SquareMasked(dSolidLevels[0]),
            0.0f,
            thrust::plus<float>());

    return std::sqrt(sum);
}

inline size_t Multigrid::sizeBytes(int level) const{
    return static_cast<size_t>(levelNx[level]) *
           levelNy[level] *
           sizeof(float);
}

void Multigrid::applyBCCuda(int level) {
    int nx = levelNx[level];
    int ny = levelNy[level];
    int maxDim = std::max(nx, ny);
    int blockSize = 256;
    int gridSize = (maxDim + blockSize - 1) / blockSize;
    applyBCKernel<<<gridSize, blockSize>>>(
        nx, ny, 
        dPressureLevels[level], 
        dSolidLevels[level]
    );
}

void Multigrid::zeroSolidPressureCuda(int level) {
    int nx = levelNx[level], ny = levelNy[level];
    int total = nx * ny;
    int blockSize = 256;
    int gridSize = (total + blockSize - 1) / blockSize;
    zeroSolidPressureKernel<<<gridSize, blockSize>>>(nx, ny, dPressureLevels[level], dSolidLevels[level]);
}

__global__ void smoothSORKernel(
    int nx,
    int ny,
    float* pressure,
    const float* rhs,
    const uint8_t* solid,
    float invDx2,
    float invDy2,
    float omega,
    int color)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;

    if(i <= 0 || i >= nx - 1)
        return;

    if(j <= 0 || j >= ny - 1)
        return;

    if(((i + j) & 1) != color)
        return;

    const int row = j * nx;
    const int id = row + i;

    if(solid[id])
        return;

    const float leftMask  = solid[id - 1] ? 0.0f : 1.0f;
    const float rightMask = solid[id + 1] ? 0.0f : 1.0f;
    const float downMask  = solid[(j - 1) * nx + i] ? 0.0f : 1.0f;
    const float upMask    = solid[(j + 1) * nx + i] ? 0.0f : 1.0f;

    // FIX: same masking bug as the CPU smoothSOR (see Multigrid.cpp) -- a solid
    // neighbour must be excluded from both the numerator and the denominator,
    // not mirrored into the numerator while excluded from the denominator.
    // SON -_-
    const float pLeft  = leftMask  * pressure[id - 1];
    const float pRight = rightMask * pressure[id + 1];
    const float pDown  = downMask  * pressure[(j - 1) * nx + i];
    const float pUp    = upMask    * pressure[(j + 1) * nx + i];

    const float diagonal =
        (leftMask + rightMask) * invDx2 +
        (downMask + upMask) * invDy2;

    if(diagonal < 1e-12f)
        return;

    const float pNew =
        ((pLeft + pRight) * invDx2 +
         (pDown + pUp) * invDy2 -
         rhs[id]) / diagonal;

    pressure[id] += omega * (pNew - pressure[id]);
}

__global__ void computeResidualKernel(
    int nx,
    int ny,
    const float* pressure,
    const float* rhs,
    float* residual,
    const uint8_t* solid,
    float invDx2,
    float invDy2)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i <= 0 || i >= nx - 1)
        return;
    if (j <= 0 || j >= ny - 1)
        return;
    const int row = j * nx;
    const int id = row + i;
    if (solid[id]){
        residual[id] = 0.0f;
        return;
    }
    float Ap = 0.0f;
    if (!solid[id - 1])
        Ap += (pressure[id - 1] - pressure[id]) * invDx2;
    if (!solid[id + 1])
        Ap += (pressure[id + 1] - pressure[id]) * invDx2;
    if (!solid[(j - 1) * nx + i])
        Ap += (pressure[(j - 1) * nx + i] - pressure[id]) * invDy2;
    if (!solid[(j + 1) * nx + i])
        Ap += (pressure[(j + 1) * nx + i] - pressure[id]) * invDy2;
    residual[id] = rhs[id] - Ap;
}

__global__ void restrictResidualKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    const float* fineResidual,
    const uint8_t* fineSolid,
    float* coarseRhs,
    float* coarsePressure,
    uint8_t* coarseSolid)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i >= coarseNx || j >= coarseNy)
        return;

    int i0 = i * 2;
    int i1 = min(i0 + 1, fineNx - 1);
    int j0 = j * 2;
    int j1 = min(j0 + 1, fineNy - 1);

    float sum = 0.0f;
    float weightSum = 0.0f;
    bool solidFlag = false;

    for (int jj = j0; jj <= j1; ++jj) {
        for (int ii = i0; ii <= i1; ++ii) {
            int fineId = jj * fineNx + ii;
            if (!fineSolid[fineId]) {
                sum += fineResidual[fineId];
                weightSum += 1.0f;
            } else {
                solidFlag = true;
            }
        }
    }

    int coarseId = j * coarseNx + i;
    coarseRhs[coarseId] = (weightSum > 0.0f) ? (sum / weightSum) : 0.0f;
    coarseSolid[coarseId] = (weightSum == 0.0f) ? 1 : 0;
    if (coarseSolid[coarseId]) {
        coarseRhs[coarseId] = 0.0f;
    }
    coarsePressure[coarseId] = 0.0f;
}

__global__ void restrictRHSKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    const float* fineRhs,
    const uint8_t* fineSolid,
    float* coarseRhs,
    uint8_t* coarseSolid)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= coarseNx || j >= coarseNy) return;

    int i0 = i * 2;
    int i1 = min(i0 + 1, fineNx - 1);
    int j0 = j * 2;
    int j1 = min(j0 + 1, fineNy - 1);

    float sum = 0.0f;
    float wsum = 0.0f;
    bool solidFlag = false;

    for (int jj = j0; jj <= j1; ++jj) {
        for (int ii = i0; ii <= i1; ++ii) {
            int id = jj * fineNx + ii;
            if (!fineSolid[id]) {
                sum += fineRhs[id];
                wsum += 1.0f;
            } else {
                solidFlag = true;
            }
        }
    }

    int coarseId = j * coarseNx + i;
    coarseRhs[coarseId] = (wsum > 0.0f) ? (sum / wsum) : 0.0f;
    coarseSolid[coarseId] = (wsum == 0.0f) ? 1 : 0;
    if (coarseSolid[coarseId]) coarseRhs[coarseId] = 0.0f;
}

__global__ void prolongateCorrectionKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    float* finePressure,
    const float* coarsePressure,
    const uint8_t* fineSolid,
    const uint8_t* coarseSolid)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;

    if(i >= fineNx || j >= fineNy)
        return;

    const int fineId = j * fineNx + i;

    if(fineSolid[fineId])
        return;

    const int ic0 = i >> 1;
    const int jc0 = j >> 1;
    const int icN = ((i & 1) == 0) ? max(ic0 - 1, 0) : min(ic0 + 1, coarseNx - 1);
    const int jcN = ((j & 1) == 0) ? max(jc0 - 1, 0) : min(jc0 + 1, coarseNy - 1);

    float value = 0.0f;
    float weight = 0.0f;
    auto add = [&](int cx,int cy, float w){
        int id = cy * coarseNx + cx;

        if(!coarseSolid[id])
        {
            value += w * coarsePressure[id];
            weight += w;
        }
    };

    add(ic0, jc0, 0.75f * 0.75f);
    add(icN, jc0, 0.25f * 0.75f);
    add(ic0, jcN, 0.75f * 0.25f);
    add(icN, jcN, 0.25f * 0.25f);

    if(weight>0.0f)
        finePressure[fineId]+=value/weight;
}

__global__ void prolongateSolutionKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    float* finePressure,
    const float* coarsePressure,
    const uint8_t* fineSolid,
    const uint8_t* coarseSolid)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;

    if(i >= fineNx || j >= fineNy)
        return;

    const int fineId = j * fineNx + i;

    if(fineSolid[fineId])
        return;

    const int ic0 = i >> 1;
    const int jc0 = j >> 1;

    const int icN = ((i & 1) == 0) ? max(ic0 - 1, 0) : min(ic0 + 1, coarseNx - 1);
    const int jcN = ((j & 1) == 0) ? max(jc0 - 1, 0) : min(jc0 + 1, coarseNy - 1);

    float value = 0.0f;
    float weight = 0.0f;
    auto add = [&](int cx,int cy, float w){
        int id = cy * coarseNx + cx;

        if(!coarseSolid[id])
        {
            value += w * coarsePressure[id];
            weight += w;
        }
    };

    add(ic0, jc0, 0.75f * 0.75f);
    add(icN, jc0, 0.25f * 0.75f);
    add(ic0, jcN, 0.75f * 0.25f);
    add(icN, jcN, 0.25f * 0.25f);

    if(weight>0.0f)
        finePressure[fineId]=value/weight;
}

__global__ void applyBCKernel(int nx, int ny, float* p, const uint8_t* solid) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    // Left and right boundaries (i=0 and i=nx-1)
    if (idx < ny) {
        int idLeft = idx * nx;
        int idRight = idx * nx + (nx - 1);
        if (!solid[idLeft]) p[idLeft] = p[idLeft + 1];
        if (!solid[idRight]) p[idRight] = 0.0f;
    }
    // Bottom and top boundaries (j=0 and j=ny-1)
    if (idx < nx) {
        int idBottom = idx;
        int idTop = (ny - 1) * nx + idx;
        if (!solid[idBottom]) p[idBottom] = p[idBottom + nx];
        if (!solid[idTop]) p[idTop] = p[idTop - nx];
    }
}

__global__ void zeroSolidPressureKernel(int nx, int ny, float* p, const uint8_t* solid) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= nx * ny) return;
    if (solid[idx]) p[idx] = 0.0f;
}

#endif