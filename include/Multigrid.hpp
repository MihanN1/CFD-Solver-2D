#pragma once
#include <vector>

class Multigrid {
public:
    Multigrid(int nx, int ny, float dx, float dy);

    void solve(
        std::vector<float>& pressure,
        const std::vector<float>& rhs,
        const std::vector<bool>& solid,
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
    std::vector<std::vector<bool>> solidLevels;
    std::vector<int> levelNx;
    std::vector<int> levelNy;
    std::vector<float> levelInvDx2;
    std::vector<float> levelInvDy2;

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
        const std::vector<bool>& solid,
        float omega);

    void computeResidual(int level);
    float computeResidualNorm() const;

    void restrictResidual(int fineLevel);

    void prolongateCorrection(int coarseLevel);
    void prolongateSolution(int coarseLevel);
};