#include "octopus/llama_cpp_backend.hpp"

#include "octopus/completion.hpp"
#include "octopus/prompt.hpp"

#include "ggml-backend.h"
#include "llama.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace octopus {
namespace {

/**
 * BackendPromptResult carries either model-facing prompt text or a harness
 * error. Keeping prompt errors in CompletionResult keeps one error path.
 */
struct BackendPromptResult {
  RenderedPrompt rendered;
  CompletionResult error;
  bool ok = true;
};

CompletionResult backend_error(std::string message) {
  CompletionResult result;
  result.finish_reason = FinishReason::BackendError;
  result.error = std::move(message);
  return result;
}

/**
 * llama.cpp APIs use int32_t sizes in several places. Check before casting so a
 * huge prompt cannot wrap into a dangerous native call.
 */
bool fits_int32(std::size_t value) {
  return value <= static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
}

bool fits_uint32(std::size_t value) {
  return value <=
         static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
}

CompletionResult tokenize_prompt(const llama_vocab *vocab,
                                 const std::string &prompt,
                                 std::vector<llama_token> &tokens) {
  if (!fits_int32(prompt.size())) {
    return backend_error("prompt is too large to tokenize");
  }

  const auto prompt_size = static_cast<int32_t>(prompt.size());
  /**
   * First call asks llama.cpp how many tokens are needed. The negative return
   * value is a sizing convention, not an error.
   */
  const int32_t sized =
      llama_tokenize(vocab, prompt.c_str(), prompt_size, nullptr, 0, true, true);
  if (sized == std::numeric_limits<int32_t>::min() || sized >= 0) {
    return backend_error("failed to size prompt tokens");
  }

  const int32_t token_count = -sized;
  if (token_count <= 0) {
    return backend_error("prompt produced no tokens");
  }

  tokens.resize(static_cast<std::size_t>(token_count));
  /**
   * Second call fills the buffer. This two-step pattern avoids guessing token
   * capacity and keeps ownership in std::vector.
   */
  const int32_t actual = llama_tokenize(
      vocab, prompt.c_str(), prompt_size, tokens.data(), token_count, true, true);
  if (actual != token_count) {
    return backend_error("failed to tokenize prompt");
  }

  CompletionResult ok;
  return ok;
}

CompletionResult token_to_piece(const llama_vocab *vocab, llama_token token,
                                std::string &piece) {
  std::vector<char> buffer(128);
  /**
   * Token pieces are byte strings, not necessarily whole words. Stop and loop
   * detectors therefore work on accumulated text across pieces.
   */
  int32_t written = llama_token_to_piece(
      vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);

  if (written < 0) {
    const auto needed = static_cast<std::size_t>(-written);
    if (!fits_int32(needed) || needed == 0) {
      return backend_error("failed to size token piece");
    }

    buffer.resize(needed);
    written = llama_token_to_piece(vocab, token, buffer.data(),
                                   static_cast<int32_t>(buffer.size()), 0, true);
  }

  if (written < 0 ||
      static_cast<std::size_t>(written) > static_cast<std::size_t>(buffer.size())) {
    return backend_error("failed to convert token to piece");
  }

  piece.assign(buffer.data(), static_cast<std::size_t>(written));
  CompletionResult ok;
  return ok;
}

bool failed(const CompletionResult &result) {
  return result.finish_reason == FinishReason::BackendError;
}

void emit_text(CompletionSink *sink, std::string text) {
  if (sink == nullptr || text.empty()) {
    return;
  }

  sink->on_text({std::move(text)});
}

bool has_gemma_fallback(const ModelProfile &profile) {
  return profile.fallback_renderer == PromptFallback::GemmaInstruction;
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

bool looks_like_integrated_amd_gpu(const std::string &description) {
  const auto lower = lowercase(description);
  return contains(lower, "ryzen") || contains(lower, "integrated") ||
         contains(lower, "radeon graphics");
}

std::vector<ggml_backend_dev_t> select_single_gpu_device() {
  /**
   * On the local AMD host, ROCm can expose both an integrated GPU and the
   * discrete Radeon. Select one discrete-looking GPU before model load so
   * llama.cpp does not split work onto a weaker or unstable device.
   */
  ggml_backend_dev_t selected = nullptr;
  std::size_t selected_free = 0;

  for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    ggml_backend_dev_t device = ggml_backend_dev_get(index);
    if (ggml_backend_dev_type(device) != GGML_BACKEND_DEVICE_TYPE_GPU) {
      continue;
    }

    const std::string description = ggml_backend_dev_description(device);
    if (looks_like_integrated_amd_gpu(description)) {
      continue;
    }

    std::size_t free = 0;
    std::size_t total = 0;
    ggml_backend_dev_memory(device, &free, &total);
    /**
     * Free memory is a simple proxy for "best available offload target" among
     * the remaining discrete-looking GPU devices.
     */
    if (selected == nullptr || free > selected_free) {
      selected = device;
      selected_free = free;
    }
  }

  if (selected == nullptr) {
    /**
     * Returning no device intentionally forces CPU execution below. That is
     * slower, but safer than accidentally selecting the integrated GPU.
     */
    return {};
  }

  // llama.cpp expects a null-terminated device list.
  return {selected, nullptr};
}

BackendPromptResult prompt_error(std::string message) {
  BackendPromptResult result;
  result.ok = false;
  result.error = backend_error(std::move(message));
  return result;
}

BackendPromptResult manual_prompt(const CompletionRequest &request) {
  BackendPromptResult result;
  result.rendered = render_prompt(request.conversation, request.model_profile);
  return result;
}

BackendPromptResult apply_model_chat_template(const llama_model *model,
                                              const CompletionRequest &request) {
  const char *chat_template = llama_model_chat_template(model, nullptr);
  if (chat_template == nullptr) {
    /**
     * Metadata templates are preferred, but a known manual fallback keeps the
     * current Gemma path usable with older or sparse GGUF files.
     */
    if (has_gemma_fallback(request.model_profile)) {
      return manual_prompt(request);
    }
    return prompt_error(
        "model chat template metadata is unavailable and no prompt fallback is configured");
  }

  const auto template_messages =
      make_chat_template_messages(request.conversation, request.model_profile);
  std::vector<llama_chat_message> chat;
  chat.reserve(template_messages.messages.size());
  for (const auto &message : template_messages.messages) {
    chat.push_back({message.role, message.content});
  }

  /**
   * Ask llama.cpp to render according to the model's own GGUF chat template.
   * This is model-specific syntax without hardcoding that syntax in Octopus.
   */
  int32_t formatted_size =
      llama_chat_apply_template(chat_template, chat.data(), chat.size(), true,
                                nullptr, 0);
  if (formatted_size < 0) {
    if (has_gemma_fallback(request.model_profile)) {
      return manual_prompt(request);
    }
    return prompt_error("model chat template is not supported by llama.cpp");
  }

  std::vector<char> buffer(static_cast<std::size_t>(formatted_size));
  /**
   * llama_chat_apply_template can report that the buffer was too small. The
   * retry below treats that as a normal growth path.
   */
  int32_t actual = llama_chat_apply_template(
      chat_template, chat.data(), chat.size(), true,
      buffer.empty() ? nullptr : buffer.data(),
      static_cast<int32_t>(buffer.size()));
  if (actual < 0) {
    if (has_gemma_fallback(request.model_profile)) {
      return manual_prompt(request);
    }
    return prompt_error("failed to apply model chat template");
  }

  if (static_cast<std::size_t>(actual) > buffer.size()) {
    if (!fits_int32(static_cast<std::size_t>(actual))) {
      return prompt_error("rendered chat template is too large");
    }
    buffer.resize(static_cast<std::size_t>(actual));
    actual = llama_chat_apply_template(
        chat_template, chat.data(), chat.size(), true,
        buffer.empty() ? nullptr : buffer.data(),
        static_cast<int32_t>(buffer.size()));
  }

  if (actual < 0 || static_cast<std::size_t>(actual) > buffer.size()) {
    if (has_gemma_fallback(request.model_profile)) {
      return manual_prompt(request);
    }
    return prompt_error("failed to apply model chat template");
  }

  BackendPromptResult result;
  if (actual > 0) {
    result.rendered.text.assign(buffer.data(), static_cast<std::size_t>(actual));
  }
  result.rendered.stop_strings = request.model_profile.stop_strings;
  return result;
}

BackendPromptResult render_backend_prompt(const llama_model *model,
                                          const CompletionRequest &request) {
  /**
   * Metadata rendering needs the loaded llama_model, so final renderer
   * selection lives in this backend rather than pure prompt.cpp.
   */
  switch (request.model_profile.prompt_renderer) {
  case PromptRenderer::GemmaInstruction:
    return manual_prompt(request);
  case PromptRenderer::LlamaChatTemplate:
    return apply_model_chat_template(model, request);
  }

  return manual_prompt(request);
}

} // namespace

