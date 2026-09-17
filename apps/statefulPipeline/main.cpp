// Stages that carry state across messages — the features that come up as soon
// as a graph does more than transform one message at a time:
//  - a stateful stage: state in its members, flushed in finish()
//  - GraphContext: how a stage hands a final result back to the application
//  - a merge stage with per-instance state (subclass + FilterRegistrar)
//  - the shared-state semantics of registerMergeFilter's combiner
//  - a derived application context, recovered with GraphContext::as<T>()
//
// See EXAMPLE.md, "Stateful stages: finish() and the graph context".
#include <filterGraph/core/filterGraph/DslFilterGraph.hpp>
#include <filterGraph/core/filterGraph/FilterRegistry.hpp>
#include <filterGraph/core/filterGraph/GraphContext.hpp>
#include <filterGraph/core/filterGraph/MergeFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using filterGraph::DslFilterGraph;
using filterGraph::FilterRegistrar;
using filterGraph::GraphContext;
using filterGraph::MergeInputs;
using filterGraph::MessageFilter;
using filterGraph::TypedMergeFilter;
using filterGraph::registerMergeFilter;

namespace {

// --- Values shared through the context ---------------------------------

// A context value is keyed by its own type, so it is a dedicated struct rather
// than a bare std::size_t that unrelated stages would silently share.
struct Summary
{
    std::size_t lines{};
    std::size_t words{};
};

// An application context: a GraphContext with members of its own. Stages that
// need the session id ask for it via context().as<AppContext>().
class AppContext : public GraphContext
{
public:
    explicit AppContext(std::string session)
        : sessionId(std::move(session))
    {
    }

    std::string sessionId;
};

std::size_t countWords(std::string_view line)
{
    std::size_t words  = 0;
    bool        inWord = false;
    for (const char c : line)
    {
        const bool isSpace = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!isSpace && !inWord)
        {
            ++words;
        }
        inWord = !isSpace;
    }
    return words;
}

// --- Stages ------------------------------------------------------------

// A stateful stage: one instance lives as long as its graph, so its members
// accumulate across messages. It passes each line through unchanged and only
// reports when the stream ends: finish() is that point. The result goes into
// the graph context, because finish() produces no message.
class CollectFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& line) override
    {
        ++mSummary.lines;
        mSummary.words += countWords(line);
        return std::move(line);
    }

    void finish() override
    {
        std::cout << std::format("[collect] end of stream: {} lines, {} words\n", mSummary.lines, mSummary.words);
        context().set(mSummary); // the application reads it after finish()
    }

private:
    Summary mSummary;
};

// Stamps each line with the session id of the application's own context. A
// stage using as<AppContext>() depends on that type, so reusable stages should
// stick to the type-keyed set/get instead.
class StampFilter : public MessageFilter<std::string>
{
public:
    std::optional<std::string> filter(std::string&& line) override
    {
        const auto* app = context().as<AppContext>();
        return app ? std::format("[{}] {}", app->sessionId, line) : std::move(line);
    }
};

class LengthFilter : public MessageFilter<std::string, std::size_t>
{
public:
    std::optional<std::size_t> filter(std::string&& line) override
    {
        return line.size();
    }
};

class WordsFilter : public MessageFilter<std::string, std::size_t>
{
public:
    std::optional<std::size_t> filter(std::string&& line) override
    {
        return countWords(line);
    }
};

// A merge stage with per-instance state and per-instance configuration: it
// keeps a sliding window of the lengths it has seen. This is the way to a
// stateful merge — subclass (here TypedMergeFilter, so the DSL still checks
// the slot types) and register a creator that reads the stage's arguments, so
// that every instance gets its own state.
class TrendFilter : public TypedMergeFilter<std::string, std::size_t, std::size_t>
{
public:
    explicit TrendFilter(std::size_t window)
        : mWindow(window)
    {
    }

    std::optional<std::string> merge(std::optional<std::size_t>&& length, std::optional<std::size_t>&& words) override
    {
        mLengths.push_back(length.value_or(0));
        if (mLengths.size() > mWindow)
        {
            mLengths.pop_front();
        }

        const double average =
            std::accumulate(mLengths.begin(), mLengths.end(), 0.0) / static_cast<double>(mLengths.size());
        return std::format("chars={} words={} avg(last {})={:.1f}", length.value_or(0), words.value_or(0), mWindow, average);
    }

private:
    std::size_t             mWindow;
    std::deque<std::size_t> mLengths;
};

