#include "octopus/inference/backend/llama_cpp_backend.hpp"

#include "octopus/llm/completion.hpp"

#include "ggml-backend.h"
#include "llama.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace octopus::inference {
namespace {

llm::InferenceResult backendError(std::string message) {
  return llm::InferenceResult::failure(std::move(message));
}

// llama.cpp APIs use int32_t sizes in several places. Check before casting so a
// huge prompt cannot wrap into a dangerous native call.
bool fitsInt32(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
}

bool fitsUint32(std::size_t value) {
  return value <=
         static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
}

std::optional<std::string> tokenizePrompt(const llama_vocab* vocab,
                                          const std::string& prompt,
                                          std::vector<llama_token>& tokens) {
  if (!fitsInt32(prompt.size())) {
    return "prompt is too large to tokenize";
  }

  const auto prompt_size = static_cast<int32_t>(prompt.size());
  // First call asks llama.cpp how many tokens are needed. The negative return
  // value is a sizing convention, not an error.
  const int32_t sized = llama_tokenize(vocab, prompt.c_str(), prompt_size,
                                       nullptr, 0, true, true);
  if (sized == std::numeric_limits<int32_t>::min() || sized >= 0) {
    return "failed to size prompt tokens";
  }

  const int32_t token_count = -sized;
  if (token_count <= 0) {
    return "prompt produced no tokens";
  }

  tokens.resize(static_cast<std::size_t>(token_count));
  // Second call fills the buffer. This two-step pattern avoids guessing token
  // capacity and keeps ownership in std::vector.
  const int32_t actual = llama_tokenize(vocab, prompt.c_str(), prompt_size,
                                        tokens.data(), token_count, true, true);
  if (actual != token_count) {
    return "failed to tokenize prompt";
  }

  return std::nullopt;
}

std::optional<std::string> tokenToPiece(const llama_vocab* vocab,
                                        llama_token token, std::string& piece) {
  std::vector<char> buffer(128);
  // Token pieces are byte strings, not necessarily whole words. Stop and loop
  // detectors therefore work on accumulated text across pieces.
  int32_t written =
      llama_token_to_piece(vocab, token, buffer.data(),
                           static_cast<int32_t>(buffer.size()), 0, true);

  if (written < 0) {
    const auto needed = static_cast<std::size_t>(-written);
    if (!fitsInt32(needed) || needed == 0) {
      return "failed to size token piece";
    }

    buffer.resize(needed);
    written =
        llama_token_to_piece(vocab, token, buffer.data(),
                             static_cast<int32_t>(buffer.size()), 0, true);
  }

  if (written < 0 || static_cast<std::size_t>(written) >
                         static_cast<std::size_t>(buffer.size())) {
    return "failed to convert token to piece";
  }

  piece.assign(buffer.data(), static_cast<std::size_t>(written));
  return std::nullopt;
}

bool cancellationRequested(const llm::CancellationToken* cancellation) {
  return cancellation != nullptr && cancellation->isCancellationRequested();
}

void emitText(llm::CompletionSink* sink, std::string text) {
  if (sink == nullptr || text.empty()) {
    return;
  }

  sink->onText({std::move(text)});
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool looksLikeIntegratedAmdGpu(const std::string& description) {
  const auto lower = lowercase(description);
  return contains(lower, "ryzen") || contains(lower, "integrated") ||
         contains(lower, "radeon graphics");
}

std::vector<ggml_backend_dev_t> selectSingleGpuDevice() {
  // On the local AMD host, ROCm can expose both an integrated GPU and the
  // discrete Radeon. Select one discrete-looking GPU before model load so
  // llama.cpp does not split work onto a weaker or unstable device.
  ggml_backend_dev_t selected = nullptr;
  std::size_t selected_free = 0;

  for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    ggml_backend_dev_t device = ggml_backend_dev_get(index);
    if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_GPU) {
      continue;
    }

    const std::string description = ggml_backend_dev_description(device);
    if (looksLikeIntegratedAmdGpu(description)) {
      continue;
    }

    std::size_t free = 0;
    std::size_t total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    // Free memory is a simple proxy for "best available offload target" among
    // the remaining discrete-looking GPU devices.
    if (selected == nullptr || free > selected_free) {
      selected = device;
      selected_free = free;
    }
  }

  if (selected == nullptr) {
    // Returning no device intentionally forces CPU execution below. That is
    // slower, but safer than accidentally selecting the integrated GPU.
    return {};
  }

