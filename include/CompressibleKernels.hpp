#pragma once
#include "SolverCompressible.hpp"

#include <cmath>

namespace cfd {

constexpr int kComponents = 5;
constexpr float kTiny = 1e-12f;
constexpr float kFloor = 1e-9f;

struct Primitive {
    float rho, u, v, p, y, gamma;
};

struct PrimitiveField {
    const float* rho = nullptr;
    const float* u = nullptr;
    const float* v = nullptr;
    const float* p = nullptr;
    const float* y = nullptr;
    const float* gamma = nullptr;

    CFD_HD Primitive at(int id) const {
        Primitive out;
        out.rho = rho[id];
        out.u = u[id];
        out.v = v[id];
        out.p = p[id];
        out.y = y[id];
        out.gamma = gamma[id];
        return out;
    }
};

CFD_HD inline float gammaOf(const GasModel& gas, float y) {
    if (!gas.species || !gas.active)
        return gas.gamma1;
    const float cp = gas.cp1 + y * (gas.cp2 - gas.cp1);
    const float cv = gas.cv1 + y * (gas.cv2 - gas.cv1);
    return cp / cv;
}

CFD_HD inline float gasConstantOf(const GasModel& gas, float y) {
    if (!gas.species || !gas.active)
        return gas.R1;
    return gas.R1 + y * (gas.R2 - gas.R1);
}

CFD_HD inline float clampTo(float value, float low, float high) {
    return fminf(fmaxf(value, low), high);
}

CFD_HD inline float limitSlope(float back, float forward, int kind) {
    if (back * forward <= 0.0f)
        return 0.0f;
    const float sign = back > 0.0f ? 1.0f : -1.0f;
    const float a = fabsf(back);
    const float b = fabsf(forward);
    if (kind == 0)
        return sign * fminf(a, b);
    if (kind == 2)
        return sign * fmaxf(fminf(2.0f * a, b), fminf(a, 2.0f * b));
    return 2.0f * back * forward / (back + forward);
}

CFD_HD inline Primitive primitiveOf(const Block& block,
                                    const GasModel& gas,
                                    int id) {
    Primitive out;
    out.rho = fmaxf(block.rho[id], kFloor);
    const float inv = 1.0f / out.rho;
    out.u = block.rhou[id] * inv;
    out.v = block.rhov[id] * inv;
    out.y = block.rhoY ? clampTo(block.rhoY[id] * inv, 0.0f, 1.0f) : 0.0f;
    out.gamma = gammaOf(gas, out.y);
    const float energy = block.rhoE[id] * inv;
    const float kinetic = 0.5f * (out.u * out.u + out.v * out.v);
    out.p = fmaxf((out.gamma - 1.0f) * out.rho * (energy - kinetic), kFloor);
    return out;
}

CFD_HD inline void conservativeOf(const Primitive& q, float* out) {
    out[0] = q.rho;
    out[1] = q.rho * q.u;
    out[2] = q.rho * q.v;
    out[3] = q.p / (q.gamma - 1.0f) + 0.5f * q.rho * (q.u * q.u + q.v * q.v);
    out[4] = q.rho * q.y;
}

CFD_HD inline void writeState(Block& block, int id, const Primitive& q) {
    float conserved[kComponents];
    conservativeOf(q, conserved);
    block.rho[id] = conserved[0];
    block.rhou[id] = conserved[1];
    block.rhov[id] = conserved[2];
    block.rhoE[id] = conserved[3];
    if (block.rhoY)
        block.rhoY[id] = conserved[4];
}

CFD_HD inline void hllc(const Primitive& left,
                        const Primitive& right,
                        bool alongX,
                        float* flux) {
    const float uL = alongX ? left.u : left.v;
    const float uR = alongX ? right.u : right.v;
    const float tL = alongX ? left.v : left.u;
    const float tR = alongX ? right.v : right.u;

    const float invRhoL = 1.0f / left.rho;
    const float invRhoR = 1.0f / right.rho;
    const float invGamL = 1.0f / (left.gamma - 1.0f);
    const float invGamR = 1.0f / (right.gamma - 1.0f);

    const float aL = sqrtf(left.gamma * left.p * invRhoL);
    const float aR = sqrtf(right.gamma * right.p * invRhoR);

    const float sL = fminf(uL - aL, uR - aR);
    const float sR = fmaxf(uL + aL, uR + aR);

    const float energyL =
        left.p * invGamL + 0.5f * left.rho * (uL * uL + tL * tL);
    const float energyR =
        right.p * invGamR + 0.5f * right.rho * (uR * uR + tR * tR);

    const float mL = left.rho * (sL - uL);
    const float mR = right.rho * (sR - uR);
    const float denominator = mL - mR;
    const float sM = (right.p - left.p + mL * uL - mR * uR) /
                     (fabsf(denominator) > kTiny ? denominator : kTiny);

    const float fL[kComponents] = {left.rho * uL,
                                   left.rho * uL * uL + left.p,
                                   left.rho * uL * tL,
                                   (energyL + left.p) * uL,
                                   left.rho * uL * left.y};
    const float fR[kComponents] = {right.rho * uR,
                                   right.rho * uR * uR + right.p,
                                   right.rho * uR * tR,
                                   (energyR + right.p) * uR,
                                   right.rho * uR * right.y};

    const float gapL = sL - sM;
    const float gapR = sR - sM;
    const float factorL = mL / (fabsf(gapL) > kTiny ? gapL : kTiny);
    const float factorR = mR / (fabsf(gapR) > kTiny ? gapR : kTiny);

    const float pushL = (sM - uL) * (sM + left.p / (mL != 0.0f ? mL : kTiny));
    const float pushR = (sM - uR) * (sM + right.p / (mR != 0.0f ? mR : kTiny));

    const float starL[kComponents] = {factorL, factorL * sM, factorL * tL,
                                      factorL * (energyL * invRhoL + pushL),
                                      factorL * left.y};
    const float starR[kComponents] = {factorR, factorR * sM, factorR * tR,
                                      factorR * (energyR * invRhoR + pushR),
                                      factorR * right.y};

    const float uLc[kComponents] = {left.rho, left.rho * uL, left.rho * tL,
                                    energyL, left.rho * left.y};
    const float uRc[kComponents] = {right.rho, right.rho * uR, right.rho * tR,
                                    energyR, right.rho * right.y};

    const float wL = sL >= 0.0f ? 1.0f : 0.0f;
    const float wR = (wL == 0.0f && sR <= 0.0f) ? 1.0f : 0.0f;
    const float middle = 1.0f - wL - wR;
    const float wSL = middle * (sM >= 0.0f ? 1.0f : 0.0f);
    const float wSR = middle * (sM < 0.0f ? 1.0f : 0.0f);

    for (int c = 0; c < kComponents; ++c) {
        const float starFluxL = fL[c] + sL * (starL[c] - uLc[c]);
        const float starFluxR = fR[c] + sR * (starR[c] - uRc[c]);
        flux[c] = wL * fL[c] + wSL * starFluxL + wSR * starFluxR + wR * fR[c];
    }

    if (!alongX) {
        const float swap = flux[1];
        flux[1] = flux[2];
        flux[2] = swap;
    }
}

CFD_HD inline Primitive reconstruct(const Primitive& centre,
                                    const Primitive& back,
                                    const Primitive& forward,
                                    float side,
                                    int limiter,
                                    const GasModel& gas) {
    Primitive out = centre;
    out.rho = centre.rho + side * 0.5f *
                               limitSlope(centre.rho - back.rho,
                                          forward.rho - centre.rho, limiter);
    out.u = centre.u + side * 0.5f * limitSlope(centre.u - back.u,
                                                forward.u - centre.u, limiter);
    out.v = centre.v + side * 0.5f * limitSlope(centre.v - back.v,
                                                forward.v - centre.v, limiter);
    out.p = centre.p + side * 0.5f * limitSlope(centre.p - back.p,
                                                forward.p - centre.p, limiter);
    out.y = centre.y + side * 0.5f * limitSlope(centre.y - back.y,
                                                forward.y - centre.y, limiter);
    out.rho = fmaxf(out.rho, kFloor);
    out.p = fmaxf(out.p, kFloor);
    out.y = clampTo(out.y, 0.0f, 1.0f);
    out.gamma = gammaOf(gas, out.y);
    return out;
}

CFD_HD inline void mirrorSide(Block& block,
                const GasModel& gas,
                const SideState& side,
                int i,
                int j,
                int mirrorI,
                int mirrorJ,
                bool horizontal,
                const BlockBoundaries& sides) {
    const int target = block.index(i, j);
    const int source = block.index(mirrorI, mirrorJ);
    Primitive q = primitiveOf(block, gas, source);

    switch (side.kind) {
    case BoundaryKind::Wall:
    case BoundaryKind::MovingWall:
        if (horizontal) {
            q.u = -q.u;
            q.v = side.noSlip ? 2.0f * side.speed - q.v : q.v;
        } else {
            q.v = -q.v;
            q.u = side.noSlip ? 2.0f * side.speed - q.u : q.u;
        }
        break;
    case BoundaryKind::Slip:
        if (horizontal)
            q.u = -q.u;
        else
            q.v = -q.v;
        break;
    case BoundaryKind::Inlet: {
        const float gamma = gas.gammaOf(q.y);
        const float gasR = gas.gasConstantOf(q.y);
        const float speedOfSound = sqrtf(gamma * gasR * sides.T0);
        const float speed = sides.mach * speedOfSound;
        const float open =
            side.banded ? ((sides.inletY >= side.from &&
                            sides.inletY <= side.to) ? 1.0f : 0.0f)
                        : 1.0f;
        if (open == 0.0f) {
            if (horizontal)
                q.u = -q.u;
            else
                q.v = -q.v;
            break;
        }
        const float density = sides.pInf / (gasR * sides.T0);
        q.rho = density;
        if (horizontal) {
            q.u = (i < 0) ? speed : -speed;
            q.v = 0.0f;
        } else {
            q.v = (j < 0) ? speed : -speed;
            q.u = 0.0f;
        }
        if (sides.mach >= 1.0f)
            q.p = sides.pInf;
        break;
    }
    case BoundaryKind::Outlet:
    default:
        const float gamma = gas.gammaOf(q.y);
        const float speedOfSound = sqrtf(gamma * q.p / q.rho);
        const float normal = horizontal ? fabsf(q.u) : fabsf(q.v);
        if (normal < speedOfSound)
            q.p = sides.pInf;
        break;
    }

    writeState(block, target, q);
}


CFD_HD inline bool touchesFluid(const Block& block, int i, int j) {
    const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int k = 0; k < 4; ++k) {
        const int ni = i + offsets[k][0];
        const int nj = j + offsets[k][1];
        if (ni < 0 || ni >= block.nx || nj < 0 || nj >= block.ny)
            continue;
        if (!block.solid[nj * block.nx + ni])
            return true;
    }
    return false;
}

