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
    int dtUpdateInterval = 5;   // steps between dt recomputations
    float dtSafety = 0.9f;      // covers the velocity growth in between

    // SOR
    // omega is used on the coarsest multigrid level, where the smoother acts as
    // a solver. Inside a V-cycle the relaxation is clamped to smootherOmega,
    // because strong over-relaxation is a bad high-frequency smoother even
    // though it is a good stand-alone solver.
    float omega = 1.85f;
    float smootherOmega = 1.15f;

    // Multigrid controls
    int mgIterations = 2;        // V-cycles per pressure solve
    float mgTolerance = 1e-4f;   // relative residual ||r|| / ||rhs||
    int mgMinCoarseSize = 8;     // stop coarsening below this many cells per axis

    // Backend. Ignored when the binary was built without CUDA support.
    // Set to 0 to force the CPU path on a CUDA build, which is how the two
    // implementations are compared against each other.
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

    // Non-interactive setup: "key=value" pairs, used by the command line
    // front end and by regression runs. Returns false for an unknown key.
    bool setParam(const std::string& key, const std::string& value);
};
