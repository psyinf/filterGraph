#pragma once

#include <filterGraph/core/filterGraph/GraphLang.hpp>
#include <filterGraph/core/filterGraph/GraphValidator.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Parameters: tuning values kept out of the graph text, so that several graphs
// can share them. A stage argument names a parameter instead of giving a value,
// and a parameter file (a JSON object) gives the values:
//
//   # alerts.fg
//   params "tuning.json"
//   in -> MinLength(minLength=$text.minLength) -> long
//   long -> Truncate(width=$text.width) -> out
//
//   // tuning.json
//   { "text": { "minLength": 5, "width": 12 } }
//
// - `$name` names a top-level member; dots walk into nested objects
//   (`$text.width`). A parameter's value may be any JSON value, objects and
//   arrays included. Parameter files may contain comments.
// - A graph may name several parameter files; later ones override earlier ones
//   member by member (JSON merge patch: nested objects merge, other values
//   replace, null removes). Paths are relative to the graph file.
// - A name that no parameter file defines is a diagnostic; parameters that the
//   graph does not use are not, since a file is meant to be shared.
//
// parseGraphProgram(text) only records the references and files; it reads
// nothing. loadGraphProgram(path) reads a graph file with its parameter files,
// and parseGraphProgram(text, parameters) / bindParameters take the values
// from the caller. A graph built from a program whose parameters were never
// bound reports each of them as not set.
namespace filterGraph::dsl {

namespace detail {

// The parameter `name` ("a" or "a.b.c") in `parameters`, or nullptr.
inline const nlohmann::json* findParameter(const nlohmann::json& parameters, std::string_view name)
{
    const nlohmann::json* value = &parameters;
    while (true)
    {
        const std::size_t      dot     = name.find('.');
        const std::string_view segment = name.substr(0, dot);
        if (!value->is_object())
        {
            return nullptr;
        }
        auto it = value->find(std::string(segment));
        if (it == value->end())
        {
            return nullptr;
        }
        value = &*it;
        if (dot == std::string_view::npos)
        {
            return value;
        }
        name.remove_prefix(dot + 1);
    }
}

// Every name a reference could use: each member, nested ones joined by dots.
inline void parameterNames(const nlohmann::json& parameters, const std::string& prefix, std::vector<std::string>& names)
{
    if (!parameters.is_object())
    {
        return;
    }
    for (const auto& item : parameters.items())
    {
        const std::string name = prefix.empty() ? item.key() : prefix + '.' + item.key();
        names.push_back(name);
        parameterNames(item.value(), name, names);
    }
}

// Reads a parameter file: a JSON object. On failure returns std::nullopt and
// sets `error`.
inline std::optional<nlohmann::json> readParameterFile(const std::filesystem::path& file, std::string& error)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        error = std::format("cannot open parameter file '{}'", file.string());
        return std::nullopt;
    }
    nlohmann::json parameters = nlohmann::json::parse(stream, nullptr, false, true);
    if (parameters.is_discarded())
    {
        // Parse again for the message; it names the line and column in the file.
        stream.clear();
        stream.seekg(0);
        try
        {
            [[maybe_unused]] const auto ignored = nlohmann::json::parse(stream, nullptr, true, true);
        }
        catch (const nlohmann::json::parse_error& parseError)
        {
            error = std::format("parameter file '{}' is not valid JSON: {}", file.string(), parseError.what());
            return std::nullopt;
        }
        error = std::format("parameter file '{}' is not valid JSON", file.string());
        return std::nullopt;
    }
    if (!parameters.is_object())
    {
        error = std::format("parameter file '{}' must hold a JSON object, not {}", file.string(), parameters.type_name());
        return std::nullopt;
    }
    return parameters;
}

// bindParameters; `reportUnknown` is false when a parameter file could not be
// read, which is reported already and would explain every unknown name.
inline void bindParameters(GraphProgram& program, const nlohmann::json& parameters, bool reportUnknown)
{
    if (!parameters.is_object())
    {
        throw std::invalid_argument(std::format("parameters must be a JSON object, not {}", parameters.type_name()));
    }

    std::vector<std::string> known;
    parameterNames(parameters, "", known);

    for (auto& stage : program.stages)
    {
        for (auto& parameter : stage.parameters)
        {
            if (const nlohmann::json* value = findParameter(parameters, parameter.name))
            {
                stage.config[parameter.argument] = *value;
                parameter.bound                  = true;
                continue;
            }
            if (!reportUnknown)
            {
                continue;
            }
            std::string message = std::format("unknown parameter '${}'", parameter.name);
            if (auto suggestion = filterGraph::detail::closestName(parameter.name, known))
            {
                message += std::format(" — did you mean '${}'?", *suggestion);
            }
            program.diagnostics.push_back({parameter.loc, std::move(message)});
        }
    }
    program.parametersBound = true;
    sortDiagnostics(program.diagnostics);
}

} // namespace detail

// Binds every `$name` argument of `program` to its value in `parameters` (a
// JSON object; dots in a name walk into nested objects), which is copied into
// the stage's config. A name `parameters` does not define is a diagnostic,
// with a suggestion if one is close. Call it once per program. Throws
// std::invalid_argument if `parameters` is not a JSON object.
inline void bindParameters(GraphProgram& program, const nlohmann::json& parameters)
{
    detail::bindParameters(program, parameters, true);
}

// Parses `text` and binds its parameters from `parameters`. A `params` line in
// the text is not read; use loadGraphProgram for a graph file.
inline GraphProgram parseGraphProgram(std::string_view text, const nlohmann::json& parameters)
{
    GraphProgram program = parseGraphProgram(text);
    bindParameters(program, parameters);
    return program;
}

// Reads a parameter file, a JSON object, e.g. to bind a graph given as text.
// Throws std::runtime_error if it cannot be read or is not a JSON object.
inline nlohmann::json loadParameters(const std::filesystem::path& file)
{
    std::string error;
    auto        parameters = detail::readParameterFile(file, error);
    if (!parameters)
    {
        throw std::runtime_error(error);
    }
    return std::move(*parameters);
}

// Loads a graph file (e.g. `alerts.fg`): parses it, reads the parameter files
// it names with `params "file.json"` (relative to the graph file's directory),
// in order, each overriding the ones before it, then applies `overrides` the
// same way, and binds the result. A parameter file that cannot be read, or is
// not a JSON object, is a diagnostic at its `params` line. Throws
// std::runtime_error if the graph file itself cannot be read.
inline GraphProgram loadGraphProgram(const std::filesystem::path& file,
                                     const nlohmann::json&        overrides = nlohmann::json::object())
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error(std::format("cannot open graph file '{}'", file.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};

    GraphProgram   program    = parseGraphProgram(text);
    nlohmann::json parameters = nlohmann::json::object();
    bool           allRead    = true;
    for (const auto& parameterFile : program.parameterFiles)
    {
        std::string error;
        if (auto values = detail::readParameterFile(file.parent_path() / parameterFile.path, error))
        {
            parameters.merge_patch(*values);
        }
        else
        {
            program.diagnostics.push_back({parameterFile.loc, std::move(error)});
            allRead = false;
        }
    }
    parameters.merge_patch(overrides);
    detail::bindParameters(program, parameters, allRead);
    return program;
}

} // namespace filterGraph::dsl
