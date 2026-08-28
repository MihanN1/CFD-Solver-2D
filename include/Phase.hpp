#pragma once
#include "Config.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The volume fraction of fluid 1 in every cell, and everything that is done to
// it: carrying it with the flow without letting the interface dissolve, keeping
// it inside [0, 1], and turning it into the density and viscosity the momentum
// equation and the pressure solve read.
//
// The field lives on cell centres like the pressure, and is carried by the same
// face velocities the projection produced, so it is transported by a field that
// is already divergence free - which is the only reason the sum of c stays put
// at all.
class PhaseField {
public:
    void initialise(const Config& cfg,
                    const std::vector<int>& solid,
                    float dx,
                    float dy,
                    std::string& warning);

    // One explicit step of dc/dt + u.grad c = 0 in conservative form, with the
    // face value chosen by the compressive scheme. u and v are the face
    // velocities after the projection, so what is carried is a divergence free
    // field and the total of c only moves through the open sides.
    void advect(const std::vector<float>& u,
                const std::vector<float>& v,
                const std::vector<uint8_t>& solid,
                float dt,
                float dx,
                float dy);

    // rho and mu per cell, and 1/rho on each face for the pressure operator and
    // the corrector. Recomputed after every advect, which is cheap next to
    // everything else in a step.
    void refreshProperties(const std::vector<uint8_t>& solid);

    // Largest fraction of a cell the interface crosses in one step, which is
    // what limits dt independently of the momentum equation: an explicit
    // compressive scheme is unconditionally wrong above a Courant number of
    // about 0.5 no matter how stable the velocity field is.
    float maxCourant(const std::vector<float>& u,
                     const std::vector<float>& v,
                     float dx,
                     float dy) const;

    double totalVolume(float cellArea) const;

    bool active() const { return nx > 0; }

    const std::vector<float>& fraction() const { return c; }
    std::vector<float>& fraction() { return c; }
    const std::vector<float>& density() const { return rho; }
    const std::vector<float>& viscosity() const { return mu; }
    // Kinematic viscosity at the face, which is what the predictor multiplies
    // the Laplacian by. Sized like u and v.
    const std::vector<float>& faceNuX() const { return nuFaceX; }
    const std::vector<float>& faceNuY() const { return nuFaceY; }
    // 1/rho at the face: the coefficient in div((1/rho) grad p) and the factor
    // the corrector scales the pressure gradient by. The same numbers, so they
    // are built once and both read them - if they ever disagreed the projected
    // field would not be divergence free and nothing would say so.
    const std::vector<float>& faceInvRhoX() const { return invRhoX; }
    const std::vector<float>& faceInvRhoY() const { return invRhoY; }

    float rhoOf(float fraction) const {
        return fraction * rho1 + (1.0f - fraction) * rho2;
    }

    void resize(int nxIn, int nyIn);
    void setFluids(float rho1In, float rho2In, float mu1In, float mu2In);
    void setScheme(VofScheme scheme) { vof = scheme; }

private:
    int nx = 0, ny = 0;
    float rho1 = 1000.0f, rho2 = 1.225f;
    float mu1 = 1e-3f, mu2 = 1.8e-5f;
    VofScheme vof = VofScheme::Hric;

    std::vector<float> c;
    std::vector<float> fluxX, fluxY;
    // What an inlet brings in, taken from the initial field at that side. A
    // channel filled with water below and air above then blows the right fluid
    // in on every row without anybody having to say so twice.
    std::vector<float> inflowLeft, inflowRight, inflowBottom, inflowTop;
    std::vector<float> rho, mu;
    std::vector<float> nuFaceX, nuFaceY;
    std::vector<float> invRhoX, invRhoY;

    template <int Scheme>
    void advectImpl(const std::vector<float>& u,
                    const std::vector<float>& v,
                    const std::vector<uint8_t>& solid,
                    float dt,
                    float dx,
                    float dy);
};

// Reads a painted field: one number per cell, whitespace or comma separated,
// row 0 first, exactly nx*ny of them. Anything else is an error naming what was
// found, because a field that is silently half read is a run that looks right
// and is not.
bool loadPhaseFile(const std::string& path,
                   int nx,
                   int ny,
                   std::vector<float>& out,
                   std::string& error);
