#pragma once

#include <filterGraph/core/filterGraph/AnyMessageFilter.hpp>
#include <filterGraph/core/filterGraph/MessageFilter.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace filterGraph {

// FilterRegistry is a single, global, type-erased registry: any MessageFilter
// implementation (regardless of its InType/OutType) can register itself under
// a name. A runtime configuration (JSON) then requests filters by name only,
// and JsonFilterGraph/AnyFilterChain validate that the resulting chain's
// types line up.
class FilterRegistry
{
public:
    using Creator = std::function<std::shared_ptr<AnyMessageFilter>(const nlohmann::json& config)>;

    static FilterRegistry& instance()
    {
        static FilterRegistry registry;
        return registry;
    }

    void registerFilter(const std::string& name, Creator creator)
    {
        mCreators[name] = std::move(creator);
    }

    std::shared_ptr<AnyMessageFilter> create(const std::string& name, const nlohmann::json& config) const
    {
        auto it = mCreators.find(name);
        if (it == mCreators.end())
        {
            throw std::runtime_error("FilterRegistry: unknown filter type '" + name + "'");
        }
        return it->second(config);
    }

    bool contains(const std::string& name) const
    {
        return mCreators.find(name) != mCreators.end();
    }

    std::vector<std::string> registeredNames() const
    {
        std::vector<std::string> names;
        names.reserve(mCreators.size());
        for (const auto& [name, creator] : mCreators)
        {
            names.push_back(name);
        }
        return names;
    }

private:
    std::unordered_map<std::string, Creator> mCreators;
};

// Helper to register a filter type at static-init time. Place one instance of
// this per filter type in the .cpp/.hpp that defines it, e.g.:
//
//     static FilterRegistrar<MyFilter> registrar("MyFilter");
//
// If the filter's constructor takes a config, use the second overload with a
// custom creator lambda that reads values out of `config`.
template <typename FilterImpl>
struct FilterRegistrar
{
    explicit FilterRegistrar(const std::string& name)
    {
        FilterRegistry::instance().registerFilter(name, [](const nlohmann::json&) {
            return std::make_shared<AnyMessageFilterAdapter<FilterImpl>>(std::make_shared<FilterImpl>());
        });
    }

    FilterRegistrar(const std::string& name, std::function<std::shared_ptr<FilterImpl>(const nlohmann::json&)> creator)
    {
        FilterRegistry::instance().registerFilter(name, [creator = std::move(creator)](const nlohmann::json& config) {
            return std::make_shared<AnyMessageFilterAdapter<FilterImpl>>(creator(config));
        });
    }
};

} // namespace filterGraph
