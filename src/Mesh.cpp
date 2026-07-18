#include "Mesh.hpp"
#include <stl_reader/stl_reader.h>
#include <tiny_obj_loader.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <utility>

#ifndef CFD_MODELS_DIR
#define CFD_MODELS_DIR "models"
#endif

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double OBSTACLE_DOMAIN_FRACTION = 0.2;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::filesystem::path resolveGeometryPath(const std::string& filename) {
    const std::filesystem::path requested(filename);
    if (pathExists(requested)) {
        return requested;
    }

    const std::filesystem::path modelPath =
        std::filesystem::path(CFD_MODELS_DIR) / requested;
    if (pathExists(modelPath)) {
        return modelPath;
    }

    return {};
}

double squaredDistance(const Mesh::Vertex& first, const Mesh::Vertex& second) {
    const double deltaX = first.x - second.x;
    const double deltaY = first.y - second.y;
    const double deltaZ = first.z - second.z;
    return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
}
}

Mesh::Mesh(const Config& cfg)
    : cfg(cfg), nx(cfg.nx), ny(cfg.ny)
{
    solid.resize(nx * ny, 0);
    createGrid();
    const bool geometryLoaded = loadGeometry(cfg.geometryFile);
    buildSection();
    rasterizeSection();
    buildSolid();

    if (!geometryLoaded || sectionContour.empty()) {
        const double cx = cfg.Lx / 2.0;
        const double cy = cfg.Ly / 2.0;
        const double radius = 0.1 * std::min(cfg.Lx, cfg.Ly);
        initCircle(cx, cy, radius);
    }
}

void Mesh::createGrid() {
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
}

void Mesh::clearSolid() {
    std::fill(solid.begin(), solid.end(), 0);
}

bool Mesh::loadGeometry(const std::string& filename) {
    triangles.clear();
    sectionContour.clear();

    if (filename.empty() || lowercase(filename) == "none") {
        return false;
    }

    const std::filesystem::path path = resolveGeometryPath(filename);
    if (path.empty()) {
        std::cerr << "Geometry file not found: " << filename << "\n";
        return false;
    }

    const std::string extension = lowercase(path.extension().string());
    if (extension == ".stl") {
        geometryType = GeometryType::STL;
    } else if (extension == ".obj") {
        geometryType = GeometryType::OBJ;
    } else {
        std::cerr << "Unsupported geometry format: " << path.string() << "\n";
        return false;
    }

    switch (geometryType) {
        case GeometryType::STL:
            return loadSTL(path.string());
        case GeometryType::OBJ:
            return loadOBJ(path.string());
    }

    return false;
}

bool Mesh::loadOBJ(const std::string& filename) {
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;

    const std::filesystem::path path(filename);
    std::string materialDirectory = path.parent_path().string();
    if (!materialDirectory.empty()) {
        materialDirectory += std::filesystem::path::preferred_separator;
    }

    const bool loaded = tinyobj::LoadObj(
        &attributes,
        &shapes,
        &materials,
        &warning,
        &error,
        filename.c_str(),
        materialDirectory.empty() ? nullptr : materialDirectory.c_str(),
        true);

    if (!warning.empty()) {
        std::cerr << "OBJ warning: " << warning << "\n";
    }
    if (!loaded) {
        std::cerr << "OBJ load failed: " << error << "\n";
        return false;
    }

    for (const tinyobj::shape_t& shape : shapes) {
        std::size_t indexOffset = 0;
        for (unsigned int faceVertexCount : shape.mesh.num_face_vertices) {
            std::vector<Vertex> faceVertices;
            faceVertices.reserve(faceVertexCount);

            for (unsigned int vertex = 0; vertex < faceVertexCount; ++vertex) {
                const tinyobj::index_t index = shape.mesh.indices[indexOffset + vertex];
                if (index.vertex_index < 0) {
                    continue;
                }

                const std::size_t coordinateIndex =
                    3 * static_cast<std::size_t>(index.vertex_index);
                if (coordinateIndex + 2 >= attributes.vertices.size()) {
                    continue;
                }

                faceVertices.push_back({
                    static_cast<double>(attributes.vertices[coordinateIndex]),
                    static_cast<double>(attributes.vertices[coordinateIndex + 1]),
                    static_cast<double>(attributes.vertices[coordinateIndex + 2])
                });
            }
            indexOffset += faceVertexCount;

            for (std::size_t vertex = 1; vertex + 1 < faceVertices.size(); ++vertex) {
                triangles.push_back({
                    faceVertices[0],
                    faceVertices[vertex],
                    faceVertices[vertex + 1]
                });
            }
        }
    }

    return !triangles.empty();
}

