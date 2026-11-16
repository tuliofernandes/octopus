#include "octopus/tool/activity.hpp"
#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace octopus::tool;

TEST_CASE("executor measures handler duration without an observer") {
    Registry registry;
    const nlohmann::json schema = {{"type", "object"}};
    REQUIRE(registry.add(
        {"test", "timed", "timed", schema, {}}, [](const auto &, const auto *) { return "ok"; },
        [](const auto &) { return std::string{}; }));
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto result = executor.execute({"id", "test.timed", nlohmann::json::object()});
    REQUIRE(result.duration.has_value());
    CHECK(*result.duration >= std::chrono::steady_clock::duration::zero());
    CHECK_FALSE(executor.execute({"bad", "test.timed", 42}).duration.has_value());
    CHECK_FALSE(executor.execute({"unknown", "missing", {}}).duration.has_value());
}

TEST_CASE("activity events correlate outcomes and isolate presentation exceptions") {
    Registry registry;
    int calls = 0;
    REQUIRE(registry.add(
        {"test", "activity", "activity", {{"type", "object"}}, {}},
        [&](const auto &, const auto *) -> Outcome {
            ++calls;
            if (calls == 2)
                throw std::runtime_error("failed");
            return {Status::Cancelled, "partial", "Stopped"};
        },
        [](const auto &) { return std::string{}; }, [](const auto &) { return "Custom label"; }));

    struct Observer : ActivityObserver {
        int starts = 0, finishes = 0;
        std::optional<std::chrono::steady_clock::duration> duration;

        void started(const ActivityStart &event) override {
            ++starts;
            CHECK(event.call_id == "id");
            CHECK(event.identity == "test.activity");
            CHECK(event.label == "Custom label");
            throw std::runtime_error("UI start");
        }

        void finished(const ActivityStart &, const Result &result) override {
            ++finishes;
            duration = result.duration;
            throw std::runtime_error("UI finish");
        }
    } observer;

    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto result =
        executor.execute({"id", "test.activity", nlohmann::json::object()}, nullptr, &observer);
    CHECK(result.status == Status::Cancelled);
    CHECK(result.summary == "Stopped");
    CHECK(result.duration == observer.duration);
    auto failure =
        executor.execute({"id", "test.activity", nlohmann::json::object()}, nullptr, &observer);
    CHECK(failure.status == Status::Failure);
    CHECK(failure.duration.has_value());
    CHECK(observer.starts == 2);
    CHECK(observer.finishes == 2);
    CHECK(calls == 2);
}

TEST_CASE("activity falls back to identity when label generation throws") {
    Registry registry;
    REQUIRE(registry.add(
        {"test", "fallback", "fallback", {{"type", "object"}}, {}},
        [](const auto &, const auto *) { return "ok"; }, [](const auto &) { return std::string{}; },
        [](const auto &) -> std::string { throw std::runtime_error("label"); }));

    struct Observer : ActivityObserver {
        std::string label;

        void started(const ActivityStart &event) override { label = event.label; }

        void finished(const ActivityStart &, const Result &) override {}
    } observer;

    AllowAllSecurityPolicy policy;
    CHECK(Executor(registry, policy)
              .execute({"id", "test.fallback", nlohmann::json::object()}, nullptr, &observer)
              .status == Status::Success);
    CHECK(observer.label == "test.fallback");
}
