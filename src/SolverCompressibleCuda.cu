#include "SolverCompressible.hpp"
#ifdef USE_CUDA
#include "CompressibleKernels.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kBlockX = 32;
constexpr int kBlockY = 8;
constexpr int kReduceBlock = 256;
constexpr int kMaxReduceBlocks = 1024;

void checkCuda(cudaError_t status, const char* what, const char* file,
               int line) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "CUDA error at %s:%d during %s: %s\n", file, line,
                     what, cudaGetErrorString(status));
        std::abort();
    }
}

}

#define CFD_CUDA(call) checkCuda((call), #call, __FILE__, __LINE__)
#define CFD_CUDA_LAUNCH(name) \
    checkCuda(cudaGetLastError(), name, __FILE__, __LINE__)

namespace {

__global__ void primitiveKernel(Block in,
                                GasModel gas,
                                float* rho,
                                float* u,
                                float* v,
                                float* p,
                                float* y,
                                float* gamma) {
    const int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= in.stride * in.rows)
        return;
    cfd::fillPrimitive(in, gas, id, rho, u, v, p, y, gamma);
}

__global__ void fluxXKernel(Block in,
                            cfd::PrimitiveField prim,
                            GasModel gas,
                            int limiter,
                            float* fx) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i > in.nx || j >= in.ny)
        return;
    cfd::faceFluxX(in, prim, gas, limiter, i, j,
                   fx + (static_cast<long long>(j) * (in.nx + 1) + i) *
                            cfd::kComponents);
}

__global__ void fluxYKernel(Block in,
                            cfd::PrimitiveField prim,
                            GasModel gas,
                            int limiter,
                            float* fy) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= in.nx || j > in.ny)
        return;
    cfd::faceFluxY(in, prim, gas, limiter, i, j,
                   fy + (static_cast<long long>(j) * in.nx + i) *
                            cfd::kComponents);
}

__global__ void combineKernel(Block in,
                              Block keep,
                              Block out,
                              GasModel gas,
                              const float* fx,
                              const float* fy,
                              float dt,
                              float a,
                              float b,
                              float diffusivity) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= in.nx || j >= in.ny)
        return;
    cfd::combine(in, keep, out, gas, fx, fy, i, j, dt, a, b, diffusivity);
}

__global__ void solidKernel(Block block, GasModel gas, int layer) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= block.nx || j >= block.ny)
        return;
    cfd::solidCell(block, gas, i, j, layer);
}

__global__ void ghostRowKernel(Block block,
                               GasModel gas,
                               BlockBoundaries sides) {
    const int j = blockIdx.x * blockDim.x + threadIdx.x;
    if (j >= block.ny)
        return;
    BlockBoundaries local = sides;
    local.inletY = (j + 0.5f) / static_cast<float>(block.ny);
    for (int k = 1; k <= block.ghost; ++k) {
        cfd::mirrorSide(block, gas, sides.left, -k, j, k - 1, j, true, local);
        cfd::mirrorSide(block, gas, sides.right, block.nx - 1 + k, j,
                        block.nx - k, j, true, local);
    }
}

__global__ void ghostColumnKernel(Block block,
                                  GasModel gas,
                                  BlockBoundaries sides) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= block.nx)
        return;
    BlockBoundaries local = sides;
    local.inletY = (i + 0.5f) / static_cast<float>(block.nx);
    for (int k = 1; k <= block.ghost; ++k) {
        cfd::mirrorSide(block, gas, sides.bottom, i, -k, i, k - 1, false,
                        local);
        cfd::mirrorSide(block, gas, sides.top, i, block.ny - 1 + k, i,
                        block.ny - k, false, local);
    }
}

__global__ void ghostCornerKernel(Block block, GasModel gas) {
    const int lane = blockIdx.x * blockDim.x + threadIdx.x;
    const int perCorner = block.ghost * block.ghost;
    if (lane >= 4 * perCorner)
        return;
    const int corner = lane / perCorner;
    const int k = lane % perCorner / block.ghost + 1;
    const int m = lane % block.ghost + 1;
    const int targetI = (corner & 1) ? block.nx - 1 + k : -k;
    const int targetJ = (corner & 2) ? block.ny - 1 + m : -m;
    const int sourceI = (corner & 1) ? block.nx - 1 : 0;
    const int sourceJ = (corner & 2) ? block.ny - 1 : 0;
    const cfd::Primitive q =
        cfd::primitiveOf(block, gas, block.index(sourceI, sourceJ));
    cfd::writeState(block, block.index(targetI, targetJ), q);
}

