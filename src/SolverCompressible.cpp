#include "CompressibleKernels.hpp"
#include "SolverCompressible.hpp"

#include "Progress.hpp"
#include "Runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {

using namespace cfd;

}

void Workspace::fit(const Block& block, int components) {
    const std::size_t faceX =
        static_cast<std::size_t>(block.nx + 1) * block.ny * components;
    const std::size_t faceY =
        static_cast<std::size_t>(block.nx) * (block.ny + 1) * components;
    if (fluxX.size() != faceX)
        fluxX.assign(faceX, 0.0f);
    if (fluxY.size() != faceY)
        fluxY.assign(faceY, 0.0f);

    const std::size_t cells =
        static_cast<std::size_t>(block.stride) * block.rows;
    for (std::vector<float>& field : primitive)
        if (field.size() != cells)
            field.assign(cells, 0.0f);
}

void GasModel::prepare() {
    cv1 = R1 / (gamma1 - 1.0f);
    cv2 = R2 / (gamma2 - 1.0f);
    cp1 = cv1 + R1;
    cp2 = cv2 + R2;
}

void fillGhostCells(Block& block,
                    const BlockBoundaries& sides,
                    const GasModel& gas) {
    const int g = block.ghost;

    #pragma omp parallel for schedule(static) if (block.ny >= 64)
    for (int j = 0; j < block.ny; ++j) {
        BlockBoundaries local = sides;
        local.inletY = (j + 0.5f) / static_cast<float>(block.ny);
        for (int k = 1; k <= g; ++k) {
            mirrorSide(block, gas, sides.left, -k, j, k - 1, j, true, local);
            mirrorSide(block, gas, sides.right, block.nx - 1 + k, j,
                       block.nx - k, j, true, local);
        }
    }

    #pragma omp parallel for schedule(static) if (block.nx >= 64)
    for (int i = 0; i < block.nx; ++i) {
        BlockBoundaries local = sides;
        local.inletY = (i + 0.5f) / static_cast<float>(block.nx);
        for (int k = 1; k <= g; ++k) {
            mirrorSide(block, gas, sides.bottom, i, -k, i, k - 1, false, local);
            mirrorSide(block, gas, sides.top, i, block.ny - 1 + k, i,
                       block.ny - k, false, local);
        }
    }

    for (int k = 1; k <= g; ++k)
        for (int m = 1; m <= g; ++m) {
            const int corners[4][2] = {{-k, -m},
                                       {block.nx - 1 + k, -m},
                                       {-k, block.ny - 1 + m},
                                       {block.nx - 1 + k, block.ny - 1 + m}};
            const int sourceIndex[4][2] = {
                {0, 0},
                {block.nx - 1, 0},
                {0, block.ny - 1},
                {block.nx - 1, block.ny - 1}};
            for (int c = 0; c < 4; ++c) {
                const Primitive q = primitiveOf(
                    block, gas, block.index(sourceIndex[c][0],
                                            sourceIndex[c][1]));
                writeState(block, block.index(corners[c][0], corners[c][1]), q);
            }
        }
}

void fillSolidCells(Block& block, const GasModel& gas) {
    if (!block.solid)
        return;

    for (int layer = 0; layer < 2; ++layer) {
        #pragma omp parallel for schedule(static) if (block.ny >= 64)
        for (int j = 0; j < block.ny; ++j)
            for (int i = 0; i < block.nx; ++i)
                solidCell(block, gas, i, j, layer);
    }
}

float blockTimeStep(const Block& block, const GasModel& gas, float cfl) {
    float worst = 0.0f;

    #pragma omp parallel if (block.ny >= 64)
    {
        float local = 0.0f;

        #pragma omp for schedule(static) nowait
        for (int j = 0; j < block.ny; ++j)
            for (int i = 0; i < block.nx; ++i)
                local = std::max(local, cellRate(block, gas, i, j));

        #pragma omp critical
        worst = std::max(worst, local);
    }

    if (!(worst > 0.0f))
        return 0.0f;
    return cfl / worst;
}

void advanceStage(Block& in,
                  const Block& keep,
                  Block& out,
                  const BlockBoundaries& sides,
                  const GasModel& gas,
                  float dt,
                  float a,
                  float b,
                  LimiterKind limiter,
                  float diffusivity,
                  Workspace& work) {
    fillSolidCells(in, gas);
    fillGhostCells(in, sides, gas);
    work.fit(in, kComponents);

    const int nx = in.nx;
    const int ny = in.ny;
    float* __restrict fx = work.fluxX.data();
    float* __restrict fy = work.fluxY.data();

    const int limiterCode = static_cast<int>(limiter);

    float* __restrict pRho = work.primitive[0].data();
    float* __restrict pU = work.primitive[1].data();
    float* __restrict pV = work.primitive[2].data();
    float* __restrict pP = work.primitive[3].data();
    float* __restrict pY = work.primitive[4].data();
    float* __restrict pGamma = work.primitive[5].data();

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = -in.ghost; j < ny + in.ghost; ++j) {
        const int row = in.index(-in.ghost, j);
        const int width = nx + 2 * in.ghost;
        for (int k = 0; k < width; ++k)
            fillPrimitive(in, gas, row + k, pRho, pU, pV, pP, pY, pGamma);
    }

    PrimitiveField prim;
    prim.rho = pRho;
    prim.u = pU;
    prim.v = pV;
    prim.p = pP;
    prim.y = pY;
    prim.gamma = pGamma;

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i <= nx; ++i)
            faceFluxX(in, prim, gas, limiterCode, i, j,
                      fx + (static_cast<std::size_t>(j) * (nx + 1) + i) *
                               kComponents);

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i < nx; ++i)
            faceFluxY(in, prim, gas, limiterCode, i, j,
                      fy + (static_cast<std::size_t>(j) * nx + i) *
                               kComponents);

    #pragma omp parallel for schedule(static) if (ny >= 32)
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            combine(in, keep, out, gas, fx, fy, i, j, dt, a, b, diffusivity);
}

