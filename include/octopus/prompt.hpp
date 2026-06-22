#pragma once

#include "octopus/llm.hpp"

#include <string>
#include <vector>

namespace octopus {

struct RenderedPrompt {
  std::string text;
  std::vector<std::string> stop_strings;
};

RenderedPrompt render_gemma_prompt(const Conversation &conversation);

} // namespace octopus
