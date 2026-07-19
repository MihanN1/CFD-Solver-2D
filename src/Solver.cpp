#include "Solver.hpp"
#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <algorithm>

static const int SAVE_INTERVAL = 1; // save every step, so that we can determine the mistakes and debug the code more easily. It can be changed to a larger number for faster simulations.

Solver::Solver(const Config& cfg, const Mesh& mesh)
    : cfg(cfg), mesh(mesh)
{
    // Allocate arrays
    p.resize(cfg.nx * cfg.ny, 0.0);
    u.resize((cfg.nx + 1) * cfg.ny, 0.0);
    v.resize(cfg.nx * (cfg.ny + 1), 0.0);
    u_star.resize((cfg.nx + 1) * cfg.ny, 0.0);
    v_star.resize(cfg.nx * (cfg.ny + 1), 0.0);
}

void Solver::initFields()
{
    std::fill(p.begin(), p.end(), 0.0);
    std::fill(u.begin(), u.end(), 0.0);
    std::fill(v.begin(), v.end(), 0.0);

    for (int j = 0; j < cfg.ny; j++) {
        for (int i = 0; i <= cfg.nx; i++) {
            bool solidLeft = false;
            bool solidRight = false;
            if (i > 0)
                solidLeft = mesh.solid[j*cfg.nx + (i-1)];
            if (i < cfg.nx)
                solidRight = mesh.solid[j*cfg.nx + i];
            if (!(solidLeft || solidRight))
                u[idxU(i,j)] = cfg.U0;
        }
    }
    applyBC();
    std::cout<<"Fields initialized.\n";
}

void Solver::computeDt() {
    // Find max absolute velocities
    double maxU = 0.0, maxV = 0.0;
    for (int j = 0; j < cfg.ny; ++j) {
        for (int i = 0; i <= cfg.nx; ++i) {
            maxU = std::max(maxU, std::abs(u[idxU(i, j)]));
        }
    }
    for (int j = 0; j <= cfg.ny; ++j) {
        for (int i = 0; i < cfg.nx; ++i) {
            maxV = std::max(maxV, std::abs(v[idxV(i, j)]));
        }
    }

    double vmax = std::max(maxU, maxV);
    double dtCFL;

    if (vmax < 1e-12)
        dtCFL = 1e9;
    else
        dtCFL = cfg.CFL * std::min(mesh.dx, mesh.dy) / vmax;
    // Also consider diffusive stability: dt <= 0.5 * dx^2 / nu
    double dtDiff = 0.5 * std::min(mesh.dx*mesh.dx, mesh.dy*mesh.dy) / cfg.nu;
    dt = std::min(dtCFL, dtDiff);
    // Guard against zero or negative
    if (dt <= 0.0) dt = 1e-6;
}

