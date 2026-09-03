#include "Solver.hpp"
#include "AppPaths.hpp"
#include "Progress.hpp"
#include "Runtime.hpp"
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

enum PhiKind {
    PhiUpwind = 0,
    PhiCentral,
    PhiMinmod,
    PhiVanLeer,
    PhiSuperbee
};

constexpr float kLimiterFloor = 1e-20f;

template <int Phi>
inline float phiOf(float r) {
    switch (Phi) {
    case PhiCentral:  return 1.0f;
    case PhiMinmod:   return std::max(0.0f, std::min(1.0f, r));
    case PhiVanLeer:  return (r + std::fabs(r)) / (1.0f + std::fabs(r));
    case PhiSuperbee:
        return std::max(0.0f,
                        std::max(std::min(2.0f * r, 1.0f),
                                 std::min(r, 2.0f)));
    default:          return 0.0f;
    }
}

template <int Phi>
inline float limitedDelta(float back, float fwd) {
    if (Phi == PhiUpwind)
        return back;
    const float safe = (std::fabs(fwd) > kLimiterFloor)
                           ? fwd
                           : (fwd < 0.0f ? -kLimiterFloor : kLimiterFloor);
    return back + phiOf<Phi>(back / safe) * 0.5f * (fwd - back);
}

#ifdef __AVX2__

template <int Phi>
inline __m256 phiOfVec(__m256 r) {
    const __m256 zero = _mm256_setzero_ps();
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 two = _mm256_set1_ps(2.0f);
    switch (Phi) {
    case PhiCentral:
        return one;
    case PhiMinmod:
        return _mm256_max_ps(zero, _mm256_min_ps(one, r));
    case PhiVanLeer: {
        const __m256 mag =
            _mm256_and_ps(r, _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF)));
        return _mm256_div_ps(_mm256_add_ps(r, mag), _mm256_add_ps(one, mag));
    }
    case PhiSuperbee:
        return _mm256_max_ps(
            zero,
            _mm256_max_ps(_mm256_min_ps(_mm256_mul_ps(two, r), one),
                          _mm256_min_ps(r, two)));
    default:
        return zero;
    }
}

template <int Phi>
inline __m256 limitedDeltaVec(__m256 back, __m256 fwd) {
    if (Phi == PhiUpwind)
        return back;
    const __m256 signBit = _mm256_castsi256_ps(_mm256_set1_epi32(0x80000000));
    const __m256 magnitude =
        _mm256_max_ps(_mm256_andnot_ps(signBit, fwd),
                      _mm256_set1_ps(kLimiterFloor));
    const __m256 safe =
        _mm256_or_ps(magnitude, _mm256_and_ps(signBit, fwd));
    return _mm256_add_ps(
        back,
        _mm256_mul_ps(
            _mm256_mul_ps(phiOfVec<Phi>(_mm256_div_ps(back, safe)),
                          _mm256_set1_ps(0.5f)),
            _mm256_sub_ps(fwd, back)));
}

#endif
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

Solver::Solver(const Config& cfg, Mesh& mesh)
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
        bodyGravity = cfg.gravityMode == GravityMode::Body;
    }

    // Relative to the executable, not to the working directory: a shortcut, a
    // file manager and a terminal each hand the process a different one, so
    // "output" used to mean three different folders.
    outputPath = resolveOutputDir(narrowToPath(cfg.outputDir));

    configHeader = "formatVersion=" + std::to_string(FRAME_FORMAT_VERSION) +
                   "\n" + cfg.serialize();
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
    if (state.cfg.gravityEnabled && !bodyGravity) {
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

    if (cfg.multiphase() &&
        state.phase.size() == static_cast<size_t>(cfg.nx) * cfg.ny) {
        phase.resize(cfg.nx, cfg.ny);
        phase.fraction() = std::move(state.phase);
    }

    restartBodies = std::move(state.bodies);

    const std::size_t cells = static_cast<std::size_t>(cfg.nx) * cfg.ny;
    for (auto& entry : state.extras) {
        if (entry.second.size() != cells)
            continue;
        if (entry.first == "k")
            restartK = std::move(entry.second);
        else if (entry.first == "omega")
            restartOmega = std::move(entry.second);
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

void Solver::resolveBoundaries() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    const auto fill = [this](SideData& data, BoundarySide which) {
        const BoundarySpec& spec = cfg.boundaries[which];
        data.kind = spec.kind;
        data.outlet = spec.kind == BoundaryKind::Outlet;
        data.inlet = spec.kind == BoundaryKind::Inlet;

        switch (spec.kind) {
        case BoundaryKind::Wall:
            data.ghostSign = -1.0f;
            data.ghostOffset = 0.0f;
            break;
        case BoundaryKind::MovingWall:
            data.ghostSign = -1.0f;
            data.ghostOffset = 2.0f * (spec.speedSet ? spec.speed : 0.0f);
            break;
        default:
            data.ghostSign = 1.0f;
            data.ghostOffset = 0.0f;
            break;
        }
    };

    fill(sideLeft, BoundarySide::Left);
    fill(sideRight, BoundarySide::Right);
    fill(sideBottom, BoundarySide::Bottom);
    fill(sideTop, BoundarySide::Top);

    const auto band = [this](BoundarySide which, int cells,
                             std::vector<float>& out) {
        const BoundarySpec& spec = cfg.boundaries[which];
        out.assign(static_cast<size_t>(cells), 0.0f);
        if (spec.kind != BoundaryKind::Inlet)
            return;
        BoundarySpec resolved = spec;
        if (!resolved.speedSet)
            resolved.speed = cfg.U0;
        for (int k = 0; k < cells; ++k)
            out[k] = inletVelocityAt(resolved,
                                     (k + 0.5f) / static_cast<float>(cells));
    };

    band(BoundarySide::Left, ny, uInletLeft);
    band(BoundarySide::Right, ny, uInletRight);
    band(BoundarySide::Bottom, nx, vInletBottom);
    band(BoundarySide::Top, nx, vInletTop);

    MultigridBC bc;
    const auto level = [](const SideData& data) {
        return data.outlet ? PressureSideBC::Dirichlet : PressureSideBC::Neumann;
    };
    bc.left = level(sideLeft);
    bc.right = level(sideRight);
    bc.bottom = level(sideBottom);
    bc.top = level(sideTop);
    multigrid.setPressureBC(bc);

    if (multigrid.singularPressure())
        std::cout << "  note: no side lets fluid out, so the pressure has no "
                     "level of its own.\n  It is solved up to a constant and "
                     "the mean is taken off every cycle, which is correct for "
                     "a\n  closed box and is what a lid driven cavity needs.\n";
}

void Solver::applyBoundaryVelocities(std::vector<float>& uf,
                                     std::vector<float>& vf,
                                     bool extrapolateOutlet) const {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    for (int j = 0; j < ny; ++j) {
        const bool solidLow = solidMask[idxP(0, j)] != 0;
        const bool solidHigh = solidMask[idxP(nx - 1, j)] != 0;

        if (sideLeft.inlet)
            uf[idxU(0, j)] = solidLow ? 0.0f : uInletLeft[j];
        else if (sideLeft.outlet) {
            if (extrapolateOutlet)
                uf[idxU(0, j)] = solidLow ? 0.0f : uf[idxU(1, j)];
        } else
            uf[idxU(0, j)] = 0.0f;

        if (sideRight.inlet)
            uf[idxU(nx, j)] = solidHigh ? 0.0f : -uInletRight[j];
        else if (sideRight.outlet) {
            if (extrapolateOutlet)
                uf[idxU(nx, j)] = solidHigh ? 0.0f : uf[idxU(nx - 1, j)];
        } else
            uf[idxU(nx, j)] = 0.0f;
    }

    for (int i = 0; i < nx; ++i) {
        const bool solidLow = solidMask[idxP(i, 0)] != 0;
        const bool solidHigh = solidMask[idxP(i, ny - 1)] != 0;

        if (sideBottom.inlet)
            vf[idxV(i, 0)] = solidLow ? 0.0f : vInletBottom[i];
        else if (sideBottom.outlet) {
            if (extrapolateOutlet)
                vf[idxV(i, 0)] = solidLow ? 0.0f : vf[idxV(i, 1)];
        } else
            vf[idxV(i, 0)] = 0.0f;

        if (sideTop.inlet)
            vf[idxV(i, ny)] = solidHigh ? 0.0f : -vInletTop[i];
        else if (sideTop.outlet) {
            if (extrapolateOutlet)
                vf[idxV(i, ny)] = solidHigh ? 0.0f : vf[idxV(i, ny - 1)];
        } else
            vf[idxV(i, ny)] = 0.0f;
    }
}

void Solver::applyOutletFaces() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;
    const float factorX = 2.f * dt * invDx;
    const float factorY = 2.f * dt * invDy;

    const float* __restrict invRhoXPtr =
        multiphase ? phase.faceInvRhoX().data() : nullptr;
    const float* __restrict invRhoYPtr =
        multiphase ? phase.faceInvRhoY().data() : nullptr;
    const auto wX = [&](int i, int j) {
        return invRhoXPtr ? invRhoXPtr[idxU(i, j)] : 1.0f;
    };
    const auto wY = [&](int i, int j) {
        return invRhoYPtr ? invRhoYPtr[idxV(i, j)] : 1.0f;
    };

    if (sideLeft.outlet)
        for (int j = 0; j < ny; ++j)
            u[idxU(0, j)] =
                solidMask[idxP(0, j)]
                    ? 0.0f
                    : u_star[idxU(0, j)] -
                          factorX * wX(0, j) *
                              (p[idxP(0, j)] -
                               phiOutside(BoundarySide::Left, j));

    if (sideRight.outlet)
        for (int j = 0; j < ny; ++j)
            u[idxU(nx, j)] =
                solidMask[idxP(nx - 1, j)]
                    ? 0.0f
                    : u_star[idxU(nx, j)] +
                          factorX * wX(nx, j) *
                              (p[idxP(nx - 1, j)] -
                               phiOutside(BoundarySide::Right, j));

    if (sideBottom.outlet)
        for (int i = 0; i < nx; ++i)
            v[idxV(i, 0)] =
                solidMask[idxP(i, 0)]
                    ? 0.0f
                    : v_star[idxV(i, 0)] -
                          factorY * wY(i, 0) *
                              (p[idxP(i, 0)] -
                               phiOutside(BoundarySide::Bottom, i));

    if (sideTop.outlet)
        for (int i = 0; i < nx; ++i)
            v[idxV(i, ny)] =
                solidMask[idxP(i, ny - 1)]
                    ? 0.0f
                    : v_star[idxV(i, ny)] +
                          factorY * wY(i, ny) *
                              (p[idxP(i, ny - 1)] -
                               phiOutside(BoundarySide::Top, i));
}

void Solver::refreshSurfaceTension() {
    if (!hasTension)
        return;

    const int nx = cfg.nx, ny = cfg.ny;
    phase.computeCurvature(solidMask, dx, dy, cfg.contactAngle);

    const float* __restrict kappa = phase.curvature().data();
    const float* __restrict weight = phase.gradientMagnitude().data();
    const float* __restrict cPtr = phase.fraction().data();
    const float* __restrict invRhoX = phase.faceInvRhoX().data();
    const float* __restrict invRhoY = phase.faceInvRhoY().data();
    float* __restrict tx = tensionX.data();
    float* __restrict ty = tensionY.data();
    const float sigma = cfg.surfaceTension;

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        tx[rowU] = 0.0f;
        tx[rowU + nx] = 0.0f;
        for (int i = 1; i < nx; ++i) {
            const float wa = weight[rowC + i - 1];
            const float wb = weight[rowC + i];
            const float sum = wa + wb;
            const float faceKappa =
                (sum > 1e-12f)
                    ? (wa * kappa[rowC + i - 1] + wb * kappa[rowC + i]) / sum
                    : 0.0f;
            tx[rowU + i] = sigma * faceKappa *
                           (cPtr[rowC + i] - cPtr[rowC + i - 1]) * invDx *
                           invRhoX[rowU + i];
        }
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowBelow = static_cast<size_t>(j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            const float wa = weight[rowBelow + i];
            const float wb = weight[rowC + i];
            const float sum = wa + wb;
            const float faceKappa =
                (sum > 1e-12f)
                    ? (wa * kappa[rowBelow + i] + wb * kappa[rowC + i]) / sum
                    : 0.0f;
            ty[rowC + i] = sigma * faceKappa *
                           (cPtr[rowC + i] - cPtr[rowBelow + i]) * invDy *
                           invRhoY[rowC + i];
        }
    }
    const size_t topRow = static_cast<size_t>(ny) * nx;
    for (int i = 0; i < nx; ++i) {
        ty[i] = 0.0f;
        ty[topRow + i] = 0.0f;
    }
}

void Solver::refreshPhaseCoefficients() {
    multigrid.setCoefficients(phase.faceInvRhoX(), phase.faceInvRhoY());
}

