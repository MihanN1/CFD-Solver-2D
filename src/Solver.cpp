#include "Solver.hpp"
#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <immintrin.h>
#include <array>

static const int SAVE_INTERVAL = 1; // save every step, so that we can determine the mistakes and debug the code more easily. It can be changed to a larger number for faster simulations.
constexpr int dtUpdateInterval = 5;

Solver::Solver(const Config& cfg, const Mesh& mesh)
    : 
    cfg(cfg),
    mesh(mesh),
    multigrid(cfg.nx, cfg.ny, mesh.dx, mesh.dy)
{
    // Allocate arrays
    p.assign(cfg.nx * cfg.ny, 0.0f);
    rhs.assign(cfg.nx * cfg.ny, 0.0f);
    u.assign((cfg.nx + 1) * cfg.ny, 0.0f);
    v.assign(cfg.nx * (cfg.ny + 1), 0.0f);
    u_star.assign((cfg.nx + 1) * cfg.ny, 0.0f);
    v_star.assign(cfg.nx * (cfg.ny + 1), 0.0f);
}

void Solver::initFields()
{
    std::fill(p.begin(), p.end(), 0.0f);
    std::fill(rhs.begin(), rhs.end(), 0.0f);
    std::fill(u.begin(), u.end(), 0.0f);
    std::fill(v.begin(), v.end(), 0.0f);
    buildFaceMasks();
    solidMask.assign(mesh.solid.begin(), mesh.solid.end());

    for (int j = 0; j < cfg.ny; j++) {
        for (int i = 0; i <= cfg.nx; i++) {
            bool solidLeft = false;
            bool solidRight = false;
            if (i > 0)
                solidLeft = mesh.solid[j*cfg.nx + (i-1)];
            if (i < cfg.nx)
                solidRight = mesh.solid[j*cfg.nx + i];
            if (!(solidLeft || solidRight))
                u[idxU(i,j)] = cfg.U0;
        }
    }
    applyBC();
    std::cout<<"Fields initialized.\n";
}

void Solver::buildFaceMasks(){
    int nx = cfg.nx;
    int ny = cfg.ny;

    uFluidMask.assign((nx + 1) * ny, 0);
    vFluidMask.assign(nx * (ny + 1), 0);

    // u faces
    for (int j = 0; j < ny; ++j){
        for (int i = 1; i < nx; ++i){
            if (!(mesh.solid[j*nx+i] && mesh.solid[j*nx+i-1])){
                uFluidMask[idxU(i,j)] = 1;
            }
        }
    }

    // v faces
    for (int j = 1; j < ny; ++j){
        for (int i = 0; i < nx; ++i){
            if (!(mesh.solid[j*nx+i] && mesh.solid[(j-1)*nx+i])){
                vFluidMask[idxV(i,j)] = 1;
            }
        }
    }
}

void Solver::computeDt(){
    float maxU = 0.0f;
    float maxV = 0.0f;

    int nx = cfg.nx;
    int ny = cfg.ny;
    const __m256 signMask = _mm256_set1_ps(-0.0f);
    __m256 maxUVec = _mm256_setzero_ps();
    __m256 maxVVec = _mm256_setzero_ps();

    for (int j = 0; j < ny; ++j)
    {
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;

        int i = 0;
        for (; i <= nx - 8; i += 8)
        {
            __m256 uVec = _mm256_andnot_ps(signMask,
                _mm256_loadu_ps(u.data() + rowU + i));
            __m256 vVec = _mm256_andnot_ps(signMask,
                _mm256_loadu_ps(v.data() + rowV + i));

            maxUVec = _mm256_max_ps(maxUVec, uVec);
            maxVVec = _mm256_max_ps(maxVVec, vVec);
        }

        // хвост обязателен
        for (; i < nx; ++i)
        {
            maxU = std::max(maxU, std::fabs(u[rowU + i]));
            maxV = std::max(maxV, std::fabs(v[rowV + i]));
        }

        maxU = std::max(maxU, std::fabs(u[rowU + nx]));
    }

    // горизонтальная редукция ОДИН раз
    alignas(32) float tmpU[8], tmpV[8];
    _mm256_store_ps(tmpU, maxUVec);
    _mm256_store_ps(tmpV, maxVVec);

    for (int k = 0; k < 8; ++k)
    {
        maxU = std::max(maxU, tmpU[k]);
        maxV = std::max(maxV, tmpV[k]);
    }
    const float invDx = 1.f / mesh.dx;
    const float invDy = 1.f / mesh.dy;

    const float adv = maxU * invDx + maxV * invDy;

    const float dtAdv =
        (adv < 1e-12f) ?
        1e9f :
        cfg.CFL / adv;

    const float dtDiff =
        1.f /
        (2.f * cfg.nu *
        (invDx * invDx + invDy * invDy));

    dt = std::min(dtAdv, dtDiff);

    if (dt <= 0.f || std::isnan(dt) || std::isinf(dt))
        dt = 1e-6f;
}

