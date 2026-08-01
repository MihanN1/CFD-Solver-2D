#include "Multigrid.hpp"
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#ifdef USE_CUDA
#include "MultigridCuda.cuh"
#endif

Multigrid::Multigrid(int nx_, int ny_, float dx_, float dy_): nx(nx_), ny(ny_), dx(dx_), dy(dy_), levels(0)
{}

void Multigrid::solve(
    std::vector<float>& pressure,
    const std::vector<float>& rhs,
    const std::vector<uint8_t>& solid,
    float omega,
    int iterations)
{
    #ifdef USE_CUDA
        solveCuda(pressure, rhs, solid, omega, iterations);
        return;
    #else
    if (levels == 0)
        buildHierarchy();

    pressureLevels[0] = pressure;
    rhsLevels[0] = rhs;
    solidLevels[0] = solid;
    std::fill(
        residualLevels[0].begin(),
        residualLevels[0].end(),
        0.0f);
    constexpr float tolerance = 1e-6f;

    for (int level = 1; level < levels; ++level)
    {
        std::fill(
            pressureLevels[level].begin(),
            pressureLevels[level].end(),
            0.0f);

        std::fill(
            rhsLevels[level].begin(),
            rhsLevels[level].end(),
            0.0f);

        std::fill(
            solidLevels[level].begin(),
            solidLevels[level].end(),
            static_cast<uint8_t>(0));
        std::fill(
            residualLevels[level].begin(),
            residualLevels[level].end(),
            0.0f);
    }

    fullMultigrid(omega);

    for (int cycle = 0; cycle < iterations; ++cycle){
        computeResidual(0);

        if (computeResidualNorm() < tolerance)
            break;

        vCycle(0, omega);
    }
    pressure = pressureLevels[0];
    #endif
}
void Multigrid::smoothSOR(
    int level,
    std::vector<float>& pressure,
    const std::vector<float>& rhs,
    const std::vector<uint8_t>& solid,
    float omega,
    int iterations){
    const int nx = levelNx[level];
    const int ny = levelNy[level];

    const float invDx2 = levelInvDx2[level];
    const float invDy2 = levelInvDy2[level];

    for (int iter = 0; iter < iterations; ++iter){
        for (int color = 0; color < 2; ++color){
            #pragma omp parallel for schedule(static)
            for (int j = 1; j < ny - 1; ++j){
                const int row = j * nx;
                const int rowTop = (j + 1) * nx;
                const int rowBottom = (j - 1) * nx;
                int iStart = 1 + ((j + color) & 1);
                for (int i = iStart; i < nx - 1; i += 2){
                    const int id = row + i;

                    if (solid[id])
                        continue;

                    const float leftMask  = solid[id - 1] ? 0.0f : 1.0f;
                    const float rightMask = solid[id + 1] ? 0.0f : 1.0f;
                    const float downMask  = solid[rowBottom + i] ? 0.0f : 1.0f;
                    const float upMask    = solid[rowTop + i] ? 0.0f : 1.0f;

                    // FIX: a masked-out (solid) neighbour must contribute 0 to BOTH
                    // the numerator and the denominator. The old code mirrored the
                    // cell's own value into pLeft/pRight/pDown/pUp for solid neighbours
                    // (a ghost-cell trick) but then excluded that same neighbour from
                    // `diagonal`. That mismatch injected an extra +self*invD2 term into
                    // the numerator with nothing to balance it in the denominator, so
                    // every SOR sweep multiplied the value of any solid-adjacent cell
                    // by a factor > 1 -- unconditionally, even for an already-converged
                    // field. That is the source of the pressure blow-up around solid
                    // bodies. Bruh im dyin -_-
                    const float pLeft  = leftMask  * pressure[id - 1];
                    const float pRight = rightMask * pressure[id + 1];
                    const float pDown  = downMask  * pressure[rowBottom + i];
                    const float pUp    = upMask    * pressure[rowTop + i];

                    const float diagonal =
                        (leftMask + rightMask) * invDx2 +
                        (downMask + upMask) * invDy2;

                    if (diagonal < 1e-12f)
                        continue;

                    const float pNew =
                        ((pLeft + pRight) * invDx2 +
                        (pDown + pUp) * invDy2 -
                        rhs[id]) / diagonal;

                    const float oldP = pressure[id];
                    pressure[id] = oldP + omega * (pNew - oldP);
                }
            }
        }
    }
}
void Multigrid::buildHierarchy(){
    pressureLevels.clear();
    rhsLevels.clear();
    residualLevels.clear();
    solidLevels.clear();

    levelNx.clear();
    levelNy.clear();
    levelInvDx2.clear();
    levelInvDy2.clear();

    int currentNx = nx;
    int currentNy = ny;
    float currentDx = dx;
    float currentDy = dy;

    levels = 0;

    while (true){
        ++levels;

        levelNx.push_back(currentNx);
        levelNy.push_back(currentNy);
        levelInvDx2.push_back(1.0f / (currentDx * currentDx));
        levelInvDy2.push_back(1.0f / (currentDy * currentDy));

        const int size = currentNx * currentNy;

        pressureLevels.emplace_back(size, 0.0f);
        rhsLevels.emplace_back(size, 0.0f);
        residualLevels.emplace_back(size, 0.0f);
        solidLevels.emplace_back(size, static_cast<uint8_t>(0));

        if (currentNx <= 4 || currentNy <= 4)
            break;

        currentNx = std::max(2, (currentNx + 1) / 2);
        currentNy = std::max(2, (currentNy + 1) / 2);
        currentDx *= 2.0f;
        currentDy *= 2.0f;
    }
}
void Multigrid::computeResidual(int level){
    const int nx = levelNx[level];
    const int ny = levelNy[level];
    const float invDx2 = levelInvDx2[level];
    const float invDy2 = levelInvDy2[level];
    const __m256 invDx2Vec = _mm256_set1_ps(invDx2);
    const __m256 invDy2Vec = _mm256_set1_ps(invDy2);

    auto& p = pressureLevels[level];
    auto& rhs = rhsLevels[level];
    auto& residual = residualLevels[level];
    auto& solid = solidLevels[level];

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny - 1; ++j){
        const int row = j * nx;
        const int rowTop = (j + 1) * nx;
        const int rowBottom = (j - 1) * nx;
        int i = 1;
        for (; i <= nx - 9; i += 8){
            bool skip = false;
            for (int k = 0; k < 8; ++k){
                if (solid[row + i + k])
                {
                    skip = true;
                    break;
                }
            }
            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    int id = row + ii;
                    if (solid[id]){
                        residual[id] = 0.f;
                        continue;
                    }
                    float Ap = 0.0f;
                    if (!solid[id - 1])
                        Ap += (p[id - 1] - p[id]) * invDx2;

                    if (!solid[id + 1])
                        Ap += (p[id + 1] - p[id]) * invDx2;

                    if (!solid[rowBottom + ii])
                        Ap += (p[rowBottom + ii] - p[id]) * invDy2;

                    if (!solid[rowTop + ii])
                        Ap += (p[rowTop + ii] - p[id]) * invDy2;

                    residual[id] = rhs[id] - Ap;
                }
                continue;
            }
            __m256 pCenter =
                _mm256_loadu_ps(p.data() + row + i);
            __m256 pLeft =
                _mm256_loadu_ps(p.data() + row + i - 1);
            __m256 pRight =
                _mm256_loadu_ps(p.data() + row + i + 1);
            __m256 pDown =
                _mm256_loadu_ps(p.data() + rowBottom + i);
            __m256 pUp =
                _mm256_loadu_ps(p.data() + rowTop + i);
            __m256 rhsVec =
                _mm256_loadu_ps(rhs.data() + row + i);
            __m256 Ap =
                _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_add_ps(
                            _mm256_sub_ps(pLeft, pCenter),
                            _mm256_sub_ps(pRight, pCenter)),
                        invDx2Vec),
                    _mm256_mul_ps(
                        _mm256_add_ps(
                            _mm256_sub_ps(pDown, pCenter),
                            _mm256_sub_ps(pUp, pCenter)),
                        invDy2Vec));
            __m256 res =
                _mm256_sub_ps(
                    rhsVec,
                    Ap);
            _mm256_storeu_ps(
                residual.data() + row + i,
                res);
        }
        for (; i < nx - 1; ++i){
            const int id = row + i;
            if (solid[id]){
                residual[id] = 0.0f;
                continue;
            }
            float Ap = 0.0f;
            if (!solid[id - 1])
                Ap += (p[id - 1] - p[id]) * invDx2;

            if (!solid[id + 1])
                Ap += (p[id + 1] - p[id]) * invDx2;

            if (!solid[rowBottom + i])
                Ap += (p[rowBottom + i] - p[id]) * invDy2;

            if (!solid[rowTop + i])
                Ap += (p[rowTop + i] - p[id]) * invDy2;

            residual[id] = rhs[id] - Ap;
        }
    }
}
float Multigrid::computeResidualNorm() const{
    const auto& residual = residualLevels[0];
    const float* ptr = residual.data();
    const int n = static_cast<int>(residual.size());
    __m256 sumVec = _mm256_setzero_ps();
    int i = 0;
    for (; i <= n - 8; i += 8){
        __m256 r =
            _mm256_loadu_ps(ptr + i);
        sumVec =
            _mm256_add_ps(
                sumVec,
                _mm256_mul_ps(r, r));
    }
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, sumVec);
    float sum =
        tmp[0] + tmp[1] + tmp[2] + tmp[3] +
        tmp[4] + tmp[5] + tmp[6] + tmp[7];
    for (; i < n; ++i)
        sum += ptr[i] * ptr[i];
    return std::sqrt(sum);
}
void Multigrid::restrictResidual(int fineLevel){
    if (fineLevel + 1 >= levels)
        return;
    const int coarseLevel = fineLevel + 1;

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    auto& fineResidual = residualLevels[fineLevel];
    const auto& fineSolid = solidLevels[fineLevel];

    auto& coarseRhs = rhsLevels[coarseLevel];
    auto& coarsePressure = pressureLevels[coarseLevel];
    auto& coarseSolid = solidLevels[coarseLevel];

    std::fill(coarsePressure.begin(), coarsePressure.end(), 0.0f);
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < coarseNy - 1; ++j){
        for (int i = 1; i < coarseNx - 1; ++i){

            const int x = std::min(i * 2, fineNx - 2);
            const int y = std::min(j * 2, fineNy - 2);
            float sum = 0.0f;
            float weightSum = 0.0f;

            auto add = [&](int xx, int yy, float w)
            {
                if (xx < 0 || xx >= fineNx || yy < 0 || yy >= fineNy)
                    return;

                int id = yy * fineNx + xx;

                if (!fineSolid[id])
                {
                    sum += w * fineResidual[id];
                    weightSum += w;
                }
            };
            add(x - 1, y - 1, 1.0f);
            add(x    , y - 1, 2.0f);
            add(x + 1, y - 1, 1.0f);

            add(x - 1, y    , 2.0f);
            add(x    , y    , 4.0f);
            add(x + 1, y    , 2.0f);

            add(x - 1, y + 1, 1.0f);
            add(x    , y + 1, 2.0f);
            add(x + 1, y + 1, 1.0f);
            coarseRhs[j * coarseNx + i] = weightSum > 0.0f ? sum / weightSum : 0.0f;
            bool solidFlag = false;
            for (int yy = y - 1; yy <= y + 1; ++yy) {
                for (int xx = x - 1; xx <= x + 1; ++xx) {
                    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy) {
                        if (fineSolid[yy * fineNx + xx]) {
                            solidFlag = true;
                            break;
                        }
                    }
                }
                if (solidFlag) break;
            }
            coarseSolid[j * coarseNx + i] = static_cast<uint8_t>(solidFlag ? 1 : 0);
        }
    }
}
void Multigrid::prolongateCorrection(int coarseLevel){
    if (coarseLevel <= 0)
        return;
    const int fineLevel = coarseLevel - 1;

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    auto& finePressure = pressureLevels[fineLevel];

    const auto& coarsePressure = pressureLevels[coarseLevel];
    const auto& fineSolid = solidLevels[fineLevel];
    const auto& coarseSolid = solidLevels[coarseLevel];

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < fineNy; ++j){
        const float y = static_cast<float>(j) * 0.5f;

        int jc0 = static_cast<int>(y);
        int jc1 = std::min(jc0 + 1, coarseNy - 1);
        
        float ty = y - jc0;

        for (int i = 0; i < fineNx; ++i){

            const int fineId = j * fineNx + i;

            if (fineSolid[fineId])
                continue;

            const float x = static_cast<float>(i) * 0.5f;

            int ic0 = static_cast<int>(x);
            int ic1 = std::min(ic0 + 1, coarseNx - 1);

            float tx = x - ic0;

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

            if (!coarseSolid[id00])
            {
                value += w00 * coarsePressure[id00];
                weight += w00;
            }

            if (!coarseSolid[id10])
            {
                value += w10 * coarsePressure[id10];
                weight += w10;
            }

            if (!coarseSolid[id01])
            {
                value += w01 * coarsePressure[id01];
                weight += w01;
            }

            if (!coarseSolid[id11])
            {
                value += w11 * coarsePressure[id11];
                weight += w11;
            }

            if (weight > 0.0f)
                finePressure[fineId] += value / weight;

        }
    }
}
void Multigrid::prolongateSolution(int coarseLevel){
    if (coarseLevel <= 0)
        return;

    int fineLevel = coarseLevel - 1;

    std::fill(pressureLevels[fineLevel].begin(), pressureLevels[fineLevel].end(), 0.0f);

    const int fineNx = levelNx[fineLevel];
    const int fineNy = levelNy[fineLevel];

    const int coarseNx = levelNx[coarseLevel];
    const int coarseNy = levelNy[coarseLevel];

    auto& finePressure = pressureLevels[fineLevel];

    const auto& coarsePressure = pressureLevels[coarseLevel];
    const auto& fineSolid = solidLevels[fineLevel];
    const auto& coarseSolid = solidLevels[coarseLevel];

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < fineNy; ++j){
        const float y = static_cast<float>(j) * 0.5f;

        int jc0 = static_cast<int>(y);
        int jc1 = std::min(jc0 + 1, coarseNy - 1);
        
        float ty = y - jc0;

        for (int i = 0; i < fineNx; ++i){

            const int fineId = j * fineNx + i;

            if (fineSolid[fineId])
                continue;

            const float x = static_cast<float>(i) * 0.5f;

            int ic0 = static_cast<int>(x);
            int ic1 = std::min(ic0 + 1, coarseNx - 1);

            float tx = x - ic0;

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

            if (!coarseSolid[id00])
            {
                value += w00 * coarsePressure[id00];
                weight += w00;
            }

            if (!coarseSolid[id10])
            {
                value += w10 * coarsePressure[id10];
                weight += w10;
            }

            if (!coarseSolid[id01])
            {
                value += w01 * coarsePressure[id01];
                weight += w01;
            }

            if (!coarseSolid[id11])
            {
                value += w11 * coarsePressure[id11];
                weight += w11;
            }

            if (weight > 0.0f)
                finePressure[fineId] = value / weight;

        }
    }
}
// Multigrid vCycle, recursive! Cool, right?
void Multigrid::vCycle(
    int level,
    float omega)
{
    const int preSmooth = 2;
    const int postSmooth = 2;
    const int coarseSmooth = 50;
    // Coarsest grid
    if (level == levels - 1)
    {
        smoothSOR(
            level,
            pressureLevels[level],
            rhsLevels[level],
            solidLevels[level],
            omega,
            coarseSmooth);

        return;
    }

    // Pre-smoothing
    smoothSOR(
        level,
        pressureLevels[level],
        rhsLevels[level],
        solidLevels[level],
        omega,
        preSmooth);

    // Residual
    computeResidual(level);

    // Restriction
    restrictResidual(level);

    // Initialize coarse grid pressure to zero
    std::fill(
        pressureLevels[level + 1].begin(),
        pressureLevels[level + 1].end(),
        0.0f);

    // Recursive call
    vCycle(level + 1, omega);

    // Prolongation
    prolongateCorrection(level + 1);

    // Post-smoothing
    smoothSOR(
        level,
        pressureLevels[level],
        rhsLevels[level],
        solidLevels[level],
        omega,
        postSmooth);
}
void Multigrid::restrictRHS(int fineLevel){
    if (fineLevel + 1 >= levels)
        return;

    int coarseLevel = fineLevel + 1;

    auto& fineRhs = rhsLevels[fineLevel];
    auto& coarseRhs = rhsLevels[coarseLevel];

    int fineNx = levelNx[fineLevel];
    int fineNy = levelNy[fineLevel];

    int coarseNx = levelNx[coarseLevel];
    int coarseNy = levelNy[coarseLevel];

    auto& fineSolid = solidLevels[fineLevel];
    auto& coarseSolid = solidLevels[coarseLevel];

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < coarseNy - 1; j++)
    {
        for (int i = 1; i < coarseNx - 1; i++)
        {
            int x = std::min(i * 2, fineNx - 2);
            int y = std::min(j * 2, fineNy - 2);
            float sum = 0.0f;
            float wsum = 0.0f;
            auto add = [&](int x,int y,float w)
            {
                if(x<0||x>=fineNx||y<0||y>=fineNy)
                    return;

                int id=y*fineNx+x;

                if(!fineSolid[id])
                {
                    sum+=w*fineRhs[id];
                    wsum+=w;
                }
            };
            add(x-1,y-1,1);
            add(x  ,y-1,2);
            add(x+1,y-1,1);

            add(x-1,y  ,2);
            add(x  ,y  ,4);
            add(x+1,y  ,2);

            add(x-1,y+1,1);
            add(x  ,y+1,2);
            add(x+1,y+1,1);

            coarseRhs[j*coarseNx+i]=(wsum>0)?sum/wsum:0;
            bool solidFlag = false;
            for (int yy = y - 1; yy <= y + 1; ++yy) {
                for (int xx = x - 1; xx <= x + 1; ++xx) {
                    if (xx >= 0 && xx < fineNx && yy >= 0 && yy < fineNy) {
                        if (fineSolid[yy * fineNx + xx]) {
                            solidFlag = true;
                            break;
                        }
                    }
                }
                if (solidFlag) break;
            }
            coarseSolid[j * coarseNx + i] = static_cast<uint8_t>(solidFlag ? 1 : 0);
        }
    }
}
void Multigrid::fullMultigrid(float omega){
    int coarsest = levels - 1;

    for (int level = 0; level < coarsest; ++level)
        restrictRHS(level);

    std::fill(
        pressureLevels[coarsest].begin(),
        pressureLevels[coarsest].end(),
        0.0f);

    smoothSOR(
        coarsest,
        pressureLevels[coarsest],
        rhsLevels[coarsest],
        solidLevels[coarsest],
        omega,
        50);

    for (int level = coarsest; level > 0; --level)
    {
        prolongateSolution(level);

        vCycle(level - 1, omega);
    }
}