__global__ void rateKernel(Block block, GasModel gas, float* partials) {
    __shared__ float shared[kReduceBlock];
    const int total = block.nx * block.ny;
    const int lane = threadIdx.x;
    float best = 0.0f;
    for (int id = blockIdx.x * blockDim.x + lane; id < total;
         id += blockDim.x * gridDim.x) {
        const int i = id % block.nx;
        const int j = id / block.nx;
        best = fmaxf(best, cfd::cellRate(block, gas, i, j));
    }
    shared[lane] = best;
    __syncthreads();
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (lane < stride)
            shared[lane] = fmaxf(shared[lane], shared[lane + stride]);
        __syncthreads();
    }
    if (lane == 0)
        partials[blockIdx.x] = shared[0];
}

}

struct CompressibleDevice {
    int nx = 0, ny = 0, ghost = 0;
    bool species = false;
    std::size_t cells = 0;

    float* fields[3][5] = {};
    uint8_t* solid = nullptr;
    float* fluxX = nullptr;
    float* fluxY = nullptr;
    float* primitive[6] = {};
    float* partials = nullptr;
    std::vector<float> partialHost;
};

bool compressibleCudaAvailable() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess)
        return false;
    return count > 0;
}

CompressibleDevice* compressibleCudaCreate(int nx,
                                           int ny,
                                           int ghost,
                                           bool species) {
    CompressibleDevice* device = new CompressibleDevice();
    device->nx = nx;
    device->ny = ny;
    device->ghost = ghost;
    device->species = species;
    device->cells = static_cast<std::size_t>(nx + 2 * ghost) *
                    static_cast<std::size_t>(ny + 2 * ghost);

    const std::size_t bytes = device->cells * sizeof(float);
    for (int set = 0; set < 3; ++set)
        for (int c = 0; c < 5; ++c) {
            if (c == 4 && !species)
                continue;
            CFD_CUDA(cudaMalloc(&device->fields[set][c], bytes));
            CFD_CUDA(cudaMemset(device->fields[set][c], 0, bytes));
        }

    CFD_CUDA(cudaMalloc(&device->solid,
                        static_cast<std::size_t>(nx) * ny * sizeof(uint8_t)));
    CFD_CUDA(cudaMalloc(&device->fluxX,
                        static_cast<std::size_t>(nx + 1) * ny * 5 *
                            sizeof(float)));
    CFD_CUDA(cudaMalloc(&device->fluxY,
                        static_cast<std::size_t>(nx) * (ny + 1) * 5 *
                            sizeof(float)));
    for (int c = 0; c < 6; ++c)
        CFD_CUDA(cudaMalloc(&device->primitive[c], bytes));
    CFD_CUDA(cudaMalloc(&device->partials,
                        kMaxReduceBlocks * sizeof(float)));
    device->partialHost.assign(kMaxReduceBlocks, 0.0f);
    return device;
}

void compressibleCudaDestroy(CompressibleDevice* device) {
    if (!device)
        return;
    for (int set = 0; set < 3; ++set)
        for (int c = 0; c < 5; ++c)
            if (device->fields[set][c])
                cudaFree(device->fields[set][c]);
    if (device->solid)
        cudaFree(device->solid);
    if (device->fluxX)
        cudaFree(device->fluxX);
    if (device->fluxY)
        cudaFree(device->fluxY);
    for (int c = 0; c < 6; ++c)
        if (device->primitive[c])
            cudaFree(device->primitive[c]);
    if (device->partials)
        cudaFree(device->partials);
    delete device;
}

void compressibleCudaUploadSolid(CompressibleDevice* device,
                                 const uint8_t* mask) {
    CFD_CUDA(cudaMemcpy(device->solid, mask,
                        static_cast<std::size_t>(device->nx) * device->ny,
                        cudaMemcpyHostToDevice));
}

void compressibleCudaUpload(CompressibleDevice* device,
                            int set,
                            const float* const* host) {
    const std::size_t bytes = device->cells * sizeof(float);
    for (int c = 0; c < 5; ++c) {
        if (c == 4 && !device->species)
            continue;
        CFD_CUDA(cudaMemcpy(device->fields[set][c], host[c], bytes,
                            cudaMemcpyHostToDevice));
    }
}

void compressibleCudaDownload(CompressibleDevice* device,
                              int set,
                              float* const* host) {
    const std::size_t bytes = device->cells * sizeof(float);
    for (int c = 0; c < 5; ++c) {
        if (c == 4 && !device->species)
            continue;
        CFD_CUDA(cudaMemcpy(host[c], device->fields[set][c], bytes,
                            cudaMemcpyDeviceToHost));
    }
}

