#pragma once

#include "octopus/core/cancellation.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace octopus::tool {
// Tool modules must be built with the same C++ ABI and dependencies as Octopus.
struct ToolModule {
  unsigned version;
  std::string (*validate)(const nlohmann::json&);
  std::string (*execute)(const nlohmann::json&, const core::CancellationToken*);
};
using ToolModuleEntry = const ToolModule* (*)();
inline constexpr unsigned kToolModuleVersion = 1;
}  // namespace octopus::tool
