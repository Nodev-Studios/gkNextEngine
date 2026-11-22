#pragma once

#include "Engine/Common/CoreMinimal.hpp"

#include <optional>
#include <variant>

namespace Runtime::Agent
{
    using FAgentQueryValue = std::variant<bool, int64_t, double, std::string>;
    class FAgentQueryRegistry final
    {
    public:
        using QueryFn = std::function<FAgentQueryValue()>;
        void Add(std::string name, QueryFn fn);
        // A value pushed by a script (C# Agent.SetQuery). It backs the name only when no function
        // was registered for it, so a game cannot shadow a host query.
        void SetValue(std::string name, double value);
        void ClearValues();
        std::optional<FAgentQueryValue> Query(const std::string& name) const;
        std::vector<std::string> Names() const;
    private:
        std::unordered_map<std::string, QueryFn> queries_;
        std::unordered_map<std::string, double> values_;
    };
    std::string ToString(const FAgentQueryValue& value);
}
