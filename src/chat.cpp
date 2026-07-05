#include "octopus/chat.hpp"

#include "octopus/ask.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace octopus {
namespace {

bool completion_failed(const CompletionResult &completion) {
  return completion.finish_reason == FinishReason::BackendError;
}

void report_backend_error(const CompletionResult &completion,
                          std::ostream &err) {
  err << (completion.error.empty() ? "LLM backend error" : completion.error)
      << '\n';
}

} // namespace

ChatRunResult run_cli_chat(const CliOptions &options, LlmBackend &backend,
                           std::istream &in, std::ostream &out,
                           std::ostream &err) {
  ChatRunResult result;
  Conversation conversation;
  std::string line;

  while (true) {
    out << "you> ";
    out.flush();

    if (!std::getline(in, line)) {
      return result;
    }

    if (line.empty()) {
      continue;
    }

    conversation.messages.push_back({Role::User, line});
    result.last_completion =
        backend.complete(make_conversation_request(options, conversation));

    if (completion_failed(result.last_completion)) {
      result.exit_code = 1;
      report_backend_error(result.last_completion, err);
      return result;
    }

    out << "octopus> " << result.last_completion.text << '\n';
    conversation.messages.push_back(
        {Role::Assistant, result.last_completion.text});
  }
}

} // namespace octopus
