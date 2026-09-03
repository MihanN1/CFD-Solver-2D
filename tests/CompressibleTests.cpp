#include "TestHarness.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <sstream>

using namespace testing;

namespace {

void writeDisc(const std::filesystem::path& path, double radius, int points) {
    std::ofstream out(path);
    for (int pass = 0; pass < 2; ++pass) {
        const double z = pass == 0 ? -0.05 : 0.05;
        for (int k = 0; k < points; ++k) {
            const double a = 2.0 * 3.14159265358979 * k / points;
            out << "v " << radius * std::cos(a) << " " << radius * std::sin(a)
                << " " << z << "\n";
        }
    }
    for (int k = 0; k < points; ++k) {
        const int a = k + 1;
        const int b = (k + 1) % points + 1;
        out << "f " << a << " " << b << " " << b + points << "\n";
        out << "f " << a << " " << b + points << " " << a + points << "\n";
    }
    out << "f";
    for (int k = 0; k < points; ++k)
        out << " " << k + 1;
    out << "\nf";
    for (int k = points - 1; k >= 0; --k)
        out << " " << points + k + 1;
    out << "\n";
}

const RestartData::BodyState* bodyOf(const RestartData& frame, int object) {
    for (const RestartData::BodyState& state : frame.bodies)
        if (state.object == object)
            return &state;
    return nullptr;
}

const std::vector<float>* fieldOf(const RestartData& frame,
                                  const std::string& name) {
    for (const auto& entry : frame.extras)
        if (entry.first == name)
            return &entry.second;
    return nullptr;
}

struct SodState {
    double rho, u, p;
};

double sodPressure(const SodState& left, const SodState& right, double gamma) {
    const double aL = std::sqrt(gamma * left.p / left.rho);
    const double aR = std::sqrt(gamma * right.p / right.rho);
    const auto branch = [&](double p, const SodState& side, double a,
                            double& value, double& slope) {
        if (p > side.p) {
            const double A = 2.0 / ((gamma + 1.0) * side.rho);
            const double B = (gamma - 1.0) / (gamma + 1.0) * side.p;
            const double root = std::sqrt(A / (B + p));
            value = (p - side.p) * root;
            slope = root * (1.0 - 0.5 * (p - side.p) / (B + p));
        } else {
            const double power = (gamma - 1.0) / (2.0 * gamma);
            value = 2.0 * a / (gamma - 1.0) *
                    (std::pow(p / side.p, power) - 1.0);
            slope = 1.0 / (side.rho * a) *
                    std::pow(p / side.p, -(gamma + 1.0) / (2.0 * gamma));
        }
    };

    double p = 0.5 * (left.p + right.p);
    for (int iteration = 0; iteration < 100; ++iteration) {
        double fL = 0.0, dL = 0.0, fR = 0.0, dR = 0.0;
        branch(p, left, aL, fL, dL);
        branch(p, right, aR, fR, dR);
        const double residual = fL + fR + (right.u - left.u);
        const double next = p - residual / (dL + dR);
        if (std::fabs(next - p) < 1e-12 * p) {
            p = next;
            break;
        }
        p = std::max(next, 1e-8 * std::min(left.p, right.p));
    }
    return p;
}

SodState sodSample(const SodState& left,
                   const SodState& right,
                   double gamma,
                   double speed) {
    const double aL = std::sqrt(gamma * left.p / left.rho);
    const double aR = std::sqrt(gamma * right.p / right.rho);
    const double pStar = sodPressure(left, right, gamma);

    const auto jump = [&](double p, const SodState& side, double a) {
        if (p > side.p) {
            const double A = 2.0 / ((gamma + 1.0) * side.rho);
            const double B = (gamma - 1.0) / (gamma + 1.0) * side.p;
            return (p - side.p) * std::sqrt(A / (B + p));
        }
        const double power = (gamma - 1.0) / (2.0 * gamma);
        return 2.0 * a / (gamma - 1.0) * (std::pow(p / side.p, power) - 1.0);
    };
    const double uStar = 0.5 * (left.u + right.u + jump(pStar, right, aR) -
                                jump(pStar, left, aL));

    if (speed <= uStar) {
        if (pStar > left.p) {
            const double ratio = pStar / left.p;
            const double factor = (gamma - 1.0) / (gamma + 1.0);
            const double shock =
                left.u - aL * std::sqrt((gamma + 1.0) / (2.0 * gamma) * ratio +
                                        (gamma - 1.0) / (2.0 * gamma));
            if (speed <= shock)
                return left;
            return {left.rho * (ratio + factor) / (factor * ratio + 1.0),
                    uStar, pStar};
        }
        const double head = left.u - aL;
        const double aStar =
            aL * std::pow(pStar / left.p, (gamma - 1.0) / (2.0 * gamma));
        const double tail = uStar - aStar;
        if (speed <= head)
            return left;
        if (speed >= tail)
            return {left.rho * std::pow(pStar / left.p, 1.0 / gamma), uStar,
                    pStar};
        const double u = 2.0 / (gamma + 1.0) *
                         (aL + (gamma - 1.0) / 2.0 * left.u + speed);
        const double a = 2.0 / (gamma + 1.0) *
                         (aL + (gamma - 1.0) / 2.0 * (left.u - speed));
        return {left.rho * std::pow(a / aL, 2.0 / (gamma - 1.0)), u,
                left.p * std::pow(a / aL, 2.0 * gamma / (gamma - 1.0))};
    }

    if (pStar > right.p) {
        const double ratio = pStar / right.p;
        const double factor = (gamma - 1.0) / (gamma + 1.0);
        const double shock =
            right.u + aR * std::sqrt((gamma + 1.0) / (2.0 * gamma) * ratio +
                                     (gamma - 1.0) / (2.0 * gamma));
        if (speed >= shock)
            return right;
        return {right.rho * (ratio + factor) / (factor * ratio + 1.0), uStar,
                pStar};
    }
    const double head = right.u + aR;
    const double aStar =
        aR * std::pow(pStar / right.p, (gamma - 1.0) / (2.0 * gamma));
    const double tail = uStar + aStar;
    if (speed >= head)
        return right;
    if (speed <= tail)
        return {right.rho * std::pow(pStar / right.p, 1.0 / gamma), uStar,
                pStar};
    const double u = 2.0 / (gamma + 1.0) *
                     (-aR + (gamma - 1.0) / 2.0 * right.u + speed);
    const double a = 2.0 / (gamma + 1.0) *
                     (aR - (gamma - 1.0) / 2.0 * (right.u - speed));
    return {right.rho * std::pow(a / aR, 2.0 / (gamma - 1.0)), u,
            right.p * std::pow(a / aR, 2.0 * gamma / (gamma - 1.0))};
}

Config gasConfig(const std::filesystem::path& out) {
    Config cfg = baseConfig(out);
    cfg.regime = Regime::Compressible;
    cfg.geometryFile = "empty";
    cfg.limiter = LimiterKind::VanLeer;
    cfg.saveInterval = 1000000;
    cfg.CFL = 0.4f;
    cfg.dtSafety = 0.9f;
    cfg.T0 = 288.15f;
    cfg.pInf = 101325.0f;
    cfg.gamma = 1.4f;
    cfg.R = 287.05f;
    cfg.machInlet = 0.0f;
    return cfg;
}

void writeWedge(const std::filesystem::path& path,
                double length,
                double angleDegrees) {
    const double rise =
        length * std::tan(angleDegrees * 3.14159265358979 / 180.0);
    const double v[4][2] = {{-0.5 * length, -0.5 * rise},
                            {0.5 * length, 0.5 * rise},
                            {0.5 * length, -0.5 * rise},
                            {-0.5 * length, -0.5 * rise}};
    std::ofstream out(path);
    for (int pass = 0; pass < 2; ++pass) {
        const double z = pass == 0 ? -0.05 : 0.05;
        for (const auto& point : v)
            out << "v " << point[0] << " " << point[1] << " " << z << "\n";
    }
    for (int k = 0; k < 4; ++k) {
        const int a = k + 1;
        const int b = (k + 1) % 4 + 1;
        out << "f " << a << " " << b << " " << b + 4 << "\n";
        out << "f " << a << " " << b + 4 << " " << a + 4 << "\n";
    }
    out << "f 1 2 3 4\nf 8 7 6 5\n";
}

}

