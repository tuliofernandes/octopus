#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

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

TEST_CASE("os.exec drains volume while retaining independent bounded prefixes") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto invoke = [&](std::vector<std::string> args) {
        return executor.execute({"id", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", args}}});
    };
    auto volume = invoke({"volume"});
    REQUIRE(volume.status == Status::Success);
    CHECK(volume.output.at("exit_code") == 0);
    for (const auto *stream : {"stdout", "stderr"}) {
        CHECK(volume.output.at(stream).get<std::string>().size() == 8192);
        CHECK(volume.output.at(std::string(stream) + "_truncated") == true);
    }
    auto exact = invoke({"bytes", "8192"});
    CHECK(exact.output.at("stdout_truncated") == false);
    auto extra = invoke({"bytes", "8193"});
    CHECK(extra.output.at("stdout_truncated") == true);
    auto lines = invoke({"lines", "201"});
    CHECK(lines.output.at("stdout").get<std::string>().size() == 400);
    CHECK(lines.output.at("stdout_truncated") == true);
    CHECK(invoke({"lines", "200"}).output.at("stdout_truncated") == false);
    auto invalid = invoke({"invalid"});
    CHECK_NOTHROW(invalid.output.dump());
    CHECK(invalid.output.at("stdout").get<std::string>().find("\xef\xbf\xbd") != std::string::npos);
}

TEST_CASE("os.exec cancellation reaps the direct child even with closed streams") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    const auto ready =
        std::filesystem::temp_directory_path() / ("octopus-ready-" + std::to_string(getpid()));

    struct ReadyToken : octopus::core::CancellationToken {
        std::filesystem::path path;

        bool isCancellationRequested() const noexcept override {
            std::error_code error;
            return std::filesystem::exists(path, error);
        }
    } token;

    token.path = ready;
    for (const char *mode : {"wait", "closed"}) {
        std::filesystem::remove(ready);
        auto result = executor.execute(
            {"cancel", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {mode, ready.string()}}}},
            &token);
        REQUIRE(result.status == Status::Cancelled);
        CHECK(result.output.at("signal") == SIGKILL);
        CHECK_FALSE(result.output.at("stdout").get<std::string>().empty());
        std::ifstream input(ready);
        pid_t child = 0;
        input >> child;
        REQUIRE(child > 0);
        int status;
        CHECK(waitpid(child, &status, WNOHANG) == -1);
        CHECK(errno == ECHILD);
    }
    auto before = executor.execute(
        {"before", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {"wait"}}}}, &token);
    CHECK(before.status == Status::Cancelled);
    std::filesystem::remove(ready);
}

TEST_CASE("os.exec bounds final capture when a descendant holds stdout open") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    auto result = Executor(registry, policy)
                      .execute({"descendant",
                                "os.exec",
                                {{"program", PROCESS_FIXTURE}, {"args", {"descendant"}}}});
    REQUIRE(result.status == Status::Success);
    const auto child = static_cast<pid_t>(std::stoi(result.output.at("stdout").get<std::string>()));

    struct Cleanup {
        pid_t child;

        ~Cleanup() {
            if (child > 0)
                kill(child, SIGKILL);
        }
    } cleanup{child};

    CHECK(result.output.at("capture_incomplete") == true);
    CHECK(result.output.at("exit_code") == 0);
}

TEST_CASE("os.exec policy denial and policy exceptions prevent process creation") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    const auto ready =
        std::filesystem::temp_directory_path() / ("octopus-denied-" + std::to_string(getpid()));

    struct Policy : SecurityPolicy {
        mutable nlohmann::json seen;
        bool fail = false;

        AuthorizationDecision authorize(const Definition &, const Call &call) const override {
            seen = call.arguments;
            if (fail)
                throw std::runtime_error("policy failure");
            return {false, "denied"};
        }
    } policy;

    Call call{
        "denied", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {"wait", ready.string()}}}};
    CHECK(Executor(registry, policy).execute(call).status == Status::Denied);
    CHECK(policy.seen == call.arguments);
    CHECK_FALSE(std::filesystem::exists(ready));
    policy.fail = true;
    CHECK(Executor(registry, policy).execute(call).status == Status::Failure);
    CHECK_FALSE(std::filesystem::exists(ready));
}
