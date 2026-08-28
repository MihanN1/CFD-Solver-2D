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
    VofCicsam,

    VofMinmod,
    VofVanLeer,
    VofSuperbee
};

constexpr float kLimiterFloor = 1e-20f;

template <int Scheme>
inline float limiterOf(float r) {
    switch (Scheme) {
    case VofMinmod:   return std::max(0.0f, std::min(1.0f, r));
    case VofSuperbee:
        return std::max(0.0f, std::max(std::min(2.0f * r, 1.0f),
                                       std::min(r, 2.0f)));
    default:          return (r + std::fabs(r)) / (1.0f + std::fabs(r));
    }
}

template <int Scheme>
inline bool smoothScheme() {
    return Scheme == VofMinmod || Scheme == VofVanLeer || Scheme == VofSuperbee;
}

inline float normalised(float donor, float acceptor, float far) {
    const float span = acceptor - far;
    if (std::fabs(span) < kFlat)
        return -1.0f;
    return (donor - far) / span;
}

template <int Scheme>
inline float faceNormalised(float cd, float courant, float cosTheta) {
    if (Scheme == VofUpwind)
        return cd;
    if (!(cd > 0.0f) || !(cd < 1.0f))
        return cd;

    if (Scheme == VofHric) {
        float value = (cd < 0.5f) ? 2.0f * cd : 1.0f;

        const float weight = std::sqrt(std::min(1.0f, std::fabs(cosTheta)));
        value = weight * value + (1.0f - weight) * cd;

        if (courant > 0.7f)
            return cd;
        if (courant > 0.3f)
            return cd + (value - cd) * (0.7f - courant) * 2.5f;
        return value;
    }

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
    if (smoothScheme<Scheme>()) {
        const float back = donor - far;
        const float fwd = acceptor - donor;
        const float safe = (std::fabs(fwd) > kLimiterFloor)
                               ? fwd
                               : (fwd < 0.0f ? -kLimiterFloor : kLimiterFloor);
        return donor + 0.5f * limiterOf<Scheme>(back / safe) * fwd;
    }
    const float cd = normalised(donor, acceptor, far);
    if (!(cd > 0.0f) || !(cd < 1.0f))
        return donor;
    const float cf = faceNormalised<Scheme>(cd, courant, cosTheta);
    return far + cf * (acceptor - far);
}

}