// --- Registration ------------------------------------------------------

static FilterRegistrar<CollectFilter> registerCollect("Collect");
static FilterRegistrar<StampFilter>   registerStamp("Stamp");
static FilterRegistrar<LengthFilter>  registerLength("Length");
static FilterRegistrar<WordsFilter>   registerWords("Words");

// A creator lambda gives every instance its own state, configured from the
// stage's arguments: `Trend(window=2)`.
static FilterRegistrar<TrendFilter> registerTrend("Trend", [](const nlohmann::json& config) {
    return std::make_shared<TrendFilter>(config.value("window", std::size_t{3}));
});

// By contrast, registerMergeFilter copies ONE combiner into every instance, so
// state the combiner captures is shared by all of them — across instances and
// across graphs. That is what block 3 demonstrates; a merge that needs its own
// state uses the Trend pattern above instead.
static const bool sRegisterTally = [] {
    auto calls = std::make_shared<int>(0);
    registerMergeFilter<std::string>("Tally", [calls](MergeInputs&& inputs) -> std::optional<std::string> {
        ++*calls;
        return std::format("call {} of the one shared combiner ({} slots)", *calls, inputs.size());
    });
    return true;
}();

const std::vector<std::string>& lines()
{
    static const std::vector<std::string> input{
        "the quick brown fox",
        "jumps over",
        "the lazy dog and keeps running",
    };
    return input;
}

} // namespace

int main()
{
    // 1) A stateful stage, flushed at the end of the stream. Nothing is
    //    reported while messages flow; finish() reports once and publishes the
    //    summary to the graph's context, where the application picks it up.
    {
        DslFilterGraph<std::string, std::string> graph("in -> Collect -> out");

        for (auto line : lines())
        {
            graph.filter(std::move(line));
        }

        graph.finish(); // the owner's call: a graph that is never finished never flushes

        const auto summary = graph.context().get<Summary>();
        std::cout << std::format("[app] read from the context: {} lines, {} words\n\n",
                                 summary->lines,
                                 summary->words);
    }

    // 2) Two instances of the same stateful merge stage, with different
    //    arguments. Each has its own sliding window, because the registered
    //    creator builds a fresh TrendFilter per instance.
    {
        DslFilterGraph<std::string> graph(R"dsl(
            in -> Length -> length
            in -> Words  -> words
            (length, words) -> Trend(window=2) -> out.short
            (length, words) -> Trend(window=3) -> out.long
        )dsl");

        for (auto line : lines())
        {
            auto outputs = graph.filter(std::move(line));
            std::cout << std::format("[trend] short: {}\n[trend] long:  {}\n",
                                     *outputs->get<std::string>("short"),
                                     *outputs->get<std::string>("long"));
        }
        std::cout << '\n';
    }

    // 3) The same graph shape with a merge registered from a combiner lambda.
    //    Both stage instances run the same captured counter, so the numbers
    //    keep climbing across instances — the state is shared, not per stage.
    {
        DslFilterGraph<std::string> graph(R"dsl(
            in -> Length -> length
            in -> Words  -> words
            (length, words) -> Tally -> out.first
            (length, words) -> Tally -> out.second
        )dsl");

        for (auto line : lines())
        {
            auto outputs = graph.filter(std::move(line));
            std::cout << std::format("[tally] {} / {}\n",
                                     *outputs->get<std::string>("first"),
                                     *outputs->get<std::string>("second"));
        }
        std::cout << '\n';
    }

    // 4) An application context: derive from GraphContext, hand it to the graph
    //    (not to individual stages — the graph overwrites a stage's context
    //    with its own), and recover it inside a stage with as<AppContext>().
    //    Collect's summary lands in the very same context. Stamp runs first
    //    here, so the session stamp counts as a word of every line.
    {
        DslFilterGraph<std::string, std::string> graph("in -> Stamp -> stamped -> Collect -> out");

        auto context = std::make_shared<AppContext>("session-42");
        graph.setContext(context);

        for (auto line : lines())
        {
            std::cout << "[stamped] " << *graph.filter(std::move(line)) << '\n';
        }

        graph.finish();
        std::cout << std::format("[app] session {} saw {} lines\n",
                                 context->sessionId,
                                 context->get<Summary>()->lines);
    }

    return 0;
}