void Solver::predictor() {
    const float invDx  = 1.0f / mesh.dx;
    const float invDy  = 1.0f / mesh.dy;
    const float invDx2 = invDx * invDx;
    const float invDy2 = invDy * invDy;
    float nu = cfg.nu;
    int nx = cfg.nx, ny = cfg.ny;
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float* __restrict uStar = u_star.data();
    float* __restrict vStar = v_star.data();
    const float dtNu = dt * nu;
    const float dtConv = dt;
    constexpr float quarter = 0.25f;

    // Compute u_star for internal fluid cells (i = 1..nx-1, j = 1..ny-2)
    // u is on vertical faces, so we need to compute convection and diffusion at those points
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny-1; ++j) {
        const int rowU = j * (nx + 1);
        const uint8_t* uMask = uFluidMask.data() + rowU;
        const int rowUTop = (j + 1) * (nx + 1);
        const int rowUBot = (j - 1) * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uTop = uPtr + rowUTop;
        const float* __restrict uBot = uPtr + rowUBot;
        const float* __restrict vRow = vPtr + rowV;
        const float* __restrict vTop = vPtr + rowVTop;
        float* __restrict uStarRow = uStar + rowU;
        int i = 1;
        __m256 zero = _mm256_setzero_ps();
        __m256 two = _mm256_set1_ps(2.f);
        for (; i <= nx - 8; i += 8){
            bool skip = false;

            for (int k = 0; k < 8; k++)
            {
                if (!uMask[i + k])
                {
                    skip = true;
                    break;
                }
            }
            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    if (!uMask[ii]){
                        uStarRow[ii] = 0.f;
                        continue;
                    }
                    // Convection: upwind for u
                    float u_ij = uRow[ii];
                    float v_n = quarter * (
                        vTop[ii-1] +
                        vTop[ii] +
                        vRow[ii-1] +
                        vRow[ii]
                    );
                    

                    float u_top = uTop[ii];
                    float u_bot = uBot[ii];
                    float u_right = uRow[ii+1];
                    float u_left  = uRow[ii-1];
                    float dudy = (v_n > 0) ? (u_ij - u_bot) * invDy : (u_top - u_ij) * invDy;
                    float dudx = (u_ij > 0) ? (u_ij - u_left) * invDx : (u_right - u_ij) * invDx;
                    // Diffusion: central differences
                    float d2udx2 = (u_right - 2.0f*u_ij + u_left) * invDx2;
                    float d2udy2 = (u_top - 2.0f*u_ij + u_bot) * invDy2;

                    uStarRow[ii] = u_ij - dtConv * (u_ij * dudx + v_n * dudy) + dtNu * (d2udx2 + d2udy2);
                }
                continue;
            }
            __m256 uij =
                _mm256_loadu_ps(uRow + i);
            __m256 utop =
                _mm256_loadu_ps(uTop + i);
            __m256 ubot =
                _mm256_loadu_ps(uBot + i);
            __m256 uright =
                _mm256_loadu_ps(uRow + i + 1);
            __m256 uleft =
                _mm256_loadu_ps(uRow + i - 1);
            __m256 vt0 =
                _mm256_loadu_ps(vTop + i - 1);
            __m256 vt1 =
                _mm256_loadu_ps(vTop + i);
            __m256 vb0 =
                _mm256_loadu_ps(vRow + i - 1);
            __m256 vb1 =
                _mm256_loadu_ps(vRow + i);
            __m256 vn =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(vt0, vt1),
                        _mm256_add_ps(vb0, vb1)),
                        _mm256_set1_ps(0.25f));
            __m256 dudx_back =
                _mm256_mul_ps(
                    _mm256_sub_ps(uij, uleft),
                    _mm256_set1_ps(invDx));
            __m256 dudx_forw =
                _mm256_mul_ps(
                    _mm256_sub_ps(uright, uij),
                    _mm256_set1_ps(invDx));
            __m256 dudy_back =
                _mm256_mul_ps(
                    _mm256_sub_ps(uij, ubot),
                    _mm256_set1_ps(invDy));
            __m256 dudy_forw =
                _mm256_mul_ps(
                    _mm256_sub_ps(utop, uij),
                    _mm256_set1_ps(invDy));
            __m256 maskU =
                _mm256_cmp_ps(uij, zero, _CMP_GT_OS);
            __m256 dudx =
                _mm256_blendv_ps(
                    dudx_forw,
                    dudx_back,
                    maskU);
            __m256 maskV =
                _mm256_cmp_ps(vn, zero, _CMP_GT_OS);
            __m256 dudy =
                _mm256_blendv_ps(
                    dudy_forw,
                    dudy_back,
                    maskV);
            __m256 d2udx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        uright,
                        _mm256_sub_ps(
                            uleft,
                            _mm256_mul_ps(two,uij))),
                    _mm256_set1_ps(invDx2));
            __m256 d2udy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        utop,
                        _mm256_sub_ps(
                            ubot,
                            _mm256_mul_ps(two, uij))),
                    _mm256_set1_ps(invDy2));
            __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(uij,dudx),
                    _mm256_mul_ps(vn,dudy));
            __m256 diff =
                _mm256_add_ps(
                    d2udx2,
                    d2udy2);
            __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        uij,
                        _mm256_mul_ps(
                            _mm256_set1_ps(dtConv),
                            conv)),
                    _mm256_mul_ps(
                        _mm256_set1_ps(dtNu),
                        diff));
            _mm256_storeu_ps(
                uStarRow+i,
                res);
        }
        for (; i < nx; ++i){
            if (!uMask[i]){
                uStarRow[i] = 0.f;
                continue;
            }
            float u_ij = uRow[i];
            float v_n = quarter * (
                vTop[i-1] +
                vTop[i] +
                vRow[i-1] +
                vRow[i]);

            float u_top   = uTop[i];
            float u_bot   = uBot[i];
            float u_right = uRow[i+1];
            float u_left  = uRow[i-1];
            float dudy = (v_n > 0) ?
                (u_ij - u_bot) * invDy :
                (u_top - u_ij) * invDy;

            float dudx = (u_ij > 0) ?
                (u_ij - u_left) * invDx :
                (u_right - u_ij) * invDx;

            float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] =
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + dtNu * (d2udx2 + d2udy2);
        }
    }

    // Compute v_star similarly
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j){ // internal horizontal faces
        const int rowV = j * nx;
        const uint8_t* vMask = vFluidMask.data() + rowV;
        const int rowVTop = (j + 1) * nx;
        const int rowVBot = (j - 1) * nx;
        const int rowU = j * (nx + 1);
        const int rowUBot = (j - 1) * (nx + 1);
        const float* __restrict vRow = vPtr + rowV;
        const float* __restrict vTop = vPtr + rowVTop;
        const float* __restrict vBot = vPtr + rowVBot;
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uBot = uPtr + rowUBot;
        float* __restrict vStarRow = vStar + rowV;
        int i = 1;
        __m256 zero = _mm256_setzero_ps();
        __m256 two = _mm256_set1_ps(2.f);
        for (; i <= nx - 9; i += 8)
        {
            bool skip = false;
            for (int k = 0; k < 8; ++k)
            {
                if (!vMask[i + k])
                {
                    skip = true;
                    break;
                }
            }

            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    if (!vMask[ii]){
                        vStarRow[ii] = 0.f;
                        continue;
                    }
                    float v_ij = vRow[ii];
                    float u_e = quarter * (
                        uRow[ii+1] +
                        uBot[ii+1] +
                        uRow[ii] +
                        uBot[ii]
                    );
                    float v_right = vRow[ii+1];
                    float v_left  = vRow[ii-1];
                    float v_top = vTop[ii];
                    float v_bot = vBot[ii];
                    // dv/dx with upwind in x
                    float dvdx = (u_e > 0) ? (v_ij - v_left) * invDx : (v_right - v_ij) * invDx;
                    // dv/dy with upwind in y
                    float dvdy = (v_ij > 0) ? (v_ij - v_bot) * invDy : (v_top - v_ij) * invDy;
                    // Diffusion
                    float d2vdx2 = (v_right - 2.0f*v_ij + v_left) * invDx2;
                    float d2vdy2 = (v_top - 2.0f*v_ij + v_bot) * invDy2;
                    vStarRow[ii] = v_ij - dtConv * (u_e * dvdx + v_ij * dvdy) + dtNu * (d2vdx2 + d2vdy2);
                }
                continue;
            }
            __m256 vij =
                _mm256_loadu_ps(vRow + i);
            __m256 vtop =
                _mm256_loadu_ps(vTop + i);
            __m256 vbot =
                _mm256_loadu_ps(vBot + i);
            __m256 vleft =
                _mm256_loadu_ps(vRow + i - 1);
            __m256 vright =
                _mm256_loadu_ps(vRow + i + 1);
            __m256 ur0 =
                _mm256_loadu_ps(uRow + i);
            __m256 ur1 =
                _mm256_loadu_ps(uRow + i + 1);
            __m256 ub0 =
                _mm256_loadu_ps(uBot + i);
            __m256 ub1 =
                _mm256_loadu_ps(uBot + i + 1);
            __m256 ue =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(ur0, ur1),
                        _mm256_add_ps(ub0, ub1)),
                    _mm256_set1_ps(0.25f));
            __m256 dvdx_back =
                _mm256_mul_ps(
                    _mm256_sub_ps(vij, vleft),
                    _mm256_set1_ps(invDx));
            __m256 dvdx_forw =
                _mm256_mul_ps(
                    _mm256_sub_ps(vright, vij),
                    _mm256_set1_ps(invDx));
            __m256 dvdy_back =
                _mm256_mul_ps(
                    _mm256_sub_ps(vij, vbot),
                    _mm256_set1_ps(invDy));
            __m256 dvdy_forw =
                _mm256_mul_ps(
                    _mm256_sub_ps(vtop, vij),
                    _mm256_set1_ps(invDy));
            __m256 maskU =
                _mm256_cmp_ps(ue, zero, _CMP_GT_OS);
            __m256 dvdx =
                _mm256_blendv_ps(
                    dvdx_forw,
                    dvdx_back,
                    maskU);
            __m256 maskV =
                _mm256_cmp_ps(vij, zero, _CMP_GT_OS);
            __m256 dvdy =
                _mm256_blendv_ps(
                    dvdy_forw,
                    dvdy_back,
                    maskV);
            __m256 d2vdx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vright,
                        _mm256_sub_ps(
                            vleft,
                            _mm256_mul_ps(two, vij))),
                    _mm256_set1_ps(invDx2));
            __m256 d2vdy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vtop,
                        _mm256_sub_ps(
                            vbot,
                            _mm256_mul_ps(two, vij))),
                    _mm256_set1_ps(invDy2));
            __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(ue, dvdx),
                    _mm256_mul_ps(vij, dvdy));
            __m256 diff =
                _mm256_add_ps(
                    d2vdx2,
                    d2vdy2);
            __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        vij,
                        _mm256_mul_ps(
                            _mm256_set1_ps(dtConv),
                            conv)),
                    _mm256_mul_ps(
                        _mm256_set1_ps(dtNu),
                        diff));
            _mm256_storeu_ps(
                vStarRow + i,
                res);
        }
        for (; i < nx - 1; ++i){
            if (!vMask[i]){
                vStarRow[i] = 0.f;
                continue;
            }
            float v_ij = vRow[i];
            float u_e = quarter * (
                uRow[i+1] +
                uBot[i+1] +
                uRow[i] +
                uBot[i]);

            float v_right = vRow[i+1];
            float v_left  = vRow[i-1];
            float v_top   = vTop[i];
            float v_bot   = vBot[i];

            float dvdx =
                (u_e > 0) ?
                (v_ij - v_left) * invDx :
                (v_right - v_ij) * invDx;

            float dvdy =
                (v_ij > 0) ?
                (v_ij - v_bot) * invDy :
                (v_top - v_ij) * invDy;

            float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[i] =
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + dtNu * (d2vdx2 + d2vdy2);
        }
    }

    // Apply BC to u_star and v_star
    // Inlet (left): u_star = U0, v_star = 0
    for (int j = 0; j < ny; ++j) {
        uStar[idxU(0, j)] = cfg.U0;
        vStar[idxV(0, j)] = 0.0; // v on left face
    }
    // Outlet (right): zero gradient (neumann)
    for (int j = 0; j < ny; ++j) {
        uStar[idxU(nx, j)] = uStar[idxU(nx-1, j)];
    }
    // Top/Bottom: slip or free-slip (we use zero gradient for u, v=0)
    for (int i = 0; i <= nx; ++i) {
        uStar[idxU(i, 0)] = uStar[idxU(i, 1)];
        uStar[idxU(i, ny-1)] = uStar[idxU(i, ny-2)];
    }
    for (int i = 0; i < nx; ++i) {
        vStar[idxV(i, 0)] = 0.0;   // bottom (no vertical flow)
        vStar[idxV(i, ny)] = 0.0;  // top
    }
}