void Solver::advectPhase() {
    phase.advect(u, v, solidMask, dt, dx, dy);
    if (hasSources) {
        float* __restrict c = phase.fraction().data();
        for (int id : sourceCells) {
            const float mix = std::min(1.0f, sourceRate[id] * dt);
            c[id] += mix * (sourcePhase[id] - c[id]);
        }
    }
    phase.refreshProperties(solidMask);
    refreshPhaseCoefficients();
    refreshSurfaceTension();
    refreshViscosity();
}

void Solver::buildSources() {
    const int nx = cfg.nx, ny = cfg.ny;
    const std::vector<FlowSource> list = cfg.resolvedSources();
    hasSources = false;
    sourceInflow = 0.0;
    if (list.empty())
        return;

    sourceRate.assign(static_cast<size_t>(nx) * ny, 0.0f);
    sourcePhase.assign(static_cast<size_t>(nx) * ny, 0.0f);
    sourceU.assign(static_cast<size_t>(nx) * ny, 0.0f);
    sourceV.assign(static_cast<size_t>(nx) * ny, 0.0f);

    constexpr float degToRad = 3.14159265358979f / 180.0f;
    int totalCells = 0;
    sourcesRide = false;
    sourceLive.assign(list.size(), 0);
    for (size_t which = 0; which < list.size(); ++which) {
        FlowSource source = list[which];

        if (source.body > 0 &&
            static_cast<size_t>(source.body) < bodies.size()) {
            const RigidBody& body = bodies[source.body];
            if (body.everFree || body.prescribed) {
                sourcesRide = true;
                const float theta = static_cast<float>(body.theta);
                const float cosT = std::cos(theta);
                const float sinT = std::sin(theta);
                const float localX = source.x;
                const float localY = source.y;
                source.x = body.cx + static_cast<float>(body.x) +
                           localX * cosT - localY * sinT;
                source.y = body.cy + static_cast<float>(body.y) +
                           localX * sinT + localY * cosT;
                source.angle += theta / degToRad;
            }
        }

        const float radiusSq = source.radius * source.radius;
        int cells = 0;
        for (int j = 0; j < ny; ++j) {
            const float y = (j + 0.5f) * dy;
            for (int i = 0; i < nx; ++i) {
                const float x = (i + 0.5f) * dx;
                const float d = (x - source.x) * (x - source.x) +
                                (y - source.y) * (y - source.y);
                if (d > radiusSq || solidMask[idxP(i, j)])
                    continue;
                ++cells;
                const int id = idxP(i, j);

                sourceRate[id] += 2.0f * source.rate / source.radius;
                sourcePhase[id] = source.phase;
                sourceU[id] += source.rate * std::cos(source.angle * degToRad);
                sourceV[id] += source.rate * std::sin(source.angle * degToRad);
            }
        }
        if (cells == 0) {
            std::cerr << "Warning: the source at (" << source.x << ", "
                      << source.y << ") with radius " << source.radius
                      << " m covers no cell at all, so nothing comes out of "
                         "it.\n  One cell is " << dx << " x " << dy
                      << " m; give it a bigger radius or a finer grid.\n";
        }
        sourceLive[which] = cells > 0 ? 1 : 0;
        totalCells += cells;
        if (cells == 0)
            continue;
        sourceInflow += static_cast<double>(source.rate) * 2.0 *
                        3.14159265358979 * source.radius;
    }

    sourceCells.clear();
    for (int id = 0; id < nx * ny; ++id)
        if (sourceRate[id] > 0.0f)
            sourceCells.push_back(id);

    hasSources = totalCells > 0;
    if (hasSources && !sourcesReported) {
        sourcesReported = true;
        std::cout << "Sources: " << list.size() << " over " << totalCells
                  << " cells, " << sourceInflow << " m^2/s in total.";
        if (sourcesRide)
            std::cout << " One of them rides a body, so this is rebuilt every "
                         "step and only said once.";
        std::cout << "\n";
    }
}

void Solver::applySources() {
    if (!hasSources)
        return;
    const int nx = cfg.nx;
    for (int id : sourceCells) {
        const int i = id % nx;
        const int j = id / nx;
        const float blend = std::min(1.0f, sourceRate[id] * dt);
        if (i > 0 && uFluidMaskF[idxU(i, j)] != 0.0f)
            u_star[idxU(i, j)] += blend * (sourceU[id] - u_star[idxU(i, j)]);
        if (i < nx - 1 && uFluidMaskF[idxU(i + 1, j)] != 0.0f)
            u_star[idxU(i + 1, j)] +=
                blend * (sourceU[id] - u_star[idxU(i + 1, j)]);
        if (j > 0 && vFluidMaskF[idxV(i, j)] != 0.0f)
            v_star[idxV(i, j)] += blend * (sourceV[id] - v_star[idxV(i, j)]);
        if (j < cfg.ny - 1 && vFluidMaskF[idxV(i, j + 1)] != 0.0f)
            v_star[idxV(i, j + 1)] +=
                blend * (sourceV[id] - v_star[idxV(i, j + 1)]);
    }
}

float Solver::phiOutside(BoundarySide side, int k) const {
    const float head = phiFace(side, k);
    if (!multiphase || head == 0.0f)
        return head;
    const int nx = cfg.nx, ny = cfg.ny;
    int cell = 0;
    switch (side) {
    case BoundarySide::Left:   cell = idxP(0, k); break;
    case BoundarySide::Right:  cell = idxP(nx - 1, k); break;
    case BoundarySide::Bottom: cell = idxP(k, 0); break;
    default:                   cell = idxP(k, ny - 1); break;
    }
    return head * phase.density()[cell];
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

            if (bodiesMove && solidLeft != solidRight) {
                const int owner =
                    mesh.objectId[row + (solidLeft ? i - 1 : i)];
                if (owner > 0 && static_cast<size_t>(owner) < bodies.size()) {
                    const RigidBody& body = bodies[owner];
                    if (body.everFree || body.prescribed)
                        uWall[idxU(i, j)] =
                            body.vx -
                            body.omega *
                                (yFace - (body.cy + static_cast<float>(body.y)));
                }
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

            if (bodiesMove && solidTop != solidBot) {
                const int owner =
                    mesh.objectId[(solidTop ? row : rowBot) + i];
                if (owner > 0 && static_cast<size_t>(owner) < bodies.size()) {
                    const RigidBody& body = bodies[owner];
                    if (body.everFree || body.prescribed)
                        vWall[idxV(i, j)] =
                            body.vy +
                            body.omega * ((i + 0.5f) * dx -
                                          (body.cx +
                                           static_cast<float>(body.x)));
                }
            }
        }
    }

    if (bodiesMove) {
        for (int j = 0; j < ny; ++j) {
            const int row = j * nx;
            const float yFace = (j + 0.5f) * dy;
            for (int i = 1; i < nx; ++i) {
                if (!solidMask[row + i] || !solidMask[row + i - 1])
                    continue;
                const int owner = mesh.objectId[row + i];
                if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                    continue;
                const RigidBody& body = bodies[owner];
                if (!body.everFree && !body.prescribed)
                    continue;
                uWall[idxU(i, j)] +=
                    body.vx -
                    body.omega * (yFace - (body.cy + static_cast<float>(body.y)));
            }
        }
        for (int j = 1; j < ny; ++j) {
            const int row = j * nx;
            const int rowBot = (j - 1) * nx;
            for (int i = 0; i < nx; ++i) {
                if (!solidMask[row + i] || !solidMask[rowBot + i])
                    continue;
                const int owner = mesh.objectId[row + i];
                if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                    continue;
                const RigidBody& body = bodies[owner];
                if (!body.everFree && !body.prescribed)
                    continue;
                vWall[idxV(i, j)] +=
                    body.vy +
                    body.omega *
                        ((i + 0.5f) * dx - (body.cx + static_cast<float>(body.x)));
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

void Solver::buildMirrorFaces() {
    const int nx = cfg.nx;
    const int ny = cfg.ny;

    uMirrorFaces.clear();
    vMirrorFaces.clear();

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 1; i < nx; ++i) {
            if (!solidMask[row + i] || !solidMask[row + i - 1])
                continue;
            const WallField& wall = wallField[mesh.objectId[row + i]];
            if (wall.slip)
                continue;

            const int below =
                (j > 0 && uFluidMask[idxU(i, j - 1)]) ? idxU(i, j - 1) : -1;
            const int above =
                (j + 1 < ny && uFluidMask[idxU(i, j + 1)]) ? idxU(i, j + 1) : -1;
            if (below < 0 && above < 0)
                continue;

            // The wall is the cell edge between the buried face and the open
            // one, so its velocity is taken there and not at either face. A
            // body one cell thick has fluid on both sides and one buried face
            // to serve both walls, and then the two are averaged.
            float sum = 0.0f;
            int walls = 0;
            if (below >= 0) {
                sum += wall.slideX - wall.omega * (j * dy - wall.cy);
                ++walls;
            }
            if (above >= 0) {
                sum += wall.slideX - wall.omega * ((j + 1) * dy - wall.cy);
                ++walls;
            }

            MirrorFace mirror;
            mirror.face = idxU(i, j);
            mirror.first = (below >= 0) ? below : above;
            mirror.second = (above >= 0) ? above : below;
            mirror.wall = sum / static_cast<float>(walls);
            uMirrorFaces.push_back(mirror);
        }
    }

    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            if (!solidMask[row + i] || !solidMask[rowBot + i])
                continue;
            const WallField& wall = wallField[mesh.objectId[row + i]];
            if (wall.slip)
                continue;

            const int left =
                (i > 0 && vFluidMask[idxV(i - 1, j)]) ? idxV(i - 1, j) : -1;
            const int right =
                (i + 1 < nx && vFluidMask[idxV(i + 1, j)]) ? idxV(i + 1, j) : -1;
            if (left < 0 && right < 0)
                continue;

            float sum = 0.0f;
            int walls = 0;
            if (left >= 0) {
                sum += wall.slideY + wall.omega * (i * dx - wall.cx);
                ++walls;
            }
            if (right >= 0) {
                sum += wall.slideY + wall.omega * ((i + 1) * dx - wall.cx);
                ++walls;
            }

            MirrorFace mirror;
            mirror.face = idxV(i, j);
            mirror.first = (left >= 0) ? left : right;
            mirror.second = (right >= 0) ? right : left;
            mirror.wall = sum / static_cast<float>(walls);
            vMirrorFaces.push_back(mirror);
        }
    }
}

void Solver::applyMirrorFaces() {
    for (const MirrorFace& mirror : uMirrorFaces)
        u[mirror.face] = 2.0f * mirror.wall -
                         0.5f * (u[mirror.first] + u[mirror.second]);
    for (const MirrorFace& mirror : vMirrorFaces)
        v[mirror.face] = 2.0f * mirror.wall -
                         0.5f * (v[mirror.first] + v[mirror.second]);
}

void Solver::refreshGeometry() {
    if (!bodiesMove)
        return;

    const int nx = cfg.nx, ny = cfg.ny;
    prevSolidMask = solidMask;
    prevUFluidMask = uFluidMask;
    prevVFluidMask = vFluidMask;
    prevObjectId = mesh.objectId;

    applyBodyPoses();
    mesh.updateSolid();

    if (!mesh.renumbered().empty() && !renumberReported) {
        renumberReported = true;
        std::cout << "  note: the bodies were renumbered - ";
        for (const std::pair<int, int>& change : mesh.renumbered()) {
            if (change.first == 0)
                std::cout << "a new body came out as " << change.second << " ";
            else
                std::cout << "body " << change.first << " is gone ";
        }
        std::cout << "\n  which happens when two of them touch or one leaves "
                     "the domain. wallMotion and bodyMotion\n  address them by "
                     "number, so from here those lines mean something else.\n";
    }

    #pragma omp parallel for schedule(static) if (nx * ny >= 4096)
    for (int id = 0; id < nx * ny; ++id) {
        solidMask[id] = mesh.solid[id] ? 1 : 0;
        fluidCellMaskF[id] = mesh.solid[id] ? 0.0f : 1.0f;
    }

    buildFaceMasks();
    buildSlipFaces();
    buildMirrorFaces();
    multigrid.setGeometry(solidMask, true);

    if (multiphase) {
        phase.refreshProperties(solidMask);
        refreshPhaseCoefficients();
        if (hasTension)
            refreshSurfaceTension();
    }
    refreshViscosity();

    fillFreshCells();

    if (hasSources && sourcesRide)
        buildSources();
}

