#include "octopus/tool/module.hpp"

#include <random>

namespace {

std::string validate(const nlohmann::json &args) {
    return args.is_object() && args.empty() ? "" : "expected an empty object";
}

octopus::tool::Outcome execute(const nlohmann::json &, const octopus::core::CancellationToken *) {
    std::random_device seed;
    std::mt19937 generator(seed());
    std::uniform_int_distribution<int> distribution(1000, 99999);
    return "Hello, world! Random number " + std::to_string(distribution(generator));
}

} /* namespace */

extern "C" const octopus::tool::ToolModule *octopusToolModule() {
    static const octopus::tool::ToolModule module{octopus::tool::kToolModuleVersion, validate,
                                                  execute};
    return &module;
}
