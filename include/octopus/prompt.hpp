#pragma once

#include "octopus/llm.hpp"

#include <string>
#include <vector>

namespace octopus {

// llama.cpp chat templates accept borrowed C strings. This lightweight view is
// paired with ChatTemplateMessages, which owns the backing strings.
struct ChatTemplateMessage {
  const char* role = nullptr;
  const char* content = nullptr;
};

// Keeps role/content storage alive while exposing const char* views to
// llama_chat_apply_template. This prevents dangling pointers during rendering.
struct ChatTemplateMessages {
  std::vector<std::string> role_storage;
  std::vector<std::string> content_storage;
  std::vector<ChatTemplateMessage> messages;
};

// A rendered prompt is the final model-facing text plus the stops that belong
// to that prompt format. The model sees text; Octopus still tracks boundaries.
struct RenderedPrompt {
  std::string text;
  std::vector<std::string> stop_strings;
};

// Convert generic Octopus messages into llama.cpp chat-template messages while
// preserving storage ownership for the borrowed role/content pointers.
ChatTemplateMessages makeChatTemplateMessages(const Conversation& conversation,
                                              const ModelProfile& profile);

// Render through the profile-selected strategy. For metadata templates this may
// intentionally return empty text so the backend can ask llama.cpp to render.
RenderedPrompt renderPrompt(const Conversation& conversation,
                            const ModelProfile& profile);

// Manual Gemma renderer used as a stable fallback when GGUF metadata is missing
// or unsupported by the linked llama.cpp version.
RenderedPrompt renderGemmaPrompt(const Conversation& conversation);

}  // namespace octopus