void Solver::solvePoisson() {
    const float invDx = 1.f / mesh.dx, invDy = 1.f / mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;
    float omega = cfg.omega;
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 invDtVec = _mm256_set1_ps(1.0f / dt);
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        const int rowTop = (j + 1) * nx;
        int i = 0;
        for (; i <= nx - 8; i += 8){
            bool skip = false;
            for (int k = 0; k < 8; ++k){
                if (mesh.solid[row + i + k]){
                    skip = true;
                    break;
                }
            }
            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    if (mesh.solid[row + ii]){
                        rhs[row + ii] = 0.f;
                        continue;
                    }
                    float div =
                        (u_star[row + ii + 1] - u_star[row + ii]) * invDx +
                        (v_star[rowTop + ii] - v_star[row + ii]) * invDy;
                    rhs[row + ii] = div / dt;
                }
                continue;
            }
            __m256 uR =
                _mm256_loadu_ps(u_star.data() + row + i + 1);
            __m256 uL =
                _mm256_loadu_ps(u_star.data() + row + i);
            __m256 vT =
                _mm256_loadu_ps(v_star.data() + rowTop + i);
            __m256 vB =
                _mm256_loadu_ps(v_star.data() + row + i);
            __m256 div =
                _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_sub_ps(uR, uL),
                        invDxVec),
                    _mm256_mul_ps(
                        _mm256_sub_ps(vT, vB),
                        invDyVec));
            __m256 rhsVec =
                _mm256_mul_ps(
                    div,
                    invDtVec);
            _mm256_storeu_ps(
                rhs.data() + row + i,
                rhsVec);
        }
        for (; i < nx; ++i){
            if (mesh.solid[row + i]){
                rhs[row + i] = 0.f;
                continue;
            }

            float div =
                (u_star[row + i + 1] - u_star[row + i]) * invDx +
                (v_star[rowTop + i] - v_star[row + i]) * invDy;

            rhs[row + i] = div / dt;
        }
    }
    multigrid.solve(
        p,
        rhs,
        solidMask,
        omega,
        2);
}

