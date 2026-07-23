#pragma once
#include <string>
#include <vector>

struct Config {
    // Domain
    float Lx = 1.0, Ly = 1.0;
    int nx = 50, ny = 50;

    // Flow
    float U0 = 1.0;
    float nu = 0.01;
    double Re = 0.0;   // 0 means compute from U0, D, nu later

    // Density
    float ro = 1.225;   // kg/m^3

    // Time
    float CFL = 0.5;
    double totalTime = 10.0;

    // SOR
    float omega = 1.85;
    float tol = 1e-5;
    int maxIterSOR = 10000;

    // Geometry
    std::string geometryFile = "none";
    float sliceAngleX = 0.0;   // degrees
    float sliceAngleZ = 0.0;   // degrees
    float sliceRotation = 0.0;   // degrees
    bool invertSection = false; // doesn't allow to invert the model by default

    // Methods
    void readFromConsole();
    void print() const;
    bool modifyParam(const std::string& name);
    bool confirm();   // returns true if confirmed, false if modified
};
