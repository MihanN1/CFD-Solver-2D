#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

enum class PressureSideBC : uint8_t {
    Neumann,
    Dirichlet
};

// What the pressure sees at each side of the domain. The defaults are the
// channel every earlier version solved: open on the right, closed elsewhere.
struct MultigridBC {
    PressureSideBC left = PressureSideBC::Neumann;
    PressureSideBC right = PressureSideBC::Dirichlet;
    PressureSideBC bottom = PressureSideBC::Neumann;
    PressureSideBC top = PressureSideBC::Neumann;
};

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

    // Which sides fix the pressure and which only reflect it. Call before
    // setGeometry, or the stencil is rebuilt for you.
    void setPressureBC(const MultigridBC& bc);

    // Per face weight of the stencil, which is 1/rho once the density stops
    // being constant. faceX has (nx+1)*ny entries, faceY has nx*(ny+1); both
    // may be empty, which means one everywhere and is what the constant
    // density case uses. Cheap enough to call every step.
    void setCoefficients(const std::vector<float>& faceX,
                         const std::vector<float>& faceY);

    // No side fixes the pressure, so the operator has the constants in its
    // null space and solve() has to remove them. A closed box is exactly this.
    bool singularPressure() const { return pressureSingular; }

    // Returns the relative residual ||r|| / ||rhs|| reached
    float solve(
        std::vector<float>& pressure,
        const std::vector<float>& rhs,
        float smootherOmega,
        float coarseOmega,
        int maxCycles,
        float tolerance,
        float rhsScale = 0.0f);

    int levelCount() const { return levels; }
    int cyclesUsed() const { return lastCycles; }

    void setUseCuda(bool enable);
    bool usingCuda() const { return useCuda; }

    // The first solve() normally runs one nested iteration to build a field
    // out of nothing. A continuation arrives with the pressure of the step it
    // stopped at, which is a better guess than that pass can produce, and
    // running it anyway would nudge the field off the original trajectory.
    // Call after setGeometry(), which resets the flag. Hope it works correctly, 
    // because the user is responsible for not changing the geometry in between.
    // Otherwise we is fucked.
    void skipInitialFullMultigrid() { firstSolve = false; }

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

        // Face weights of this level, coarsened from the finest one
        std::vector<float> faceX;   // (nx+1) * ny
        std::vector<float> faceY;   // nx * (ny+1)

        // Sum of the prolongation weights that land on fluid cells, used to
        // normalise both the prolongation and the restriction
        std::vector<float> prolongWeight;

        // Only allocated when the face weights stop being one: the pressure
        // and the residual as they were before a coarse grid correction, which
        // is what the line search below needs to judge it by.
        std::vector<float> savedPressure;
        std::vector<float> savedResidual;
    };

    int nx;
    int ny;

    float dx;
    float dy;

    int minCoarseSize;
    int levels = 0;
    int lastCycles = 0;
    bool geometryReady = false;
    bool pressureSingular = false;
    bool coefficientsUniform = true;
    MultigridBC pressureBC;
    std::vector<float> fineFaceX;
    std::vector<float> fineFaceY;
    bool firstSolve = true;
    bool useCuda = false;

    std::vector<Level> gridLevels;

    void buildHierarchy();

    void buildCoefficients(Level& grid);

    void coarsenFaceWeights();
    void rebuildCoefficients();
    void removeNullSpace(float* values, const Level& grid) const;

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

    // Takes the correction the coarse grid just handed up and scales it by
    // however much of it actually reduces the residual. Bilinear interpolation
    // assumes the solution is smooth across a coarse cell, and at a density
    // jump it is not: the correction comes back pointing the right way and far
    // too long, and a V-cycle that keeps applying it in full grows instead of
    // converging. One extra operator application per level says how long the
    // step should have been. Skipped entirely while the coefficients are
    // uniform, where the answer is one and always was.
    void dampCorrection(int level);

    // out = A x, with A the same five point operator the smoother inverts.
    void applyOperator(int level, const float* x, float* out) const;

    // Conjugate gradients with one V-cycle as the preconditioner, used when
    // the face weights are not all one. Plain V-cycles are enough for a
    // constant coefficient Laplacian and are what every single phase run still
    // gets, bit for bit. Across a density jump they are not: the coarse grids
    // stop representing the fine problem, the convergence rate goes from 0.1
    // per cycle to 0.9 and then past 1, and no amount of cycles gets there.
    // The Krylov iteration outside them does not care how good the
    // preconditioner is - it only needs it to point roughly the right way, and
    // it gets the length right by construction.
    float solvePCG(std::vector<float>& pressure,
                   const std::vector<float>& rhs,
                   float smootherOmega,
                   float coarseOmega,
                   int maxCycles,
                   float tolerance,
                   float rhsScale);

    Field cgX, cgR, cgZ, cgD, cgQ;
    std::vector<float> cgPrevR;
    bool cudaFallbackReported = false;
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
        // cudaMalloc aborts the process when the toolkit is there but no device
        // is behind it, which is a whole class of machines, so the backend is
        // asked whether it exists at all before anything is allocated on it.
        static bool cudaDeviceAvailable();

        void setGeometryCuda();

        void uploadCoefficientsCuda();

        float solveCuda(
            std::vector<float>& pressure,
            const std::vector<float>& rhs,
            float smootherOmega,
            float coarseOmega,
            int maxCycles,
            float tolerance,
            float rhsScale);

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
