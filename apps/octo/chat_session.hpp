#pragma once

#include "octopus/cli/cli.hpp"
#include "octopus/llm/runtime.hpp"
#include <string>

int runChatSession(const octopus::CliOptions& options,
                   octopus::llm::Runtime& runtime, const std::string& command);
