#include "TestHarness.hpp"

#include <cstdio>

using namespace testing;

namespace {

// Everything the accelerators do is meant to be invisible in the answer. Every
// row of the release matrix is the same program built with a different subset
// of them, so anything that stops agreeing here ships as several solvers that
// quietly disagree with each other.
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

}   // namespace

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

    // The vector and scalar kernels sum in a different order and the pressure
    // lands on a slightly different multigrid iterate, so this is a tolerance
    // rather than an equality. It is the same tolerance the separate AVX2 and
    // non-AVX2 downloads have always agreed to.
    rc |= compare(both, noAvx, "avx2 on against off", 1e-3f);
    // Threading changes no arithmetic at all, only how much of it happens at
    // once, so this one is much tighter.
    rc |= compare(both, oneThread, "many threads against one", 1e-4f);

    removeDir(root);
    if (rc)
        return rc;
    std::printf("BackendAgreementTests OK\n");
    return 0;
}
