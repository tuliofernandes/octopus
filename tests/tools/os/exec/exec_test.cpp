#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <fstream>
#include <sys/resource.h>
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

TEST_CASE("os.exec bounds normalized UTF-8 and drains alternating streams") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto invoke = [&](const char *mode) {
        return executor.execute(
            {"id", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {mode}}}});
    };
    auto cut = invoke("utf-cut");
    CHECK(cut.output.at("stdout").get<std::string>() == std::string(8191, 'x'));
    CHECK(cut.output.at("stdout_truncated") == true);
    auto replacements = invoke("replacement-volume");
    CHECK_NOTHROW(replacements.output.dump());
    CHECK(replacements.output.at("stdout").get<std::string>().size() <= 8192);
    CHECK(replacements.output.at("stdout_truncated") == true);
    auto alternating = invoke("alternating");
    CHECK(alternating.output.at("exit_code") == 0);
    CHECK(alternating.output.at("stdout_truncated") == true);
    CHECK(alternating.output.at("stderr_truncated") == true);
}

TEST_CASE("os.exec inherits cwd environment and PATH without leaking descriptors or zombies") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    const char *previous = getenv("OCTOPUS_EXEC_TEST");
    const std::optional<std::string> saved =
        previous ? std::optional<std::string>(previous) : std::nullopt;

    struct Restore {
        std::optional<std::string> value;

        ~Restore() {
            if (value)
                setenv("OCTOPUS_EXEC_TEST", value->c_str(), 1);
            else
                unsetenv("OCTOPUS_EXEC_TEST");
        }
    } restore{saved};

    REQUIRE(setenv("OCTOPUS_EXEC_TEST", "inherited value", 1) == 0);
    auto context = executor.execute(
        {"context", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {"context"}}}});
    CHECK(context.output.at("stdout") == std::filesystem::current_path().string());
    CHECK(context.output.at("stderr") == "inherited value");
    CHECK(executor.execute({"path", "os.exec", {{"program", "printf"}, {"args", {"literal"}}}})
              .output.at("stdout") == "literal");
    CHECK(
        executor.execute({"127", "os.exec", {{"program", "/bin/sh"}, {"args", {"-c", "exit 127"}}}})
            .output.at("exit_code") == 127);
    CHECK(executor
              .execute({"permission",
                        "os.exec",
                        {{"program", "/dev/null"}, {"args", nlohmann::json::array()}}})
              .status == Status::Failure);
    auto countFds = [] {
        std::size_t count = 0;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
        return count;
    };
    const auto before = countFds();
    for (int i = 0; i < 12; ++i) {
        auto result = executor.execute(
            {"repeat", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {"pid"}}}});
        REQUIRE(result.status == Status::Success);
        const auto child =
            static_cast<pid_t>(std::stoi(result.output.at("stdout").get<std::string>()));
        int status;
        CHECK(waitpid(child, &status, WNOHANG) == -1);
        CHECK(errno == ECHILD);
    }
    CHECK(countFds() == before);
}

TEST_CASE("os.exec reports pipe acquisition failure without leaking resources") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    struct rlimit original;
    REQUIRE(getrlimit(RLIMIT_NOFILE, &original) == 0);

    struct RestoreLimit {
        struct rlimit original;

        ~RestoreLimit() { setrlimit(RLIMIT_NOFILE, &original); }
    } restore{original};

    auto limited = original;
    limited.rlim_cur = 3;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    auto failure =
        Executor(registry, policy)
            .execute({"no-fds",
                      "os.exec",
                      {{"program", PROCESS_FIXTURE}, {"args", nlohmann::json::array()}}});
    CHECK(failure.status == Status::Failure);
    CHECK(failure.output.get<std::string>().find("pipe:") != std::string::npos);
}

TEST_CASE("os.exec normalizes auxiliary descriptors when standard streams are closed") {
    Registry registry;
    loadTools(registry, TOOL_RESOURCE_ROOT);
    AllowAllSecurityPolicy policy;
    for (int mask : {1, 2, 4, 7}) {
        const auto child = fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            for (int fd = 0; fd < 3; ++fd)
                if (mask & (1 << fd))
                    close(fd);
            try {
                const auto result =
                    Executor(registry, policy)
                        .execute({"closed",
                                  "os.exec",
                                  {{"program", PROCESS_FIXTURE}, {"args", {"argv", "literal"}}}});
                _exit(result.status == Status::Success &&
                              result.output.at("stdout") == "[literal]" &&
                              result.output.at("stderr") == "diagnostic"
                          ? 0
                          : 1);
            } catch (...) {
                _exit(2);
            }
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        REQUIRE(waited == child);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
}