bool Mesh::loadSTL(const std::string& filename) {
    try {
        const stl_reader::StlMesh<float, unsigned int> mesh(filename);
        triangles.reserve(mesh.num_tris());

        for (std::size_t triangleIndex = 0;
             triangleIndex < mesh.num_tris();
             ++triangleIndex) {
            std::array<Vertex, 3> vertices{};
            for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
                const float* coordinates = mesh.tri_corner_coords(triangleIndex, corner);
                vertices[corner] = {
                    static_cast<double>(coordinates[0]),
                    static_cast<double>(coordinates[1]),
                    static_cast<double>(coordinates[2])
                };
            }

            triangles.push_back({vertices[0], vertices[1], vertices[2]});
        }
    } catch (const std::exception& exception) {
        std::cerr << "STL load failed: " << exception.what() << "\n";
        return false;
    }

    return !triangles.empty();
}

void Mesh::buildSection() {
    sectionContour.clear();
    if (triangles.empty()) {
        return;
    }

    Vertex minimum{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()
    };
    Vertex maximum{
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest()
    };

    for (const Triangle& triangle : triangles) {
        for (const Vertex& vertex : {triangle.v0, triangle.v1, triangle.v2}) {
            minimum.x = std::min(minimum.x, vertex.x);
            minimum.y = std::min(minimum.y, vertex.y);
            minimum.z = std::min(minimum.z, vertex.z);
            maximum.x = std::max(maximum.x, vertex.x);
            maximum.y = std::max(maximum.y, vertex.y);
            maximum.z = std::max(maximum.z, vertex.z);
        }
    }

    const Vertex centre{
        0.5 * (minimum.x + maximum.x),
        0.5 * (minimum.y + maximum.y),
        0.5 * (minimum.z + maximum.z)
    };
    const double characteristicLength = std::max({
        maximum.x - minimum.x,
        maximum.y - minimum.y,
        maximum.z - minimum.z
    });
    if (characteristicLength <= 0.0) {
        return;
    }

    const double angleX = cfg.sliceAngleX * PI / 180.0;
    const double angleZ = cfg.sliceAngleZ * PI / 180.0;
    const double cosineX = std::cos(angleX);
    const double sineX = std::sin(angleX);
    const double cosineZ = std::cos(angleZ);
    const double sineZ = std::sin(angleZ);

    // Rz(angleZ) * Rx(angleX) gives an orthonormal basis for the section plane.
    const Vertex sectionAxisX{cosineZ, sineZ, 0.0};
    const Vertex sectionAxisY{
        -sineZ * cosineX,
        cosineZ * cosineX,
        sineX
    };
    const Vertex sectionNormal{
        sineZ * sineX,
        -cosineZ * sineX,
        cosineX
    };

    const double tolerance = std::max(1e-12, characteristicLength * 1e-9);
    const double toleranceSquared = tolerance * tolerance;
    using Segment = std::pair<SectionPoint, SectionPoint>;
    std::vector<Segment> segments;

    const auto relativeToCentre = [&centre](const Vertex& vertex) {
        return Vertex{
            vertex.x - centre.x,
            vertex.y - centre.y,
            vertex.z - centre.z
        };
    };
    const auto dot = [](const Vertex& first, const Vertex& second) {
        return first.x * second.x + first.y * second.y + first.z * second.z;
    };
    const auto project = [&](const Vertex& vertex) {
        const Vertex relative = relativeToCentre(vertex);
        return SectionPoint{
            dot(relative, sectionAxisX),
            dot(relative, sectionAxisY)
        };
    };

    // Each non-coplanar triangle contributes at most one plane-intersection segment.
    for (const Triangle& triangle : triangles) {
        const std::array<Vertex, 3> vertices{
            triangle.v0,
            triangle.v1,
            triangle.v2
        };
        std::array<double, 3> distances{};
        for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
            distances[vertex] = dot(relativeToCentre(vertices[vertex]), sectionNormal);
        }

        std::vector<Vertex> intersections;
        const auto addUniqueIntersection = [&](const Vertex& intersection) {
            const bool duplicate = std::any_of(
                intersections.begin(),
                intersections.end(),
                [&](const Vertex& existing) {
                    return squaredDistance(existing, intersection) <= toleranceSquared;
                });
            if (!duplicate) {
                intersections.push_back(intersection);
            }
        };

        for (std::size_t edge = 0; edge < vertices.size(); ++edge) {
            const std::size_t next = (edge + 1) % vertices.size();
            const Vertex& first = vertices[edge];
            const Vertex& second = vertices[next];
            const double firstDistance = distances[edge];
            const double secondDistance = distances[next];
            const bool firstOnPlane = std::abs(firstDistance) <= tolerance;
            const bool secondOnPlane = std::abs(secondDistance) <= tolerance;

            if (firstOnPlane) {
                addUniqueIntersection(first);
            }
            if (secondOnPlane) {
                addUniqueIntersection(second);
            }
            if (!firstOnPlane &&
                !secondOnPlane &&
                ((firstDistance < 0.0) != (secondDistance < 0.0))) {
                const double interpolation =
                    firstDistance / (firstDistance - secondDistance);
                addUniqueIntersection({
                    first.x + interpolation * (second.x - first.x),
                    first.y + interpolation * (second.y - first.y),
                    first.z + interpolation * (second.z - first.z)
                });
            }
        }

        if (intersections.size() == 2) {
            const SectionPoint first = project(intersections[0]);
            const SectionPoint second = project(intersections[1]);
            const double deltaX = first.x - second.x;
            const double deltaY = first.y - second.y;
            if (deltaX * deltaX + deltaY * deltaY > toleranceSquared) {
                segments.emplace_back(first, second);
            }
        }
    }

    if (segments.empty()) {
        return;
    }

    // Merge coincident segment endpoints into a graph of section edges.
    std::vector<SectionPoint> nodes;
    const auto findOrAddNode = [&](const SectionPoint& point) {
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const double deltaX = nodes[index].x - point.x;
            const double deltaY = nodes[index].y - point.y;
            if (deltaX * deltaX + deltaY * deltaY <= toleranceSquared) {
                return static_cast<int>(index);
            }
        }
        nodes.push_back(point);
        return static_cast<int>(nodes.size() - 1);
    };

    std::set<std::pair<int, int>> edges;
    for (const Segment& segment : segments) {
        int first = findOrAddNode(segment.first);
        int second = findOrAddNode(segment.second);
        if (first == second) {
            continue;
        }
        if (first > second) {
            std::swap(first, second);
        }
        edges.emplace(first, second);
    }

    std::vector<std::vector<int>> adjacency(nodes.size());
    for (const auto& edge : edges) {
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }

    std::set<std::pair<int, int>> visitedEdges;
    std::vector<int> largestLoop;
    double largestArea = 0.0;
    const auto normalizedEdge = [](int first, int second) {
        return std::pair<int, int>{std::min(first, second), std::max(first, second)};
    };

    // A watertight manifold section has degree two at every contour node.
    for (const auto& edge : edges) {
        if (visitedEdges.count(edge) != 0) {
            continue;
        }

        std::vector<int> loop{edge.first};
        int previous = edge.first;
        int current = edge.second;

        while (loop.size() <= edges.size() + 1) {
            visitedEdges.insert(normalizedEdge(previous, current));
            loop.push_back(current);
            if (current == loop.front()) {
                break;
            }

            int next = -1;
            for (int candidate : adjacency[current]) {
                if (visitedEdges.count(normalizedEdge(current, candidate)) == 0) {
                    next = candidate;
                    break;
                }
            }
            if (next < 0) {
                break;
            }

            previous = current;
            current = next;
        }

        if (loop.size() < 4 || loop.back() != loop.front()) {
            continue;
        }
        loop.pop_back();

        double signedAreaTwice = 0.0;
        for (std::size_t index = 0; index < loop.size(); ++index) {
            const SectionPoint& first = nodes[loop[index]];
            const SectionPoint& second = nodes[loop[(index + 1) % loop.size()]];
            signedAreaTwice += first.x * second.y - second.x * first.y;
        }

        const double area = 0.5 * std::abs(signedAreaTwice);
        if (area > largestArea) {
            largestArea = area;
            largestLoop = loop;
        }
    }

    if (largestLoop.size() < 3) {
        return;
    }

    sectionContour.reserve(largestLoop.size());
    const double rotation = cfg.sliceRotation * PI / 180.0;
    const double cosineRotation = std::cos(rotation);
    const double sineRotation = std::sin(rotation);

    for (int nodeIndex : largestLoop) {
        SectionPoint point = nodes[nodeIndex];
        if (cfg.invertSection) {
            point.x = -point.x;
        }
        sectionContour.push_back({
            cosineRotation * point.x - sineRotation * point.y,
            sineRotation * point.x + cosineRotation * point.y
        });
    }

    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const SectionPoint& point : sectionContour) {
        minimumX = std::min(minimumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumX = std::max(maximumX, point.x);
        maximumY = std::max(maximumY, point.y);
    }

    const double sectionSpan = std::max(maximumX - minimumX, maximumY - minimumY);
    if (sectionSpan <= tolerance) {
        sectionContour.clear();
        return;
    }

    const double targetSpan =
        OBSTACLE_DOMAIN_FRACTION * std::min(cfg.Lx, cfg.Ly);
    const double scale = targetSpan / sectionSpan;
    const double sectionCentreX = 0.5 * (minimumX + maximumX);
    const double sectionCentreY = 0.5 * (minimumY + maximumY);

    // Preserve the former circle diameter while centring imported geometry.
    for (SectionPoint& point : sectionContour) {
        point.x = cfg.Lx / 2.0 + scale * (point.x - sectionCentreX);
        point.y = cfg.Ly / 2.0 + scale * (point.y - sectionCentreY);
    }
}

