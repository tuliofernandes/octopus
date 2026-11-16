#include "octopus/tool/module.hpp"
#include "process.hpp"

namespace {

std::string validate(const nlohmann::json &args) {
    if (args.at("program").get<std::string>().find('\0') != std::string::npos)
        return "program contains NUL";
    for (const auto &arg : args.at("args"))
        if (arg.get<std::string>().find('\0') != std::string::npos)
            return "argument contains NUL";
    return {};
}

octopus::tool::Outcome execute(const nlohmann::json &args,
                               const octopus::core::CancellationToken *cancel) {
    return octopus::tool::os_exec::run(
        {args.at("program").get<std::string>(), args.at("args").get<std::vector<std::string>>()},
        cancel);
}

std::string label(const nlohmann::json &args) {
    std::string text = "Running command " + args.at("program").dump();
    for (const auto &arg : args.at("args"))
        text += " " + arg.dump();
    return text;
}

} /* namespace */

extern "C" const octopus::tool::ToolModule *octopusToolModule() {
    static const octopus::tool::ToolModule module{octopus::tool::kToolModuleVersion, validate,
                                                  execute, label};
    return &module;
}
