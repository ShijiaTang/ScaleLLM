#pragma once

#include <torch/torch.h>

#include "layers/activation.h"
#include "layers/attention_rope.h"
#include "layers/embedding.h"
#include "layers/linear.h"
#include "layers/normalization.h"
#include "memory/kv_cache.h"
#include "models/args.h"
#include "models/input_parameters.h"
#include "models/model_registry.h"

// Internlm model compatible with huggingface weights
namespace llm::hf {

class InternlmMLPImpl : public torch::nn::Module {
 public:
  InternlmMLPImpl(const ModelArgs& args,
                  const QuantArgs& quant_args,
                  const ParallelArgs& parallel_args,
                  torch::ScalarType dtype,
                  const torch::Device& device) {
    act_with_mul_ = Activation::get_act_with_mul_func("silu", device);
    GCHECK(act_with_mul_ != nullptr);

    const int64_t hidden_size = args.hidden_size();
    const int64_t intermediate_size = args.intermediate_size();

    // register the weight parameter
    gate_up_proj_ =
        register_module("gate_up_proj",
                        ColumnParallelLinear(hidden_size,
                                             intermediate_size * 2,
                                             /*bias=*/false,
                                             /*gather_output=*/false,
                                             quant_args,
                                             parallel_args,
                                             dtype,
                                             device));
    down_proj_ =
        register_module("down_proj",
                        RowParallelLinear(intermediate_size,
                                          hidden_size,
                                          /*bias=*/false,
                                          /*input_is_parallelized=*/true,
                                          quant_args,
                                          parallel_args,
                                          dtype,
                                          device));
  }

