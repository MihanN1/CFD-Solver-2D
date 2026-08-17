#include "Solver.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include <array>
#include <limits>
#include <sstream>
#include <utility>

namespace {
#ifdef __AVX2__
// Largest of the 8 lanes
float horizontalMax(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_max_ps(lo, hi);
    lo = _mm_max_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_max_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

// Sum of the 8 lanes
float horizontalSum(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

// Clears the sign bit, i.e. fabs for a whole vector
__m256 absMask() {
    return _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
}
#endif
}

Solver::Solver(const Config& cfg, const Mesh& mesh)
    :
    cfg(cfg),
    mesh(mesh),
    multigrid(cfg.nx, cfg.ny, mesh.dx, mesh.dy, cfg.mgMinCoarseSize)
{
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    // Allocate arrays
    p.assign(static_cast<size_t>(nx) * ny, 0.0f);
    rhs.assign(static_cast<size_t>(nx) * ny, 0.0f);
    u.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    v.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    u_star.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    v_star.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);

    dx = mesh.dx;
    dy = mesh.dy;
    invDx = 1.0f / dx;
    invDy = 1.0f / dy;
    invDx2 = invDx * invDx;
    invDy2 = invDy * invDy;
    if (cfg.gravityEnabled) {
        constexpr float degToRad = 3.14159265358979f / 180.0f;
        const float rad = cfg.gravityAngle * degToRad;
        gx = -cfg.gravityAccel * std::sin(rad) + 0.f;
        gy = -cfg.gravityAccel * std::cos(rad) + 0.f;
    }

    outputPath =
        cfg.outputDir.empty() ?
        std::filesystem::path(".") :
        narrowToPath(cfg.outputDir);

    configHeader = "formatVersion=1\n" + cfg.serialize();
}

bool Solver::setInitialState(RestartData&& state,
                             const std::string& prefix) {
    if (state.u.size() != u.size() ||
        state.v.size() != v.size() ||
        state.p.size() != p.size())
        return false;

    u = std::move(state.u);
    v = std::move(state.v);
    p = std::move(state.p);

    // Frames carry the total pressure, the solver works with the reduced one,
    // so the hydrostatic field of the run that wrote the frame comes back off
    // here. Gravity may differ from that run, or be gone; either way what is
    // subtracted is the head the frame was written with.
    if (state.cfg.gravityEnabled) {
        constexpr float degToRad = 3.14159265358979f / 180.0f;
        const float rad = state.cfg.gravityAngle * degToRad;
        const float oldGx = -state.cfg.gravityAccel * std::sin(rad) + 0.f;
        const float oldGy = -state.cfg.gravityAccel * std::cos(rad) + 0.f;
        for (int j = 0; j < cfg.ny; ++j)
            for (int i = 0; i < cfg.nx; ++i)
                p[idxP(i, j)] -=
                    oldGx * ((i + 0.5f - cfg.nx) * dx) +
                    oldGy * ((j + 0.5f - 0.5f * cfg.ny) * dy);
    }

    currentTime = state.currentTime;
    step = state.step;
    restartDt = state.dt;
    needsProjection = !state.exactState;
    hasRestartState = true;
    if (!prefix.empty())
        framePrefix = prefix;

    return true;
}

void Solver::buildFaceMasks(){
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    uFluidMask.assign(static_cast<size_t>(nx + 1) * ny, 0);
    vFluidMask.assign(static_cast<size_t>(nx) * (ny + 1), 0);
    uFluidMaskF.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    vFluidMaskF.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    uWall.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    vWall.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);

    // u faces
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;
        const float yFace = (j + 0.5f) * dy;
        for (int i = 1; i < nx; ++i){
            const bool solidLeft = solidMask[row + i - 1] != 0;
            const bool solidRight = solidMask[row + i] != 0;
            const bool fluid = !solidLeft && !solidRight;
            uFluidMask[idxU(i, j)] = fluid ? 1 : 0;
            uFluidMaskF[idxU(i, j)] = fluid ? 1.0f : 0.0f;

            // Solid on one side only means this face is normal to a wall and
            // the body does not move, so it stays shut. Solid on both sides
            // means the face lies inside the body, where its only job is to
            // be the tangential value the fluid above and below shears against.
            if (wallsMove && solidLeft && solidRight) {
                const WallField& wall = wallField[mesh.objectId[row + i]];
                uWall[idxU(i, j)] =
                    wall.slideX - wall.omega * (yFace - wall.cy);
            }
        }
    }

    // v faces
    for (int j = 1; j < ny; ++j){
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i){
            const bool solidTop = solidMask[row + i] != 0;
            const bool solidBot = solidMask[rowBot + i] != 0;
            const bool fluid = !solidTop && !solidBot;
            vFluidMask[idxV(i, j)] = fluid ? 1 : 0;
            vFluidMaskF[idxV(i, j)] = fluid ? 1.0f : 0.0f;

            if (wallsMove && solidTop && solidBot) {
                const WallField& wall = wallField[mesh.objectId[row + i]];
                vWall[idxV(i, j)] =
                    wall.slideY + wall.omega * ((i + 0.5f) * dx - wall.cx);
            }
        }
    }
}

