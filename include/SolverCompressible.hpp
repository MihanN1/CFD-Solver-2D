#pragma once
#include "Config.hpp"
#include "Mesh.hpp"
#include "Restart.hpp"

#include <cstdint>
#include <string>
#include <vector>

#if defined(__CUDACC__)
#define CFD_HD __host__ __device__
#else
#define CFD_HD
#endif

struct GasModel {
    float gamma1 = 1.4f;
    float R1 = 287.05f;
    float gamma2 = 1.667f;
    float R2 = 2077.0f;
    bool species = false;
    bool active = true;

    float cv1 = 0.0f, cv2 = 0.0f;
    float cp1 = 0.0f, cp2 = 0.0f;

    void prepare();

    float gammaOf(float y) const {
        if (!species || !active)
            return gamma1;
        const float cp = cp1 + y * (cp2 - cp1);
        const float cv = cv1 + y * (cv2 - cv1);
        return cp / cv;
    }
    float gasConstantOf(float y) const {
        if (!species || !active)
            return R1;
        return R1 + y * (R2 - R1);
    }
};

struct Block {
    int nx = 0;
    int ny = 0;
    int ghost = 2;
    int stride = 0;
    int rows = 0;
    float dx = 1.0f;
    float dy = 1.0f;
    float x0 = 0.0f;
    float y0 = 0.0f;

    float* rho = nullptr;
    float* rhou = nullptr;
    float* rhov = nullptr;
    float* rhoE = nullptr;
    float* rhoY = nullptr;

    const uint8_t* solid = nullptr;

    CFD_HD int index(int i, int j) const {
        return (j + ghost) * stride + (i + ghost);
    }
    CFD_HD int cells() const { return stride * rows; }
    CFD_HD float cellX(int i) const { return x0 + (i + 0.5f) * dx; }
    CFD_HD float cellY(int j) const { return y0 + (j + 0.5f) * dy; }
};

struct SideState {
    BoundaryKind kind = BoundaryKind::Wall;
    bool noSlip = true;
    float speed = 0.0f;
    float from = 0.0f;
    float to = 1.0f;
    bool banded = false;
};

struct BlockBoundaries {
    SideState left, right, bottom, top;
    float pInf = 101325.0f;
    float T0 = 288.15f;
    float mach = 0.5f;
    float inletY = 0.0f;
};

struct Workspace {
    std::vector<float> fluxX;
    std::vector<float> fluxY;
    std::vector<float> primitive[6];
    void fit(const Block& block, int components);
};

void fillGhostCells(Block& block,
                    const BlockBoundaries& sides,
                    const GasModel& gas);

void fillSolidCells(Block& block, const GasModel& gas);

float blockTimeStep(const Block& block, const GasModel& gas, float cfl);

void advanceStage(Block& in,
                  const Block& keep,
                  Block& out,
                  const BlockBoundaries& sides,
                  const GasModel& gas,
                  float dt,
                  float a,
                  float b,
                  LimiterKind limiter,
                  float diffusivity,
                  Workspace& work);

#ifdef USE_CUDA
struct CompressibleDevice;

bool compressibleCudaAvailable();
CompressibleDevice* compressibleCudaCreate(int nx, int ny, int ghost,
                                           bool species);
void compressibleCudaDestroy(CompressibleDevice* device);
void compressibleCudaUploadSolid(CompressibleDevice* device,
                                 const uint8_t* mask);
void compressibleCudaUpload(CompressibleDevice* device, int set,
                            const float* const* host);
void compressibleCudaDownload(CompressibleDevice* device, int set,
                              float* const* host);
float compressibleCudaTimeStep(CompressibleDevice* device, const Block& shape,
                               const GasModel& gas, float cfl);
void compressibleCudaStage(CompressibleDevice* device, const Block& shape,
                           int inSet, int keepSet, int outSet,
                           const GasModel& gas, const BlockBoundaries& sides,
                           float dt, float a, float b, int limiter,
                           float diffusivity);
#endif

class CompressibleRun {
public:
    CompressibleRun(const Config& cfg, Mesh& mesh);
    ~CompressibleRun();

    bool setInitialState(RestartData&& state, const std::string& framePrefix);
    void run();

private:
    const Config& cfg;
    Mesh& mesh;

    int nx = 0, ny = 0, ghost = 2;
    float dx = 0.0f, dy = 0.0f;
    double currentTime = 0.0;
    int step = 0;
    float dt = 0.0f;
    std::string framePrefix = "solution";
    std::filesystem::path outputPath;
    bool hasRestartState = false;

    GasModel gas;
    BlockBoundaries sides;
    std::vector<uint8_t> solidMask;

    std::vector<float> rho, rhou, rhov, rhoE, rhoY;
    std::vector<float> rho1, rhou1, rhov1, rhoE1, rhoY1;
    std::vector<float> rho2, rhou2, rhov2, rhoE2, rhoY2;

    std::vector<float> pressureMean;
    std::vector<float> pressureFast;
    std::vector<float> pressureRms;
    std::vector<float> crossingRate;
    std::vector<int8_t> lastSign;
    bool acousticsReady = false;

    std::vector<Microphone> mics;
    std::vector<std::vector<float>> micSamples;
    std::vector<float> micTimes;
    Workspace work;
    bool onDevice = false;
#ifdef USE_CUDA
    CompressibleDevice* device = nullptr;
#endif

    Block view(std::vector<float>& r,
               std::vector<float>& ru,
               std::vector<float>& rv,
               std::vector<float>& re,
               std::vector<float>& ry);

    void allocate();
    void initialise();
    void computeStep();
    float timeStep(const Block& block);
    void syncFromDevice();
    void syncToDevice();
    void updateAcoustics(float stepDt);
    void sampleMicrophones();
    void writeMicrophones() const;
    void saveVTK(int stepNumber) const;
    void reportStep() const;

    std::vector<float> primitive(const char* what) const;
};
