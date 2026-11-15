#include "octopus/chat/runtime.hpp"
#include "octopus/cli/chat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sstream>

namespace {
std::string validateEmpty(const nlohmann::json &args) {
    return args.is_object() && args.empty() ? "" : "expected an empty object";
}

class Compiler : public octopus::llm::PromptCompiler {
  public:
    mutable std::vector<octopus::llm::Conversation> requests;

    octopus::llm::CompileResult compile(const octopus::llm::Conversation &c) const override {
        requests.push_back(c);
        return octopus::llm::CompileResult::success({"prompt", {}});
    }
};

class Parser : public octopus::llm::AssistantResponseParser {
  public:
    octopus::llm::ParseResult parse(const octopus::llm::RawCompletion &c) const override {
        if (c.text == "call")
            return octopus::llm::ParseResult::success(
                {"", {{"", "misc.hello", nlohmann::json::object()}}});
        return octopus::llm::ParseResult::success({c.text});
    }
};

class Backend : public octopus::llm::InferenceBackend {
  public:
    int calls = 0;

    octopus::llm::InferenceResult generate(const octopus::llm::CompiledPrompt &,
                                           const octopus::llm::GenerationOptions &,
                                           const octopus::llm::CancellationToken *,
                                           octopus::llm::CompletionSink *) override {
        return octopus::llm::InferenceResult::success(
            {calls++ == 0 ? "call" : "done", octopus::llm::FinishReason::EndOfGeneration, 0});
    }
};
} // namespace

TEST_CASE("chat executes one call and passes its correlated result to the next "
          "generation") {
    Compiler compiler;
    Parser parser;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    int executed = 0;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}},
        [&](const auto &, const auto *) {
            ++executed;
            return "hello result";
        },
        validateEmpty));
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    auto result = chat.turn("please call", nullptr);
    REQUIRE(result.hasValue());
    CHECK(result.value().response.text == "done");
    REQUIRE(compiler.requests.size() == 2);
    const auto &messages = compiler.requests[1].messages;
    REQUIRE(messages.size() == 3);
    CHECK(messages[1].tool_calls[0].id == messages[2].tool_call_id);
    CHECK(messages[2].content.find("hello result") != std::string::npos);
    CHECK(executed == 1);
    const auto previous_id = messages[2].tool_call_id;
    auto later = chat.turn("follow up", nullptr);
    REQUIRE(later.hasValue());
    REQUIRE(compiler.requests.size() == 3);
    CHECK(compiler.requests[2].messages.size() == 5);
    CHECK(compiler.requests[2].messages[2].tool_call_id == previous_id);
    CHECK(compiler.requests[2].messages[3].content == "done");
}

TEST_CASE("chat stops repeated tool calls at the configured round limit") {
    struct Calls : octopus::llm::InferenceBackend {
        int calls = 0;

        octopus::llm::InferenceResult generate(const octopus::llm::CompiledPrompt &,
                                               const octopus::llm::GenerationOptions &,
                                               const octopus::llm::CancellationToken *,
                                               octopus::llm::CompletionSink *) override {
            ++calls;
            return octopus::llm::InferenceResult::success(
                {"call", octopus::llm::FinishReason::EndOfGeneration, 0});
        }
    } backend;

    Compiler compiler;
    Parser parser;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    int executions = 0;
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}},
        [&](const auto &, const auto *) {
            ++executions;
            return "ok";
        },
        validateEmpty));
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::chat::Runtime chat(llm, registry, executor, {}, 1);
    auto result = chat.turn("call", nullptr);
    CHECK_FALSE(result.hasValue());
    CHECK(result.error().find("limit") != std::string::npos);
    CHECK(executions == 1);
    CHECK(backend.calls == 2);
    CHECK(chat.history().messages.size() == 3);
}

TEST_CASE("chat preserves completed results and cancels later calls") {
    struct MultipleParser : octopus::llm::AssistantResponseParser {
        octopus::llm::ParseResult parse(const octopus::llm::RawCompletion &) const override {
            return octopus::llm::ParseResult::success(
                {"",
                 {{"", "misc.hello", nlohmann::json::object()},
                  {"", "misc.hello", nlohmann::json::object()}}});
        }
    } parser;

    struct Token : octopus::core::CancellationToken {
        bool cancelled = false;

        bool isCancellationRequested() const noexcept override { return cancelled; }
    } token;

    Compiler compiler;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    int executions = 0;
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}},
        [&](const auto &, const auto *) {
            ++executions;
            token.cancelled = true;
            return "completed";
        },
        validateEmpty));
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    auto result = chat.turn("call", &token);
    REQUIRE(result.hasValue());
    CHECK(result.value().finish_reason == octopus::llm::FinishReason::Cancelled);
    CHECK(executions == 1);
    CHECK(backend.calls == 1);
    const auto &history = chat.history().messages;
    REQUIRE(history.size() == 4);
    CHECK(history[2].content.find("completed") != std::string::npos);
    CHECK(history[3].content.find("cancelled") != std::string::npos);
    CHECK(history[1].tool_calls[0].id == history[2].tool_call_id);
    CHECK(history[1].tool_calls[1].id == history[3].tool_call_id);
    token.cancelled = false;
    auto next = chat.turn("next", &token);
    REQUIRE(next.hasValue());
    REQUIRE(compiler.requests.size() == 2);
    REQUIRE(compiler.requests[1].messages.size() == 5);
    CHECK(compiler.requests[1].messages[2].content.find("completed") != std::string::npos);
    CHECK(compiler.requests[1].messages[3].content.find("cancelled") != std::string::npos);
}