void Solver::fillFreshCells() {
    const int nx = cfg.nx, ny = cfg.ny;
    freshCells = 0;
    if (prevSolidMask.size() != solidMask.size())
        return;

    //

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        const float yFace = (j + 0.5f) * dy;
        for (int i = 0; i <= nx; ++i) {
            const int id = idxU(i, j);
            if (!uFluidMask[id] || prevUFluidMask[id])
                continue;
            ++freshCells;

            float filled = 0.0f;
            bool haveBody = false;
            for (int side = 0; side < 2 && !haveBody; ++side) {
                const int cell = i - 1 + side;
                if (cell < 0 || cell >= nx)
                    continue;
                const int owner = prevObjectId[row + cell];
                if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                    continue;
                const RigidBody& body = bodies[owner];
                if (!body.everFree && !body.prescribed)
                    continue;
                filled = body.vx -
                         body.omega *
                             (yFace - (body.cy + static_cast<float>(body.y)));
                haveBody = true;
            }

            if (!haveBody) {
                float sum = 0.0f;
                int count = 0;
                for (int step2 = -1; step2 <= 1; step2 += 2) {
                    const int other = i + step2;
                    if (other < 0 || other > nx)
                        continue;
                    if (prevUFluidMask[idxU(other, j)]) {
                        sum += u[idxU(other, j)];
                        ++count;
                    }
                    const int upDown = j + step2;
                    if (upDown < 0 || upDown >= ny)
                        continue;
                    if (prevUFluidMask[idxU(i, upDown)]) {
                        sum += u[idxU(i, upDown)];
                        ++count;
                    }
                }
                filled = count > 0 ? sum / static_cast<float>(count) : 0.0f;
            }
            u[id] = filled;
        }
    }

    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int id = idxV(i, j);
            if (!vFluidMask[id] || prevVFluidMask[id])
                continue;
            ++freshCells;

            float filled = 0.0f;
            bool haveBody = false;
            for (int side = 0; side < 2 && !haveBody; ++side) {
                const int cellRow = j - 1 + side;
                if (cellRow < 0 || cellRow >= ny)
                    continue;
                const int owner = prevObjectId[cellRow * nx + i];
                if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                    continue;
                const RigidBody& body = bodies[owner];
                if (!body.everFree && !body.prescribed)
                    continue;
                filled = body.vy +
                         body.omega * ((i + 0.5f) * dx -
                                       (body.cx + static_cast<float>(body.x)));
                haveBody = true;
            }

            if (!haveBody) {
                float sum = 0.0f;
                int count = 0;
                for (int step2 = -1; step2 <= 1; step2 += 2) {
                    const int other = j + step2;
                    if (other >= 0 && other <= ny &&
                        prevVFluidMask[idxV(i, other)]) {
                        sum += v[idxV(i, other)];
                        ++count;
                    }
                    const int leftRight = i + step2;
                    if (leftRight >= 0 && leftRight < nx &&
                        prevVFluidMask[idxV(leftRight, j)]) {
                        sum += v[idxV(leftRight, j)];
                        ++count;
                    }
                }
                filled = count > 0 ? sum / static_cast<float>(count) : 0.0f;
            }
            v[id] = filled;
        }
    }

    std::vector<float>* fraction = multiphase ? &phase.fraction() : nullptr;
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i) {
            const int id = row + i;
            if (solidMask[id] || !prevSolidMask[id])
                continue;

            float sumP = 0.0f, sumC = 0.0f;
            int count = 0;
            const int neighbours[4] = {i > 0 ? id - 1 : -1,
                                       i + 1 < nx ? id + 1 : -1,
                                       j > 0 ? id - nx : -1,
                                       j + 1 < ny ? id + nx : -1};
            for (int other : neighbours) {
                if (other < 0 || prevSolidMask[other])
                    continue;
                sumP += p[other];
                if (fraction)
                    sumC += (*fraction)[other];
                ++count;
            }
            if (count == 0)
                continue;
            p[id] = sumP / static_cast<float>(count);
            if (fraction)
                (*fraction)[id] = sumC / static_cast<float>(count);
        }
    }
}

void Solver::bodyForces() {
    if (!bodiesMove)
        return;
    const bool reportForces = cfg.bodyForceReport;

    const int nx = cfg.nx, ny = cfg.ny;
    for (RigidBody& body : bodies) {
        body.forceX = 0.0f;
        body.forceY = 0.0f;
        body.torque = 0.0f;
    }

    const float scale = pressureScale();
    const float* density = multiphase ? phase.density().data() : nullptr;
    const float faceX = dy;
    const float faceY = dx;

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        const float y = (j + 0.5f) * dy;
        for (int i = 1; i < nx; ++i) {
            const bool left = solidMask[row + i - 1] != 0;
            const bool right = solidMask[row + i] != 0;
            if (left == right)
                continue;
            const int owner = mesh.objectId[row + (left ? i - 1 : i)];
            if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                continue;
            RigidBody& body = bodies[owner];
            if (!body.everFree && !reportForces)
                continue;

            const int fluidCell = left ? row + i : row + i - 1;

            const float sign = left ? -1.0f : 1.0f;
            const float pressure = p[fluidCell] * scale;
            const float fx = sign * pressure * faceX;

            const float nu = multiphase
                                 ? phase.viscosity()[fluidCell] /
                                       std::max(1e-20f, density[fluidCell])
                                 : cfg.nu;
            const float rho = multiphase ? density[fluidCell] : cfg.ro;
            const float wallV = vWall[idxV(i - 1 + (left ? 1 : 0), j)];
            const float tangential =
                0.5f * (v[idxV(left ? i : i - 1, j)] +
                        v[idxV(left ? i : i - 1, j + 1)]) -
                wallV;
            const float fy = rho * nu * tangential / (0.5f * dx) * faceX * sign;

            body.forceX += fx;
            body.forceY += fy;
            const float armY = y - (body.cy + static_cast<float>(body.y));
            const float armX = i * dx - (body.cx + static_cast<float>(body.x));
            body.torque += armX * fy - armY * fx;
        }
    }

    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            const bool top = solidMask[row + i] != 0;
            const bool bottom = solidMask[rowBot + i] != 0;
            if (top == bottom)
                continue;
            const int owner = mesh.objectId[(top ? row : rowBot) + i];
            if (owner <= 0 || static_cast<size_t>(owner) >= bodies.size())
                continue;
            RigidBody& body = bodies[owner];
            if (!body.everFree && !reportForces)
                continue;

            const int fluidCell = top ? rowBot + i : row + i;
            const float sign = top ? 1.0f : -1.0f;
            const float pressure = p[fluidCell] * scale;
            const float fy = sign * pressure * faceY;

            const float nu = multiphase
                                 ? phase.viscosity()[fluidCell] /
                                       std::max(1e-20f, density[fluidCell])
                                 : cfg.nu;
            const float rho = multiphase ? density[fluidCell] : cfg.ro;
            const int fluidRow = top ? j - 1 : j;
            const float wallU = uWall[idxU(i, fluidRow)];
            const float tangential =
                0.5f * (u[idxU(i, fluidRow)] + u[idxU(i + 1, fluidRow)]) -
                wallU;
            const float fx = rho * nu * tangential / (0.5f * dy) * faceY * sign;

            body.forceX += fx;
            body.forceY += fy;
            const float armY = j * dy - (body.cy + static_cast<float>(body.y));
            const float armX =
                (i + 0.5f) * dx - (body.cx + static_cast<float>(body.x));
            body.torque += armX * fy - armY * fx;
        }
    }

    if (sourcesRide) {
        const std::vector<FlowSource> list = cfg.resolvedSources();
        constexpr float degToRad = 3.14159265358979f / 180.0f;
        for (size_t which = 0; which < list.size(); ++which) {
            const FlowSource& source = list[which];

            if (which >= sourceLive.size() || !sourceLive[which])
                continue;
            if (source.body <= 0 ||
                static_cast<size_t>(source.body) >= bodies.size())
                continue;
            RigidBody& body = bodies[source.body];
            if (!body.everFree && !body.prescribed)
                continue;
            const float rho =
                multiphase
                    ? source.phase * cfg.rho1 + (1.0f - source.phase) * cfg.rho2
                    : cfg.ro;
            const float flow = source.rate * 2.0f * 3.14159265358979f *
                               source.radius;
            const float thrust = rho * flow * source.rate;
            const float theta = static_cast<float>(body.theta);
            const float aim = source.angle * degToRad + theta;
            const float tx = -thrust * std::cos(aim);
            const float ty = -thrust * std::sin(aim);
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            const float armX = source.x * cosT - source.y * sinT;
            const float armY = source.x * sinT + source.y * cosT;
            body.forceX += tx;
            body.forceY += ty;
            body.torque += armX * ty - armY * tx;
        }
    }

    if (cfg.gravityEnabled) {
        for (RigidBody& body : bodies) {
            if (!body.everFree && !body.prescribed)
                continue;
            const float displaced =
                bodyGravity ? 0.0f
                            : (multiphase ? std::min(cfg.rho1, cfg.rho2)
                                          : cfg.ro) *
                                  body.area;
            body.forceX += (body.mass - displaced) * gx;
            body.forceY += (body.mass - displaced) * gy;
        }
    }
}

void Solver::advanceBodies(float stepDt) {
    if (!bodiesMove)
        return;

    const double middle = currentTime + 0.5 * static_cast<double>(stepDt);
    for (RigidBody& body : bodies)
        if (body.prescribed || body.everFree)
            body.step(middle, stepDt);

    if (bodyCollisions)
        resolveCollisions(stepDt);

    for (RigidBody& body : bodies)
        if (body.prescribed || body.everFree)
            body.advancePose(stepDt);
}

void Solver::resolveCollisions(float stepDt) {
    resolveBodyCollisions(bodies, mesh.ownership(), mesh.contested(),
                          cfg.nx, cfg.ny, cfg.Lx, cfg.Ly,
                          cfg.bodyRestitution, stepDt, contactsReported);
}

void Solver::applyBodyPoses() {
    for (const RigidBody& body : bodies) {
        if (!body.everFree && !body.prescribed)
            continue;
        Mesh::BodyPose pose;
        pose.x = body.x;
        pose.y = body.y;
        pose.theta = body.theta;
        mesh.setPose(body.object, pose);
    }
}

void Solver::resolveBodyMotion() {
    bodies.clear();
    bodiesMove = false;
    bodiesFree = false;
    if (cfg.bodyMotion.empty())
        return;

    std::vector<BodyMotion> motions;
    std::string error;
    if (!parseBodyMotion(cfg.bodyMotion, motions, error)) {
        std::cout << "\n!!! " << error << "\n    No body moves.\n";
        return;
    }
    if (motions.empty())
        return;

    if (mesh.objects.empty()) {
        std::cout << "\n!!! bodyMotion was given but the domain holds no body "
                     "at all, so there is nothing to move.\n";
        return;
    }
    if (!mesh.prepareMotion()) {
        std::cout << "\n!!! the mask this run started from cannot be moved: it "
                     "came out of a frame rather than\n    a model, and moving "
                     "a body means cutting its outline again every step. Give "
                     "the run\n    a geometryFile or profiles= and the bodies "
                     "will move.\n";
        return;
    }

    std::vector<BodyGeometry> geometry(mesh.objects.size() + 1);
    for (std::size_t id = 1; id < geometry.size(); ++id) {
        const Mesh::SolidObject& body = mesh.objects[id - 1];
        geometry[id].cx = static_cast<float>(body.cx);
        geometry[id].cy = static_cast<float>(body.cy);
        geometry[id].radius = static_cast<float>(body.radius);
        geometry[id].area = static_cast<float>(body.area);
    }

    const float fluidDensity =
        multiphase ? std::min(cfg.rho1, cfg.rho2) : cfg.ro;
    std::vector<std::string> notes;
    buildRigidBodies(motions, geometry, bodies, fluidDensity, notes);
    for (const std::string& note : notes)
        std::cout << "\n!!! " << note << "\n";

    for (const RigidBody& body : bodies) {
        if (body.everFree)
            bodiesFree = true;
        if (body.everFree || body.prescribed)
            bodiesMove = true;
    }
    if (!bodiesMove)
        return;
    bodyCollisions = cfg.bodyCollisions;

    if (cfg.bodyCoupling == BodyCoupling::Weak)
        for (RigidBody& body : bodies) {
            body.addedMass = 0.0f;
            body.addedInertia = 0.0f;
        }

    for (RigidBody& body : bodies)
        body.sampleVelocity(currentTime);

    for (const RestartData::BodyState& saved : restartBodies) {
        if (saved.object < 1 ||
            static_cast<size_t>(saved.object) >= bodies.size())
            continue;
        RigidBody& body = bodies[saved.object];
        body.x = saved.x;
        body.y = saved.y;
        body.theta = saved.theta;
        if (body.free) {
            body.vx = saved.vx;
            body.vy = saved.vy;
            body.omega = saved.omega;
        }
    }

    if (!restartBodies.empty()) {
        applyBodyPoses();
        mesh.updateSolid();
        const int cells = cfg.nx * cfg.ny;
        for (int id = 0; id < cells; ++id) {
            solidMask[id] = mesh.solid[id] ? 1 : 0;
            fluidCellMaskF[id] = mesh.solid[id] ? 0.0f : 1.0f;
        }
        std::cout << "  the bodies were put back where the frame left them "
                     "and the outline cut again there,\n  rather than the "
                     "rasterised mask being inherited.\n";
    }
}

