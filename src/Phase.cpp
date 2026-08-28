#include "Phase.hpp"

#include "Runtime.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace {

constexpr float kFlat = 1e-6f;

enum VofKind {
    VofUpwind = 0,
    VofHric,
    VofCicsam
};

inline float normalised(float donor, float acceptor, float far) {
    const float span = acceptor - far;
    if (std::fabs(span) < kFlat)
        return -1.0f;   // flat: outside [0,1], which every scheme reads as upwind
    return (donor - far) / span;
}

// The normalised face value each scheme asks for, given the normalised donor
// value, the face Courant number and how square the interface sits to the face.
// Everything is in normalised variables, so the answer is between 0 and 1 and
// the caller maps it back.
template <int Scheme>
inline float faceNormalised(float cd, float courant, float cosTheta) {
    if (Scheme == VofUpwind)
        return cd;
    if (!(cd > 0.0f) || !(cd < 1.0f))
        return cd;   // not monotone across the face, so nothing to compress

    if (Scheme == VofHric) {
        // Downwind where the donor is under half full, full where it is over:
        // that is what pushes the interface back together instead of letting
        // the scheme average it away over ten cells.
        float value = (cd < 0.5f) ? 2.0f * cd : 1.0f;
        // An interface lying along the flow must not be compressed - doing it
        // anyway is what tears a smooth surface into flotsam.
        const float weight = std::sqrt(std::min(1.0f, std::fabs(cosTheta)));
        value = weight * value + (1.0f - weight) * cd;
        // And compression is only stable while the interface stays inside the
        // cell for the step, so it is faded out as the Courant number climbs.
        if (courant > 0.7f)
            return cd;
        if (courant > 0.3f)
            return cd + (value - cd) * (0.7f - courant) * 2.5f;
        return value;
    }

    // CICSAM: the same idea with the two bounds written out properly. Hyper-C
    // is the most downwind a face can be without going out of bounds at this
    // Courant number; ULTIMATE-QUICKEST is the smooth third order value, and
    // the interface angle decides which of the two the face gets.
    const float safeCo = std::max(courant, 1e-6f);
    const float hyperC = std::min(1.0f, cd / safeCo);
    const float uq =
        std::min(hyperC,
                 (8.0f * safeCo * cd + (1.0f - safeCo) * (6.0f * cd + 3.0f)) *
                     0.125f);
    const float gamma =
        std::min(1.0f, (1.0f + std::cos(2.0f * std::acos(
                                   std::min(1.0f, std::fabs(cosTheta))))) *
                           0.5f);
    return gamma * hyperC + (1.0f - gamma) * uq;
}

template <int Scheme>
inline float faceValue(float donor, float acceptor, float far,
                       float courant, float cosTheta) {
    if (Scheme == VofUpwind)
        return donor;
    const float cd = normalised(donor, acceptor, far);
    if (!(cd > 0.0f) || !(cd < 1.0f))
        return donor;
    const float cf = faceNormalised<Scheme>(cd, courant, cosTheta);
    return far + cf * (acceptor - far);
}

}   // namespace

void PhaseField::resize(int nxIn, int nyIn) {
    nx = nxIn;
    ny = nyIn;
    const size_t cells = static_cast<size_t>(nx) * ny;
    c.assign(cells, 0.0f);
    rho.assign(cells, rho2);
    mu.assign(cells, mu2);
    fluxX.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    fluxY.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    nuFaceX.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    nuFaceY.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    invRhoX.assign(static_cast<size_t>(nx + 1) * ny, 0.0f);
    invRhoY.assign(static_cast<size_t>(nx) * (ny + 1), 0.0f);
    inflowLeft.assign(static_cast<size_t>(ny), 0.0f);
    inflowRight.assign(static_cast<size_t>(ny), 0.0f);
    inflowBottom.assign(static_cast<size_t>(nx), 0.0f);
    inflowTop.assign(static_cast<size_t>(nx), 0.0f);
}

void PhaseField::setFluids(float rho1In, float rho2In,
                           float mu1In, float mu2In) {
    rho1 = rho1In;
    rho2 = rho2In;
    mu1 = mu1In;
    mu2 = mu2In;
}

