#pragma once
#include "Config.hpp"
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

    explicit Mesh(const Config& cfg);

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
