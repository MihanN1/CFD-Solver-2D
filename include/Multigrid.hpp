#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>


class Multigrid {
public:
    Multigrid(int nx, int ny, float dx, float dy, int minCoarseSize = 8);
    ~Multigrid();

    Multigrid(const Multigrid&) = delete;
    Multigrid& operator=(const Multigrid&) = delete;

    void setGeometry(const std::vector<uint8_t>& solid);

    float solve(std::vector<float>& pressure,
                const std::vector<float>& rhs,
                float smootherOmega,
                float coarseOmega,
                int   maxCycles,
                float tolerance);

    int levelCount() const { return levels; }
    int cyclesUsed() const { return lastCycles; }

    void setUseCuda(bool enable);
    bool usingCuda() const { return useCuda; }

private:
    struct Field {
        std::vector<float> storage;
        float* base = nullptr;
        int    halo = 0;

        void init(int n, int h) {
            halo = h;
            storage.assign(static_cast<size_t>(n) + 2u * h + 16u, 0.0f);
            base = storage.data() + h;
        }
        void zero() {
            std::fill(storage.begin(), storage.end(), 0.0f);
        }
        float*       data()       { return base; }
        const float* data() const { return base; }
    };

    struct Level {
        int nx = 0;
        int ny = 0;
        int n  = 0;
        float dx = 0.0f;
        float dy = 0.0f;

        int refX = 1;
        int refY = 1;

        Field p;
        Field res;
        std::vector<float> rhs;

        std::vector<float> cW, cE, cS, cN;
        std::vector<float> diag;
        std::vector<float> invDiag;

        std::vector<uint8_t> solid;

        std::vector<float> pWeight;

    };

    int   nx0;
    int   ny0;
    float dx0;
    float dy0;
    int   minCoarseSize;
    int   levels = 0;
    int   lastCycles = 0;
    bool  geometryReady = false;
    bool  firstSolve = true;
    bool  useCuda = false;

    std::vector<Level> lv;

    void buildHierarchy();
    void buildCoefficients(Level& L);

    void smooth(int level, float omega, int sweeps);
    void computeResidual(int level);
    float residualNorm(int level) const;
    static float vectorNorm(const float* v, int n);

    void buildTransferWeights(int fineLevel);
    void restrict(int fineLevel, const float* fineSrc);
    void restrictResidual(int fineLevel);   // res(fine) -> rhs(coarse)
    void restrictRHS(int fineLevel);        // rhs(fine)  -> rhs(coarse), for FMG
    void prolongateAdd(int coarseLevel);    // p(fine) += I * p(coarse)
    void prolongateSet(int coarseLevel);    // p(fine)  = I * p(coarse)

    void vCycle(int level, float smootherOmega, float coarseOmega);
    void fullMultigrid(float smootherOmega, float coarseOmega);

#ifdef USE_CUDA
public:
    void setGeometryCuda();
    float solveCuda(std::vector<float>& pressure,
                    const std::vector<float>& rhs,
                    float smootherOmega,
                    float coarseOmega,
                    int   maxCycles,
                    float tolerance);

private:
    struct DeviceLevel {
        float* p = nullptr;
        float* pAlloc = nullptr;
        float* res = nullptr;
        float* resAlloc = nullptr;
        float* rhs = nullptr;
        float* cW = nullptr;
        float* cE = nullptr;
        float* cS = nullptr;
        float* cN = nullptr;
        float* diag = nullptr;
        float* invDiag = nullptr;
        uint8_t* solid = nullptr;
        float* pWeight = nullptr;
        int halo = 0;
    };

    std::vector<DeviceLevel> dev;
    float* dReduce = nullptr;
    float* hReduce = nullptr;
    int    reduceBlocks = 0;
    bool   deviceReady = false;

    void allocateDevice();
    void freeDevice();
    void smoothCuda(int level, float omega, int sweeps);
    void computeResidualCuda(int level);
    float residualNormCuda(int level);
    float rhsNormCuda(int level);
    void restrictResidualCuda(int fineLevel);
    void restrictRHSCuda(int fineLevel);
    void prolongateAddCuda(int coarseLevel);
    void prolongateSetCuda(int coarseLevel);
    void vCycleCuda(int level, float smootherOmega, float coarseOmega);
    void fullMultigridCuda(float smootherOmega, float coarseOmega);
#endif
};