void Solver::reportBodies() const {
    if (!bodiesMove)
        return;

    constexpr float degToRad = 3.14159265358979f / 180.0f;
    std::cout << "Bodies that travel:\n";
    for (const RigidBody& body : bodies) {
        if (!body.everFree && !body.prescribed)
            continue;
        std::cout << "  object " << body.object << " at (" << body.cx << ", "
                  << body.cy << ") m, " << body.area << " m^2";
        if (body.free) {
            std::cout << ", let go at " << body.mass << " kg/m";
            if (body.pinX || body.pinY || body.pinRot) {
                std::cout << ", pinned in";
                if (body.pinX) std::cout << " x";
                if (body.pinY) std::cout << " y";
                if (body.pinRot) std::cout << " rotation";
            }
            std::cout << "\n    the fluid it displaces weighs "
                      << body.addedMass << " kg/m, "
                      << (body.mass > 0.0f ? body.addedMass / body.mass : 0.0f)
                      << " of its own mass\n";
        } else if (!body.keys.empty()) {
            std::cout << ", on a timetable of " << body.keys.size()
                      << " keyframes from " << body.keys.front().time << " to "
                      << body.keys.back().time << " s\n";
        } else {
            std::cout << ", travelling at (" << body.vx << ", " << body.vy
                      << ") m/s, turning " << body.omega / degToRad
                      << " deg/s\n";
        }
    }

    if (bodiesFree) {
        std::cout << "  coupling: " << bodyCouplingName(cfg.bodyCoupling);
        switch (cfg.bodyCoupling) {
        case BodyCoupling::Weak:
            std::cout << " - one force evaluation a step. Stable only while "
                         "the body is a good deal\n    heavier than the fluid "
                         "it pushes aside; under that it oscillates and no "
                         "smaller\n    step fixes it, because the thing "
                         "driving it is the fluid's own inertia.\n";
            break;
        case BodyCoupling::Strong:
            std::cout << " - force and motion iterated up to "
                      << cfg.bodyIterations
                      << " times a step until they agree.\n";
            break;
        default:
            std::cout << " - the fluid that moves with the body is carried on "
                         "the left hand side of its\n    own equation of "
                         "motion, which is where the added-mass instability "
                         "comes from.\n";
            break;
        }
    }

    std::cout << "  the mask is cut again every step, so what used to be a "
                 "setup cost is now a per-step one.\n";
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

    multiphase = cfg.multiphase();
    hasTension = cfg.hasSurfaceTension();
    turbulent = cfg.turbulent();
    if (turbulent) {
        const float molecular =
            multiphase ? std::min(cfg.nu1, cfg.nu2) : cfg.nu;
        const std::size_t cells = static_cast<std::size_t>(nx) * ny;
        if (restartK.size() == cells && restartOmega.size() == cells)
            turbulence.setState(std::move(restartK), std::move(restartOmega));
        turbulence.initialise(cfg, solidMask, nx, ny, dx, dy, molecular);
        nuTurb = &turbulence.viscosity();
    }
    variableViscosity = multiphase || turbulent;
    if (variableViscosity) {
        nuCell.assign(static_cast<size_t>(nx) * ny, cfg.nu);
        nuNode.assign(static_cast<size_t>(nx + 1) * (ny + 1), cfg.nu);
        viscX.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
        viscY.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    }
    if (multiphase) {
        tensionX.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
        tensionY.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    }
    if (multiphase) {
        std::string warning;

        if (phase.fraction().size() != static_cast<size_t>(nx) * ny) {
            phase.initialise(cfg, mesh.solid, dx, dy, warning);
            if (!warning.empty())
                std::cerr << "Warning: " << warning << "\n";
        } else {
            phase.setFluids(cfg.rho1, cfg.rho2,
                            cfg.rho1 * cfg.nu1, cfg.rho2 * cfg.nu2);
            phase.setScheme(cfg.vofScheme);
        }
    }

    resolveWallMotion();
    resolveBodyMotion();
    resolveBoundaries();
    if (turbulent)
        turbulence.setGhosts({sideLeft.ghostSign, sideLeft.ghostOffset},
                             {sideRight.ghostSign, sideRight.ghostOffset},
                             {sideBottom.ghostSign, sideBottom.ghostOffset},
                             {sideTop.ghostSign, sideTop.ghostOffset});
    buildFaceMasks();
    buildSlipFaces();
    buildMirrorFaces();
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
            const float seed = uInletLeft[j];
            for (int i = 1; i < nx; ++i)
                if (uFluidMask[idxU(i, j)])
                    u[idxU(i, j)] = seed;

            if (!solidMask[idxP(0, j)])
                u[idxU(0, j)] = uInletLeft[j];
            if (!solidMask[idxP(nx - 1, j)])
                u[idxU(nx, j)] = uInletLeft[j];
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
    if (multiphase) {
        phase.refreshProperties(solidMask);
        refreshPhaseCoefficients();
        refreshSurfaceTension();
    }
    refreshViscosity();
    buildSources();

    if (bodiesMove) {
        prevSolidMask = solidMask;
        prevUFluidMask = uFluidMask;
        prevVFluidMask = vFluidMask;
        prevObjectId = mesh.objectId;
    }

    applySlipFaces();
    applyMirrorFaces();

    std::cout << "Fields initialized. Multigrid levels: "
              << multigrid.levelCount()
              << ", backend: " << (multigrid.usingCuda() ? "CUDA" : "CPU")
              << "\n";
    reportBodies();
    if (multiphase) {
        std::cout << "Two fluids: 1 is rho " << cfg.rho1 << " kg/m^3, nu "
                  << cfg.nu1 << " m^2/s; 2 is rho " << cfg.rho2 << " kg/m^3, nu "
                  << cfg.nu2 << " m^2/s.\n  Density ratio "
                  << std::max(cfg.rho1, cfg.rho2) / std::min(cfg.rho1, cfg.rho2)
                  << ":1, interface carried by "
                  << vofSchemeName(cfg.vofScheme)
                  << ", pressure solved with 1/rho on every face.\n";
        if (std::max(cfg.rho1, cfg.rho2) / std::min(cfg.rho1, cfg.rho2) > 100.f)
            std::cout << "  note: past a hundred to one the pressure solve "
                         "needs more V-cycles than a single fluid does.\n"
                         "  If the mg column climbs or the run says it ran out "
                         "of cycles, raise mgIterations.\n";
        if (!cfg.gravityEnabled && !cfg.miscible() && !hasTension)
            std::cout << "  note: gravity is off, so the two fluids have no "
                         "reason to separate and nothing here will float.\n"
                         "  gravityEnabled=1 is what makes this a two-phase "
                         "case rather than two dyes.\n";
        if (cfg.miscible()) {
            std::cout << "  They mix: no interface, no surface tension, and "
                         "the composition spreads at "
                      << cfg.diffusivity << " m^2/s as well as being "
                         "carried.\n";
            const float across = std::min(cfg.Lx, cfg.Ly);
            if (cfg.diffusivity > 0.0f)
                std::cout << "  Diffusion alone would cross the domain in "
                          << across * across / (4.0f * cfg.diffusivity)
                          << " s, against a run of " << cfg.totalTime
                          << " s.\n";
        } else if (hasTension) {
            const float smallest = std::min(dx, dy);
            const float laplace = cfg.surfaceTension / (0.1f * cfg.Ly);
            std::cout << "  Surface tension " << cfg.surfaceTension
                      << " N/m, contact angle " << cfg.contactAngle
                      << " deg. A drop a tenth of the domain across holds "
                      << laplace << " Pa more inside than out.\n";
            const float capillary =
                std::sqrt((cfg.rho1 + cfg.rho2) * smallest * smallest *
                          smallest /
                          (4.0f * 3.14159265358979f * cfg.surfaceTension));
            std::cout << "  Shortest capillary wave the grid holds: "
                      << capillary << " s per step.\n";
        }
    }
    if (!multiphase && cfg.U0 > 0.0f && cfg.nu > 0.0f) {
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
    // Diffusion is explicit, so its stability limit does not look at the flow
    // at all: a thick enough fluid pins dt orders of magnitude below the CFL
    // one and the run turns into millions of steps without a single sign that
    // anything is wrong. Both limits go on the screen before that happens.
    const float nuShown = multiphase ? std::max(cfg.nu1, cfg.nu2) : cfg.nu;
    if (nuShown > 0.0f) {
        const float dtDiff = 1.f / (2.f * nuShown * (invDx2 + invDy2));
        const float gravityStep =
            (bodyGravity && (gx != 0.0f || gy != 0.0f))
                ? std::sqrt(std::min(dx, dy) /
                            std::sqrt(gx * gx + gy * gy))
                : dtDiff;
        const float speed = std::fabs(cfg.U0);
        const float dtAdv =
            (speed > 0.0f) ? cfg.CFL / (speed * (invDx + invDy)) : dtDiff;
        const double steps =
            (cfg.totalTime - currentTime) /
            (cfg.dtSafety * std::min(gravityStep, std::min(dtAdv, dtDiff)));

        // steps is unbounded: a domain small enough to overflow invDx2 sends
        // dtDiff to zero, and converting an infinite double to long long is
        // undefined rather than merely wrong
        std::cout << "Time step: viscous limit " << dtDiff
                  << " s, advective limit " << dtAdv << " s -> ";
        if (!(steps < 1e9))
            std::cout << "more steps than this run can ever take";
        else
            std::cout << "about "
                      << static_cast<long long>(std::max(steps, 1.0))
                      << " steps";
        std::cout << " for " << cfg.totalTime - currentTime << " s.\n";

        if (steps < 4.0)
            std::cout << "  note: neither limit bites at this scale, so the "
                         "whole run fits in a couple of steps. That is a\n"
                         "  correct answer to what was asked, but it is not a "
                         "film: ask for more time, or a finer grid.\n";

        if (dtDiff < dtAdv)
            std::cout << "  note: viscosity sets dt here, not the flow, and "
                         "that limit falls with dx^2:\n"
                         "  halving the cell size quadruples the step count. A "
                         "coarser grid is the only\n"
                         "  lever this solver has, the diffusion term is "
                         "explicit.\n";

        if (steps > 1e6)
            std::cout << "  note: that is past a million steps. Cut totalTime, "
                         "coarsen the grid, or expect\n"
                         "  the run to take hours.\n";
    }
    if (cfg.gravityEnabled && multiphase) {
        const float heavy = std::max(cfg.rho1, cfg.rho2);
        const float light = std::min(cfg.rho1, cfg.rho2);
        std::cout << "Gravity: " << cfg.gravityAccel << " m/s^2 at "
                  << cfg.gravityAngle << " deg -> g = (" << gx << ", " << gy
                  << ") m/s^2.\n"
                     "  Two densities, so this is the force that moves the "
                     "fluid and not a head added on\n  output: it is in the "
                     "predictor, p is the real pressure in pascals, and what "
                     "the\n  interface does is the difference of "
                  << heavy - light << " kg/m^3 falling through the rest.\n";
    }
    if (cfg.gravityEnabled && !multiphase) {
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
    // maxps hands back its second operand when either is a NaN and std::max
    // drops it the same way, so a field that has stopped being a number would
    // sail straight through the reduction. It is caught on the way past instead.
    bool notANumber = false;

    #pragma omp parallel
    {
#ifdef __AVX2__
        __m256 localVec = _mm256_setzero_ps();
        __m256 localNan = _mm256_setzero_ps();
#endif
        float localScalar = 0.0f;
        bool localNotANumber = false;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j)
        {
            const int rowU = j * (nx + 1);
            const int rowV = j * nx;
            const int rowVTop = (j + 1) * nx;

            int i = 0;
#ifdef __AVX2__
            for (; runtime::avx2 && i + 8 <= nx; i += 8)
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
                localNan = _mm256_or_ps(
                    localNan,
                    _mm256_cmp_ps(courant, courant, _CMP_UNORD_Q));
            }
#endif

            for (; i < nx; ++i)
            {
                const float maxU = std::max(std::fabs(u[rowU + i]),
                                            std::fabs(u[rowU + i + 1]));
                const float maxV = std::max(std::fabs(v[rowV + i]),
                                            std::fabs(v[rowVTop + i]));
                const float courant = maxU * invDx + maxV * invDy;
                localScalar = std::max(localScalar, courant);
                localNotANumber = localNotANumber || !(courant == courant);
            }
        }

#ifdef __AVX2__
        const float localMax = std::max(horizontalMax(localVec), localScalar);
        localNotANumber =
            localNotANumber || (_mm256_movemask_ps(localNan) != 0);
#else
        const float localMax = localScalar;
#endif
        #pragma omp critical
        {
            maxCourant = std::max(maxCourant, localMax);
            notANumber = notANumber || localNotANumber;
        }
    }

    for (const RigidBody& body : bodies) {
        if (!body.everFree && !body.prescribed)
            continue;
        const float surface = std::hypot(body.vx, body.vy) +
                              std::fabs(body.omega) * body.radius;
        maxCourant = std::max(maxCourant, surface * std::max(invDx, invDy));
    }

    const float dtAdv =
        (maxCourant < 1e-12f) ?
        1e9f :
        cfg.CFL / maxCourant;

    const float nuLimit =
        multiphase ? std::max(cfg.nu1, cfg.nu2) : cfg.nu;
    const float dtDiff =
        (nuLimit > 0.0f) ?
        1.f / (2.f * nuLimit * (invDx2 + invDy2)) :
        1e9f;

    dt = cfg.dtSafety * std::min(dtAdv, dtDiff);

    if (multiphase) {
        const float interfaceRate = phase.maxCourant(u, v, dx, dy);
        if (interfaceRate > 1e-12f)
            dt = std::min(dt, cfg.dtSafety * 0.5f / interfaceRate);
    }

    if (hasTension) {
        const float smallest = std::min(dx, dy);
        const float mass = cfg.rho1 + cfg.rho2;
        const float capillary =
            std::sqrt(mass * smallest * smallest * smallest /
                      (4.0f * 3.14159265358979f * cfg.surfaceTension));
        if (capillary < dt) {
            dt = cfg.dtSafety * capillary;
            if (!capillaryReported) {
                capillaryReported = true;
                std::cout << "  note: surface tension sets the step size here, "
                             "not the flow and not the viscosity.\n  The "
                             "shortest capillary wave this grid can hold "
                             "crosses a cell in " << capillary
                          << " s, and that limit falls as dx^1.5:\n  halving "
                             "the cell size costs about three times the steps. "
                             "It is not the solver hanging.\n";
            }
        }
    }

    if (cfg.miscible() && cfg.diffusivity > 0.0f) {
        dt = std::min(dt, cfg.dtSafety /
                              (2.0f * cfg.diffusivity * (invDx2 + invDy2)));
    }

    if (bodyGravity) {
        const float g = std::sqrt(gx * gx + gy * gy);
        if (g > 1e-12f)
            dt = std::min(dt,
                          cfg.dtSafety * std::sqrt(std::min(dx, dy) / g));
    }

    if (turbulent) {
        const float peak = turbulence.peakViscosity();
        if (peak > 0.0f)
            dt = std::min(dt, cfg.dtSafety /
                                  (2.0f * (nuLimit + peak) *
                                   (invDx2 + invDy2)));

        const float sources = turbulence.sourceStepLimit();
        if (sources > 0.0f && cfg.dtSafety * sources < dt) {
            dt = cfg.dtSafety * sources;
            if (!turbulenceReported) {
                turbulenceReported = true;
                std::cout << "  note: the k-omega source terms set the step "
                             "size here, not the flow.\n  omega is 60 nu / "
                             "(beta1 d^2) at a wall, so the first cell off it "
                             "decides, and that\n  limit falls as dx^2. It is "
                             "not the solver hanging.\n";
            }
        }
    }

    // A field that has stopped being a number has no step size in it, and the
    // old fallback of 1e-6 was a number with no relation to the grid, the fluid
    // or the run: it kept grinding out frames of NaN until the check that only
    // runs every tenth step happened to look. Same for a dt that came out zero
    // or infinite, which means the grid or the viscosity are outside what a
    // float can express, not that 1e-6 is a good idea.
    if (notANumber || !std::isfinite(maxCourant)) {
        if (fieldBroken)
            return;
        fieldBroken = true;
        std::cerr << "The velocity field is no longer a number at step " << step
                  << ", so no step size can be read out of it. Stopping.\n";
        return;
    }
    if (!(dt > 0.f) || !std::isfinite(dt)) {
        if (fieldBroken)
            return;
        fieldBroken = true;
        std::cerr << "The time step came out as " << dt << " at step " << step
                  << ". The grid spacing or the viscosity are outside the range"
                     " a float can\n  hold: dx = " << dx << ", dy = " << dy
                  << ", nu = " << cfg.nu << ". Stopping.\n";
        return;
    }
}