void Solver::buildSlipFaces() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    uSlipFaces.clear();
    vSlipFaces.clear();
    if (!wallsSlip)
        return;

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 1; i < nx; ++i) {
            if (!solidMask[row + i] || !solidMask[row + i - 1])
                continue;
            if (!wallField[mesh.objectId[row + i]].slip)
                continue;

            const int below =
                (j > 0 && uFluidMask[idxU(i, j - 1)]) ? idxU(i, j - 1) : -1;
            const int above =
                (j + 1 < ny && uFluidMask[idxU(i, j + 1)]) ? idxU(i, j + 1) : -1;
            if (below < 0 && above < 0)
                continue;

            SlipFace mirror;
            mirror.face = idxU(i, j);
            mirror.first = (below >= 0) ? below : above;
            mirror.second = (above >= 0) ? above : below;
            uSlipFaces.push_back(mirror);
        }
    }

    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            if (!solidMask[row + i] || !solidMask[rowBot + i])
                continue;
            if (!wallField[mesh.objectId[row + i]].slip)
                continue;

            const int left =
                (i > 0 && vFluidMask[idxV(i - 1, j)]) ? idxV(i - 1, j) : -1;
            const int right =
                (i + 1 < nx && vFluidMask[idxV(i + 1, j)]) ? idxV(i + 1, j) : -1;
            if (left < 0 && right < 0)
                continue;

            SlipFace mirror;
            mirror.face = idxV(i, j);
            mirror.first = (left >= 0) ? left : right;
            mirror.second = (right >= 0) ? right : left;
            vSlipFaces.push_back(mirror);
        }
    }
}

void Solver::applySlipFaces() {
    for (const SlipFace& mirror : uSlipFaces)
        u[mirror.face] = 0.5f * (u[mirror.first] + u[mirror.second]);
    for (const SlipFace& mirror : vSlipFaces)
        v[mirror.face] = 0.5f * (v[mirror.first] + v[mirror.second]);
}

void Solver::resolveWallMotion() {
    wallField.assign(mesh.objects.size() + 1, WallField());
    wallsMove = false;
    wallsSlip = false;

    for (size_t id = 1; id < wallField.size(); ++id) {
        wallField[id].cx = static_cast<float>(mesh.objects[id - 1].cx);
        wallField[id].cy = static_cast<float>(mesh.objects[id - 1].cy);
    }

    if (cfg.wallMotion.empty())
        return;

    std::vector<WallMotion> motions;
    std::string error;
    if (!parseWallMotion(cfg.wallMotion, motions, error)) {
        std::cout << "\n!!! " << error << "\n    No wall moves.\n";
        return;
    }

    constexpr float degToRad = 3.14159265358979f / 180.0f;
    for (const WallMotion& motion : motions) {
        if (static_cast<size_t>(motion.object) >= wallField.size()) {
            std::cout << "\n!!! wallMotion moves object " << motion.object
                      << ", but this geometry has " << mesh.objects.size()
                      << (mesh.objects.size() == 1 ? " object" : " objects")
                      << ". That part of the line does nothing.\n";
            continue;
        }
        WallField& wall = wallField[motion.object];
        wall.omega = motion.rotation * degToRad;
        wall.slideX = motion.slideX;
        wall.slideY = motion.slideY;
        wall.slip = motion.slip;
        if (wall.omega != 0.0f || wall.slideX != 0.0f || wall.slideY != 0.0f)
            wallsMove = true;
        if (wall.slip)
            wallsSlip = true;
    }

    if (!wallsMove && !wallsSlip)
        return;

    const float inlet = std::fabs(cfg.U0);
    std::cout << "Walls:\n";
    for (size_t id = 1; id < wallField.size(); ++id) {
        const WallField& wall = wallField[id];
        if (wall.slip) {
            std::cout << "  object " << id
                      << ": free-slip, the fluid slides along it and it "
                         "exerts no drag\n";
            continue;
        }
        if (wall.omega == 0.0f && wall.slideX == 0.0f && wall.slideY == 0.0f)
            continue;

        const float rim =
            std::fabs(wall.omega) *
            static_cast<float>(mesh.objects[id - 1].radius);
        const float surface = rim + std::hypot(wall.slideX, wall.slideY);

        std::cout << "  object " << id << " at (" << wall.cx << ", " << wall.cy
                  << ") m: rot = " << wall.omega / degToRad << " deg/s"
                  << " -> rim speed " << rim << " m/s, slide = ("
                  << wall.slideX << ", " << wall.slideY << ") m/s\n";
        if (inlet > 0.0f)
            std::cout << "    surface moves at " << surface / inlet
                      << "x the inlet velocity\n";
        if (inlet > 0.0f && surface > 5.0f * inlet)
            std::cout << "    note: the wall is now the fastest thing in the "
                         "domain, so it sets dt through the\n"
                         "    CFL condition and the run gets slower in "
                         "proportion.\n";
    }
}

