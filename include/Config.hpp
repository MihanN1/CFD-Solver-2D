#pragma once
#include <string>
#include <vector>

struct Config {
    // Domain
    double Lx = 1.0, Ly = 1.0;
    int nx = 50, ny = 50;

    // Flow
    double U0 = 1.0;
    double nu = 0.01;
    double Re = 0.0;   // 0 means compute from U0, D, nu later

    // Density
    double ro = 1.225;   // kg/m^3

    // Time
    double CFL = 0.5;
    double totalTime = 10.0;

    // SOR
    double omega = 1.85;
    double tol = 1e-5;
    int maxIterSOR = 10000;

    // Geometry
    std::string geometryFile = "none";
    double sliceAngleX = 0.0;   // degrees
    double sliceAngleZ = 0.0;   // degrees
    double sliceRotation = 0.0;   // degrees
    bool invertSection = false; // doesn't allow to invert the model by default

    // Methods
    void readFromConsole();
    void print() const;
    bool modifyParam(const std::string& name);
    bool confirm();   // returns true if confirmed, false if modified

    // Density
    double ro = 1.225;
};