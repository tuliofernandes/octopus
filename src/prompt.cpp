#include "octopus/prompt.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace octopus {
namespace {

constexpr const char *kGemmaEndOfTurn = "<end_of_turn>";

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

void append_turn(std::ostringstream &output, Role role,
                 const std::string &content) {
  output << "<start_of_turn>" << role_name(role) << '\n'
         << content << kGemmaEndOfTurn << '\n';
}

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

RenderedPrompt render_gemma_prompt(const Conversation &conversation) {
  std::ostringstream output;
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

  output << "<start_of_turn>model\n";

  return {output.str(), {kGemmaEndOfTurn}};
}

} // namespace octopus
