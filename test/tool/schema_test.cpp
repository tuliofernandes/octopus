#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace octopus::tool;

TEST_CASE("schema rejects invalid array items before semantic validation and policy") {
    Registry registry;
    int semantic_calls = 0;
    REQUIRE(registry.add(
        {"os",
         "test",
         "test",
         {{"type", "object"},
          {"properties",
           {{"program", {{"type", "string"}, {"minLength", 1}}},
            {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
          {"required", {"program", "args"}},
          {"additionalProperties", false}},
         {}},
        [](const auto &, const auto *) { return "unexpected"; },
        [&](const auto &) {
            ++semantic_calls;
            return std::string{};
        }));

    struct Policy : SecurityPolicy {
        mutable int calls = 0;

        AuthorizationDecision authorize(const Definition &, const Call &) const override {
            ++calls;
            return {true, {}};
        }
    } policy;

    Executor executor(registry, policy);
    for (const auto &args : {nlohmann::json{{"program", "x"}, {"args", {42}}},
                             nlohmann::json{{"program", ""}, {"args", nlohmann::json::array()}},
                             nlohmann::json{{"program", "x"}}})
        CHECK(executor.execute({"id", "os.test", args}).status == Status::InvalidArguments);
    CHECK(semantic_calls == 0);
    CHECK(policy.calls == 0);
}

TEST_CASE("registry refuses malformed schemas and unsupported dialects") {
    for (const auto &schema :
         {nlohmann::json{{"type", "object"}, {"required", "bad"}},
          nlohmann::json{{"type", "object"},
                         {"$schema", "https://json-schema.org/draft/2020-12/schema"}},
          nlohmann::json{{"type", "object"}, {"$ref", "https://example.org/external"}}}) {
        Registry registry;
        CHECK_FALSE(registry.add(
            {"test", "bad", "test", schema, {}}, [](const auto &, const auto *) { return "ok"; },
            [](const auto &) { return std::string{}; }));
        CHECK(registry.find("test.bad") == nullptr);
    }
}

TEST_CASE("compiled schemas enforce local references numeric bounds and extra properties") {
    Registry registry;
    nlohmann::json schema = {
        {"type", "object"},
        {"definitions", {{"count", {{"type", "integer"}, {"minimum", 2}, {"maximum", 4}}}}},
        {"properties", {{"n", {{"$ref", "#/definitions/count"}}}}},
        {"required", {"n"}},
        {"additionalProperties", false}};
    REQUIRE(registry.add(
        {"test", "bounds", "bounds", schema, {}}, [](const auto &, const auto *) { return "ok"; },
        [](const auto &) { return std::string{}; }));
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    CHECK(executor.execute({"id", "test.bounds", {{"n", 3}}}).status == Status::Success);
    CHECK(executor.execute({"id", "test.bounds", {{"n", 1}}}).status == Status::InvalidArguments);
    CHECK(executor.execute({"id", "test.bounds", {{"n", 5}}}).status == Status::InvalidArguments);
    CHECK(executor.execute({"id", "test.bounds", {{"n", 3}, {"extra", true}}}).status ==
          Status::InvalidArguments);
}
