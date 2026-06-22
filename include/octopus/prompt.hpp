#pragma once

#include "octopus/llm.hpp"

#include <string>
#include <vector>

namespace octopus {

struct ChatTemplateMessage {
  const char *role = nullptr;
  const char *content = nullptr;
};

struct ChatTemplateMessages {
  std::vector<std::string> role_storage;
  std::vector<std::string> content_storage;
  std::vector<ChatTemplateMessage> messages;
};

struct RenderedPrompt {
  std::string text;
  std::vector<std::string> stop_strings;
};

ChatTemplateMessages make_chat_template_messages(const Conversation &conversation,
                                                 const ModelProfile &profile);

RenderedPrompt render_prompt(const Conversation &conversation,
                             const ModelProfile &profile);

RenderedPrompt render_gemma_prompt(const Conversation &conversation);

} // namespace octopus