void Solver::corrector() {
    const float invDx = 1.f / mesh.dx, invDy = 1.f / mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 dtVec    = _mm256_set1_ps(dt);

    // Update u: u_new = u_star - dt * (p(i+1) - p(i)) / dx
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        int i = 1;
        for (; i <= nx - 8; i += 8){
            bool skip = false;
            for (int k = 0; k < 8; ++k){
                if (!uFluidMask[idxU(i + k, j)]){
                    skip = true;
                    break;
                }
            }
            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    if (!uFluidMask[idxU(ii, j)]){
                        u[idxU(ii, j)] = 0.f;
                        continue;
                    }
                    float p_right =
                        (!mesh.solid[row + ii])
                            ? p[row + ii]
                            : p[row + ii - 1];
                    float p_left =
                        (!mesh.solid[row + ii - 1])
                            ? p[row + ii - 1]
                            : p[row + ii];
                    float dpdx = (p_right - p_left) * invDx;
                    u[idxU(ii, j)] =
                        u_star[idxU(ii, j)] - dt * dpdx;
                }
                continue;
            }
            __m256 pRight =
                _mm256_loadu_ps(p.data() + row + i);
            __m256 pLeft =
                _mm256_loadu_ps(p.data() + row + i - 1);
            __m256 uStar =
                _mm256_loadu_ps(u_star.data() + idxU(i, j));
            __m256 dpdx =
                _mm256_mul_ps(
                    _mm256_sub_ps(pRight, pLeft),
                    invDxVec);
            __m256 res =
                _mm256_sub_ps(
                    uStar,
                    _mm256_mul_ps(dtVec, dpdx));
            _mm256_storeu_ps(
                u.data() + idxU(i, j),
                res);
        }
        for (; i < nx; ++i){
            if (!uFluidMask[idxU(i, j)]){
                u[idxU(i, j)] = 0.f;
                continue;
            }
            float p_right =
                (!mesh.solid[row + i]) ?
                p[row + i] :
                p[row + i - 1];

            float p_left =
                (!mesh.solid[row + i - 1]) ?
                p[row + i - 1] :
                p[row + i];

            u[idxU(i, j)] =
                u_star[idxU(i, j)]
                - dt * (p_right - p_left) * invDx;
        }
    }

    // Update v: v_new = v_star - dt * (p(j+1) - p(j)) / dy
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        int i = 0;
        for (; i <= nx - 8; i += 8){
            bool skip = false;
            for (int k = 0; k < 8; ++k){
                if (!vFluidMask[idxV(i + k, j)]){
                    skip = true;
                    break;
                }
            }
            if (skip){
                for (int k = 0; k < 8; ++k){
                    int ii = i + k;
                    if (!vFluidMask[idxV(ii, j)]){
                        v[idxV(ii, j)] = 0.f;
                        continue;
                    }
                    float p_top =
                        (!mesh.solid[row + ii])
                            ? p[row + ii]
                            : p[rowBot + ii];
                    float p_bot =
                        (!mesh.solid[rowBot + ii])
                            ? p[rowBot + ii]
                            : p[row + ii];
                    float dpdy = (p_top - p_bot) * invDy;
                    v[idxV(ii, j)] =
                        v_star[idxV(ii, j)] - dt * dpdy;
                }
                continue;
            }
            __m256 pTop =
                _mm256_loadu_ps(p.data() + row + i);
            __m256 pBot =
                _mm256_loadu_ps(p.data() + rowBot + i);
            __m256 vStar =
                _mm256_loadu_ps(v_star.data() + idxV(i, j));
            __m256 dpdy =
                _mm256_mul_ps(
                    _mm256_sub_ps(pTop, pBot),
                    invDyVec);
            __m256 res =
                _mm256_sub_ps(
                    vStar,
                    _mm256_mul_ps(dtVec, dpdy));
            _mm256_storeu_ps(
                v.data() + idxV(i, j),
                res);
        }
        for (; i < nx; ++i){
            if (!vFluidMask[idxV(i, j)]){
                v[idxV(i, j)] = 0.f;
                continue;
            }
            float p_top =
                (!mesh.solid[row + i]) ?
                p[row + i] :
                p[rowBot + i];

            float p_bot =
                (!mesh.solid[rowBot + i]) ?
                p[rowBot + i] :
                p[row + i];

            v[idxV(i, j)] =
                v_star[idxV(i, j)]
                - dt * (p_top - p_bot) * invDy;
        }
    }
    // Apply boundary conditions again
    applyBC();
}