namespace {

Block deviceBlock(CompressibleDevice* device, int set, const Block& shape) {
    Block block = shape;
    block.rho = device->fields[set][0];
    block.rhou = device->fields[set][1];
    block.rhov = device->fields[set][2];
    block.rhoE = device->fields[set][3];
    block.rhoY = device->species ? device->fields[set][4] : nullptr;
    block.solid = device->solid;
    return block;
}

}

float compressibleCudaTimeStep(CompressibleDevice* device,
                               const Block& shape,
                               const GasModel& gas,
                               float cfl) {
    const Block block = deviceBlock(device, 0, shape);
    const int total = block.nx * block.ny;
    int blocks = (total + kReduceBlock - 1) / kReduceBlock;
    if (blocks > kMaxReduceBlocks)
        blocks = kMaxReduceBlocks;
    rateKernel<<<blocks, kReduceBlock>>>(block, gas, device->partials);
    CFD_CUDA_LAUNCH("rateKernel");
    CFD_CUDA(cudaMemcpy(device->partialHost.data(), device->partials,
                        blocks * sizeof(float), cudaMemcpyDeviceToHost));

    float worst = 0.0f;
    for (int k = 0; k < blocks; ++k)
        worst = std::max(worst, device->partialHost[k]);
    if (!(worst > 0.0f))
        return 0.0f;
    return cfl / worst;
}

void compressibleCudaStage(CompressibleDevice* device,
                           const Block& shape,
                           int inSet,
                           int keepSet,
                           int outSet,
                           const GasModel& gas,
                           const BlockBoundaries& sides,
                           float dt,
                           float a,
                           float b,
                           int limiter,
                           float diffusivity) {
    Block in = deviceBlock(device, inSet, shape);
    Block keep = deviceBlock(device, keepSet, shape);
    Block out = deviceBlock(device, outSet, shape);

    const dim3 threads(kBlockX, kBlockY);
    const dim3 fillGrid((in.nx + kBlockX - 1) / kBlockX,
                        (in.ny + kBlockY - 1) / kBlockY);
    if (in.solid)
        for (int layer = 0; layer < 2; ++layer) {
            solidKernel<<<fillGrid, threads>>>(in, gas, layer);
            CFD_CUDA_LAUNCH("solidKernel");
        }

    ghostRowKernel<<<(in.ny + 127) / 128, 128>>>(in, gas, sides);
    CFD_CUDA_LAUNCH("ghostRowKernel");
    ghostColumnKernel<<<(in.nx + 127) / 128, 128>>>(in, gas, sides);
    CFD_CUDA_LAUNCH("ghostColumnKernel");
    const int corners = 4 * in.ghost * in.ghost;
    ghostCornerKernel<<<(corners + 63) / 64, 64>>>(in, gas);
    CFD_CUDA_LAUNCH("ghostCornerKernel");

    const dim3 faceX((in.nx + 1 + kBlockX) / kBlockX,
                     (in.ny + kBlockY - 1) / kBlockY);
    const dim3 faceY((in.nx + kBlockX - 1) / kBlockX,
                     (in.ny + 1 + kBlockY) / kBlockY);
    const dim3 cellGrid((in.nx + kBlockX - 1) / kBlockX,
                        (in.ny + kBlockY - 1) / kBlockY);

    const int total = in.stride * in.rows;
    primitiveKernel<<<(total + 255) / 256, 256>>>(
        in, gas, device->primitive[0], device->primitive[1],
        device->primitive[2], device->primitive[3], device->primitive[4],
        device->primitive[5]);
    CFD_CUDA_LAUNCH("primitiveKernel");

    cfd::PrimitiveField prim;
    prim.rho = device->primitive[0];
    prim.u = device->primitive[1];
    prim.v = device->primitive[2];
    prim.p = device->primitive[3];
    prim.y = device->primitive[4];
    prim.gamma = device->primitive[5];

    fluxXKernel<<<faceX, threads>>>(in, prim, gas, limiter, device->fluxX);
    CFD_CUDA_LAUNCH("fluxXKernel");
    fluxYKernel<<<faceY, threads>>>(in, prim, gas, limiter, device->fluxY);
    CFD_CUDA_LAUNCH("fluxYKernel");
    combineKernel<<<cellGrid, threads>>>(in, keep, out, gas, device->fluxX,
                                         device->fluxY, dt, a, b, diffusivity);
    CFD_CUDA_LAUNCH("combineKernel");
}

#endif