void PhaseField::initialise(const Config& cfg,
                            const std::vector<int>& solid,
                            float dx,
                            float dy,
                            std::string& warning) {
    resize(cfg.nx, cfg.ny);
    setFluids(cfg.rho1, cfg.rho2, cfg.rho1 * cfg.nu1, cfg.rho2 * cfg.nu2);
    vof = cfg.vofScheme;
    (void)solid;

    if (cfg.phaseInit == PhaseInit::File) {
        std::string error;
        std::vector<float> loaded;
        if (loadPhaseFile(cfg.initialPhaseFile, nx, ny, loaded, error)) {
            c = std::move(loaded);
        } else {
            warning = error + " Falling back to a layer at half height.";
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                    c[static_cast<size_t>(j) * nx + i] =
                        (j + 0.5f) < 0.5f * ny ? 1.0f : 0.0f;
        }
    } else if (cfg.phaseInit == PhaseInit::Drop) {
        const float cx = cfg.phaseX * cfg.Lx;
        const float cy = cfg.phaseY * cfg.Ly;
        const float r = cfg.phaseLevel * 0.5f * std::min(cfg.Lx, cfg.Ly);
        for (int j = 0; j < ny; ++j) {
            const float y = (j + 0.5f) * dy;
            for (int i = 0; i < nx; ++i) {
                const float x = (i + 0.5f) * dx;
                const float d = std::sqrt((x - cx) * (x - cx) +
                                          (y - cy) * (y - cy));
                // A fraction rather than a step across the last cell, so the
                // circle does not start life as a staircase.
                const float t = 0.5f - (d - r) / std::max(dx, dy);
                c[static_cast<size_t>(j) * nx + i] =
                    std::min(1.0f, std::max(0.0f, t));
            }
        }
    } else if (cfg.phaseInit == PhaseInit::Column) {
        const float width = cfg.phaseX * cfg.Lx;
        const float height = cfg.phaseLevel * cfg.Ly;
        for (int j = 0; j < ny; ++j) {
            const float y = (j + 0.5f) * dy;
            for (int i = 0; i < nx; ++i) {
                const float x = (i + 0.5f) * dx;
                c[static_cast<size_t>(j) * nx + i] =
                    (x < width && y < height) ? 1.0f : 0.0f;
            }
        }
    } else {
        const float level = cfg.phaseLevel * cfg.Ly;
        for (int j = 0; j < ny; ++j) {
            const float y = (j + 0.5f) * dy;
            const float t = 0.5f - (y - level) / dy;
            const float value = std::min(1.0f, std::max(0.0f, t));
            for (int i = 0; i < nx; ++i)
                c[static_cast<size_t>(j) * nx + i] = value;
        }
    }

    for (int j = 0; j < ny; ++j) {
        inflowLeft[j] = c[static_cast<size_t>(j) * nx];
        inflowRight[j] = c[static_cast<size_t>(j) * nx + nx - 1];
    }
    for (int i = 0; i < nx; ++i) {
        inflowBottom[i] = c[i];
        inflowTop[i] = c[static_cast<size_t>(ny - 1) * nx + i];
    }
}

float PhaseField::maxCourant(const std::vector<float>& u,
                             const std::vector<float>& v,
                             float dx,
                             float dy) const {
    float worst = 0.0f;
    for (int j = 0; j < ny; ++j) {
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        for (int i = 0; i <= nx; ++i)
            worst = std::max(worst, std::fabs(u[rowU + i]) / dx);
    }
    for (int j = 0; j <= ny; ++j) {
        const size_t rowV = static_cast<size_t>(j) * nx;
        for (int i = 0; i < nx; ++i)
            worst = std::max(worst, std::fabs(v[rowV + i]) / dy);
    }
    return worst;
}

double PhaseField::totalVolume(float cellArea) const {
    double total = 0.0;
    for (float value : c)
        total += value;
    return total * cellArea;
}

