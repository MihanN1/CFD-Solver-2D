#include "AmrHierarchy.hpp"
#include "CompressibleKernels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using namespace cfd;

std::string lowerCase(std::string text) {
    for (char& character : text)
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    return text;
}

float minmod(float back, float forward) {
    if (back * forward <= 0.0f)
        return 0.0f;
    return std::fabs(back) < std::fabs(forward) ? back : forward;
}

}

AmrBox amrIntersect(const AmrBox& a, const AmrBox& b) {
    AmrBox out;
    out.i0 = std::max(a.i0, b.i0);
    out.j0 = std::max(a.j0, b.j0);
    out.nx = std::min(a.i1(), b.i1()) - out.i0;
    out.ny = std::min(a.j1(), b.j1()) - out.j0;
    if (out.nx < 0)
        out.nx = 0;
    if (out.ny < 0)
        out.ny = 0;
    return out;
}

AmrBox amrGrow(const AmrBox& box, int by, int limitNx, int limitNy) {
    AmrBox out;
    out.i0 = std::max(0, box.i0 - by);
    out.j0 = std::max(0, box.j0 - by);
    out.nx = std::min(limitNx, box.i1() + by) - out.i0;
    out.ny = std::min(limitNy, box.j1() + by) - out.j0;
    return out;
}

AmrBox amrRefine(const AmrBox& box, int ratio) {
    AmrBox out;
    out.i0 = box.i0 * ratio;
    out.j0 = box.j0 * ratio;
    out.nx = box.nx * ratio;
    out.ny = box.ny * ratio;
    return out;
}

AmrBox amrCoarsen(const AmrBox& box, int ratio) {
    AmrBox out;
    out.i0 = box.i0 / ratio;
    out.j0 = box.j0 / ratio;
    out.nx = (box.i1() + ratio - 1) / ratio - out.i0;
    out.ny = (box.j1() + ratio - 1) / ratio - out.j0;
    return out;
}

namespace {

long long countTags(const std::vector<uint8_t>& tags, int nx,
                    const AmrBox& box) {
    long long total = 0;
    for (int j = box.j0; j < box.j1(); ++j)
        for (int i = box.i0; i < box.i1(); ++i)
            total += tags[static_cast<std::size_t>(j) * nx + i] ? 1 : 0;
    return total;
}

AmrBox shrinkToTags(const std::vector<uint8_t>& tags, int nx,
                    const AmrBox& box) {
    int lowI = box.i1(), highI = box.i0 - 1;
    int lowJ = box.j1(), highJ = box.j0 - 1;
    for (int j = box.j0; j < box.j1(); ++j)
        for (int i = box.i0; i < box.i1(); ++i) {
            if (!tags[static_cast<std::size_t>(j) * nx + i])
                continue;
            lowI = std::min(lowI, i);
            highI = std::max(highI, i);
            lowJ = std::min(lowJ, j);
            highJ = std::max(highJ, j);
        }
    AmrBox out;
    if (highI < lowI || highJ < lowJ)
        return out;
    out.i0 = lowI;
    out.j0 = lowJ;
    out.nx = highI - lowI + 1;
    out.ny = highJ - lowJ + 1;
    return out;
}

void clusterInto(const std::vector<uint8_t>& tags,
                 int nx,
                 const AmrBox& region,
                 int minSide,
                 int maxSide,
                 double fillTarget,
                 int depth,
                 std::vector<AmrBox>& out) {
    const AmrBox box = shrinkToTags(tags, nx, region);
    if (box.empty())
        return;

    const long long tagged = countTags(tags, nx, box);
    if (tagged <= 0)
        return;

    const double fill = static_cast<double>(tagged) /
                        static_cast<double>(box.area());
    const bool small = box.nx <= minSide && box.ny <= minSide;
    const bool fits = box.nx <= maxSide && box.ny <= maxSide;

    if ((fill >= fillTarget && fits) || small || depth >= 12) {
        if (fits) {
            out.push_back(box);
            return;
        }
    }

    const bool splitX = box.nx >= box.ny;
    const int span = splitX ? box.nx : box.ny;
    if (span < 2 * minSide && fits) {
        out.push_back(box);
        return;
    }

    std::vector<long long> signature(static_cast<std::size_t>(span), 0);
    for (int j = box.j0; j < box.j1(); ++j)
        for (int i = box.i0; i < box.i1(); ++i) {
            if (!tags[static_cast<std::size_t>(j) * nx + i])
                continue;
            const int slot = splitX ? i - box.i0 : j - box.j0;
            ++signature[static_cast<std::size_t>(slot)];
        }

    int cut = -1;
    for (int k = minSide; k <= span - minSide; ++k)
        if (signature[static_cast<std::size_t>(k)] == 0) {
            cut = k;
            break;
        }

    if (cut < 0) {
        long long best = -1;
        for (int k = minSide; k <= span - minSide; ++k) {
            const long long left =
                signature[static_cast<std::size_t>(k)] -
                signature[static_cast<std::size_t>(k - 1)];
            const long long right =
                signature[static_cast<std::size_t>(k)] -
                (k + 1 < span ? signature[static_cast<std::size_t>(k + 1)] : 0);
            const long long turn = std::llabs(left - right);
            if (turn > best) {
                best = turn;
                cut = k;
            }
        }
    }

    if (cut < minSide || cut > span - minSide) {
        if (fits) {
            out.push_back(box);
            return;
        }
        cut = span / 2;
        if (cut < 1)
            return;
    }

    AmrBox low = box;
    AmrBox high = box;
    if (splitX) {
        low.nx = cut;
        high.i0 = box.i0 + cut;
        high.nx = box.nx - cut;
    } else {
        low.ny = cut;
        high.j0 = box.j0 + cut;
        high.ny = box.ny - cut;
    }
    clusterInto(tags, nx, low, minSide, maxSide, fillTarget, depth + 1, out);
    clusterInto(tags, nx, high, minSide, maxSide, fillTarget, depth + 1, out);
}

}

