#pragma once

#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <any>
#include <memory>
#include <optional>
#include <typeindex>
#include <utility>

namespace filterGraph {

// Type-erased view of a MessageFilter<In, Out>, so that stages with different
// (and otherwise incompatible) In/Out types can be stored/chained together at
// runtime, e.g. when the pipeline shape is described entirely by JSON.
//
// inputType()/outputType() allow JsonFilterGraph to validate at construction
// time that consecutive stages actually agree on their data type, even though
// the chain itself is only known at runtime.
class AnyMessageFilter
{
public:
    virtual ~AnyMessageFilter() = default;

    virtual std::optional<std::any> filter(std::any&& input) = 0;

    virtual std::type_index inputType() const  = 0;
    virtual std::type_index outputType() const = 0;
};

// Adapts a concrete MessageFilter<Filter::InType, Filter::OutType> to the
// type-erased AnyMessageFilter interface.
template <typename Filter>
class AnyMessageFilterAdapter : public AnyMessageFilter
{
public:
    explicit AnyMessageFilterAdapter(std::shared_ptr<Filter> filter)
        : mFilter(std::move(filter))
    {
    }

    std::optional<std::any> filter(std::any&& input) override
    {
        auto& typedInput = std::any_cast<typename Filter::InType&>(input);
        auto  result     = mFilter->filter(std::move(typedInput));
        if (!result)
        {
            return std::nullopt;
        }
        return std::any(std::move(*result));
    }

    std::type_index inputType() const override
    {
        return typeid(typename Filter::InType);
    }

    std::type_index outputType() const override
    {
        return typeid(typename Filter::OutType);
    }

private:
    std::shared_ptr<Filter> mFilter;
};

} // namespace filterGraph
