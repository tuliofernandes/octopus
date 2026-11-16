#include "octopus/tool/resources.hpp"
#include "octopus/tool/tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <stdexcept>
using namespace octopus::tool;

static std::string validateEmpty(const nlohmann::json &args) {
    return args.is_object() && args.empty() ? "" : "expected an empty object";
}

const nlohmann::json kSchema = {
    {"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}};

TEST_CASE("registry validates dispatch and authorizes calls") {
    Registry registry;
    int calls = 0;
    Definition def{"misc", "hello", "greeting", kSchema, {}};
    REQUIRE(registry.add(
        def,
        [&](const auto &, const auto *) {
            ++calls;
            return "hello";
        },
        validateEmpty));
    REQUIRE_FALSE(
        registry.add(def, [](const auto &, const auto *) { return "other"; }, validateEmpty));
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto good = executor.execute({"one", "misc.hello", nlohmann::json::object()});
    CHECK(good.status == Status::Success);
    CHECK(good.call_id == "one");
    CHECK(good.output == "hello");
    CHECK(executor.execute({"two", "misc.hello", {{"extra", true}}}).status ==
          Status::InvalidArguments);
    CHECK(executor.execute({"three", "unknown", {}}).status == Status::UnknownTool);
    CHECK(executor.execute({"four", "misc.hello", 42}).status == Status::InvalidArguments);
    CHECK(calls == 1);
}

TEST_CASE("pilot manifest is loaded from its resource root") {
    auto definition = loadManifest(TOOL_RESOURCE_ROOT, "misc", "hello_world");
    CHECK(definition.identity() == "misc.hello_world");
    CHECK(definition.parameters["type"] == "object");
    CHECK_THROWS(loadManifest(TOOL_RESOURCE_ROOT, "misc", "missing"));
}

TEST_CASE("security denial and cancellation prevent invocation") {
    struct Deny : SecurityPolicy {
        AuthorizationDecision authorize(const Definition &, const Call &) const override {
            return {false, "no"};
        }
    } deny;

    struct Cancel : octopus::core::CancellationToken {
        bool isCancellationRequested() const noexcept override { return true; }
    } cancel;

    Registry registry;
    int calls = 0;
    REQUIRE(registry.add(
        {"misc", "hello", "greeting", kSchema, {}},
        [&](const auto &, const auto *) {
            ++calls;
            return "hello";
        },
        validateEmpty));
    Executor executor(registry, deny);
    auto denied = executor.execute({"one", "misc.hello", nlohmann::json::object()});
    CHECK(denied.status == Status::Denied);
    CHECK(denied.output == "no");
    CHECK(executor.execute({"two", "misc.hello", nlohmann::json::object()}, &cancel).status ==
          Status::Cancelled);
    CHECK(calls == 0);
}

TEST_CASE("policy and handler errors retain call correlation") {
    struct FailingPolicy : SecurityPolicy {
        AuthorizationDecision authorize(const Definition &, const Call &) const override {
            throw std::runtime_error("policy failed");
        }
    } failing;

    Registry registry;
    int calls = 0;
    REQUIRE_FALSE(registry.add({"misc", "bad", "bad", kSchema, {}}, {}, validateEmpty));
    REQUIRE(registry.add(
        {"misc", "bad", "bad", kSchema, {}},
        [&](const auto &, const auto *) -> std::string {
            ++calls;
            throw std::runtime_error("handler failed");
        },
        validateEmpty));
    Executor no_dispatch(registry, failing);
    auto failure = no_dispatch.execute({"id1", "misc.bad", nlohmann::json::object()});
    CHECK(failure.status == Status::Failure);
    CHECK(failure.call_id == "id1");
    CHECK(failure.output == "policy failed");
    CHECK(calls == 0);
    AllowAllSecurityPolicy allow;
    auto handler_failure =
        Executor(registry, allow).execute({"id2", "misc.bad", nlohmann::json::object()});
    CHECK(handler_failure.status == Status::Failure);
    CHECK(handler_failure.call_id == "id2");
    CHECK(handler_failure.output == "handler failed");
    CHECK(calls == 1);
}

TEST_CASE("manifest loader rejects invalid version, malformed JSON and schema") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-tool-manifest-tests";
    fs::create_directories(root / "misc" / "hello");
    auto path = root / "misc" / "hello" / "manifest.json";
    auto write = [&](const std::string &text) {
        std::ofstream output(path);
        output << text;
    };
    write("{");
    CHECK_THROWS(loadManifest(root, "misc", "hello"));
    write(
        R"({"schema_version":2,"name":"hello","category":"misc","description":"x","parameters":{"type":"object","properties":{},"additionalProperties":false},"permissions":[]})");
    CHECK_THROWS(loadManifest(root, "misc", "hello"));
    write(
        R"({"schema_version":1,"name":"hello","category":"misc","description":"x","parameters":{"type":"string"},"permissions":[]})");
    CHECK_THROWS(loadManifest(root, "misc", "hello"));
    CHECK_THROWS(loadManifest(root, "../misc", "hello"));
    fs::remove_all(root);
}

TEST_CASE("valid executions consult security with exact call before dispatch") {
    struct Recording : SecurityPolicy {
        mutable std::string name;
        mutable std::string id;

        AuthorizationDecision authorize(const Definition &definition,
                                        const Call &call) const override {
            name = definition.identity();
            id = call.id;
            return {true, {}};
        }
    } policy;

    Registry registry;
    REQUIRE(registry.add(
        {"misc", "hello", "greeting", kSchema, {"read"}},
        [&](const auto &, const auto *) {
            REQUIRE(policy.id == "id");
            return "ok";
        },
        validateEmpty));
    const auto result =
        Executor(registry, policy).execute({"id", "misc.hello", nlohmann::json::object()});
    CHECK(result.status == Status::Success);
    CHECK(policy.name == "misc.hello");
    CHECK(policy.id == "id");
}

TEST_CASE("completion remains successful when cancellation arrives during handler") {
    struct MutableCancellation : octopus::core::CancellationToken {
        bool cancelled = false;

        bool isCancellationRequested() const noexcept override { return cancelled; }
    } cancel;

    Registry registry;
    REQUIRE(registry.add(
        {"misc", "done", "done", kSchema, {}},
        [&](const auto &, const auto *) {
            cancel.cancelled = true;
            return "completed";
        },
        validateEmpty));
    AllowAllSecurityPolicy allow;
    const auto result =
        Executor(registry, allow).execute({"id", "misc.done", nlohmann::json::object()}, &cancel);
    CHECK(result.status == Status::Success);
    CHECK(result.output == "completed");
}

TEST_CASE("resource root works independently of current working directory") {
    const auto original = std::filesystem::current_path();

    struct Restore {
        std::filesystem::path path;

        ~Restore() { std::filesystem::current_path(path); }
    } restore{original};

    std::filesystem::current_path(std::filesystem::temp_directory_path());
    CHECK(loadManifest(TOOL_RESOURCE_ROOT, "misc", "hello_world").identity() == "misc.hello_world");
}

TEST_CASE("registry refuses unsupported schemas and malformed identities") {
    Registry registry;
    CHECK_FALSE(registry.add(
        {"../misc", "bad", "x", {}, {}}, [](const auto &, const auto *) { return "x"; },
        validateEmpty));
    CHECK_FALSE(registry.add(
        {"misc", "bad", "x", {{"type", "string"}}, {}},
        [](const auto &, const auto *) { return "x"; }, validateEmpty));
    CHECK(registry.find("misc.bad") == nullptr);
}

TEST_CASE("tool resources resolve from build and installed executables") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-resource-resolution-test";
    fs::remove_all(root);
    fs::create_directories(root / "build/share/octopus/tools");
    fs::create_directories(root / "install/bin");
    fs::create_directories(root / "install/share/octopus/tools");
    std::ofstream(root / "build/octo").put('\n');
    std::ofstream(root / "install/bin/octo").put('\n');
    REQUIRE(octopus::tool::resourceRoot((root / "build/octo").string()) ==
            root / "build/share/octopus/tools");
    REQUIRE(octopus::tool::resourceRoot((root / "install/bin/octo").string()) ==
            root / "install/share/octopus/tools");
    fs::remove_all(root);
}

