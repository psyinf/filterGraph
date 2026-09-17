#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace filterGraph {

// The input of a merge stage: one slot per fan-in edge, in the order the edges
// are listed in the DSL group `(a, b, c) -> Merge`. An empty slot is a "hole"
// left by an upstream path that dropped the message via std::nullopt.
using MergeInputs = std::vector<std::any>;

// MergeFilter<OutputType> is the fan-in stage of the text DSL: it combines the
// values arriving on several edges into a single OutputType via a
// caller-supplied combiner. It is an ordinary MessageFilter<MergeInputs,
// OutputType>, so it lives in the same FilterRegistry as every other stage; the
// DSL runtime recognises a merge by its MergeInputs input type and gathers the
// group's edges into it.
//
// The combiner has the same signature as JoinFilter's, so a combiner written
// for a JSON Join can be reused as a DSL merge unchanged.
template <typename OutputType>
class MergeFilter : public MessageFilter<MergeInputs, OutputType>
{
public:
    using Combiner = std::function<std::optional<OutputType>(MergeInputs&&)>;

    explicit MergeFilter(Combiner combiner)
        : mCombiner(std::move(combiner))
    {
    }

    std::optional<OutputType> filter(MergeInputs&& inputs) override
    {
        return mCombiner(std::move(inputs));
    }

private:
    Combiner mCombiner;
};

// Registers MergeFilter<OutputType> under `name` for use as a DSL fan-in stage:
//
//     (a, b) -> Name -> merged
template <typename OutputType>
void registerMergeFilter(const std::string& name, typename MergeFilter<OutputType>::Combiner combiner)
{
    FilterRegistrar<MergeFilter<OutputType>> registrar(
        name,
        [combiner = std::move(combiner)](const nlohmann::json&) {
            return std::make_shared<MergeFilter<OutputType>>(combiner);
        });
}

} // namespace filterGraph
