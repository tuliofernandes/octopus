#include "models/alibaba/qwen3_5/integration.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace octopus::models::alibaba::qwen3_5 {
namespace {

constexpr const char *kEndOfTurn = "<|im_end|>";

bool isPolicyRole(llm::Role role) {
    return role == llm::Role::System || role == llm::Role::Developer;
}

const char *policyRoleName(llm::Role role) {
    return role == llm::Role::Developer ? "developer" : "system";
}

const char *templateRoleName(llm::Role role) {
    switch (role) {
    case llm::Role::User:
        return "user";
    case llm::Role::Assistant:
        return "assistant";
    case llm::Role::Tool:
        return "tool";
    case llm::Role::System:
    case llm::Role::Developer:
        return "system";
    }

    return "user";
}

std::string combinedPolicy(const std::vector<llm::Message> &messages) {
    std::ostringstream output;
    output << "Octopus operating instructions:\n";
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto &message = messages[index];
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

MessageCompilation makeTemplateMessages(const llm::Conversation &conversation) {
    MessageCompilation result;
    result.messages.reserve(conversation.messages.size());
    /**
     * The Qwen template receives leading system and developer instructions
     * as one system message, while retaining their role labels in the text.
     */
    std::vector<llm::Message> policy_messages;
    bool conversation_started = false;
    bool has_user_query = false;

    for (const auto &message : conversation.messages) {
        if (isPolicyRole(message.role)) {
            if (conversation_started) {
                result.error = "Qwen3.5 requires system and developer messages to be leading";
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
        result.messages.push_back({templateRoleName(message.role), message.content,
                                   message.tool_calls, message.tool_call_id});
    }

    if (!has_user_query) {
        result.messages.clear();
        result.error = "Qwen3.5 conversation requires a user query";
    }
    return result;
}

} /* namespace */

llm::CompileResult Qwen35PromptCompiler::compile(const llm::Conversation &conversation) const {
    auto compiled_messages = makeTemplateMessages(conversation);
    if (!compiled_messages.error.empty()) {
        return llm::CompileResult::failure(std::move(compiled_messages.error));
    }

    /**
     * Forward the structured messages and tool catalog to the model
     * template engine. The llama.cpp adapter converts them into native
     * template inputs; the GGUF template determines their prompt syntax.
     */
    const auto templated =
        template_engine_.render({std::move(compiled_messages.messages), true,
                                 llm::TemplateReasoningPolicy::Disabled, conversation.tools});
    if (!templated.hasValue()) {
        return llm::CompileResult::failure("Qwen3.5 chat template rendering failed: " +
                                           templated.error());
    }

    return llm::CompileResult::success({templated.value(), std::vector<std::string>{kEndOfTurn}});
}

llm::ParseResult Qwen35ResponseParser::parse(const llm::RawCompletion &completion) const {
    return parse(completion, {});
}

llm::ParseResult Qwen35ResponseParser::parse(const llm::RawCompletion &completion,
                                             const std::vector<tool::Definition> &tools) const {
    /**
     * Recognize the Qwen tool-call protocol in generated text. A tag is
     * considered only at the beginning of the output or a new line;
     * ordinary text and fenced examples remain assistant text. Once a
     * call starts, malformed or truncated protocol fails the parse rather
     * than dispatching a partially generated request. For example:
     *   <tool_call>
     *   <function=misc.hello_world>
     *   </function>
     *   </tool_call>
     * This becomes a structured call with identity "misc.hello_world"
     * and arguments {}. chat::Runtime assigns its ID before execution.
     */
    const std::string open = "<tool_call>\n<function=";
    const std::string close = "</function>\n</tool_call>";
    const auto start = completion.text.find("<tool_call>");
    if (start == std::string::npos || (start != 0 && completion.text[start - 1] != '\n')) {
        return llm::ParseResult::success({completion.text, {}});
    }
    /**
     * A protocol-looking example inside a fenced code block is assistant
     * text, not a tool request. This check counts triple-backtick sequences
     * before the first tag; it is not a complete Markdown fence parser.
     * Only the first <tool_call> occurrence is considered as the start.
     */
    std::size_t fence_count = 0;
    for (std::size_t at = 0;
         (at = completion.text.find("```", at)) != std::string::npos && at < start; at += 3)
        ++fence_count;
    if (fence_count % 2 != 0)
        return llm::ParseResult::success({completion.text, {}});
    if (completion.text.compare(start, open.size(), open) != 0) {
        return llm::ParseResult::failure("malformed tool call");
    }
    if (completion.finish_reason == llm::FinishReason::MaxTokens ||
        completion.finish_reason == llm::FinishReason::LoopDetected ||
        completion.finish_reason == llm::FinishReason::Cancelled) {
        return llm::ParseResult::failure("incomplete tool call generation");
    }
    llm::AssistantResponse response;
    response.text = completion.text.substr(0, start);
    auto cursor = start;
    while (cursor < completion.text.size()) {
        if (completion.text.compare(cursor, open.size(), open) != 0) {
            return llm::ParseResult::failure("malformed tool call");
        }
        const auto name_start = cursor + open.size();
        const auto name_end = completion.text.find(">\n", name_start);
        if (name_end == std::string::npos || name_end == name_start) {
            return llm::ParseResult::failure("incomplete or invalid tool call");
        }
        const auto name = completion.text.substr(name_start, name_end - name_start);
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos) {
            return llm::ParseResult::failure("invalid tool name");
        }
        /**
         * XML strings remain literal. Declared non-string parameter types
         * use JSON syntax, as rendered by the native Qwen template. Invalid
         * values remain strings for the executor to reject through the schema.
         */
        auto arguments = nlohmann::json::object();
        cursor = name_end + 2;
        const std::string parameter_open = "<parameter=";
        const std::string parameter_close = "\n</parameter>\n";
        while (completion.text.compare(cursor, parameter_open.size(), parameter_open) == 0) {
            const auto key_start = cursor + parameter_open.size();
            const auto key_end = completion.text.find(">\n", key_start);
            if (key_end == std::string::npos || key_end == key_start)
                return llm::ParseResult::failure("invalid tool parameter");
            const auto key = completion.text.substr(key_start, key_end - key_start);
            if (arguments.contains(key))
                return llm::ParseResult::failure("duplicate tool parameter");
            const auto value_end = completion.text.find(parameter_close, key_end + 2);
            if (value_end == std::string::npos)
                return llm::ParseResult::failure("incomplete tool parameter");
            arguments[key] = completion.text.substr(key_end + 2, value_end - key_end - 2);
            for (const auto &definition : tools) {
                if (definition.identity() != name)
                    continue;
                const auto properties = definition.parameters.find("properties");
                if (properties == definition.parameters.end() || !properties->is_object())
                    break;
                const auto property = properties->find(key);
                if (property == properties->end() || !property->is_object())
                    break;
                const auto type = property->value("type", nlohmann::json());
                if (type == "array" || type == "object" || type == "boolean" || type == "integer" ||
                    type == "number" || type == "null") {
                    auto value =
                        nlohmann::json::parse(arguments[key].get<std::string>(), nullptr, false);
                    if (!value.is_discarded())
                        arguments[key] = std::move(value);
                }
                break;
            }
            cursor = value_end + parameter_close.size();
        }
        if (completion.text.compare(cursor, close.size(), close) != 0)
            return llm::ParseResult::failure("incomplete tool call");
        /**
         * chat::Runtime assigns the call ID after parsing and correlates it
         * with the eventual tool result in conversation history.
         */
        response.tool_calls.push_back({{}, name, std::move(arguments)});
        cursor += close.size();
        while (cursor < completion.text.size() &&
               (completion.text[cursor] == '\n' || completion.text[cursor] == ' '))
            ++cursor;
    }
    return llm::ParseResult::success(std::move(response));
}

Qwen35Integration::Qwen35Integration(const llm::ChatTemplateEngine &template_engine)
    : compiler_(template_engine), parser_(),
      integration_({true, false, false}, compiler_, parser_) {}

} /* namespace octopus::models::alibaba::qwen3_5 */
