#include "TestHarness.hpp"

#include <cstdio>
#include <fstream>

using namespace testing;

namespace {

void writeBox(const std::filesystem::path& path, double w, double h) {
    const double v[4][2] = {{-w / 2, -h / 2}, {w / 2, -h / 2},
                            {w / 2, h / 2},   {-w / 2, h / 2}};
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

const std::vector<float>* fieldOf(const RestartData& frame,
                                  const std::string& name) {
    for (const auto& entry : frame.extras)
        if (entry.first == name)
            return &entry.second;
    return nullptr;
}

Config channel(const std::filesystem::path& out) {
    Config cfg = baseConfig(out);
    cfg.Lx = 2.0f;
    cfg.Ly = 0.2f;
    cfg.nx = 96;
    cfg.ny = 32;
    cfg.U0 = 1.0f;
    cfg.nu = 1.5e-5f;
    cfg.geometryFile = "empty";
    cfg.convection = ConvectionScheme::Muscl;
    cfg.timeScheme = TimeScheme::RK2;
    cfg.boundaries[BoundarySide::Bottom].kind = BoundaryKind::Wall;
    cfg.boundaries[BoundarySide::Top].kind = BoundaryKind::Wall;
    cfg.mgIterations = 20;
    cfg.mgTolerance = 1e-5f;
    cfg.saveInterval = 1000000;
    cfg.totalTime = 0.6;
    return cfg;
}

double strongestBackflow(const RestartData& frame, double fromX, double step) {
    const int nx = frame.nx, ny = frame.ny;
    double worst = 0.0;
    for (int i = static_cast<int>(fromX / frame.dx) + 2; i < nx; ++i) {
        int j = 0;
        while (j < ny && frame.solid[j * nx + i])
            ++j;
        if (j >= ny)
            continue;
        const double u =
            0.5 * (frame.u[j * (nx + 1) + i] + frame.u[j * (nx + 1) + i + 1]);
        worst = std::min(worst, u);
    }
    (void)step;
    return worst;
}

}

int main() {
    const std::filesystem::path root = scratchDir("turbulence");
    std::string error;
    int rc = 0;

    {
        Config cfg = channel(root / "walls");
        cfg.turbulence = TurbulenceKind::Smagorinsky;
        cfg.Cs = 0.1f;
        cfg.extraFields = "wallDistance,nuT,strain";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the wall distance run failed: " + error);

        const std::vector<float>* distance = fieldOf(frame, "wallDistance");
        const std::vector<float>* eddy = fieldOf(frame, "nuT");
        const std::vector<float>* shear = fieldOf(frame, "strain");
        if (!distance || !eddy || !shear)
            return fail("the frame carries no wallDistance, nuT or strain "
                        "field");

        const int nx = frame.nx, ny = frame.ny;
        const double half = 0.5 * cfg.Ly;
        const double firstRow = (*distance)[nx / 2];
        const double middle = (*distance)[(ny / 2) * nx + nx / 2];
        if (std::fabs(firstRow - 0.5 * frame.dy) > 1e-6)
            return fail("the first row off the wall is " +
                        std::to_string(firstRow) + " m away, not half a cell");
        if (std::fabs(middle - half) > 2.0 * frame.dy)
            return fail("the middle of the channel came out " +
                        std::to_string(middle) + " m from a wall, not " +
                        std::to_string(half));

        const double filter = cfg.Cs * std::sqrt(frame.dx * frame.dy);
        double loosest = 0.0, wallLength = 0.0, peak = 0.0;
        for (std::size_t id = 0; id < eddy->size(); ++id) {
            peak = std::max<double>(peak, (*eddy)[id]);
            if (!((*shear)[id] > 1e-6f))
                continue;
            const double length = std::sqrt((*eddy)[id] / (*shear)[id]);
            loosest = std::max(loosest, length /
                                            std::min<double>(filter,
                                                             0.41 *
                                                                 (*distance)[id]));
            if (id < static_cast<std::size_t>(nx))
                wallLength = std::max(wallLength, length / filter);
        }
        if (!(peak > 0.0))
            return fail("Smagorinsky produced no eddy viscosity at all");
        if (loosest > 1.0 + 1e-3)
            return fail("a mixing length came out " + std::to_string(loosest) +
                        " times min(Cs*delta, kappa*y), which is the one thing "
                        "the model is not allowed to do");
        if (!(wallLength < 0.75))
            return fail("the mixing length in the first row off the wall is " +
                        std::to_string(wallLength) +
                        " of the filter width, so the near wall damping is not "
                        "doing anything");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "smagorinsky   nuT peak %.4e against nu %.1e (%.0fx), "
                      "mixing length at the wall %.0f%% of Cs*delta",
                      peak, static_cast<double>(cfg.nu), peak / cfg.nu,
                      100.0 * wallLength);
        report(line);
    }

