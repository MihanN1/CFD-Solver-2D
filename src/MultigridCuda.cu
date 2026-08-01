#include "Multigrid.hpp"
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
    constexpr int coarseSmooth = 20;

    if(level == levels - 1){
        smoothSORCuda(
            level,
            omega,
            coarseSmooth);

        return;
    }

    smoothSORCuda(
        level,
        omega,
        preSmooth);

    computeResidualCuda(level);

    restrictResidualCuda(level);

    cudaMemset(
        dPressureLevels[level + 1],
        0,
        sizeBytes(level + 1));

    vCycleCuda(level + 1, omega);

    prolongateCorrectionCuda(level + 1);

    smoothSORCuda(
        level,
        omega,
        postSmooth);
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
        12);

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
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i <= 0 || i >= coarseNx - 1)
        return;

    if (j <= 0 || j >= coarseNy - 1)
        return;

    const int fi = min(i * 2, fineNx - 1);
    const int fj = min(j * 2, fineNy - 1);
    const int x = min(i * 2, fineNx - 2);
    const int y = min(j * 2, fineNy - 2);

    float sum = 0.0f;
    float weightSum = 0.0f;

    int xx, yy, id;

    // (-1,-1)
    xx = x - 1;
    yy = y - 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += fineResidual[id];
            weightSum += 1.0f;
        }
    }
    // (0,-1)
    xx = x;
    yy = y - 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += 2.0f * fineResidual[id];
            weightSum += 2.0f;
        }
    }
    // (+1,-1)
    xx = x + 1;
    yy = y - 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += fineResidual[id];
            weightSum += 1.0f;
        }
    }
    // (-1,0)
    xx = x - 1;
    yy = y;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += 2.0f * fineResidual[id];
            weightSum += 2.0f;
        }
    }
    // (0,0)
    xx = x;
    yy = y;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += 4.0f * fineResidual[id];
            weightSum += 4.0f;
        }
    }
    // (+1,0)
    xx = x + 1;
    yy = y;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += 2.0f * fineResidual[id];
            weightSum += 2.0f;
        }
    }
    // (-1,+1)
    xx = x - 1;
    yy = y + 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += fineResidual[id];
            weightSum += 1.0f;
        }
    }
    // (0,+1)
    xx = x;
    yy = y + 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += 2.0f * fineResidual[id];
            weightSum += 2.0f;
        }
    }
    // (+1,+1)
    xx = x + 1;
    yy = y + 1;
    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if (!fineSolid[id]){
            sum += fineResidual[id];
            weightSum += 1.0f;
        }
    }

    const int coarseId = j * coarseNx + i;
    coarseRhs[coarseId] =
        (weightSum > 0.0f) ? (sum / weightSum) : 0.0f;

    coarsePressure[coarseId] = 0.0f;
    int x0 = i * 2;
    int x1 = min(i * 2 + 1, fineNx - 1);
    int y0 = j * 2;
    int y1 = min(j * 2 + 1, fineNy - 1);

    coarseSolid[j * coarseNx + i] =
        fineSolid[y0 * fineNx + x0] &&
        fineSolid[y0 * fineNx + x1] &&
        fineSolid[y1 * fineNx + x0] &&
        fineSolid[y1 * fineNx + x1];
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
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;

    if(i <= 0 || i >= coarseNx - 1)
        return;

    if(j <= 0 || j >= coarseNy - 1)
        return;

    const int fi = min(i * 2, fineNx - 1);
    const int fj = min(j * 2, fineNy - 1);

    const int x = fi;
    const int y = fj;

    float sum = 0.0f;
    float wsum = 0.0f;

    int xx, yy, id;

    xx = x - 1;
    yy = y - 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += fineRhs[id];
            wsum += 1.0f;
        }
    }

    xx = x;
    yy = y - 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += 2.0f * fineRhs[id];
            wsum += 2.0f;
        }
    }

    xx = x + 1;
    yy = y - 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += fineRhs[id];
            wsum += 1.0f;
        }
    }

    xx = x - 1;
    yy = y;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += 2.0f * fineRhs[id];
            wsum += 2.0f;
        }
    }

    xx = x;
    yy = y;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += 4.0f * fineRhs[id];
            wsum += 4.0f;
        }
    }

    xx = x + 1;
    yy = y;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += 2.0f * fineRhs[id];
            wsum += 2.0f;
        }
    }

    xx = x - 1;
    yy = y + 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += fineRhs[id];
            wsum += 1.0f;
        }
    }

    xx = x;
    yy = y + 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += 2.0f * fineRhs[id];
            wsum += 2.0f;
        }
    }

    xx = x + 1;
    yy = y + 1;
    if(xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy){
        id = yy * fineNx + xx;
        if(!fineSolid[id]){
            sum += fineRhs[id];
            wsum += 1.0f;
        }
    }

    coarseRhs[j * coarseNx + i] =
        (wsum > 0.0f) ? sum / wsum : 0.0f;

    int x0 = i * 2;
    int x1 = min(i * 2 + 1, fineNx - 1);
    int y0 = j * 2;
    int y1 = min(j * 2 + 1, fineNy - 1);

    coarseSolid[j * coarseNx + i] =
        fineSolid[y0 * fineNx + x0] &&
        fineSolid[y0 * fineNx + x1] &&
        fineSolid[y1 * fineNx + x0] &&
        fineSolid[y1 * fineNx + x1];
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

    const float x = static_cast<float>(i) * 0.5f;
    const float y = static_cast<float>(j) * 0.5f;

    const int ic0 = static_cast<int>(x);
    const int jc0 = static_cast<int>(y);

    const int ic1 = min(ic0 + 1, coarseNx - 1);
    const int jc1 = min(jc0 + 1, coarseNy - 1);

    const float tx = x - ic0;
    const float ty = y - jc0;

    const int id00 = jc0 * coarseNx + ic0;
    const int id10 = jc0 * coarseNx + ic1;
    const int id01 = jc1 * coarseNx + ic0;
    const int id11 = jc1 * coarseNx + ic1;

    float value = 0.0f;
    float weight = 0.0f;

    const float w00 = (1.0f - tx) * (1.0f - ty);
    const float w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty;
    const float w11 = tx * ty;

    if(!coarseSolid[id00]){
        value += w00 * coarsePressure[id00];
        weight += w00;
    }

    if(!coarseSolid[id10]){
        value += w10 * coarsePressure[id10];
        weight += w10;
    }

    if(!coarseSolid[id01]){
        value += w01 * coarsePressure[id01];
        weight += w01;
    }

    if(!coarseSolid[id11]){
        value += w11 * coarsePressure[id11];
        weight += w11;
    }

    if(weight > 0.0f)
        finePressure[fineId] += value / weight;
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

    const float x = static_cast<float>(i) * 0.5f;
    const float y = static_cast<float>(j) * 0.5f;

    const int ic0 = static_cast<int>(x);
    const int jc0 = static_cast<int>(y);

    const int ic1 = min(ic0 + 1, coarseNx - 1);
    const int jc1 = min(jc0 + 1, coarseNy - 1);

    const float tx = x - ic0;
    const float ty = y - jc0;

    const int id00 = jc0 * coarseNx + ic0;
    const int id10 = jc0 * coarseNx + ic1;
    const int id01 = jc1 * coarseNx + ic0;
    const int id11 = jc1 * coarseNx + ic1;

    float value = 0.0f;
    float weight = 0.0f;

    const float w00 = (1.0f - tx) * (1.0f - ty);
    const float w10 = tx * (1.0f - ty);
    const float w01 = (1.0f - tx) * ty;
    const float w11 = tx * ty;

    if(!coarseSolid[id00]){
        value += w00 * coarsePressure[id00];
        weight += w00;
    }

    if(!coarseSolid[id10]){
        value += w10 * coarsePressure[id10];
        weight += w10;
    }

    if(!coarseSolid[id01]){
        value += w01 * coarsePressure[id01];
        weight += w01;
    }

    if(!coarseSolid[id11]){
        value += w11 * coarsePressure[id11];
        weight += w11;
    }

    if(weight > 0.0f)
        finePressure[fineId] = value / weight;
}

#endif