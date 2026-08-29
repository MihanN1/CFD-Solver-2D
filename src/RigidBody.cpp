#include "RigidBody.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

constexpr float kPi = 3.14159265358979f;

float easeInOf(InterpKind kind, float t) {
    switch (kind) {
    case InterpKind::Sine:
        return 1.0f - std::cos(t * kPi * 0.5f);
    case InterpKind::Quad:
        return t * t;
    case InterpKind::Cubic:
        return t * t * t;
    case InterpKind::Quart:
        return t * t * t * t;
    case InterpKind::Quint:
        return t * t * t * t * t;
    case InterpKind::Expo:
        return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
    case InterpKind::Circ:
        return 1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t));
    case InterpKind::Back: {
        const float c = 1.70158f;
        return t * t * ((c + 1.0f) * t - c);
    }
    case InterpKind::Elastic: {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        const float p = 0.3f;
        return -std::pow(2.0f, 10.0f * (t - 1.0f)) *
               std::sin((t - 1.0f - p * 0.25f) * 2.0f * kPi / p);
    }
    default:
        return t;
    }
}

float bounceOut(float t) {
    const float n = 7.5625f;
    const float d = 2.75f;
    if (t < 1.0f / d)
        return n * t * t;
    if (t < 2.0f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    }
    if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}

EaseKind resolveEase(InterpKind kind, EaseKind ease) {
    if (ease != EaseKind::Auto)
        return ease;
    switch (kind) {
    case InterpKind::Back:
    case InterpKind::Bounce:
    case InterpKind::Elastic:
        return EaseKind::In;
    default:
        return EaseKind::Out;
    }
}

float shape(InterpKind kind, EaseKind ease, float t) {
    t = std::min(1.0f, std::max(0.0f, t));
    if (kind == InterpKind::Constant)
        return 0.0f;
    if (kind == InterpKind::Linear || kind == InterpKind::Bezier)
        return t;

    const EaseKind side = resolveEase(kind, ease);
    if (kind == InterpKind::Bounce) {
        switch (side) {
        case EaseKind::In:
            return 1.0f - bounceOut(1.0f - t);
        case EaseKind::InOut:
            return t < 0.5f ? 0.5f * (1.0f - bounceOut(1.0f - 2.0f * t))
                            : 0.5f * (1.0f + bounceOut(2.0f * t - 1.0f));
        default:
            return bounceOut(t);
        }
    }

    switch (side) {
    case EaseKind::In:
        return easeInOf(kind, t);
    case EaseKind::InOut:
        return t < 0.5f ? 0.5f * easeInOf(kind, 2.0f * t)
                        : 1.0f - 0.5f * easeInOf(kind, 2.0f - 2.0f * t);
    default:
        return 1.0f - easeInOf(kind, 1.0f - t);
    }
}
}

bool RigidBody::freeAt(double when) const {
    if (keys.empty())
        return free;
    const float t = static_cast<float>(when);
    if (t <= keys.front().time)
        return keys.front().free;
    std::size_t upper = 0;
    while (upper + 1 < keys.size() && keys[upper + 1].time <= t)
        ++upper;
    return keys[upper].free;
}

void RigidBody::sampleVelocity(double when) {
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
    const std::size_t lower = upper - 1;
    const BodyKeyframe& a = keys[lower];
    const BodyKeyframe& b = keys[upper];
    const float span = b.time - a.time;
    const float raw = span > 0.0f ? (t - a.time) / span : 0.0f;

    if (a.interp == InterpKind::Bezier) {
        const auto tangent = [&](std::size_t index, float BodyKeyframe::*field) {
            const float here = keys[index].*field;
            if (index == 0 || index + 1 >= keys.size()) {
                const std::size_t other = index == 0 ? 1 : index - 1;
                const float gap = keys[other].time - keys[index].time;
                return gap != 0.0f ? (keys[other].*field - here) / gap : 0.0f;
            }
            const float before = keys[index - 1].*field;
            const float after = keys[index + 1].*field;
            if ((here - before) * (after - here) <= 0.0f)
                return 0.0f;
            const float gap = keys[index + 1].time - keys[index - 1].time;
            return gap != 0.0f ? (after - before) / gap : 0.0f;
        };
        const auto hermite = [&](float BodyKeyframe::*field) {
            const float p0 = a.*field;
            const float p1 = b.*field;
            const float m0 = tangent(lower, field) * span;
            const float m1 = tangent(upper, field) * span;
            const float s = raw;
            const float s2 = s * s;
            const float s3 = s2 * s;
            return (2.0f * s3 - 3.0f * s2 + 1.0f) * p0 +
                   (s3 - 2.0f * s2 + s) * m0 +
                   (-2.0f * s3 + 3.0f * s2) * p1 +
                   (s3 - s2) * m1;
        };
        vx = hermite(&BodyKeyframe::vx);
        vy = hermite(&BodyKeyframe::vy);
        omega = hermite(&BodyKeyframe::omega) * kDegToRad;
        return;
    }

    const float w = shape(a.interp, a.ease, raw);
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

void RigidBody::step(double when, float dt) {
    const bool letGo = freeAt(when);
    if (letGo) {
        integrate(dt);
    } else {
        sampleVelocity(when);
        applyPins();
    }
    free = letGo;
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
        body.prescribed = true;
        body.everFree = motion.free;
        for (const BodyKeyframe& frame : motion.keys)
            if (frame.free)
                body.everFree = true;
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
        if (!body.everFree)
            continue;
        if (body.inertia <= 0.0f)
            body.inertia = 0.5f * body.mass * body.radius * body.radius;
        body.addedMass = fluidDensity * body.area;
        body.addedInertia =
            0.125f * fluidDensity * body.area * body.radius * body.radius;
    }
}