int main() {
    const std::filesystem::path root = scratchDir("compressible");
    std::string error;

    {
        Config cfg = gasConfig(root / "sod");
        cfg.Lx = 1.0f;
        cfg.Ly = 0.1f;
        cfg.nx = 400;
        cfg.ny = 8;
        cfg.caseType = CaseType::ShockTube;
        cfg.boundaries = closedBoundaries();
        cfg.totalTime = 0.0004;
        cfg.extraFields = "mach,temperature";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the shock tube run failed: " + error);

        const std::size_t cells =
            static_cast<std::size_t>(frame.nx) * frame.ny;
        if (frame.stateRho.size() != cells)
            return fail("the frame carries no conservative state");
        std::vector<float> density = frame.stateRho;
        std::vector<float> pressure(cells, 0.0f);
        for (std::size_t id = 0; id < cells; ++id) {
            const double r = frame.stateRho[id];
            const double kinetic =
                0.5 * (frame.stateRhoU[id] * frame.stateRhoU[id] +
                       frame.stateRhoV[id] * frame.stateRhoV[id]) / r;
            pressure[id] = static_cast<float>((cfg.gamma - 1.0) *
                                              (frame.stateRhoE[id] - kinetic));
        }

        const double rhoLeft = cfg.pInf / (cfg.R * cfg.T0);
        const SodState left{rhoLeft, 0.0, cfg.pInf};
        const SodState right{0.125 * rhoLeft, 0.0, 0.1 * cfg.pInf};

        const int row = frame.ny / 2;
        double worstRho = 0.0, worstP = 0.0;
        double scaleRho = 0.0, scaleP = 0.0;
        for (int i = 0; i < frame.nx; ++i) {
            const double x = (i + 0.5) * frame.dx - 0.5 * cfg.Lx;
            const SodState exact =
                sodSample(left, right, cfg.gamma, x / frame.currentTime);
            const std::size_t id =
                static_cast<std::size_t>(row) * frame.nx + i;
            worstRho = std::max(worstRho, std::fabs(density[id] - exact.rho));
            worstP = std::max(worstP, std::fabs(pressure[id] - exact.p));
            scaleRho = std::max(scaleRho, exact.rho);
            scaleP = std::max(scaleP, exact.p);
        }

        const double relativeRho = worstRho / scaleRho;
        const double relativeP = worstP / scaleP;
        if (!(relativeRho < 0.15) || !(relativeP < 0.15))
            return fail("the shock tube is off the exact solution by " +
                        std::to_string(relativeRho) + " in density and " +
                        std::to_string(relativeP) +
                        " in pressure, which is more than a limited second "
                        "order scheme should smear a contact by");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "sod tube      worst error against the exact Riemann "
                      "solution: %.2f%% density, %.2f%% pressure",
                      100.0 * relativeRho, 100.0 * relativeP);
        report(line);
    }

    {
        const double wedgeDegrees = 15.0;
        const double machFree = 2.5;
        const double gamma = 1.4;

        const std::filesystem::path model = root / "wedge.obj";
        writeWedge(model, 1.0, wedgeDegrees);

        Config cfg = gasConfig(root / "wedge");
        cfg.Lx = 1.2f;
        cfg.Ly = 0.6f;
        cfg.nx = 240;
        cfg.ny = 120;
        cfg.geometryFile = "none";
        cfg.profiles = model.string() + "@x=0.75,y=0.07,size=1.0,attach=1";
        cfg.machInlet = static_cast<float>(machFree);
        cfg.boundaries[BoundarySide::Left].kind = BoundaryKind::Inlet;
        cfg.boundaries[BoundarySide::Right].kind = BoundaryKind::Outlet;
        cfg.boundaries[BoundarySide::Bottom].kind = BoundaryKind::Slip;
        cfg.boundaries[BoundarySide::Top].kind = BoundaryKind::Outlet;
        cfg.totalTime = 0.004;
        cfg.extraFields = "mach";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the wedge run failed: " + error);

        const std::size_t cells =
            static_cast<std::size_t>(frame.nx) * frame.ny;
        if (frame.stateRho.size() != cells)
            return fail("the wedge frame carries no conservative state");
        std::vector<float> pressure(cells, 0.0f);
        for (std::size_t id = 0; id < cells; ++id) {
            const double r = frame.stateRho[id];
            const double kinetic =
                0.5 * (frame.stateRhoU[id] * frame.stateRhoU[id] +
                       frame.stateRhoV[id] * frame.stateRhoV[id]) / r;
            pressure[id] = static_cast<float>((gamma - 1.0) *
                                              (frame.stateRhoE[id] - kinetic));
        }

        double beta = 0.0;
        {
            const double theta = wedgeDegrees * 3.14159265358979 / 180.0;
            double low = std::asin(1.0 / machFree) + 1e-6;
            double high = 3.14159265358979 / 2.0 - 1e-6;
            for (int iteration = 0; iteration < 200; ++iteration) {
                const double mid = 0.5 * (low + high);
                const double normal = machFree * std::sin(mid);
                const double value =
                    2.0 / std::tan(mid) * (normal * normal - 1.0) /
                    (machFree * machFree * (gamma + std::cos(2.0 * mid)) + 2.0);
                if (value < std::tan(theta))
                    low = mid;
                else
                    high = mid;
            }
            beta = 0.5 * (low + high);
        }
        const double normalMach = machFree * std::sin(beta);
        const double ratio =
            1.0 + 2.0 * gamma / (gamma + 1.0) * (normalMach * normalMach - 1.0);

        double ahead = 0.0, behind = 0.0;
        const int column = static_cast<int>(0.72 * frame.nx);
        for (int j = 0; j < frame.ny; ++j) {
            const std::size_t id =
                static_cast<std::size_t>(j) * frame.nx + column;
            if (frame.solid[id])
                continue;
            const double value = pressure[id];
            if (j > 3 * frame.ny / 4)
                ahead = ahead == 0.0 ? value : std::min(ahead, value);
            else
                behind = std::max(behind, value);
        }
        if (!(ahead > 0.0) || !(behind > 0.0))
            return fail("the column across the wedge found no flow at all");

        const double measured = behind / ahead;
        const double relative = std::fabs(measured - ratio) / ratio;
        if (!(relative < 0.2))
            return fail("the oblique shock raised the pressure by " +
                        std::to_string(measured) + " against the " +
                        std::to_string(ratio) +
                        " Rankine-Hugoniot gives for a 15 degree wedge at "
                        "Mach 2.5");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "oblique shock p2/p1 %.3f against the %.3f "
                      "Rankine-Hugoniot gives, shock angle %.1f deg",
                      measured, ratio, beta * 180.0 / 3.14159265358979);
        report(line);
    }

    {
        Config cfg = gasConfig(root / "species");
        cfg.Lx = 1.0f;
        cfg.Ly = 0.1f;
        cfg.nx = 200;
        cfg.ny = 8;
        cfg.phases = 2;
        cfg.gamma2 = 1.667f;
        cfg.R2 = 2077.0f;
        cfg.phaseInit = PhaseInit::Column;
        cfg.phaseLevel = 0.5f;
        cfg.diffusivity = 0.0f;
        cfg.boundaries = closedBoundaries();
        cfg.totalTime = 0.0002;
        cfg.extraFields = "speedOfSound,temperature";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the two gas run failed: " + error);

        const std::vector<float>* fraction = fieldOf(frame, "species");
        const std::vector<float>* sound = fieldOf(frame, "speedOfSound");
        if (!fraction || !sound)
            return fail("the frame carries no species or speedOfSound");

        double heliumSound = 0.0, airSound = 0.0;
        for (std::size_t id = 0; id < fraction->size(); ++id) {
            if ((*fraction)[id] > 0.99f)
                heliumSound =
                    std::max(heliumSound, static_cast<double>((*sound)[id]));
            if ((*fraction)[id] < 0.01f)
                airSound =
                    std::max(airSound, static_cast<double>((*sound)[id]));
        }
        if (!(airSound > 0.0) || !(heliumSound > 0.0))
            return fail("one of the two gases never appeared in the frame");

        const double expected = std::sqrt(1.667 * 2077.0) /
                                std::sqrt(1.4 * 287.05);
        const double measured = heliumSound / airSound;
        if (std::fabs(measured - expected) > 0.05 * expected)
            return fail("helium carries sound " + std::to_string(measured) +
                        " times as fast as air here, against the " +
                        std::to_string(expected) +
                        " the gas constants give, so the mixture properties "
                        "are not following the composition");

        for (float value : *fraction)
            if (!(value >= -1e-4f && value <= 1.0f + 1e-4f))
                return fail("a mass fraction left [0, 1], and every property "
                            "the model computes is interpolated on it");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "two gases     sound in helium / in air %.3f against "
                      "the %.3f the gas constants give",
                      measured, expected);
        report(line);
    }

    {
        Config cfg = gasConfig(root / "sound");
        cfg.Lx = 0.34f;
        cfg.Ly = 0.04f;
        cfg.nx = 340;
        cfg.ny = 8;
        cfg.caseType = CaseType::ShockTube;
        cfg.boundaries = closedBoundaries();
        cfg.acousticFields = true;
        cfg.acousticWindow = 0.01f;
        cfg.microphones = "x=0.05,y=0.02";
        cfg.micInterval = 1;
        cfg.totalTime = 0.02;
        cfg.extraFields = "spl,pitch,pfluct";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the acoustic run failed: " + error);

        const std::vector<float>* spl = fieldOf(frame, "SPL");
        const std::vector<float>* pitch = fieldOf(frame, "pitch");
        if (!spl || !pitch)
            return fail("the frame carries no SPL or pitch field");

        double peakSpl = 0.0;
        for (float value : *spl)
            peakSpl = std::max(peakSpl, static_cast<double>(value));
        double sum = 0.0, weight = 0.0;
        for (std::size_t id = 0; id < pitch->size(); ++id)
            if ((*spl)[id] > peakSpl - 6.0) {
                sum += (*pitch)[id];
                weight += 1.0;
            }
        const double loudPitch = weight > 0.0 ? sum / weight : 0.0;

        const double speedOfSound = std::sqrt(cfg.gamma * cfg.R * cfg.T0);
        const double fundamental = speedOfSound / (2.0 * cfg.Lx);

        if (!(peakSpl > 120.0))
            return fail("a pressure step in a closed tube came out at " +
                        std::to_string(peakSpl) +
                        " dB, which is quieter than the step itself");
        if (!(loudPitch > 0.5 * fundamental) ||
            !(loudPitch < 4.0 * fundamental))
            return fail("the pitch came out at " + std::to_string(loudPitch) +
                        " Hz against a " + std::to_string(fundamental) +
                        " Hz fundamental, so the zero crossing estimate is "
                        "not tracking the tube at all");

        const std::filesystem::path trace =
            std::filesystem::path(cfg.outputDir) / "microphones.txt";
        if (!std::filesystem::exists(trace))
            return fail("the microphone wrote no trace");
        std::ifstream input(trace);
        int lines = 0;
        double micSpl = 0.0, micPeak = 0.0;
        std::string text;
        while (std::getline(input, text)) {
            ++lines;
            if (text.rfind("# ", 0) == 0 && text.find('=') == std::string::npos) {
                std::istringstream parts(text.substr(2));
                double x = 0.0, y = 0.0, level = 0.0, frequency = 0.0;
                if (parts >> x >> y >> level >> frequency) {
                    micSpl = level;
                    micPeak = frequency;
                }
            }
        }
        if (lines < 100)
            return fail("the microphone trace has only " +
                        std::to_string(lines) + " lines in it");
        if (!(micSpl > 120.0))
            return fail("the microphone heard " + std::to_string(micSpl) +
                        " dB where the field says " + std::to_string(peakSpl));
        if (!(micPeak > fundamental))
            return fail("the microphone put the peak at " +
                        std::to_string(micPeak) +
                        " Hz, below the " + std::to_string(fundamental) +
                        " Hz lowest mode the tube has, which it cannot ring "
                        "slower than");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "acoustics     field %.0f dB and %.0f Hz by zero "
                      "crossings, mic %.0f dB and %.0f Hz by spectrum, "
                      "fundamental %.0f Hz",
                      peakSpl, loudPitch, micSpl, micPeak, fundamental);
        report(line);
    }

    {
        const std::filesystem::path model = root / "disc.obj";
        writeDisc(model, 0.05, 48);

        Config cfg = gasConfig(root / "piston");
        cfg.Lx = 1.0f;
        cfg.Ly = 0.6f;
        cfg.nx = 128;
        cfg.ny = 76;
        cfg.geometryFile = "none";
        cfg.profiles = model.string() + "@x=0.35,y=0.3,size=0.16";
        cfg.boundaries = closedBoundaries();
        cfg.machInlet = 0.0f;
        cfg.totalTime = 0.0006;

        const double speedOfSound =
            std::sqrt(static_cast<double>(cfg.gamma) * cfg.R * cfg.T0);
        const double travel = 0.25 * speedOfSound;
        cfg.bodyMotion = "1:vx=" + std::to_string(travel);

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the moving body run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("a compressible run with a moving body wrote no body "
                        "state into its frame");
        const double expected = travel * cfg.totalTime;
        if (std::fabs(body->x - expected) > 0.02 * expected)
            return fail("a body told to travel at " + std::to_string(travel) +
                        " m/s moved " + std::to_string(body->x) +
                        " m instead of " + std::to_string(expected));

        const std::size_t cells =
            static_cast<std::size_t>(frame.nx) * frame.ny;
        if (frame.stateRho.size() != cells)
            return fail("the moving body frame carries no state");

        if (frame.solid.size() != cells)
            return fail("the moving body frame carries no mask");

        double centre = 0.0, mass = 0.0;
        for (int j = 0; j < frame.ny; ++j)
            for (int i = 0; i < frame.nx; ++i)
                if (frame.solid[static_cast<std::size_t>(j) * frame.nx + i]) {
                    centre += i;
                    mass += 1.0;
                }
        if (!(mass > 0.0))
            return fail("the body left the mask entirely");
        centre = (centre / mass + 0.5) * (cfg.Lx / frame.nx);
        if (std::fabs(centre - (0.35 + expected)) > 3.0 * cfg.Lx / frame.nx)
            return fail("the body state moved but the mask did not follow it: "
                        "the solid cells are centred at " +
                        std::to_string(centre) + " m and the body says " +
                        std::to_string(0.35 + expected));

        double ahead = 0.0, behind = 0.0;
        int aheadCount = 0, behindCount = 0;
        for (int j = 0; j < frame.ny; ++j)
            for (int i = 0; i < frame.nx; ++i) {
                const std::size_t id =
                    static_cast<std::size_t>(j) * frame.nx + i;
                if (frame.solid[id])
                    continue;
                const double x = (i + 0.5) * cfg.Lx / frame.nx;
                const double y = (j + 0.5) * cfg.Ly / frame.ny;
                if (std::fabs(y - 0.3) > 0.07)
                    continue;
                const double gamma = cfg.gamma;
                const double rhoHere = frame.stateRho[id];
                const double kinetic =
                    0.5 *
                    (static_cast<double>(frame.stateRhoU[id]) *
                         frame.stateRhoU[id] +
                     static_cast<double>(frame.stateRhoV[id]) *
                         frame.stateRhoV[id]) /
                    rhoHere;
                const double p =
                    (gamma - 1.0) * (frame.stateRhoE[id] - kinetic);
                const double front = 0.35 + expected;
                if (x > front + 0.06 && x < front + 0.18) {
                    ahead += p;
                    ++aheadCount;
                } else if (x < front - 0.06 && x > front - 0.18) {
                    behind += p;
                    ++behindCount;
                }
            }
        if (aheadCount == 0 || behindCount == 0)
            return fail("the piston test sampled no gas on one side");
        ahead /= aheadCount;
        behind /= behindCount;
        if (!(ahead > behind * 1.02))
            return fail("a body driven at Mach 0.25 through still gas left " +
                        std::to_string(ahead) + " Pa in front of it and " +
                        std::to_string(behind) +
                        " Pa behind: it is not pushing on the gas at all, so "
                        "the wall ghosts are not mirroring about its velocity");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "moving body   Mach 0.25 piston moved %.4f m against "
                      "%.4f asked, %.0f Pa ahead against %.0f Pa behind",
                      body->x, expected, ahead, behind);
        report(line);
    }

    {
        Config cfg = gasConfig(root / "audio");
        cfg.Lx = 0.34f;
        cfg.Ly = 0.04f;
        cfg.nx = 340;
        cfg.ny = 8;
        cfg.caseType = CaseType::ShockTube;
        cfg.boundaries = closedBoundaries();
        cfg.microphones = "x=0.05,y=0.02";
        cfg.micInterval = 1;
        cfg.micAudio = true;
        cfg.micAudioRate = 44100;
        cfg.micAudioSpeed = 0.05f;
        cfg.totalTime = 0.0015;

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the audio run failed: " + error);

        const std::filesystem::path wav =
            std::filesystem::path(cfg.outputDir) / "microphone1.wav";
        if (!std::filesystem::exists(wav))
            return fail("micAudio=1 wrote no microphone1.wav");

        std::ifstream file(wav, std::ios::binary);
        std::vector<unsigned char> raw(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        if (raw.size() < 44)
            return fail("the wav is " + std::to_string(raw.size()) +
                        " bytes, which is not even a header");

        const auto read32 = [&](std::size_t at) {
            return static_cast<uint32_t>(raw[at]) |
                   (static_cast<uint32_t>(raw[at + 1]) << 8) |
                   (static_cast<uint32_t>(raw[at + 2]) << 16) |
                   (static_cast<uint32_t>(raw[at + 3]) << 24);
        };
        const auto read16 = [&](std::size_t at) {
            return static_cast<uint16_t>(
                static_cast<uint32_t>(raw[at]) |
                (static_cast<uint32_t>(raw[at + 1]) << 8));
        };

        if (std::string(raw.begin(), raw.begin() + 4) != "RIFF" ||
            std::string(raw.begin() + 8, raw.begin() + 12) != "WAVE" ||
            std::string(raw.begin() + 12, raw.begin() + 16) != "fmt " ||
            std::string(raw.begin() + 36, raw.begin() + 40) != "data")
            return fail("the wav header is not a wav header");
        if (read32(4) != raw.size() - 8)
            return fail("the RIFF size does not match the file");
        if (read16(20) != 1 || read16(22) != 1 || read16(34) != 16)
            return fail("the wav is not 16 bit mono PCM");
        if (static_cast<int>(read32(24)) != cfg.micAudioRate)
            return fail("the wav says " + std::to_string(read32(24)) +
                        " Hz where micAudioRate asked for " +
                        std::to_string(cfg.micAudioRate));
        if (read32(28) != read32(24) * 2u || read16(32) != 2)
            return fail("the wav block alignment contradicts its own format");

        const uint32_t dataBytes = read32(40);
        if (dataBytes != raw.size() - 44)
            return fail("the data chunk length does not match what follows");

        const double seconds =
            cfg.totalTime / static_cast<double>(cfg.micAudioSpeed);
        const double frames = dataBytes / 2.0;
        const double asked = seconds * cfg.micAudioRate;
        if (std::fabs(frames - asked) > 0.02 * asked)
            return fail(std::to_string(frames) + " frames at " +
                        std::to_string(cfg.micAudioRate) + " Hz is " +
                        std::to_string(frames / cfg.micAudioRate) +
                        " s, where " + std::to_string(cfg.totalTime) +
                        " s slowed by " +
                        std::to_string(1.0 / cfg.micAudioSpeed) + " is " +
                        std::to_string(seconds) + " s");

        double sum = 0.0;
        int peak = 0;
        for (std::size_t at = 44; at + 1 < raw.size(); at += 2) {
            const int16_t value = static_cast<int16_t>(read16(at));
            sum += value;
            peak = std::max(peak, std::abs(static_cast<int>(value)));
        }
        const double mean = sum / frames;
        if (std::abs(peak - 29490) > 2)
            return fail("the wav peaks at " + std::to_string(peak) +
                        " where peak normalisation to 0.9 of full scale is "
                        "29490");
        if (std::fabs(mean) > 0.01 * peak)
            return fail("the wav carries a DC offset of " +
                        std::to_string(mean) +
                        " counts, and a wav is a fluctuation");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "audio         %.0f frames at %d Hz is %.3f s of file "
                      "for %.4f s of flow, peak %d, mean %.1f",
                      frames, cfg.micAudioRate, frames / cfg.micAudioRate,
                      cfg.totalTime, peak, mean);
        report(line);
    }

    removeDir(root);
    std::cout << "CompressibleTests OK\n";
    return 0;
}
