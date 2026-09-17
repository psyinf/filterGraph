#pragma once

#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/Void.hpp>

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

// AnyFilterChain builds and runs a sequence of stages (a "path") described by
// a JSON array, resolving each stage by name via FilterRegistry. It is itself
// an AnyMessageFilter, so a whole path can be used:
//  - as the top-level pipeline (see JsonFilterGraph), or
//  - as a single branch inside a FanoutFilter, allowing branches to be
//    multi-stage paths rather than a single filter.
//
// Type-compatibility between consecutive stages is validated eagerly at
// construction time (fail fast).
class AnyFilterChain : public AnyMessageFilter
{
public:
    explicit AnyFilterChain(const nlohmann::json& stagesConfig)
    {
        if (stagesConfig.empty())
        {
            throw std::runtime_error("AnyFilterChain: a path must contain at least one stage");
        }

        for (const auto& stageConfig : stagesConfig)
        {
            const std::string name   = stageConfig.at("type").get<std::string>();
            nlohmann::json    config = stageConfig.contains("config") ? stageConfig.at("config") : nlohmann::json::object();

            auto stage = FilterRegistry::instance().create(name, config);

            if (!mStages.empty() && mStages.back()->outputType() == std::type_index(typeid(Void)))
            {
                throw std::runtime_error(std::format(
                    "AnyFilterChain: stage '{}' cannot follow a terminal Void stage; a Void stage must be the last stage in a path",
                    name));
            }

            if (!mStages.empty() && stage->inputType() != mStages.back()->outputType())
            {
                throw std::runtime_error(std::format(
                    "AnyFilterChain: stage '{}' expects input type '{}' but received '{}' from the previous stage",
                    name,
                    stage->inputType().name(),
                    mStages.back()->outputType().name()));
            }

            mStages.push_back(std::move(stage));
        }
    }

    std::optional<std::any> filter(std::any&& input) override
    {
        std::any current = std::move(input);

        for (auto& stage : mStages)
        {
            auto result = stage->filter(std::move(current));
            if (!result)
            {
                // TODO(roadmap): record which stage short-circuited (index/name)
                // and expose it to the caller instead of returning a bare nullopt.
                return std::nullopt;
            }
            current = std::move(*result);
        }

        return current;
    }

    std::type_index inputType() const override
    {
        return mStages.front()->inputType();
    }

    std::type_index outputType() const override
    {
        return mStages.back()->outputType();
    }

    void setContext(std::shared_ptr<GraphContext> context) override
    {
        for (auto& stage : mStages)
        {
            stage->setContext(context);
        }
    }

private:
    std::vector<std::shared_ptr<AnyMessageFilter>> mStages;
};

} // namespace filterGraph