TEST_CASE("rejected calls return correlated outcomes for the model") {
    struct Parser : octopus::llm::AssistantResponseParser {
        octopus::llm::ParseResult parse(const octopus::llm::RawCompletion &raw) const override {
            if (raw.text == "call")
                return octopus::llm::ParseResult::success(
                    {"",
                     {{"", "missing.unknown", nlohmann::json::object()},
                      {"", "misc.hello", {{"extra", true}}},
                      {"", "misc.hello", nlohmann::json::object()}}});
            return octopus::llm::ParseResult::success({raw.text});
        }
    } parser;

    struct Deny : octopus::tool::SecurityPolicy {
        octopus::tool::AuthorizationDecision authorize(const octopus::tool::Definition &,
                                                       const octopus::tool::Call &) const override {
            return {false, "no permission"};
        }
    } deny;

    Compiler compiler;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    int executions = 0;
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}},
        [&](const auto &, const auto *) {
            ++executions;
            return "bad";
        },
        validateEmpty));
    octopus::tool::Executor executor(registry, deny);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    auto result = chat.turn("call", nullptr);
    REQUIRE(result.hasValue());
    REQUIRE(compiler.requests.size() == 2);
    const auto &messages = compiler.requests[1].messages;
    REQUIRE(messages.size() == 5);
    CHECK(messages[2].content.find("unknown_tool") != std::string::npos);
    CHECK(messages[3].content.find("invalid_arguments") != std::string::npos);
    CHECK(messages[4].content.find("no permission") != std::string::npos);
    CHECK(executions == 0);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(messages[1].tool_calls[i].id == messages[2 + i].tool_call_id);
}

TEST_CASE("chat executes multiple calls serially and correlates their results") {
    struct Many : octopus::llm::AssistantResponseParser {
        octopus::llm::ParseResult parse(const octopus::llm::RawCompletion &raw) const override {
            if (raw.text != "call")
                return octopus::llm::ParseResult::success({raw.text});
            return octopus::llm::ParseResult::success(
                {"",
                 {{"", "misc.hello", nlohmann::json::object()},
                  {"", "misc.hello", nlohmann::json::object()}}});
        }
    } parser;

    Compiler compiler;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    int executions = 0;
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}},
        [&](const auto &, const auto *) { return std::to_string(++executions); }, validateEmpty));
    octopus::tool::AllowAllSecurityPolicy allow;
    octopus::tool::Executor executor(registry, allow);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    REQUIRE(chat.turn("call", nullptr).hasValue());
    const auto &messages = compiler.requests[1].messages;
    REQUIRE(messages.size() == 4);
    CHECK(messages[1].tool_calls[0].id != messages[1].tool_calls[1].id);
    CHECK(messages[1].tool_calls[0].id == messages[2].tool_call_id);
    CHECK(messages[1].tool_calls[1].id == messages[3].tool_call_id);
    CHECK(nlohmann::json::parse(messages[2].content)["output"] == "1");
    CHECK(nlohmann::json::parse(messages[3].content)["output"] == "2");
}

TEST_CASE("CLI chat renders final text without leaking tool protocol") {
    Compiler compiler;
    Parser parser;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", nlohmann::json::object()},
                                   {"additionalProperties", false}};
    REQUIRE(registry.add(
        {"misc", "hello", "hello", schema, {}}, [](const auto &, const auto *) { return "hidden"; },
        validateEmpty));
    octopus::tool::AllowAllSecurityPolicy allow;
    octopus::tool::Executor executor(registry, allow);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    std::istringstream in("call\n");
    std::ostringstream out, err;
    auto result = octopus::runCliChat({}, llm, in, out, err, &chat);
    CHECK(result.exit_code == 0);
    CHECK(out.str().find("done") != std::string::npos);
    CHECK(out.str().find("hidden") == std::string::npos);
    CHECK(err.str().empty());
}