void PhaseField::resize(int nxIn, int nyIn) {
    nx = nxIn;
    ny = nyIn;
    const size_t cells = static_cast<size_t>(nx) * ny;
    c.assign(cells, 0.0f);
    rho.assign(cells, rho2);
    mu.assign(cells, mu2);
    invRhoCell.assign(cells, 1.0f / rho2);
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
    setMixing(cfg.mixing, cfg.limiter, cfg.diffusivity);
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

        constexpr int kSamples = 64;
        const float step = 1.0f / kSamples;
        const float weight = step * step;

        const float reach = 0.5f * std::sqrt(dx * dx + dy * dy);
        #pragma omp parallel for schedule(static)
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const float x = (i + 0.5f) * dx - cx;
                const float y = (j + 0.5f) * dy - cy;
                const float distance = std::sqrt(x * x + y * y);
                float value;
                if (distance < r - reach) {
                    value = 1.0f;
                } else if (distance > r + reach) {
                    value = 0.0f;
                } else {
                    float inside = 0.0f;
                    for (int sj = 0; sj < kSamples; ++sj) {
                        const float sy = (j + (sj + 0.5f) * step) * dy - cy;
                        for (int si = 0; si < kSamples; ++si) {
                            const float sx = (i + (si + 0.5f) * step) * dx - cx;
                            if (sx * sx + sy * sy <= r * r)
                                inside += weight;
                        }
                    }
                    value = inside;
                }
                c[static_cast<size_t>(j) * nx + i] = value;
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
    const float invDx = 1.0f / dx;
    const float invDy = 1.0f / dy;
    const int uCount = static_cast<int>(u.size());
    const int vCount = static_cast<int>(v.size());
    const float* __restrict uPtr = u.data();
    const float* __restrict vPtr = v.data();
    float worst = 0.0f;

    #pragma omp parallel if (uCount + vCount >= 8192)
    {
        float local = 0.0f;
#ifdef __AVX2__
        __m256 localVec = _mm256_setzero_ps();
        const __m256 mask =
            _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
        const __m256 invDxVec = _mm256_set1_ps(invDx);
        const __m256 invDyVec = _mm256_set1_ps(invDy);
#endif
        #pragma omp for schedule(static) nowait
        for (int start = 0; start < uCount; start += 8) {
#ifdef __AVX2__
            if (runtime::avx2 && start + 8 <= uCount) {
                localVec = _mm256_max_ps(
                    localVec,
                    _mm256_mul_ps(
                        _mm256_and_ps(mask, _mm256_loadu_ps(uPtr + start)),
                        invDxVec));
                continue;
            }
#endif
            for (int id = start; id < std::min(start + 8, uCount); ++id)
                local = std::max(local, std::fabs(uPtr[id]) * invDx);
        }
        #pragma omp for schedule(static) nowait
        for (int start = 0; start < vCount; start += 8) {
#ifdef __AVX2__
            if (runtime::avx2 && start + 8 <= vCount) {
                localVec = _mm256_max_ps(
                    localVec,
                    _mm256_mul_ps(
                        _mm256_and_ps(mask, _mm256_loadu_ps(vPtr + start)),
                        invDyVec));
                continue;
            }
#endif
            for (int id = start; id < std::min(start + 8, vCount); ++id)
                local = std::max(local, std::fabs(vPtr[id]) * invDy);
        }
#ifdef __AVX2__
        {
            alignas(32) float lanes[8];
            _mm256_store_ps(lanes, localVec);
            for (float value : lanes)
                local = std::max(local, value);
        }
#endif
        #pragma omp critical
        worst = std::max(worst, local);
    }
    return worst;
}

double PhaseField::totalVolume(float cellArea) const {
    double total = 0.0;
    const float* __restrict values = c.data();
    const int count = static_cast<int>(c.size());
    #pragma omp parallel for schedule(static) reduction(+ : total) \
        if (count >= 8192)
    for (int id = 0; id < count; ++id)
        total += static_cast<double>(values[id]);
    return total * cellArea;
}