void Solver::applyBC() {
    int nx = cfg.nx, ny = cfg.ny;
    // Inlet (left): u = U0, v = 0
    for (int j = 0; j < ny; ++j) {
        u[idxU(0, j)] = cfg.U0;
        v[idxV(0, j)] = 0.0;
    }
    // Outlet: zero gradient
    for (int j = 0; j < ny; ++j) {
        u[idxU(nx, j)] = u[idxU(nx-1, j)];
    }
    // Top/Bottom: free slip (u gradient zero, v=0)
    for (int i = 0; i <= nx; ++i) {
        u[idxU(i, 0)] = u[idxU(i, 1)];
        u[idxU(i, ny-1)] = u[idxU(i, ny-2)];
    }
    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0;
        v[idxV(i, ny)] = 0.0;
    }
}

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    currentTime = 0.0;
    step = 0;

    // Save initial state
    saveVTK(step);

    while (currentTime < cfg.totalTime) {
        if (step % dtUpdateInterval == 0)
            computeDt();
        if (currentTime + dt > cfg.totalTime) dt = cfg.totalTime - currentTime; // avoid overshoot

        predictor();
        solvePoisson();
        corrector();

        currentTime += dt;
        step++;

        // Progress output
        if (step % 10 == 0) {
            std::cout << "Step " << step << ", time = " << currentTime << " s, dt = " << dt << std::endl;
        }

        // Save VTK periodically
        if (step % SAVE_INTERVAL == 0) {
            saveVTK(step);
        }
    }
    // Final save
    saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s.\n";
    // Not sure about this, if it works its so cool
}

