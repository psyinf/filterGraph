#pragma once

#include <filterGraph/core/filterGraph/FilterRegistry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace filterGraph {

// A single problem found while validating a JSON graph config, located by a
// JSON pointer (e.g. "/0/config/paths/1/0") so a caller/editor can point at the
// exact offending node instead of guessing from a first-failure exception.
struct Diagnostic
{
    std::string pointer;
    std::string message;
};

namespace detail {

// Config keys whose value is a list of sub-paths (each sub-path being a chain).
// This is the one place that knows how composite stages nest; keep it in sync
// with FanoutFilter ("branches") and JoinFilter ("paths").
inline const std::vector<std::string>& compositePathKeys()
{
    static const std::vector<std::string> keys = {"branches", "paths"};
    return keys;
}

inline std::size_t levenshtein(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> curr(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
    {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        curr[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[b.size()];
}

inline std::optional<std::string> closestName(const std::string& name, const std::vector<std::string>& known)
{
    std::optional<std::string> best;
    std::size_t                bestDistance = 0;
    for (const auto& candidate : known)
    {
        const std::size_t distance = levenshtein(name, candidate);
        if (!best || distance < bestDistance)
        {
            best         = candidate;
            bestDistance = distance;
        }
    }
    // Only suggest when the typo is "close" to avoid nonsense suggestions.
    if (best && bestDistance <= 3 && bestDistance < name.size())
    {
        return best;
    }
    return std::nullopt;
}

inline std::string joinSorted(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    std::string joined;
    for (const auto& name : names)
    {
        if (!joined.empty())
        {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

void validateChain(const nlohmann::json& chain, const std::string& pointer, std::vector<Diagnostic>& out);

// Validates a single stage object. Returns the stage's {inputType, outputType}
// when they can be determined (leaf stages that construct cleanly), or
// std::nullopt otherwise (errors, or composite stages whose through-type is not
// knowable from JSON alone) so the caller stops type-chaining across it.
inline std::optional<std::pair<std::type_index, std::type_index>> validateStage(
    const nlohmann::json& stage,
    const std::string&    pointer,
    std::vector<Diagnostic>& out)
{
    if (!stage.is_object())
    {
        out.push_back({pointer, "expected a stage object with a \"type\" field"});
        return std::nullopt;
    }
    if (!stage.contains("type") || !stage.at("type").is_string())
    {
        out.push_back({pointer, "stage is missing a string \"type\" field"});
        return std::nullopt;
    }

    const std::string    name   = stage.at("type").get<std::string>();
    const nlohmann::json config = stage.contains("config") ? stage.at("config") : nlohmann::json::object();

    const bool isComposite = config.is_object()
        && std::any_of(detail::compositePathKeys().begin(), detail::compositePathKeys().end(),
                       [&](const std::string& key) { return config.contains(key); });

    auto& registry = FilterRegistry::instance();
    if (!registry.contains(name))
    {
        std::string message = std::format("unknown filter type '{}'", name);
        if (auto suggestion = closestName(name, registry.registeredNames()))
        {
            message += std::format(" \u2014 did you mean '{}'?", *suggestion);
        }
        message += std::format(" (known types: {})", joinSorted(registry.registeredNames()));
        out.push_back({pointer, message});
    }

    // Recurse into any sub-paths so nested mistakes are reported with their
    // own location, regardless of whether this stage's own type resolved.
    if (config.is_object())
    {
        for (const auto& key : detail::compositePathKeys())
        {
            if (!config.contains(key))
            {
                continue;
            }
            const std::string subPointer = std::format("{}/config/{}", pointer, key);
            const auto&        subPaths   = config.at(key);
            if (!subPaths.is_array())
            {
                out.push_back({subPointer, std::format("\"{}\" must be an array of paths", key)});
                continue;
            }
            for (std::size_t j = 0; j < subPaths.size(); ++j)
            {
                validateChain(subPaths[j], std::format("{}/{}", subPointer, j), out);
            }
        }
    }

    // Only leaf stages are instantiated (to learn their types and surface bad
    // or missing config). Composite stages are left to their own recursion.
    if (registry.contains(name) && !isComposite)
    {
        try
        {
            auto stageFilter = registry.create(name, config);
            return std::make_pair(stageFilter->inputType(), stageFilter->outputType());
        }
        catch (const std::exception& error)
        {
            out.push_back({pointer, std::format("could not construct '{}': {}", name, error.what())});
        }
    }

    return std::nullopt;
}

inline void validateChain(const nlohmann::json& chain, const std::string& pointer, std::vector<Diagnostic>& out)
{
    if (!chain.is_array())
    {
        out.push_back({pointer.empty() ? "" : pointer, "expected an array of stages"});
        return;
    }
    if (chain.empty())
    {
        out.push_back({pointer.empty() ? "" : pointer, "a path must contain at least one stage"});
        return;
    }

    std::optional<std::type_index> previousOutput;
    std::string                    previousName;

    for (std::size_t i = 0; i < chain.size(); ++i)
    {
        const std::string     stagePointer = std::format("{}/{}", pointer, i);
        const nlohmann::json& stage        = chain[i];
        const std::string     name = (stage.is_object() && stage.contains("type") && stage.at("type").is_string())
                                          ? stage.at("type").get<std::string>()
                                          : std::string{"<stage>"};

        auto types = validateStage(stage, stagePointer, out);

        if (types && previousOutput && *previousOutput != types->first)
        {
            out.push_back({stagePointer,
                           std::format("stage '{}' expects input type '{}' but previous stage '{}' produces '{}'",
                                       name,
                                       types->first.name(),
                                       previousName,
                                       previousOutput->name())});
        }

        if (types)
        {
            previousOutput = types->second;
            previousName   = name;
        }
        else
        {
            // Type chain is unknown past this stage (error or composite); stop
            // comparing until a stage with a known type appears again.
            previousOutput.reset();
            previousName.clear();
        }
    }
}

} // namespace detail

// Validates a JSON graph config (the same array-of-stages shape consumed by
// JsonFilterGraph/AnyFilterChain) WITHOUT running any messages, collecting all
// problems it finds rather than throwing on the first. Each Diagnostic carries
// a JSON pointer to the offending node.
//
// Covers: unknown/typo'd stage types (with a suggestion + the list of known
// types), structural mistakes (non-object stage, missing "type", a composite's
// sub-paths not being an array), adjacent type mismatches between leaf stages,
// and bad/missing per-stage config (leaf stages are constructed to check).
//
// Note: a graph's own declared input/output types live in C++ template
// parameters, not JSON, so they are not checked here; and type-chaining pauses
// across a composite stage (Fanout/Join), whose through-type is combiner- or
// branch-defined and not knowable from JSON.
inline std::vector<Diagnostic> validateGraph(const nlohmann::json& graph)
{
    std::vector<Diagnostic> diagnostics;
    detail::validateChain(graph, "", diagnostics);
    return diagnostics;
}

} // namespace filterGraph
