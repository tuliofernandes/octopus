#pragma once

#include "octopus/cli/cli.hpp"
#include "octopus/llm/runtime.hpp"

#include <iosfwd>

namespace octopus {

// Captures both process-level outcome and model-level outcome for one-shot
// ask.
struct AskRunResult {
  int exit_code = 0;
  llm::CompletionResult completion;
};

// Build the generic harness request for an accumulated CLI conversation. The
// active model integration is selected only by the application composition.
llm::CompletionRequest makeConversationRequest(
    const CliOptions& options, const llm::Conversation& conversation);

// Build the model-neutral request for CLI ask mode.
llm::CompletionRequest makeAskRequest(const CliOptions& options);

// Execute one ask request and print only the assistant-facing text. Backend
// details stay inside CompletionResult/error handling.
AskRunResult runOneShotAsk(const CliOptions& options, llm::Runtime& runtime,
                           std::ostream& out, std::ostream& err);

}  // namespace octopus