namespace {

int roundUp(double value) {
    return static_cast<int>(value + 0.5);
}

}

CompressibleRun::CompressibleRun(const Config& configuration, Mesh& meshIn)
    : cfg(configuration), mesh(meshIn) {
    nx = cfg.nx;
    ny = cfg.ny;
    dx = cfg.Lx / nx;
    dy = cfg.Ly / ny;
    outputPath = narrowToPath(cfg.outputDir);

    gas.gamma1 = cfg.gamma;
    gas.R1 = cfg.R;
    gas.gamma2 = cfg.gamma2;
    gas.R2 = cfg.R2;
    gas.species = cfg.twoSpecies();
    gas.active = cfg.speciesMode == SpeciesMode::Active;
    gas.prepare();

    const auto fill = [&](SideState& state, BoundarySide which) {
        const BoundarySpec& spec = cfg.boundaries[which];
        state.kind = spec.kind;
        state.noSlip = spec.kind == BoundaryKind::Wall ||
                       spec.kind == BoundaryKind::MovingWall;
        state.speed = spec.speedSet ? spec.speed : 0.0f;
        state.from = spec.from;
        state.to = spec.to;
        state.banded = spec.kind == BoundaryKind::Inlet &&
                       (spec.from > 0.0f || spec.to < 1.0f);
    };
    fill(sides.left, BoundarySide::Left);
    fill(sides.right, BoundarySide::Right);
    fill(sides.bottom, BoundarySide::Bottom);
    fill(sides.top, BoundarySide::Top);
    sides.pInf = cfg.pInf;
    sides.T0 = cfg.T0;
    sides.mach = cfg.machInlet;

    solidMask.assign(static_cast<std::size_t>(nx) * ny, 0);
    solidVelX.assign(static_cast<std::size_t>(nx) * ny, 0.0f);
    solidVelY.assign(static_cast<std::size_t>(nx) * ny, 0.0f);
    for (int id = 0; id < nx * ny; ++id)
        solidMask[id] = mesh.solid[id] ? 1 : 0;

    allocate();

#ifdef USE_CUDA
    if (cfg.useCuda && runtime::settings().useCuda && compressibleCudaAvailable()) {
        device = compressibleCudaCreate(nx, ny, ghost, cfg.twoSpecies());
        onDevice = device != nullptr;
        if (onDevice)
            compressibleCudaUploadSolid(device, solidMask.data(),
                                        solidVelX.data(), solidVelY.data());
    }
#endif
}

CompressibleRun::~CompressibleRun() {
#ifdef USE_CUDA
    if (device)
        compressibleCudaDestroy(device);
#endif
}

void CompressibleRun::allocate() {
    const std::size_t stride = static_cast<std::size_t>(nx) + 2 * ghost;
    const std::size_t rows = static_cast<std::size_t>(ny) + 2 * ghost;
    const std::size_t total = stride * rows;
    const bool species = cfg.twoSpecies();

    const auto give = [&](std::vector<float>& field) {
        field.assign(total, 0.0f);
    };
    give(rho); give(rhou); give(rhov); give(rhoE);
    give(rho1); give(rhou1); give(rhov1); give(rhoE1);
    give(rho2); give(rhou2); give(rhov2); give(rhoE2);
    if (species) {
        give(rhoY); give(rhoY1); give(rhoY2);
    }

    const std::size_t cells = static_cast<std::size_t>(nx) * ny;
    if (cfg.acousticFields) {
        pressureMean.assign(cells, 0.0f);
        pressureFast.assign(cells, 0.0f);
        pressureRms.assign(cells, 0.0f);
        crossingRate.assign(cells, 0.0f);
        lastSign.assign(cells, 0);
    }
    mics = cfg.resolvedMicrophones();
    micSamples.assign(mics.size(), {});
}

Block CompressibleRun::view(std::vector<float>& r,
                            std::vector<float>& ru,
                            std::vector<float>& rv,
                            std::vector<float>& re,
                            std::vector<float>& ry) {
    Block block;
    block.nx = nx;
    block.ny = ny;
    block.ghost = ghost;
    block.stride = nx + 2 * ghost;
    block.rows = ny + 2 * ghost;
    block.dx = dx;
    block.dy = dy;
    block.x0 = 0.0f;
    block.y0 = 0.0f;
    block.rho = r.data();
    block.rhou = ru.data();
    block.rhov = rv.data();
    block.rhoE = re.data();
    block.rhoY = ry.empty() ? nullptr : ry.data();
    block.solid = solidMask.data();
    block.solidU = solidVelX.data();
    block.solidV = solidVelY.data();
    return block;
}

