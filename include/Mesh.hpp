#pragma once
#include "Config.hpp"
#include <cstdint>
#include <string>
#include <vector>

class Mesh {
public:
    struct Vertex {
        double x;
        double y;
        double z;
    };

    struct Triangle {
        Vertex v0;
        Vertex v1;
        Vertex v2;
    };

    enum class GeometryType {
        STL,
        OBJ
    };

    // presetSolid short-circuits the whole geometry pipeline: a continuation
    // already has the mask in its frame, so the model file does not have to
    // exist any more and the rasterizer cannot drift between runs.
    explicit Mesh(const Config& cfg,
                  const std::vector<uint8_t>* presetSolid = nullptr);

    std::vector<double> x, y;
    std::vector<int> solid;   // 1 = inside body, 0 = fluid
    std::vector<Triangle> triangles;
    GeometryType geometryType = GeometryType::STL;
    float dx, dy;
    int nx, ny;

    void initCircle(double cx, double cy, double R);
    bool loadGeometry(const std::string& filename);
    bool loadOBJ(const std::string& filename);
    bool loadSTL(const std::string& filename);
    void buildSection();
    void rasterizeSection();
    void buildSolid();
    void printInfo() const;

private:
    struct SectionPoint {
        double x;
        double y;
    };

    const Config& cfg;
    std::vector<SectionPoint> sectionContour;

    void createGrid();
    void clearSolid();
    bool pointInsideSection(double x, double y) const;
};