void Solver::predictor() {
    double dx = mesh.dx, dy = mesh.dy;
    double nu = cfg.nu;
    int nx = cfg.nx, ny = cfg.ny;

    // Compute u_star for internal fluid cells (i = 1..nx-1, j = 1..ny-2)
    // u is on vertical faces, so we need to compute convection and diffusion at those points
    for (int j = 1; j < ny-1; ++j) {
        for (int i = 1; i < nx; ++i) { // internal faces
            // Skip if solid
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[j * nx + (i - 1)] == 1) {
                u_star[idxU(i, j)] = 0.0;
                continue;
            }
            // Convection: upwind for u
            double u_ij = u[idxU(i, j)];
            double u_e = 0.25 * (
                u[idxU(i+1, j-1)] +
                u[idxU(i+1, j)] +
                u[idxU(i,   j-1)] +
                u[idxU(i,   j)]
            );
            double u_w = 0.25 * (
                u[idxU(i,   j-1)] +
                u[idxU(i,   j)] +
                u[idxU(i-1, j-1)] +
                u[idxU(i-1, j)]
            );
            double v_n = 0.25 * (
                v[idxV(i-1, j+1)] +
                v[idxV(i,   j+1)] +
                v[idxV(i-1, j)] +
                v[idxV(i,   j)]
            );
            double v_s = 0.25 * (
                v[idxV(i-1, j)] +
                v[idxV(i,   j)] +
                v[idxV(i-1, j-1)] +
                v[idxV(i,   j-1)]
            ); // v on horizontal faces
            
            double v_adv = 0.5 * (v_n + v_s);

            // du/dx at (i,j) using upwind
            double dudx = (u_ij > 0) ? (u_ij - u_w) / dx : (u_e - u_ij) / dx;
            // du/dy at (i,j) – need v and u values
            //  For upwind in y:
            double u_top = u[idxU(i, j+1)];
            double u_bot = u[idxU(i, j-1)];
            double u_right = u[idxU(i+1, j)];
            double u_left  = u[idxU(i-1, j)];
            double dudy = (v_adv > 0) ? (u_ij - u_bot) / dy : (u_top - u_ij) / dy;

            // Diffusion: central differences
            double d2udx2 = (u_right - 2.0*u_ij + u_left) / (dx*dx);
            double d2udy2 = (u_top - 2.0*u_ij + u_bot) / (dy*dy);

            u_star[idxU(i, j)] = u_ij + dt * (- (u_ij * dudx + v_adv * dudy) + nu * (d2udx2 + d2udy2));
        }
    }

    // Compute v_star similarly
    for (int j = 1; j < ny-1; ++j) { // internal horizontal faces
        for (int i = 1; i < nx-1; ++i) {
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[(j - 1) * nx + i] == 1) {
                v_star[idxV(i, j)] = 0.0;
                continue;
            }

            double v_ij = v[idxV(i, j)];
            double v_n = 0.25 * (
                v[idxV(i-1, j+1)] +
                v[idxV(i,   j+1)] +
                v[idxV(i-1, j)] +
                v[idxV(i,   j)]
            );
            double v_s = 0.25 * (
                v[idxV(i-1, j)] +
                v[idxV(i,   j)] +
                v[idxV(i-1, j-1)] +
                v[idxV(i,   j-1)]
            );
            double u_e = 0.25 * (
                u[idxU(i+1, j-1)] +
                u[idxU(i+1, j)] +
                u[idxU(i,   j-1)] +
                u[idxU(i,   j)]
            );
            double u_w = 0.25 * (
                u[idxU(i,   j-1)] +
                u[idxU(i,   j)] +
                u[idxU(i-1, j-1)] +
                u[idxU(i-1, j)]
            );

            double u_adv = 0.5 * (u_e + u_w);

            // dv/dx with upwind in x
            double dvdx = (u_adv > 0) ? (v_ij - v[idxV(i-1, j)]) / dx : (v[idxV(i+1, j)] - v_ij) / dx;
            // dv/dy with upwind in y
            double dvdy = (v_ij > 0) ? (v_ij - v_s) / dy : (v_n - v_ij) / dy;

            // Diffusion
            double v_right = v[idxV(i+1, j)];
            double v_left  = v[idxV(i-1, j)];
            double v_top = v[idxV(i, j+1)];
            double v_bot = v[idxV(i, j-1)];
            double d2vdx2 = (v_right - 2.0*v_ij + v_left) / (dx*dx);
            double d2vdy2 = (v_top - 2.0*v_ij + v_bot) / (dy*dy);

            v_star[idxV(i, j)] = v_ij + dt * (- (u_adv * dvdx + v_ij * dvdy) + nu * (d2vdx2 + d2vdy2));
        }
    }

    // Apply BC to u_star and v_star
    // Inlet (left): u_star = U0, v_star = 0
    for (int j = 0; j < ny; ++j) {
        u_star[idxU(0, j)] = cfg.U0;
        v_star[idxV(0, j)] = 0.0; // v on left face
    }
    // Outlet (right): zero gradient (neumann)
    for (int j = 0; j < ny; ++j) {
        u_star[idxU(nx, j)] = u_star[idxU(nx-1, j)];
        v_star[idxV(nx, j)] = v_star[idxV(nx-1, j)];
    }
    // Top/Bottom: slip or free-slip (we use zero gradient for u, v=0)
    for (int i = 0; i <= nx; ++i) {
        u_star[idxU(i, 0)] = u_star[idxU(i, 1)];
        // Верхняя граница существует только как ny-1
        u_star[idxU(i, ny-1)] = u_star[idxU(i, ny-2)];
    }
    for (int i = 0; i < nx; ++i) {
        v_star[idxV(i, 0)] = 0.0;   // bottom (no vertical flow)
        v_star[idxV(i, ny)] = 0.0;  // top
    }
}