  // llama.cpp expects a null-terminated device list.
  return {selected, nullptr};
}

}  // namespace

struct LlamaCppBackend::Impl {
  explicit Impl(LlamaCppBackendOptions init_options)
      : options(std::move(init_options)), model(nullptr, llama_model_free) {
    if (options.quiet) {
      llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
    }

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = options.n_gpu_layers;
    // Device selection is done before model load because llama.cpp decides
    // tensor placement while loading the GGUF.
    selected_devices = selectSingleGpuDevice();
    if (!selected_devices.empty()) {
      // Single-device, no-split offload keeps local HIP behavior predictable.
      model_params.devices = selected_devices.data();
      model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
      model_params.main_gpu = 0;
    } else {
      model_params.n_gpu_layers = 0;
    }
    model.reset(
        llama_model_load_from_file(options.model_path.c_str(), model_params));
  }

  llm::InferenceResult generate(const llm::CompiledPrompt& prompt,
                                const llm::GenerationOptions& generation,
                                const llm::CancellationToken* cancellation,
                                llm::CompletionSink* sink) {
    if (model == nullptr) {
      return backendError("unable to load model: " + options.model_path);
    }

    llm::StopDetector stop_detector(prompt.stop_strings);
    llm::LoopDetector loop_detector;
    llm::StopSafeTextBuffer text_buffer(stop_detector.stopStrings());

    const llama_vocab* vocab = llama_model_get_vocab(model.get());
    if (vocab == nullptr) {
      return backendError("model does not expose a vocabulary");
    }

    std::vector<llama_token> prompt_tokens;
    if (auto error = tokenizePrompt(vocab, prompt.text, prompt_tokens)) {
      return backendError(std::move(*error));
    }

    const auto context_tokens =
        prompt_tokens.size() + static_cast<std::size_t>(generation.max_tokens);
    // Context must fit the prompt plus the requested completion budget.
    if (!fitsUint32(context_tokens) || !fitsInt32(prompt_tokens.size())) {
      return backendError("requested context is too large");
    }

    llama_context_params ctx_params = llama_context_default_params();
    // This one-shot context is sized exactly for the current request. A future
    // chat session will likely keep context alive across turns.
    ctx_params.n_ctx = static_cast<uint32_t>(context_tokens);
    ctx_params.n_batch = static_cast<uint32_t>(prompt_tokens.size());
    ctx_params.no_perf = options.quiet || generation.quiet;

    std::unique_ptr<llama_context, decltype(&llama_free)> ctx(
        llama_init_from_model(model.get(), ctx_params), llama_free);
    if (ctx == nullptr) {
      return backendError("failed to create llama context");
    }

    auto sampler_params = llama_sampler_chain_default_params();
    sampler_params.no_perf = options.quiet || generation.quiet;
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        llama_sampler_chain_init(sampler_params), llama_sampler_free);
    if (sampler == nullptr) {
      return backendError("failed to create llama sampler");
    }
    if (llm::repeatPenaltyEnabled(generation)) {
      // Penalties run before greedy selection, nudging the token distribution
      // away from recent repetition while keeping deterministic output.
      std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> penalties(
          llama_sampler_init_penalties(
              generation.repeat_last_n, generation.repeat_penalty,
              generation.frequency_penalty, generation.presence_penalty),
          llama_sampler_free);
      if (penalties == nullptr) {
        return backendError("failed to create penalties sampler");
      }
      llama_sampler_chain_add(sampler.get(), penalties.release());
    }
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> greedy(
        llama_sampler_init_greedy(), llama_sampler_free);
    if (greedy == nullptr) {
      return backendError("failed to create greedy sampler");
    }
    llama_sampler_chain_add(sampler.get(), greedy.release());

