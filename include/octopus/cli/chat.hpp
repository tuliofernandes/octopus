#pragma once

#include "octopus/cli/cli.hpp"
#include "octopus/cli/terminal_renderer.hpp"
#include "octopus/llm/runtime.hpp"

#include <chrono>
#include <functional>
#include <iosfwd>
#include <optional>

namespace octopus {

// Captures process-level outcome and the final model completion for CLI chat.
struct ChatRunResult {
  int exit_code = 0;
  std::optional<llm::CompletionResult> last_completion;
};

struct ChatPresentation {
  TerminalCapabilities capabilities;
  TranscriptRenderer::WidthQuery width_query;
  std::function<std::chrono::steady_clock::time_point()> now =
      std::chrono::steady_clock::now;
};

// Run the progressively enhanced stdin/stdout multi-turn chat loop. Streams,
// callbacks, and backend are borrowed only for the duration of the call.
ChatRunResult runCliChat(const CliOptions& options, llm::Runtime& runtime,
                         std::istream& in, std::ostream& out,
                         std::ostream& err);
ChatRunResult runCliChat(const CliOptions& options, llm::Runtime& runtime,
                         std::istream& in, std::ostream& out, std::ostream& err,
                         ChatPresentation presentation);

}  // namespace octopus