void Mesh::rasterizeSection() {
    clearSolid();
    if (sectionContour.size() < 3) {
        return;
    }

    const double boundaryRadius = 0.5 * std::hypot(dx, dy);
    const auto distanceToSegment = [](double pointX,
                                      double pointY,
                                      const SectionPoint& first,
                                      const SectionPoint& second) {
        const double segmentX = second.x - first.x;
        const double segmentY = second.y - first.y;
        const double lengthSquared = segmentX * segmentX + segmentY * segmentY;
        if (lengthSquared <= 0.0) {
            return std::hypot(pointX - first.x, pointY - first.y);
        }

        const double projection = std::clamp(
            ((pointX - first.x) * segmentX + (pointY - first.y) * segmentY) /
                lengthSquared,
            0.0,
            1.0);
        const double closestX = first.x + projection * segmentX;
        const double closestY = first.y + projection * segmentY;
        return std::hypot(pointX - closestX, pointY - closestY);
    };

    // Mark cells touched by the polygon boundary before filling its interior.
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double cellX = (i + 0.5) * dx;
            const double cellY = (j + 0.5) * dy;

            for (std::size_t point = 0; point < sectionContour.size(); ++point) {
                const SectionPoint& first = sectionContour[point];
                const SectionPoint& second =
                    sectionContour[(point + 1) % sectionContour.size()];
                if (distanceToSegment(cellX, cellY, first, second) <=
                    boundaryRadius) {
                    solid[j * nx + i] = 1;
                    break;
                }
            }
        }
    }
}