    llama_batch batch = llama_batch_get_one(
        prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));

    llm::RawCompletion result;
    // MaxTokens is the default until a more specific stop condition occurs.
    result.finish_reason = llm::FinishReason::MaxTokens;

    llama_token sampled_token = LLAMA_TOKEN_NULL;
    while (result.generated_tokens < generation.max_tokens) {
      if (cancellationRequested(cancellation)) {
        result.finish_reason = llm::FinishReason::Cancelled;
        result.text = stop_detector.text();
        emitText(sink, text_buffer.flush(result.text));
        return llm::InferenceResult::success(std::move(result));
      }

      // The first decode evaluates the full prompt. Later iterations evaluate
      // the single token sampled in the previous loop.
      if (llama_decode(ctx.get(), batch)) {
        return backendError("failed to evaluate llama batch");
      }

      sampled_token = llama_sampler_sample(sampler.get(), ctx.get(), -1);

      // EOG is the model's native "I am done" signal. StopDetector handles
      // textual protocol stops such as chat turn delimiters.
      if (llama_vocab_is_eog(vocab, sampled_token)) {
        result.finish_reason = llm::FinishReason::EndOfGeneration;
        break;
      }

      std::string piece;
      if (auto error = tokenToPiece(vocab, sampled_token, piece)) {
        return backendError(std::move(*error));
      }

      ++result.generated_tokens;

      // Stop detection happens before loop detection so a valid end-of-turn
      // marker wins over repetition heuristics.
      if (stop_detector.append(piece)) {
        result.finish_reason = llm::FinishReason::Stop;
        emitText(sink, text_buffer.flush(stop_detector.text()));
        break;
      }

      if (loop_detector.append(piece)) {
        // LoopDetected is still a usable completion: trim repeated suffixes and
        // let the caller print the cleaned answer.
        stop_detector.truncate(loop_detector.trimSize());
        result.finish_reason = llm::FinishReason::LoopDetected;
        emitText(sink, text_buffer.flush(stop_detector.text()));
        break;
      }

      emitText(sink, text_buffer.append(piece));

      // Feed the sampled token back into the next decode step. This is the raw
      // autoregressive loop: predict token, append token, predict next token.
      batch = llama_batch_get_one(&sampled_token, 1);
    }

    result.text = stop_detector.text();
    emitText(sink, text_buffer.flush(result.text));
    return llm::InferenceResult::success(std::move(result));
  }

  llm::TemplateRenderResult render(
      const std::vector<llm::TemplateMessage>& messages) const {
    if (model == nullptr) {
      return llm::TemplateRenderResult::failure("model is unavailable");
    }

    const char* chat_template = llama_model_chat_template(model.get(), nullptr);
    if (chat_template == nullptr) {
      return llm::TemplateRenderResult::failure(
          "model chat template metadata is unavailable");
    }

    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    for (const auto& message : messages) {
      chat.push_back({message.role.c_str(), message.content.c_str()});
    }

    int32_t formatted_size = llama_chat_apply_template(
        chat_template, chat.data(), chat.size(), true, nullptr, 0);
    if (formatted_size < 0) {
      return llm::TemplateRenderResult::failure(
          "model chat template is not supported by llama.cpp");
    }

    std::vector<char> buffer(static_cast<std::size_t>(formatted_size));
    int32_t actual =
        llama_chat_apply_template(chat_template, chat.data(), chat.size(), true,
                                  buffer.empty() ? nullptr : buffer.data(),
                                  static_cast<int32_t>(buffer.size()));
    if (actual < 0) {
      return llm::TemplateRenderResult::failure(
          "failed to apply model chat template");
    }

    if (static_cast<std::size_t>(actual) > buffer.size()) {
      if (!fitsInt32(static_cast<std::size_t>(actual))) {
        return llm::TemplateRenderResult::failure(
            "rendered chat template is too large");
      }
      buffer.resize(static_cast<std::size_t>(actual));
      actual = llama_chat_apply_template(
          chat_template, chat.data(), chat.size(), true,
          buffer.empty() ? nullptr : buffer.data(),
          static_cast<int32_t>(buffer.size()));
    }

    if (actual < 0 || static_cast<std::size_t>(actual) > buffer.size()) {
      return llm::TemplateRenderResult::failure(
          "failed to apply model chat template");
    }

    std::string rendered;
    if (actual > 0) {
      rendered.assign(buffer.data(), static_cast<std::size_t>(actual));
    }
    return llm::TemplateRenderResult::success(std::move(rendered));
  }

  LlamaCppBackendOptions options;
  std::vector<ggml_backend_dev_t> selected_devices;
  std::unique_ptr<llama_model, decltype(&llama_model_free)> model;
};

LlamaCppBackend::LlamaCppBackend(LlamaCppBackendOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LlamaCppBackend::~LlamaCppBackend() = default;

llm::InferenceResult LlamaCppBackend::generate(
    const llm::CompiledPrompt& prompt, const llm::GenerationOptions& generation,
    const llm::CancellationToken* cancellation, llm::CompletionSink* sink) {
  return impl_->generate(prompt, generation, cancellation, sink);
}

llm::TemplateRenderResult LlamaCppBackend::render(
    const std::vector<llm::TemplateMessage>& messages) const {
  return impl_->render(messages);
}

}  // namespace octopus::inference
