#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace octopus::tool;

TEST_CASE("real builtin returns a greeting and an in-range number") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    CHECK(executor.execute({"bad", "misc.hello_world", {{"extra", true}}}).status ==
          Status::InvalidArguments);
    CHECK(executor.execute({"bad", "misc.hello_world", 42}).status == Status::InvalidArguments);
    auto result = executor.execute({"pilot-1", "misc.hello_world", nlohmann::json::object()});
    REQUIRE(result.status == Status::Success);
    CHECK(result.call_id == "pilot-1");
    const std::string prefix = "Hello, world! Random number ";
    REQUIRE(result.output.get<std::string>().rfind(prefix, 0) == 0);
    const auto digits = result.output.get<std::string>().substr(prefix.size());
    REQUIRE_FALSE(digits.empty());
    REQUIRE(digits.find_first_not_of("0123456789") == std::string::npos);
    CHECK(std::stoi(digits) >= 1000);
    CHECK(std::stoi(digits) <= 99999);
}
