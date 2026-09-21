// Tuning parameters kept out of the graph text, in files that several graphs
// share:
//  - `$name` arguments, `Truncate(width=$text.width)`, and nested values such
//    as a list of objects, which inline arguments cannot express
//  - graph files (graphs/*.fg) that name their parameter files with
//    `params "tuning.json"`, loaded with dsl::loadGraphProgram
//  - one parameter changed for a run, without editing any file
//  - renderings of a graph before and after its parameters are bound
//  - the diagnostic for a misspelled parameter
//
// See EXAMPLE.md, "Parameters shared by several graphs".
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphParameters.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using filterGraph::DslFilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::GraphOutputs;
using filterGraph::MessageFilter;
using filterGraph::validateDslGraph;

namespace dsl = filterGraph::dsl;

namespace {

// --- Stages ------------------------------------------------------------

// Drops lines shorter than `minLength`.
class MinLengthFilter : public MessageFilter<std::string>
{
public:
    explicit MinLengthFilter(std::size_t minLength)
        : mMinLength(minLength)
    {
    }

    std::optional<std::string> filter(std::string&& text) override
    {
        if (text.size() < mMinLength)
        {
            return std::nullopt;
        }
        return std::move(text);
    }

private:
    std::size_t mMinLength;
};

// Cuts lines to `width` characters, marking a cut with "...".
class TruncateFilter : public MessageFilter<std::string>
{
public:
    explicit TruncateFilter(std::size_t width)
        : mWidth(width)
    {
    }

    std::optional<std::string> filter(std::string&& text) override
    {
        if (text.size() > mWidth)
        {
            text = text.substr(0, mWidth) + "...";
        }
        return std::move(text);
    }

private:
    std::size_t mWidth;
};

// Names a line's size class: the label of the first class it fits into.
class ClassifyFilter : public MessageFilter<std::string>
{
public:
    struct Class
    {
        std::size_t upTo;
        std::string label;
    };

    explicit ClassifyFilter(std::vector<Class> classes)
        : mClasses(std::move(classes))
    {
    }

    std::optional<std::string> filter(std::string&& text) override
    {
        for (const auto& sizeClass : mClasses)
        {
            if (text.size() <= sizeClass.upTo)
            {
                return sizeClass.label;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<Class> mClasses;
};

// --- Registration --------------------------------------------------------

// A parameter arrives in the stage's config like any other argument.
static FilterRegistrar<MinLengthFilter> registerMinLength("MinLength", [](const nlohmann::json& config) {
    return std::make_shared<MinLengthFilter>(config.at("minLength").get<std::size_t>());
});

static FilterRegistrar<TruncateFilter> registerTruncate("Truncate", [](const nlohmann::json& config) {
    return std::make_shared<TruncateFilter>(config.at("width").get<std::size_t>());
});

// `classes` is a list of objects: only a parameter can give it in the DSL.
static FilterRegistrar<ClassifyFilter> registerClassify("Classify", [](const nlohmann::json& config) {
    std::vector<ClassifyFilter::Class> classes;
    for (const auto& sizeClass : config.at("classes"))
    {
        classes.push_back({sizeClass.at("upTo").get<std::size_t>(), sizeClass.at("label").get<std::string>()});
    }
    return std::make_shared<ClassifyFilter>(std::move(classes));
});

const std::vector<std::string> kLines = {
    "short one",
    "a line of medium length",
    "and a considerably longer line than the others",
};

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path graphs = argc > 1 ? argv[1] : TUNED_PIPELINE_GRAPHS;

    try
    {
        // 1) Two graphs, one parameter file. Both read `$text.width` from
        //    tuning.json, so they cut lines to the same width; report.fg also
        //    reads its size classes from report.json.
        {
            DslFilterGraph<std::string, std::string> alerts(dsl::loadGraphProgram(graphs / "alerts.fg"));
            DslFilterGraph<std::string>              report(dsl::loadGraphProgram(graphs / "report.fg"));

            for (auto line : kLines)
            {
                const auto alert = alerts.filter(std::string{line});
                auto       row   = report.filter(std::move(line));
                std::cout << std::format("[shared] {:<20} {:<7} alert: {}\n", *row->get<std::string>("text"),
                                         *row->get<std::string>("size"), alert.value_or("-"));
            }
        }

        // 2) One run with a different width, e.g. to try a value out, without
        //    editing tuning.json: overrides apply on top of the files.
        {
            DslFilterGraph<std::string, std::string> alerts(
                dsl::loadGraphProgram(graphs / "alerts.fg", {{"text", {{"width", 6}}}}));

            std::cout << "[override] " << alerts.filter(std::string{kLines.back()}).value_or("-") << '\n';
        }

        // 3) Parsing reads no files: the parameters show by name. Once bound,
        //    the renderings show their values.
        {
            std::cout << '\n'
                      << dsl::toAscii(dsl::parseGraphProgram("in -> Truncate(width=$text.width) -> out")) << '\n'
                      << dsl::toAscii(dsl::parseGraphProgram("in -> Truncate(width=$text.width) -> out",
                                                             dsl::loadParameters(graphs / "tuning.json")))
                      << '\n';
        }

        // 4) A misspelled parameter is a located diagnostic, with a suggestion.
        {
            const auto program = dsl::parseGraphProgram("in -> Truncate(width=$text.widht) -> out",
                                                        dsl::loadParameters(graphs / "tuning.json"));
            for (const auto& diagnostic : validateDslGraph<std::string, std::string>(program))
            {
                std::cout << "[check] " << dsl::formatDiagnostic(diagnostic) << '\n';
            }
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
