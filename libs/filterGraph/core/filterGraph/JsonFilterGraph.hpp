#pragma once

#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <format>
#include <stdexcept>
#include <typeindex>

namespace filterGraph {

// JsonFilterGraph<InputType, OutputType> builds an entire filter chain from a
// JSON configuration (delegating the stage-by-stage construction/validation
// to AnyFilterChain), and exposes it as a typed MessageFilter<InputType,
// OutputType> so it plugs directly into any code expecting a MessageFilter.
//
// Expected JSON shape:
// [
//   { "type": "StageA" },
//   { "type": "StageB", "config": { ... } }
// ]
//
// Stages may internally fan out into multiple parallel, multi-stage paths
// (see FanoutFilter), so the overall shape need not be a single linear chain.
template <typename InputType, typename OutputType>
class JsonFilterGraph : public MessageFilter<InputType, OutputType>
{
public:
    explicit JsonFilterGraph(const nlohmann::json& stagesConfig)
        : mChain(stagesConfig)
    {
        if (mChain.inputType() != std::type_index(typeid(InputType)))
        {
            throw std::runtime_error(std::format(
                "JsonFilterGraph: first stage expects input type '{}' but graph input type is '{}'",
                mChain.inputType().name(),
                typeid(InputType).name()));
        }

        if (mChain.outputType() != std::type_index(typeid(OutputType)))
        {
            throw std::runtime_error(std::format(
                "JsonFilterGraph: final stage produces type '{}' but graph output type is '{}'",
                mChain.outputType().name(),
                typeid(OutputType).name()));
        }

        JsonFilterGraph::setContext(this->sharedContext());
    }

    std::optional<OutputType> filter(InputType&& data) override
    {
        auto result = mChain.filter(std::any(std::move(data)));
        if (!result)
        {
            return std::nullopt;
        }
        return std::any_cast<OutputType>(std::move(*result));
    }

    void setContext(std::shared_ptr<GraphContext> context) override
    {
        mChain.setContext(context);
        MessageFilter<InputType, OutputType>::setContext(std::move(context));
    }

    void finish() override
    {
        mChain.finish();
    }

private:
    AnyFilterChain mChain;
};

} // namespace filterGraph
