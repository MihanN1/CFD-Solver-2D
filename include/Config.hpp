#pragma once
#include <string>
#include <vector>

struct Config {
    bool restart = false;
    std::string restartFile = "";
    double addTime = 0.0;

    // Domain
    float Lx = 1.0, Ly = 1.0;
    int nx = 50, ny = 50;

    // Flow
    float U0 = 1.0;
    float nu = 0.01;

    // Density
    float ro = 1.225;   // kg/m^3

    bool gravityEnabled = false;
    float gravityAccel = 9.81f;   // m/s^2
    float gravityAngle = 0.0f;    // degrees, clockwise, 0 = down

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

    // One prompt = one whole line = one setParam, so a bad answer cannot
    // poison the stream for every prompt after it. Enter keeps the current
    // value. Returns false only when the input stream is dead.
    bool ask(const std::string& key, const std::string& prompt);

    // Current value of a key as text, for the [default] shown in the prompt.
    std::string currentValue(const std::string& key) const;

    void print() const;
    bool modifyParam(const std::string& name);
    bool confirm();   // returns true if confirmed, false if modified

    bool setParam(const std::string& key, const std::string& value);

    // Same, but says what exactly is wrong with the value instead of quietly
    // turning it into a zero. *warning is filled when the value is legal but
    // almost certainly not what was meant; the assignment still happens then.
    bool setParam(const std::string& key,
                  const std::string& value,
                  std::string& error,
                  std::string* warning = nullptr);

    // "nx" for "NX", "--nx", "'nx'". Empty when it is not a parameter at all.
    static std::string canonicalKey(const std::string& key);
    // Nearest parameter name for a typo, empty when nothing is close enough.
    static std::string suggestKey(const std::string& key);

    std::string serialize() const;
};
