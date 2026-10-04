#include "models/alibaba/qwen3_5/integration.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace octopus::models::alibaba::qwen3_5 {
namespace {

constexpr const char* kEndOfTurn = "<|im_end|>";

bool isPolicyRole(llm::Role role) {
  return role == llm::Role::System || role == llm::Role::Developer;
}

const char* policyRoleName(llm::Role role) {
  return role == llm::Role::Developer ? "developer" : "system";
}

const char* templateRoleName(llm::Role role) {
  switch (role) {
    case llm::Role::User:
      return "user";
    case llm::Role::Assistant:
      return "assistant";
    case llm::Role::System:
    case llm::Role::Developer:
      return "system";
  }

  return "user";
}

std::string combinedPolicy(const std::vector<llm::Message>& messages) {
  std::ostringstream output;
  output << "Octopus operating instructions:\n";
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto& message = messages[index];
    output << '[' << policyRoleName(message.role) << "]\n" << message.content;
    if (index + 1 < messages.size()) {
      output << '\n';
    }
  }
  return output.str();
}

struct MessageCompilation {
  std::vector<llm::TemplateMessage> messages;
  std::string error;
};

MessageCompilation makeTemplateMessages(const llm::Conversation& conversation) {
  MessageCompilation result;
  result.messages.reserve(conversation.messages.size());
  std::vector<llm::Message> policy_messages;
  bool conversation_started = false;
  bool has_user_query = false;

  for (const auto& message : conversation.messages) {
    if (isPolicyRole(message.role)) {
      if (conversation_started) {
        result.error =
            "Qwen3.5 requires system and developer messages to be leading";
        return result;
      }
      policy_messages.push_back(message);
      continue;
    }

    if (!conversation_started) {
      conversation_started = true;
      if (!policy_messages.empty()) {
        result.messages.push_back({"system", combinedPolicy(policy_messages)});
      }
    }

    has_user_query = has_user_query || message.role == llm::Role::User;
    result.messages.push_back(
        {templateRoleName(message.role), message.content});
  }

  if (!has_user_query) {
    result.messages.clear();
    result.error = "Qwen3.5 conversation requires a user query";
  }
  return result;
}

}  // namespace

llm::CompileResult Qwen35PromptCompiler::compile(
    const llm::Conversation& conversation) const {
  auto compiled_messages = makeTemplateMessages(conversation);
  if (!compiled_messages.error.empty()) {
    return llm::CompileResult::failure(std::move(compiled_messages.error));
  }

  const auto templated =
      template_engine_.render({std::move(compiled_messages.messages), true,
                               llm::TemplateReasoningPolicy::Disabled});
  if (!templated.hasValue()) {
    return llm::CompileResult::failure(
        "Qwen3.5 chat template rendering failed: " + templated.error());
  }

  return llm::CompileResult::success(
      {templated.value(), std::vector<std::string>{kEndOfTurn}});
}

llm::ParseResult Qwen35ResponseParser::parse(
    const llm::RawCompletion& completion) const {
  return llm::ParseResult::success({completion.text});
}

Qwen35Integration::Qwen35Integration(
    const llm::ChatTemplateEngine& template_engine)
    : compiler_(template_engine),
      parser_(),
      integration_({}, compiler_, parser_) {}

}  // namespace octopus::models::alibaba::qwen3_5