void Solver::initFields()
{
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    solidMask.assign(static_cast<size_t>(nx) * ny, 0);
    fluidCellMaskF.assign(static_cast<size_t>(nx) * ny, 0.0f);
    for (int id = 0; id < nx * ny; ++id) {
        solidMask[id] = mesh.solid[id] ? 1 : 0;
        fluidCellMaskF[id] = mesh.solid[id] ? 0.0f : 1.0f;
    }

    resolveWallMotion();
    buildFaceMasks();
    buildSlipFaces();
    multigrid.setUseCuda(cfg.useCuda);
    multigrid.setGeometry(solidMask);

    if (hasRestartState && !needsProjection)
        multigrid.skipInitialFullMultigrid();

    std::fill(rhs.begin(), rhs.end(), 0.0f);
    std::fill(u_star.begin(), u_star.end(), 0.0f);
    std::fill(v_star.begin(), v_star.end(), 0.0f);

    if (!hasRestartState) {
        std::fill(p.begin(), p.end(), 0.0f);
        std::fill(u.begin(), u.end(), 0.0f);
        std::fill(v.begin(), v.end(), 0.0f);

        for (int j = 0; j < ny; ++j) {
            for (int i = 1; i < nx; ++i)
                if (uFluidMask[idxU(i, j)])
                    u[idxU(i, j)] = cfg.U0;

            if (!solidMask[idxP(0, j)])
                u[idxU(0, j)] = cfg.U0;
            if (!solidMask[idxP(nx - 1, j)])
                u[idxU(nx, j)] = cfg.U0;
        }
    }

    // Runs in both cases: on a restart this is what applies a changed U0
    applyBC();

    // The closed faces of a frame carry the motion of the run that wrote it,
    // which is not necessarily this one, and a fresh run has zeros there. Both
    // are stamped over before anything reads them, so step 0 already shows the
    // walls moving and the first dt already accounts for them.
    if (wallsMove) {
        for (int j = 0; j < ny; ++j)
            for (int i = 1; i < nx; ++i) {
                const int id = idxU(i, j);
                u[id] = uFluidMaskF[id] * u[id] + uWall[id];
            }
        for (int j = 1; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int id = idxV(i, j);
                v[id] = vFluidMaskF[id] * v[id] + vWall[id];
            }
    }
    applySlipFaces();

    std::cout << "Fields initialized. Multigrid levels: "
              << multigrid.levelCount()
              << ", backend: " << (multigrid.usingCuda() ? "CUDA" : "CPU")
              << "\n";
    if (cfg.U0 > 0.0f && cfg.nu > 0.0f) {
        const float nuNum = 0.5f * cfg.U0 * dx;   // upwind
        const float Lref  = 0.2f * std::min(cfg.Lx, cfg.Ly);
        const float ReSet = cfg.U0 * Lref / cfg.nu;
        const float ReEff = cfg.U0 * Lref / (cfg.nu + nuNum);

        std::cout << "Fluid: rho = " << cfg.ro << " kg/m^3, nu = " << cfg.nu
                  << " m^2/s\n  Re(set) = " << ReSet
                  << ", nu_numerical = " << nuNum
                  << ", Re(effective) = " << ReEff << "\n";

        if (nuNum > cfg.nu)
            std::cout << "  note: upwind adds " << nuNum / cfg.nu
                      << "x more viscosity than the fluid has. The run behaves "
                         "like Re " << ReEff << ", not " << ReSet
                      << ".\n         dx < 2*nu/U0 = " << 2.f * cfg.nu / cfg.U0
                      << " m would fix it, i.e. nx > "
                      << cfg.Lx * cfg.U0 / (2.f * cfg.nu) << ".\n";

        if (ReEff > 3000.f)
            std::cout << "  note: Re(effective) is past the laminar range and "
                         "there is no turbulence model here.\n";
    }
    if (cfg.gravityEnabled) {
        const float head = std::fabs(gx) * cfg.Lx + std::fabs(gy) * cfg.Ly;
        std::cout << "Gravity: " << cfg.gravityAccel << " m/s^2 at "
                  << cfg.gravityAngle << " deg -> g = (" << gx << ", " << gy
                  << ") m/s^2.\n"
                  << "  At constant density this cannot change the velocity "
                     "field; it is absorbed\n"
                     "  into the pressure as a hydrostatic head of "
                  << head * cfg.ro << " Pa across the domain.\n";

        const float dynamic = cfg.U0 * cfg.U0;
        if (dynamic > 0.0f && head > 5.0f * dynamic) {
            std::cout << "  note: that head is " << (head / dynamic)
                      << "x the dynamic scale U0^2, so it dominates the "
                         "pressure map in\n"
                         "  ParaView. It is added on output only and never "
                         "enters the solve, so it\n"
                         "  costs no accuracy; rescale the colour map to see "
                         "the dynamic part.\n";
        }
    }

    if (multigrid.levelCount() < 3 && (nx > 32 || ny > 32)) {
        std::cout << "  note: few multigrid levels for " << nx << "x" << ny
                  << ". Cell counts divisible by a high power of two "
                     "(e.g. 256x128) give the deepest hierarchy and the "
                     "fastest pressure solve.\n";
    }
}

void Solver::computeDt(){
    const int nx = cfg.nx;
    const int ny = cfg.ny;

#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 signMask = absMask();
#endif

    float maxCourant = 0.0f;

    #pragma omp parallel
    {
#ifdef __AVX2__
        __m256 localVec = _mm256_setzero_ps();
#endif
        float localScalar = 0.0f;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j)
        {
            const int rowU = j * (nx + 1);
            const int rowV = j * nx;
            const int rowVTop = (j + 1) * nx;

            int i = 0;
#ifdef __AVX2__
            for (; i + 8 <= nx; i += 8)
            {
                const __m256 uL = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(u.data() + rowU + i));
                const __m256 uR = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(u.data() + rowU + i + 1));
                const __m256 vB = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(v.data() + rowV + i));
                const __m256 vT = _mm256_and_ps(signMask,
                    _mm256_loadu_ps(v.data() + rowVTop + i));

                const __m256 courant =
                    _mm256_add_ps(
                        _mm256_mul_ps(_mm256_max_ps(uL, uR), invDxVec),
                        _mm256_mul_ps(_mm256_max_ps(vB, vT), invDyVec));

                localVec = _mm256_max_ps(localVec, courant);
            }
#endif

            for (; i < nx; ++i)
            {
                const float maxU = std::max(std::fabs(u[rowU + i]),
                                            std::fabs(u[rowU + i + 1]));
                const float maxV = std::max(std::fabs(v[rowV + i]),
                                            std::fabs(v[rowVTop + i]));
                localScalar = std::max(localScalar,
                                       maxU * invDx + maxV * invDy);
            }
        }