void PhaseField::advect(const std::vector<float>& u,
                        const std::vector<float>& v,
                        const std::vector<uint8_t>& solid,
                        float dt,
                        float dx,
                        float dy) {
    if (mixing == MixingKind::Miscible) {
        switch (limiter) {
        case LimiterKind::Minmod:
            advectImpl<VofMinmod>(u, v, solid, dt, dx, dy);
            return;
        case LimiterKind::Superbee:
            advectImpl<VofSuperbee>(u, v, solid, dt, dx, dy);
            return;
        default:
            advectImpl<VofVanLeer>(u, v, solid, dt, dx, dy);
            return;
        }
    }
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

    const bool diffuse = smoothScheme<Scheme>() && diffusion > 0.0f;
    const float dcX = diffuse ? dt * diffusion / (dx * dx) : 0.0f;
    const float dcY = diffuse ? dt * diffusion / (dy * dy) : 0.0f;
    if (diffuse) {
        if (previous.size() != c.size())
            previous.resize(c.size());
        std::copy(c.begin(), c.end(), previous.begin());
    }
    const float* __restrict old = diffuse ? previous.data() : nullptr;

    float* __restrict cOut = c.data();
#ifdef __AVX2__
    const __m256 coXVec = _mm256_set1_ps(coX);
    const __m256 coYVec = _mm256_set1_ps(coY);
    const __m256 dcXVec = _mm256_set1_ps(dcX);
    const __m256 dcYVec = _mm256_set1_ps(dcY);
    const __m256 twoVec = _mm256_set1_ps(2.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 one = _mm256_set1_ps(1.0f);
#endif
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        const size_t rowV = static_cast<size_t>(j) * nx;
        const size_t rowVTop = static_cast<size_t>(j + 1) * nx;
        const size_t rowUp = static_cast<size_t>(std::min(j + 1, ny - 1)) * nx;
        const size_t rowDown = static_cast<size_t>(std::max(j - 1, 0)) * nx;

        const auto oneCell = [&](int i) {
            float updated =
                cOut[rowC + i] -
                coX * (fx[rowU + i + 1] - fx[rowU + i]) -
                coY * (fy[rowVTop + i] - fy[rowV + i]);
            if (diffuse) {
                const int left = std::max(i - 1, 0);
                const int right = std::min(i + 1, nx - 1);
                const float centre = old[rowC + i];
                updated +=
                    dcX * (old[rowC + right] + old[rowC + left] - 2.0f * centre) +
                    dcY * (old[rowUp + i] + old[rowDown + i] - 2.0f * centre);
            }
            cOut[rowC + i] = std::min(1.0f, std::max(0.0f, updated));
        };

        const int first = diffuse ? 1 : 0;
        const int limit = diffuse ? nx - 1 : nx;
        for (int i = 0; i < first; ++i)
            oneCell(i);
        int i = first;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= limit; i += 8) {
            const __m256 value = _mm256_loadu_ps(cOut + rowC + i);
            const __m256 dfx =
                _mm256_sub_ps(_mm256_loadu_ps(fx + rowU + i + 1),
                              _mm256_loadu_ps(fx + rowU + i));
            const __m256 dfy =
                _mm256_sub_ps(_mm256_loadu_ps(fy + rowVTop + i),
                              _mm256_loadu_ps(fy + rowV + i));
            __m256 updated =
                _mm256_sub_ps(value,
                              _mm256_add_ps(_mm256_mul_ps(coXVec, dfx),
                                            _mm256_mul_ps(coYVec, dfy)));
            if (diffuse) {
                const __m256 centre = _mm256_loadu_ps(old + rowC + i);
                updated = _mm256_add_ps(
                    updated,
                    _mm256_add_ps(
                        _mm256_mul_ps(
                            dcXVec,
                            _mm256_sub_ps(
                                _mm256_add_ps(_mm256_loadu_ps(old + rowC + i + 1),
                                              _mm256_loadu_ps(old + rowC + i - 1)),
                                _mm256_mul_ps(twoVec, centre))),
                        _mm256_mul_ps(
                            dcYVec,
                            _mm256_sub_ps(
                                _mm256_add_ps(_mm256_loadu_ps(old + rowUp + i),
                                              _mm256_loadu_ps(old + rowDown + i)),
                                _mm256_mul_ps(twoVec, centre)))));
            }
            _mm256_storeu_ps(cOut + rowC + i,
                             _mm256_min_ps(one, _mm256_max_ps(zero, updated)));
        }
#endif
        for (; i < nx; ++i)
            oneCell(i);
    }
}

void PhaseField::buildNormals(const std::vector<uint8_t>& solid,
                              float dx,
                              float dy,
                              float contactAngleDegrees) {
    const size_t cells = static_cast<size_t>(nx) * ny;
    if (normalX.size() != cells) {
        normalX.assign(cells, 0.0f);
        normalY.assign(cells, 0.0f);
        gradMag.assign(cells, 0.0f);
        kappa.assign(cells, 0.0f);
    }

    const float halfInvDx = 0.5f / dx;
    const float halfInvDy = 0.5f / dy;
    const float* __restrict cPtr = c.data();
    float* __restrict nxPtr = normalX.data();
    float* __restrict nyPtr = normalY.data();
    float* __restrict magPtr = gradMag.data();

#ifdef __AVX2__
    const __m256 halfInvDxVec = _mm256_set1_ps(halfInvDx);
    const __m256 halfInvDyVec = _mm256_set1_ps(halfInvDy);
#endif
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t row = static_cast<size_t>(j) * nx;
        const size_t up = static_cast<size_t>(std::min(j + 1, ny - 1)) * nx;
        const size_t down = static_cast<size_t>(std::max(j - 1, 0)) * nx;
        const auto oneCell = [&](int i) {
            const int left = std::max(i - 1, 0);
            const int right = std::min(i + 1, nx - 1);
            const float gx = (cPtr[row + right] - cPtr[row + left]) * halfInvDx;
            const float gy = (cPtr[up + i] - cPtr[down + i]) * halfInvDy;
            nxPtr[row + i] = gx;
            nyPtr[row + i] = gy;
            magPtr[row + i] = std::sqrt(gx * gx + gy * gy);
        };
        oneCell(0);
        int i = 1;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= nx - 1; i += 8) {
            const __m256 gx =
                _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(cPtr + row + i + 1),
                                            _mm256_loadu_ps(cPtr + row + i - 1)),
                              halfInvDxVec);
            const __m256 gy =
                _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(cPtr + up + i),
                                            _mm256_loadu_ps(cPtr + down + i)),
                              halfInvDyVec);
            _mm256_storeu_ps(nxPtr + row + i, gx);
            _mm256_storeu_ps(nyPtr + row + i, gy);
            _mm256_storeu_ps(magPtr + row + i,
                             _mm256_sqrt_ps(_mm256_add_ps(
                                 _mm256_mul_ps(gx, gx), _mm256_mul_ps(gy, gy))));
        }
