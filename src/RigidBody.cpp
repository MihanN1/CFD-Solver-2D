#include "RigidBody.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
}

void RigidBody::sampleVelocity(double when) {
    if (!prescribed)
        return;
    if (keys.empty()) {
        vx = baseVx;
        vy = baseVy;
        omega = baseOmega;
        return;
    }

    const float t = static_cast<float>(when);
    if (t <= keys.front().time) {
        vx = keys.front().vx;
        vy = keys.front().vy;
        omega = keys.front().omega * kDegToRad;
        return;
    }
    if (t >= keys.back().time) {
        vx = keys.back().vx;
        vy = keys.back().vy;
        omega = keys.back().omega * kDegToRad;
        return;
    }

    std::size_t upper = 1;
    while (upper + 1 < keys.size() && keys[upper].time < t)
        ++upper;
    const BodyKeyframe& a = keys[upper - 1];
    const BodyKeyframe& b = keys[upper];
    const float span = b.time - a.time;
    const float w = span > 0.0f ? (t - a.time) / span : 0.0f;
    vx = lerp(a.vx, b.vx, w);
    vy = lerp(a.vy, b.vy, w);
    omega = lerp(a.omega, b.omega, w) * kDegToRad;
}

void RigidBody::integrate(float dt) {
    if (!free)
        return;

    const float translational = mass + addedMass;
    const float rotational = inertia + addedInertia;

    if (translational > 0.0f) {
        vx += dt * forceX / translational;
        vy += dt * forceY / translational;
    }
    if (rotational > 0.0f)
        omega += dt * torque / rotational;

    applyPins();
}

void RigidBody::applyPins() {
    if (pinX)
        vx = 0.0f;
    if (pinY)
        vy = 0.0f;
    if (pinRot)
        omega = 0.0f;
}

void RigidBody::advancePose(float dt) {
    x += static_cast<double>(vx) * dt;
    y += static_cast<double>(vy) * dt;
    theta += static_cast<double>(omega) * dt;
}

void buildRigidBodies(const std::vector<BodyMotion>& motions,
                      const std::vector<BodyGeometry>& geometry,
                      std::vector<RigidBody>& out,
                      float fluidDensity,
                      std::vector<std::string>& notes) {
    const int objectCount = static_cast<int>(geometry.size()) - 1;
    out.assign(geometry.size(), RigidBody());
    for (int id = 0; id <= objectCount; ++id) {
        out[id].object = id;
        out[id].cx = geometry[id].cx;
        out[id].cy = geometry[id].cy;
        out[id].radius = geometry[id].radius;
        out[id].area = geometry[id].area;
    }

    for (const BodyMotion& motion : motions) {
        if (motion.object < 1 || motion.object > objectCount) {
            notes.push_back(
                "bodyMotion moves object " + std::to_string(motion.object) +
                ", but this geometry has " + std::to_string(objectCount) +
                (objectCount == 1 ? " object" : " objects") +
                ". That part of the line does nothing.");
            continue;
        }

        RigidBody& body = out[motion.object];
        body.free = motion.free;
        body.prescribed = !motion.free;
        body.pinX = motion.pinX;
        body.pinY = motion.pinY;
        body.pinRot = motion.pinRot;
        body.baseVx = motion.vx;
        body.baseVy = motion.vy;
        body.baseOmega = motion.omega * kDegToRad;
        body.vx = motion.vx;
        body.vy = motion.vy;
        body.omega = motion.omega * kDegToRad;
        body.keys = motion.keys;
        body.mass = motion.mass;
        body.inertia = motion.inertia;
        if (motion.density > 0.0f)
            body.mass = motion.density * body.area;
        body.applyPins();
    }

    for (RigidBody& body : out) {
        if (!body.free)
            continue;
        if (body.inertia <= 0.0f)
            body.inertia = 0.5f * body.mass * body.radius * body.radius;
        body.addedMass = fluidDensity * body.area;
        body.addedInertia =
            0.125f * fluidDensity * body.area * body.radius * body.radius;
    }
}
