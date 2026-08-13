#pragma once
#include <string>
#include <vector>

struct Config {
    // Run mode. This is the first thing the configuration asks for: a fresh
    // run, or a continuation of one that already produced VTK frames.
    // restartFile is either a solution_*.vtk or the folder holding them, in
    // which case the newest frame in it is taken.
    bool restart = false;
    std::string restartFile = "";

    // Domain
    float Lx = 1.0, Ly = 1.0;
    int nx = 50, ny = 50;

    // Flow
    float U0 = 1.0;
    float nu = 0.01;

    // Density
    float ro = 1.225;   // kg/m^3

    // Time
    float CFL = 0.5;
    double totalTime = 10.0;
    int dtUpdateInterval = 5;   // steps between dt recomputations
    float dtSafety = 0.9f;      // covers the velocity growth in between

    float omega = 1.85f;
    float smootherOmega = 1.15f;

    // Multigrid controls
    int mgIterations = 2;        // V-cycles per pressure solve
    float mgTolerance = 1e-4f;   // relative residual ||r|| / ||rhs||
    int mgMinCoarseSize = 8;     // stop coarsening below this many cells per axis

    bool useCuda = true;

    // Output
    int saveInterval = 20;              // write a VTK file every N steps
    std::string outputDir = "output";   // directory for solution_*.vtk

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

    bool setParam(const std::string& key, const std::string& value);

    // The same "key=value" lines, one per parameter. This is what gets
    // embedded into every VTK frame so a continuation can restore the run
    // without the user retyping anything. Reading it back is just setParam().
    std::string serialize() const;
};