void CompressibleRun::initialise() {
    if (hasRestartState)
        return;

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    const bool tube = cfg.caseType == CaseType::ShockTube;
    const float baseDensity = cfg.pInf / (cfg.R * cfg.T0);
    const float speedOfSound = std::sqrt(cfg.gamma * cfg.R * cfg.T0);
    const bool blows = cfg.boundaries[BoundarySide::Left].kind ==
                       BoundaryKind::Inlet;
    const float speed = tube ? 0.0f : (blows ? cfg.machInlet * speedOfSound
                                             : 0.0f);

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const float fx = (i + 0.5f) / static_cast<float>(nx);
            const float fy = (j + 0.5f) / static_cast<float>(ny);

            Primitive q;
            q.u = speed;
            q.v = 0.0f;
            q.y = 0.0f;

            if (cfg.twoSpecies()) {
                switch (cfg.phaseInit) {
                case PhaseInit::Layer:
                    q.y = fy < cfg.phaseLevel ? 1.0f : 0.0f;
                    break;
                case PhaseInit::Column:
                    q.y = fx < cfg.phaseLevel ? 1.0f : 0.0f;
                    break;
                case PhaseInit::Drop:
                default: {
                    const float ddx = (fx - cfg.phaseX) * cfg.Lx;
                    const float ddy = (fy - cfg.phaseY) * cfg.Ly;
                    const float radius = 0.5f * cfg.phaseLevel *
                                         std::min(cfg.Lx, cfg.Ly);
                    q.y = std::hypot(ddx, ddy) < radius ? 1.0f : 0.0f;
                    break;
                }
                }
            }

            const bool far = tube && fx > 0.5f;
            const float pressureFactor = far ? 0.1f : 1.0f;
            const float densityFactor = far ? 0.125f : 1.0f;

            q.gamma = gas.gammaOf(q.y);
            const float gasR = gas.gasConstantOf(q.y);
            q.p = cfg.pInf * pressureFactor;
            q.rho = cfg.pInf / (gasR * cfg.T0) * densityFactor;
            writeState(block, block.index(i, j), q);
        }

    fillSolidCells(block, gas);
    fillGhostCells(block, sides, gas);
}

bool CompressibleRun::setInitialState(RestartData&& state,
                                      const std::string& prefix) {
    const std::size_t cells = static_cast<std::size_t>(nx) * ny;
    if (state.stateRho.size() != cells || state.stateRhoU.size() != cells ||
        state.stateRhoV.size() != cells || state.stateRhoE.size() != cells)
        return false;

    restartBodies = state.bodies;
    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    const bool species = cfg.twoSpecies() && state.stateRhoY.size() == cells;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            const int id = block.index(i, j);
            rho[id] = state.stateRho[flat];
            rhou[id] = state.stateRhoU[flat];
            rhov[id] = state.stateRhoV[flat];
            rhoE[id] = state.stateRhoE[flat];
            if (block.rhoY)
                rhoY[id] = species ? state.stateRhoY[flat] : 0.0f;
        }

    currentTime = state.currentTime;
    step = state.step;
    dt = state.dt;
    hasRestartState = true;
    if (!prefix.empty())
        framePrefix = prefix;
    fillSolidCells(block, gas);
    fillGhostCells(block, sides, gas);
    return true;
}

void CompressibleRun::computeStep() {
    Block current = view(rho, rhou, rhov, rhoE, rhoY);
    Block stage1 = view(rho1, rhou1, rhov1, rhoE1, rhoY1);
    Block stage2 = view(rho2, rhou2, rhov2, rhoE2, rhoY2);

    const float diffusivity = cfg.twoSpecies() ? cfg.diffusivity : 0.0f;

#ifdef USE_CUDA
    if (onDevice) {
        const int limiter = static_cast<int>(cfg.limiter);
        compressibleCudaStage(device, current, 0, 0, 1, gas, sides, dt, 0.0f,
                              1.0f, limiter, diffusivity);
        compressibleCudaStage(device, current, 1, 0, 2, gas, sides, dt, 0.75f,
                              0.25f, limiter, diffusivity);
        compressibleCudaStage(device, current, 2, 0, 0, gas, sides, dt,
                              1.0f / 3.0f, 2.0f / 3.0f, limiter, diffusivity);
        return;
    }
#endif

    advanceStage(current, current, stage1, sides, gas, dt, 0.0f, 1.0f,
                 cfg.limiter, diffusivity, work);
    advanceStage(stage1, current, stage2, sides, gas, dt, 0.75f, 0.25f,
                 cfg.limiter, diffusivity, work);
    advanceStage(stage2, current, current, sides, gas, dt,
                 1.0f / 3.0f, 2.0f / 3.0f, cfg.limiter, diffusivity, work);
}

void CompressibleRun::syncFromDevice() {
#ifdef USE_CUDA
    if (!onDevice)
        return;
    float* host[5] = {rho.data(), rhou.data(), rhov.data(), rhoE.data(),
                      rhoY.empty() ? nullptr : rhoY.data()};
    compressibleCudaDownload(device, 0, host);
#endif
}

void CompressibleRun::syncToDevice() {
#ifdef USE_CUDA
    if (!onDevice)
        return;
    const float* host[5] = {rho.data(), rhou.data(), rhov.data(), rhoE.data(),
                            rhoY.empty() ? nullptr : rhoY.data()};
    compressibleCudaUpload(device, 0, host);
#endif
}

float CompressibleRun::timeStep(const Block& block) {
#ifdef USE_CUDA
    if (onDevice)
        return compressibleCudaTimeStep(device, block, gas, cfg.CFL);
#endif
    return blockTimeStep(block, gas, cfg.CFL);
}