std::vector<AmrBox> amrCluster(const std::vector<uint8_t>& tags,
                               int nx,
                               int ny,
                               int minSide,
                               int maxSide,
                               double fillTarget) {
    std::vector<AmrBox> out;
    AmrBox whole;
    whole.i0 = 0;
    whole.j0 = 0;
    whole.nx = nx;
    whole.ny = ny;
    clusterInto(tags, nx, whole, std::max(1, minSide), std::max(minSide, maxSide),
                fillTarget, 0, out);
    return out;
}

std::string amrCriterionName(AmrCriterion kind) {
    switch (kind) {
    case AmrCriterion::Density:
        return "density";
    case AmrCriterion::Vorticity:
        return "vorticity";
    case AmrCriterion::Species:
        return "species";
    case AmrCriterion::Body:
        return "body";
    case AmrCriterion::Everything:
    default:
        return "everything";
    }
}

bool parseAmrCriterion(const std::string& text, AmrCriterion& out) {
    const std::string name = lowerCase(text);
    if (name == "density" || name == "shock") {
        out = AmrCriterion::Density;
        return true;
    }
    if (name == "vorticity" || name == "wake") {
        out = AmrCriterion::Vorticity;
        return true;
    }
    if (name == "species" || name == "mixing") {
        out = AmrCriterion::Species;
        return true;
    }
    if (name == "body" || name == "wall") {
        out = AmrCriterion::Body;
        return true;
    }
    if (name == "everything" || name == "all" || name.empty()) {
        out = AmrCriterion::Everything;
        return true;
    }
    return false;
}

void AmrPatch::allocate(const AmrBox& region, int ghostWidth,
                        bool carriesSpecies) {
    box = region;
    ghost = ghostWidth;
    species = carriesSpecies;
    stride = box.nx + 2 * ghost;
    rows = box.ny + 2 * ghost;
    const std::size_t total =
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(rows);
    for (int set = 0; set < 4; ++set)
        for (int component = 0; component < 5; ++component) {
            if (component == 4 && !species) {
                sets[set][component].clear();
                continue;
            }
            sets[set][component].assign(total, 0.0f);
        }
    const std::size_t cells =
        static_cast<std::size_t>(box.nx) * static_cast<std::size_t>(box.ny);
    solid.assign(cells, 0);
    solidU.assign(cells, 0.0f);
    solidV.assign(cells, 0.0f);
}

Block AmrPatch::view(int set, float dx, float dy) {
    Block block;
    block.nx = box.nx;
    block.ny = box.ny;
    block.ghost = ghost;
    block.stride = stride;
    block.rows = rows;
    block.dx = dx;
    block.dy = dy;
    block.x0 = box.i0 * dx;
    block.y0 = box.j0 * dy;
    block.rho = sets[set][0].data();
    block.rhou = sets[set][1].data();
    block.rhov = sets[set][2].data();
    block.rhoE = sets[set][3].data();
    block.rhoY = species ? sets[set][4].data() : nullptr;
    block.solid = solid.data();
    block.solidU = solidU.data();
    block.solidV = solidV.data();
    return block;
}