void Solver::solvePoisson() {
    double dx = mesh.dx, dy = mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;
    double omega = cfg.omega;
    double tol = cfg.tol;
    int maxIter = cfg.maxIterSOR;

    // Precompute coefficients
    double invDx2 = 1.0 / (dx*dx);
    double invDy2 = 1.0 / (dy*dy);
    double coeff = 1.0 / (2.0 * (invDx2 + invDy2));

    int iter = 0;
    double residual = 1.0;

    while (iter < maxIter && residual > tol) {
        residual = 0.0;
        // SOR sweep over interior cells (excluding solid)
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (mesh.solid[j * nx + i] == 1) continue; // skip solid

                // Compute divergence of u_star, v_star at cell centre
                double div = (u_star[idxU(i+1, j)] - u_star[idxU(i, j)]) / dx
                           + (v_star[idxV(i, j+1)] - v_star[idxV(i, j)]) / dy;

                // Right-hand side of Poisson: div / dt
                double f = div / dt;

                // Compute explicit pressure from neighbours (use latest values)
                double p_e = (i+1 < nx && mesh.solid[j*nx + (i+1)] == 0) ? p[idxP(i+1, j)] : p[idxP(i, j)];
                double p_w = (i-1 >= 0 && mesh.solid[j*nx + (i-1)] == 0) ? p[idxP(i-1, j)] : p[idxP(i, j)];
                double p_n = (j+1 < ny && mesh.solid[(j+1)*nx + i] == 0) ? p[idxP(i, j+1)] : p[idxP(i, j)];
                double p_s = (j-1 >= 0 && mesh.solid[(j-1)*nx + i] == 0) ? p[idxP(i, j-1)] : p[idxP(i, j)];

                double p_explicit = coeff * ( (p_e + p_w) * invDx2 + (p_n + p_s) * invDy2 - f );

                double p_old = p[idxP(i, j)];
                double p_new = (1.0 - omega) * p_old + omega * p_explicit;
                p[idxP(i, j)] = p_new;

                // Compute residual (max absolute difference)
                double res = std::abs(p_new - p_old);
                if (res > residual) residual = res;
            }
        }
        iter++;
    }
    p[idxP(0,0)] = 0.0;
    // Optionally print residual
    // std::cout << "SOR iterations: " << iter << ", residual: " << residual << std::endl;
    // I think its not needed this much so its commentated by default
}

void Solver::corrector() {
    double dx = mesh.dx, dy = mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;

    // Update u: u_new = u_star - dt * (p(i+1) - p(i)) / dx
    for (int j = 0; j < ny; ++j) {
        for (int i = 1; i < nx; ++i) { // internal faces
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[j * nx + (i - 1)] == 1) {
                u[idxU(i, j)] = 0.0;
                continue;
            }
            // So gradient = (p[i] - p[i-1]) / dx
            double p_right = (i < nx && mesh.solid[j*nx + i] == 0) ? p[idxP(i, j)] : p[idxP(i-1, j)];
            double p_left  = (i-1 >= 0 && mesh.solid[j*nx + (i-1)] == 0) ? p[idxP(i-1, j)] : p[idxP(i, j)];
            double dpdx = (p_right - p_left) / dx;
            u[idxU(i, j)] = u_star[idxU(i, j)] - dt * dpdx;
        }
    }

    // Update v: v_new = v_star - dt * (p(j+1) - p(j)) / dy
    for (int j = 1; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[(j - 1) * nx + i] == 1) {
                v[idxV(i, j)] = 0.0;
                continue;
            }
            double p_top = (j < ny && mesh.solid[(j)*nx + i] == 0) ? p[idxP(i, j)] : p[idxP(i, j-1)];
            double p_bot = (j-1 >= 0 && mesh.solid[(j-1)*nx + i] == 0) ? p[idxP(i, j-1)] : p[idxP(i, j)];
            double dpdy = (p_top - p_bot) / dy;
            v[idxV(i, j)] = v_star[idxV(i, j)] - dt * dpdy;
        }
    }

    // Enforce no-slip inside solid and on boundaries
    for (int j = 0; j < ny; ++j) {
        for (int i = 1; i < nx; ++i) {
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[j * nx + (i - 1)] == 1) {
                u[idxU(i, j)] = 0.0;
            }
        }
    }
    for (int j = 0; j < ny; ++j) {
        u[idxU(0, j)] = 0.0;
        u[idxU(nx, j)] = 0.0;
    }
    for (int j = 1; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (mesh.solid[j * nx + i] == 1 || mesh.solid[(j - 1) * nx + i] == 1) {
                v[idxV(i, j)] = 0.0;
            }
        }
    }
    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0;
        v[idxV(i, ny)] = 0.0;
    }

    // Apply boundary conditions again
    applyBC();
}