CFD_HD inline void solidCell(Block& block,
                             const GasModel& gas,
                             int i,
                             int j,
                             int layer) {
    if (!block.solid || !block.solid[j * block.nx + i])
        return;
    const bool touching = touchesFluid(block, i, j);
    if (layer == 0 ? !touching : touching)
        return;

    float sumRho = 0.0f, sumU = 0.0f, sumV = 0.0f, sumP = 0.0f, sumY = 0.0f;
    int count = 0;
    int normalX = 0, normalY = 0;
    const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int k = 0; k < 4; ++k) {
        const int ni = i + offsets[k][0];
        const int nj = j + offsets[k][1];
        if (ni < 0 || ni >= block.nx || nj < 0 || nj >= block.ny)
            continue;
        const bool wanted = layer == 0
                                ? !block.solid[nj * block.nx + ni]
                                : (block.solid[nj * block.nx + ni] &&
                                   touchesFluid(block, ni, nj));
        if (!wanted)
            continue;
        const Primitive q = primitiveOf(block, gas, block.index(ni, nj));
        sumRho += q.rho;
        sumU += q.u;
        sumV += q.v;
        sumP += q.p;
        sumY += q.y;
        normalX += offsets[k][0];
        normalY += offsets[k][1];
        ++count;
    }
    if (count == 0)
        return;

    const float inv = 1.0f / static_cast<float>(count);
    Primitive q;
    q.rho = sumRho * inv;
    q.u = sumU * inv;
    q.v = sumV * inv;
    q.p = sumP * inv;
    q.y = sumY * inv;

    if (layer == 0) {
        const float length =
            sqrtf(static_cast<float>(normalX * normalX + normalY * normalY));
        if (length > 0.0f) {
            const float nxDir = normalX / length;
            const float nyDir = normalY / length;
            const float dot = q.u * nxDir + q.v * nyDir;
            q.u -= 2.0f * dot * nxDir;
            q.v -= 2.0f * dot * nyDir;
        } else {
            q.u = -q.u;
            q.v = -q.v;
        }
    } else {
        q.u = -q.u;
        q.v = -q.v;
    }
    q.gamma = gammaOf(gas, q.y);
    writeState(block, block.index(i, j), q);
}