void PhaseField::advect(const std::vector<float>& u,
                        const std::vector<float>& v,
                        const std::vector<uint8_t>& solid,
                        float dt,
                        float dx,
                        float dy) {
    switch (vof) {
    case VofScheme::Cicsam: advectImpl<VofCicsam>(u, v, solid, dt, dx, dy); return;
    case VofScheme::Upwind: advectImpl<VofUpwind>(u, v, solid, dt, dx, dy); return;
    default:                advectImpl<VofHric>(u, v, solid, dt, dx, dy);   return;
    }
}

template <int Scheme>
void PhaseField::advectImpl(const std::vector<float>& u,
                            const std::vector<float>& v,
                            const std::vector<uint8_t>& solid,
                            float dt,
                            float dx,
                            float dy) {
    const float* __restrict cPtr = c.data();
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    const uint8_t* __restrict solidPtr = solid.data();
    float* __restrict fx = fluxX.data();
    float* __restrict fy = fluxY.data();

    const float coX = dt / dx;
    const float coY = dt / dy;
    const float invDy4 = 0.25f / dy;
    const float invDx4 = 0.25f / dx;

    // ---- fluxes through the vertical faces ---------------------------------
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        const size_t rowUp = static_cast<size_t>(std::min(j + 1, ny - 1)) * nx;
        const size_t rowDown = static_cast<size_t>(std::max(j - 1, 0)) * nx;

        for (int i = 1; i < nx; ++i) {
            const float uf = uPtr[rowU + i];
            if (solidPtr[rowC + i] || solidPtr[rowC + i - 1] || uf == 0.0f) {
                fx[rowU + i] = 0.0f;
                continue;
            }
            const bool toRight = uf > 0.0f;
            const int donorIndex = toRight ? i - 1 : i;
            const int farIndex = toRight ? std::max(i - 2, 0)
                                         : std::min(i + 1, nx - 1);
            const float donor = cPtr[rowC + donorIndex];
            const float acceptor = cPtr[rowC + (toRight ? i : i - 1)];
            const float far = cPtr[rowC + farIndex];

            const float dcdx = (cPtr[rowC + i] - cPtr[rowC + i - 1]) / dx;
            const float dcdy =
                (cPtr[rowUp + i] + cPtr[rowUp + i - 1] -
                 cPtr[rowDown + i] - cPtr[rowDown + i - 1]) * invDy4;
            const float length =
                std::sqrt(dcdx * dcdx + dcdy * dcdy) + 1e-20f;

            fx[rowU + i] =
                uf * faceValue<Scheme>(donor, acceptor, far,
                                       std::fabs(uf) * coX,
                                       std::fabs(dcdx) / length);
        }

        // The two ends. An inlet carries the fluid that was on that side when
        // the run started, an outlet lets whatever is leaving leave, and a wall
        // has no velocity through it so the flux is zero either way.
        const float uLeft = uPtr[rowU];
        fx[rowU] = solidPtr[rowC]
                       ? 0.0f
                       : uLeft * (uLeft > 0.0f ? inflowLeft[j] : cPtr[rowC]);
        const float uRight = uPtr[rowU + nx];
        fx[rowU + nx] =
            solidPtr[rowC + nx - 1]
                ? 0.0f
                : uRight * (uRight < 0.0f ? inflowRight[j]
                                          : cPtr[rowC + nx - 1]);
    }

    // ---- fluxes through the horizontal faces -------------------------------
    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowBelow = static_cast<size_t>(j - 1) * nx;
        const size_t rowV = static_cast<size_t>(j) * nx;
        const size_t rowFar = static_cast<size_t>(std::max(j - 2, 0)) * nx;
        const size_t rowOver = static_cast<size_t>(std::min(j + 1, ny - 1)) * nx;

        for (int i = 0; i < nx; ++i) {
            const float vf = vPtr[rowV + i];
            if (solidPtr[rowC + i] || solidPtr[rowBelow + i] || vf == 0.0f) {
                fy[rowV + i] = 0.0f;
                continue;
            }
            const bool up = vf > 0.0f;
            const float donor = up ? cPtr[rowBelow + i] : cPtr[rowC + i];
            const float acceptor = up ? cPtr[rowC + i] : cPtr[rowBelow + i];
            const float far = up ? cPtr[rowFar + i] : cPtr[rowOver + i];

            const float dcdy = (cPtr[rowC + i] - cPtr[rowBelow + i]) / dy;
            const int left = std::max(i - 1, 0);
            const int right = std::min(i + 1, nx - 1);
            const float dcdx =
                (cPtr[rowC + right] + cPtr[rowBelow + right] -
                 cPtr[rowC + left] - cPtr[rowBelow + left]) * invDx4;
            const float length =
                std::sqrt(dcdx * dcdx + dcdy * dcdy) + 1e-20f;

            fy[rowV + i] =
                vf * faceValue<Scheme>(donor, acceptor, far,
                                       std::fabs(vf) * coY,
                                       std::fabs(dcdy) / length);
        }
    }

    const size_t topRow = static_cast<size_t>(ny) * nx;
    const size_t lastRow = static_cast<size_t>(ny - 1) * nx;
    for (int i = 0; i < nx; ++i) {
        const float vBottom = vPtr[i];
        fy[i] = solidPtr[i]
                    ? 0.0f
                    : vBottom * (vBottom > 0.0f ? inflowBottom[i] : cPtr[i]);
        const float vTop = vPtr[topRow + i];
        fy[topRow + i] =
            solidPtr[lastRow + i]
                ? 0.0f
                : vTop * (vTop < 0.0f ? inflowTop[i] : cPtr[lastRow + i]);
    }

    // ---- one conservative update, then back inside [0, 1] ------------------
    float* __restrict cOut = c.data();
