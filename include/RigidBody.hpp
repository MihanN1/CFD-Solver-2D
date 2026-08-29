#pragma once
#include "Config.hpp"

#include <string>
#include <vector>

struct RigidBody {
    int object = 0;
    bool free = false;
    bool prescribed = false;

    float mass = 0.0f;
    float inertia = 0.0f;
    bool pinX = false, pinY = false, pinRot = false;

    double x = 0.0, y = 0.0, theta = 0.0;
    float vx = 0.0f, vy = 0.0f, omega = 0.0f;
    float cx = 0.0f, cy = 0.0f;
    float radius = 0.0f;
    float area = 0.0f;

    float addedMass = 0.0f;
    float addedInertia = 0.0f;

    float forceX = 0.0f, forceY = 0.0f, torque = 0.0f;

    std::vector<BodyKeyframe> keys;
    float baseVx = 0.0f, baseVy = 0.0f, baseOmega = 0.0f;

    void sampleVelocity(double when);
    void integrate(float dt);
    void advancePose(float dt);
    void applyPins();
};

struct BodyGeometry {
    float cx = 0.0f, cy = 0.0f;
    float radius = 0.0f;
    float area = 0.0f;
};

void buildRigidBodies(const std::vector<BodyMotion>& motions,
                      const std::vector<BodyGeometry>& geometry,
                      std::vector<RigidBody>& out,
                      float fluidDensity,
                      std::vector<std::string>& notes);