CFD_HD inline void faceFluxX(const Block& in,
                             const PrimitiveField& prim,
                             const GasModel& gas,
                             int limiter,
                             int i,
                             int j,
                             float* face) {
    const int nx = in.nx;
    const int id = in.index(i, j);
    const Primitive backLeft = prim.at(id - 2);
    const Primitive centreLeft = prim.at(id - 1);
    const Primitive centreRight = prim.at(id);
    const Primitive forwardRight = prim.at(id + 1);

    const Primitive left =
        reconstruct(centreLeft, backLeft, centreRight, 1.0f, limiter, gas);
    const Primitive right =
        reconstruct(centreRight, centreLeft, forwardRight, -1.0f, limiter, gas);

    const bool solidLeft = in.solid && i > 0 && in.solid[j * nx + i - 1];
    const bool solidRight = in.solid && i < nx && in.solid[j * nx + i];
    if (solidLeft || solidRight) {
        const float wallPressure =
            solidLeft && solidRight ? 0.0f : (solidLeft ? right.p : left.p);
        face[0] = 0.0f;
        face[1] = wallPressure;
        face[2] = 0.0f;
        face[3] = 0.0f;
        face[4] = 0.0f;
        return;
    }
    hllc(left, right, true, face);
}

CFD_HD inline void faceFluxY(const Block& in,
                             const PrimitiveField& prim,
                             const GasModel& gas,
                             int limiter,
                             int i,
                             int j,
                             float* face) {
    const int nx = in.nx;
    const int ny = in.ny;
    const int id = in.index(i, j);
    const int step = in.stride;
    const Primitive backLow = prim.at(id - 2 * step);
    const Primitive centreLow = prim.at(id - step);
    const Primitive centreHigh = prim.at(id);
    const Primitive forwardHigh = prim.at(id + step);

    const Primitive low =
        reconstruct(centreLow, backLow, centreHigh, 1.0f, limiter, gas);
    const Primitive high =
        reconstruct(centreHigh, centreLow, forwardHigh, -1.0f, limiter, gas);

    const bool solidLow = in.solid && j > 0 && in.solid[(j - 1) * nx + i];
    const bool solidHigh = in.solid && j < ny && in.solid[j * nx + i];
    if (solidLow || solidHigh) {
        const float wallPressure =
            solidLow && solidHigh ? 0.0f : (solidLow ? high.p : low.p);
        face[0] = 0.0f;
        face[1] = 0.0f;
        face[2] = wallPressure;
        face[3] = 0.0f;
        face[4] = 0.0f;
        return;
    }
    hllc(low, high, false, face);
}