  torch::Tensor forward(torch::Tensor x) {
    return down_proj_(act_with_mul_(gate_up_proj_(x)));
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    gate_up_proj_->load_state_dict(state_dict, {"gate_proj.", "up_proj."});
    down_proj_->load_state_dict(state_dict.select("down_proj."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    gate_up_proj_->verify_loaded_weights(prefix + "[gate_proj,up_proj].");
    down_proj_->verify_loaded_weights(prefix + "down_proj.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear gate_up_proj_{nullptr};
  RowParallelLinear down_proj_{nullptr};

  // calculate act(x) * y
  ActFunc act_with_mul_{nullptr};
};
TORCH_MODULE(InternlmMLP);

class InternlmAttentionImpl : public torch::nn::Module {
 public:
  InternlmAttentionImpl(const ModelArgs& args,
                        const QuantArgs& quant_args,
                        const ParallelArgs& parallel_args,
                        torch::ScalarType dtype,
                        const torch::Device& device) {
    const int32_t world_size = parallel_args.world_size();
    const int64_t hidden_size = args.hidden_size();
    const int64_t n_heads = args.n_heads();
    const int64_t head_dim = hidden_size / n_heads;
    const int64_t n_local_heads = n_heads / world_size;

    // register submodules
    qkv_proj_ = register_module("qkv_proj",
                                ColumnParallelLinear(hidden_size,
                                                     3 * hidden_size,
                                                     /*bias=*/false,
                                                     /*gather_output=*/false,
                                                     quant_args,
                                                     parallel_args,
                                                     dtype,
                                                     device));

    o_proj_ = register_module("o_proj",
                              RowParallelLinear(hidden_size,
                                                hidden_size,
                                                /*bias=*/false,
                                                /*input_is_parallelized=*/true,
                                                quant_args,
                                                parallel_args,
                                                dtype,
                                                device));

    // initialize attention
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim));
    atten_ = register_module("atten",
                             AttentionWithRoPE(n_local_heads,
                                               n_local_heads,
                                               head_dim,
                                               scale,
                                               /*rotary_dim=*/head_dim,
                                               args.rope_scaling(),
                                               args.rope_theta(),
                                               args.max_position_embeddings(),
                                               /*interleaved=*/false,
                                               dtype,
                                               device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    // (num_tokens, dim) x (dim, n_local_heads * head_dim)
    // => (num_tokens, n_local_heads * head_dim)
    auto qkv = qkv_proj_(x).chunk(/*chunks=*/3, /*dim=*/1);

    // calculate attention, output: (num_tokens, n_local_heads * head_dim)
    auto output =
        atten_(qkv[0], qkv[1], qkv[2], positions, kv_cache, input_params);
    return o_proj_(output);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    qkv_proj_->load_state_dict(state_dict, {"q_proj.", "k_proj.", "v_proj."});
    o_proj_->load_state_dict(state_dict.select("o_proj."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    qkv_proj_->verify_loaded_weights(prefix + "[q_proj,k_proj,v_proj].");
    o_proj_->verify_loaded_weights(prefix + "o_proj.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear qkv_proj_{nullptr};

  RowParallelLinear o_proj_{nullptr};

  // module members without parameters
  AttentionWithRoPE atten_{nullptr};
};
TORCH_MODULE(InternlmAttention);

class InternlmDecoderLayerImpl : public torch::nn::Module {
 public:
  InternlmDecoderLayerImpl(const ModelArgs& args,
                           const QuantArgs& quant_args,
                           const ParallelArgs& parallel_args,
                           torch::ScalarType dtype,
                           const torch::Device& device) {
    // register submodules
    self_attn_ = register_module(
        "self_attn",
        InternlmAttention(args, quant_args, parallel_args, dtype, device));
    mlp_ = register_module(
        "mlp", InternlmMLP(args, quant_args, parallel_args, dtype, device));
    input_layernorm_ = register_module(
        "input_layernorm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
    post_attention_layernorm_ = register_module(
        "post_attention_layernorm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    auto h =
        x + self_attn_(input_layernorm_(x), positions, kv_cache, input_params);
    return h + mlp_(post_attention_layernorm_(h));
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    self_attn_->load_state_dict(state_dict.select("self_attn."));
    mlp_->load_state_dict(state_dict.select("mlp."));
    input_layernorm_->load_state_dict(state_dict.select("input_layernorm."));
    post_attention_layernorm_->load_state_dict(
        state_dict.select("post_attention_layernorm."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    self_attn_->verify_loaded_weights(prefix + "self_attn.");
    mlp_->verify_loaded_weights(prefix + "mlp.");
    input_layernorm_->verify_loaded_weights(prefix + "input_layernorm.");
    post_attention_layernorm_->verify_loaded_weights(
        prefix + "post_attention_layernorm.");
  }

 private:
  // parameter members, must be registered
  InternlmAttention self_attn_{nullptr};

  InternlmMLP mlp_{nullptr};

  RMSNorm input_layernorm_{nullptr};

  RMSNorm post_attention_layernorm_{nullptr};
};
TORCH_MODULE(InternlmDecoderLayer);

class InternlmModelImpl : public torch::nn::Module {
 public:
  InternlmModelImpl(const ModelArgs& args,
                    const QuantArgs& quant_args,
                    const ParallelArgs& parallel_args,
                    torch::ScalarType dtype,
                    const torch::Device& device) {
    // register submodules
    embed_tokens_ = register_module("embed_tokens",
                                    ParallelEmbedding(args.vocab_size(),
                                                      args.hidden_size(),
                                                      parallel_args,
                                                      dtype,
                                                      device));
    blocks_ = register_module("layers", torch::nn::ModuleList());
    layers_.reserve(args.n_layers());
    for (int32_t i = 0; i < args.n_layers(); i++) {
      auto block =
          InternlmDecoderLayer(args, quant_args, parallel_args, dtype, device);
      layers_.push_back(block);
      blocks_->push_back(block);
    }
    norm_ = register_module(
        "norm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = embed_tokens_(tokens);
    for (size_t i = 0; i < layers_.size(); i++) {
      auto& layer = layers_[i];
      h = layer(h, positions, kv_caches[i], input_params);
    }
    return norm_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    embed_tokens_->load_state_dict(state_dict.select("embed_tokens."));
    // call each layer's load_state_dict function
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->load_state_dict(
          state_dict.select("layers." + std::to_string(i) + "."));
    }
    norm_->load_state_dict(state_dict.select("norm."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    embed_tokens_->verify_loaded_weights(prefix + "embed_tokens.");
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->verify_loaded_weights(prefix + "layers." + std::to_string(i) +
                                        ".");
    }
    norm_->verify_loaded_weights(prefix + "norm.");
  }

 private:
  // parameter members, must be registered
  ParallelEmbedding embed_tokens_{nullptr};

  torch::nn::ModuleList blocks_{nullptr};
  // hold same data but different type as blocks_ to avoid type cast
  std::vector<InternlmDecoderLayer> layers_;

  RMSNorm norm_{nullptr};
};
TORCH_MODULE(InternlmModel);

class InternlmForCausalLMImpl : public torch::nn::Module {
 public:
  InternlmForCausalLMImpl(const ModelArgs& args,
                          const QuantArgs& quant_args,
                          const ParallelArgs& parallel_args,
                          torch::ScalarType dtype,
                          const torch::Device& device) {
    // register submodules
    model_ = register_module(
        "model", InternlmModel(args, quant_args, parallel_args, dtype, device));

    lm_head_ = register_module("lm_head",
                               ColumnParallelLinear(args.hidden_size(),
                                                    args.vocab_size(),
                                                    /*bias=*/false,
                                                    /*gather_output=*/true,
                                                    parallel_args,
                                                    dtype,
                                                    device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = model_(tokens, positions, kv_caches, input_params);
    // select last token for each sequence
    h = h.index_select(/*dim=*/0, input_params.last_token_idxes);
    return lm_head_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    model_->load_state_dict(state_dict.select("model."));
    lm_head_->load_state_dict(state_dict.select("lm_head."));
  }

  void verify_loaded_weights() const {
    model_->verify_loaded_weights("model.");
    lm_head_->verify_loaded_weights("lm_head.");
  }

 private:
  // parameter members, must be registered
  InternlmModel model_{nullptr};

  ColumnParallelLinear lm_head_{nullptr};
};
TORCH_MODULE(InternlmForCausalLM);

class InternlmDialog final : public Conversation {
 public:
  // generate prompt from dialogs
  // Prompt template:
  // <s><|User|>:{messages[0]}<eoh>\n<|Bot|>:{messages[1]}<eoa>\n
  std::optional<std::string> get_prompt() const override {
    // at least one user message
    if (messages_.size() % 2 == 0) {
      return std::nullopt;
    }

    std::stringstream ss;
    // start with system message
    if (!system_message_.empty()) {
      ss << system_message_;
    }
    // then user and assistant message pairs (u/a/u/a/u...)
    for (size_t i = 0; i < messages_.size(); ++i) {
      if (i % 2 == 0) {
        // user message
        ss << "<s><|User|>:" << messages_[i] << "<eoh>\n";
      } else {
        // assistant message
        ss << "<|Bot|>:" << messages_[i] << "<eoa>\n";
      }
    }
    // end with assistant message
    ss << "<|Bot|>:";
    return ss.str();
  }
};

// register the model to make it available
REGISTER_CAUSAL_MODEL(internlm, InternlmForCausalLM);
REGISTER_CONVERSATION_TEMPLATE(internlm, InternlmDialog);
REGISTER_MODEL_ARGS(internlm, [&] {
  LOAD_ARG_OR(model_type, "model_type", "internlm");
  LOAD_ARG_OR(dtype, "torch_dtype", "");
  LOAD_ARG_OR(vocab_size, "vocab_size", 103168);
  LOAD_ARG_OR(hidden_size, "hidden_size", 5120);
  LOAD_ARG_OR(n_layers, "num_hidden_layers", 60);
  LOAD_ARG_OR(n_heads, "num_attention_heads", 40);
  LOAD_ARG(n_kv_heads, "num_key_value_heads");
  LOAD_ARG_OR(intermediate_size, "intermediate_size", 13824);
  LOAD_ARG_OR(max_position_embeddings, "max_position_embeddings", 4096);
  LOAD_ARG_OR(rms_norm_eps, "rms_norm_eps", 1e-6);
  LOAD_ARG_OR(bos_token_id, "bos_token_id", 1);
  LOAD_ARG_OR(eos_token_id, "eos_token_id", 2);
  LOAD_ARG_OR(hidden_act, "hidden_act", "silu");
  LOAD_ARG_OR(rope_theta, "rope_theta", 10000.0f);

  // stop token ids: [1, 103028]
  SET_ARG(stop_token_ids, std::unordered_set<int32_t>({1, 103028}));
});

}  // namespace llm::hf
