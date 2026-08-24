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

    // One connected body in the rasterized mask. radius is the distance from
    // the centroid to the farthest cell of the body, i.e. what its rim speed
    // is computed from when it spins.
    struct SolidObject {
        int cells = 0;
        double cx = 0.0;
        double cy = 0.0;
        double radius = 0.0;
    };

    // presetSolid short-circuits the whole geometry pipeline: a continuation
    // already has the mask in its frame, so the model file does not have to
    // exist any more and the rasterizer cannot drift between runs.
    explicit Mesh(const Config& cfg,
                  const std::vector<uint8_t>* presetSolid = nullptr);

    std::vector<double> x, y;
    std::vector<int> solid;   // 1 = inside body, 0 = fluid
    std::vector<int> objectId;   // 0 = fluid, 1..objects.size() = which body
    std::vector<SolidObject> objects;
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

    // Flood-fills the mask into numbered bodies, 8-connected: two cells that
    // meet only at a corner are one object, and the flow cannot squeeze
    // through that corner either. Numbering follows the scan order of the
    // grid, so the same mask always produces the same numbers.
    void labelObjects();

    void printInfo() const;

private:
    struct SectionPoint {
        double x;
        double y;
    };

    const Config& cfg;
    // Every closed loop the section plane cut out of the model, not just the
    // biggest one. Two aerofoils side by side, a ring, a body with a hole
    // through it - all of these are several loops, and keeping only the
    // largest silently dropped the rest. The even-odd fill in
    // pointInsideSection runs across the whole set, so a loop inside another
    // loop comes out as a hole rather than as solid, which is what it is.
    std::vector<std::vector<SectionPoint>> sectionContours;

    void createGrid();
    void clearSolid();
    bool pointInsideSection(double x, double y) const;
    // At least one loop with enough points to enclose anything.
    bool hasSection() const;
    std::size_t sectionPointCount() const;
};
