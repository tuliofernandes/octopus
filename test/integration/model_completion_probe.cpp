#include "octopus/cli/ask.hpp"
#include "octopus/inference/backend/llama_cpp_backend.hpp"
#include "octopus/llm/runtime.hpp"

#include "models/alibaba/qwen3_5/integration.hpp"
#include "models/google/gemma/integration.hpp"

#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

namespace {

using Clock = std::chrono::steady_clock;

const char* finishReasonName(octopus::llm::FinishReason reason) {
  switch (reason) {
    case octopus::llm::FinishReason::Stop:
      return "stop";
    case octopus::llm::FinishReason::MaxTokens:
      return "max_tokens";
    case octopus::llm::FinishReason::EndOfGeneration:
      return "end_of_generation";
    case octopus::llm::FinishReason::LoopDetected:
      return "loop_detected";
    case octopus::llm::FinishReason::Cancelled:
      return "cancelled";
  }

  return "unknown";
}

std::optional<int> parseGpuLayers(const std::string& value) {
  int layers = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), layers);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      layers < 0) {
    return std::nullopt;
  }
  return layers;
}

class TimedOutputSink final : public octopus::llm::CompletionSink {
 public:
  void onText(const octopus::llm::CompletionChunk& chunk) override {
    if (chunk.text.empty()) {
      return;
    }
    if (!first_text_) {
      first_text_ = Clock::now();
    }
    std::cout << chunk.text << std::flush;
  }

  std::optional<Clock::time_point> firstText() const { return first_text_; }

 private:
  std::optional<Clock::time_point> first_text_;
};

template <typename Integration>
int runProbe(const std::string& model_path, int gpu_layers,
             const std::string& prompt) {
  octopus::inference::LlamaCppBackend backend({model_path, gpu_layers, true});
  Integration integration(backend);
  octopus::llm::Runtime runtime(integration.modelIntegration(), backend);

  octopus::CliOptions options;
  options.prompt = prompt;
  options.n_gpu_layers = gpu_layers;
  TimedOutputSink sink;
  const auto started = Clock::now();
  const auto completion =
      runtime.completeStreaming(octopus::makeAskRequest(options), sink);
  const auto finished = Clock::now();

  if (!completion.hasValue()) {
    std::cerr << completion.error() << '\n';
    return 1;
  }

  std::cout << '\n';
  const auto first_text = sink.firstText().value_or(finished);
  const double ttft_ms =
      std::chrono::duration<double, std::milli>(first_text - started).count();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(finished - started).count();
  const double decode_seconds =
      std::chrono::duration<double>(finished - first_text).count();
  const int decode_tokens = completion.value().generated_tokens > 1
                                ? completion.value().generated_tokens - 1
                                : completion.value().generated_tokens;
  const double decode_rate =
      decode_seconds > 0.0 ? decode_tokens / decode_seconds : 0.0;

  std::cerr << std::fixed << std::setprecision(2) << "metrics generated_tokens="
            << completion.value().generated_tokens << " ttft_ms=" << ttft_ms
            << " elapsed_ms=" << elapsed_ms
            << " decode_tokens_per_second=" << decode_rate << " finish_reason="
            << finishReasonName(completion.value().finish_reason) << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: octopus_model_completion_probe "
                 "<qwen35|gemma> <model.gguf> <gpu-layers> <prompt>\n";
    return 2;
  }

  const auto gpu_layers = parseGpuLayers(argv[3]);
  if (!gpu_layers) {
    std::cerr << "gpu-layers must be a non-negative integer\n";
    return 2;
  }

  const std::string model = argv[1];
  if (model == "qwen35") {
    return runProbe<octopus::models::alibaba::qwen3_5::Qwen35Integration>(
        argv[2], *gpu_layers, argv[4]);
  }
  if (model == "gemma") {
    return runProbe<octopus::models::google::gemma::GemmaIntegration>(
        argv[2], *gpu_layers, argv[4]);
  }

  std::cerr << "model must be qwen35 or gemma\n";
  return 2;
}
