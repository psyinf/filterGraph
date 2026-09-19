// Names in a graph's wiring — for graphs with more than one of something that
// positions alone cannot tell apart:
//  - named merge slots: `(after: x, before: y) -> Compare`, matched by name
//  - named slots on a merge registered from a lambda
//  - several named graph inputs, `in.<key>`, fed by push() or filter()
//  - the build-time and run-time checks that the names make possible
//  - toMermaid, which labels a merge's edges with its slot names
//  - a merge that reads a named input directly, `(greeting: in.greeting, ...)`
//
// See EXAMPLE.md, "Names in the wiring: merge slots and graph inputs".
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using filterGraph::DslFilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::GraphInputs;
using filterGraph::MessageFilter;
using filterGraph::TypedMergeFilter;
using filterGraph::registerTypedMergeFilter;
using filterGraph::validateDslGraph;

namespace dsl = filterGraph::dsl;

namespace {

// --- Stages ------------------------------------------------------------

class UppercaseFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return std::move(text);
    }
};

class ReverseFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::reverse(text);
        return std::move(text);
    }
};

// "aDA" -> "Ada": upper-case first letter, the rest lower-case.
class CapitalizeFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& text) override
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!text.empty())
        {
            text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
        }
        return std::move(text);
    }
};

// A merge with two slots of the same type, where the order matters. The slot
// types cannot tell them apart, so it names them: a group can then match them
// by name, `(after: x, before: y) -> Compare`, in any order.
class CompareFilter : public TypedMergeFilter<std::string, std::string, std::string>
{
public:
    std::optional<std::string> merge(std::optional<std::string>&& before,
                                     std::optional<std::string>&& after) override
    {
        return std::format("{} => {}", before.value_or("-"), after.value_or("-"));
    }

    std::vector<std::string> mergeInputNames() const override
    {
        return {"before", "after"};
    }
};

// --- Registration --------------------------------------------------------

static FilterRegistrar<UppercaseFilter>  registerUppercase("Uppercase");
static FilterRegistrar<ReverseFilter>    registerReverse("Reverse");
static FilterRegistrar<CapitalizeFilter> registerCapitalize("Capitalize");
static FilterRegistrar<CompareFilter>    registerCompare("Compare");

// The same kind of merge from a lambda: registerTypedMergeFilter takes the
// slot names, one per slot type, before the function. A hole (an input not
// fed in this run) falls back to a default.
static const bool sRegisterGreet = [] {
    registerTypedMergeFilter<std::string, std::string, std::string>(
        "Greet",
        {"greeting", "name"},
        [](std::optional<std::string>&& greeting, std::optional<std::string>&& name) -> std::optional<std::string> {
            return std::format("{}, {}!", greeting.value_or("Hello"), name.value_or("stranger"));
        });
    return true;
}();

void printDiagnostics(const char* tag, const std::vector<dsl::TextDiagnostic>& diagnostics)
{
    for (const auto& diagnostic : diagnostics)
    {
        std::cout << std::format("[{}] {}\n", tag, dsl::formatDiagnostic(diagnostic));
    }
}

} // namespace

int main()
{
    // 1) Named slots: Compare's two slots have the same type, so only their
    //    names say which is which. The group lists them in the other order;
    //    the merge still receives `before` first.
    {
        DslFilterGraph<std::string, std::string> compare(R"dsl(
            in -> Uppercase -> upper
            upper -> Reverse -> reversed
            (after: reversed, before: upper) -> Compare -> out
        )dsl");

        std::cout << "[slots] " << *compare.filter(std::string{"Hello, filterGraph!"}) << '\n';
    }

    // 2) What the names catch: a misspelled slot name, and with it a slot the
    //    group no longer feeds. Positional wiring cannot report either.
    {
        printDiagnostics("slot check",
                         validateDslGraph<std::string, std::string>(
                             "in -> Uppercase -> upper\n"
                             "upper -> Reverse -> reversed\n"
                             "(after: reversed, befor: upper) -> Compare -> out\n"));
    }

    // 3) Several named inputs, with declared types. push() feeds one input
    //    and runs only the stages it reaches; the other input is empty.
    //    filter() takes a GraphInputs and feeds several inputs in one run.
    {
        DslFilterGraph<GraphInputs> routes(R"dsl(
            in.greeting -> Uppercase -> out.shouted
            in.name -> Reverse -> out.reversed
        )dsl",
                                           GraphInputs::of<std::string, std::string>("greeting", "name"));

        auto pushed = routes.push("name", std::string{"filterGraph"});
        std::cout << std::format("[push]   shouted={} reversed={}\n",
                                 pushed->has("shouted") ? "present" : "not fed",
                                 *pushed->get<std::string>("reversed"));

        auto both = routes.filter(GraphInputs{}.set("greeting", std::string{"hi"}).set("name", std::string{"filterGraph"}));
        std::cout << std::format("[filter] shouted={} reversed={}\n",
                                 *both->get<std::string>("shouted"),
                                 *both->get<std::string>("reversed"));

        // A key the graph does not have, or a value of the wrong type, is
        // rejected when it is fed.
        try
        {
            routes.push("nmae", std::string{"filterGraph"});
        }
        catch (const std::out_of_range& error)
        {
            std::cout << "[feed check] " << error.what() << '\n';
        }
        try
        {
            routes.push("name", 42);
        }
        catch (const std::invalid_argument& error)
        {
            std::cout << "[feed check] " << error.what() << '\n';
        }
    }

    // 4) The declared inputs are checked against the graph when it is built:
    //    a misspelled `in.<key>` is an undeclared input, and the declared one
    //    it was meant to be is never read.
    {
        printDiagnostics("input check",
                         validateDslGraph<GraphInputs>("in.greeting -> Uppercase -> out.shouted\n"
                                                       "in.nmae -> Reverse -> out.reversed\n",
                                                       GraphInputs::of<std::string, std::string>("greeting", "name")));
    }

    // 5) Both together: two named inputs meet in a merge with named slots. An
    //    input that is not fed leaves a hole in its slot, and Greet fills it
    //    with a default.
    {
        DslFilterGraph<GraphInputs, std::string> greet(R"dsl(
            in.greeting -> Uppercase -> shouted
            in.name -> Capitalize -> proper
            (name: proper, greeting: shouted) -> Greet -> out
        )dsl",
                                                       GraphInputs::of<std::string, std::string>("greeting", "name"));

        std::cout << "[greet] " << *greet.push("name", std::string{"aDA"}) << '\n';
        std::cout << "[greet] " << *greet.push("greeting", std::string{"good morning"}) << '\n';
        std::cout << "[greet] "
                  << *greet.filter(GraphInputs{}.set("greeting", std::string{"hi"}).set("name", std::string{"grace"}))
                  << '\n';

        // 6) toMermaid labels the merge's edges with the slot names.
        std::cout << '\n' << dsl::toMermaid(greet.program());
    }

    // 7) A group can read a named input directly: the greeting needs no
    //    preparing, so no stage has to copy it onto an edge of its own. The
    //    undeclared input takes the type of the slot it feeds.
    {
        DslFilterGraph<GraphInputs, std::string> greet(R"dsl(
            in.name -> Capitalize -> proper
            (greeting: in.greeting, name: proper) -> Greet -> out
        )dsl");

        std::cout << "\n[direct] " << *greet.push("greeting", std::string{"Welcome"}) << '\n';
        std::cout << "[direct] "
                  << *greet.filter(GraphInputs{}.set("greeting", std::string{"Welcome"}).set("name", std::string{"lINUS"}))
                  << "\n\n"
                  << dsl::toAscii(greet.program());
    }

    return 0;
}