#ifdef __AVX2__
        const float localMax = std::max(horizontalMax(localVec), localScalar);
#else
        const float localMax = localScalar;
#endif
        #pragma omp critical
        {
            maxCourant = std::max(maxCourant, localMax);
        }
    }

    const float dtAdv =
        (maxCourant < 1e-12f) ?
        1e9f :
        cfg.CFL / maxCourant;

    const float dtDiff =
        (cfg.nu > 0.0f) ?
        1.f / (2.f * cfg.nu * (invDx2 + invDy2)) :
        1e9f;

    dt = cfg.dtSafety * std::min(dtAdv, dtDiff);

    if (!(dt > 0.f) || std::isnan(dt) || std::isinf(dt))
        dt = 1e-6f;
}

void Solver::predictor() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float dtNu = dt * cfg.nu;
    const float dtConv = dt;
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float* __restrict uStar = u_star.data();
    float* __restrict vStar = v_star.data();

#ifdef __AVX2__
    const __m256 zero    = _mm256_setzero_ps();
    const __m256 two     = _mm256_set1_ps(2.f);
    const __m256 quarter = _mm256_set1_ps(0.25f);
    const __m256 invDxVec  = _mm256_set1_ps(invDx);
    const __m256 invDyVec  = _mm256_set1_ps(invDy);
    const __m256 invDx2Vec = _mm256_set1_ps(invDx2);
    const __m256 invDy2Vec = _mm256_set1_ps(invDy2);
    const __m256 dtConvVec = _mm256_set1_ps(dtConv);
    const __m256 dtNuVec   = _mm256_set1_ps(dtNu);
#endif

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny - 1; ++j) {
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uTop = uPtr + (j + 1) * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict uMask = uFluidMaskF.data() + rowU;
        const float* __restrict uWallRow = uWall.data() + rowU;
        float* __restrict uStarRow = uStar + rowU;
        int i = 1;
#ifdef __AVX2__
        for (; i + 8 <= nx; i += 8) {
            const __m256 uij =
                _mm256_loadu_ps(uRow + i);
            const __m256 utop =
                _mm256_loadu_ps(uTop + i);
            const __m256 ubot =
                _mm256_loadu_ps(uBot + i);
            const __m256 uleft =
                _mm256_loadu_ps(uRow + i - 1);
            const __m256 uright =
                _mm256_loadu_ps(uRow + i + 1);
            // Convection: upwind for u
            const __m256 vn =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(
                            _mm256_loadu_ps(vTop + i - 1),
                            _mm256_loadu_ps(vTop + i)),
                        _mm256_add_ps(
                            _mm256_loadu_ps(vRow + i - 1),
                            _mm256_loadu_ps(vRow + i))),
                    quarter);
            const __m256 dudx =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(uright, uij), invDxVec),
                    _mm256_mul_ps(_mm256_sub_ps(uij, uleft), invDxVec),
                    _mm256_cmp_ps(uij, zero, _CMP_GT_OS));
            const __m256 dudy =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(utop, uij), invDyVec),
                    _mm256_mul_ps(_mm256_sub_ps(uij, ubot), invDyVec),
                    _mm256_cmp_ps(vn, zero, _CMP_GT_OS));
            // Diffusion: central differences
            const __m256 d2udx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        uright,
                        _mm256_sub_ps(
                            uleft,
                            _mm256_mul_ps(two, uij))),
                    invDx2Vec);
            const __m256 d2udy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        utop,
                        _mm256_sub_ps(
                            ubot,
                            _mm256_mul_ps(two, uij))),
                    invDy2Vec);
            const __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(uij, dudx),
                    _mm256_mul_ps(vn, dudy));
            const __m256 diff =
                _mm256_add_ps(
                    d2udx2,
                    d2udy2);
            const __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        uij,
                        _mm256_mul_ps(dtConvVec, conv)),
                    _mm256_mul_ps(dtNuVec, diff));
            _mm256_storeu_ps(
                uStarRow + i,
                _mm256_add_ps(
                    _mm256_mul_ps(res, _mm256_loadu_ps(uMask + i)),
                    _mm256_loadu_ps(uWallRow + i)));
        }