void CompressibleRun::updateAcoustics(float stepDt) {
    if (!cfg.acousticFields || stepDt <= 0.0f)
        return;

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    const float window = std::max(cfg.acousticWindow, stepDt);
    const float alpha = std::min(1.0f, stepDt / window);

    const float alphaFast = std::min(1.0f, 16.0f * stepDt / window);
    const float rate = 1.0f / stepDt;

    #pragma omp parallel for schedule(static) if (ny >= 64)
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            if (solidMask[flat]) {
                pressureMean[flat] = 0.0f;
                pressureFast[flat] = 0.0f;
                pressureRms[flat] = 0.0f;
                crossingRate[flat] = 0.0f;
                continue;
            }
            const Primitive q = primitiveOf(block, gas, block.index(i, j));
            if (!acousticsReady) {
                pressureMean[flat] = q.p;
                pressureFast[flat] = q.p;
                continue;
            }
            pressureMean[flat] += alpha * (q.p - pressureMean[flat]);
            pressureFast[flat] += alphaFast * (q.p - pressureFast[flat]);
            const float fluctuation = q.p - pressureMean[flat];
            pressureRms[flat] +=
                alpha * (fluctuation * fluctuation - pressureRms[flat]);

            const int8_t sign = q.p > pressureFast[flat] ? 1 : -1;
            const float crossed =
                (lastSign[flat] != 0 && sign != lastSign[flat]) ? rate : 0.0f;
            crossingRate[flat] += alpha * (crossed - crossingRate[flat]);
            lastSign[flat] = sign;
        }
    }
    acousticsReady = true;
}

void CompressibleRun::sampleMicrophones() {
    if (mics.empty() || (step % std::max(1, cfg.micInterval)) != 0)
        return;

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    micTimes.push_back(static_cast<float>(currentTime));
    for (std::size_t m = 0; m < mics.size(); ++m) {
        const int i = std::clamp(static_cast<int>(mics[m].x / dx), 0, nx - 1);
        const int j = std::clamp(static_cast<int>(mics[m].y / dy), 0, ny - 1);
        const Primitive q = primitiveOf(block, gas, block.index(i, j));
        micSamples[m].push_back(q.p);
    }
}

void CompressibleRun::writeMicrophones() const {
    if (mics.empty() || micTimes.size() < 4)
        return;

    std::error_code directoryError;
    std::filesystem::create_directories(outputPath, directoryError);
    std::ofstream out(outputPath / "microphones.txt");
    if (!out.is_open())
        return;

    const std::size_t samples = micTimes.size();
    const double span = micTimes.back() - micTimes.front();
    const double rate = span > 0.0 ? (samples - 1) / span : 0.0;

    out << "# t";
    for (std::size_t m = 0; m < mics.size(); ++m)
        out << " p" << (m + 1);
    out << "\n";
    for (std::size_t k = 0; k < samples; ++k) {
        out << micTimes[k];
        for (const auto& channel : micSamples)
            out << " " << channel[k];
        out << "\n";
    }

    std::cout << "\nMicrophones (" << samples << " samples at "
              << rate << " Hz):\n";
    out << "#\n# summary: x y SPL_dB peak_Hz\n";

    constexpr int kBins = 512;
    for (std::size_t m = 0; m < mics.size(); ++m) {
        const std::vector<float>& channel = micSamples[m];
        double mean = 0.0;
        for (float value : channel)
            mean += value;
        mean /= static_cast<double>(samples);

        double energy = 0.0;
        for (float value : channel)
            energy += (value - mean) * (value - mean);
        const double rms = std::sqrt(energy / static_cast<double>(samples));
        const double spl =
            rms > 0.0 ? 20.0 * std::log10(rms / cfg.acousticRef) : 0.0;

        double bestPower = 0.0;
        double bestFrequency = 0.0;
        const double nyquist = 0.5 * rate;
        for (int bin = 1; bin <= kBins; ++bin) {
            const double frequency = nyquist * bin / (kBins + 1.0);
            const double omegaBin = 2.0 * 3.14159265358979 * frequency / rate;
            const double coefficient = 2.0 * std::cos(omegaBin);
            double s1 = 0.0, s2 = 0.0;
            for (float value : channel) {
                const double s0 = (value - mean) + coefficient * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
            if (power > bestPower) {
                bestPower = power;
                bestFrequency = frequency;
            }
        }

        char line[200];
        std::snprintf(line, sizeof(line),
                      "  mic %zu at (%.4g, %.4g): %.1f dB, peak %.1f Hz",
                      m + 1, static_cast<double>(mics[m].x),
                      static_cast<double>(mics[m].y), spl, bestFrequency);
        std::cout << line << "\n";
        out << "# " << mics[m].x << " " << mics[m].y << " " << spl << " "
            << bestFrequency << "\n";
    }
    std::cout << "Written " << pathToConsole(outputPath / "microphones.txt")
              << "\n";
}

std::vector<float> CompressibleRun::primitive(const char* what) const {
    const std::size_t cells = static_cast<std::size_t>(nx) * ny;
    std::vector<float> out(cells, 0.0f);
    Block block = const_cast<CompressibleRun*>(this)->view(
        const_cast<std::vector<float>&>(rho),
        const_cast<std::vector<float>&>(rhou),
        const_cast<std::vector<float>&>(rhov),
        const_cast<std::vector<float>&>(rhoE),
        const_cast<std::vector<float>&>(rhoY));

    const std::string key = what;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            const Primitive q = primitiveOf(block, gas, block.index(i, j));
            const float gasR = gas.gasConstantOf(q.y);
            const float temperature = q.p / (q.rho * gasR);
            const float speedOfSound = std::sqrt(q.gamma * q.p / q.rho);
            if (key == "pressure")
                out[flat] = q.p;
            else if (key == "density")
                out[flat] = q.rho;
            else if (key == "temperature")
                out[flat] = temperature;
            else if (key == "mach")
                out[flat] = std::hypot(q.u, q.v) / speedOfSound;
            else if (key == "speedofsound")
                out[flat] = speedOfSound;
            else if (key == "species")
                out[flat] = q.y;
            else if (key == "entropy")
                out[flat] = gasR / (q.gamma - 1.0f) *
                            std::log(q.p / std::pow(q.rho, q.gamma));
            else if (key == "u")
                out[flat] = q.u;
            else if (key == "v")
                out[flat] = q.v;
            else if (key == "speed")
                out[flat] = std::hypot(q.u, q.v);
            else if (key == "pfluct")
                out[flat] = pressureMean.empty()
                                ? 0.0f
                                : q.p - pressureMean[flat];
            else if (key == "spl")
                out[flat] =
                    pressureRms.empty() || !(pressureRms[flat] > 0.0f)
                        ? 0.0f
                        : 20.0f * std::log10(std::sqrt(pressureRms[flat]) /
                                             cfg.acousticRef);
            else if (key == "pitch")
                out[flat] =
                    crossingRate.empty() ? 0.0f : 0.5f * crossingRate[flat];
        }
    return out;
}