struct LlamaCppBackend::Impl {
  explicit Impl(LlamaCppBackendOptions init_options)
      : options(std::move(init_options)),
        model(nullptr, llama_model_free) {
    if (options.quiet) {
      llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    }

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = options.n_gpu_layers;
    /**
     * Device selection is done before model load because llama.cpp decides
     * tensor placement while loading the GGUF.
     */
    selected_devices = select_single_gpu_device();
    if (!selected_devices.empty()) {
      /**
       * Single-device, no-split offload keeps local HIP behavior predictable.
       */
      model_params.devices = selected_devices.data();
      model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
      model_params.main_gpu = 0;
    } else {
      model_params.n_gpu_layers = 0;
    }
    model.reset(llama_model_load_from_file(options.model_path.c_str(),
                                           model_params));
  }

  CompletionResult complete(const CompletionRequest &request) {
    return complete_impl(request, nullptr);
  }

  CompletionResult complete_streaming(const CompletionRequest &request,
                                      CompletionSink &sink) {
    return complete_impl(request, &sink);
  }

  CompletionResult complete_impl(const CompletionRequest &request,
                                 CompletionSink *sink) {
    /**
     * The adapter validates generic harness policy before translating it to
     * llama.cpp objects.
     */
    const auto validation = validate_generation_options(request.generation);
    if (!validation.ok) {
      return backend_error(validation.error);
    }

    if (model == nullptr) {
      return backend_error("unable to load model: " + options.model_path);
    }

    auto prompt_result = render_backend_prompt(model.get(), request);
    if (!prompt_result.ok) {
      return prompt_result.error;
    }
    const auto &rendered = prompt_result.rendered;
    /**
     * Prompt-format stops and caller-requested stops are both output
     * boundaries, so they are enforced by the same detector.
     */
    std::vector<std::string> stop_strings = rendered.stop_strings;
    stop_strings.insert(stop_strings.end(),
                        request.generation.stop_strings.begin(),
                        request.generation.stop_strings.end());
    StopDetector stop_detector(std::move(stop_strings));
    LoopDetector loop_detector;
    StopSafeTextBuffer text_buffer(stop_detector.stop_strings());

    const llama_vocab *vocab = llama_model_get_vocab(model.get());
    if (vocab == nullptr) {
      return backend_error("model does not expose a vocabulary");
    }

    std::vector<llama_token> prompt_tokens;
    auto tokenized = tokenize_prompt(vocab, rendered.text, prompt_tokens);
    if (failed(tokenized)) {
      return tokenized;
    }

    const auto context_tokens =
        prompt_tokens.size() +
        static_cast<std::size_t>(request.generation.max_tokens);
    /**
     * Context must fit the prompt plus the requested completion budget.
     */
    if (!fits_uint32(context_tokens) || !fits_int32(prompt_tokens.size())) {
      return backend_error("requested context is too large");
    }

    llama_context_params ctx_params = llama_context_default_params();
    /**
     * This one-shot context is sized exactly for the current request. A future
     * chat session will likely keep context alive across turns.
     */
    ctx_params.n_ctx = static_cast<uint32_t>(context_tokens);
    ctx_params.n_batch = static_cast<uint32_t>(prompt_tokens.size());
    ctx_params.no_perf = options.quiet || request.generation.quiet;

    std::unique_ptr<llama_context, decltype(&llama_free)> ctx(
        llama_init_from_model(model.get(), ctx_params), llama_free);
    if (ctx == nullptr) {
      return backend_error("failed to create llama context");
    }

    auto sampler_params = llama_sampler_chain_default_params();
    sampler_params.no_perf = options.quiet || request.generation.quiet;
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        llama_sampler_chain_init(sampler_params), llama_sampler_free);
    if (sampler == nullptr) {
      return backend_error("failed to create llama sampler");
    }
    if (repeat_penalty_enabled(request.generation)) {
      /**
       * Penalties run before greedy selection, nudging the token distribution
       * away from recent repetition while keeping deterministic output.
       */
      std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> penalties(
          llama_sampler_init_penalties(
              request.generation.repeat_last_n,
              request.generation.repeat_penalty,
              request.generation.frequency_penalty,
              request.generation.presence_penalty),
          llama_sampler_free);
      if (penalties == nullptr) {
        return backend_error("failed to create penalties sampler");
      }
      llama_sampler_chain_add(sampler.get(), penalties.release());
    }
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> greedy(
        llama_sampler_init_greedy(), llama_sampler_free);
    if (greedy == nullptr) {
      return backend_error("failed to create greedy sampler");
    }
    llama_sampler_chain_add(sampler.get(), greedy.release());