bool Mesh::pointInsideSection(double pointX, double pointY) const {
    // Toggle once per horizontal-ray crossing (even-odd polygon rule).
    bool inside = false;
    for (std::size_t current = 0, previous = sectionContour.size() - 1;
         current < sectionContour.size();
         previous = current++) {
        const SectionPoint& first = sectionContour[current];
        const SectionPoint& second = sectionContour[previous];
        const bool crossesRay =
            ((first.y > pointY) != (second.y > pointY)) &&
            (pointX <
             (second.x - first.x) * (pointY - first.y) /
                     (second.y - first.y) +
                 first.x);
        if (crossesRay) {
            inside = !inside;
        }
    }
    return inside;
}

void Mesh::buildSolid() {
    if (sectionContour.size() < 3) {
        return;
    }

    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (solid[j * nx + i] != 0) {
                continue;
            }

            const double cellX = (i + 0.5) * dx;
            const double cellY = (j + 0.5) * dy;
            if (pointInsideSection(cellX, cellY)) {
                solid[j * nx + i] = 1;
            }
        }
    }
}

void Mesh::initCircle(double cx, double cy, double R) {
    clearSolid();
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
    std::cout << "  Number of geometry triangles = " << triangles.size() << "\n";
    std::cout << "  Number of section points = " << sectionContour.size() << "\n";
    std::cout << "=========================\n";
}
