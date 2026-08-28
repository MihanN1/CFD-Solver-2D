#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

bool runWith(const Config& cfg,
             bool avx2,
             bool openMp,
             int threads,
             RestartData& out,
             std::string& error) {
    runtime::Settings& settings = runtime::mutableSettings();
    const runtime::Settings saved = settings;
    settings.useAvx2 = avx2;
    settings.useOpenMp = openMp;
    settings.threads = threads;
    runtime::apply();

    const bool ok = runCase(cfg, out, error);

    settings = saved;
    runtime::apply();
    return ok;
}

int compare(const RestartData& a,
            const RestartData& b,
            const char* label,
            float limit) {
    const float du = maxDifference(a.u, b.u) / magnitude(a.u);
    const float dv = maxDifference(a.v, b.v) / magnitude(a.v);
    const float dp = maxDifference(a.p, b.p) / magnitude(a.p);
    std::printf("  %-22s u %.3e  v %.3e  p %.3e\n", label, du, dv, dp);
    if (!(du < limit && dv < limit && dp < limit))
        return fail(std::string(label) + ": the backends disagree by more than "
                    + std::to_string(limit));
    return 0;
}

}

int main() {
    const std::filesystem::path root = scratchDir("backend");
    std::string error;
    int rc = 0;

    Config cfg = baseConfig(root / "a");
    cfg.nx = 96;
    cfg.ny = 48;
    cfg.totalTime = 0.15;
    cfg.mgIterations = 10;
    cfg.geometryFile = "";

    RestartData both, noAvx, oneThread;
    if (!runWith(cfg, true, true, 0, both, error))
        return fail("avx2 + openmp: " + error);

    cfg.outputDir = (root / "b").string();
    if (!runWith(cfg, false, true, 0, noAvx, error))
        return fail("scalar: " + error);

    cfg.outputDir = (root / "c").string();
    if (!runWith(cfg, true, false, 1, oneThread, error))
        return fail("single thread: " + error);

    rc |= compare(both, noAvx, "avx2 on against off", 1e-3f);

    rc |= compare(both, oneThread, "many threads against one", 1e-4f);

    removeDir(root);
    if (rc)
        return rc;
    std::printf("BackendAgreementTests OK\n");
    return 0;
}