#endif
        for (; i < nx; ++i)
            oneCell(i);
    }

    const float theta = contactAngleDegrees * 3.14159265358979f / 180.0f;
    const float cosT = std::cos(theta);
    const float sinT = std::sin(theta);
    const bool neutral = std::fabs(contactAngleDegrees - 90.0f) < 1e-3f;
    if (neutral)
        return;

    const auto rotate = [&](int i, int j, float wallX, float wallY) {
        const size_t id = static_cast<size_t>(j) * nx + i;
        const float length = magPtr[id];
        if (!(length > 1e-12f))
            return;

        float tx = -wallY, ty = wallX;
        if (nxPtr[id] * tx + nyPtr[id] * ty < 0.0f) {
            tx = -tx;
            ty = -ty;
        }
        nxPtr[id] = length * (wallX * cosT + tx * sinT);
        nyPtr[id] = length * (wallY * cosT + ty * sinT);
    };

    for (int j = 0; j < ny; ++j) {
        rotate(0, j, 1.0f, 0.0f);
        rotate(nx - 1, j, -1.0f, 0.0f);
    }
    for (int i = 0; i < nx; ++i) {
        rotate(i, 0, 0.0f, 1.0f);
        rotate(i, ny - 1, 0.0f, -1.0f);
    }
    if (!solid.empty()) {
        for (int j = 1; j < ny - 1; ++j)
            for (int i = 1; i < nx - 1; ++i) {
                const size_t id = static_cast<size_t>(j) * nx + i;
                if (solid[id])
                    continue;
                float wallX = 0.0f, wallY = 0.0f;
                if (solid[id - 1]) wallX += 1.0f;
                if (solid[id + 1]) wallX -= 1.0f;
                if (solid[id - nx]) wallY += 1.0f;
                if (solid[id + nx]) wallY -= 1.0f;
                const float length = std::sqrt(wallX * wallX + wallY * wallY);
                if (length > 0.0f)
                    rotate(i, j, wallX / length, wallY / length);
            }
    }
}