CFD_HD inline void combine(const Block& in,
                           const Block& keep,
                           Block& out,
                           const GasModel& gas,
                           const float* fx,
                           const float* fy,
                           int i,
                           int j,
                           float dt,
                           float a,
                           float b,
                           float diffusivity) {
    const int nx = in.nx;
    const int id = in.index(i, j);
    if (in.solid && in.solid[j * nx + i]) {
        out.rho[id] = in.rho[id];
        out.rhou[id] = in.rhou[id];
        out.rhov[id] = in.rhov[id];
        out.rhoE[id] = in.rhoE[id];
        if (out.rhoY)
            out.rhoY[id] = in.rhoY[id];
        return;
    }

    const float invDx = 1.0f / in.dx;
    const float invDy = 1.0f / in.dy;
    const long long xLow =
        (static_cast<long long>(j) * (nx + 1) + i) * kComponents;
    const long long xHigh = xLow + kComponents;
    const long long yLow = (static_cast<long long>(j) * nx + i) * kComponents;
    const long long yHigh =
        (static_cast<long long>(j + 1) * nx + i) * kComponents;

    const bool carries = in.rhoY != nullptr;
    const float current[kComponents] = {in.rho[id], in.rhou[id], in.rhov[id],
                                        in.rhoE[id],
                                        carries ? in.rhoY[id] : 0.0f};
    const float kept[kComponents] = {keep.rho[id], keep.rhou[id],
                                     keep.rhov[id], keep.rhoE[id],
                                     carries ? keep.rhoY[id] : 0.0f};

    float updated[kComponents];
    for (int c = 0; c < kComponents; ++c) {
        const float divergence = (fx[xHigh + c] - fx[xLow + c]) * invDx +
                                 (fy[yHigh + c] - fy[yLow + c]) * invDy;
        updated[c] = current[c] - dt * divergence;
    }

    if (carries && diffusivity > 0.0f) {
        const float here = in.rhoY[id] / fmaxf(in.rho[id], kFloor);
        const float east = in.rhoY[id + 1] / fmaxf(in.rho[id + 1], kFloor);
        const float west = in.rhoY[id - 1] / fmaxf(in.rho[id - 1], kFloor);
        const float north =
            in.rhoY[id + in.stride] / fmaxf(in.rho[id + in.stride], kFloor);
        const float south =
            in.rhoY[id - in.stride] / fmaxf(in.rho[id - in.stride], kFloor);
        const float laplacian = (east - 2.0f * here + west) * invDx * invDx +
                                (north - 2.0f * here + south) * invDy * invDy;
        updated[4] += dt * diffusivity * in.rho[id] * laplacian;
    }

    out.rho[id] = fmaxf(a * kept[0] + b * updated[0], kFloor);
    out.rhou[id] = a * kept[1] + b * updated[1];
    out.rhov[id] = a * kept[2] + b * updated[2];
    out.rhoE[id] = a * kept[3] + b * updated[3];
    if (out.rhoY)
        out.rhoY[id] = clampTo(a * kept[4] + b * updated[4], 0.0f, out.rho[id]);

    const float density = out.rho[id];
    const float kinetic =
        0.5f * (out.rhou[id] * out.rhou[id] + out.rhov[id] * out.rhov[id]) /
        density;
    const float internal = out.rhoE[id] - kinetic;
    const float y = out.rhoY ? out.rhoY[id] / density : 0.0f;
    const float floorEnergy = kFloor / (gammaOf(gas, y) - 1.0f);
    if (internal < floorEnergy)
        out.rhoE[id] = kinetic + floorEnergy;
}

