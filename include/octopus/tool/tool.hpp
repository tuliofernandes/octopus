#pragma once
#include "octopus/core/cancellation.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace octopus::tool {
struct Definition {
  std::string category, name, description;
  nlohmann::json parameters;
  std::vector<std::string> permissions;
  std::string identity() const { return category + "." + name; }
};
Definition loadManifest(const std::filesystem::path& root,
                        const std::string& category, const std::string& name);
struct Call {
  std::string id, identity;
  nlohmann::json arguments;
};
enum class Status {
  Success,
  UnknownTool,
  InvalidArguments,
  Denied,
  Failure,
  Cancelled
};
struct Result {
  std::string call_id;
  Status status;
  std::string output;
};
using Handler = std::function<std::string(const nlohmann::json&,
                                          const core::CancellationToken*)>;
struct Entry {
  Definition definition;
  Handler handler;
};
class Registry {
 public:
  bool add(Definition definition, Handler handler);
  const Entry* find(const std::string& identity) const;
  std::vector<Definition> definitions() const;

 private:
  std::map<std::string, Entry> entries_;
};
void registerBuiltins(Registry& registry, const std::filesystem::path& root);
struct AuthorizationDecision {
  bool allowed;
  std::string reason;
};
class SecurityPolicy {
 public:
  virtual ~SecurityPolicy() = default;
  virtual AuthorizationDecision authorize(const Definition&,
                                          const Call&) const = 0;
};
class AllowAllSecurityPolicy final : public SecurityPolicy {
 public:
  AuthorizationDecision authorize(const Definition&,
                                  const Call&) const override {
    return {true, {}};
  }
};
class Executor {
 public:
  Executor(const Registry& registry, const SecurityPolicy& policy)
      : registry_(registry), policy_(policy) {}
  Result execute(const Call& call,
                 const core::CancellationToken* cancellation = nullptr) const;

 private:
  const Registry& registry_;
  const SecurityPolicy& policy_;
};
}  // namespace octopus::tool
