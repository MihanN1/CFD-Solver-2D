#include <SFML/Graphics.hpp>

#include <tiny_obj_loader.h>
#include <stl_reader.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool loadFirstObjFromModels()
{
    const fs::path modelsDir = CFD_MODELS_DIR;

    if (!fs::exists(modelsDir)) {
        std::cout << "[models] folder not found: " << modelsDir << '\n';
        return false;
    }

    for (const auto& entry : fs::directory_iterator(modelsDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (entry.path().extension() != ".obj") {
            continue;
        }

        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn;
        std::string err;

        const std::string inputFile = entry.path().string();
        const std::string materialDir = entry.path().parent_path().string();

        const bool ok = tinyobj::LoadObj(
            &attrib,
            &shapes,
            &materials,
            &warn,
            &err,
            inputFile.c_str(),
            materialDir.c_str()
        );

        if (!warn.empty()) {
            std::cout << "[OBJ warn] " << warn << '\n';
        }

        if (!err.empty()) {
            std::cout << "[OBJ error] " << err << '\n';
        }

        if (!ok) {
            std::cout << "[OBJ] failed: " << inputFile << '\n';
            return false;
        }

        std::cout << "[OBJ] loaded: " << inputFile << '\n';
        std::cout << "[OBJ] vertices: " << attrib.vertices.size() / 3 << '\n';
        std::cout << "[OBJ] shapes: " << shapes.size() << '\n';
        std::cout << "[OBJ] materials: " << materials.size() << '\n';
        return true;
    }

    std::cout << "[OBJ] no .obj files in models/\n";
    return false;
}

int main()
{
    loadFirstObjFromModels();

    sf::RenderWindow window(
        sf::VideoMode({1000u, 700u}),
        "CFD Solver 2D"
    );

    window.setFramerateLimit(60);

    sf::RectangleShape cell({18.0f, 18.0f});
    cell.setFillColor(sf::Color(35, 35, 35));
    cell.setOutlineColor(sf::Color(60, 60, 60));
    cell.setOutlineThickness(1.0f);

    sf::CircleShape particle(4.0f);
    particle.setFillColor(sf::Color(200, 200, 200));

    float t = 0.0f;

    while (window.isOpen()) {
        while (const auto event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
        }

        t += 0.016f;

        window.clear(sf::Color::Black);

        for (int y = 0; y < 30; ++y) {
            for (int x = 0; x < 45; ++x) {
                cell.setPosition({40.0f + x * 19.0f, 40.0f + y * 19.0f});
                window.draw(cell);
            }
        }

        for (int i = 0; i < 80; ++i) {
            const float x = 60.0f + static_cast<float>((i * 37) % 840);
            const float y = 70.0f + static_cast<float>((i * 53) % 520);
            particle.setPosition({x + t * 30.0f, y});
            window.draw(particle);
        }

        window.display();
    }

    return 0;
}
