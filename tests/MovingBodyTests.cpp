#include "TestHarness.hpp"

#include <cstdio>
#include <fstream>

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

int solidCells(const RestartData& frame) {
    int total = 0;
    for (uint8_t value : frame.solid)
        if (value)
            ++total;
    return total;
}

const RestartData::BodyState* bodyOf(const RestartData& frame, int object) {
    for (const RestartData::BodyState& state : frame.bodies)
        if (state.object == object)
            return &state;
    return nullptr;
}

Config movingCase(const std::filesystem::path& out,
                  const std::filesystem::path& model) {
    Config cfg = baseConfig(out);
    cfg.Lx = 2.0f;
    cfg.Ly = 1.0f;
    cfg.nx = 64;
    cfg.ny = 32;
    cfg.U0 = 0.0f;
    cfg.nu = 0.01f;
    cfg.geometryFile = "none";
    cfg.profiles = model.string() + "@x=0.5,y=0.5,size=0.24";
    for (int side = 0; side < 4; ++side)
        cfg.boundaries.side[side].kind = BoundaryKind::Wall;
    cfg.mgIterations = 40;
    cfg.mgTolerance = 1e-6f;
    cfg.saveInterval = 1000000;
    return cfg;
}

}

int main() {
    const std::filesystem::path root = scratchDir("movingbody");
    const std::filesystem::path model = root / "disc.obj";
    writeDisc(model, 0.1, 64);

    std::string error;
    int rc = 0;

    {
        Config cfg = movingCase(root / "prescribed", model);
        cfg.totalTime = 0.5;
        cfg.bodyMotion = "1:vx=0.4";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the prescribed run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the frame carries no body state at all");

        const double expected = 0.4 * 0.5;
        const double drift = std::fabs(body->x - expected);
        if (drift > 1e-4)
            return fail("a body told to travel at 0.4 m/s for 0.5 s moved " +
                        std::to_string(body->x) + " m instead of " +
                        std::to_string(expected));
        if (std::fabs(body->y) > 1e-9)
            return fail("a body told to move along x drifted in y");

        const float div = maxDivergence(frame);
        if (!(div < 1e-3f))
            return fail("a moving body left a divergence of " +
                        std::to_string(div) +
                        ": the cells it uncovers are not being filled");

        char line[220];
        std::snprintf(line, sizeof(line),
                      "prescribed    moved %.6f m against %.6f exact, div "
                      "%.3e, %d solid cells",
                      body->x, expected, div, solidCells(frame));
        report(line);
    }

    {
        Config cfg = movingCase(root / "keys", model);
        cfg.totalTime = 0.4;
        cfg.bodyMotion = "1:@0,vx=0,@0.2,vx=0.5,@0.4,vx=0.5";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the keyframe run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the keyframe run wrote no body state");

        const double expected = 0.5 * 0.5 * 0.2 + 0.5 * 0.2;
        if (std::fabs(body->x - expected) > 3e-3)
            return fail("a body ramped from 0 to 0.5 m/s over 0.2 s and held "
                        "moved " + std::to_string(body->x) + " m, not " +
                        std::to_string(expected));

        char line[200];
        std::snprintf(line, sizeof(line),
                      "keyframes     moved %.5f m against %.5f from the ramp "
                      "it was given",
                      body->x, expected);
        report(line);
    }

    {
        Config cfg = movingCase(root / "numbering", model);
        cfg.totalTime = 0.3;
        cfg.profiles = model.string() + "@x=0.5,y=0.5,size=0.2;" +
                       model.string() + "@x=1.4,y=0.5,size=0.2";
        cfg.bodyMotion = "1:vx=0.4;2:vx=-0.4";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the two-body run failed: " + error);

        const RestartData::BodyState* first = bodyOf(frame, 1);
        const RestartData::BodyState* second = bodyOf(frame, 2);
        if (!first || !second)
            return fail("one of the two bodies lost its number while moving");
        if (!(first->x > 0.1) || !(second->x < -0.1))
            return fail("the two bodies did not travel the way they were "
                        "told: " + std::to_string(first->x) + " and " +
                        std::to_string(second->x));

        char line[200];
        std::snprintf(line, sizeof(line),
                      "numbering     bodies 1 and 2 kept their numbers "
                      "closing at %.4f and %.4f m",
                      first->x, second->x);
        report(line);
    }

    {
        double settled[3] = {0.0, 0.0, 0.0};
        const BodyCoupling kinds[3] = {BodyCoupling::Weak, BodyCoupling::Added,
                                      BodyCoupling::Strong};
        const char* names[3] = {"weak", "added", "strong"};

        for (int which = 0; which < 3; ++which) {
            Config cfg = movingCase(root / ("fall-" + std::string(names[which])),
                                    model);
            cfg.Lx = 0.4f;
            cfg.Ly = 1.0f;
            cfg.nx = 32;
            cfg.ny = 80;
            cfg.nu = 1e-3f;
            cfg.ro = 1000.0f;
            cfg.gravityEnabled = true;
            cfg.gravityAccel = 9.81f;
            cfg.totalTime = 0.3;
            cfg.profiles = model.string() + "@x=0.2,y=0.75,size=0.1";
            cfg.bodyMotion = "1:free=1,density=2000";
            cfg.bodyCoupling = kinds[which];
            cfg.bodyIterations = 8;

            RestartData frame;
            if (!runCase(cfg, frame, error))
                return fail(std::string("the ") + names[which] +
                            " falling run failed: " + error);

            const RestartData::BodyState* body = bodyOf(frame, 1);
            if (!body)
                return fail("the falling run wrote no body state");
            if (!(body->vy < 0.0f))
                return fail("a body twice as dense as the fluid did not fall");
            settled[which] = body->vy;
        }

        const double bound = -(2000.0 - 1000.0) * 9.81 / (2000.0 + 1000.0) * 0.3;
        for (int which = 0; which < 3; ++which)
            if (settled[which] < bound * 1.05)
                return fail(std::string("the ") + names[which] +
                            " body fell faster than free fall allows: " +
                            std::to_string(settled[which]) + " against " +
                            std::to_string(bound));

        const double addedGap =
            std::fabs(settled[1] - settled[2]) / std::fabs(settled[2]);
        const double weakGap =
            std::fabs(settled[0] - settled[2]) / std::fabs(settled[2]);
        if (addedGap > 0.05)
            return fail("added coupling is " + std::to_string(addedGap * 100.0) +
                        "% away from strong, which is too far for the thing "
                        "that is meant to stand in for it");
        if (weakGap < 2.0 * addedGap)
            return fail("weak coupling came out as close to strong as added "
                        "did, so this case does not test what it claims to");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "coupling      weak %.4f, added %.4f, strong %.4f m/s - "
                      "added is %.1f%% off, weak %.1f%%",
                      settled[0], settled[1], settled[2], addedGap * 100.0,
                      weakGap * 100.0);
        report(line);
    }

    {
        Config cfg = movingCase(root / "neutral", model);
        cfg.Lx = 0.4f;
        cfg.Ly = 1.0f;
        cfg.nx = 32;
        cfg.ny = 80;
        cfg.nu = 1e-3f;
        cfg.ro = 1000.0f;
        cfg.gravityEnabled = true;
        cfg.gravityAccel = 9.81f;

        cfg.gravityMode = GravityMode::Body;
        cfg.totalTime = 0.3;
        cfg.profiles = model.string() + "@x=0.2,y=0.5,size=0.1";
        cfg.bodyMotion = "1:free=1,density=1000";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the neutrally buoyant run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the neutrally buoyant run wrote no body state");

        const double freeFall = 9.81 * 0.3;
        if (std::fabs(body->vy) > 0.05 * freeFall)
            return fail("a body weighing exactly what it displaces reached " +
                        std::to_string(body->vy) +
                        " m/s, which is not neutral buoyancy");

        char line[200];
        std::snprintf(line, sizeof(line),
                      "neutral       %.4f m/s after 0.3 s against %.3f if it "
                      "were falling free (%.2f%%)",
                      body->vy, freeFall,
                      100.0 * std::fabs(body->vy) / freeFall);
        report(line);
    }

    {
        Config cfg = movingCase(root / "thrust", model);
        cfg.totalTime = 0.25;
        cfg.ro = 1.0f;
        cfg.profiles = model.string() + "@x=1.0,y=0.5,size=0.2";
        cfg.bodyMotion = "1:free=1,mass=2";

        cfg.sources = "x=-0.14,y=0,r=0.06,rate=3,angle=180,body=1";
        cfg.boundaries = defaultChannelBoundaries();

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the thrust run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the thrust run wrote no body state");
        if (!(body->vx > 0.0f))
            return fail("a body with a source aimed at -x was pushed " +
                        std::to_string(body->vx) +
                        " m/s, and a rocket goes the other way to its exhaust");

        const double flow = 3.0 * 2.0 * 3.14159265358979 * 0.06;
        const double thrust = 1.0 * flow * 3.0;
        const double ceiling = thrust / 2.0 * 0.25;
        if (body->vx > ceiling)
            return fail("the body ended up at " + std::to_string(body->vx) +
                        " m/s, past the " + std::to_string(ceiling) +
                        " m/s its own jet could ever give it");

        char line[220];
        std::snprintf(line, sizeof(line),
                      "thrust        a jet aimed at -x pushed the body to "
                      "%.4f m/s along +x against a ceiling of %.4f",
                      body->vx, ceiling);
        report(line);
    }

    removeDir(root);
    if (rc == 0)
        std::cout << "MovingBodyTests OK\n";
    return rc;
}