void Solver::saveVTK(int stepNum) const {
    int nx = cfg.nx, ny = cfg.ny;
    const float dx = mesh.dx, dy = mesh.dy;
    constexpr size_t BUFFER_FLOATS = 4096;
    std::array<uint32_t, BUFFER_FLOATS> buffer;
    size_t bufferPos = 0;

    std::string filename = "solution_" + std::to_string(stepNum) + ".vtk";
    std::ofstream fout(filename, std::ios::binary);
    if (!fout){
        std::cerr << "Cannot open " << filename << '\n';
        return;
    }
    fout
        << "# vtk DataFile Version 3.0\n"
        << "CFD Solver\n"
        << "BINARY\n"
        << "DATASET STRUCTURED_POINTS\n"
        << "DIMENSIONS "
        << nx + 1 << " "
        << ny + 1 << " 1\n"
        << "ORIGIN 0 0 0\n"
        << "SPACING "
        << dx << " "
        << dy << " 0\n"
        << "CELL_DATA "
        << nx * ny << "\n";
    auto flushFloatBuffer = [&](){
        fout.write(
            reinterpret_cast<char*>(buffer.data()),
            bufferPos * sizeof(uint32_t));
        bufferPos = 0;
    };
    auto writeFloat = [&](float value){
        uint32_t x;
        std::memcpy(&x, &value, sizeof(float));
        x =
            ((x & 0x000000FFu) << 24) |
            ((x & 0x0000FF00u) << 8 ) |
            ((x & 0x00FF0000u) >> 8 ) |
            ((x & 0xFF000000u) >> 24);
        buffer[bufferPos++] = x;
        if (bufferPos == BUFFER_FLOATS)
            flushFloatBuffer();
    };
    auto writeInt = [&](int value){
        int32_t x = value;
        uint32_t y;
        std::memcpy(&y, &x, sizeof(int32_t));
        y =
            ((y & 0x000000FFu) << 24) |
            ((y & 0x0000FF00u) << 8 ) |
            ((y & 0x00FF0000u) >> 8 ) |
            ((y & 0xFF000000u) >> 24);
        fout.write(reinterpret_cast<char*>(&y), sizeof(y));
    };
    fout << "SCALARS pressure float\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i){
            writeFloat(p[row + i] * cfg.ro);
        }
    }
    flushFloatBuffer();
    fout << "\n";

    fout << "SCALARS solid int 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i)
        {
            writeInt(mesh.solid[row + i]);
        }
    }
    fout << "\n";
    fout << "VECTORS velocity float\n";
    for (int j = 0; j < ny; ++j){
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i){
            float uu =
                0.5f * (u[rowU + i] + u[rowU + i + 1]);
            float vv =
                0.5f * (v[rowV + i] + v[rowVTop + i]);
            writeFloat(uu);
            writeFloat(vv);
            writeFloat(0.0f);
        }
    }
    flushFloatBuffer();
    fout.close();
    std::cout << "Saved " << filename << std::endl;
}
