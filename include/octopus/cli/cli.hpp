#pragma once

#include <string>
#include <vector>

namespace octopus {

enum class CliMode {
  Interactive,
  Ask,
};

enum class CliModel {
  Qwen35,
  Gemma,
};

struct CliOptions {
  CliMode mode = CliMode::Interactive;
  CliModel model = CliModel::Gemma;
  std::string prompt;
  int n_predict = 512;
  int n_gpu_layers = 99;
  bool quiet = true;
};

struct CliParseResult {
  bool ok = false;
  int exit_code = 1;
  CliOptions options;
  std::string error;
  std::string help;
};

CliParseResult parseCli(const std::vector<std::string>& arguments);

}  // namespace octopus
