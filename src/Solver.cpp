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
    p.assign(cfg.nx * cfg.ny, 0.0f);
    u.assign((cfg.nx + 1) * cfg.ny, 0.0f);
    v.assign(cfg.nx * (cfg.ny + 1), 0.0f);
    u_star.assign((cfg.nx + 1) * cfg.ny, 0.0f);
    v_star.assign(cfg.nx * (cfg.ny + 1), 0.0f);
}

void Solver::initFields()
{
    std::fill(p.begin(), p.end(), 0.0f);
    std::fill(u.begin(), u.end(), 0.0f);
    std::fill(v.begin(), v.end(), 0.0f);

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
    float maxU = 0.0f, maxV = 0.0f;
    for (int j = 0; j < cfg.ny; ++j) {
        for (int i = 0; i <= cfg.nx; ++i) {
            maxU = std::max(maxU, std::fabsf(u[idxU(i, j)]));
        }
    }
    for (int j = 0; j <= cfg.ny; ++j) {
        for (int i = 0; i < cfg.nx; ++i) {
            maxV = std::max(maxV, std::fabsf(v[idxV(i, j)]));
        }
    }
    const float invDx = 1.f / mesh.dx;
    const float invDy = 1.f / mesh.dy;

    float advDenom = maxU * invDx + maxV * invDy;
    double dtAdv;
    if (advDenom < 1e-12)
        dtAdv = 1e9;
    else
        dtAdv = cfg.CFL / advDenom;
    double invDx2 = 1.0f * invDx * invDx;
    double invDy2 = 1.0f * invDy * invDy;
    double dtDiff = 1.0f /
        (2.0f * cfg.nu * (invDx2 + invDy2));

    dt = std::min(dtAdv, dtDiff);
    // Guard against zero or negative
    if (dt <= 0.0 || std::isnan(dt) || std::isinf(dt))
        dt = 1e-6;
}

