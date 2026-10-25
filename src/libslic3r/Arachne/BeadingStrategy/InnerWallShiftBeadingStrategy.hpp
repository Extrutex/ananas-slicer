#ifndef INNER_WALL_SHIFT_BEADING_STRATEGY_H
#define INNER_WALL_SHIFT_BEADING_STRATEGY_H

#include <string>

#include "BeadingStrategy.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

/*!
 * This is a meta-strategy that moves every bead behind the outer wall inwards, away from the outer
 * wall, as far as the wall has room for it.
 *
 * It realises the precise outer wall as a shift of toolpaths: the outline and therefore the thickness
 * the parent strategies decide on stay those of the model. The outer bead keeps its place, with its
 * outer edge on the model surface. The beads behind it and the 0-width bead that marks the end of the
 * walled area move inwards by `inner_wall_shift`, but never by more than half of the gap the beading
 * leaves in the middle of the wall. A wall the beads fill completely has no such gap and comes out as
 * the parent computed it, so thin features keep their thickness and their volume.
 *
 * It has to be applied after the LimitedBeadingStrategy: that strategy places the marker bead and
 * knows the real thickness of a wall whose bead count it limited.
 *
 * `first_shifted_bead` is the index of the first bead that moves, counted from each outline. It is 1
 * for a wall that contains its outer wall. For the inner walls that PerimeterGenerator computes on
 * their own behind an already placed single outer wall (only_one_wall_top), it is 0: there the first
 * bead of the region is the first wall behind the outer wall and has to move as well.
 */
class InnerWallShiftBeadingStrategy : public BeadingStrategy
{
public:
    InnerWallShiftBeadingStrategy(coord_t inner_wall_shift, BeadingStrategyPtr parent, size_t first_shifted_bead = 1);

    ~InnerWallShiftBeadingStrategy() override = default;

    Beading compute(coord_t thickness, coord_t bead_count) const override;

    coord_t getOptimalThickness(coord_t bead_count) const override;
    coord_t getTransitionThickness(coord_t lower_bead_count) const override;
    coord_t getOptimalBeadCount(coord_t thickness) const override;
    coord_t getTransitioningLength(coord_t lower_bead_count) const override;
    float   getTransitionAnchorPos(coord_t lower_bead_count) const override;
    std::vector<coord_t> getNonlinearThicknesses(coord_t lower_bead_count) const override;

    std::string toString() const override;

private:
    BeadingStrategyPtr parent;
    coord_t            inner_wall_shift;
    size_t             first_shifted_bead;
};

} // namespace Slic3r::Arachne
#endif // INNER_WALL_SHIFT_BEADING_STRATEGY_H
