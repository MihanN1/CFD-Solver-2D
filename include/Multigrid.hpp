#pragma once
#include <vector>
#include <cstdint>

class Multigrid {
public:
    Multigrid(int nx, int ny, float dx, float dy);

    void solve(
        std::vector<float>& pressure,
        const std::vector<float>& rhs,
        const std::vector<uint8_t>& solid,
        float omega,
        int iterations);
private:
    int nx;
    int ny;

    float dx;
    float dy;

    int levels;

    std::vector<std::vector<float>> pressureLevels;
    std::vector<std::vector<float>> rhsLevels;
    std::vector<std::vector<float>> residualLevels;
    std::vector<std::vector<uint8_t>> solidLevels;
    std::vector<int> levelNx;
    std::vector<int> levelNy;
    std::vector<float> levelInvDx2;
    std::vector<float> levelInvDy2;
    #ifdef USE_CUDA
        std::vector<float*> dPressureLevels;
        std::vector<float*> dRhsLevels;
        std::vector<float*> dResidualLevels;
        std::vector<uint8_t*> dSolidLevels;
    #endif

    void buildHierarchy();

    void fullMultigrid(float omega);

    void restrictRHS(int fineLevel);

    void vCycle(
        int level,
        float omega);

    void smoothSOR(
        int level,
        std::vector<float>& pressure,
        const std::vector<float>& rhs,
        const std::vector<uint8_t>& solid,
        float omega,
        int iterations);

    void computeResidual(int level);
    float computeResidualNorm() const;

    void restrictResidual(int fineLevel);

    void prolongateCorrection(int coarseLevel);
    void prolongateSolution(int coarseLevel);
    #ifdef USE_CUDA
    public:
        void solveCuda(
            std::vector<float>& pressure,
            const std::vector<float>& rhs,
            const std::vector<uint8_t>& solid,
            float omega,
            int iterations);

    private:
        void smoothSORCuda(
            int level,
            float omega,
            int iterations);

        void computeResidualCuda(int level);

        float computeResidualNormCuda();

        void restrictResidualCuda(int fineLevel);

        void restrictRHSCuda(int fineLevel);

        void prolongateCorrectionCuda(int coarseLevel);

        void prolongateSolutionCuda(int coarseLevel);

        void vCycleCuda(
            int level,
            float omega);

        void fullMultigridCuda(float omega);

        size_t sizeBytes(int level) const;
    #endif
};