Block AmrPatch::view(int set, float dx, float dy) const {
    return const_cast<AmrPatch*>(this)->view(set, dx, dy);
}

void AmrHierarchy::build(const AmrSettings& settings,
                         int baseNx,
                         int baseNy,
                         float baseDx,
                         float baseDy,
                         bool species) {
    levels_.clear();
    species_ = species;
    if (settings.levels <= 0)
        return;

    levels_.resize(static_cast<std::size_t>(settings.levels));
    int ratio = 1;
    for (int which = 0; which < settings.levels; ++which) {
        ratio *= 2;
        AmrLevel& here = levels_[static_cast<std::size_t>(which)];
        here.ratio = ratio;
        here.dx = baseDx / ratio;
        here.dy = baseDy / ratio;
        here.nx = baseNx * ratio;
        here.ny = baseNy * ratio;
    }
}

void AmrHierarchy::tagFrom(const Block& base,
                           const GasModel& gas,
                           const AmrSettings& settings,
                           std::vector<uint8_t>& tags) const {
    const int nx = base.nx;
    const int ny = base.ny;
    tags.assign(static_cast<std::size_t>(nx) * ny, 0);
    if (nx < 3 || ny < 3)
        return;

    std::vector<float> score(static_cast<std::size_t>(nx) * ny, 0.0f);
    const bool wantDensity = settings.criterion == AmrCriterion::Density ||
                             settings.criterion == AmrCriterion::Everything;
    const bool wantVorticity = settings.criterion == AmrCriterion::Vorticity ||
                               settings.criterion == AmrCriterion::Everything;
    const bool wantSpecies = (settings.criterion == AmrCriterion::Species ||
                              settings.criterion == AmrCriterion::Everything) &&
                             base.rhoY != nullptr;
    const bool wantBody = settings.criterion == AmrCriterion::Body ||
                          settings.criterion == AmrCriterion::Everything;

    float worst = 0.0f;
    for (int j = 1; j < ny - 1; ++j)
        for (int i = 1; i < nx - 1; ++i) {
            const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
            if (base.solid && base.solid[flat])
                continue;

            const Primitive here = primitiveOf(base, gas, base.index(i, j));
            const Primitive east = primitiveOf(base, gas, base.index(i + 1, j));
            const Primitive west = primitiveOf(base, gas, base.index(i - 1, j));
            const Primitive north =
                primitiveOf(base, gas, base.index(i, j + 1));
            const Primitive south =
                primitiveOf(base, gas, base.index(i, j - 1));

            float value = 0.0f;
            if (wantDensity) {
                const float jump =
                    std::fabs(east.rho - west.rho) +
                    std::fabs(north.rho - south.rho);
                value = std::max(value, jump / std::max(here.rho, kFloor));
            }
            if (wantVorticity) {
                const float curl =
                    (north.u - south.u) / (2.0f * base.heightAt(j)) -
                    (east.v - west.v) / (2.0f * base.widthAt(i));
                const float scale =
                    std::max(1.0f, std::fabs(here.u) + std::fabs(here.v));
                value = std::max(value,
                                 std::fabs(curl) * base.widthAt(i) / scale);
            }
            if (wantSpecies) {
                const float jump = std::fabs(east.y - west.y) +
                                   std::fabs(north.y - south.y);
                value = std::max(value, jump);
            }
            score[flat] = value;
            worst = std::max(worst, value);
        }

    const float cut = settings.threshold * worst;
    for (std::size_t flat = 0; flat < score.size(); ++flat)
        if (worst > 0.0f && score[flat] >= cut && score[flat] > 0.0f)
            tags[flat] = 1;

    if (wantBody && base.solid) {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::size_t flat = static_cast<std::size_t>(j) * nx + i;
                if (!base.solid[flat])
                    continue;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) {
                        const int ni = i + di;
                        const int nj = j + dj;
                        if (ni < 0 || ni >= nx || nj < 0 || nj >= ny)
                            continue;
                        const std::size_t at =
                            static_cast<std::size_t>(nj) * nx + ni;
                        if (!base.solid[at])
                            tags[at] = 1;
                    }
            }
    }

    if (settings.buffer > 0) {
        std::vector<uint8_t> grown = tags;
        const int reach = settings.buffer;
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                if (!tags[static_cast<std::size_t>(j) * nx + i])
                    continue;
                for (int dj = -reach; dj <= reach; ++dj)
                    for (int di = -reach; di <= reach; ++di) {
                        const int ni = i + di;
                        const int nj = j + dj;
                        if (ni < 0 || ni >= nx || nj < 0 || nj >= ny)
                            continue;
                        grown[static_cast<std::size_t>(nj) * nx + ni] = 1;
                    }
            }
        tags.swap(grown);
    }
}