void CompressibleRun::saveVTK(int stepNumber) const {
    constexpr std::size_t kBufferWords = 4096;
    std::vector<uint32_t> buffer(kBufferWords);
    std::error_code directoryError;
    std::filesystem::create_directories(outputPath, directoryError);

    std::filesystem::path filename = outputPath;
    filename /= framePrefix + "_" + std::to_string(stepNumber) + ".vtk";
    std::ofstream fout(filename, std::ios::binary);
    if (!fout) {
        std::cerr << "Cannot open " << pathToConsole(filename)
                  << " for writing.\n";
        return;
    }

    fout << "# vtk DataFile Version 3.0\n"
         << "CFD-Solver-2D output, step " << stepNumber << "\n"
         << "BINARY\n"
         << "DATASET STRUCTURED_POINTS\n"
         << "DIMENSIONS " << nx + 1 << " " << ny + 1 << " 1\n"
         << "ORIGIN 0 0 0\n"
         << "SPACING " << dx << " " << dy << " 1\n"
         << "CELL_DATA " << nx * ny << "\n";

    const auto writeArray = [&](const float* values, std::size_t count) {
        std::size_t done = 0;
        while (done < count) {
            const std::size_t take = std::min(kBufferWords, count - done);
            for (std::size_t k = 0; k < take; ++k) {
                uint32_t word;
                std::memcpy(&word, values + done + k, sizeof(float));
                buffer[k] = ((word & 0x000000FFu) << 24) |
                            ((word & 0x0000FF00u) << 8) |
                            ((word & 0x00FF0000u) >> 8) |
                            ((word & 0xFF000000u) >> 24);
            }
            fout.write(reinterpret_cast<const char*>(buffer.data()),
                       static_cast<std::streamsize>(take * sizeof(uint32_t)));
            done += take;
        }
    };

    const auto writeScalar = [&](const char* name,
                                 const std::vector<float>& values) {
        fout << "SCALARS " << name << " float 1\nLOOKUP_TABLE default\n";
        writeArray(values.data(), values.size());
        fout << "\n";
    };

    const std::vector<float> pressure = primitive("pressure");
    const std::vector<float> density = primitive("density");
    writeScalar("pressure", pressure);
    writeScalar("density", density);
    if (cfg.twoSpecies())
        writeScalar("species", primitive("species"));

    fout << "SCALARS solid unsigned_char 1\nLOOKUP_TABLE default\n";
    fout.write(reinterpret_cast<const char*>(solidMask.data()),
               static_cast<std::streamsize>(solidMask.size()));
    fout << "\n";

    const std::vector<float> uCell = primitive("u");
    const std::vector<float> vCell = primitive("v");
    const std::size_t cells = static_cast<std::size_t>(nx) * ny;
    std::vector<float> interleaved(cells * 3, 0.0f);
    for (std::size_t id = 0; id < cells; ++id) {
        interleaved[3 * id] = uCell[id];
        interleaved[3 * id + 1] = vCell[id];
    }
    fout << "VECTORS velocity float\n";
    writeArray(interleaved.data(), interleaved.size());
    fout << "\n";

    std::string wanted = cfg.extraFields;
    for (char& c : wanted)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t position = 0;
    while (position <= wanted.size()) {
        const std::size_t comma = wanted.find(',', position);
        std::string key = wanted.substr(
            position, comma == std::string::npos ? std::string::npos
                                                 : comma - position);
        position = comma == std::string::npos ? wanted.size() + 1 : comma + 1;
        while (!key.empty() && key.front() == ' ')
            key.erase(key.begin());
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        if (key.empty() || key == "density" || key == "species" ||
            key == "pressure")
            continue;

        const char* name = nullptr;
        if (key == "temperature") name = "temperature";
        else if (key == "mach") name = "mach";
        else if (key == "speedofsound") name = "speedOfSound";
        else if (key == "entropy") name = "entropy";
        else if (key == "speed") name = "speed";
        else if (key == "objectid") name = "objectId";
        else if (key == "pfluct") name = "pFluctuation";
        else if (key == "spl") name = "SPL";
        else if (key == "pitch") name = "pitch";
        if (!name)
            continue;

        if (key == "objectid") {
            std::vector<float> ids(cells, 0.0f);
            for (std::size_t id = 0; id < cells; ++id)
                ids[id] = static_cast<float>(mesh.objectId[id]);
            writeScalar(name, ids);
        } else {
            writeScalar(name, primitive(key.c_str()));
        }
    }

    Config stored = cfg;
    stored.restart = true;
    std::string configText = stored.serialize();
    configText += "restartTime=" + std::to_string(currentTime) + "\n";
    configText += "restartStep=" + std::to_string(stepNumber) + "\n";
    configText += "restartDt=" + std::to_string(dt) + "\n";
    configText += "formatVersion=" + std::to_string(FRAME_FORMAT_VERSION) +
                  "\n";
    if (bodiesMove) {
        std::ostringstream bodyLine;
        bodyLine << "bodyState=";
        for (const RigidBody& body : bodies) {
            if (!body.everFree && !body.prescribed)
                continue;
            bodyLine << std::setprecision(
                            std::numeric_limits<double>::max_digits10)
                     << body.object << ":" << body.x << "," << body.y << ","
                     << body.theta << ","
                     << std::setprecision(
                            std::numeric_limits<float>::max_digits10)
                     << body.vx << "," << body.vy << "," << body.omega << ";";
        }
        bodyLine << "\n";
        configText += bodyLine.str();
    }

    const int arrays = cfg.twoSpecies() ? 6 : 5;
    fout << "FIELD RestartData " << arrays << "\n";
    fout << "configText 1 " << configText.size() << " char\n";
    fout.write(configText.data(),
               static_cast<std::streamsize>(configText.size()));
    fout << "\n";

    const auto writeField = [&](const char* name,
                                const std::vector<float>& source) {
        std::vector<float> packed(cells, 0.0f);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
                packed[flat] = source[(j + ghost) * (nx + 2 * ghost) + i +
                                      ghost];
            }
        fout << name << " 1 " << cells << " float\n";
        writeArray(packed.data(), packed.size());
        fout << "\n";
    };

    writeField("stateRho", rho);
    writeField("stateRhoU", rhou);
    writeField("stateRhoV", rhov);
    writeField("stateRhoE", rhoE);
    if (cfg.twoSpecies())
        writeField("stateRhoY", rhoY);

    if (stepNumber % (std::max(1, cfg.saveInterval) * 10) == 0 ||
        stepNumber == 0)
        std::cout << "Saved " << pathToConsole(filename) << std::endl;
}

