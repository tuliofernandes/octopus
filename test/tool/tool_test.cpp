#include "octopus/tool/resources.hpp"
#include <fstream>
#include "octopus/tool/tool.hpp"
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <fstream>
using namespace octopus::tool;

const nlohmann::json kSchema = {{"type", "object"},
                                {"properties", nlohmann::json::object()},
                                {"additionalProperties", false}};

TEST_CASE("registry validates dispatch and authorizes calls") {
  Registry registry;
  int calls = 0;
  Definition def{"misc", "hello", "greeting", kSchema, {}};
  REQUIRE(registry.add(def, [&](const auto&, const auto*) {
    ++calls;
    return "hello";
  }));
  REQUIRE_FALSE(
      registry.add(def, [](const auto&, const auto*) { return "other"; }));
  AllowAllSecurityPolicy policy;
  Executor executor(registry, policy);
  auto good = executor.execute({"one", "misc.hello", nlohmann::json::object()});
  CHECK(good.status == Status::Success);
  CHECK(good.call_id == "one");
  CHECK(good.output == "hello");
  CHECK(executor.execute({"two", "misc.hello", {{"extra", true}}}).status ==
        Status::InvalidArguments);
  CHECK(executor.execute({"three", "unknown", {}}).status ==
        Status::UnknownTool);
  CHECK(executor.execute({"four", "misc.hello", 42}).status ==
        Status::InvalidArguments);
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
    AuthorizationDecision authorize(const Definition&,
                                    const Call&) const override {
      return {false, "no"};
    }
  } deny;
  struct Cancel : octopus::core::CancellationToken {
    bool isCancellationRequested() const noexcept override { return true; }
  } cancel;
  Registry registry;
  int calls = 0;
  REQUIRE(registry.add({"misc", "hello", "greeting", kSchema, {}},
                       [&](const auto&, const auto*) {
                         ++calls;
                         return "hello";
                       }));
  Executor executor(registry, deny);
  auto denied =
      executor.execute({"one", "misc.hello", nlohmann::json::object()});
  CHECK(denied.status == Status::Denied);
  CHECK(denied.output == "no");
  CHECK(
      executor.execute({"two", "misc.hello", nlohmann::json::object()}, &cancel)
          .status == Status::Cancelled);
  CHECK(calls == 0);
}

TEST_CASE("real builtin returns a greeting and an in-range number") {
  Registry registry;
  registerBuiltins(registry, TOOL_RESOURCE_ROOT);
  AllowAllSecurityPolicy policy;
  Executor executor(registry, policy);
  auto result = executor.execute(
      {"pilot-1", "misc.hello_world", nlohmann::json::object()});
  REQUIRE(result.status == Status::Success);
  CHECK(result.call_id == "pilot-1");
  const std::string prefix = "Hello, world! Random number ";
  REQUIRE(result.output.rfind(prefix, 0) == 0);
  const auto digits = result.output.substr(prefix.size());
  REQUIRE_FALSE(digits.empty());
  REQUIRE(digits.find_first_not_of("0123456789") == std::string::npos);
  CHECK(std::stoi(digits) >= 1000);
  CHECK(std::stoi(digits) <= 99999);
}

TEST_CASE("policy and handler errors retain call correlation") {
  struct FailingPolicy : SecurityPolicy {
    AuthorizationDecision authorize(const Definition&,
                                    const Call&) const override {
      throw std::runtime_error("policy failed");
    }
  } failing;
  Registry registry;
  int calls = 0;
  REQUIRE_FALSE(registry.add({"misc", "bad", "bad", kSchema, {}}, {}));
  REQUIRE(registry.add({"misc", "bad", "bad", kSchema, {}},
                       [&](const auto&, const auto*) -> std::string {
                         ++calls;
                         throw std::runtime_error("handler failed");
                       }));
  Executor no_dispatch(registry, failing);
  auto failure =
      no_dispatch.execute({"id1", "misc.bad", nlohmann::json::object()});
  CHECK(failure.status == Status::Failure);
  CHECK(failure.call_id == "id1");
  CHECK(failure.output == "policy failed");
  CHECK(calls == 0);
  AllowAllSecurityPolicy allow;
  auto handler_failure =
      Executor(registry, allow)
          .execute({"id2", "misc.bad", nlohmann::json::object()});
  CHECK(handler_failure.status == Status::Failure);
  CHECK(handler_failure.call_id == "id2");
  CHECK(handler_failure.output == "handler failed");
  CHECK(calls == 1);
}