    {
        Config cfg = channel(root / "komega");
        cfg.turbulence = TurbulenceKind::KOmegaSST;
        cfg.turbIntensity = 0.05f;
        cfg.turbLengthScale = 0.02f;
        cfg.extraFields = "nuT,k,omega";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the k-omega run failed: " + error);

        const std::vector<float>* eddy = fieldOf(frame, "nuT");
        const std::vector<float>* energy = fieldOf(frame, "k");
        const std::vector<float>* rate = fieldOf(frame, "omega");
        if (!eddy || !energy || !rate)
            return fail("the frame carries no nuT, k or omega field");

        for (float value : *energy)
            if (!(value >= 0.0f) || !std::isfinite(value))
                return fail("k went negative or stopped being a number, and "
                            "sqrt(k) is read every step");
        for (float value : *rate)
            if (!(value > 0.0f) || !std::isfinite(value))
                return fail("omega went to zero or below, and it is divided "
                            "by every step");

        double peakEddy = 0.0, peakK = 0.0, peakOmega = 0.0;
        for (float value : *eddy)
            peakEddy = std::max<double>(peakEddy, value);
        for (float value : *energy)
            peakK = std::max<double>(peakK, value);
        for (float value : *rate)
            peakOmega = std::max<double>(peakOmega, value);

        const double inletK =
            1.5 * (cfg.turbIntensity * cfg.U0) * (cfg.turbIntensity * cfg.U0);
        if (peakK < 0.5 * inletK || peakK > 4.0 * inletK)
            return fail("k peaked at " + std::to_string(peakK) +
                        " against the " + std::to_string(inletK) +
                        " the inlet is putting in");

        const double wallOmega =
            60.0 * cfg.nu / (0.075 * (0.5 * frame.dy) * (0.5 * frame.dy));
        if (std::fabs(peakOmega - wallOmega) > 0.02 * wallOmega)
            return fail("omega peaked at " + std::to_string(peakOmega) +
                        " against the " + std::to_string(wallOmega) +
                        " the wall condition imposes");

        if (!(peakEddy > cfg.nu))
            return fail("the eddy viscosity never got above the molecular "
                        "one, so the model is doing nothing");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "k-omega       k %.4e against %.4e in, omega at the "
                      "wall %.1f against %.1f, nuT/nu %.0f",
                      peakK, inletK, peakOmega, wallOmega, peakEddy / cfg.nu);
        report(line);
    }

    {
        Config whole = channel(root / "whole");
        whole.turbulence = TurbulenceKind::KOmegaSST;
        whole.turbIntensity = 0.05f;
        whole.turbLengthScale = 0.02f;
        whole.totalTime = 0.4;

        RestartData wholeFrame;
        if (!runCase(whole, wholeFrame, error))
            return fail("the straight through k-omega run failed: " + error);

        Config half = whole;
        half.outputDir = (root / "half").string();
        half.totalTime = 0.2;

        RestartData halfFrame;
        if (!runCase(half, halfFrame, error))
            return fail("the first half of the k-omega run failed: " + error);
        if (!fieldOf(halfFrame, "k") || !fieldOf(halfFrame, "omega"))
            return fail("the frame carries no k or omega, so a k-omega run "
                        "cannot be continued from it at all");

        std::filesystem::path source;
        for (const auto& entry :
             std::filesystem::directory_iterator(root / "half"))
            if (entry.path().extension() == ".vtk")
                source = entry.path();

        RestartData restFrame;
        {
            Quiet quiet;
            RestartData loaded;
            if (!loadRestart(source, loaded, error))
                return fail("loading the half frame back: " + error);

            Config merged = loaded.cfg;
            merged.restart = true;
            merged.restartFile = source.string();
            merged.outputDir = (root / "cont").string();
            merged.totalTime = whole.totalTime;
            std::filesystem::create_directories(merged.outputDir);

            Mesh mesh(merged, &loaded.solid);
            Solver solver(merged, mesh);
            if (!solver.setInitialState(std::move(loaded), "cont"))
                return fail("the solver refused the half frame");
            solver.run();

            std::filesystem::path newest;
            for (const auto& entry :
                 std::filesystem::directory_iterator(root / "cont"))
                if (entry.path().extension() == ".vtk")
                    newest = entry.path();
            if (newest.empty() || !loadRestart(newest, restFrame, error))
                return fail("continuation frame: " + error);
        }

        const std::vector<float>* wholeK = fieldOf(wholeFrame, "k");
        const std::vector<float>* contK = fieldOf(restFrame, "k");
        if (!wholeK || !contK)
            return fail("one of the two runs wrote no k field");
        const float drift = maxDifference(*wholeK, *contK) / magnitude(*wholeK);
        if (!(drift < 5e-2f))
            return fail("continuing a k-omega run reproduces k only to " +
                        std::to_string(drift) +
                        ", so the model is not being handed its own state "
                        "back");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "restart       k after a break differs by %.3e relative, "
                      "peak %.4e",
                      static_cast<double>(drift),
                      static_cast<double>(magnitude(*wholeK)));
        report(line);
    }

    {
        const std::filesystem::path model = root / "step.obj";
        writeBox(model, 0.1, 0.0485);

        double backflow[2] = {0.0, 0.0};
        const TurbulenceKind kinds[2] = {TurbulenceKind::None,
                                         TurbulenceKind::KOmegaSST};
        const char* names[2] = {"laminar", "k-omega"};
        for (int which = 0; which < 2; ++which) {
            Config cfg = channel(root / ("step-" + std::string(names[which])));
            cfg.Lx = 0.6f;
            cfg.Ly = 0.1f;
            cfg.nx = 120;
            cfg.ny = 24;
            cfg.geometryFile = "none";
            cfg.profiles = model.string() +
                           "@x=0.05,y=0.02425,size=0.1,attach=1";
            cfg.boundaries[BoundarySide::Left].kind = BoundaryKind::Inlet;
            cfg.boundaries[BoundarySide::Right].kind = BoundaryKind::Outlet;
            cfg.boundaries[BoundarySide::Left].from = 0.485f;
            cfg.boundaries[BoundarySide::Left].to = 1.0f;
            cfg.turbulence = kinds[which];
            cfg.turbIntensity = 0.03f;
            cfg.turbLengthScale = 0.005f;
            cfg.totalTime = 6.0;

            RestartData frame;
            if (!runCase(cfg, frame, error))
                return fail(std::string("the ") + names[which] +
                            " step run failed: " + error);
            backflow[which] = strongestBackflow(frame, 0.1, 0.0485);
        }

        if (!(backflow[0] < -0.05))
            return fail("the laminar step produced no recirculation at all: " +
                        std::to_string(backflow[0]));
        if (!(backflow[1] > 0.6 * backflow[0]))
            return fail("k-omega left the recirculation as strong as the "
                        "laminar run did (" + std::to_string(backflow[1]) +
                        " against " + std::to_string(backflow[0]) +
                        "), so the eddy viscosity is not reaching the shear "
                        "layer");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "backward step laminar backflow %.4f m/s, k-omega "
                      "%.4f - the model took %.0f%% of it out",
                      backflow[0], backflow[1],
                      100.0 * (1.0 - backflow[1] / backflow[0]));
        report(line);
    }

    removeDir(root);
    if (rc == 0)
        std::cout << "TurbulenceTests OK\n";
    return rc;
}