void AmrHierarchy::regrid(const Block& base,
                          const GasModel& gas,
                          const AmrSettings& settings,
                          const std::vector<uint8_t>& baseSolid) {
    if (levels_.empty())
        return;

    std::vector<uint8_t> tags;
    tagFrom(base, gas, settings, tags);

    struct Seed {
        AmrBox box;
        int parent;
    };
    std::vector<Seed> seeds;
    for (const AmrBox& found :
         amrCluster(tags, base.nx, base.ny, settings.minSide,
                    std::max(settings.minSide, settings.maxSide / 2),
                    settings.fillTarget))
        seeds.push_back({found, -1});

    std::vector<std::vector<AmrPatch>> previous;
    previous.reserve(levels_.size());
    for (AmrLevel& here : levels_)
        previous.push_back(std::move(here.patches));

    for (int which = 0; which < depth(); ++which) {
        AmrLevel& here = levels_[static_cast<std::size_t>(which)];
        std::vector<AmrPatch> built;
        built.reserve(seeds.size());

        for (const Seed& seed : seeds) {
            AmrBox fine = amrRefine(seed.box, 2);
            fine.i0 = std::max(0, fine.i0);
            fine.j0 = std::max(0, fine.j0);
            fine.nx = std::min(here.nx, fine.i1()) - fine.i0;
            fine.ny = std::min(here.ny, fine.j1()) - fine.j0;
            fine.nx -= fine.nx % 2;
            fine.ny -= fine.ny % 2;
            if (fine.nx < 4 || fine.ny < 4)
                continue;
            AmrPatch patch;
            patch.allocate(fine, base.ghost, species_);
            patch.parent = seed.parent;
            built.push_back(std::move(patch));
        }

        here.patches = std::move(built);
        if (here.patches.empty()) {
            for (int deeper = which; deeper < depth(); ++deeper)
                levels_[static_cast<std::size_t>(deeper)].patches.clear();
            return;
        }

        setSolidFromPoint(which, baseSolid, base.nx, base.ny);
        seedLevel(which, base);
        carryOver(which, previous[static_cast<std::size_t>(which)]);

        if (which + 1 >= depth())
            break;

        std::vector<Seed> next;
        const int trim = std::max(2, settings.buffer);
        for (std::size_t index = 0; index < here.patches.size(); ++index) {
            AmrBox shrunk = here.patches[index].box;
            shrunk.i0 += trim;
            shrunk.j0 += trim;
            shrunk.nx -= 2 * trim;
            shrunk.ny -= 2 * trim;
            shrunk.nx -= shrunk.nx % 2;
            shrunk.ny -= shrunk.ny % 2;
            if (shrunk.nx >= 4 && shrunk.ny >= 4)
                next.push_back({shrunk, static_cast<int>(index)});
        }
        seeds = std::move(next);
        if (seeds.empty()) {
            for (int deeper = which + 1; deeper < depth(); ++deeper)
                levels_[static_cast<std::size_t>(deeper)].patches.clear();
            return;
        }
    }
}

void AmrHierarchy::setSolidFromPoint(int which,
                                     const std::vector<uint8_t>& baseSolid,
                                     int baseNx,
                                     int baseNy) {
    AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    const int ratio = here.ratio;
    for (AmrPatch& patch : here.patches) {
        for (int j = 0; j < patch.box.ny; ++j)
            for (int i = 0; i < patch.box.nx; ++i) {
                const int globalI = patch.box.i0 + i;
                const int globalJ = patch.box.j0 + j;
                const int coarseI = std::min(baseNx - 1, globalI / ratio);
                const int coarseJ = std::min(baseNy - 1, globalJ / ratio);
                patch.solid[static_cast<std::size_t>(j) * patch.box.nx + i] =
                    baseSolid[static_cast<std::size_t>(coarseJ) * baseNx +
                              coarseI];
            }
    }
}