void Solver::applyBC() {
    int nx = cfg.nx, ny = cfg.ny;
    // Inlet (left): u = U0, v = 0
    for (int j = 0; j < ny; ++j) {
        u[idxU(0, j)] = cfg.U0;
        v[idxV(0, j)] = 0.0;
    }
    // Outlet: zero gradient
    for (int j = 0; j < ny; ++j) {
        u[idxU(nx, j)] = u[idxU(nx-1, j)];
        v[idxV(nx, j)] = v[idxV(nx-1, j)];
    }
    // Top/Bottom: free slip (u gradient zero, v=0)
    for (int i = 0; i <= nx; ++i) {
        u[idxU(i, 0)] = u[idxU(i, 1)];
        u[idxU(i, ny-1)] = u[idxU(i, ny-2)];
    }
    for (int i = 0; i < nx; ++i) {
        v[idxV(i, 0)] = 0.0;
        v[idxV(i, ny)] = 0.0;
    }
}

void Solver::run() {
    std::cout << "Starting simulation...\n";
    initFields();
    currentTime = 0.0;
    step = 0;

    // Save initial state
    saveVTK(step);

    while (currentTime < cfg.totalTime) {
        computeDt();
        if (currentTime + dt > cfg.totalTime) dt = cfg.totalTime - currentTime; // avoid overshoot

        predictor();
        solvePoisson();
        corrector();

        currentTime += dt;
        step++;

        // Progress output
        if (step % 10 == 0) {
            std::cout << "Step " << step << ", time = " << currentTime << " s, dt = " << dt << std::endl;
        }

        // Save VTK periodically
        if (step % SAVE_INTERVAL == 0) {
            saveVTK(step);
        }
    }
    // Final save
    saveVTK(step);
    std::cout << "Simulation finished at t = " << currentTime << " s.\n";
    // Not sure about this, if it works its so cool
}

void Solver::saveVTK(int stepNum) const {
    int nx = cfg.nx, ny = cfg.ny;
    double dx = mesh.dx, dy = mesh.dy;

    std::string filename = "solution_" + std::to_string(stepNum) + ".vtk";
    std::ofstream fout(filename);
    if (!fout.is_open()) {
        std::cerr << "Cannot open " << filename << " for writing.\n";
        return;
    }

    fout << "# vtk DataFile Version 3.0\n";
    fout << "CFD-Solver-2D output, step " << stepNum << "\n";
    fout << "ASCII\n";
    fout << "DATASET STRUCTURED_POINTS\n";
    fout << "DIMENSIONS " << nx + 1 << " " << ny + 1 << " 1\n";
    fout << "ORIGIN 0 0 0\n";
    fout << "SPACING " << dx << " " << dy << " 0\n";
    fout << "CELL_DATA " << nx * ny << "\n";

    // Compute cell-centred fields
    std::vector<double> p_cell(nx * ny);
    std::vector<double> u_cell(nx * ny), v_cell(nx * ny);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            p_cell[j * nx + i] = p[idxP(i, j)] * cfg.ro; // physical pressure (Pa)
            // average u from left and right faces
            double u_left = u[idxU(i, j)];
            double u_right = u[idxU(i+1, j)];
            u_cell[j * nx + i] = 0.5 * (u_left + u_right);
            // average v from bottom and top faces
            double v_bot = v[idxV(i, j)];
            double v_top = v[idxV(i, j+1)];
            v_cell[j * nx + i] = 0.5 * (v_bot + v_top);
        }
    }

    // Now write fields (we already set ORIGIN to centre)
    fout << "SCALARS pressure float\n";
    fout << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            fout << p_cell[j * nx + i] << " ";
        }
        fout << "\n";
    }
    fout << "SCALARS solid int 1\n";
    fout << "LOOKUP_TABLE default\n";
    for (int j = 0; j < ny; ++j){
        for (int i = 0; i < nx; ++i)
        {
            fout << mesh.solid[j*nx+i] << " ";
        }
        fout << "\n";
    }
    fout << "VECTORS velocity float\n";
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            fout << u_cell[j * nx + i] << " " << v_cell[j * nx + i] << " 0\n";
        }
    }

    fout.close();
    std::cout << "Saved " << filename << std::endl;
}
