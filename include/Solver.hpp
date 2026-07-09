#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include <vector>
#include <string>

class Solver {
public:
    Solver(const Config& cfg, const Mesh& mesh);
    // Main loop: run simulation until totalTime
    void run();

private:
    const Config& cfg;
    const Mesh& mesh;

    // Fields on staggered grid
    // Pressure (cell centres): size nx * ny
    std::vector<double> p;
    // u on vertical faces: size (nx+1) * ny
    std::vector<double> u, u_star;
    // v on horizontal faces: size nx * (ny+1)
    std::vector<double> v, v_star;

    double currentTime = 0.0;
    int step = 0;
    double dt = 0.0;  // current time step

    // Helper methods
    void initFields();
    void computeDt();
    void predictor();
    void solvePoisson();
    void corrector();
    void applyBC();

    // VTK output
    void saveVTK(int stepNum) const;

    // Interpolation helpers for VTK (to get values at nodes)
    double interpU(int i, int j) const; // u at node (i,j)
    double interpV(int i, int j) const;
    double interpP(int i, int j) const;

    // Inline index helpers (for readability)
    inline int idxP(int i, int j) const { return j * cfg.nx + i; }
    inline int idxU(int i, int j) const { return j * (cfg.nx + 1) + i; }
    inline int idxV(int i, int j) const { return j * cfg.nx + i; } // v has nx columns
};