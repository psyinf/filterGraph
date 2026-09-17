#pragma once

#include <any>
#include <typeindex>
#include <vector>

namespace filterGraph {

// The input of a merge stage: one slot per fan-in edge, in the order the edges
// are listed in the DSL group `(a, b, c) -> Merge`. An empty slot is a "hole"
// left by an upstream path that dropped the message via std::nullopt.
using MergeInputs = std::vector<std::any>;

// The slot types a merge stage declares, so that the DSL can check the edges of
// its fan-in group when the graph is built.
struct MergeSlotTypes
{
    // One type per slot, in slot order; a uniform merge lists its single slot
    // type. Empty for an untyped merge, whose edges are not checked.
    std::vector<std::type_index> types;

    // Any number of slots, each of types.front().
    bool uniform = false;

    bool empty() const noexcept
    {
        return types.empty();
    }
};

// Implemented by merge stages (MessageFilter<MergeInputs, Out>) that declare
// their slot types. TypedMergeFilter and UniformMergeFilter implement it; a
// hand-written merge stage may derive from it too.
class MergeStage
{
public:
    virtual ~MergeStage() = default;

    virtual MergeSlotTypes mergeInputTypes() const = 0;
};

} // namespace filterGraph
