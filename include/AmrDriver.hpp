#pragma once
#include "AmrHierarchy.hpp"

#include <vector>

class AmrDriver {
public:
    AmrDriver(AmrHierarchy& tree,
              const GasModel& gas,
              const BlockBoundaries& sides,
              LimiterKind limiter,
              float diffusivity);

    void advance(Block& base,
                 Block& baseStage1,
                 Block& baseStage2,
                 Workspace& baseWork,
                 float dt);

    float finestRate(const Block& base, float cfl) const;

private:
    AmrHierarchy& tree_;
    const GasModel& gas_;
    const BlockBoundaries& sides_;
    LimiterKind limiter_;
    float diffusivity_;

    BlockBoundaries patchSides(int which, const AmrPatch& patch) const;

    void stageBlock(Block& current,
                    Block& stage1,
                    Block& stage2,
                    Workspace& work,
                    const BlockBoundaries& sides,
                    float dt);

    void advanceChildren(int which,
                         int parentIndex,
                         Block& coarse,
                         const AmrBox& coarseBox,
                         float dtCoarse);
};