#endif
        for (; i < nx; ++i) {
            const float u_ij = uRow[i];
            const float v_n = 0.25f * (
                vTop[i-1] +
                vTop[i] +
                vRow[i-1] +
                vRow[i]);

            const float u_left  = uRow[i-1];
            const float u_right = uRow[i+1];
            const float u_bot   = uBot[i];
            const float u_top   = uTop[i];

            const float dudx =
                (u_ij > 0.f) ?
                (u_ij - u_left) * invDx :
                (u_right - u_ij) * invDx;

            const float dudy =
                (v_n > 0.f) ?
                (u_ij - u_bot) * invDy :
                (u_top - u_ij) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + dtNu * (d2udx2 + d2udy2))
                + uWallRow[i];
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        if (ny < 2)
            break;
        const int j = (pass == 0) ? 0 : ny - 1;
        const int rowU = j * (nx + 1);
        const float* __restrict uRow = uPtr + rowU;
        const float* __restrict uOther =
            uPtr + ((pass == 0) ? (nx + 1) : (j - 1) * (nx + 1));
        const float* __restrict vRow = vPtr + j * nx;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict uMask = uFluidMaskF.data() + rowU;
        const float* __restrict uWallRow = uWall.data() + rowU;
        float* __restrict uStarRow = uStar + rowU;
        for (int i = 1; i < nx; ++i) {
            const float u_ij = uRow[i];
            const float v_n = 0.25f * (
                vTop[i-1] +
                vTop[i] +
                vRow[i-1] +
                vRow[i]);

            const float u_left  = uRow[i-1];
            const float u_right = uRow[i+1];
            const float u_bot = (pass == 0) ? u_ij : uOther[i];
            const float u_top = (pass == 0) ? uOther[i] : u_ij;

            const float dudx =
                (u_ij > 0.f) ?
                (u_ij - u_left) * invDx :
                (u_right - u_ij) * invDx;

            const float dudy =
                (v_n > 0.f) ?
                (u_ij - u_bot) * invDy :
                (u_top - u_ij) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + dtNu * (d2udx2 + d2udy2))
                + uWallRow[i];
        }
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j){ // internal horizontal faces
        const int rowV = j * nx;
        const float* __restrict vRow = vPtr + rowV;
        const float* __restrict vTop = vPtr + (j + 1) * nx;
        const float* __restrict vBot = vPtr + (j - 1) * nx;
        const float* __restrict uRow = uPtr + j * (nx + 1);
        const float* __restrict uBot = uPtr + (j - 1) * (nx + 1);
        const float* __restrict vMask = vFluidMaskF.data() + rowV;
        const float* __restrict vWallRow = vWall.data() + rowV;
        float* __restrict vStarRow = vStar + rowV;
        int i = 1;
#ifdef __AVX2__
        for (; i + 8 <= nx - 1; i += 8) {
            const __m256 vij =
                _mm256_loadu_ps(vRow + i);
            const __m256 vtop =
                _mm256_loadu_ps(vTop + i);
            const __m256 vbot =
                _mm256_loadu_ps(vBot + i);
            const __m256 vleft =
                _mm256_loadu_ps(vRow + i - 1);
            const __m256 vright =
                _mm256_loadu_ps(vRow + i + 1);
            const __m256 ue =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        _mm256_add_ps(
                            _mm256_loadu_ps(uRow + i),
                            _mm256_loadu_ps(uRow + i + 1)),
                        _mm256_add_ps(
                            _mm256_loadu_ps(uBot + i),
                            _mm256_loadu_ps(uBot + i + 1))),
                    quarter);
            const __m256 dvdx =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vright, vij), invDxVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vleft), invDxVec),
                    _mm256_cmp_ps(ue, zero, _CMP_GT_OS));
            const __m256 dvdy =
                _mm256_blendv_ps(
                    _mm256_mul_ps(_mm256_sub_ps(vtop, vij), invDyVec),
                    _mm256_mul_ps(_mm256_sub_ps(vij, vbot), invDyVec),
                    _mm256_cmp_ps(vij, zero, _CMP_GT_OS));
            const __m256 d2vdx2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vright,
                        _mm256_sub_ps(
                            vleft,
                            _mm256_mul_ps(two, vij))),
                    invDx2Vec);
            const __m256 d2vdy2 =
                _mm256_mul_ps(
                    _mm256_add_ps(
                        vtop,
                        _mm256_sub_ps(
                            vbot,
                            _mm256_mul_ps(two, vij))),
                    invDy2Vec);
            const __m256 conv =
                _mm256_add_ps(
                    _mm256_mul_ps(ue, dvdx),
                    _mm256_mul_ps(vij, dvdy));
            const __m256 diff =
                _mm256_add_ps(
                    d2vdx2,
                    d2vdy2);
            const __m256 res =
                _mm256_add_ps(
                    _mm256_sub_ps(
                        vij,
                        _mm256_mul_ps(dtConvVec, conv)),
                    _mm256_mul_ps(dtNuVec, diff));
            _mm256_storeu_ps(
                vStarRow + i,
                _mm256_add_ps(
                    _mm256_mul_ps(res, _mm256_loadu_ps(vMask + i)),
                    _mm256_loadu_ps(vWallRow + i)));
        }
#endif
        for (; i < nx - 1; ++i) {
            const float v_ij = vRow[i];
            const float u_e = 0.25f * (
                uRow[i] +
                uRow[i+1] +
                uBot[i] +
                uBot[i+1]);

            const float v_left  = vRow[i-1];
            const float v_right = vRow[i+1];
            const float v_bot   = vBot[i];
            const float v_top   = vTop[i];

            const float dvdx =
                (u_e > 0.f) ?
                (v_ij - v_left) * invDx :
                (v_right - v_ij) * invDx;

            const float dvdy =
                (v_ij > 0.f) ?
                (v_ij - v_bot) * invDy :
                (v_top - v_ij) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[i] = vMask[i] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + dtNu * (d2vdx2 + d2vdy2))
                + vWallRow[i];
        }

        for (int pass = 0; pass < 2; ++pass) {
            const int iCol = (pass == 0) ? 0 : nx - 1;
            if (iCol < 0 || (pass == 1 && nx < 2))
                continue;
            const float v_ij = vRow[iCol];
            const float u_e = 0.25f * (
                uRow[iCol] +
                uRow[iCol+1] +
                uBot[iCol] +
                uBot[iCol+1]);

            const float v_left  = (iCol == 0)      ? v_ij : vRow[iCol-1];
            const float v_right = (iCol == nx - 1) ? v_ij : vRow[iCol+1];
            const float v_bot   = vBot[iCol];
            const float v_top   = vTop[iCol];

            const float dvdx =
                (u_e > 0.f) ?
                (v_ij - v_left) * invDx :
                (v_right - v_ij) * invDx;

            const float dvdy =
                (v_ij > 0.f) ?
                (v_ij - v_bot) * invDy :
                (v_top - v_ij) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[iCol] = vMask[iCol] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + dtNu * (d2vdx2 + d2vdy2))
                + vWallRow[iCol];
        }
    }

    for (int j = 0; j < ny; ++j) {
        uStar[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        uStar[idxU(nx, j)] =
            solidMask[idxP(nx - 1, j)] ?
            0.0f :
            uStar[idxU(nx - 1, j)];
    }
    for (int i = 0; i < nx; ++i) {
        vStar[idxV(i, 0)] = 0.0f;
        vStar[idxV(i, ny)] = 0.0f;
    }
}

