#pragma once

#include <filterGraph/core/filterGraph/AnyFilterChain.hpp>
#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <any>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <vector>

namespace filterGraph {

// FanoutFilter<InputType> duplicates the incoming message to every registered
// receiver branch, then passes the original message through unchanged (i.e.
// it behaves as a MessageFilter<InputType, InputType> "tap" in a chain).
//
// Branches are stored as type-erased AnyMessageFilter so that:
//  - any registered filter/sub-chain can be used as a branch (including
//    another JsonFilterGraph or even another FanoutFilter), and
//  - the whole thing can be configured/constructed from JSON.
//
// Each branch's output is discarded; fanout is for side-effecting receivers
// (logging, forwarding, metrics, ...). InputType must be copyable, since a
// separate copy is fed to each branch.
template <typename InputType>
class FanoutFilter : public MessageFilter<InputType, InputType>
{
public:
    void addReceiver(std::shared_ptr<AnyMessageFilter> receiver)
    {
        if (receiver->inputType() != std::type_index(typeid(InputType)))
        {
            throw std::runtime_error(std::format(
                "FanoutFilter: branch expects input type '{}' but fanout carries '{}'",
                receiver->inputType().name(),
                typeid(InputType).name()));
        }
        receiver->setContext(this->sharedContext());
        mReceivers.push_back(std::move(receiver));
    }

    std::optional<InputType> filter(InputType&& data) override
    {
        for (auto& receiver : mReceivers)
        {
            std::any branchInput = data; // copy: original must survive for pass-through and other branches
            receiver->filter(std::move(branchInput));
        }
        return std::move(data);
    }

    void setContext(std::shared_ptr<GraphContext> context) override
    {
        for (auto& receiver : mReceivers)
        {
            receiver->setContext(context);
        }
        MessageFilter<InputType, InputType>::setContext(std::move(context));
    }

private:
    std::vector<std::shared_ptr<AnyMessageFilter>> mReceivers;
};

// Registers FanoutFilter<InputType> under `name` for use from JSON.
//
// Each entry in "branches" is itself a full path (an array of stages), built
// via AnyFilterChain, so a single fanout can drive multiple, independent,
// multi-stage paths in parallel:
//
// {
//   "branches": [
//     [ { "type": "StageA" } ],
//     [ { "type": "StageB" }, { "type": "StageC" } ]
//   ]
// }
template <typename InputType>
void registerFanoutFilter(const std::string& name)
{
    FilterRegistrar<FanoutFilter<InputType>> registrar(
        name,
        [](const nlohmann::json& config) {
            auto fanout = std::make_shared<FanoutFilter<InputType>>();

            if (config.contains("branches"))
            {
                for (const auto& branchPathConfig : config.at("branches"))
                {
                    fanout->addReceiver(std::make_shared<AnyFilterChain>(branchPathConfig));
                }
            }

            return fanout;
        });
}

} // namespace filterGraph