void CompressibleRun::reportStep() const {
    Block block = const_cast<CompressibleRun*>(this)->view(
        const_cast<std::vector<float>&>(rho),
        const_cast<std::vector<float>&>(rhou),
        const_cast<std::vector<float>&>(rhov),
        const_cast<std::vector<float>&>(rhoE),
        const_cast<std::vector<float>&>(rhoY));

    float peakMach = 0.0f, lowPressure = std::numeric_limits<float>::max();
    float highPressure = 0.0f;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            if (solidMask[static_cast<std::size_t>(j) * nx + i])
                continue;
            const Primitive q = primitiveOf(block, gas, block.index(i, j));
            const float speedOfSound = std::sqrt(q.gamma * q.p / q.rho);
            peakMach = std::max(peakMach, std::hypot(q.u, q.v) / speedOfSound);
            lowPressure = std::min(lowPressure, q.p);
            highPressure = std::max(highPressure, q.p);
        }

    char line[220];
    std::snprintf(line, sizeof(line),
                  "step %6d  t = %.6f s  dt = %.3e  Mach max %.3f  "
                  "p %.4g .. %.4g Pa",
                  step, currentTime, static_cast<double>(dt),
                  static_cast<double>(peakMach),
                  static_cast<double>(lowPressure),
                  static_cast<double>(highPressure));
    std::cout << line << "\n";
}

void CompressibleRun::run() {
    initialise();
    resolveBodyMotion();
    reportBodies();

    std::error_code directoryError;
    std::filesystem::create_directories(outputPath, directoryError);

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    syncToDevice();
    if (dt <= 0.0f)
        dt = timeStep(block) * cfg.dtSafety;

    const double target = cfg.restart && cfg.addTime > 0.0
                              ? currentTime + cfg.addTime
                              : cfg.totalTime;

    progress::begin("Fluid Solver", currentTime, target, cfg.outputDir);

    if (!hasRestartState)
        saveVTK(step);

    bool stopped = false;
    int sinceReport = 0;
    while (currentTime < target - 1e-12) {
        if ((step % std::max(1, cfg.dtUpdateInterval)) == 0)
            dt = timeStep(block) * cfg.dtSafety;
        if (!(dt > 0.0f)) {
            std::cerr << "\nThe time step collapsed to zero, which means the "
                         "state stopped being a state.\n";
            break;
        }
        const double remaining = target - currentTime;
        if (remaining <= 0.0)
            break;
        if (static_cast<double>(dt) > remaining) {
            const float clipped = static_cast<float>(remaining);
            if (!(clipped > 0.0f))
                break;
            dt = clipped;
        }

        computeStep();
        if (bodiesMove) {
            syncFromDevice();
            advanceBodies(dt);
            syncToDevice();
        }
        currentTime += dt;
        ++step;
        ++sinceReport;

        const bool wanted = (step % std::max(1, cfg.saveInterval)) == 0;
        if (cfg.acousticFields || !mics.empty() || wanted)
            syncFromDevice();

        updateAcoustics(dt);
        sampleMicrophones();

        if (wanted)
            saveVTK(step);

        progress::update(currentTime);
        if (sinceReport >= std::max(1, cfg.saveInterval)) {
            reportStep();
            sinceReport = 0;
        }
        if (progress::stopRequested()) {
            stopped = true;
            break;
        }
    }

    syncFromDevice();
    saveVTK(step);
    reportStep();
    writeMicrophones();
    writeMicrophoneAudio();
    progress::finish(!stopped);

    std::cout << "\nSimulation finished at t = " << currentTime << " s after "
              << step << " steps.\n";
}

void CompressibleRun::resolveBodyMotion() {
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
                     "the run a geometryFile\n    or profiles= and the bodies "
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

    const float fluidDensity = cfg.pInf / (cfg.R * cfg.T0);
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
            static_cast<std::size_t>(saved.object) >= bodies.size())
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

    applyBodyPoses();
    refreshSolidMask();
}

