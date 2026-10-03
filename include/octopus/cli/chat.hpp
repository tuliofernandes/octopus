#pragma once

#include "octopus/cli/cli.hpp"
#include "octopus/llm/runtime.hpp"

#include <iosfwd>
#include <optional>

namespace octopus {

// Captures process-level outcome and the final model completion for CLI chat.
struct ChatRunResult {
  int exit_code = 0;
  std::optional<llm::CompletionResult> last_completion;
};

// Run the plain stdin/stdout multi-turn chat loop. Streams and backend are
// borrowed only for the duration of the call.
ChatRunResult runCliChat(const CliOptions& options, llm::Runtime& runtime,
                         std::istream& in, std::ostream& out,
                         std::ostream& err);

}  // namespace octopus
