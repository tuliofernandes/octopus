#pragma once

#include "octopus/cli.hpp"
#include "octopus/llm.hpp"

#include <iosfwd>

namespace octopus {

struct AskRunResult {
  int exit_code = 0;
  CompletionResult completion;
};

CompletionRequest make_ask_request(const CliOptions &options);

AskRunResult run_one_shot_ask(const CliOptions &options, LlmBackend &backend,
                              std::ostream &out, std::ostream &err);

} // namespace octopus