Block AmrHierarchy::coarseViewFor(int which, const Block& base,
                                  int patchIndex, AmrBox& coarseBox) {
    if (which == 0) {
        coarseBox.i0 = 0;
        coarseBox.j0 = 0;
        coarseBox.nx = base.nx;
        coarseBox.ny = base.ny;
        return base;
    }
    AmrLevel& above = levels_[static_cast<std::size_t>(which - 1)];
    AmrPatch& parent = above.patches[static_cast<std::size_t>(patchIndex)];
    coarseBox = parent.box;
    return parent.view(0, above.dx, above.dy);
}

void AmrHierarchy::interpolateInto(AmrPatch& patch,
                                   int which,
                                   const Block& coarse,
                                   const AmrBox& coarseBox,
                                   bool interiorToo) {
    AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    Block fine = patch.view(0, here.dx, here.dy);
    const int ghost = patch.ghost;

    const float* const source[5] = {coarse.rho, coarse.rhou, coarse.rhov,
                                    coarse.rhoE, coarse.rhoY};
    float* const target[5] = {fine.rho, fine.rhou, fine.rhov, fine.rhoE,
                              fine.rhoY};
    const int components = species_ ? 5 : 4;

    for (int j = -ghost; j < patch.box.ny + ghost; ++j)
        for (int i = -ghost; i < patch.box.nx + ghost; ++i) {
            const bool interior =
                i >= 0 && i < patch.box.nx && j >= 0 && j < patch.box.ny;
            if (interior && !interiorToo)
                continue;

            const int globalI = patch.box.i0 + i;
            const int globalJ = patch.box.j0 + j;
            const int coarseGlobalI =
                globalI >= 0 ? globalI / 2 : -((-globalI + 1) / 2);
            const int coarseGlobalJ =
                globalJ >= 0 ? globalJ / 2 : -((-globalJ + 1) / 2);

            const int localI = std::clamp(coarseGlobalI - coarseBox.i0,
                                          -coarse.ghost + 1,
                                          coarse.nx + coarse.ghost - 2);
            const int localJ = std::clamp(coarseGlobalJ - coarseBox.j0,
                                          -coarse.ghost + 1,
                                          coarse.ny + coarse.ghost - 2);

            const int centre = coarse.index(localI, localJ);
            const float offsetX =
                (globalI - 2 * coarseGlobalI) == 0 ? -0.25f : 0.25f;
            const float offsetY =
                (globalJ - 2 * coarseGlobalJ) == 0 ? -0.25f : 0.25f;

            const int at = fine.index(i, j);
            for (int component = 0; component < components; ++component) {
                const float* from = source[component];
                if (!from || !target[component])
                    continue;
                const float middle = from[centre];
                const float slopeX = minmod(middle - from[centre - 1],
                                            from[centre + 1] - middle);
                const float slopeY =
                    minmod(middle - from[centre - coarse.stride],
                           from[centre + coarse.stride] - middle);
                target[component][at] =
                    middle + slopeX * offsetX + slopeY * offsetY;
            }
            if (target[0][at] < kFloor)
                target[0][at] = kFloor;
        }
}

void AmrHierarchy::seedLevel(int which, const Block& base) {
    AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    for (std::size_t index = 0; index < here.patches.size(); ++index) {
        AmrPatch& patch = here.patches[index];
        AmrBox coarseBox;
        const Block coarse =
            coarseViewFor(which, base, patch.parent, coarseBox);
        interpolateInto(patch, which, coarse, coarseBox, true);
        for (int set = 1; set < 4; ++set)
            for (int component = 0; component < 5; ++component)
                if (!patch.sets[0][component].empty())
                    patch.sets[set][component] = patch.sets[0][component];
    }
}

void AmrHierarchy::carryOver(int which,
                             const std::vector<AmrPatch>& previous) {
    if (previous.empty())
        return;
    AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    for (AmrPatch& into : here.patches) {
        Block destination = into.view(0, here.dx, here.dy);
        for (const AmrPatch& from : previous) {
            Block origin =
                const_cast<AmrPatch&>(from).view(0, here.dx, here.dy);
            amrCopyOverlap(origin, destination, from.box, into.box, species_);
        }
        for (int set = 1; set < 4; ++set)
            for (int component = 0; component < 5; ++component)
                if (!into.sets[0][component].empty())
                    into.sets[set][component] = into.sets[0][component];
    }
}