#ifdef __AVX2__
    const __m256 coXVec = _mm256_set1_ps(coX);
    const __m256 coYVec = _mm256_set1_ps(coY);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 one = _mm256_set1_ps(1.0f);
#endif
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        const size_t rowV = static_cast<size_t>(j) * nx;
        const size_t rowVTop = static_cast<size_t>(j + 1) * nx;
        int i = 0;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= nx; i += 8) {
            const __m256 value = _mm256_loadu_ps(cOut + rowC + i);
            const __m256 dfx =
                _mm256_sub_ps(_mm256_loadu_ps(fx + rowU + i + 1),
                              _mm256_loadu_ps(fx + rowU + i));
            const __m256 dfy =
                _mm256_sub_ps(_mm256_loadu_ps(fy + rowVTop + i),
                              _mm256_loadu_ps(fy + rowV + i));
            const __m256 updated =
                _mm256_sub_ps(value,
                              _mm256_add_ps(_mm256_mul_ps(coXVec, dfx),
                                            _mm256_mul_ps(coYVec, dfy)));
            _mm256_storeu_ps(cOut + rowC + i,
                             _mm256_min_ps(one, _mm256_max_ps(zero, updated)));
        }
#endif
        for (; i < nx; ++i) {
            const float updated =
                cOut[rowC + i] -
                coX * (fx[rowU + i + 1] - fx[rowU + i]) -
                coY * (fy[rowVTop + i] - fy[rowV + i]);
            cOut[rowC + i] = std::min(1.0f, std::max(0.0f, updated));
        }
    }
}

