#include "Engine/Common/CoreMinimal.hpp"
#include "Engine/Runtime/Interface/AgentQueries.hpp"

namespace Runtime::Agent
{
    void FAgentQueryRegistry::Add(std::string name, QueryFn fn) { if (!name.empty() && fn) queries_[std::move(name)] = std::move(fn); }
    void FAgentQueryRegistry::SetValue(std::string name, double value) { if (!name.empty()) values_[std::move(name)] = value; }
    void FAgentQueryRegistry::ClearValues() { values_.clear(); }
    std::optional<FAgentQueryValue> FAgentQueryRegistry::Query(const std::string& name) const
    {
        if (const auto it = queries_.find(name); it != queries_.end()) return it->second();
        if (const auto it = values_.find(name); it != values_.end()) return FAgentQueryValue(it->second);
        return std::nullopt;
    }
    std::vector<std::string> FAgentQueryRegistry::Names() const
    {
        std::vector<std::string> result;
        for (const auto& [name, fn] : queries_) result.push_back(name);
        for (const auto& [name, value] : values_) if (!queries_.contains(name)) result.push_back(name);
        std::sort(result.begin(), result.end());
        return result;
    }
    std::string ToString(const FAgentQueryValue& value)
    {
        if (const auto* item = std::get_if<bool>(&value)) return *item ? "true" : "false";
        if (const auto* item = std::get_if<int64_t>(&value)) return std::to_string(*item);
        if (const auto* item = std::get_if<double>(&value)) return fmt::format("{}", *item);
        return std::get<std::string>(value);
    }
}