template <bool TwoPhase, bool VarVisc>
void Solver::predictorByScheme() {
    switch (cfg.convection) {
    case ConvectionScheme::Central:
        predictorImpl<PhiCentral, TwoPhase, VarVisc>();
        return;
    case ConvectionScheme::Muscl:
        switch (cfg.limiter) {
        case LimiterKind::Minmod:
            predictorImpl<PhiMinmod, TwoPhase, VarVisc>();
            return;
        case LimiterKind::Superbee:
            predictorImpl<PhiSuperbee, TwoPhase, VarVisc>();
            return;
        default:
            predictorImpl<PhiVanLeer, TwoPhase, VarVisc>();
            return;
        }
    default: break;
    }
    predictorImpl<PhiUpwind, TwoPhase, VarVisc>();
}

void Solver::refreshViscosity() {
    if (!variableViscosity)
        return;
    const int nx = cfg.nx, ny = cfg.ny;

    const float* __restrict mu = multiphase ? phase.viscosity().data() : nullptr;
    const float* __restrict rho = multiphase ? phase.density().data() : nullptr;

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i) {
            const int id = row + i;
            const float molecular =
                multiphase ? mu[id] / std::max(1e-20f, rho[id]) : cfg.nu;
            nuCell[id] = molecular + (turbulent ? (*nuTurb)[id] : 0.0f);
        }
    }

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            float sum = 0.0f;
            int count = 0;
            for (int dj = -1; dj <= 0; ++dj)
                for (int di = -1; di <= 0; ++di) {
                    const int cj = j + dj, ci = i + di;
                    if (cj < 0 || cj >= ny || ci < 0 || ci >= nx)
                        continue;
                    sum += nuCell[cj * nx + ci];
                    ++count;
                }
            nuNode[j * (nx + 1) + i] = count > 0 ? sum / count : cfg.nu;
        }
    }
}

void Solver::computeViscousStress() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    const float* __restrict nuC = nuCell.data();
    const float* __restrict nuN = nuNode.data();
    float* __restrict outX = viscX.data();
    float* __restrict outY = viscY.data();

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j < ny; ++j) {
        const int rowU = j * (nx + 1);
        const int row = j * nx;
        for (int i = 1; i < nx; ++i) {
            const int id = rowU + i;
            const float dudxRight =
                (uPtr[id + 1] - uPtr[id]) * invDx;
            const float dudxLeft =
                (uPtr[id] - uPtr[id - 1]) * invDx;
            const float normal =
                2.0f * (nuC[row + i] * dudxRight -
                        nuC[row + i - 1] * dudxLeft) * invDx;

            const int nodeBottom = j * (nx + 1) + i;
            const int nodeTop = (j + 1) * (nx + 1) + i;
            const float uAbove =
                (j + 1 < ny) ? uPtr[id + (nx + 1)]
                             : sideTop.ghostSign * uPtr[id] +
                                   sideTop.ghostOffset;
            const float uBelow =
                (j > 0) ? uPtr[id - (nx + 1)]
                        : sideBottom.ghostSign * uPtr[id] +
                              sideBottom.ghostOffset;
            const float shearTop =
                (uAbove - uPtr[id]) * invDy +
                (vPtr[(j + 1) * nx + i] - vPtr[(j + 1) * nx + i - 1]) * invDx;
            const float shearBottom =
                (uPtr[id] - uBelow) * invDy +
                (vPtr[row + i] - vPtr[row + i - 1]) * invDx;
            const float cross =
                (nuN[nodeTop] * shearTop - nuN[nodeBottom] * shearBottom) *
                invDy;

            outX[id] = normal + cross;
        }
        outX[rowU] = 0.0f;
        outX[rowU + nx] = 0.0f;
    }

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 1; j < ny; ++j) {
        const int rowV = j * nx;
        for (int i = 0; i < nx; ++i) {
            const int id = rowV + i;
            const float dvdyTop =
                (vPtr[id + nx] - vPtr[id]) * invDy;
            const float dvdyBottom =
                (vPtr[id] - vPtr[id - nx]) * invDy;
            const float normal =
                2.0f * (nuC[rowV + i] * dvdyTop -
                        nuC[rowV - nx + i] * dvdyBottom) * invDy;

            const int nodeLeft = j * (nx + 1) + i;
            const int nodeRight = j * (nx + 1) + i + 1;
            const float vRight =
                (i + 1 < nx) ? vPtr[id + 1]
                             : sideRight.ghostSign * vPtr[id] +
                                   sideRight.ghostOffset;
            const float vLeft =
                (i > 0) ? vPtr[id - 1]
                        : sideLeft.ghostSign * vPtr[id] +
                              sideLeft.ghostOffset;
            const float shearRight =
                (vRight - vPtr[id]) * invDx +
                (uPtr[j * (nx + 1) + i + 1] -
                 uPtr[(j - 1) * (nx + 1) + i + 1]) * invDy;
            const float shearLeft =
                (vPtr[id] - vLeft) * invDx +
                (uPtr[j * (nx + 1) + i] -
                 uPtr[(j - 1) * (nx + 1) + i]) * invDy;
            const float cross =
                (nuN[nodeRight] * shearRight - nuN[nodeLeft] * shearLeft) *
                invDx;

            outY[id] = normal + cross;
        }
    }
    std::fill(viscY.begin(), viscY.begin() + nx, 0.0f);
    std::fill(viscY.end() - nx, viscY.end(), 0.0f);
}

void Solver::predictor() {
    if (variableViscosity)
        computeViscousStress();
    if (multiphase)
        predictorByScheme<true, true>();
    else if (variableViscosity)
        predictorByScheme<false, true>();
    else
        predictorByScheme<false, false>();
}

