#include "octopus/chat/runtime.hpp"
#include "octopus/cli/chat.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

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
    int generated_tokens = 0;

    octopus::llm::InferenceResult generate(const octopus::llm::CompiledPrompt &,
                                           const octopus::llm::GenerationOptions &,
                                           const octopus::llm::CancellationToken *,
                                           octopus::llm::CompletionSink *) override {
        return octopus::llm::InferenceResult::success({calls++ == 0 ? "call" : "done",
                                                       octopus::llm::FinishReason::EndOfGeneration,
                                                       generated_tokens});
    }
};

} /* namespace */

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
            return octopus::tool::Outcome{octopus::tool::Status::Success,
                                          nlohmann::json{{"text", "hello result"}}};
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
    CHECK(nlohmann::json::parse(messages[2].content).at("output").at("text") == "hello result");
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

TEST_CASE("chat executes real os.exec and retains bounded objects across turns") {
    struct ExecParser : octopus::llm::AssistantResponseParser {
        octopus::llm::ParseResult
        parse(const octopus::llm::RawCompletion &completion) const override {
            if (completion.text == "call")
                return octopus::llm::ParseResult::success(
                    {"", {{"", "os.exec", {{"program", PROCESS_FIXTURE}, {"args", {"volume"}}}}}});
            return octopus::llm::ParseResult::success({completion.text});
        }
    } parser;

    struct RepeatingBackend : octopus::llm::InferenceBackend {
        int calls = 0;

        octopus::llm::InferenceResult generate(const octopus::llm::CompiledPrompt &,
                                               const octopus::llm::GenerationOptions &,
                                               const octopus::llm::CancellationToken *,
                                               octopus::llm::CompletionSink *) override {
            return octopus::llm::InferenceResult::success(
                {calls++ % 2 == 0 ? "call" : "done", octopus::llm::FinishReason::EndOfGeneration,
                 0});
        }
    } backend;

    Compiler compiler;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    octopus::tool::loadTools(registry, TOOL_RESOURCE_ROOT);
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    std::string previous_id;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(chat.turn("execute", nullptr).hasValue());
        const auto &messages = compiler.requests.back().messages;
        const auto &result = messages.back();
        REQUIRE(result.role == octopus::llm::Role::Tool);
        auto payload = nlohmann::json::parse(result.content);
        CHECK(payload.at("output").is_object());
        CHECK(payload.at("output").at("stdout").get<std::string>().size() == 8192);
        CHECK(payload.at("output").at("stderr_truncated") == true);
        CHECK(result.tool_call_id != previous_id);
        CHECK(messages[messages.size() - 2].tool_calls[0].id == result.tool_call_id);
        previous_id = result.tool_call_id;
    }
}

TEST_CASE("CLI presents generic tool activity summaries without adding them to history") {
    Compiler compiler;
    Parser parser;
    Backend backend;
    octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
    octopus::llm::Runtime llm(model, backend);
    octopus::tool::Registry registry;
    REQUIRE(registry.add(
        {"misc", "hello", "hello", {{"type", "object"}}, {}},
        [](const auto &, const auto *) {
            return octopus::tool::Outcome{octopus::tool::Status::Success, "hidden", "Exit 2"};
        },
        validateEmpty, [](const auto &) { return "Running example"; }));
    octopus::tool::AllowAllSecurityPolicy policy;
    octopus::tool::Executor executor(registry, policy);
    octopus::chat::Runtime chat(llm, registry, executor, {});
    octopus::ChatPresentation presentation;
    presentation.capabilities.interactive_session = true;
    presentation.capabilities.cursor_control = true;
    presentation.capabilities.erase_line = true;
    presentation.capabilities.columns = 80;
    std::istringstream in("call\n");
    std::ostringstream out, err;
    CHECK(octopus::runCliChat({}, llm, in, out, err, presentation, &chat).exit_code == 0);
    CHECK(out.str().find("Running example") != std::string::npos);
    CHECK(out.str().find("Exit 2. ") != std::string::npos);
    CHECK(out.str().find("ms") != std::string::npos);
    for (const auto &message : chat.history().messages) {
        CHECK(message.content.find("Running example") == std::string::npos);
        CHECK(message.content.find("Exit 2") == std::string::npos);
    }
}