void PhaseField::computeCurvature(const std::vector<uint8_t>& solid,
                                  float dx,
                                  float dy,
                                  float contactAngleDegrees) {
    buildNormals(solid, dx, dy, contactAngleDegrees);

    const float* __restrict cPtr = c.data();
    const float* __restrict nxPtr = normalX.data();
    const float* __restrict nyPtr = normalY.data();
    const float* __restrict magPtr = gradMag.data();
    float* __restrict kPtr = kappa.data();

    constexpr int kStack = 3;
    constexpr int kMaxStack = 8;
    const float invDx2 = 1.0f / (dx * dx);
    const float invDy2 = 1.0f / (dy * dy);
    const float halfInvDx = 0.5f / dx;
    const float halfInvDy = 0.5f / dy;
    const float pure = 1e-3f;
    const float cellSpan = std::max(dx, dy);

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t row = static_cast<size_t>(j) * nx;
        for (int i = 0; i < nx; ++i) {
            const size_t id = row + i;
            kPtr[id] = 0.0f;

            if (magPtr[id] * cellSpan < pure)
                continue;
            if (!solid.empty() && solid[id])
                continue;

            const bool vertical = std::fabs(nyPtr[id]) >= std::fabs(nxPtr[id]);
            float height[3] = {0.0f, 0.0f, 0.0f};
            bool complete = false;

            for (int half = kStack; half <= kMaxStack && !complete; ++half) {
                complete = true;
                if (vertical) {
                    const int low = j - half, high = j + half;
                    if (low < 0 || high >= ny) { complete = false; break; }
                    for (int k = -1; k <= 1 && complete; ++k) {
                        const int column = std::min(std::max(i + k, 0), nx - 1);
                        const float bottom =
                            cPtr[static_cast<size_t>(low) * nx + column];
                        const float top =
                            cPtr[static_cast<size_t>(high) * nx + column];
                        if (std::fabs(bottom - top) < 1.0f - 4.0f * pure) {
                            complete = false;
                            break;
                        }
                        float total = 0.0f;
                        for (int m = low; m <= high; ++m)
                            total += cPtr[static_cast<size_t>(m) * nx + column];
                        height[k + 1] = total * dy;
                    }
                    if (complete) {
                        const float slope = (height[2] - height[0]) * halfInvDx;
                        const float second =
                            (height[2] - 2.0f * height[1] + height[0]) * invDx2;
                        kPtr[id] = -second /
                                   std::pow(1.0f + slope * slope, 1.5f);
                    }
                } else {
                    const int low = i - half, high = i + half;
                    if (low < 0 || high >= nx) { complete = false; break; }
                    for (int k = -1; k <= 1 && complete; ++k) {
                        const int rowIndex =
                            std::min(std::max(j + k, 0), ny - 1);
                        const size_t base = static_cast<size_t>(rowIndex) * nx;
                        if (std::fabs(cPtr[base + low] - cPtr[base + high]) <
                            1.0f - 4.0f * pure) {
                            complete = false;
                            break;
                        }
                        float total = 0.0f;
                        for (int m = low; m <= high; ++m)
                            total += cPtr[base + m];
                        height[k + 1] = total * dx;
                    }
                    if (complete) {
                        const float slope = (height[2] - height[0]) * halfInvDy;
                        const float second =
                            (height[2] - 2.0f * height[1] + height[0]) * invDy2;
                        kPtr[id] = -second /
                                   std::pow(1.0f + slope * slope, 1.5f);
                    }
                }
            }
            if (complete)
                continue;

            const int left = std::max(i - 1, 0);
            const int right = std::min(i + 1, nx - 1);
            const size_t up = static_cast<size_t>(std::min(j + 1, ny - 1)) * nx;
            const size_t down = static_cast<size_t>(std::max(j - 1, 0)) * nx;
            const auto unitX = [&](size_t at) {
                const float length = std::max(magPtr[at], 1e-12f);
                return nxPtr[at] / length;
            };
            const auto unitY = [&](size_t at) {
                const float length = std::max(magPtr[at], 1e-12f);
                return nyPtr[at] / length;
            };
            kPtr[id] = -((unitX(row + right) - unitX(row + left)) * halfInvDx +
                         (unitY(up + i) - unitY(down + i)) * halfInvDy);
        }
    }
}

void PhaseField::refreshProperties(const std::vector<uint8_t>& solid) {
    const float* __restrict cPtr = c.data();
    float* __restrict rhoPtr = rho.data();
    float* __restrict muPtr = mu.data();
    float* __restrict invRhoPtr = invRhoCell.data();
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
        const __m256 density =
            _mm256_add_ps(rho1Vec, _mm256_mul_ps(rest, dRhoVec));
        _mm256_storeu_ps(rhoPtr + id, density);
        _mm256_storeu_ps(muPtr + id,
                         _mm256_add_ps(mu1Vec, _mm256_mul_ps(rest, dMuVec)));

        _mm256_storeu_ps(invRhoPtr + id, _mm256_div_ps(one, density));
    }