CFD_HD inline float cellRate(const Block& block,
                             const GasModel& gas,
                             int i,
                             int j) {
    if (block.solid && block.solid[j * block.nx + i])
        return 0.0f;
    const Primitive q = primitiveOf(block, gas, block.index(i, j));
    const float speedOfSound = sqrtf(q.gamma * q.p / q.rho);
    return (fabsf(q.u) + speedOfSound) / block.dx +
           (fabsf(q.v) + speedOfSound) / block.dy;
}

CFD_HD inline void fillPrimitive(const Block& block,
                                 const GasModel& gas,
                                 int id,
                                 float* rho,
                                 float* u,
                                 float* v,
                                 float* p,
                                 float* y,
                                 float* gamma) {
    const float density = fmaxf(block.rho[id], kFloor);
    const float inv = 1.0f / density;
    const float velocityX = block.rhou[id] * inv;
    const float velocityY = block.rhov[id] * inv;
    const float fraction =
        block.rhoY ? clampTo(block.rhoY[id] * inv, 0.0f, 1.0f) : 0.0f;
    const float ratio = gammaOf(gas, fraction);
    const float energy = block.rhoE[id] * inv;
    const float kinetic =
        0.5f * (velocityX * velocityX + velocityY * velocityY);
    rho[id] = density;
    u[id] = velocityX;
    v[id] = velocityY;
    y[id] = fraction;
    gamma[id] = ratio;
    p[id] = fmaxf((ratio - 1.0f) * density * (energy - kinetic), kFloor);
}

}
