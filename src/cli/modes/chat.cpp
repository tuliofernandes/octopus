#include "octopus/cli/chat.hpp"

#include "octopus/cli/ask.hpp"
#include "octopus/cli/input_editor.hpp"
#include "octopus/cli/terminal_renderer.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <unistd.h>
#include <utility>

namespace octopus {

namespace {

const char *finishReasonName(llm::FinishReason reason) {
    switch (reason) {
    case llm::FinishReason::Stop:
        return "stop";
    case llm::FinishReason::MaxTokens:
        return "max_tokens";
    case llm::FinishReason::EndOfGeneration:
        return "end_of_generation";
    case llm::FinishReason::LoopDetected:
        return "loop_detected";
    case llm::FinishReason::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

class ToolActivity final : public tool::ActivityObserver {
  public:
    explicit ToolActivity(TranscriptRenderer &renderer) : renderer_(renderer) {}

    void started(const tool::ActivityStart &event) override {
        renderer_.updateActivity(event.label);
    }

    void finished(const tool::ActivityStart &, const tool::Result &result) override {
        std::string text =
            result.summary.value_or(result.status == tool::Status::Success     ? "Ok"
                                    : result.status == tool::Status::Cancelled ? "Cancelled"
                                                                               : "Failed");
        if (result.duration)
            text += ". " +
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(*result.duration)
                            .count()) +
                    "ms";
        renderer_.updateActivity(text);
    }

  private:
    TranscriptRenderer &renderer_;
};

constexpr const char *kUserPrompt = "> ";
constexpr const char *kContinuationPrompt = "... ";

volatile std::sig_atomic_t g_generation_cancelled = 0;

void handleGenerationSigint(int) { g_generation_cancelled = 1; }

class SignalCancellationToken final : public llm::CancellationToken {
  public:
    bool isCancellationRequested() const noexcept override { return g_generation_cancelled != 0; }
};

class ScopedGenerationCancelHandler final {
  public:
    ScopedGenerationCancelHandler()
        : previous_handler_(std::signal(SIGINT, handleGenerationSigint)) {
        g_generation_cancelled = 0;
    }

    ScopedGenerationCancelHandler(const ScopedGenerationCancelHandler &) = delete;
    ScopedGenerationCancelHandler &operator=(const ScopedGenerationCancelHandler &) = delete;

    ~ScopedGenerationCancelHandler() { std::signal(SIGINT, previous_handler_); }

    const llm::CancellationToken &token() const noexcept { return token_; }

  private:
    using SignalHandler = void (*)(int);

    SignalHandler previous_handler_;
    SignalCancellationToken token_;
};

bool completionFailed(const llm::CompletionResult &completion) { return !completion.hasValue(); }

bool completionCancelled(const llm::CompletionResult &completion) {
    return completion.hasValue() &&
           completion.value().finish_reason == llm::FinishReason::Cancelled;
}

void reportBackendError(const llm::CompletionResult &completion, std::ostream &err) {
    err << (completion.error().empty() ? "LLM backend error" : completion.error()) << '\n';
}

/**
 * Intermediate chunks can contain model tool protocol. The CLI renders only
 * the completed, parsed assistant response after the runtime finishes.
 */
class ChatOutputSink final : public llm::CompletionSink {
  public:
    void onText(const llm::CompletionChunk &) override {}
};

} /* namespace */

ChatRunResult runCliChat(const CliOptions &options, llm::Runtime &runtime, std::istream &in,
                         std::ostream &out, std::ostream &err, chat::Runtime *tool_runtime) {
    ChatPresentation presentation;
    presentation.capabilities = detectTerminalCapabilities(in, out);
    if (&out == &std::cout) {
        presentation.width_query = [] { return queryTerminalWidth(STDOUT_FILENO); };
    }
    if (&in == &std::cin && &out == &std::cout && presentation.capabilities.color &&
        !presentation.capabilities.response_background.has_value()) {
        const auto background = queryTerminalBackground(STDIN_FILENO, STDOUT_FILENO);
        if (background.has_value()) {
            presentation.capabilities.terminal_background = background;
            presentation.capabilities.response_background = deriveResponseBackground(*background);
        }
    }
    return runCliChat(options, runtime, in, out, err, std::move(presentation), tool_runtime);
}

ChatRunResult runCliChat(const CliOptions &options, llm::Runtime &runtime, std::istream &in,
                         std::ostream &out, std::ostream &err, ChatPresentation presentation,
                         chat::Runtime *tool_runtime) {
    const char *debug_setting = std::getenv("OCTOPUS_DEBUG_TOOLS");
    const bool debug_tools = debug_setting && std::string(debug_setting) == "1";
    std::size_t turn_number = 0, generation_number = 0;
    chat::ToolTrace trace;
    chat::GenerationTrace generation_trace;
    if (debug_tools) {
        generation_trace = [&](std::chrono::steady_clock::duration elapsed,
                               const llm::CompletionResult &completion) {
            nlohmann::json record = {
                {"event", "generation"},
                {"turn", turn_number},
                {"round", ++generation_number},
                {"duration_ms", std::chrono::duration<double, std::milli>(elapsed).count()}};
            if (completion.hasValue()) {
                const auto &value = completion.value();
                record["status"] =
                    value.finish_reason == llm::FinishReason::Cancelled ? "cancelled" : "success";
                record["finish_reason"] = finishReasonName(value.finish_reason);
                record["generated_tokens"] = value.generated_tokens;
                record["tool_calls"] = value.response.tool_calls.size();
            } else {
                record["status"] = "failure";
                record["error"] = completion.error();
                record["generated_tokens"] = nullptr;
                record["tool_calls"] = nullptr;
                record["finish_reason"] = nullptr;
            }
            err << record.dump() << '\n';
            err.flush();
        };
        trace = [&](const tool::Call &call, const nlohmann::json &payload,
                    std::optional<std::chrono::steady_clock::duration> duration) {
            auto record = payload;
            record["event"] = "tool_result";
            record["turn"] = turn_number;
            record["round"] = generation_number;
            record["duration_ms"] =
                duration
                    ? nlohmann::json(std::chrono::duration<double, std::milli>(*duration).count())
                    : nlohmann::json(nullptr);
            record["call_id"] = call.id;
            record["tool"] = call.identity;
            record["arguments"] = call.arguments;
            err << record.dump() << '\n';
            err.flush();
        };
    }
    ChatRunResult result;
    llm::Conversation conversation;
    const bool interactive_session = presentation.capabilities.interactive_session;
    InputEditor input_editor(in, out,
                             interactive_session
                                 ? InputEditorPrompts{kUserPrompt, kContinuationPrompt}
                                 : InputEditorPrompts{});
    TranscriptRenderer renderer(out, std::move(presentation.capabilities),
                                std::move(presentation.width_query));
    bool previous_input_cancelled = false;

    while (true) {
        const auto input = input_editor.read();
        if (input.status == InputEditorStatus::EndOfFile) {
            return result;
        }
        if (input.status == InputEditorStatus::Cancelled) {
            if (previous_input_cancelled) {
                return result;
            }
            previous_input_cancelled = true;
            continue;
        }
        previous_input_cancelled = false;

        if (input.text.empty()) {
            continue;
        }

        /**
         * The plain LLM path uses this conversation; tool_runtime owns its own
         * history, including tool calls and their correlated results.
         */
        conversation.messages.push_back({llm::Role::User, input.text});
        ChatOutputSink sink;
        llm::CompletionRequest request = makeConversationRequest(options, conversation);
        ScopedGenerationCancelHandler cancel_handler;
        request.cancellation = &cancel_handler.token();
        if (!debug_tools)
            renderer.startSpinner();
        ToolActivity activity(renderer);
        ++turn_number;
        generation_number = 0;
        auto completion = tool_runtime ? tool_runtime->turn(input.text, request.cancellation, &sink,
                                                            debug_tools ? nullptr : &activity,
                                                            trace, generation_trace)
                                       : runtime.completeStreaming(request, sink);

        if (completionFailed(completion)) {
            result.exit_code = 1;
            renderer.clearActivity();
            reportBackendError(completion, err);
            result.last_completion = std::move(completion);
            return result;
        }
        if (completionCancelled(completion)) {
            renderer.clearActivity();
            conversation.messages.pop_back();
            result.last_completion = std::move(completion);
            continue;
        }

        renderer.stopSpinner();
        renderer.renderAssistant(completion.value().response.text);
        renderer.finishTurn();
        conversation.messages.push_back({llm::Role::Assistant, completion.value().response.text});
        result.last_completion = std::move(completion);
    }
}

} /* namespace octopus */
