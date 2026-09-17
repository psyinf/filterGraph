#pragma once

#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <vector>

namespace filterGraph {

// JoinFilter<InputType, OutputType> is the mirror image of FanoutFilter: it
// scatters ONE incoming message (a copy) through N independent, multi-stage
// paths, gathers their outputs, and combines them into a single OutputType via
// a caller-supplied combiner (an N->1 gather, versus fanout's 1->N split).
//
// Paths may produce different output types, so their gathered results are kept
// type-erased (std::vector<std::any>, one slot per path, in registration
// order). A path that short-circuits with std::nullopt leaves an EMPTY slot (a
// "hole"): the combiner receives whatever arrived and decides how to handle
// missing paths. A Void-terminated path produces no value to gather and is
// therefore rejected at construction.
//
// InputType must be copyable, since a separate copy is fed to each path.
template <typename InputType, typename OutputType>
class JoinFilter : public MessageFilter<InputType, OutputType>
{
public:
    // Receives one slot per path (in registration order); empty slots are
    // holes left by paths that dropped their message via std::nullopt.
    using Combiner = std::function<std::optional<OutputType>(std::vector<std::any>&&)>;

    explicit JoinFilter(Combiner combiner)
        : mCombiner(std::move(combiner))
    {
    }

    void addPath(std::shared_ptr<AnyMessageFilter> path)
    {
        if (path->inputType() != std::type_index(typeid(InputType)))
        {
            throw std::runtime_error(std::format(
                "JoinFilter: path expects input type '{}' but join scatters '{}'",
                path->inputType().name(),
                typeid(InputType).name()));
        }
        if (path->outputType() == std::type_index(typeid(Void)))
        {
            throw std::runtime_error(
                "JoinFilter: a Void-terminated path produces no value to join; join paths must produce a value");
        }
        path->setContext(this->sharedContext());
        mPaths.push_back(std::move(path));
    }

    void setContext(std::shared_ptr<GraphContext> context) override
    {
        for (auto& path : mPaths)
        {
            path->setContext(context);
        }
        MessageFilter<InputType, OutputType>::setContext(std::move(context));
    }

    std::optional<OutputType> filter(InputType&& data) override
    {
        std::vector<std::any> gathered;
        gathered.reserve(mPaths.size());

        for (auto& path : mPaths)
        {
            std::any pathInput = data; // copy: original must survive for the other paths
            auto     result    = path->filter(std::move(pathInput));
            gathered.push_back(result ? std::move(*result) : std::any{}); // empty slot == hole
        }

        return mCombiner(std::move(gathered));
    }

private:
    Combiner                                       mCombiner;
    std::vector<std::shared_ptr<AnyMessageFilter>> mPaths;
};

// Registers JoinFilter<InputType, OutputType> under `name` for use from JSON.
//
// The combiner is supplied in C++ (it cannot be expressed in JSON), while the
// paths themselves come from configuration. Each entry in "paths" is a full
// path (an array of stages), built via AnyFilterChain:
//
// {
//   "paths": [
//     [ { "type": "StageA" } ],
//     [ { "type": "StageB" }, { "type": "StageC" } ]
//   ]
// }
template <typename InputType, typename OutputType>
void registerJoinFilter(const std::string& name, typename JoinFilter<InputType, OutputType>::Combiner combiner)
{
    FilterRegistrar<JoinFilter<InputType, OutputType>> registrar(
        name,
        [combiner = std::move(combiner)](const nlohmann::json& config) {
            auto join = std::make_shared<JoinFilter<InputType, OutputType>>(combiner);

            if (config.contains("paths"))
            {
                for (const auto& pathConfig : config.at("paths"))
                {
                    join->addPath(std::make_shared<AnyFilterChain>(pathConfig));
                }
            }

            return join;
        });
}

} // namespace filterGraph
