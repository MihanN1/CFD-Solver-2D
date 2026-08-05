#pragma once

#ifdef USE_CUDA

#include <cuda_runtime.h>
#include <cstdint>
class Multigrid;

__global__ void smoothSORKernel(
    int nx,
    int ny,
    float* pressure,
    const float* rhs,
    const float* coefW,
    const float* coefE,
    const float* coefS,
    const float* coefN,
    const float* invDiag,
    float omega,
    int color);

__global__ void computeResidualKernel(
    int nx,
    int ny,
    const float* pressure,
    const float* rhs,
    const float* coefW,
    const float* coefE,
    const float* coefS,
    const float* coefN,
    const float* diag,
    float* residual);

// Restricts either the residual or the rhs, depending on what is passed in
__global__ void restrictKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    int refineX,
    int refineY,
    const float* fineSrc,
    const float* fineProlongWeight,
    const uint8_t* coarseSolid,
    const float* coarseDiag,
    float* coarseRhs);

// addMode = 1 adds a correction (prolongateCorrection),
// addMode = 0 overwrites the fine solution (prolongateSolution)
__global__ void prolongateKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    int refineX,
    int refineY,
    float* finePressure,
    const float* coarsePressure,
    const float* fineProlongWeight,
    const uint8_t* coarseSolid,
    int addMode);

// Zeroes pressure and rhs in the cells that take no part in the solve
__global__ void zeroSolidPressureKernel(
    int cellCount,
    float* pressure,
    float* rhs,
    const float* invDiag);

// Partial sums of squares, one value per block
__global__ void computeNormKernel(
    int cellCount,
    const float* values,
    float* partial);

#endif