#endif
    for (; id < cells; ++id) {
        const float value = cPtr[id];
        const float density = value * rho1 + (1.0f - value) * rho2;
        rhoPtr[id] = density;
        muPtr[id] = value * mu1 + (1.0f - value) * mu2;
        invRhoPtr[id] = 1.0f / density;
    }

    float* __restrict invX = invRhoX.data();
    float* __restrict invY = invRhoY.data();
    float* __restrict nuX = nuFaceX.data();
    float* __restrict nuY = nuFaceY.data();
#ifdef __AVX2__
    const __m256 halfVec = _mm256_set1_ps(0.5f);
#endif

    #pragma omp parallel for schedule(static)
    for (int j = 0; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowU = static_cast<size_t>(j) * (nx + 1);
        int i = 1;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= nx; i += 8) {
            const __m256 invLeft = _mm256_loadu_ps(invRhoPtr + rowC + i - 1);
            const __m256 invRight = _mm256_loadu_ps(invRhoPtr + rowC + i);
            const __m256 faceInv =
                _mm256_mul_ps(halfVec, _mm256_add_ps(invLeft, invRight));
            const __m256 muFace =
                _mm256_mul_ps(halfVec,
                              _mm256_add_ps(_mm256_loadu_ps(muPtr + rowC + i - 1),
                                            _mm256_loadu_ps(muPtr + rowC + i)));
            _mm256_storeu_ps(invX + rowU + i, faceInv);
            _mm256_storeu_ps(nuX + rowU + i, _mm256_mul_ps(muFace, faceInv));
        }
#endif
        for (; i < nx; ++i) {
            const float faceInv =
                0.5f * (invRhoPtr[rowC + i - 1] + invRhoPtr[rowC + i]);
            invX[rowU + i] = faceInv;
            nuX[rowU + i] =
                0.5f * (muPtr[rowC + i - 1] + muPtr[rowC + i]) * faceInv;
        }
        invX[rowU] = invRhoPtr[rowC];
        nuX[rowU] = muPtr[rowC] * invX[rowU];
        invX[rowU + nx] = invRhoPtr[rowC + nx - 1];
        nuX[rowU + nx] = muPtr[rowC + nx - 1] * invX[rowU + nx];
    }

    #pragma omp parallel for schedule(static)
    for (int j = 1; j < ny; ++j) {
        const size_t rowC = static_cast<size_t>(j) * nx;
        const size_t rowBelow = static_cast<size_t>(j - 1) * nx;
        int i = 0;
#ifdef __AVX2__
        for (; runtime::avx2 && i + 8 <= nx; i += 8) {
            const __m256 faceInv =
                _mm256_mul_ps(halfVec,
                              _mm256_add_ps(_mm256_loadu_ps(invRhoPtr + rowBelow + i),
                                            _mm256_loadu_ps(invRhoPtr + rowC + i)));
            const __m256 muFace =
                _mm256_mul_ps(halfVec,
                              _mm256_add_ps(_mm256_loadu_ps(muPtr + rowBelow + i),
                                            _mm256_loadu_ps(muPtr + rowC + i)));
            _mm256_storeu_ps(invY + rowC + i, faceInv);
            _mm256_storeu_ps(nuY + rowC + i, _mm256_mul_ps(muFace, faceInv));
        }
#endif
        for (; i < nx; ++i) {
            const float faceInv =
                0.5f * (invRhoPtr[rowBelow + i] + invRhoPtr[rowC + i]);
            invY[rowC + i] = faceInv;
            nuY[rowC + i] =
                0.5f * (muPtr[rowBelow + i] + muPtr[rowC + i]) * faceInv;
        }
    }
    const size_t topRow = static_cast<size_t>(ny) * nx;
    const size_t lastRow = static_cast<size_t>(ny - 1) * nx;
    for (int i = 0; i < nx; ++i) {
        invY[i] = invRhoPtr[i];
        nuY[i] = muPtr[i] * invY[i];
        invY[topRow + i] = invRhoPtr[lastRow + i];
        nuY[topRow + i] = muPtr[lastRow + i] * invY[topRow + i];
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
