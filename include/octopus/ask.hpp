#pragma once

#include "octopus/cli.hpp"
#include "octopus/llm.hpp"

#include <iosfwd>

namespace octopus {

/** Captures both process-level outcome and model-level outcome for one-shot
 * ask. */
struct AskRunResult {
  int exit_code = 0;
  CompletionResult completion;
};

/**
 * Build the generic harness request for an accumulated CLI conversation. The
 * overload with a profile is mainly for tests and future model selection.
 */
CompletionRequest make_conversation_request(const CliOptions& options,
                                            const Conversation& conversation);
CompletionRequest make_conversation_request(const CliOptions& options,
                                            const Conversation& conversation,
                                            const ModelProfile& profile);

/**
 * Build the generic harness request for CLI ask mode. The overload with a
 * profile is mainly for tests and future model selection.
 */
CompletionRequest make_ask_request(const CliOptions& options);
CompletionRequest make_ask_request(const CliOptions& options,
                                   const ModelProfile& profile);

/**
 * Execute one ask request and print only the assistant-facing text. Backend
 * details stay inside CompletionResult/error handling.
 */
AskRunResult run_one_shot_ask(const CliOptions& options, LlmBackend& backend,
                              std::ostream& out, std::ostream& err);

}  // namespace octopus
