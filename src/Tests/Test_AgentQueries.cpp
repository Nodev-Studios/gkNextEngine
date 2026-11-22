#include <catch2/catch_all.hpp>

#include "Engine/Runtime/Interface/AgentQueries.hpp"

using Runtime::Agent::FAgentQueryRegistry;
using Runtime::Agent::FAgentQueryValue;

TEST_CASE("Agent query values set from scripts are queryable until cleared", "[Unit][AgentQueries]")
{
    FAgentQueryRegistry registry;
    CHECK_FALSE(registry.Query("props").has_value());

    registry.SetValue("props", 1256.0);
    registry.SetValue("props", 1257.0);
    const auto value = registry.Query("props");
    REQUIRE(value.has_value());
    REQUIRE(std::holds_alternative<double>(*value));
    CHECK(std::get<double>(*value) == 1257.0);
    CHECK(Runtime::Agent::ToString(*value) == "1257");
    CHECK(registry.Names() == std::vector<std::string>{"props"});

    registry.ClearValues();
    CHECK_FALSE(registry.Query("props").has_value());
    CHECK(registry.Names().empty());
}

TEST_CASE("Agent query functions take precedence over script values", "[Unit][AgentQueries]")
{
    FAgentQueryRegistry registry;
    registry.Add("state", [] { return FAgentQueryValue(std::string("Playing")); });
    registry.SetValue("state", 3.0);

    const auto value = registry.Query("state");
    REQUIRE(value.has_value());
    CHECK(Runtime::Agent::ToString(*value) == "Playing");
    CHECK(registry.Names() == std::vector<std::string>{"state"});

    // Clearing script values never removes what the host registered.
    registry.ClearValues();
    CHECK(registry.Query("state").has_value());
}
