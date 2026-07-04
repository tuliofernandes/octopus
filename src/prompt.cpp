#include "octopus/prompt.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace octopus {
namespace {

constexpr const char *kGemmaEndOfTurn = "<end_of_turn>";

/**
 * Manual Gemma rendering uses Gemma's training-time role names. "Assistant" is
 * called "model" in that syntax, which is why the mapping is not symmetric.
 */
const char *role_name(Role role) {
  switch (role) {
  case Role::System:
    return "system";
  case Role::Developer:
    return "developer";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "model";
  }

  return "user";
}

/**
 * llama.cpp's generic chat-template API expects common chat roles. We collapse
 * Octopus policy roles to "system" and let the selected model template decide
 * how that role should appear in the final prompt.
 */
const char *chat_template_role_name(Role role) {
  switch (role) {
  case Role::System:
  case Role::Developer:
    return "system";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "assistant";
  }

  return "user";
}

/**
 * A Gemma turn is plain text with special delimiters. This is the final shape
 * the raw model was trained to continue.
 */
void append_turn(std::ostringstream &output, Role role,
                 const std::string &content) {
  output << "<start_of_turn>" << role_name(role) << '\n'
         << content << kGemmaEndOfTurn << '\n';
}

/**
 * Some models only understand user/assistant turns. Folding preserves Octopus
 * system/developer policy by embedding it into the next user message.
 */
std::string policy_prelude(const std::vector<Message> &policy_messages) {
  if (policy_messages.empty()) {
    return {};
  }

  std::ostringstream output;
  output << "Octopus operating instructions:\n";
  for (const auto &message : policy_messages) {
    output << '[' << role_name(message.role) << "]\n"
           << message.content << '\n';
  }
  output << "\nUser request:\n";
  return output.str();
}

} // namespace

ModelProfile ModelProfile::gemma_instruction() {
  ModelProfile profile;
  /**
   * Prefer the GGUF template when available because it is model-authored
   * metadata. Keep the manual Gemma renderer as a compatibility fallback.
   */
  profile.prompt_renderer = PromptRenderer::LlamaChatTemplate;
  profile.fallback_renderer = PromptFallback::GemmaInstruction;
  profile.stop_strings = {kGemmaEndOfTurn};
  profile.fold_policy_messages = true;
  return profile;
}

ModelProfile ModelProfile::llama_chat_template() {
  ModelProfile profile;
  /**
   * Strict metadata profile: useful for models where Octopus has no manual
   * renderer and should fail clearly if the GGUF template is missing.
   */
  profile.prompt_renderer = PromptRenderer::LlamaChatTemplate;
  profile.fallback_renderer = PromptFallback::None;
  return profile;
}

ChatTemplateMessages
make_chat_template_messages(const Conversation &conversation,
                            const ModelProfile &profile) {
  ChatTemplateMessages result;
  result.role_storage.reserve(conversation.messages.size());
  result.content_storage.reserve(conversation.messages.size());
  result.messages.reserve(conversation.messages.size());

  /**
   * Policy messages may need to travel inside the next user turn. Holding them
   * here keeps the generic Conversation intact while producing model-friendly
   * chat-template input.
   */
  std::vector<Message> pending_policy;

  for (const auto &message : conversation.messages) {
    if (profile.fold_policy_messages &&
        (message.role == Role::System || message.role == Role::Developer)) {
      pending_policy.push_back(message);
      continue;
    }

    result.role_storage.emplace_back(chat_template_role_name(message.role));
    if (profile.fold_policy_messages && message.role == Role::User) {
      result.content_storage.push_back(policy_prelude(pending_policy) +
                                       message.content);
      pending_policy.clear();
    } else {
    result.content_storage.push_back(message.content);
    }
  }

  /**
   * llama_chat_apply_template borrows pointers, so build the pointer views only
   * after role_storage/content_storage are fully populated.
   */
  result.messages.reserve(result.role_storage.size());
  for (std::size_t index = 0; index < result.role_storage.size(); ++index) {
    result.messages.push_back({result.role_storage[index].c_str(),
                               result.content_storage[index].c_str()});
  }

  return result;
}

RenderedPrompt render_prompt(const Conversation &conversation,
                             const ModelProfile &profile) {
  /**
   * This function is intentionally fallback-oriented. The llama.cpp metadata
   * path is handled in the backend because it needs the loaded model object.
   */
  switch (profile.prompt_renderer) {
  case PromptRenderer::GemmaInstruction: {
    auto rendered = render_gemma_prompt(conversation);
    rendered.stop_strings = profile.stop_strings;
    return rendered;
  }
  case PromptRenderer::LlamaChatTemplate:
    if (profile.fallback_renderer == PromptFallback::GemmaInstruction) {
      auto rendered = render_gemma_prompt(conversation);
      rendered.stop_strings = profile.stop_strings;
      return rendered;
    }
    return {{}, profile.stop_strings};
  }

  auto rendered = render_gemma_prompt(conversation);
  rendered.stop_strings = profile.stop_strings;
  return rendered;
}

RenderedPrompt render_gemma_prompt(const Conversation &conversation) {
  std::ostringstream output;
  /**
   * Gemma does not receive separate system/developer turns in our manual
   * format, so we queue policy until the next user request.
   */
  std::vector<Message> pending_policy;

  for (const auto &message : conversation.messages) {
    if (message.role == Role::System || message.role == Role::Developer) {
      pending_policy.push_back(message);
      continue;
    }

    if (message.role == Role::User) {
      append_turn(output, Role::User,
                  policy_prelude(pending_policy) + message.content);
      pending_policy.clear();
      continue;
    }

    append_turn(output, message.role, message.content);
  }

  /**
   * The trailing model prefix asks the raw next-token engine to continue as the
   * assistant/model, not as the user.
   */
  output << "<start_of_turn>model\n";

  return {output.str(), {kGemmaEndOfTurn}};
}

} // namespace octopus
