#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace maskui {

enum ParameterIndex : std::size_t {
    WindSpeed,
    Viscosity,
    Density,
    Phases,
    Density1,
    Viscosity1,
    Density2,
    Viscosity2,
    PhaseInitKind,
    PhaseLevel,
    PhaseSpotX,
    PhaseSpotY,
    VofSchemeKind,
    MixingKindRow,
    Diffusivity,
    SurfaceTension,
    ContactAngle,
    SourceLine,
    GravityEnabled,
    GravityAccel,
    GravityAngle,
    GravityMode,
    RegimeKind,
    Gamma1,
    GasConstant1,
    Gamma2,
    GasConstant2,
    Temperature0,
    AmbientPressure,
    MachInlet,
    SpeciesModeRow,
    GridStretchKind,
    StretchRatio,
    RefineNear,
    AmrLevels,
    AmrCriterionKind,
    AmrThreshold,
    AmrEvery,
    AcousticFields,
    AcousticWindow,
    AcousticRef,
    MicrophoneLine,
    MicInterval,
    MicAudio,
    MicAudioRate,
    MicAudioSpeed,
    TurbulenceKindRow,
    SmagorinskyCs,
    TurbIntensity,
    TurbLengthScale,
    SliceX,
    SliceZ,
    SliceRotation,
    Profiles,
    CaseKind,
    LidSpeed,
    BcLeft,
    BcRight,
    BcBottom,
    BcTop,
    BcLeftSpeed,
    BcRightSpeed,
    BcBottomSpeed,
    BcTopSpeed,
    InletFrom,
    InletTo,
    InletProfileKind,
    WallMotionLine,
    BodySelect,
    BodyBehaviour,
    BodyRotation,
    BodySlideX,
    BodySlideY,
    BodyVelocityX,
    BodyVelocityY,
    BodySpin,
    BodyMass,
    BodyDensity,
    BodyPins,
    BodyMotionLine,
    BodyTrackLine,
    BodyCouplingKind,
    BodyCollisions,
    BodyRestitution,
    BodyForceReport,
    DomainX,
    DomainY,
    CellsX,
    CellsY,
    Cfl,
    TotalTime,
    SteadyTolerance,
    AddTime,
    DtUpdateInterval,
    DtSafety,
    Convection,
    Limiter,
    TimeSchemeKind,
    CoarseSorOmega,
    SmootherOmega,
    MgIterations,
    MgTolerance,
    MgMinCoarseSize,
    SaveInterval,
    ExtraFields,
    UseCuda,
    UseAvx2,
    UseOpenMp,
    SolverThreads,
    CacheMegabytes,
    ParameterCount
};

struct ParameterGroupInfo {
    std::size_t firstIndex;
    const char* label;
};

constexpr std::array<ParameterGroupInfo, 16> PARAMETER_GROUPS{{
    {WindSpeed, "FLOW"},
    {Phases, "FLUIDS"},
    {RegimeKind, "GAS / COMPRESSIBLE"},
    {AcousticFields, "ACOUSTICS"},
    {TurbulenceKindRow, "TURBULENCE"},
    {SliceX, "GEOMETRY"},
    {CaseKind, "BOUNDARIES"},
    {WallMotionLine, "WALLS"},
    {BodySelect, "BODIES"},
    {DomainX, "DOMAIN / GRID"},
    {Cfl, "TIME"},
    {Convection, "NUMERICS"},
    {CoarseSorOmega, "PRESSURE / MULTIGRID"},
    {SaveInterval, "OUTPUT"},
    {UseCuda, "ACCELERATION"},
    {CacheMegabytes, "UI"}
}};

struct ParameterTabInfo {
    const char* label;
    std::array<int, 6> groups;
};

constexpr std::array<ParameterTabInfo, 5> PARAMETER_TABS{{
    {"All",   {{-1, -1, -1, -1, -1, -1}}},
    {"Flow",  {{0, 1, 2, 3, 4, -1}}},
    {"Shape", {{5, 6, 7, 8, -1, -1}}},
    {"Grid",  {{9, 10, 11, 12, -1, -1}}},
    {"Run",   {{13, 14, 15, -1, -1, -1}}}
}};

const char* parameterKey(std::size_t index);

const char* parameterHint(std::size_t index);

std::string parameterHelp(std::size_t index);

} // namespace maskui
