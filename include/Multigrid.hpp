#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

class Multigrid {
public:
    Multigrid(int nx, int ny, float dx, float dy, int minCoarseSize = 8);
    ~Multigrid();

    // The levels own raw device pointers, so copying is forbidden
    Multigrid(const Multigrid&) = delete;
    Multigrid& operator=(const Multigrid&) = delete;

    // Builds the level hierarchy and the stencil coefficients for the geometry.
    // Must be called before solve().
    void setGeometry(const std::vector<uint8_t>& solid);

    // Returns the relative residual ||r|| / ||rhs|| reached
    float solve(
        std::vector<float>& pressure,
        const std::vector<float>& rhs,
        float smootherOmega,
        float coarseOmega,
        int maxCycles,
        float tolerance);

    int levelCount() const { return levels; }
    int cyclesUsed() const { return lastCycles; }

    void setUseCuda(bool enable);
    bool usingCuda() const { return useCuda; }

private:
    // Array with a halo on both sides, so the stencil can read one cell past
    // the ends without an out-of-range check
    struct Field {
        std::vector<float> storage;
        float* base = nullptr;
        int halo = 0;

        void init(int cellCount, int haloWidth) {
            halo = haloWidth;
            storage.assign(
                static_cast<size_t>(cellCount) + 2u * haloWidth + 16u, 0.0f);
            base = storage.data() + haloWidth;
        }
        void zero() {
            std::fill(storage.begin(), storage.end(), 0.0f);
        }
        float* data() { return base; }
        const float* data() const { return base; }
    };

    // One grid of the hierarchy
    struct Level {
        int nx = 0;
        int ny = 0;
        int cellCount = 0;
        float dx = 0.0f;
        float dy = 0.0f;

        // Coarsening ratio towards the next coarser level (1 = axis not coarsened)
        int refineX = 1;
        int refineY = 1;

        Field pressure;
        Field residual;
        std::vector<float> rhs;

        // Five point stencil: West/East/South/North neighbours and the diagonal
        std::vector<float> coefW, coefE, coefS, coefN;
        std::vector<float> diag;
        std::vector<float> invDiag;

        std::vector<uint8_t> solid;

        // Sum of the prolongation weights that land on fluid cells, used to
        // normalise both the prolongation and the restriction
        std::vector<float> prolongWeight;
    };

    int nx;
    int ny;

    float dx;
    float dy;

    int minCoarseSize;
    int levels = 0;
    int lastCycles = 0;
    bool geometryReady = false;
    bool firstSolve = true;
    bool useCuda = false;

    std::vector<Level> gridLevels;

    void buildHierarchy();

    void buildCoefficients(Level& grid);

    void fullMultigrid(float smootherOmega, float coarseOmega);

    void vCycle(
        int level,
        float smootherOmega,
        float coarseOmega);

    void smoothSOR(
        int level,
        float omega,
        int sweeps);

    void computeResidual(int level);
    float computeResidualNorm(int level) const;
    static float computeVectorNorm(const float* values, int count);

    void buildTransferWeights(int fineLevel);
    void restrictField(int fineLevel, const float* fineSrc);
    void restrictResidual(int fineLevel);   // res(fine) -> rhs(coarse)
    void restrictRHS(int fineLevel);        // rhs(fine)  -> rhs(coarse), for FMG

    void prolongateCorrection(int coarseLevel);  // p(fine) += I * p(coarse)
    void prolongateSolution(int coarseLevel);    // p(fine)  = I * p(coarse)

    #ifdef USE_CUDA
    public:
        void setGeometryCuda();

        float solveCuda(
            std::vector<float>& pressure,
            const std::vector<float>& rhs,
            float smootherOmega,
            float coarseOmega,
            int maxCycles,
            float tolerance);

    private:
        // Device side mirror of Level
        struct DeviceLevel {
            float* pressure = nullptr;
            float* pressureAlloc = nullptr;
            float* residual = nullptr;
            float* residualAlloc = nullptr;
            float* rhs = nullptr;
            float* coefW = nullptr;
            float* coefE = nullptr;
            float* coefS = nullptr;
            float* coefN = nullptr;
            float* diag = nullptr;
            float* invDiag = nullptr;
            uint8_t* solid = nullptr;
            float* prolongWeight = nullptr;
            int halo = 0;
        };

        std::vector<DeviceLevel> deviceLevels;

        float* deviceReduceBuffer = nullptr;
        float* hostReduceBuffer = nullptr;
        int reduceBlocks = 0;
        bool deviceReady = false;

        void allocateDevice();

        void freeDevice();

        void smoothSORCuda(
            int level,
            float omega,
            int sweeps);

        void computeResidualCuda(int level);

        float computeResidualNormCuda(int level);

        float computeRhsNormCuda(int level);

        void restrictResidualCuda(int fineLevel);

        void restrictRHSCuda(int fineLevel);

        void prolongateCorrectionCuda(int coarseLevel);

        void prolongateSolutionCuda(int coarseLevel);

        void vCycleCuda(
            int level,
            float smootherOmega,
            float coarseOmega);

        void fullMultigridCuda(float smootherOmega, float coarseOmega);
    #endif
};