void CompressibleRun::reportBodies() const {
    if (!bodiesMove)
        return;

    constexpr float degToRad = 3.14159265358979f / 180.0f;
    std::cout << "Bodies that travel:\n";
    for (const RigidBody& body : bodies) {
        if (!body.everFree && !body.prescribed)
            continue;
        std::cout << "  object " << body.object << " at (" << body.cx << ", "
                  << body.cy << ") m, ";
        if (body.free)
            std::cout << "free, mass " << body.mass << " kg, added "
                      << body.addedMass << " kg";
        else
            std::cout << "on rails";
        if (body.pinX || body.pinY || body.pinRot) {
            std::cout << ", pinned in";
            if (body.pinX) std::cout << " x";
            if (body.pinY) std::cout << " y";
            if (body.pinRot) std::cout << " rotation";
        }
        std::cout << "\n";
    }
    if (bodiesFree)
        std::cout << "  the force on a free body is the pressure integral "
                     "over its own faces; the wall\n  ghosts mirror about the "
                     "body velocity, so a moving body pushes on the gas and "
                     "the\n  gas pushes back.\n";
    if (bodyCollisions)
        std::cout << "  bodyCollisions is on, restitution "
                  << cfg.bodyRestitution << ".\n";
    (void)degToRad;
}

void CompressibleRun::applyBodyPoses() {
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

void CompressibleRun::refreshSolidMask() {
    const std::size_t cells = static_cast<std::size_t>(nx) * ny;
    std::vector<uint8_t> before = solidMask;

    mesh.updateSolid();
    for (std::size_t id = 0; id < cells; ++id)
        solidMask[id] = mesh.solid[id] ? 1 : 0;

    const std::vector<int>& owner = mesh.ownership();
    std::fill(solidVelX.begin(), solidVelX.end(), 0.0f);
    std::fill(solidVelY.begin(), solidVelY.end(), 0.0f);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            if (!solidMask[flat])
                continue;
            const int id = owner.empty() ? 0 : owner[flat];
            if (id <= 0 || static_cast<std::size_t>(id) >= bodies.size())
                continue;
            const RigidBody& body = bodies[id];
            const float armX =
                (i + 0.5f) * dx - (body.cx + static_cast<float>(body.x));
            const float armY =
                (j + 0.5f) * dy - (body.cy + static_cast<float>(body.y));
            solidVelX[flat] = body.vx - body.omega * armY;
            solidVelY[flat] = body.vy + body.omega * armX;
        }

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            if (solidMask[flat] || !before[flat])
                continue;

            float sumRho = 0.0f, sumP = 0.0f, sumY = 0.0f;
            int count = 0;
            const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int k = 0; k < 4; ++k) {
                const int ni = i + offsets[k][0];
                const int nj = j + offsets[k][1];
                if (ni < 0 || ni >= nx || nj < 0 || nj >= ny)
                    continue;
                if (solidMask[static_cast<std::size_t>(nj) * nx + ni])
                    continue;
                const Primitive n =
                    primitiveOf(block, gas, block.index(ni, nj));
                sumRho += n.rho;
                sumP += n.p;
                sumY += n.y;
                ++count;
            }

            Primitive q;
            if (count > 0) {
                const float inv = 1.0f / static_cast<float>(count);
                q.rho = sumRho * inv;
                q.p = sumP * inv;
                q.y = sumY * inv;
            } else {
                q.rho = cfg.pInf / (cfg.R * cfg.T0);
                q.p = cfg.pInf;
                q.y = 0.0f;
            }
            q.u = solidVelX[flat];
            q.v = solidVelY[flat];
            q.gamma = gammaOf(gas, q.y);
            writeState(block, block.index(i, j), q);
        }

#ifdef USE_CUDA
    if (onDevice)
        compressibleCudaUploadSolid(device, solidMask.data(),
                                    solidVelX.data(), solidVelY.data());
#endif
}

void CompressibleRun::bodyForces() {
    if (!bodiesMove)
        return;

    for (RigidBody& body : bodies) {
        body.forceX = 0.0f;
        body.forceY = 0.0f;
        body.torque = 0.0f;
    }
    if (!bodiesFree)
        return;

    const std::vector<int>& owner = mesh.ownership();
    if (owner.empty())
        return;

    Block block = view(rho, rhou, rhov, rhoE, rhoY);
    const float faceX = dy;
    const float faceY = dx;

    const auto push = [&](int solidIndex, float fx, float fy, float px,
                          float py) {
        const int id = owner[solidIndex];
        if (id <= 0 || static_cast<std::size_t>(id) >= bodies.size())
            return;
        RigidBody& body = bodies[id];
        const float armX = px - (body.cx + static_cast<float>(body.x));
        const float armY = py - (body.cy + static_cast<float>(body.y));
        body.forceX += fx;
        body.forceY += fy;
        body.torque += armX * fy - armY * fx;
    };

    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        const float y = (j + 0.5f) * dy;
        for (int i = 1; i < nx; ++i) {
            const bool left = solidMask[row + i - 1] != 0;
            const bool right = solidMask[row + i] != 0;
            if (left == right)
                continue;
            const int fluid = left ? i : i - 1;
            const Primitive q = primitiveOf(block, gas, block.index(fluid, j));
            const float force = q.p * faceX * (left ? -1.0f : 1.0f);
            push(row + (left ? i - 1 : i), force, 0.0f, i * dx, y);
        }
    }

    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 0; i < nx; ++i) {
            const bool low = solidMask[row - nx + i] != 0;
            const bool high = solidMask[row + i] != 0;
            if (low == high)
                continue;
            const int fluid = low ? j : j - 1;
            const Primitive q = primitiveOf(block, gas, block.index(i, fluid));
            const float force = q.p * faceY * (low ? -1.0f : 1.0f);
            push((low ? row - nx : row) + i, 0.0f, force, (i + 0.5f) * dx,
                 j * dy);
        }
    }
}

