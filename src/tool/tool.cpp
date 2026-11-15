#include "octopus/tool/tool.hpp"

#include <exception>
#include <regex>
#include <stdexcept>
#include <utility>

namespace octopus::tool {
bool Registry::add(Definition definition, Handler handler, ArgumentValidator validator) {
    static const std::regex identifier("[a-z][a-z0-9_]*");
    if (!handler || !validator || !std::regex_match(definition.category, identifier) ||
        !std::regex_match(definition.name, identifier) || definition.description.empty() ||
        !definition.parameters.is_object() ||
        definition.parameters.value("type", nlohmann::json()) != "object")
        return false;
    const auto identity = definition.identity();
    return entries_
        .emplace(identity, Entry{std::move(definition), std::move(handler), std::move(validator)})
        .second;
}

const Entry *Registry::find(const std::string &identity) const {
    const auto it = entries_.find(identity);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<Definition> Registry::definitions() const {
    std::vector<Definition> definitions;
    for (const auto &entry : entries_)
        definitions.push_back(entry.second.definition);
    return definitions;
}

Result Executor::execute(const Call &call, const core::CancellationToken *cancellation) const {
    const auto *entry = registry_.find(call.identity);
    if (!entry)
        return {call.id, Status::UnknownTool, "unknown tool"};
    if (cancellation && cancellation->isCancellationRequested())
        return {call.id, Status::Cancelled, "cancelled"};
    try {
        auto diagnostic = entry->validator(call.arguments);
        if (!diagnostic.empty())
            return {call.id, Status::InvalidArguments, std::move(diagnostic)};
        auto decision = policy_.authorize(entry->definition, call);
        if (!decision.allowed)
            return {call.id, Status::Denied, std::move(decision.reason)};
        if (cancellation && cancellation->isCancellationRequested())
            return {call.id, Status::Cancelled, "cancelled"};
        return {call.id, Status::Success, entry->handler(call.arguments, cancellation)};
    } catch (const std::exception &error) {
        return {call.id, Status::Failure, error.what()};
    }
}
} // namespace octopus::tool