TEST_CASE(
    "manifest loader rejects invalid version, malformed JSON and schema") {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / "octopus-tool-manifest-tests";
  fs::create_directories(root / "misc" / "hello");
  auto path = root / "misc" / "hello" / "manifest.json";
  auto write = [&](const std::string& text) {
    std::ofstream output(path);
    output << text;
  };
  write("{");
  CHECK_THROWS(loadManifest(root, "misc", "hello"));
  write(
      R"({"schema_version":2,"name":"hello","category":"misc","description":"x","parameters":{"type":"object","properties":{},"additionalProperties":false},"permissions":[]})");
  CHECK_THROWS(loadManifest(root, "misc", "hello"));
  write(
      R"({"schema_version":1,"name":"hello","category":"misc","description":"x","parameters":{"type":"object"},"permissions":[]})");
  CHECK_THROWS(loadManifest(root, "misc", "hello"));
  CHECK_THROWS(loadManifest(root, "../misc", "hello"));
  fs::remove_all(root);
}

TEST_CASE("valid executions consult security with exact call before dispatch") {
  struct Recording : SecurityPolicy {
    mutable std::string name;
    mutable std::string id;
    AuthorizationDecision authorize(const Definition& definition,
                                    const Call& call) const override {
      name = definition.identity();
      id = call.id;
      return {true, {}};
    }
  } policy;
  Registry registry;
  REQUIRE(registry.add({"misc", "hello", "greeting", kSchema, {"read"}},
                       [&](const auto&, const auto*) {
                         REQUIRE(policy.id == "id");
                         return "ok";
                       }));
  const auto result =
      Executor(registry, policy)
          .execute({"id", "misc.hello", nlohmann::json::object()});
  CHECK(result.status == Status::Success);
  CHECK(policy.name == "misc.hello");
  CHECK(policy.id == "id");
}

TEST_CASE(
    "completion remains successful when cancellation arrives during handler") {
  struct MutableCancellation : octopus::core::CancellationToken {
    bool cancelled = false;
    bool isCancellationRequested() const noexcept override { return cancelled; }
  } cancel;
  Registry registry;
  REQUIRE(registry.add({"misc", "done", "done", kSchema, {}},
                       [&](const auto&, const auto*) {
                         cancel.cancelled = true;
                         return "completed";
                       }));
  AllowAllSecurityPolicy allow;
  const auto result =
      Executor(registry, allow)
          .execute({"id", "misc.done", nlohmann::json::object()}, &cancel);
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
  CHECK(loadManifest(TOOL_RESOURCE_ROOT, "misc", "hello_world").identity() ==
        "misc.hello_world");
}

TEST_CASE("registry refuses unsupported schemas and malformed identities") {
  Registry registry;
  CHECK_FALSE(registry.add({"../misc", "bad", "x", {}, {}},
                           [](const auto&, const auto*) { return "x"; }));
  CHECK_FALSE(registry.add({"misc", "bad", "x", {{"type", "string"}}, {}},
                           [](const auto&, const auto*) { return "x"; }));
  CHECK(registry.find("misc.bad") == nullptr);
}

TEST_CASE("tool resources resolve from build and installed executables") {
  namespace fs = std::filesystem;
  const auto root =
      fs::temp_directory_path() / "octopus-resource-resolution-test";
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
