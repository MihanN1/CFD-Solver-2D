#include "Config.hpp"
#include <iostream>
#include <limits>
#include <cctype>

void Config::readFromConsole() {
    std::cout << "=== CFD-Solver-2D Configuration ===\n";
    std::cout << "Enter domain width Lx (m): ";
    std::cin >> Lx;
    std::cout << "Enter domain height Ly (m): ";
    std::cin >> Ly;
    std::cout << "Enter number of cells in x-direction nx: ";
    std::cin >> nx;
    std::cout << "Enter number of cells in y-direction ny: ";
    std::cin >> ny;
    std::cout << "Enter inlet velocity U0 (m/s): ";
    std::cin >> U0;
    std::cout << "Enter kinematic viscosity nu (m^2/s): ";
    std::cin >> nu;
    std::cout << "Enter Reynolds number (0 to auto-compute later): ";
    std::cin >> Re;
    std::cout << "Enter CFL number (recommended 0.3-0.5): ";
    std::cin >> CFL;
    std::cout << "Enter total simulation time(seconds): ";
    std::cin >> totalTime;
    std::cout << "Enter SOR relaxation parameter omega (1.0-2.0): ";
    std::cin >> omega;
    std::cout << "Enter SOR tolerance (e.g., 1e-5): ";
    std::cin >> tol;
    std::cout << "Enter max SOR iterations: ";
    std::cin >> maxIterSOR;
    std::cout << "Enter path to 3D model (or 'none' for circle): ";
    std::cin >> modelFile;
    std::cout << "Enter slice angle (degrees, default 0): ";
    std::cin >> sliceAngle;
    std::cout << "Configuration read.\n";
    std::cout << "Enter density ro. Make sure that the gas/liquid is incompressible(meaning for air speed its less than 0.3M)(kg/m^3): ";
    std::cin >> ro; 

}
void Config::print() const {
    std::cout << "\n--- Current Configuration ---\n";
    std::cout << "  Lx            = " << Lx << " m\n";
    std::cout << "  Ly            = " << Ly << " m\n";
    std::cout << "  nx            = " << nx << "\n";
    std::cout << "  ny            = " << ny << "\n";
    std::cout << "  U0            = " << U0 << " m/s\n";
    std::cout << "  nu            = " << nu << " m^2/s\n";
    std::cout << "  Re            = " << Re << "\n";
    std::cout << "  CFL           = " << CFL << "\n";
    std::cout << "  totalTime        = " << totalTime << "\n";
    std::cout << "  omega         = " << omega << "\n";
    std::cout << "  tol           = " << tol << "\n";
    std::cout << "  maxIterSOR    = " << maxIterSOR << "\n";
    std::cout << "  modelFile     = " << modelFile << "\n";
    std::cout << "  sliceAngle    = " << sliceAngle << " deg\n";
    std::cout << "  ro            = " << ro << "kg/m^3\n"; 
    std::cout << "--------------------------------\n";
}
bool Config::modifyParam(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = std::tolower(c);

    if (lower == "lx") {
        std::cout << "New Lx: ";
        std::cin >> Lx;
    } else if (lower == "ly") {
        std::cout << "New Ly: ";
        std::cin >> Ly;
    } else if (lower == "nx") {
        std::cout << "New nx: ";
        std::cin >> nx;
    } else if (lower == "ny") {
        std::cout << "New ny: ";
        std::cin >> ny;
    } else if (lower == "u0") {
        std::cout << "New U0: ";
        std::cin >> U0;
    } else if (lower == "nu") {
        std::cout << "New nu: ";
        std::cin >> nu;
    } else if (lower == "re") {
        std::cout << "New Re: ";
        std::cin >> Re;
    } else if (lower == "cfl") {
        std::cout << "New CFL: ";
        std::cin >> CFL;
    } else if (lower == "totaltime") {
        std::cout << "New totalTime: ";
        std::cin >> totalTime;
    } else if (lower == "omega") {
        std::cout << "New omega: ";
        std::cin >> omega;
    } else if (lower == "tol") {
        std::cout << "New tol: ";
        std::cin >> tol;
    } else if (lower == "maxitersor") {
        std::cout << "New maxIterSOR: ";
        std::cin >> maxIterSOR;
    } else if (lower == "modelfile") {
        std::cout << "New modelFile: ";
        std::cin >> modelFile;
    } else if (lower == "sliceangle") {
        std::cout << "New sliceAngle (deg): ";
        std::cin >> sliceAngle;
    } else if (lower == "ro") {
        std::cout << "New ro(kg/m^3): ";
        std::cin >> ro;
    } else {
        std::cout << "Unknown parameter: " << name << "\n";
        return false;
    }
    std::cout << "Parameter updated.\n";
    return true;
}
bool Config::confirm() {
    print();
    std::cout << "\nTo change a parameter, type its name (e.g., 'nx') and press Enter.\n";
    std::cout << "To confirm all parameters and proceed, just press Enter (empty line).\n";
    std::cout << "> ";

    std::string input;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // clear newline
    std::getline(std::cin, input);

    if (input.empty()) {
        return true;   // confirmed
    } else {
        modifyParam(input);
        return false;  // not confirmed yet, loop again
    }
}