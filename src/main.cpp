#include "Config.hpp"
#include "Mesh.hpp"
#include "Solver.hpp"
#include <iostream>

//for now no other libs are needed, so i deleted everything else from main.cpp, but we will need to add more later. for now these are used and none more
//to perform tests faster.

int main() {
    std::cout << "=== CFD-Solver-2D ===\n\n";

    Config cfg;
    cfg.readFromConsole();

    // Confirmation loop
    while (!cfg.confirm()) {
        // loop will repeat until user presses Enter without text
    }

    // Final confirmation output
    std::cout << "\n--- Final Configuration ---\n";
    cfg.print();

    // Instruction about geometry import
    std::cout << "\nNote: This version supports STL and OBJ models.\n";
    std::cout << "      The mask is generated from the projection of the model\n";
    std::cout << "      onto the XY plane (slice angle is applied). You will be able to zoom in and out.\n";
    std::cout << "      Currently, only a circle is used as placeholder.\n";

    std::cout << "      Total simulation time: " << cfg.totalTime << " s.\n";
    std::cout << "      (Time‑stepping will stop when current time reaches this. You will be able to go forwards and backwards in time.)\n";

    // Create mesh (circle)
    Mesh mesh(cfg);
    mesh.printInfo();

    Solver solver(cfg, mesh);
    solver.run();
    std::cout << "\nSimulation complete. Press Enter to exit...";
    std::cin.get();
    return 0;
}
