#include "models/google/gemma/integration.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace octopus::models::google::gemma {
namespace {

constexpr const char* kEndOfTurn = "<end_of_turn>";

const char* manualRoleName(llm::Role role) {
  switch (role) {
    case llm::Role::System:
      return "system";
    case llm::Role::Developer:
      return "developer";
    case llm::Role::User:
      return "user";
    case llm::Role::Assistant:
      return "model";
  }

  return "user";
}

const char* templateRoleName(llm::Role role) {
  switch (role) {
    case llm::Role::System:
    case llm::Role::Developer:
      return "system";
    case llm::Role::User:
      return "user";
    case llm::Role::Assistant:
      return "assistant";
  }

  return "user";
}

std::string policyPrelude(const std::vector<llm::Message>& policy_messages) {
  if (policy_messages.empty()) {
    return {};
  }

  std::ostringstream output;
  output << "Octopus operating instructions:\n";
  for (const auto& message : policy_messages) {
    output << '[' << manualRoleName(message.role) << "]\n"
           << message.content << '\n';
  }
  output << "\nUser request:\n";
  return output.str();
}

std::vector<llm::TemplateMessage> makeTemplateMessages(
    const llm::Conversation& conversation) {
  std::vector<llm::TemplateMessage> messages;
  messages.reserve(conversation.messages.size());
  std::vector<llm::Message> pending_policy;

  for (const auto& message : conversation.messages) {
    if (message.role == llm::Role::System ||
        message.role == llm::Role::Developer) {
      pending_policy.push_back(message);
      continue;
    }

    std::string content = message.content;
    if (message.role == llm::Role::User) {
      content = policyPrelude(pending_policy) + content;
      pending_policy.clear();
    }
    messages.push_back({templateRoleName(message.role), std::move(content)});
  }

  return messages;
}

void appendTurn(std::ostringstream& output, llm::Role role,
                const std::string& content) {
  output << "<start_of_turn>" << manualRoleName(role) << '\n'
         << content << kEndOfTurn << '\n';
}

llm::CompiledPrompt compileManual(const llm::Conversation& conversation) {
  std::ostringstream output;
  std::vector<llm::Message> pending_policy;

  for (const auto& message : conversation.messages) {
    if (message.role == llm::Role::System ||
        message.role == llm::Role::Developer) {
      pending_policy.push_back(message);
      continue;
    }

    if (message.role == llm::Role::User) {
      appendTurn(output, llm::Role::User,
                 policyPrelude(pending_policy) + message.content);
      pending_policy.clear();
      continue;
    }

    appendTurn(output, message.role, message.content);
  }

  output << "<start_of_turn>model\n";
  return {output.str(), {kEndOfTurn}};
}

}  // namespace

llm::CompileResult GemmaPromptCompiler::compile(
    const llm::Conversation& conversation) const {
  const auto templated =
      template_engine_.render({makeTemplateMessages(conversation), true,
                               llm::TemplateReasoningPolicy::ModelDefault});
  if (templated.hasValue()) {
    return llm::CompileResult::success({templated.value(), {kEndOfTurn}});
  }

  return llm::CompileResult::success(compileManual(conversation));
}

llm::ParseResult GemmaResponseParser::parse(
    const llm::RawCompletion& completion) const {
  return llm::ParseResult::success({completion.text});
}

GemmaIntegration::GemmaIntegration(
    const llm::ChatTemplateEngine& template_engine)
    : compiler_(template_engine),
      parser_(),
      integration_({}, compiler_, parser_) {}

}  // namespace octopus::models::google::gemma
