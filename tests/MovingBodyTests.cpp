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

std::vector<std::pair<double, double>> blobCentres(const RestartData& frame) {
    const int nx = frame.nx, ny = frame.ny;
    std::vector<int> label(static_cast<std::size_t>(nx) * ny, 0);
    std::vector<std::pair<double, double>> out;
    std::vector<int> pending;
    for (int seed = 0; seed < nx * ny; ++seed) {
        if (!frame.solid[seed] || label[seed])
            continue;
        const int mark = static_cast<int>(out.size()) + 1;
        double sumX = 0.0, sumY = 0.0;
        int count = 0;
        label[seed] = mark;
        pending.push_back(seed);
        while (!pending.empty()) {
            const int id = pending.back();
            pending.pop_back();
            const int i = id % nx, j = id / nx;
            sumX += (i + 0.5) * frame.dx;
            sumY += (j + 0.5) * frame.dy;
            ++count;
            for (int nj = std::max(j - 1, 0); nj <= std::min(j + 1, ny - 1); ++nj)
                for (int ni = std::max(i - 1, 0); ni <= std::min(i + 1, nx - 1);
                     ++ni) {
                    const int other = nj * nx + ni;
                    if (frame.solid[other] && !label[other]) {
                        label[other] = mark;
                        pending.push_back(other);
                    }
                }
        }
        out.push_back({sumX / count, sumY / count});
    }
    return out;
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

        const std::vector<std::pair<double, double>> blobs =
            blobCentres(frame);
        if (blobs.size() != 2)
            return fail("the two bodies came out as " +
                        std::to_string(blobs.size()) + " blobs in the mask");
        for (const RestartData::BodyState* body : {first, second}) {
            double best = 1e30;
            for (const std::pair<double, double>& blob : blobs)
                best = std::min(best, std::fabs(blob.first - (body == first
                                                    ? 0.5 + body->x
                                                    : 1.4 + body->x)));
            if (best > 3.0 * frame.dx)
                return fail("a body says it is at " +
                            std::to_string(body->x) +
                            " but no blob in the mask is within three cells "
                            "of there");
        }

        char line[220];
        std::snprintf(line, sizeof(line),
                      "numbering     bodies 1 and 2 kept their numbers closing "
                      "at %.4f and %.4f m, mask agrees",
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

        Config loose = cfg;
        loose.outputDir = (root / "thrust-loose").string();
        loose.sources = "x=0.86,y=0.5,r=0.06,rate=3,angle=180";
        RestartData looseFrame;
        if (!runCase(loose, looseFrame, error))
            return fail("the unattached-jet run failed: " + error);
        const RestartData::BodyState* adrift = bodyOf(looseFrame, 1);
        if (!adrift)
            return fail("the unattached-jet run wrote no body state");
        if (!(body->vx > adrift->vx))
            return fail("attaching the jet to the body changed nothing: " +
                        std::to_string(body->vx) + " against " +
                        std::to_string(adrift->vx) +
                        " with the same jet bolted to the domain");

        char line[240];
        std::snprintf(line, sizeof(line),
                      "thrust        jet on the body %.4f m/s, the same jet "
                      "bolted down %.4f m/s - the reaction is worth %.4f",
                      body->vx, adrift->vx, body->vx - adrift->vx);
        report(line);
    }

    {
        Config cfg = movingCase(root / "carried", model);
        cfg.U0 = 1.0f;
        cfg.nu = 0.01f;
        cfg.ro = 500.0f;
        cfg.totalTime = 0.35;
        cfg.profiles = model.string() + "@x=0.4,y=0.5,size=0.2";
        cfg.boundaries = defaultChannelBoundaries();
        cfg.bodyMotion = "1:free=1,density=700";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the carried run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the carried run wrote no body state");
        if (!(body->vx > 0.0f) || !(body->x > 0.0))
            return fail("a body let go in a flow running at +1 m/s ended up "
                        "going " + std::to_string(body->vx) +
                        " m/s: the horizontal force has the wrong sign");
        if (body->vx > cfg.U0)
            return fail("a body let go in a flow overtook the flow that is "
                        "carrying it, at " + std::to_string(body->vx) +
                        " m/s against " + std::to_string(cfg.U0));

        char line[220];
        std::snprintf(line, sizeof(line),
                      "carried       let go in a 1 m/s flow, reached %.4f m/s "
                      "downstream and moved %.4f m",
                      body->vx, body->x);
        report(line);
    }

    {
        Config cfg = movingCase(root / "released", model);
        cfg.Lx = 1.2f;
        cfg.Ly = 1.0f;
        cfg.nx = 72;
        cfg.ny = 60;
        cfg.nu = 1e-3f;
        cfg.ro = 1000.0f;
        cfg.totalTime = 0.4;
        cfg.profiles = model.string() + "@x=0.3,y=0.5,size=0.15";
        cfg.bodyMotion = "1:@0,vx=0.6,@0.15,free=1,density=1200";

        RestartData frame;
        if (!runCase(cfg, frame, error))
            return fail("the released run failed: " + error);

        const RestartData::BodyState* body = bodyOf(frame, 1);
        if (!body)
            return fail("the released run wrote no body state");
        if (!(body->vx > 0.0f))
            return fail("a body let go at 0.6 m/s stopped or reversed: " +
                        std::to_string(body->vx));
        if (!(body->vx < 0.6f))
            return fail("a body let go at 0.6 m/s with nothing pushing it "
                        "reached " + std::to_string(body->vx) +
                        " m/s, so the drag is pushing rather than dragging");

        char line[220];
        std::snprintf(line, sizeof(line),
                      "released      driven at 0.600, let go at 0.15 s, down "
                      "to %.4f m/s by 0.4 s",
                      body->vx);
        report(line);
    }

    {
        double outcome[2] = {0.0, 0.0};
        for (int pass = 0; pass < 2; ++pass) {
            Config cfg = movingCase(
                root / (pass == 0 ? "pass-through" : "bounce"), model);
            cfg.Lx = 2.0f;
            cfg.Ly = 1.0f;
            cfg.nx = 96;
            cfg.ny = 48;
            cfg.ro = 1.0f;
            cfg.nu = 1e-3f;
            cfg.totalTime = 0.9;
            cfg.profiles = model.string() + "@x=0.5,y=0.5,size=0.2;" +
                           model.string() + "@x=1.3,y=0.5,size=0.2";

            cfg.bodyMotion = "1:vx=-0.3;2:free=1,density=50,vx=0.6";
            cfg.bodyCollisions = pass == 1;
            cfg.bodyRestitution = 0.8f;

            RestartData frame;
            if (!runCase(cfg, frame, error))
                return fail("the collision run failed: " + error);
            const RestartData::BodyState* body = bodyOf(frame, 2);
            if (!body)
                return fail("the collision run wrote no body state");
            outcome[pass] = body->vx;
        }

        if (!(outcome[0] > 0.0))
            return fail("with collisions off the free body did not keep going "
                        "the way it was sent: " + std::to_string(outcome[0]));
        if (!(outcome[1] < outcome[0]))
            return fail("turning collisions on changed nothing: " +
                        std::to_string(outcome[1]) + " against " +
                        std::to_string(outcome[0]));

        char line[220];
        std::snprintf(line, sizeof(line),
                      "collisions    off %.4f m/s, on %.4f m/s - the second "
                      "one met something",
                      outcome[0], outcome[1]);
        report(line);
    }

    removeDir(root);
    if (rc == 0)
        std::cout << "MovingBodyTests OK\n";
    return rc;
}
