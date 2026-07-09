#pragma once
#include <vector>
#include "Config.hpp"

class Mesh {
public:
    Mesh(const Config& cfg);

    std::vector<double> x, y;
    std::vector<int> solid;   // 1 = inside body, 0 = fluid
    double dx, dy;
    int nx, ny;

    void initCircle(double cx, double cy, double R);
    void printInfo() const;
};