#include "octopus/core/cancellation.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace {

struct LegacyModule {
    unsigned version = 1;
    std::string (*validate)(const nlohmann::json &) = nullptr;
    std::string (*execute)(const nlohmann::json &,
                           const octopus::core::CancellationToken *) = nullptr;
};

} /* namespace */

extern "C" const LegacyModule *octopusToolModule() {
    static const LegacyModule module;
    return &module;
}