template <int Phi, bool TwoPhase, bool VarVisc>
void Solver::predictorImpl() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float dtNu = dt * cfg.nu;
    const float dtConv = dt;

    const float* __restrict viscXPtr = VarVisc ? viscX.data() : nullptr;
    const float* __restrict viscYPtr = VarVisc ? viscY.data() : nullptr;

    const float* __restrict tensX = TwoPhase ? tensionX.data() : nullptr;
    const float* __restrict tensY = TwoPhase ? tensionY.data() : nullptr;
    const float bodyGx = bodyGravity ? dt * gx : 0.0f;
    const float bodyGy = bodyGravity ? dt * gy : 0.0f;
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
    const __m256 dtVec     = _mm256_set1_ps(dt);
    const __m256 bodyGxVec = _mm256_set1_ps(bodyGx);
    const __m256 bodyGyVec = _mm256_set1_ps(bodyGy);
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
        for (; runtime::avx2 && i + 8 <= nx; i += 8) {
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
            const __m256 backX = _mm256_sub_ps(uij, uleft);
            const __m256 fwdX = _mm256_sub_ps(uright, uij);
            const __m256 towardsX = _mm256_cmp_ps(uij, zero, _CMP_GT_OS);
            const __m256 dudx =
                _mm256_mul_ps(
                    limitedDeltaVec<Phi>(
                        _mm256_blendv_ps(fwdX, backX, towardsX),
                        _mm256_blendv_ps(backX, fwdX, towardsX)),
                    invDxVec);

            const __m256 backY = _mm256_sub_ps(uij, ubot);
            const __m256 fwdY = _mm256_sub_ps(utop, uij);
            const __m256 towardsY = _mm256_cmp_ps(vn, zero, _CMP_GT_OS);
            const __m256 dudy =
                _mm256_mul_ps(
                    limitedDeltaVec<Phi>(
                        _mm256_blendv_ps(fwdY, backY, towardsY),
                        _mm256_blendv_ps(backY, fwdY, towardsY)),
                    invDyVec);
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
            __m256 viscVec = _mm256_mul_ps(dtNuVec, diff);
            if constexpr (VarVisc)
                viscVec = _mm256_mul_ps(
                    dtVec, _mm256_loadu_ps(viscXPtr + rowU + i));
            __m256 forceVec = bodyGxVec;
            if constexpr (TwoPhase)
                forceVec = _mm256_add_ps(
                    forceVec,
                    _mm256_mul_ps(dtVec, _mm256_loadu_ps(tensX + rowU + i)));
            const __m256 res =
                _mm256_add_ps(
                    _mm256_add_ps(
                        _mm256_sub_ps(
                            uij,
                            _mm256_mul_ps(dtConvVec, conv)),
                        viscVec),
                    forceVec);
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

            const float backX = u_ij - u_left;
            const float fwdX = u_right - u_ij;
            const float dudx =
                ((u_ij > 0.f) ? limitedDelta<Phi>(backX, fwdX)
                              : limitedDelta<Phi>(fwdX, backX)) * invDx;

            const float backY = u_ij - u_bot;
            const float fwdY = u_top - u_ij;
            const float dudy =
                ((v_n > 0.f) ? limitedDelta<Phi>(backY, fwdY)
                             : limitedDelta<Phi>(fwdY, backY)) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + (VarVisc ? dt * viscXPtr[rowU + i]
                           : dtNu * (d2udx2 + d2udy2))
                + bodyGx + (TwoPhase ? dt * tensX[rowU + i] : 0.0f))
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
            const float u_bot = (pass == 0)
                ? sideBottom.ghostSign * u_ij + sideBottom.ghostOffset
                : uOther[i];
            const float u_top = (pass == 0)
                ? uOther[i]
                : sideTop.ghostSign * u_ij + sideTop.ghostOffset;

            const float backX = u_ij - u_left;
            const float fwdX = u_right - u_ij;
            const float dudx =
                ((u_ij > 0.f) ? limitedDelta<Phi>(backX, fwdX)
                              : limitedDelta<Phi>(fwdX, backX)) * invDx;

            const float backY = u_ij - u_bot;
            const float fwdY = u_top - u_ij;
            const float dudy =
                ((v_n > 0.f) ? limitedDelta<Phi>(backY, fwdY)
                             : limitedDelta<Phi>(fwdY, backY)) * invDy;

            const float d2udx2 =
                (u_right - 2.f*u_ij + u_left) * invDx2;

            const float d2udy2 =
                (u_top - 2.f*u_ij + u_bot) * invDy2;

            uStarRow[i] = uMask[i] * (
                u_ij
                - dtConv * (u_ij*dudx + v_n*dudy)
                + (VarVisc ? dt * viscXPtr[rowU + i]
                           : dtNu * (d2udx2 + d2udy2))
                + bodyGx + (TwoPhase ? dt * tensX[rowU + i] : 0.0f))
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
        for (; runtime::avx2 && i + 8 <= nx - 1; i += 8) {
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
            const __m256 backX = _mm256_sub_ps(vij, vleft);
            const __m256 fwdX = _mm256_sub_ps(vright, vij);
            const __m256 towardsX = _mm256_cmp_ps(ue, zero, _CMP_GT_OS);
            const __m256 dvdx =
                _mm256_mul_ps(
                    limitedDeltaVec<Phi>(
                        _mm256_blendv_ps(fwdX, backX, towardsX),
                        _mm256_blendv_ps(backX, fwdX, towardsX)),
                    invDxVec);

            const __m256 backY = _mm256_sub_ps(vij, vbot);
            const __m256 fwdY = _mm256_sub_ps(vtop, vij);
            const __m256 towardsY = _mm256_cmp_ps(vij, zero, _CMP_GT_OS);
            const __m256 dvdy =
                _mm256_mul_ps(
                    limitedDeltaVec<Phi>(
                        _mm256_blendv_ps(fwdY, backY, towardsY),
                        _mm256_blendv_ps(backY, fwdY, towardsY)),
                    invDyVec);
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
            __m256 viscVec = _mm256_mul_ps(dtNuVec, diff);
            if constexpr (VarVisc)
                viscVec = _mm256_mul_ps(
                    dtVec, _mm256_loadu_ps(viscYPtr + rowV + i));
            __m256 forceVec = bodyGyVec;
            if constexpr (TwoPhase)
                forceVec = _mm256_add_ps(
                    forceVec,
                    _mm256_mul_ps(dtVec, _mm256_loadu_ps(tensY + rowV + i)));
            const __m256 res =
                _mm256_add_ps(
                    _mm256_add_ps(
                        _mm256_sub_ps(
                            vij,
                            _mm256_mul_ps(dtConvVec, conv)),
                        viscVec),
                    forceVec);
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

            const float backX = v_ij - v_left;
            const float fwdX = v_right - v_ij;
            const float dvdx =
                ((u_e > 0.f) ? limitedDelta<Phi>(backX, fwdX)
                             : limitedDelta<Phi>(fwdX, backX)) * invDx;

            const float backY = v_ij - v_bot;
            const float fwdY = v_top - v_ij;
            const float dvdy =
                ((v_ij > 0.f) ? limitedDelta<Phi>(backY, fwdY)
                              : limitedDelta<Phi>(fwdY, backY)) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[i] = vMask[i] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + (VarVisc ? dt * viscYPtr[rowV + i]
                           : dtNu * (d2vdx2 + d2vdy2))
                + bodyGy + (TwoPhase ? dt * tensY[rowV + i] : 0.0f))
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

            const float v_left  = (iCol == 0)
                ? sideLeft.ghostSign * v_ij + sideLeft.ghostOffset
                : vRow[iCol-1];
            const float v_right = (iCol == nx - 1)
                ? sideRight.ghostSign * v_ij + sideRight.ghostOffset
                : vRow[iCol+1];
            const float v_bot   = vBot[iCol];
            const float v_top   = vTop[iCol];

            const float backX = v_ij - v_left;
            const float fwdX = v_right - v_ij;
            const float dvdx =
                ((u_e > 0.f) ? limitedDelta<Phi>(backX, fwdX)
                             : limitedDelta<Phi>(fwdX, backX)) * invDx;

            const float backY = v_ij - v_bot;
            const float fwdY = v_top - v_ij;
            const float dvdy =
                ((v_ij > 0.f) ? limitedDelta<Phi>(backY, fwdY)
                              : limitedDelta<Phi>(fwdY, backY)) * invDy;

            const float d2vdx2 =
                (v_right - 2.f*v_ij + v_left) * invDx2;

            const float d2vdy2 =
                (v_top - 2.f*v_ij + v_bot) * invDy2;

            vStarRow[iCol] = vMask[iCol] * (
                v_ij
                - dtConv * (u_e*dvdx + v_ij*dvdy)
                + (VarVisc ? dt * viscYPtr[rowV + iCol]
                           : dtNu * (d2vdx2 + d2vdy2))
                + bodyGy + (TwoPhase ? dt * tensY[rowV + iCol] : 0.0f))
                + vWallRow[iCol];
        }
    }

    applyBoundaryVelocities(u_star, v_star, true);
}

void Solver::advanceStage() {
    applySlipFaces();
    applyMirrorFaces();
    predictor();
    applySources();
    solvePoisson();
    corrector();
}

bool Solver::steadyReached() {
    if (!(cfg.steadyTolerance > 0.0f))
        return false;

    if (uSteady.size() != u.size()) {
        uSteady = u;
        vSteady = v;
        steadyStamp = currentTime;
        return false;
    }

    const double elapsed = currentTime - steadyStamp;
    if (!(elapsed > 0.0))
        return false;

    float worst = 0.0f;
    for (size_t id = 0; id < u.size(); ++id)
        worst = std::max(worst, std::fabs(u[id] - uSteady[id]));
    for (size_t id = 0; id < v.size(); ++id)
        worst = std::max(worst, std::fabs(v[id] - vSteady[id]));

    float driving = std::fabs(cfg.U0);
    for (int side = 0; side < 4; ++side) {
        const BoundarySpec& spec = cfg.boundaries.side[side];
        if (spec.kind == BoundaryKind::MovingWall || spec.speedSet)
            driving = std::max(driving, std::fabs(spec.speed));
    }
    if (!(driving > 0.0f))
        driving = 1.0f;

    steadyRate = static_cast<float>(worst / elapsed) / driving;

    uSteady = u;
    vSteady = v;
    steadyStamp = currentTime;
    return steadyRate < cfg.steadyTolerance;
}

void Solver::blendWithPrevious(float weightPrevious) {
    const float weightNow = 1.0f - weightPrevious;
    const int uCount = static_cast<int>(u.size());
    const int vCount = static_cast<int>(v.size());

    #pragma omp parallel for schedule(static)
    for (int id = 0; id < uCount; ++id)
        u[id] = weightPrevious * uPrev[id] + weightNow * u[id];

    #pragma omp parallel for schedule(static)
    for (int id = 0; id < vCount; ++id)
        v[id] = weightPrevious * vPrev[id] + weightNow * v[id];

    applyBC();
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
        for (; runtime::avx2 && i + 8 <= nx; i += 8){
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

    if (hasSources) {
        const float* __restrict rate = sourceRate.data();

        for (int id : sourceCells) {
            const float before = rhsPtr[id];
            const float after = before - rate[id] * invDt * cellMask[id];
            rhsPtr[id] = after;
            rhsSqSum += static_cast<double>(after) * after -
                        static_cast<double>(before) * before;
        }
    }

    if (bodyGravity) {
        const float twoX = 2.f * invDx2;
        const float twoY = 2.f * invDy2;
        const float* __restrict invRhoXPtr =
            multiphase ? phase.faceInvRhoX().data() : nullptr;
        const float* __restrict invRhoYPtr =
            multiphase ? phase.faceInvRhoY().data() : nullptr;
        const auto wX = [&](int i, int j) {
            return invRhoXPtr ? invRhoXPtr[idxU(i, j)] : 1.0f;
        };
        const auto wY = [&](int i, int j) {
            return invRhoYPtr ? invRhoYPtr[idxV(i, j)] : 1.0f;
        };
        if (sideLeft.outlet)
            for (int j = 0; j < ny; ++j)
                rhsPtr[idxP(0, j)] -= twoX * wX(0, j) *
                                      phiOutside(BoundarySide::Left, j) *
                                      cellMask[idxP(0, j)];
        if (sideRight.outlet)
            for (int j = 0; j < ny; ++j)
                rhsPtr[idxP(nx - 1, j)] -=
                    twoX * wX(nx, j) * phiOutside(BoundarySide::Right, j) *
                    cellMask[idxP(nx - 1, j)];
        if (sideBottom.outlet)
            for (int i = 0; i < nx; ++i)
                rhsPtr[idxP(i, 0)] -= twoY * wY(i, 0) *
                                      phiOutside(BoundarySide::Bottom, i) *
                                      cellMask[idxP(i, 0)];
        if (sideTop.outlet)
            for (int i = 0; i < nx; ++i)
                rhsPtr[idxP(i, ny - 1)] -=
                    twoY * wY(i, ny) * phiOutside(BoundarySide::Top, i) *
                    cellMask[idxP(i, ny - 1)];
        rhsSqSum = 0.0;
        #pragma omp parallel for schedule(static) reduction(+ : rhsSqSum)
        for (int j = 0; j < ny; ++j) {
            double rowSum = 0.0;
            for (int i = 0; i < nx; ++i) {
                const float value = rhsPtr[j * nx + i];
                rowSum += static_cast<double>(value) * value;
            }
            rhsSqSum += rowSum;
        }
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

    // The opening transient starts from a field the previous step did not
    // produce, so the first steps run out of cycles on almost every case and
    // say nothing at all. What matters is running out long after that, or on
    // most of the run: then the velocity field carries a divergence of that
    // size the whole way and nothing but the mg column ever said so.
    if (lastResidual > cfg.mgTolerance && multigrid.cyclesUsed() >= cycles) {
        ++poissonShortSteps;
        poissonWorstResidual = std::max(poissonWorstResidual, lastResidual);
        if (!poissonShortReported && step > 20 &&
            lastResidual > 10.f * cfg.mgTolerance) {
            poissonShortReported = true;
            std::cout << "  note: the pressure solve used all " << cycles
                      << " V-cycles at step " << step
                      << " and stopped at a relative residual of "
                      << lastResidual << ", against mgTolerance "
                      << cfg.mgTolerance << ".\n"
                         "  The velocity field keeps a divergence of that "
                         "order until it converges. Raise mgIterations if the "
                         "div column\n  in the step lines stays large.\n";
        }
    }
}

void Solver::corrector() {
    if (multiphase)
        correctorImpl<true>();
    else
        correctorImpl<false>();
}

template <bool TwoPhase>
void Solver::correctorImpl() {
    const int nx = cfg.nx, ny = cfg.ny;
    const float* __restrict invRhoX =
        TwoPhase ? phase.faceInvRhoX().data() : nullptr;
    const float* __restrict invRhoY =
        TwoPhase ? phase.faceInvRhoY().data() : nullptr;
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
        for (; runtime::avx2 && i + 8 <= nx; i += 8){
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
            __m256 scale = dtVec;
            if constexpr (TwoPhase)
                scale = _mm256_mul_ps(dtVec,
                                      _mm256_loadu_ps(invRhoX + rowU + i));
            const __m256 res =
                _mm256_sub_ps(
                    uS,
                    _mm256_mul_ps(scale, dpdx));
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
                - dt * (TwoPhase ? invRhoX[rowU + i] : 1.0f) *
                      (pPtr[rowP + i] - pPtr[rowP + i - 1]) * invDx;

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
        for (; runtime::avx2 && i + 8 <= nx; i += 8){
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
            __m256 scale = dtVec;
            if constexpr (TwoPhase)
                scale = _mm256_mul_ps(dtVec,
                                      _mm256_loadu_ps(invRhoY + rowV + i));
            const __m256 res =
                _mm256_sub_ps(
                    vS,
                    _mm256_mul_ps(scale, dpdy));
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
                - dt * (TwoPhase ? invRhoY[rowV + i] : 1.0f) *
                      (pPtr[rowP + i] - pPtr[rowPBot + i]) * invDy;

            vPtr[rowV + i] = vMask[i] * res + vWallRow[i];
        }
    }

    applyOutletFaces();
    applyBC();
}

void Solver::applyBC() {
    applyBoundaryVelocities(u, v, false);
}

float Solver::maxDivergence() const {
    const int nx = cfg.nx, ny = cfg.ny;

    const float* __restrict rate = hasSources ? sourceRate.data() : nullptr;
    const float* __restrict mask = fluidCellMaskF.data();
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float worst = 0.0f;
    int bad = 0;

    #pragma omp parallel reduction(+ : bad) if (nx * ny >= 8192)
    {
        float local = 0.0f;
        int localBad = 0;
#ifdef __AVX2__
        __m256 localVec = _mm256_setzero_ps();
        __m256 badVec = _mm256_setzero_ps();
        const __m256 invDxVec = _mm256_set1_ps(invDx);
        const __m256 invDyVec = _mm256_set1_ps(invDy);
        const __m256 signMask = absMask();
#endif
        #pragma omp for schedule(static) nowait
        for (int j = 0; j < ny; ++j) {
            const int rowP = j * nx;
            const int rowU = j * (nx + 1);
            const int rowVTop = (j + 1) * nx;
            int i = 0;
#ifdef __AVX2__
            for (; runtime::avx2 && i + 8 <= nx; i += 8) {
                __m256 div = _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_sub_ps(_mm256_loadu_ps(uPtr + rowU + i + 1),
                                      _mm256_loadu_ps(uPtr + rowU + i)),
                        invDxVec),
                    _mm256_mul_ps(
                        _mm256_sub_ps(_mm256_loadu_ps(vPtr + rowVTop + i),
                                      _mm256_loadu_ps(vPtr + rowP + i)),
                        invDyVec));
                if (rate)
                    div = _mm256_sub_ps(div, _mm256_loadu_ps(rate + rowP + i));
                badVec = _mm256_or_ps(
                    badVec, _mm256_cmp_ps(div, div, _CMP_UNORD_Q));

                localVec = _mm256_max_ps(
                    localVec,
                    _mm256_mul_ps(_mm256_and_ps(signMask, div),
                                  _mm256_loadu_ps(mask + rowP + i)));
            }
#endif
            for (; i < nx; ++i) {
                if (solidMask[rowP + i])
                    continue;
                const float div =
                    (uPtr[rowU + i + 1] - uPtr[rowU + i]) * invDx +
                    (vPtr[rowVTop + i] - vPtr[rowP + i]) * invDy -
                    (rate ? rate[rowP + i] : 0.0f);
                if (!std::isfinite(div))
                    ++localBad;
                local = std::max(local, std::fabs(div));
            }
        }
#ifdef __AVX2__
        local = std::max(local, horizontalMax(localVec));
        if (_mm256_movemask_ps(badVec) != 0)
            ++localBad;
#endif
        bad += localBad;
        #pragma omp critical
        worst = std::max(worst, local);
    }

    if (bad != 0)
        return std::numeric_limits<float>::quiet_NaN();
    return worst;
}