void Solver::solvePoisson() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float invDt = 1.f / dt;
#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 invDtVec = _mm256_set1_ps(invDt);
#endif
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    const float* __restrict cellMask = fluidCellMaskF.data();
    float* __restrict rhsPtr = rhs.data();

    double rhsSqSum = 0.0;

    #pragma omp parallel for schedule(static) reduction(+ : rhsSqSum)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        float rowSum = 0.0f;
        int i = 0;
#ifdef __AVX2__
        __m256 sqAcc = _mm256_setzero_ps();
        for (; i + 8 <= nx; i += 8){
            const __m256 uR =
                _mm256_loadu_ps(uStar + rowU + i + 1);
            const __m256 uL =
                _mm256_loadu_ps(uStar + rowU + i);
            const __m256 vT =
                _mm256_loadu_ps(vStar + rowVTop + i);
            const __m256 vB =
                _mm256_loadu_ps(vStar + rowV + i);
            const __m256 div =
                _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_sub_ps(uR, uL),
                        invDxVec),
                    _mm256_mul_ps(
                        _mm256_sub_ps(vT, vB),
                        invDyVec));
            // A solid cell is not solved for, and the walls of a moving body
            // put a divergence inside it that would otherwise be counted in
            // ||rhs|| and loosen the tolerance the whole grid is judged by.
            const __m256 val =
                _mm256_mul_ps(
                    _mm256_mul_ps(div, invDtVec),
                    _mm256_loadu_ps(cellMask + rowP + i));
            _mm256_storeu_ps(rhsPtr + rowP + i, val);
            sqAcc = _mm256_add_ps(sqAcc, _mm256_mul_ps(val, val));
        }
        rowSum = horizontalSum(sqAcc);
#endif
        for (; i < nx; ++i){
            const float div =
                (uStar[rowU + i + 1] - uStar[rowU + i]) * invDx +
                (vStar[rowVTop + i] - vStar[rowV + i]) * invDy;

            const float val = div * invDt * cellMask[rowP + i];
            rhsPtr[rowP + i] = val;
            rowSum += val * val;
        }
        rhsSqSum += double(rowSum);
    }

    const float rhsNorm = float(std::sqrt(rhsSqSum));

    const int cycles = std::max(1, cfg.mgIterations);
    lastResidual = multigrid.solve(
        p,
        rhs,
        cfg.smootherOmega,
        cfg.omega,
        cycles,
        cfg.mgTolerance,
        rhsNorm);
}

void Solver::corrector() {
    const int nx = cfg.nx, ny = cfg.ny;
#ifdef __AVX2__
    const __m256 invDxVec = _mm256_set1_ps(invDx);
    const __m256 invDyVec = _mm256_set1_ps(invDy);
    const __m256 dtVec    = _mm256_set1_ps(dt);
#endif
    const float* __restrict pPtr = p.data();
    const float* __restrict uStar = u_star.data();
    const float* __restrict vStar = v_star.data();
    float* __restrict uPtr = u.data();
    float* __restrict vPtr = v.data();

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const float* __restrict uMask = uFluidMaskF.data() + rowU;
        const float* __restrict uWallRow = uWall.data() + rowU;
        int i = 1;
#ifdef __AVX2__
        for (; i + 8 <= nx; i += 8){
            const __m256 pRight =
                _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pLeft =
                _mm256_loadu_ps(pPtr + rowP + i - 1);
            const __m256 uS =
                _mm256_loadu_ps(uStar + rowU + i);
            const __m256 dpdx =
                _mm256_mul_ps(
                    _mm256_sub_ps(pRight, pLeft),
                    invDxVec);
            const __m256 res =
                _mm256_sub_ps(
                    uS,
                    _mm256_mul_ps(dtVec, dpdx));
            _mm256_storeu_ps(
                uPtr + rowU + i,
                _mm256_add_ps(
                    _mm256_mul_ps(res, _mm256_loadu_ps(uMask + i)),
                    _mm256_loadu_ps(uWallRow + i)));
        }
#endif
        for (; i < nx; ++i){
            const float res =
                uStar[rowU + i]
                - dt * (pPtr[rowP + i] - pPtr[rowP + i - 1]) * invDx;

            uPtr[rowU + i] = uMask[i] * res + uWallRow[i];
        }
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowPBot = (j - 1) * nx;
        const int rowV = j * nx;
        const float* __restrict vMask = vFluidMaskF.data() + rowV;
        const float* __restrict vWallRow = vWall.data() + rowV;
        int i = 0;
#ifdef __AVX2__
        for (; i + 8 <= nx; i += 8){
            const __m256 pTop =
                _mm256_loadu_ps(pPtr + rowP + i);
            const __m256 pBot =
                _mm256_loadu_ps(pPtr + rowPBot + i);
            const __m256 vS =
                _mm256_loadu_ps(vStar + rowV + i);
            const __m256 dpdy =
                _mm256_mul_ps(
                    _mm256_sub_ps(pTop, pBot),
                    invDyVec);
            const __m256 res =
                _mm256_sub_ps(
                    vS,
                    _mm256_mul_ps(dtVec, dpdy));
            _mm256_storeu_ps(
                vPtr + rowV + i,
                _mm256_add_ps(
                    _mm256_mul_ps(res, _mm256_loadu_ps(vMask + i)),
                    _mm256_loadu_ps(vWallRow + i)));
        }
#endif
        for (; i < nx; ++i){
            const float res =
                vStar[rowV + i]
                - dt * (pPtr[rowP + i] - pPtr[rowPBot + i]) * invDy;

            vPtr[rowV + i] = vMask[i] * res + vWallRow[i];
        }
    }

    const float outletFactor = 2.f * dt * invDx;
    for (int j = 0; j < ny; ++j) {
        if (solidMask[idxP(nx - 1, j)]) {
            u[idxU(nx, j)] = 0.0f;
        } else {
            u[idxU(nx, j)] =
                u_star[idxU(nx, j)] +
                outletFactor * p[idxP(nx - 1, j)];
        }
    }

    applyBC();
}