TEST_CASE("CLI opt-in tool debug traces rejected and executed calls without changing history") {
    const char *previous = std::getenv("OCTOPUS_DEBUG_TOOLS");

    struct Restore {
        std::optional<std::string> value;

        ~Restore() {
            if (value)
                setenv("OCTOPUS_DEBUG_TOOLS", value->c_str(), 1);
            else
                unsetenv("OCTOPUS_DEBUG_TOOLS");
        }
    } restore{previous ? std::optional<std::string>(previous) : std::nullopt};

    struct Calls : octopus::llm::AssistantResponseParser {
        octopus::llm::ParseResult parse(const octopus::llm::RawCompletion &raw) const override {
            if (raw.text == "call")
                return octopus::llm::ParseResult::success(
                    {"",
                     {{"", "misc.hello", {{"extra", true}}},
                      {"", "misc.hello", nlohmann::json::object()}}});
            return octopus::llm::ParseResult::success({raw.text});
        }
    } parser;

    for (bool enabled : {false, true}) {
        REQUIRE(setenv("OCTOPUS_DEBUG_TOOLS", enabled ? "1" : "0", 1) == 0);
        Compiler compiler;
        Backend backend;
        backend.generated_tokens = 7;
        octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
        octopus::llm::Runtime llm(model, backend);
        octopus::tool::Registry registry;
        REQUIRE(registry.add(
            {"misc", "hello", "hello", {{"type", "object"}, {"additionalProperties", false}}, {}},
            [](const auto &, const auto *) {
                return octopus::tool::Outcome{
                    octopus::tool::Status::Success,
                    nlohmann::json{{"exit_code", 2}, {"stderr", "bad option\n"}}};
            },
            validateEmpty));
        octopus::tool::AllowAllSecurityPolicy policy;
        octopus::tool::Executor executor(registry, policy);
        octopus::chat::Runtime chat(llm, registry, executor, {});
        std::istringstream input("call\n");
        std::ostringstream output, errors;
        CHECK(octopus::runCliChat({}, llm, input, output, errors, &chat).exit_code == 0);
        CHECK(output.str().find("bad option") == std::string::npos);
        if (!enabled) {
            CHECK(errors.str().empty());
            continue;
        }
        std::istringstream records(errors.str());
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(records, line)));
        auto generation = nlohmann::json::parse(line);
        CHECK(generation.at("event") == "generation");
        CHECK(generation.at("duration_ms").get<double>() >= 0);
        CHECK(generation.at("generated_tokens") == 7);
        CHECK(generation.at("tool_calls") == 2);
        CHECK(generation.at("round") == 1);
        CHECK(generation.at("turn") == 1);
        REQUIRE(static_cast<bool>(std::getline(records, line)));
        auto rejected = nlohmann::json::parse(line);
        CHECK(rejected.at("duration_ms").is_null());
        CHECK(rejected.at("status") == "invalid_arguments");
        CHECK(rejected.at("arguments").at("extra") == true);
        REQUIRE(static_cast<bool>(std::getline(records, line)));
        auto executed = nlohmann::json::parse(line);
        CHECK(executed.at("event") == "tool_result");
        CHECK(executed.at("duration_ms").get<double>() >= 0);
        CHECK(executed.at("round") == 1);
        CHECK(executed.at("tool") == "misc.hello");
        CHECK(executed.at("call_id") != rejected.at("call_id"));
        CHECK(executed.at("output").at("exit_code") == 2);
        CHECK(executed.at("output").at("stderr") == "bad option\n");
        REQUIRE(static_cast<bool>(std::getline(records, line)));
        auto final_generation = nlohmann::json::parse(line);
        CHECK(final_generation.at("event") == "generation");
        CHECK(final_generation.at("round") == 2);
        CHECK(final_generation.at("tool_calls") == 0);
        CHECK(final_generation.at("generated_tokens") == 7);
        CHECK_FALSE(static_cast<bool>(std::getline(records, line)));
        CHECK(chat.history().messages.size() == 5);
        for (const auto &message : chat.history().messages)
            CHECK(message.content.find("duration_ms") == std::string::npos);
    }
}

TEST_CASE("generation diagnostics retain failures and cancellation despite callback errors") {
    for (bool cancelled : {false, true}) {
        struct DiagnosticBackend : octopus::llm::InferenceBackend {
            bool cancelled = false;

            octopus::llm::InferenceResult generate(const octopus::llm::CompiledPrompt &,
                                                   const octopus::llm::GenerationOptions &,
                                                   const octopus::llm::CancellationToken *,
                                                   octopus::llm::CompletionSink *) override {
                if (cancelled)
                    return octopus::llm::InferenceResult::success(
                        {"partial", octopus::llm::FinishReason::Cancelled, 3});
                return octopus::llm::InferenceResult::failure("backend unavailable");
            }
        } backend;

        backend.cancelled = cancelled;
        Compiler compiler;
        Parser parser;
        octopus::llm::ModelIntegration model({true, false, false}, compiler, parser);
        octopus::llm::Runtime llm(model, backend);
        octopus::tool::Registry registry;
        octopus::tool::AllowAllSecurityPolicy policy;
        octopus::tool::Executor executor(registry, policy);
        octopus::chat::Runtime chat(llm, registry, executor, {});
        int notifications = 0;
        auto completion =
            chat.turn("request", nullptr, nullptr, nullptr, {},
                      [&](std::chrono::steady_clock::duration elapsed,
                          const octopus::llm::CompletionResult &result) {
                          ++notifications;
                          CHECK(elapsed >= std::chrono::steady_clock::duration::zero());
                          CHECK(result.hasValue() == cancelled);
                          if (cancelled)
                              CHECK(result.value().generated_tokens == 3);
                          else
                              CHECK(result.error() == "backend unavailable");
                          throw std::runtime_error("diagnostic failure");
                      });
        CHECK(notifications == 1);
        CHECK(completion.hasValue() == cancelled);
        if (cancelled)
            CHECK(completion.value().finish_reason == octopus::llm::FinishReason::Cancelled);
        else
            CHECK(completion.error() == "backend unavailable");
        CHECK(chat.history().messages.empty());
    }
}