void CompressibleRun::advanceBodies(float stepDt) {
    if (!bodiesMove)
        return;

    if (bodiesFree) {
        bodyForces();
        for (RigidBody& body : bodies)
            if (body.free)
                body.integrate(stepDt);
    }

    const double middle = currentTime + 0.5 * static_cast<double>(stepDt);
    for (RigidBody& body : bodies)
        if (body.prescribed || body.everFree)
            body.step(middle, stepDt);

    if (bodyCollisions)
        resolveBodyCollisions(bodies, mesh.ownership(), mesh.contested(), nx,
                              ny, cfg.Lx, cfg.Ly, cfg.bodyRestitution, stepDt,
                              contactsReported);

    for (RigidBody& body : bodies)
        if (body.prescribed || body.everFree)
            body.advancePose(stepDt);

    applyBodyPoses();
    refreshSolidMask();
}

void CompressibleRun::writeMicrophoneAudio() const {
    if (!cfg.recordsAudio() || mics.empty() || micTimes.size() < 8)
        return;

    const double first = micTimes.front();
    const double span = micTimes.back() - first;
    if (!(span > 0.0))
        return;

    const double speed = cfg.micAudioSpeed > 0.0f ? cfg.micAudioSpeed : 1.0;
    const int rate = cfg.micAudioRate;
    const double audioSeconds = span / speed;
    const long long frames =
        static_cast<long long>(audioSeconds * rate);
    if (frames < 2)
        return;

    const std::size_t samples = micTimes.size();
    std::cout << "\nAudio (" << rate << " Hz, " << audioSeconds
              << " s per file";
    if (speed != 1.0)
        std::cout << ", " << span << " s of flow slowed by " << (1.0 / speed)
                  << "x";
    std::cout << "):\n";

    for (std::size_t m = 0; m < mics.size(); ++m) {
        const std::vector<float>& channel = micSamples[m];
        if (channel.size() != samples)
            continue;

        double mean = 0.0;
        for (float value : channel)
            mean += value;
        mean /= static_cast<double>(samples);

        std::vector<double> track(static_cast<std::size_t>(frames), 0.0);
        std::size_t cursor = 0;
        for (long long k = 0; k < frames; ++k) {
            const double windowStart =
                first + span * static_cast<double>(k) / frames;
            const double windowEnd =
                first + span * static_cast<double>(k + 1) / frames;

            while (cursor + 1 < samples && micTimes[cursor] < windowStart)
                ++cursor;

            double sum = 0.0;
            int count = 0;
            for (std::size_t s = cursor;
                 s < samples && micTimes[s] < windowEnd; ++s) {
                sum += channel[s] - mean;
                ++count;
            }
            if (count > 0) {
                track[static_cast<std::size_t>(k)] =
                    sum / static_cast<double>(count);
                continue;
            }

            const double when = 0.5 * (windowStart + windowEnd);
            std::size_t hi = cursor;
            while (hi + 1 < samples && micTimes[hi] < when)
                ++hi;
            const std::size_t lo = hi > 0 ? hi - 1 : 0;
            const double t0 = micTimes[lo];
            const double t1 = micTimes[hi];
            const double weight =
                t1 > t0 ? (when - t0) / (t1 - t0) : 0.0;
            track[static_cast<std::size_t>(k)] =
                (channel[lo] - mean) +
                weight * (channel[hi] - channel[lo]);
        }

        double drift = 0.0;
        for (double value : track)
            drift += value;
        drift /= static_cast<double>(track.size());

        double peak = 0.0;
        for (double& value : track) {
            value -= drift;
            peak = std::max(peak, std::fabs(value));
        }
        const double gain = peak > 0.0 ? 0.9 * 32767.0 / peak : 0.0;

        char name[64];
        std::snprintf(name, sizeof(name), "microphone%zu.wav", m + 1);
        const std::filesystem::path file = outputPath / name;
        std::ofstream out(file, std::ios::binary);
        if (!out.is_open())
            continue;

        const uint32_t dataBytes = static_cast<uint32_t>(frames * 2);
        const uint32_t sampleRate = static_cast<uint32_t>(rate);
        const auto put32 = [&](uint32_t value) {
            const unsigned char raw[4] = {
                static_cast<unsigned char>(value & 0xFF),
                static_cast<unsigned char>((value >> 8) & 0xFF),
                static_cast<unsigned char>((value >> 16) & 0xFF),
                static_cast<unsigned char>((value >> 24) & 0xFF)};
            out.write(reinterpret_cast<const char*>(raw), 4);
        };
        const auto put16 = [&](uint16_t value) {
            const unsigned char raw[2] = {
                static_cast<unsigned char>(value & 0xFF),
                static_cast<unsigned char>((value >> 8) & 0xFF)};
            out.write(reinterpret_cast<const char*>(raw), 2);
        };

        out.write("RIFF", 4);
        put32(36 + dataBytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        put32(16);
        put16(1);
        put16(1);
        put32(sampleRate);
        put32(sampleRate * 2);
        put16(2);
        put16(16);
        out.write("data", 4);
        put32(dataBytes);

        for (double value : track) {
            double scaled = value * gain;
            scaled = std::max(-32768.0, std::min(32767.0, scaled));
            put16(static_cast<uint16_t>(static_cast<int16_t>(
                scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5)));
        }
        out.close();

        char line[220];
        std::snprintf(line, sizeof(line),
                      "  mic %zu -> %s, full scale is %.4g Pa of fluctuation",
                      m + 1, name, peak);
        std::cout << line << "\n";
    }
    std::cout << "Written next to the frames in "
              << pathToConsole(outputPath) << "\n";
}
