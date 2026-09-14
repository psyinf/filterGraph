#pragma once

#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace filterGraph {

// FilterGraph chains together any number of MessageFilter stages, where the
// OutType of each stage must match the InType of the following stage.
//
// The graph itself is a MessageFilter<FirstStage::InType, LastStage::OutType>,
// so it can be used anywhere a single MessageFilter is expected.
//
// It is entirely configured from the outside: callers supply the concrete
// filter instances (in order) at construction time, e.g. built dynamically
// from a runtime configuration. FilterGraph itself has no knowledge of which
// concrete filters are used.
template <typename... Filters>
class FilterGraph;

// Single-stage specialization: terminates the recursion.
template <typename Filter>
class FilterGraph<Filter> : public MessageFilter<typename Filter::InType, typename Filter::OutType>
{
public:
    using InType  = typename Filter::InType;
    using OutType = typename Filter::OutType;

    explicit FilterGraph(std::shared_ptr<Filter> filter)
        : mFilter(std::move(filter))
    {
    }

    std::optional<OutType> filter(InType&& data) override
    {
        return mFilter->filter(std::move(data));
    }

private:
    std::shared_ptr<Filter> mFilter;
};

// Multi-stage specialization: recursively composes the first stage with the
// remainder of the graph.
template <typename Filter, typename... Rest>
class FilterGraph<Filter, Rest...> : public MessageFilter<typename Filter::InType, typename FilterGraph<Rest...>::OutType>
{
public:
    using InType  = typename Filter::InType;
    using OutType = typename FilterGraph<Rest...>::OutType;

    FilterGraph(std::shared_ptr<Filter> filter, std::shared_ptr<Rest>... rest)
        : mFilter(std::move(filter))
        , mNext(std::move(rest)...)
    {
    }

    std::optional<OutType> filter(InType&& data) override
    {
        auto intermediate = mFilter->filter(std::move(data));
        if (!intermediate)
        {
            return std::nullopt;
        }
        return mNext.filter(std::move(*intermediate));
    }

private:
    std::shared_ptr<Filter> mFilter;
    FilterGraph<Rest...>    mNext;
};

} // namespace filterGraph
