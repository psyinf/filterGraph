#pragma once

#include <any>
#include <string>
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
// their slots: their types, their names, or both. TypedMergeFilter,
// UniformMergeFilter and MergeFilter implement it; a hand-written merge stage
// may derive from it too.
class MergeStage
{
public:
    virtual ~MergeStage() = default;

    // The slot types; empty (the default) for an untyped merge.
    virtual MergeSlotTypes mergeInputTypes() const
    {
        return {};
    }

    // The slot names, in slot order; empty (the default) for a merge whose
    // slots are positional. A merge that names its slots has exactly that many,
    // and a DSL group may then match them by name, `(raw: msg, checked: valid)`,
    // in any order: the slots are handed over in the order declared here.
    virtual std::vector<std::string> mergeInputNames() const
    {
        return {};
    }
};

} // namespace filterGraph
