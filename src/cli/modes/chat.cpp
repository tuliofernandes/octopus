#include "octopus/cli/chat.hpp"

#include "octopus/cli/ask.hpp"
#include "octopus/cli/input_editor.hpp"
#include "octopus/cli/terminal_renderer.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <utility>

#include <unistd.h>

namespace octopus {
namespace {

constexpr const char* kUserPrompt = "> ";
constexpr const char* kContinuationPrompt = "... ";

volatile std::sig_atomic_t g_generation_cancelled = 0;

void handleGenerationSigint(int) { g_generation_cancelled = 1; }

class SignalCancellationToken final : public llm::CancellationToken {
 public:
  bool isCancellationRequested() const noexcept override {
    return g_generation_cancelled != 0;
  }
};

class ScopedGenerationCancelHandler final {
 public:
  ScopedGenerationCancelHandler()
      : previous_handler_(std::signal(SIGINT, handleGenerationSigint)) {
    g_generation_cancelled = 0;
  }

  ScopedGenerationCancelHandler(const ScopedGenerationCancelHandler&) = delete;
  ScopedGenerationCancelHandler& operator=(
      const ScopedGenerationCancelHandler&) = delete;

  ~ScopedGenerationCancelHandler() { std::signal(SIGINT, previous_handler_); }

  const llm::CancellationToken& token() const noexcept { return token_; }

 private:
  using SignalHandler = void (*)(int);

  SignalHandler previous_handler_;
  SignalCancellationToken token_;
};

bool completionFailed(const llm::CompletionResult& completion) {
  return !completion.hasValue();
}

bool completionCancelled(const llm::CompletionResult& completion) {
  return completion.hasValue() &&
         completion.value().finish_reason == llm::FinishReason::Cancelled;
}

void reportBackendError(const llm::CompletionResult& completion,
                        std::ostream& err) {
  err << (completion.error().empty() ? "LLM backend error" : completion.error())
      << '\n';
}

class ChatOutputSink final : public llm::CompletionSink {
 public:
  void onText(const llm::CompletionChunk&) override {}
};

}  // namespace

ChatRunResult runCliChat(const CliOptions& options, llm::Runtime& runtime,
                         std::istream& in, std::ostream& out,
                         std::ostream& err) {
  ChatPresentation presentation;
  presentation.capabilities = detectTerminalCapabilities(in, out);
  if (&out == &std::cout) {
    presentation.width_query = [] { return queryTerminalWidth(STDOUT_FILENO); };
  }
  if (&in == &std::cin && &out == &std::cout &&
      presentation.capabilities.color &&
      !presentation.capabilities.response_background.has_value()) {
    const auto background =
        queryTerminalBackground(STDIN_FILENO, STDOUT_FILENO);
    if (background.has_value()) {
      presentation.capabilities.terminal_background = background;
      presentation.capabilities.response_background =
          deriveResponseBackground(*background);
    }
  }
  return runCliChat(options, runtime, in, out, err, std::move(presentation));
}

ChatRunResult runCliChat(const CliOptions& options, llm::Runtime& runtime,
                         std::istream& in, std::ostream& out, std::ostream& err,
                         ChatPresentation presentation) {
  ChatRunResult result;
  llm::Conversation conversation;
  const bool interactive_session =
      presentation.capabilities.interactive_session;
  InputEditor input_editor(
      in, out,
      interactive_session ? InputEditorPrompts{kUserPrompt, kContinuationPrompt}
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

    conversation.messages.push_back({llm::Role::User, input.text});
    ChatOutputSink sink;
    llm::CompletionRequest request =
        makeConversationRequest(options, conversation);
    ScopedGenerationCancelHandler cancel_handler;
    request.cancellation = &cancel_handler.token();
    renderer.startSpinner();
    auto completion = runtime.completeStreaming(request, sink);

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
    conversation.messages.push_back(
        {llm::Role::Assistant, completion.value().response.text});
    result.last_completion = std::move(completion);
  }
}

}  // namespace octopus