void Solver::applyBC() {
    const int nx = cfg.nx, ny = cfg.ny;
    for (int j = 0; j < ny; ++j)
        u[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;

    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0f;
        v[idxV(i, ny)] = 0.0f;
    }
}

float Solver::maxDivergence() const {
    const int nx = cfg.nx, ny = cfg.ny;
    float worst = 0.0f;
    for (int j = 0; j < ny; ++j) {
        const int rowP = j * nx;
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i) {
            if (solidMask[rowP + i])
                continue;
            const float div =
                (u[rowU + i + 1] - u[rowU + i]) * invDx +
                (v[rowVTop + i] - v[rowV + i]) * invDy;
            worst = std::max(worst, std::fabs(div));
        }
    }
    return worst;
}

float Solver::maxVelocity() const {
    float maxVel = 0.0f;
    for (float value : u) maxVel = std::max(maxVel, std::fabs(value));
    for (float value : v) maxVel = std::max(maxVel, std::fabs(value));
    return maxVel;
}

void Solver::projectRestartState() {
    const int nx = cfg.nx, ny = cfg.ny;

    u_star = u;
    v_star = v;

    for (int j = 0; j < ny; ++j) {
        u_star[idxU(0, j)] = solidMask[idxP(0, j)] ? 0.0f : cfg.U0;
        u_star[idxU(nx, j)] =
            solidMask[idxP(nx - 1, j)] ?
            0.0f :
            u_star[idxU(nx - 1, j)];
    }
    for (int i = 0; i < nx; ++i) {
        v_star[idxV(i, 0)] = 0.0f;
        v_star[idxV(i, ny)] = 0.0f;
    }

    solvePoisson();
    corrector();

    std::cout << "  reconstructed state projected, div = "
              << maxDivergence() << "\n";
}

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    if (!hasRestartState) {
        currentTime = 0.0;
        step = 0;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputPath, ec);

    if (hasRestartState && restartDt > 0.0f && !needsProjection) {
        dt = restartDt;
    } else {
        computeDt();
    }
    std::cout << "Program outputs 'Saved ---' one in ten saves. " << std::endl;
    if (hasRestartState) {
        if (needsProjection)
            projectRestartState();
        std::cout << "Continuing from t = " << currentTime
                  << " s, step " << step
                  << ". Frames go to " << framePrefix << "_<step>.vtk\n";
    } else {
        saveVTK(step);
    }

    const int saveInterval = std::max(1, cfg.saveInterval);
    const int dtUpdateInterval = std::max(1, cfg.dtUpdateInterval);

    while (currentTime < cfg.totalTime) {
        if (step % dtUpdateInterval == 0)
            computeDt();

        float stepDt = dt;
        bool lastStep = false;
        const double remaining = cfg.totalTime - currentTime;
        if (remaining <= static_cast<double>(dt)) {
            stepDt = static_cast<float>(remaining);
            lastStep = true;
        } else if (remaining < 2.0 * static_cast<double>(dt)) {
            // Halving the last stretch beats a full step followed by whatever
            // is left. The Poisson right-hand side is div/dt, so a step of
            // nanoseconds writes a frame whose pressure map is scaled by
            // millions; this way no step is ever shorter than half of dt.
            stepDt = static_cast<float>(0.5 * remaining);
        }
        if (!(stepDt > 0.f))
            break;
        const float savedDt = dt;
        dt = stepDt;

        applySlipFaces();
        predictor();
        solvePoisson();
        corrector();

        currentTime += dt;
        // stepDt is a float and the remainder it was cut from is a double, so
        // accumulating it lands a few ulps short of totalTime and the loop
        // would run once more for a leftover that is pure rounding.
        if (lastStep)
            currentTime = cfg.totalTime;
        step++;
        dt = savedDt;

        if (step % 10 == 0) {
            const float maxVel = maxVelocity();
            std::cout << "Step " << step
                      << ", t = " << currentTime
                      << " s, dt = " << stepDt
                      << ", |u|max = " << maxVel
                      << ", div = " << maxDivergence()
                      << ", mg res = " << lastResidual
                      << " (" << multigrid.cyclesUsed() << " cycles)"
                      << std::endl;
            if (std::isnan(maxVel) || std::isinf(maxVel)) {
                std::cerr << "Solution diverged at step " << step
                          << "; aborting.\n";
                break;
            }
        }

        if (step % saveInterval == 0)
            saveVTK(step);
    }

    if (step % saveInterval != 0)
        saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s after "
              << step << " steps.\n";
}