float Solver::maxVelocity() const {
    float maxVel = 0.0f;
    int nonFinite = 0;

    const float* const uValues = u.data();
    const float* const vValues = v.data();
    const int uCount = static_cast<int>(u.size());
    const int vCount = static_cast<int>(v.size());
    int uStart = 0;
    int vStart = 0;

#ifdef __AVX2__
    if (runtime::avx2) {
        const __m256 signMask = absMask();
        const __m256 infVec =
            _mm256_set1_ps(std::numeric_limits<float>::infinity());

        #pragma omp parallel reduction(+ : nonFinite) \
                if (uCount + vCount >= 8192)
        {
            __m256 localVec = _mm256_setzero_ps();
            __m256 localBad = _mm256_setzero_ps();

            #pragma omp for schedule(static) nowait
            for (int i = 0; i <= uCount - 8; i += 8) {
                const __m256 magnitude =
                    _mm256_and_ps(signMask, _mm256_loadu_ps(uValues + i));
                localBad = _mm256_or_ps(localBad,
                    _mm256_cmp_ps(magnitude, infVec, _CMP_NLT_UQ));
                localVec = _mm256_max_ps(localVec, magnitude);
            }

            #pragma omp for schedule(static) nowait
            for (int i = 0; i <= vCount - 8; i += 8) {
                const __m256 magnitude =
                    _mm256_and_ps(signMask, _mm256_loadu_ps(vValues + i));
                localBad = _mm256_or_ps(localBad,
                    _mm256_cmp_ps(magnitude, infVec, _CMP_NLT_UQ));
                localVec = _mm256_max_ps(localVec, magnitude);
            }

            nonFinite += (_mm256_movemask_ps(localBad) != 0) ? 1 : 0;
            #pragma omp critical
            {
                maxVel = std::max(maxVel, horizontalMax(localVec));
            }
        }
        uStart = (uCount / 8) * 8;
        vStart = (vCount / 8) * 8;
    }
#endif

    for (int i = uStart; i < uCount; ++i) {
        if (!std::isfinite(uValues[i]))
            ++nonFinite;
        else
            maxVel = std::max(maxVel, std::fabs(uValues[i]));
    }
    for (int i = vStart; i < vCount; ++i) {
        if (!std::isfinite(vValues[i]))
            ++nonFinite;
        else
            maxVel = std::max(maxVel, std::fabs(vValues[i]));
    }

    return nonFinite ? std::numeric_limits<float>::quiet_NaN() : maxVel;
}

void Solver::projectRestartState() {
    const int nx = cfg.nx, ny = cfg.ny;

    // The reconstruction from cell averages fills every face, including the
    // ones held shut against a wall, and those carry whatever the average
    // beside them happened to be. Left in, the Poisson solve accounts for that
    // flow through the wall and the corrector then wipes the same faces to the
    // wall value, putting the divergence straight back: the projection came out
    // at div = 3.9 and no number of V-cycles moved it, because nothing about it
    // was a convergence problem. Masking first is what the predictor does every
    // step, and it is what makes this a projection of a legal field.
    for (size_t id = 0; id < u.size(); ++id)
        u_star[id] = uFluidMaskF[id] * u[id] + uWall[id];
    for (size_t id = 0; id < v.size(); ++id)
        v_star[id] = vFluidMaskF[id] * v[id] + vWall[id];

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

    const int dtUpdateInterval =
        bodyGravity ? 1 : std::max(1, cfg.dtUpdateInterval);
    if (bodyGravity && cfg.dtUpdateInterval > 1)
        std::cout << "  note: the step size is recomputed every step rather "
                     "than every " << cfg.dtUpdateInterval
                  << ", because gravity is\n  in the solve and the field it "
                     "is accelerating is a different one every time.\n";

    // The tray icon and the taskbar bar, or - where there is no tray a console
    // binary can reach - the terminal title. It reports simulated seconds
    // rather than steps, because dt moves and steps do not mean anything to
    // somebody waiting for the run to end.
    progress::begin("Fluid Solver", currentTime, cfg.totalTime,
                    pathToConsole(outputPath));
    bool stopped = false;
    bool diverged = false;
    bool steady = false;

    while (currentTime < cfg.totalTime) {
        if (step % dtUpdateInterval == 0)
            computeDt();

        // How the last step of a run is chosen, and why it is not simply
        // "whatever is left".
        //
        // The projection builds its right-hand side as the divergence left by
        // the predictor divided by the step being taken, so the pressure it
        // solves for scales as 1/dt. Over a normal step that is fine: the
        // divergence is itself proportional to dt and the two cancel. Over a
        // step a millionth of dt it does not: what is left is the previous
        // step's round-off, and dividing that by almost nothing writes a
        // pressure field millions of times larger than every frame before it.
        // The velocities stay right - the correction is dt * grad(p), and the
        // dt cancels again - but that one frame then set the colour range for
        // the whole series and made everything else render flat. That is what
        // "the pressure comes out very strange" was.
        //
        // Such steps were unavoidable before, because currentTime is a double
        // adding up float-sized steps: after "the last step" it lands a few
        // parts in 1e10 short of totalTime rather than on it, and the loop
        // dutifully went round again for the remainder. Twice.
        //
        // So: anything left under a thousandth of a step is round-off rather
        // than simulation and ends the run; a remainder between one and one
        // and a half steps is split in two so neither half is tiny; and the
        // step that does land on totalTime says so exactly instead of leaving
        // the sliver behind for the next iteration to find.
        const double remaining = cfg.totalTime - currentTime;
        if (remaining <= 1e-3 * static_cast<double>(dt))
            break;

        float stepDt = dt;
        bool lastStep = false;
        if (remaining <= static_cast<double>(stepDt)) {
            stepDt = static_cast<float>(remaining);
            lastStep = true;
        } else if (remaining < 2.0 * static_cast<double>(stepDt)) {
            // Halving the last stretch beats a full step followed by whatever
            // is left. The Poisson right-hand side is div/dt, so a step of
            // nanoseconds writes a frame whose pressure map is scaled by
            // millions; this way no step is ever shorter than half of dt.
            stepDt = static_cast<float>(0.5 * remaining);
        }
        if (!(stepDt > 0.f) || fieldBroken)
            break;
        const float savedDt = dt;
        dt = stepDt;

        const auto runStage = [this]() {
            switch (cfg.timeScheme) {
            case TimeScheme::RK2:
                uPrev = u;
                vPrev = v;
                advanceStage();
                advanceStage();
                blendWithPrevious(0.5f);
                break;
            case TimeScheme::RK3:
                uPrev = u;
                vPrev = v;
                advanceStage();
                advanceStage();
                blendWithPrevious(0.75f);
                advanceStage();
                blendWithPrevious(1.0f / 3.0f);
                break;
            default:
                advanceStage();
                break;
            }
        };

        if (!bodiesMove) {
            runStage();
        } else if (!bodiesFree ||
                   cfg.bodyCoupling != BodyCoupling::Strong) {
            bodyForces();
            advanceBodies(stepDt);
            refreshGeometry();
            runStage();
        } else {
            const std::vector<float> savedU = u;
            const std::vector<float> savedV = v;
            const std::vector<float> savedP = p;
            const std::vector<RigidBody> savedBodies = bodies;
            bodyForces();

            float change = 0.0f;
            int taken = 0;
            for (int pass = 0; pass < cfg.bodyIterations; ++pass) {
                std::vector<float> forceX(bodies.size());
                std::vector<float> forceY(bodies.size());
                std::vector<float> torque(bodies.size());
                for (size_t id = 0; id < bodies.size(); ++id) {
                    forceX[id] = bodies[id].forceX;
                    forceY[id] = bodies[id].forceY;
                    torque[id] = bodies[id].torque;
                }

                if (pass > 0) {
                    u = savedU;
                    v = savedV;
                    p = savedP;
                }
                std::vector<float> beforeVx(bodies.size());
                std::vector<float> beforeVy(bodies.size());
                bodies = savedBodies;
                for (size_t id = 0; id < bodies.size(); ++id) {
                    bodies[id].forceX = forceX[id];
                    bodies[id].forceY = forceY[id];
                    bodies[id].torque = torque[id];
                }

                advanceBodies(stepDt);
                for (size_t id = 0; id < bodies.size(); ++id) {
                    beforeVx[id] = bodies[id].vx;
                    beforeVy[id] = bodies[id].vy;
                }
                refreshGeometry();
                runStage();
                bodyForces();
                ++taken;

                change = 0.0f;
                float reference = 0.0f;
                for (size_t id = 0; id < bodies.size(); ++id) {
                    if (!bodies[id].free)
                        continue;
                    const float total =
                        bodies[id].mass + bodies[id].addedMass;
                    if (!(total > 0.0f))
                        continue;
                    const float nextVx =
                        savedBodies[id].vx + stepDt * bodies[id].forceX / total;
                    const float nextVy =
                        savedBodies[id].vy + stepDt * bodies[id].forceY / total;
                    change = std::max(change,
                                      std::hypot(nextVx - beforeVx[id],
                                                 nextVy - beforeVy[id]));
                    reference = std::max(reference,
                                         std::hypot(nextVx, nextVy));
                }
                if (change <= 1e-4f * std::max(reference, 1e-6f))
                    break;
            }
            bodyPasses = taken;
        }

        if (multiphase)
            advectPhase();
        if (turbulent) {
            turbulence.advance(u, v, solidMask, dt);
            refreshViscosity();
        }

        currentTime += dt;
        // stepDt is a float and the remainder it was cut from is a double, so
        // accumulating it lands a few ulps short of totalTime and the loop
        // would run once more for a leftover that is pure rounding.
        if (lastStep)
            currentTime = cfg.totalTime;
        step++;
        dt = savedDt;

        progress::update(currentTime);

        // Asked for from the tray menu, or by a first Ctrl+C. The step that
        // was running is finished and the frame below is written, so the run
        // can be continued from it later - which is the whole point of
        // stopping this way rather than killing the process.
        if (progress::stopRequested()) {
            std::cout << "\nStopping at t = " << currentTime
                      << " s as asked. The frame being written now can be "
                         "continued from.\n";
            stopped = true;
            break;
        }

        const float maxVel = maxVelocity();
        diverged = !std::isfinite(maxVel);

        if (diverged || step % 10 == 0) {
            std::cout << "Step " << step
                      << ", t = " << currentTime
                      << " s, dt = " << stepDt
                      << ", |u|max = " << maxVel
                      << ", div = " << maxDivergence()
                      << ", mg res = " << lastResidual
                      << " (" << multigrid.cyclesUsed() << " cycles)"
                      << std::endl;

            if (bodiesMove) {
                constexpr float degToRad = 3.14159265358979f / 180.0f;
                for (const RigidBody& body : bodies) {
                    if (!body.everFree && !body.prescribed)
                        continue;
                    std::cout << "    body " << body.object << " at ("
                              << body.cx + body.x << ", " << body.cy + body.y
                              << ") m, v = (" << body.vx << ", " << body.vy
                              << ") m/s, turned " << body.theta / degToRad
                              << " deg";
                    if (body.everFree)
                        std::cout << (body.free ? ", free" : ", held");
                    if (cfg.bodyForceReport && !body.everFree)
                        std::cout << ", fluid pushes ("
                                  << body.forceX << ", " << body.forceY
                                  << ") N/m, ignored";
                    if (freshCells > 0)
                        std::cout << ", " << freshCells << " faces uncovered";
                    if (bodyPasses > 1)
                        std::cout << ", " << bodyPasses << " coupling passes";
                    std::cout << "\n";
                }
            }
        }

        if (step % 10 == 0 && steadyReached()) {
            std::cout << "Step " << step << ", t = " << currentTime
                      << " s: the field is changing at " << steadyRate
                      << " per second against the driving speed, under the "
                         "steadyTolerance of " << cfg.steadyTolerance
                      << ".\n  Steady state, stopping here.\n";
            steady = true;
            break;
        }

        if (diverged) {
            std::cerr << "Solution diverged at step " << step
                      << "; aborting. The last frame on disk is the last good "
                         "one.\n";
            break;
        }

        if (step % saveInterval == 0)
            saveVTK(step);
    }

    if (!diverged && !fieldBroken && step % saveInterval != 0)
        saveVTK(step);
    progress::finish(!stopped && !diverged && !fieldBroken);
    std::cout << (diverged ? "Simulation aborted at t = "
                           : steady ? "Simulation reached steady state at t = "
                                    : (stopped || fieldBroken)
                                          ? "Simulation stopped at t = "
                                          : "Simulation finished at t = ")
              << currentTime << " s after " << step << " steps.\n";
    if (stopped)
        std::cout << "Continue it with: restart=1 restartFile="
                  << pathToConsole(outputPath) << "\n";
    if (step > 0 && poissonShortSteps * 4 > step)
        std::cout << "  the pressure solve ran out of V-cycles on "
                  << poissonShortSteps << " of them, worst relative residual "
                  << poissonWorstResidual << " against mgTolerance "
                  << cfg.mgTolerance << ". mgIterations = " << cfg.mgIterations
                  << " is low for this case.\n";
}

