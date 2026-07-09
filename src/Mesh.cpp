#include "Mesh.hpp"
#include <cmath>
#include <iostream>

Mesh::Mesh(const Config& cfg)
    : nx(cfg.nx), ny(cfg.ny)
{
    dx = cfg.Lx / nx;
    dy = cfg.Ly / ny;

    x.resize((nx + 1) * (ny + 1));
    y.resize((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            x[j * (nx + 1) + i] = i * dx;
            y[j * (nx + 1) + i] = j * dy;
        }
    }

    solid.resize(nx * ny, 0);

    // Place a circle in the centre (10% of min dimension)
    double cx = cfg.Lx / 2.0;
    double cy = cfg.Ly / 2.0;
    double R = 0.1 * std::min(cfg.Lx, cfg.Ly);
    initCircle(cx, cy, R);
}

void Mesh::initCircle(double cx, double cy, double R) {
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            double xc = (i + 0.5) * dx;
            double yc = (j + 0.5) * dy;
            double dist = std::sqrt((xc - cx) * (xc - cx) + (yc - cy) * (yc - cy));
            if (dist <= R)
                solid[j * nx + i] = 1;
            else
                solid[j * nx + i] = 0;
        }
    }
}

void Mesh::printInfo() const {
    std::cout << "\n=== Mesh Information ===\n";
    std::cout << "  nx = " << nx << ", ny = " << ny << "\n";
    std::cout << "  dx = " << dx << ", dy = " << dy << "\n";
    int count = 0;
    for (int v : solid) if (v) ++count;
    std::cout << "  Number of solid cells = " << count << "\n";
    std::cout << "=========================\n";
}