void PhaseField::refreshProperties(const std::vector<uint8_t>& solid) {
    const float* __restrict cPtr = c.data();
    float* __restrict rhoPtr = rho.data();
    float* __restrict muPtr = mu.data();
    const size_t cells = static_cast<size_t>(nx) * ny;
    (void)solid;

#ifdef __AVX2__
    const __m256 rho1Vec = _mm256_set1_ps(rho1);
    const __m256 dRhoVec = _mm256_set1_ps(rho2 - rho1);
    const __m256 mu1Vec = _mm256_set1_ps(mu1);
    const __m256 dMuVec = _mm256_set1_ps(mu2 - mu1);
    const __m256 one = _mm256_set1_ps(1.0f);
#endif
    size_t id = 0;
#ifdef __AVX2__
    for (; runtime::avx2 && id + 8 <= cells; id += 8) {
        const __m256 value = _mm256_loadu_ps(cPtr + id);
        const __m256 rest = _mm256_sub_ps(one, value);
        _mm256_storeu_ps(rhoPtr + id,
                         _mm256_add_ps(rho1Vec, _mm256_mul_ps(rest, dRhoVec)));
        _mm256_storeu_ps(muPtr + id,
                         _mm256_add_ps(mu1Vec, _mm256_mul_ps(rest, dMuVec)));
    }
#endif
    for (; id < cells; ++id) {
        const float value = cPtr[id];
        rhoPtr[id] = value * rho1 + (1.0f - value) * rho2;
        muPtr[id] = value * mu1 + (1.0f - value) * mu2;
    }

    // Density is averaged harmonically across a face rather than arithmetically.
    // At a thousand to one the arithmetic mean of the two is half the heavy
    // one, so a face between water and air would carry the pressure gradient of
    // something five hundred times denser than the air on one side of it, and
    // the multigrid convergence falls apart on exactly that face.
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        for (int i = 1; i < nx; ++i) {
            const float a = rhoPtr[rowC + i - 1];
            const float b = rhoPtr[rowC + i];
            invRhoX[rowU + i] = 0.5f * (1.0f / a + 1.0f / b);
            nuFaceX[rowU + i] =
                0.5f * (muPtr[rowC + i - 1] + muPtr[rowC + i]) *
                invRhoX[rowU + i];
        }
        invRhoX[rowU] = 1.0f / rhoPtr[rowC];
        nuFaceX[rowU] = muPtr[rowC] * invRhoX[rowU];
        invRhoX[rowU + nx] = 1.0f / rhoPtr[rowC + nx - 1];
        nuFaceX[rowU + nx] = muPtr[rowC + nx - 1] * invRhoX[rowU + nx];
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowBelow = static_cast<size_t>(j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            const float a = rhoPtr[rowBelow + i];
            const float b = rhoPtr[rowC + i];
            invRhoY[rowC + i] = 0.5f * (1.0f / a + 1.0f / b);
            nuFaceY[rowC + i] =
                0.5f * (muPtr[rowBelow + i] + muPtr[rowC + i]) *
                invRhoY[rowC + i];
        }
    }
    const size_t topRow = static_cast<size_t>(ny) * nx;
    const size_t lastRow = static_cast<size_t>(ny - 1) * nx;
    for (int i = 0; i < nx; ++i) {
        invRhoY[i] = 1.0f / rhoPtr[i];
        nuFaceY[i] = muPtr[i] * invRhoY[i];
        invRhoY[topRow + i] = 1.0f / rhoPtr[lastRow + i];
        nuFaceY[topRow + i] = muPtr[lastRow + i] * invRhoY[topRow + i];
    }
}

bool loadPhaseFile(const std::string& path,
                   int nx,
                   int ny,
                   std::vector<float>& out,
                   std::string& error) {
    std::ifstream fin(path);
    if (!fin) {
        error = "Cannot open the initial phase file '" + path + "'.";
        return false;
    }

    const size_t want = static_cast<size_t>(nx) * ny;
    out.clear();
    out.reserve(want);

    std::string token;
    const auto flush = [&]() {
        if (token.empty())
            return true;
        try {
            out.push_back(std::min(1.0f, std::max(0.0f, std::stof(token))));
        } catch (...) {
            error = "The initial phase file has '" + token +
                    "' in it, which is not a number.";
            return false;
        }
        token.clear();
        return true;
    };

    char ch = 0;
    while (fin.get(ch)) {
        if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!flush())
                return false;
        } else {
            token.push_back(ch);
        }
    }
    if (!flush())
        return false;

    if (out.size() != want) {
        error = "The initial phase file holds " + std::to_string(out.size()) +
                " values and this grid has " + std::to_string(want) + " cells (" +
                std::to_string(nx) + " x " + std::to_string(ny) +
                "). Repaint it at this resolution, or set nx and ny to match.";
        out.clear();
        return false;
    }
    return true;
}