std::vector<Solver::ExtraField> Solver::buildExtraFields(
    const std::vector<float>& uCell,
    const std::vector<float>& vCell) const {
    std::vector<ExtraField> fields;
    if (cfg.extraFields.empty())
        return fields;

    const int nx = cfg.nx, ny = cfg.ny;
    const size_t cells = static_cast<size_t>(nx) * ny;

    size_t pos = 0;
    while (pos <= cfg.extraFields.size()) {
        const size_t comma = cfg.extraFields.find(',', pos);
        std::string name = cfg.extraFields.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? cfg.extraFields.size() + 1
                                           : comma + 1;
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
            name.erase(name.begin());
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        if (name.empty())
            continue;
        std::string key = name;
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        ExtraField field;
        field.values.assign(cells, 0.0f);

        if (key == "vorticity") {
            field.name = "vorticity";
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    const int im = std::max(i - 1, 0);
                    const int ip = std::min(i + 1, nx - 1);
                    const int jm = std::max(j - 1, 0);
                    const int jp = std::min(j + 1, ny - 1);
                    const float dvdx = (vCell[j * nx + ip] - vCell[j * nx + im]) /
                                       ((ip - im) * dx);
                    const float dudy = (uCell[jp * nx + i] - uCell[jm * nx + i]) /
                                       ((jp - jm) * dy);
                    field.values[j * nx + i] = dvdx - dudy;
                }
        } else if (key == "divergence") {
            field.name = "divergence";
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    field.values[j * nx + i] =
                        solidMask[j * nx + i]
                            ? 0.0f
                            : (u[idxU(i + 1, j)] - u[idxU(i, j)]) * invDx +
                                  (v[idxV(i, j + 1)] - v[idxV(i, j)]) * invDy -
                                  (hasSources ? sourceRate[j * nx + i] : 0.0f);
        } else if (key == "speed") {
            field.name = "speed";
            for (size_t id = 0; id < cells; ++id)
                field.values[id] = std::hypot(uCell[id], vCell[id]);
        } else if (key == "density") {
            field.name = "density";
            if (phase.active())
                field.values = phase.density();
            else
                std::fill(field.values.begin(), field.values.end(), cfg.ro);
        } else if (key == "curvature") {
            field.name = "curvature";
            if (phase.curvature().size() == cells)
                field.values = phase.curvature();
        } else if (key == "source") {
            field.name = "source";
            if (hasSources)
                field.values = sourceRate;
        } else if (key == "nut") {
            field.name = "nuT";
            if (turbulent)
                field.values = turbulence.viscosity();
        } else if (key == "k" || key == "omega") {
            continue;
        } else if (key == "walldistance") {
            field.name = "wallDistance";
            if (turbulent)
                field.values = turbulence.distance();
        } else if (key == "strain") {
            field.name = "strain";
            if (turbulent)
                field.values = turbulence.strain();
        } else if (key == "objectid") {
            field.name = "objectId";
            for (size_t id = 0; id < cells; ++id)
                field.values[id] = static_cast<float>(mesh.objectId[id]);
        } else {
            continue;
        }
        fields.push_back(std::move(field));
    }
    return fields;
}

void Solver::saveVTK(int stepNum) const {
    const int nx = cfg.nx, ny = cfg.ny;
    constexpr size_t BUFFER_WORDS = 4096;
    std::array<uint32_t, BUFFER_WORDS> buffer;
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

    auto writeArray = [&](const float* values, size_t count){
        size_t done = 0;
        while (done < count) {
            const size_t take = std::min(BUFFER_WORDS, count - done);
            const float* src = values + done;
            uint32_t* dst = buffer.data();
            size_t k = 0;
#ifdef __AVX2__
            const __m256i order = _mm256_setr_epi8(
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
            for (; k + 8 <= take; k += 8) {
                const __m256i word =
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + k));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + k),
                                    _mm256_shuffle_epi8(word, order));
            }
#endif
            for (; k < take; ++k) {
                uint32_t x;
                std::memcpy(&x, src + k, sizeof(float));
                dst[k] =
                    ((x & 0x000000FFu) << 24) |
                    ((x & 0x0000FF00u) << 8 ) |
                    ((x & 0x00FF0000u) >> 8 ) |
                    ((x & 0xFF000000u) >> 24);
            }
            fout.write(reinterpret_cast<const char*>(dst),
                       static_cast<std::streamsize>(take * sizeof(uint32_t)));
            done += take;
        }
    };

    const size_t cellCount = static_cast<size_t>(nx) * ny;
    std::vector<float> scratch(cellCount);

    fout << "SCALARS pressure float 1\n" << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        const int row = j * nx;

        for (int i = 0; i < nx; ++i){
            // p holds the reduced pressure, so the hydrostatic field is put
            // back here and nowhere else. Solid cells have a zero diagonal and
            // never take part in the solve, so they keep the hydrostatic value
            // alone instead of punching a hole through the pressure map.

            if (multiphase) {
                scratch[row + i] = solidMask[row + i]
                                       ? headCell(i, j) * phase.density()[row + i]
                                       : p[row + i];
            } else {
                const float value =
                    solidMask[row + i] ? headCell(i, j)
                                       : phiCell(i, j) + p[row + i];
                scratch[row + i] = value * cfg.ro;
            }
        }
    }
    writeArray(scratch.data(), cellCount);
    fout << "\n";

    if (multiphase) {
        fout << "SCALARS phase float 1\n" << "LOOKUP_TABLE default\n";
        writeArray(phase.fraction().data(), phase.fraction().size());
        fout << "\n";
    }

    if (turbulence.transported()) {
        fout << "SCALARS k float 1\n" << "LOOKUP_TABLE default\n";
        writeArray(turbulence.kinetic().data(), turbulence.kinetic().size());
        fout << "\n";
        fout << "SCALARS omega float 1\n" << "LOOKUP_TABLE default\n";
        writeArray(turbulence.frequency().data(),
                   turbulence.frequency().size());
        fout << "\n";
    }

    // One byte a cell instead of an int32, for a mask that only ever holds 0
    // or 1. solidMask is already exactly that array.
    fout << "SCALARS solid unsigned_char 1\n" << "LOOKUP_TABLE default\n";
    fout.write(reinterpret_cast<const char*>(solidMask.data()),
               static_cast<std::streamsize>(solidMask.size()));
    fout << "\n";

    // Kept, because the restart block below is stored as the difference from
    // these two and has to be able to reproduce them exactly
    std::vector<float> uCell(static_cast<size_t>(nx) * ny);
    std::vector<float> vCell(static_cast<size_t>(nx) * ny);

    std::vector<float> interleaved(cellCount * 3u, 0.0f);

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
            // Inside a body the buried faces carry the mirrored value the
            // no-slip condition needs, which is an artefact of the stencil and
            // not a velocity anything moves at. Solid cells get the surface
            // velocity they actually impose instead, zero for a body that does
            // not move. Nothing reads this back; it is what ParaView sees.
            if (solidMask[rowV + i]) {
                const WallField& wall = wallField[mesh.objectId[rowV + i]];
                uu = wall.slideX - wall.omega * ((j + 0.5f) * dy - wall.cy);
                vv = wall.slideY + wall.omega * ((i + 0.5f) * dx - wall.cx);
            }
            uCell[rowV + i] = uu;
            vCell[rowV + i] = vv;
            interleaved[(rowV + i) * 3u] = uu;
            interleaved[(rowV + i) * 3u + 1u] = vv;
        }
    }
    writeArray(interleaved.data(), interleaved.size());
    fout << "\n";

    for (const ExtraField& field : buildExtraFields(uCell, vCell)) {
        fout << "SCALARS " << field.name << " float 1\n"
             << "LOOKUP_TABLE default\n";
        writeArray(field.values.data(), field.values.size());
        fout << "\n";
    }

    std::ostringstream state;
    state << std::setprecision(std::numeric_limits<double>::max_digits10)
          << "restartTime=" << currentTime << "\n"
          << "restartStep=" << stepNum << "\n"
          << std::setprecision(std::numeric_limits<float>::max_digits10)
          << "restartDt=" << dt << "\n";

    if (bodiesMove) {
        state << "bodyState=";
        for (const RigidBody& body : bodies) {
            if (!body.everFree && !body.prescribed)
                continue;
            state << std::setprecision(
                         std::numeric_limits<double>::max_digits10)
                  << body.object << ":" << body.x << "," << body.y << ","
                  << body.theta << ","
                  << std::setprecision(
                         std::numeric_limits<float>::max_digits10)
                  << body.vx << "," << body.vy << "," << body.omega << ";";
        }
        state << "\n";
    }

    const std::string configText = configHeader + state.str();

    // The pressure array above already holds p + phiCell scaled by the density,
    // which is exactly what the pRaw array used to repeat, so it is gone and
    // the reader divides that one back out. The face velocities are stored as
    // what is left of them once the cell averages have predicted them, which
    // is a quarter of the space and still exact to the bit.
    const std::string facePack =
        packFaceVelocities(nx, ny, u, v, uCell, vCell);

    fout << "FIELD RestartData 2\n";
    fout << "configText 1 " << configText.size() << " char\n";
    fout.write(configText.data(),
               static_cast<std::streamsize>(configText.size()));
    fout << "\n";

    fout << "facePack 1 " << facePack.size() << " unsigned_char\n";
    fout.write(facePack.data(),
               static_cast<std::streamsize>(facePack.size()));
    fout << "\n";

    if (stepNum % (std::max(1, cfg.saveInterval) * 10) == 0 || stepNum == 0)
        std::cout << "Saved " << pathToConsole(filename) << std::endl;
}
