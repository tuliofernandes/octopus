#include "octopus/tool/tool.hpp"
#include <exception>
#include <random>
#include <regex>
#include <stdexcept>
#include <utility>
namespace octopus::tool {
bool Registry::add(Definition definition, Handler handler) {
  const nlohmann::json schema = {{"type", "object"},
                                 {"properties", nlohmann::json::object()},
                                 {"additionalProperties", false}};
  static const std::regex identifier("[a-z][a-z0-9_]*");
  if (!handler || !std::regex_match(definition.category, identifier) ||
      !std::regex_match(definition.name, identifier) ||
      definition.description.empty() || definition.parameters != schema)
    return false;
  const auto identity = definition.identity();
  return entries_
      .emplace(identity, Entry{std::move(definition), std::move(handler)})
      .second;
}
const Entry* Registry::find(const std::string& identity) const {
  const auto it = entries_.find(identity);
  return it == entries_.end() ? nullptr : &it->second;
}
std::vector<Definition> Registry::definitions() const {
  std::vector<Definition> definitions;
  for (const auto& entry : entries_)
    definitions.push_back(entry.second.definition);
  return definitions;
}
void registerBuiltins(Registry& registry, const std::filesystem::path& root) {
  auto definition = loadManifest(root, "misc", "hello_world");
  if (!registry.add(std::move(definition), [](const nlohmann::json&,
                                              const core::CancellationToken*) {
        std::random_device seed;
        std::mt19937 generator(seed());
        std::uniform_int_distribution<int> distribution(1000, 99999);
        return "Hello, world! Random number " +
               std::to_string(distribution(generator));
      }))
    throw std::runtime_error("failed to register misc.hello_world");
}
Result Executor::execute(const Call& call,
                         const core::CancellationToken* cancellation) const {
  const auto* entry = registry_.find(call.identity);
  if (!entry) return {call.id, Status::UnknownTool, "unknown tool"};
  if (!call.arguments.is_object() || !call.arguments.empty())
    return {call.id, Status::InvalidArguments, "expected an empty object"};
  if (cancellation && cancellation->isCancellationRequested())
    return {call.id, Status::Cancelled, "cancelled"};
  try {
    auto decision = policy_.authorize(entry->definition, call);
    if (!decision.allowed)
      return {call.id, Status::Denied, std::move(decision.reason)};
    if (cancellation && cancellation->isCancellationRequested())
      return {call.id, Status::Cancelled, "cancelled"};
    return {call.id, Status::Success,
            entry->handler(call.arguments, cancellation)};
  } catch (const std::exception& error) {
    return {call.id, Status::Failure, error.what()};
  }
}
}  // namespace octopus::tool
