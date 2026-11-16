#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <csignal>

using namespace octopus::tool;

TEST_CASE("os.exec preserves literal argv separate streams and nonzero exit") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    auto result =
        Executor(registry, policy)
            .execute({"process",
                      "os.exec",
                      {{"program", PROCESS_FIXTURE}, {"args", {"argv", "a b; $HOME", ""}}}});
    REQUIRE(result.status == Status::Success);
    CHECK(result.output.at("stdout") == "[a b; $HOME][]");
    CHECK(result.output.at("stderr") == "diagnostic");
    CHECK(result.output.at("exit_code") == 2);
    CHECK(result.output.at("signal").is_null());
}

TEST_CASE("os.exec observes EOF signals launch errors and rejects NUL") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto invoke = [&](const std::string &mode) {
        return executor.execute(
            {"id", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {mode}}}});
    };
    CHECK(invoke("stdin").output.at("exit_code") == 0);
    auto signalled = invoke("signal");
    CHECK(signalled.status == Status::Success);
    CHECK(signalled.output.at("exit_code").is_null());
    CHECK(signalled.output.at("signal") == SIGTERM);
    CHECK(executor
              .execute(
                  {"id",
                   "os.exec",
                   {{"program", "/nonexistent/octopus-test"}, {"args", nlohmann::json::array()}}})
              .status == Status::Failure);
    CHECK(executor
              .execute({"id",
                        "os.exec",
                        {{"program", std::string("x\0y", 3)}, {"args", nlohmann::json::array()}}})
              .status == Status::InvalidArguments);
    CHECK(executor
              .execute({"id",
                        "os.exec",
                        {{"program", PROCESS_FIXTURE}, {"args", {std::string("x\0y", 3)}}}})
              .status == Status::InvalidArguments);
}