void AmrHierarchy::fillGhostsFor(int which,
                                 int parentIndex,
                                 const Block& coarse,
                                 const AmrBox& coarseBox) {
    AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    for (AmrPatch& patch : here.patches) {
        if (patch.parent != parentIndex)
            continue;
        interpolateInto(patch, which, coarse, coarseBox, false);
    }

    for (std::size_t target = 0; target < here.patches.size(); ++target) {
        AmrPatch& into = here.patches[target];
        if (into.parent != parentIndex)
            continue;
        Block destination = into.view(0, here.dx, here.dy);
        for (std::size_t source = 0; source < here.patches.size(); ++source) {
            if (source == target)
                continue;
            const AmrPatch& from = here.patches[source];
            if (from.parent != parentIndex)
                continue;
            Block origin = from.view(0, here.dx, here.dy);
            amrCopyOverlap(origin, destination, from.box, into.box, species_);
        }
    }
}

void amrCopyOverlap(const Block& from, Block& to, const AmrBox& fromBox,
                    const AmrBox& toBox, bool species) {
    AmrBox reach = toBox;
    reach.i0 -= to.ghost;
    reach.j0 -= to.ghost;
    reach.nx += 2 * to.ghost;
    reach.ny += 2 * to.ghost;
    const AmrBox shared = amrIntersect(reach, fromBox);
    if (shared.empty())
        return;

    for (int j = shared.j0; j < shared.j1(); ++j)
        for (int i = shared.i0; i < shared.i1(); ++i) {
            const int toIndex = to.index(i - toBox.i0, j - toBox.j0);
            const int fromIndex = from.index(i - fromBox.i0, j - fromBox.j0);
            to.rho[toIndex] = from.rho[fromIndex];
            to.rhou[toIndex] = from.rhou[fromIndex];
            to.rhov[toIndex] = from.rhov[fromIndex];
            to.rhoE[toIndex] = from.rhoE[fromIndex];
            if (species && to.rhoY && from.rhoY)
                to.rhoY[toIndex] = from.rhoY[fromIndex];
        }
}

void AmrHierarchy::averageDownFor(int which,
                                  int parentIndex,
                                  Block& coarse,
                                  const AmrBox& coarseBox) const {
    const AmrLevel& here = levels_[static_cast<std::size_t>(which)];
    for (const AmrPatch& patch : here.patches) {
        if (patch.parent != parentIndex)
            continue;
        Block fine = patch.view(0, here.dx, here.dy);
        const AmrBox target = amrIntersect(amrCoarsen(patch.box, 2), coarseBox);
        for (int j = target.j0; j < target.j1(); ++j)
            for (int i = target.i0; i < target.i1(); ++i) {
                const int fineI = i * 2 - patch.box.i0;
                const int fineJ = j * 2 - patch.box.j0;
                if (fineI < 0 || fineJ < 0 || fineI + 1 >= patch.box.nx ||
                    fineJ + 1 >= patch.box.ny)
                    continue;

                const int localI = i - coarseBox.i0;
                const int localJ = j - coarseBox.j0;
                if (localI < 0 || localJ < 0 || localI >= coarse.nx ||
                    localJ >= coarse.ny)
                    continue;
                if (coarse.solid &&
                    coarse.solid[static_cast<std::size_t>(localJ) * coarse.nx +
                                 localI])
                    continue;

                const int coarseAt = coarse.index(localI, localJ);
                const int a = fine.index(fineI, fineJ);
                const int b = fine.index(fineI + 1, fineJ);
                const int c = fine.index(fineI, fineJ + 1);
                const int d = fine.index(fineI + 1, fineJ + 1);
                const auto mean = [&](const float* field) {
                    return 0.25f *
                           (field[a] + field[b] + field[c] + field[d]);
                };
                coarse.rho[coarseAt] = mean(fine.rho);
                coarse.rhou[coarseAt] = mean(fine.rhou);
                coarse.rhov[coarseAt] = mean(fine.rhov);
                coarse.rhoE[coarseAt] = mean(fine.rhoE);
                if (coarse.rhoY && fine.rhoY)
                    coarse.rhoY[coarseAt] = mean(fine.rhoY);
            }
    }
}

long long AmrHierarchy::cellCount() const {
    long long total = 0;
    for (const AmrLevel& here : levels_)
        for (const AmrPatch& patch : here.patches)
            total += patch.box.area();
    return total;
}

void AmrHierarchy::describe(std::vector<int>& patchesPerLevel,
                            std::vector<long long>& cellsPerLevel) const {
    patchesPerLevel.clear();
    cellsPerLevel.clear();
    for (const AmrLevel& here : levels_) {
        patchesPerLevel.push_back(static_cast<int>(here.patches.size()));
        long long cells = 0;
        for (const AmrPatch& patch : here.patches)
            cells += patch.box.area();
        cellsPerLevel.push_back(cells);
    }
}