void Solver::saveVTK(int stepNum) const {
    const int nx = cfg.nx, ny = cfg.ny;
    constexpr size_t BUFFER_WORDS = 4096;
    std::array<uint32_t, BUFFER_WORDS> buffer;
    size_t bufferPos = 0;
    std::filesystem::path filename = outputPath;
    filename /= framePrefix + "_" + std::to_string(stepNum) + ".vtk";

    std::ofstream fout(filename, std::ios::binary);
    if (!fout){
        std::cerr << "Cannot open " << pathToConsole(filename) << " for writing.\n";
        return;
    }
    fout
        << "# vtk DataFile Version 3.0\n"
        << "CFD-Solver-2D output, step " << stepNum << "\n"
        << "BINARY\n"
        << "DATASET STRUCTURED_POINTS\n"
        << "DIMENSIONS "
        << nx + 1 << " "
        << ny + 1 << " 1\n"
        << "ORIGIN 0 0 0\n"
        << "SPACING "
        << dx << " "
        << dy << " 1\n"
        << "CELL_DATA "
        << nx * ny << "\n";

    auto flushFloatBuffer = [&](){
        if (bufferPos == 0)
            return;
        fout.write(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<std::streamsize>(bufferPos * sizeof(uint32_t)));
        bufferPos = 0;
    };
    // Legacy VTK binary data is big endian, so every word is byte-swapped
    auto writeWord = [&](uint32_t x){
        buffer[bufferPos++] =
            ((x & 0x000000FFu) << 24) |
            ((x & 0x0000FF00u) << 8 ) |
            ((x & 0x00FF0000u) >> 8 ) |
            ((x & 0xFF000000u) >> 24);
        if (bufferPos == BUFFER_WORDS)
            flushFloatBuffer();
    };
    auto writeFloat = [&](float value){
        uint32_t x;
        std::memcpy(&x, &value, sizeof(float));
        writeWord(x);
    };
    auto writeInt = [&](int32_t value){
        uint32_t x;
        std::memcpy(&x, &value, sizeof(int32_t));
        writeWord(x);
    };

    fout << "SCALARS pressure float 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i){
            // p holds the reduced pressure, so the hydrostatic field is put
            // back here and nowhere else. Solid cells have a zero diagonal and
            // never take part in the solve, so they keep the hydrostatic value
            // alone instead of punching a hole through the pressure map.
            const float value =
                phiCell(i, j) + (solidMask[row + i] ? 0.0f : p[row + i]);
            writeFloat(value * cfg.ro);
        }
    }
    flushFloatBuffer();
    fout << "\n";

    fout << "SCALARS solid int 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i)
        {
            writeInt(static_cast<int32_t>(solidMask[row + i]));
        }
    }
    flushFloatBuffer();
    fout << "\n";

    fout << "VECTORS velocity float\n";
    for (int j = 0; j < ny; ++j){
        const int rowU = j * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 0; i < nx; ++i){
            float uu =
                0.5f * (u[rowU + i] + u[rowU + i + 1]);
            float vv =
                0.5f * (v[rowV + i] + v[rowVTop + i]);
            // The faces normal to a wall are held shut, so averaging them
            // would draw the rim of a spinning body at half speed. Solid cells
            // get the surface velocity they actually impose instead. Nothing
            // reads this back; it is purely what ParaView sees.
            if ((wallsMove || wallsSlip) && solidMask[rowV + i]) {
                const WallField& wall = wallField[mesh.objectId[rowV + i]];
                uu = wall.slideX - wall.omega * ((j + 0.5f) * dy - wall.cy);
                vv = wall.slideY + wall.omega * ((i + 0.5f) * dx - wall.cx);
            }
            writeFloat(uu);
            writeFloat(vv);
            writeFloat(0.0f);
        }
    }
    flushFloatBuffer();
    fout << "\n";

    std::ostringstream state;
    state << std::setprecision(std::numeric_limits<double>::max_digits10)
          << "restartTime=" << currentTime << "\n"
          << "restartStep=" << stepNum << "\n"
          << std::setprecision(std::numeric_limits<float>::max_digits10)
          << "restartDt=" << dt << "\n";
    const std::string configText = configHeader + state.str();

    const size_t uCount = static_cast<size_t>(nx + 1) * ny;
    const size_t vCount = static_cast<size_t>(nx) * (ny + 1);
    const size_t pCount = static_cast<size_t>(nx) * ny;

    fout << "FIELD RestartData 4\n";
    fout << "configText 1 " << configText.size() << " char\n";
    fout.write(configText.data(),
               static_cast<std::streamsize>(configText.size()));
    fout << "\n";

    fout << "uFace 1 " << uCount << " float\n";
    for (size_t id = 0; id < uCount; ++id)
        writeFloat(u[id]);
    flushFloatBuffer();
    fout << "\n";

    fout << "vFace 1 " << vCount << " float\n";
    for (size_t id = 0; id < vCount; ++id)
        writeFloat(v[id]);
    flushFloatBuffer();
    fout << "\n";

    // Written as the total pressure, which is what every frame ever written
    // holds, so old frames keep restarting and setInitialState has one rule.
    fout << "pRaw 1 " << pCount << " float\n";
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            writeFloat(p[j * nx + i] + phiCell(i, j));
    flushFloatBuffer();
    fout << "\n";

    if (stepNum % (std::max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
        std::cout << "Saved " << pathToConsole(filename) << std::endl;
}
