#pragma once
#ifdef USE_CUDA
#include <cstdint>
#include <cuda_runtime.h>

__global__ void mgSmoothKernel(int nx, int ny,
                               float* __restrict__ p,
                               const float* __restrict__ rhs,
                               const float* __restrict__ cW,
                               const float* __restrict__ cE,
                               const float* __restrict__ cS,
                               const float* __restrict__ cN,
                               const float* __restrict__ invDiag,
                               float omega,
                               int color);

__global__ void mgResidualKernel(int nx, int ny,
                                 const float* __restrict__ p,
                                 const float* __restrict__ rhs,
                                 const float* __restrict__ cW,
                                 const float* __restrict__ cE,
                                 const float* __restrict__ cS,
                                 const float* __restrict__ cN,
                                 const float* __restrict__ diag,
                                 float* __restrict__ res);

__global__ void mgRestrictKernel(int fineNx, int fineNy,
                                 int coarseNx, int coarseNy,
                                 int refX, int refY,
                                 const float* __restrict__ fineSrc,
                                 const float* __restrict__ finePWeight,
                                 const uint8_t* __restrict__ coarseSolid,
                                 const float* __restrict__ coarseDiag,
                                 float* __restrict__ coarseRhs);

__global__ void mgProlongateKernel(int fineNx, int fineNy,
                                   int coarseNx, int coarseNy,
                                   int refX, int refY,
                                   float* __restrict__ fineP,
                                   const float* __restrict__ coarseP,
                                   const float* __restrict__ finePWeight,
                                   const uint8_t* __restrict__ coarseSolid,
                                   int addMode);

__global__ void mgMaskKernel(int n,
                             float* __restrict__ p,
                             float* __restrict__ rhs,
                             const float* __restrict__ invDiag);

// Partial sums of squares, one value per block.
__global__ void mgNormKernel(int n,
                             const float* __restrict__ v,
                             float* __restrict__ partial);

#endif