void Solver::predictor() {
    const float invDx  = 1.0f / mesh.dx;
    const float invDy  = 1.0f / mesh.dy;
    const float invDx2 = invDx * invDx;
    const float invDy2 = invDy * invDy;
    float nu = cfg.nu;
    int nx = cfg.nx, ny = cfg.ny;

    // Compute u_star for internal fluid cells (i = 1..nx-1, j = 1..ny-2)
    // u is on vertical faces, so we need to compute convection and diffusion at those points
    for (int j = 1; j < ny-1; ++j) {
        const int rowU = j * (nx + 1);
        const int rowUTop = (j + 1) * (nx + 1);
        const int rowUBot = (j - 1) * (nx + 1);
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        for (int i = 1; i < nx; ++i) { // internal faces
            // Skip if solid
            if (mesh.solid[j * nx + i] || mesh.solid[j * nx + (i - 1)]) {
                u_star[rowU + i] = 0.0;
                continue;
            }
            // Convection: upwind for u
            float u_ij = u[rowU + i];
            float v_n = 0.25f * (
                v[rowVTop + (i-1)] +
                v[rowVTop + i] +
                v[rowV + (i-1)] +
                v[rowV + i]
            );
            

            float u_top = u[rowUTop + i];
            float u_bot = u[rowUBot + i];
            float u_right = u[rowU + (i+1)];
            float u_left  = u[rowU + (i-1)];
            float dudy = (v_n > 0) ? (u_ij - u_bot) * invDy : (u_top - u_ij) * invDy;
            float dudx = (u_ij > 0) ? (u_ij - u_left) * invDx : (u_right - u_ij) * invDx;
            // Diffusion: central differences
            float d2udx2 = (u_right - 2.0f*u_ij + u_left) * invDx2;
            float d2udy2 = (u_top - 2.0f*u_ij + u_bot) * invDy2;

            u_star[rowU + i] = u_ij + dt * (- (u_ij * dudx + v_n * dudy) + nu * (d2udx2 + d2udy2));
        }
    }

    // Compute v_star similarly
    for (int j = 1; j < ny; ++j) { // internal horizontal faces
        const int rowV = j * nx;
        const int rowVTop = (j + 1) * nx;
        const int rowVBot = (j - 1) * nx;
        const int rowU = j * (nx + 1);
        const int rowUBot = (j - 1) * (nx + 1);
        for (int i = 1; i < nx-1; ++i) {
            if (mesh.solid[j * nx + i] || mesh.solid[(j - 1) * nx + i]) {
                v_star[rowV + i] = 0.0;
                continue;
            }

            float v_ij = v[rowV + i];
            float u_e = 0.25f * (
                u[rowU + (i+1)] +
                u[rowUBot + (i+1)] +
                u[rowU + i] +
                u[rowUBot + i]
            );
            float v_right = v[rowV + (i+1)];
            float v_left  = v[rowV + (i-1)];
            float v_top = v[rowVTop + i];
            float v_bot = v[rowVBot + i];
            // dv/dx with upwind in x
            float dvdx = (u_e > 0) ? (v_ij - v_left) * invDx : (v_right - v_ij) * invDx;
            // dv/dy with upwind in y
            float dvdy = (v_ij > 0) ? (v_ij - v_bot) * invDy : (v_top - v_ij) * invDy;

            // Diffusion
            float d2vdx2 = (v_right - 2.0f*v_ij + v_left) * invDx2;
            float d2vdy2 = (v_top - 2.0f*v_ij + v_bot) * invDy2;

            v_star[rowV + i] = v_ij + dt * (- (u_e * dvdx + v_ij * dvdy) + nu * (d2vdx2 + d2vdy2));
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
    }
    // Top/Bottom: slip or free-slip (we use zero gradient for u, v=0)
    for (int i = 0; i <= nx; ++i) {
        u_star[idxU(i, 0)] = u_star[idxU(i, 1)];
        u_star[idxU(i, ny-1)] = u_star[idxU(i, ny-2)];
    }
    for (int i = 0; i < nx; ++i) {
        v_star[idxV(i, 0)] = 0.0;   // bottom (no vertical flow)
        v_star[idxV(i, ny)] = 0.0;  // top
    }
}

void Solver::solvePoisson() {
    const float invDx = 1.f / mesh.dx, invDy = 1.f / mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;
    float omega = cfg.omega;
    float tol = cfg.tol;
    int maxIter = cfg.maxIterSOR;

    // Precompute coefficients
    float invDx2 = 1.0f * invDx * invDx;
    float invDy2 = 1.0f * invDy * invDy;
    float coeff = 1.0f / (2.0f * (invDx2 + invDy2));

    int iter = 0;
    float residual = 1.0f;

    while (iter < maxIter && residual > tol) {
        residual = 0.0f;
        // SOR sweep over interior cells (excluding solid)
        for (int j = 0; j < ny; ++j) {
            const int row = j * nx;
            const int rowTop = (j + 1) * nx;
            const int rowBot = (j - 1) * nx;
            for (int i = 0; i < nx; ++i) {
                if (mesh.solid[row + i]) continue; // skip solid

                // Compute divergence of u_star, v_star at cell centre
                float div = (u_star[row + (i+1)] - u_star[row + i]) * invDx
                           + (v_star[rowTop + i] - v_star[row + i]) * invDy;

                // Right-hand side of Poisson: div / dt
                float f = div / static_cast<float>(dt);

                // Compute explicit pressure from neighbours (use latest values)
                float p_e = (i+1 < nx && !mesh.solid[row + (i+1)]) ? p[row + (i+1)] : p[row + i];
                float p_w = (i-1 >= 0 && !mesh.solid[row + (i-1)]) ? p[row + (i-1)] : p[row + i];
                float p_n = (j+1 < ny && !mesh.solid[rowTop + i]) ? p[rowTop + i] : p[row + i];
                float p_s = (j-1 >= 0 && !mesh.solid[rowBot + i]) ? p[rowBot + i] : p[row + i];

                float p_explicit = coeff * ( (p_e + p_w) * invDx2 + (p_n + p_s) * invDy2 - f );

                float p_old = p[row + i];
                float p_new = (1.0 - omega) * p_old + omega * p_explicit;
                p[row + i] = p_new;

                // Residual of Poisson equation
                const float p_c = p[row + i];
                const float p_e_res =
                    (i + 1 < nx && !mesh.solid[row + i + 1])
                        ? p[row + i + 1]
                        : p_c;
                const float p_w_res =
                    (i > 0 && !mesh.solid[row + i - 1])
                        ? p[row + i - 1]
                        : p_c;
                const float p_n_res =
                    (j + 1 < ny && !mesh.solid[rowTop + i])
                        ? p[rowTop + i]
                        : p_c;
                const float p_s_res =
                    (j > 0 && !mesh.solid[rowBot + i])
                        ? p[rowBot + i]
                        : p_c;
                const float laplace =
                    (p_e_res - 2.f*p_c + p_w_res) * invDx2 +
                    (p_n_res - 2.f*p_c + p_s_res) * invDy2;
                const float res = fabsf(laplace - f);
                if(res > residual)
                    residual = res;
            }
        }
        iter++;
    }
    p[idxP(0,0)] = 0.0f;
    // Optionally print residual
    // std::cout << "SOR iterations: " << iter << ", residual: " << residual << std::endl;
    // I think its not needed this much so its commentated by default
}

void Solver::corrector() {
    const float invDx = 1.f / mesh.dx, invDy = 1.f / mesh.dy;
    int nx = cfg.nx, ny = cfg.ny;

    // Update u: u_new = u_star - dt * (p(i+1) - p(i)) / dx
    for (int j = 0; j < ny; ++j) {
        const int row = j * nx;
        for (int i = 1; i < nx; ++i) { // internal faces
            if (mesh.solid[row + i] || mesh.solid[row + (i - 1)]) {
                u[idxU(i, j)] = 0.0;
                continue;
            }
            // So gradient = (p[i] - p[i-1]) / dx
            float p_right = (i < nx && !mesh.solid[row + i]) ? p[row + i] : p[row + (i-1)];
            float p_left  = (i-1 >= 0 && !mesh.solid[row + (i-1)]) ? p[row + (i-1)] : p[row + i];
            float dpdx = (p_right - p_left) * invDx;
            u[idxU(i, j)] = u_star[idxU(i, j)] - dt * dpdx;
        }
    }

    // Update v: v_new = v_star - dt * (p(j+1) - p(j)) / dy
    for (int j = 1; j < ny; ++j) {
        const int row = j * nx;
        const int rowBot = (j - 1) * nx;
        for (int i = 0; i < nx; ++i) {
            if (mesh.solid[row + i] || mesh.solid[rowBot + i]) {
                v[idxV(i, j)] = 0.0;
                continue;
            }
            float p_top = (j < ny && !mesh.solid[row + i]) ? p[row + i] : p[rowBot + i];
            float p_bot = (j-1 >= 0 && !mesh.solid[rowBot + i]) ? p[rowBot + i] : p[row + i];
            float dpdy = (p_top - p_bot) * invDy;
            v[idxV(i, j)] = v_star[idxV(i, j)] - dt * dpdy;
        }
    }

    // Enforce no-slip inside solid and on boundaries
    for (int j = 0; j < ny; ++j) {
        for (int i = 1; i < nx; ++i) {
            if (mesh.solid[j * nx + i] || mesh.solid[j * nx + (i - 1)]) {
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
            if (mesh.solid[j * nx + i] || mesh.solid[(j - 1) * nx + i]) {
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
    const float dx = mesh.dx, dy = mesh.dy;

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
    std::vector<float> p_cell(nx * ny);
    std::vector<float> u_cell(nx * ny), v_cell(nx * ny);
    for (int j = 0; j < ny; ++j) {
        const int rowP = j*nx;
        const int rowU = j*(nx+1);
        const int rowV = j*nx;
        const int rowVTop = (j+1)*nx;
        for (int i = 0; i < nx; ++i) {
            p_cell[j * nx + i] = p[rowP + i] * cfg.ro; // physical pressure (Pa)
            // average u from left and right faces
            float u_left = u[rowU + i];
            float u_right = u[rowU + i + 1];
            u_cell[j * nx + i] = 0.5 * (u_left + u_right);
            // average v from bottom and top faces
            float v_bot = v[rowV + i];
            float v_top = v[rowVTop + i];
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