TEST_CASE("tool-specific validators accept or reject arguments before dispatch") {
    Registry registry;
    int calls = 0;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"path", {{"type", "string"}}}}},
                                   {"required", {"path"}}};
    auto validate = [](const nlohmann::json &args) -> std::string {
        if (!args.is_object() || !args.contains("path") || !args["path"].is_string())
            return "path must be a string";
        return {};
    };
    REQUIRE(registry.add(
        {"fs", "read", "read", schema, {}},
        [&](const auto &args, const auto *) {
            ++calls;
            return args.at("path").template get<std::string>();
        },
        validate));
    AllowAllSecurityPolicy policy;
    Executor executor(registry, policy);
    auto valid = executor.execute({"one", "fs.read", {{"path", "file.txt"}}});
    CHECK(valid.status == Status::Success);
    CHECK(valid.output == "file.txt");
    auto invalid = executor.execute({"two", "fs.read", {{"path", 42}}});
    CHECK(invalid.status == Status::InvalidArguments);
    CHECK_FALSE(invalid.output.empty());
    CHECK(invalid.call_id == "two");
    CHECK(calls == 1);
}

TEST_CASE("manifest supports object schemas with declared parameters") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-parameter-manifest-test";
    fs::create_directories(root / "fs/read");
    {
        std::ofstream file(root / "fs/read/manifest.json");
        file
            << R"({"schema_version":1,"category":"fs","name":"read","description":"read file","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]},"permissions":[]})";
    }
    const auto definition = loadManifest(root, "fs", "read");
    CHECK(definition.parameters.at("properties").at("path").at("type") == "string");
    fs::remove_all(root);
}