    llama_batch batch =
        llama_batch_get_one(prompt_tokens.data(),
                            static_cast<int32_t>(prompt_tokens.size()));

    CompletionResult result;
    /**
     * MaxTokens is the default until a more specific stop condition occurs.
     */
    result.finish_reason = FinishReason::MaxTokens;

    llama_token sampled_token = LLAMA_TOKEN_NULL;
    while (result.generated_tokens < request.generation.max_tokens) {
      /**
       * The first decode evaluates the full prompt. Later iterations evaluate
       * the single token sampled in the previous loop.
       */
      if (llama_decode(ctx.get(), batch)) {
        return backend_error("failed to evaluate llama batch");
      }

      sampled_token = llama_sampler_sample(sampler.get(), ctx.get(), -1);

      /**
       * EOG is the model's native "I am done" signal. StopDetector handles
       * textual protocol stops such as chat turn delimiters.
       */
      if (llama_vocab_is_eog(vocab, sampled_token)) {
        result.finish_reason = FinishReason::EndOfGeneration;
        break;
      }

      std::string piece;
      auto converted = token_to_piece(vocab, sampled_token, piece);
      if (failed(converted)) {
        return converted;
      }

      ++result.generated_tokens;

      /**
       * Stop detection happens before loop detection so a valid end-of-turn
       * marker wins over repetition heuristics.
       */
      if (stop_detector.append(piece)) {
        result.finish_reason = FinishReason::Stop;
        emit_text(sink, text_buffer.flush(stop_detector.text()));
        break;
      }

      if (loop_detector.append(piece)) {
        /**
         * LoopDetected is still a usable completion: trim repeated suffixes and
         * let the caller print the cleaned answer.
         */
        stop_detector.truncate(loop_detector.trim_size());
        result.finish_reason = FinishReason::LoopDetected;
        emit_text(sink, text_buffer.flush(stop_detector.text()));
        break;
      }

      emit_text(sink, text_buffer.append(piece));

      /**
       * Feed the sampled token back into the next decode step. This is the raw
       * autoregressive loop: predict token, append token, predict next token.
       */
      batch = llama_batch_get_one(&sampled_token, 1);
    }

    result.text = stop_detector.text();
    emit_text(sink, text_buffer.flush(result.text));
    return result;
  }

  LlamaCppBackendOptions options;
  std::vector<ggml_backend_dev_t> selected_devices;
  std::unique_ptr<llama_model, decltype(&llama_model_free)> model;
};

LlamaCppBackend::LlamaCppBackend(LlamaCppBackendOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LlamaCppBackend::~LlamaCppBackend() = default;

LlamaCppBackend::LlamaCppBackend(LlamaCppBackend &&) noexcept = default;

LlamaCppBackend &
LlamaCppBackend::operator=(LlamaCppBackend &&) noexcept = default;

CompletionResult
LlamaCppBackend::complete(const CompletionRequest &request) {
  return impl_->complete(request);
}

CompletionResult
LlamaCppBackend::complete_streaming(const CompletionRequest &request,
                                    CompletionSink &sink) {
  return impl_->complete_streaming(request, sink);
}

} // namespace octopus
