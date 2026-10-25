#include "InnerWallShiftBeadingStrategy.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r::Arachne
{

InnerWallShiftBeadingStrategy::InnerWallShiftBeadingStrategy(coord_t inner_wall_shift, BeadingStrategyPtr parent, size_t first_shifted_bead)
    : BeadingStrategy(*parent), parent(std::move(parent)), inner_wall_shift(inner_wall_shift), first_shifted_bead(first_shifted_bead)
{
    name = "InnerWallShiftBeadingStrategy";
}

coord_t InnerWallShiftBeadingStrategy::getOptimalThickness(coord_t bead_count) const
{
    return parent->getOptimalThickness(bead_count);
}

coord_t InnerWallShiftBeadingStrategy::getTransitionThickness(coord_t lower_bead_count) const
{
    return parent->getTransitionThickness(lower_bead_count);
}

coord_t InnerWallShiftBeadingStrategy::getOptimalBeadCount(coord_t thickness) const
{
    return parent->getOptimalBeadCount(thickness);
}

coord_t InnerWallShiftBeadingStrategy::getTransitioningLength(coord_t lower_bead_count) const
{
    return parent->getTransitioningLength(lower_bead_count);
}

float InnerWallShiftBeadingStrategy::getTransitionAnchorPos(coord_t lower_bead_count) const
{
    return parent->getTransitionAnchorPos(lower_bead_count);
}

std::vector<coord_t> InnerWallShiftBeadingStrategy::getNonlinearThicknesses(coord_t lower_bead_count) const
{
    return parent->getNonlinearThicknesses(lower_bead_count);
}

std::string InnerWallShiftBeadingStrategy::toString() const
{
    return std::string("InnerWallShiftBeadingStrategy+") + parent->toString();
}

BeadingStrategy::Beading InnerWallShiftBeadingStrategy::compute(coord_t thickness, coord_t bead_count) const
{
    Beading ret = parent->compute(thickness, bead_count);

    // Beads are listed from one outline to the other. Only the half next to the first outline is
    // shifted here, the other half mirrors it. An odd middle bead belongs to neither half.
    const size_t count = ret.toolpath_locations.size();
    const size_t half  = count / 2;
    // Without a printed bead to move on its own side of the middle there is nothing to do: the marker
    // bead of a single outer wall stays at that wall's inner edge.
    const size_t first = first_shifted_bead;
    if (half <= first || std::none_of(ret.bead_widths.begin() + first, ret.bead_widths.begin() + half, [](const coord_t width) { return width > 0; }))
        return ret;

    // The gap between the innermost bead of this half and the middle of the wall, or the middle bead.
    const coord_t inner_edge = ret.toolpath_locations[half - 1] + ret.bead_widths[half - 1] / 2;
    const coord_t middle     = count % 2 == 1 ? ret.toolpath_locations[half] - ret.bead_widths[half] / 2 : thickness / 2;
    const coord_t shift      = std::clamp<coord_t>(middle - inner_edge, 0, inner_wall_shift);
    if (shift == 0)
        return ret;

    for (size_t bead_idx = first; bead_idx < half; ++bead_idx) {
        ret.toolpath_locations[bead_idx]             += shift;
        ret.toolpath_locations[count - 1 - bead_idx] -= shift;
    }
    return ret;
}

} // namespace Slic3r::Arachne