TEST_CASE("registry requires a validator and validator failures prevent execution") {
    Registry registry;
    int calls = 0;
    auto handler = [&](const auto &, const auto *) {
        ++calls;
        return "ok";
    };
    Definition def{"misc", "test", "test", kSchema, {}};
    CHECK_FALSE(registry.add(def, handler, {}));
    CHECK(registry.find("misc.test") == nullptr);
    REQUIRE(registry.add(def, handler, [](const auto &) -> std::string {
        throw std::runtime_error("validator failed");
    }));
    AllowAllSecurityPolicy policy;
    const auto result =
        Executor(registry, policy).execute({"id", "misc.test", nlohmann::json::object()});
    CHECK(result.status == Status::Failure);
    CHECK(result.output == "validator failed");
    CHECK(result.call_id == "id");
    CHECK(calls == 0);
}

TEST_CASE("loader discovers a tool by its manifest without a built-in mapping") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-tool-discovery-test";
    const auto dir = root / "other" / "greeting";
    fs::remove_all(root);
    fs::create_directories(dir);
    {
        std::ofstream manifest(dir / "manifest.json");
        manifest
            << R"({"schema_version":1,"category":"other","name":"greeting","description":"greeting","parameters":{"type":"object","properties":{},"additionalProperties":false},"permissions":[]})";
    }
    fs::copy_file(fs::path(TOOL_RESOURCE_ROOT) / "misc/hello_world" /
                      (std::string("tool") + TOOL_MODULE_SUFFIX),
                  dir / (std::string("tool") + TOOL_MODULE_SUFFIX));
    Registry registry;
    loadTools(registry, root);
    fs::remove_all(root);
    AllowAllSecurityPolicy policy;
    auto result =
        Executor(registry, policy).execute({"id", "other.greeting", nlohmann::json::object()});
    CHECK(result.status == Status::Success);
    CHECK(result.output.get<std::string>().rfind("Hello, world! Random number ", 0) == 0);
}

TEST_CASE("loader reports a missing tool library") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-missing-tool-test";
    const auto dir = root / "misc" / "hello_world";
    fs::remove_all(root);
    fs::create_directories(dir);
    fs::copy_file(fs::path(TOOL_RESOURCE_ROOT) / "misc/hello_world/manifest.json",
                  dir / "manifest.json");
    Registry registry;
    CHECK_THROWS_WITH(loadTools(registry, root),
                      Catch::Matchers::ContainsSubstring("cannot load tool"));
    CHECK(registry.definitions().empty());
    fs::remove_all(root);
}

TEST_CASE("handler owns structured output and operational status") {
    Registry registry;
    REQUIRE(registry.add(
        {"test", "outcome", "outcome", kSchema, {}},
        [](const auto &, const auto *) {
            return Outcome{Status::Cancelled, nlohmann::json{{"partial", true}}};
        },
        validateEmpty));
    AllowAllSecurityPolicy policy;
    const auto result = Executor(registry, policy)
                            .execute({"structured", "test.outcome", nlohmann::json::object()});
    CHECK(result.call_id == "structured");
    CHECK(result.status == Status::Cancelled);
    CHECK(result.output.at("partial") == true);
}

TEST_CASE("loader rejects ABI v1 before reading callbacks") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "octopus-legacy-module-test";
    const auto dir = root / "misc/hello_world";
    fs::create_directories(dir);
    fs::copy_file(fs::path(TOOL_RESOURCE_ROOT) / "misc/hello_world/manifest.json",
                  dir / "manifest.json", fs::copy_options::overwrite_existing);
    fs::copy_file(LEGACY_MODULE_PATH, dir / (std::string("tool") + TOOL_MODULE_SUFFIX),
                  fs::copy_options::overwrite_existing);
    Registry registry;
    CHECK_THROWS_WITH(loadTools(registry, root), Catch::Matchers::ContainsSubstring("ABI v2"));
    fs::remove_all(root);
}
