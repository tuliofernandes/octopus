#pragma once

#include "octopus/cli/cli.hpp"
#include "octopus/inference/harness/llm.hpp"

#include <iosfwd>

namespace octopus {

// Captures both process-level outcome and model-level outcome for one-shot
// ask.
struct AskRunResult {
  int exit_code = 0;
  CompletionResult completion;
};

// Build the generic harness request for an accumulated CLI conversation. The
// overload with a profile is mainly for tests and future model selection.
CompletionRequest makeConversationRequest(const CliOptions& options,
                                          const Conversation& conversation);
CompletionRequest makeConversationRequest(const CliOptions& options,
                                          const Conversation& conversation,
                                          const ModelProfile& profile);

// Build the generic harness request for CLI ask mode. The overload with a
// profile is mainly for tests and future model selection.
CompletionRequest makeAskRequest(const CliOptions& options);
CompletionRequest makeAskRequest(const CliOptions& options,
                                 const ModelProfile& profile);

// Execute one ask request and print only the assistant-facing text. Backend
// details stay inside CompletionResult/error handling.
AskRunResult runOneShotAsk(const CliOptions& options, LlmBackend& backend,
                           std::ostream& out, std::ostream& err);

}  // namespace octopus
