#pragma once

#include "octopus/cli.hpp"
#include "octopus/llm.hpp"

#include <iosfwd>

namespace octopus {

// Captures process-level outcome and the final model completion for CLI chat.
struct ChatRunResult {
  int exit_code = 0;
  CompletionResult last_completion;
};

// Run the plain stdin/stdout multi-turn chat loop. Streams and backend are
// borrowed only for the duration of the call.
ChatRunResult runCliChat(const CliOptions& options, LlmBackend& backend,
                         std::istream& in, std::ostream& out,
                         std::ostream& err);

}  // namespace octopus
