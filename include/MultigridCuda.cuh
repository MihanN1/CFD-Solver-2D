#pragma once

#ifdef USE_CUDA

#include <cuda_runtime.h>
class Multigrid;

__global__ void smoothSORKernel(
    int nx,
    int ny,
    float* pressure,
    const float* rhs,
    const uint8_t* solid,
    float invDx2,
    float invDy2,
    float omega,
    int color);

__global__ void computeResidualKernel(
    int nx,
    int ny,
    const float* pressure,
    const float* rhs,
    float* residual,
    const uint8_t* solid,
    float invDx2,
    float invDy2);

__global__ void restrictResidualKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    const float* fineResidual,
    const uint8_t* fineSolid,
    float* coarseRhs,
    float* coarsePressure,
    uint8_t* coarseSolid);

__global__ void restrictRHSKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    const float* fineRhs,
    const uint8_t* fineSolid,
    float* coarseRhs,
    uint8_t* coarseSolid);

__global__ void prolongateCorrectionKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    float* finePressure,
    const float* coarsePressure,
    const uint8_t* fineSolid,
    const uint8_t* coarseSolid);

__global__ void prolongateSolutionKernel(
    int fineNx,
    int fineNy,
    int coarseNx,
    int coarseNy,
    float* finePressure,
    const float* coarsePressure,
    const uint8_t* fineSolid,
    const uint8_t* coarseSolid);

__global__ void applyBCKernel(
    int nx,
    int ny,
    float* p,
    const uint8_t* solid);
__global__ void zeroSolidPressureKernel(
    int nx, 
    int ny, 
    float* p, 
    const uint8_t* solid);
#endif