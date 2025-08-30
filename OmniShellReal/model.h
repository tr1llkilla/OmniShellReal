//model.h

#pragma once
#include "math.h"
#include "types.h"
#include "tokenizer.h"
#include <random>
#include <functional>
#include <vector>
#include <string>
#include <cstddef>
#include <optional>
#include <algorithm> // for std::max_element

namespace ai::gen {

    struct ILogitModel {
        virtual ~ILogitModel() = default;
        virtual std::vector<f32> forward_next(token_id last_token) = 0;
        virtual std::vector<f32> prefill(const std::vector<token_id>& prompt) {
            std::vector<f32> logits;
            for (size_t i = 0; i < prompt.size(); ++i)
                logits = forward_next(prompt[i]);
            return logits;
        }
        virtual size_t vocab_size() const = 0;
    };

    // NEW: minimum probability filter declaration
    void min_prob_filter(std::vector<f32>& probs, f32 min_prob);

    struct GenerationConfig {
        SamplingParams sampling{};
        i32   max_new_tokens = 128;
        token_id eos_id = -1;
        bool  allow_empty_output = false;
    };

    struct GenerationCallbacks {
        std::function<void(token_id id, const std::string& piece)> on_token;
        std::function<void(int step, const std::vector<f32>& logits)> on_logits;
        std::function<bool()> is_cancelled;
    };

    void apply_repetition_penalty(std::vector<f32>& logits,
        const std::vector<token_id>& sequence,
        f32 penalty);

    void top_p_filter(std::vector<f32>& probs, f32 top_p);

    std::vector<token_id> generate(ILogitModel& model,
        const std::vector<token_id>& prompt,
        const GenerationConfig& cfg,
        const GenerationCallbacks& cb);

    template <typename ForwardFn, typename Tokenizer, typename PieceFn>
    std::vector<token_id> generate(ForwardFn&& forward,
        const std::vector<token_id>& prompt,
        const GenerationConfig& cfg,
        const Tokenizer& tok,
        PieceFn&& on_piece,
        const GenerationCallbacks& cb = {}) {

        std::vector<token_id> seq = prompt;
        std::vector<f32> logits;
        for (size_t i = 0; i < prompt.size(); ++i) {
            logits = forward(prompt[i]);
            if (cb.on_logits) cb.on_logits(static_cast<int>(i), logits);
        }

        static thread_local std::mt19937 rng{ std::random_device{}() };
        int step = 0;

        while (step < cfg.max_new_tokens) {
            if (cb.is_cancelled && cb.is_cancelled()) break;

            std::vector<f32> probs = logits;
            if (cfg.sampling.temperature > 0.0f) {
                softmax_inplace(probs.data(), static_cast<i32>(probs.size()), cfg.sampling.temperature);
            }

            if (cfg.sampling.top_k > 0) {
                top_k_filter(probs.data(), static_cast<i32>(probs.size()), cfg.sampling.top_k);
            }
            if (cfg.sampling.top_p > 0.0f && cfg.sampling.top_p < 1.0f) {
                top_p_filter(probs, cfg.sampling.top_p);
            }
            if (cfg.sampling.repetition_penalty > 1.0f && !seq.empty()) {
                apply_repetition_penalty(probs, seq, cfg.sampling.repetition_penalty);
            }
            // ADDITION: min-prob filter
            if (cfg.sampling.min_prob > 0.0f) {
                min_prob_filter(probs, cfg.sampling.min_prob);
            }

            token_id next = 0;
            if (!cfg.sampling.do_sample || cfg.sampling.temperature <= 0.0f) {
                next = static_cast<token_id>(std::max_element(probs.begin(), probs.end()) - probs.begin());
            }
            else {
                f32 sum = 0.f;
                for (auto p : probs) sum += p;
                if (sum <= 0.f) {
                    next = static_cast<token_id>(std::max_element(probs.begin(), probs.end()) - probs.begin());
                }
                else {
                    f32 r = std::generate_canonical<f32, 24>(rng) * sum;
                    f32 c = 0.f;
                    for (size_t i = 0; i < probs.size(); ++i) {
                        c += probs[i];
                        if (r <= c) { next = static_cast<token_id>(i); break; }
                    }
                }
            }

            if (cfg.eos_id >= 0 && next == cfg.eos_id) {
                if (cfg.allow_empty_output || !seq.empty()) break;
            }

            seq.push_back(next);

            using ai::tokext::decode_piece;
            const std::string piece = decode_piece(tok, next);
            on_piece(next, piece);
            if (cb.on_token) cb.on_token(next, piece);

            logits = forward(next);
            if (cb.on_logits) cb.on_logits(static_cast<int>(prompt.size() + step), logits);
            ++step;
        }
        return seq;
    }

} // namespace ai::gen
// ==== Model data structures and public API ====

struct ModelConfig {
    i32 vocab_size = 260;
    i32 d_model = 256;
    i32 n_heads = 8;
    i32 n_layers = 2;
    i32 d_ff = 1024;
    i32 max_seq = 1024; // context window

    // [ADD] Architecture knobs (defaults match your load() setup)
    i32 mlp_kind = 0;         // 0 = ReLU, 1 = SwiGLU
    i32 norm_kind = 1;         // 0 = LayerNorm, 1 = RMSNorm
    f32 rope_theta_base = 10000.0f;  // LLaMA/Mistral default
    f32 rope_freq_scale = 1.0f;      // scaled RoPE variants
};

struct CLLFHeader {
    u32 magic;      // 'C','L','L','F' = 0x464C4C43
    u32 version;    // 1
    u32 endian;     // 1 = little
    u32 reserved;   // 0
    u32 vocab_size;
    u32 d_model;
    u32 n_heads;
    u32 n_layers;
    u32 d_ff;
    u32 max_seq;
    u32 token_kind; // 0=bytes, 1=BPE
    u32 pad[5];     // future use

    // [ADD] New config fields (present when version >= 2)
    i32 mlp_kind;        // 0 = ReLU, 1 = SwiGLU
    i32 norm_kind;       // 0 = LayerNorm, 1 = RMSNorm
    f32 rope_theta_base; // default 10000.0f
    f32 rope_freq_scale; // default 1.0f
};

struct LayerWeights {
    std::vector<f32> Wq, Wk, Wv, Wo; // [d_model x d_model]
    std::vector<f32> W1, b1;         // [d_model x d_ff], [d_ff]
    std::vector<f32> W2, b2;         // [d_ff x d_model], [d_model]
    std::vector<f32> ln1_g, ln1_b;   // [d_model]
    std::vector<f32> ln2_g, ln2_b;   // [d_model]
};

struct Weights {
    ModelConfig cfg;
    std::vector<f32> tok_emb;  // [vocab_size x d_model]
    std::vector<f32> lm_head;  // [d_model x vocab_size]
    std::vector<LayerWeights> layers;
    std::vector<f32> ln_f_g, ln_f_b; // [d_model]
};

struct KVCache {
    i32 head_dim = 0;
    i32 max_seq = 0;
    i32 n_heads = 0;

    std::vector<f32> K; // size = n_heads * max_seq * head_dim
    std::vector<f32> V; // size = n_heads * max_seq * head_dim

    inline f32* K_ptr(i32 h, i32 t) { return K.data() + (h * max_seq + t) * head_dim; }
    inline f32* V_ptr(i32 h, i32 t) { return V.data() + (h * max_seq + t) * head_dim; }
};

struct Runtime {
    Weights* W = nullptr;
    std::vector<KVCache> kv; // size = n_layers
    i32 seq_len = 0;         // current sequence length
};

struct CLLF {
    Tokenizer tok;
    Weights W;
    Runtime Rt;

    bool load(std::string_view path);
    void reset_session();
    // Prefill entire prompt, return logits for last token
    std::vector<f32> prefill(const std::vector<int>& tokens);
    // Decode one token step using KV cache, return logits
    std::vector<f32> decode_step(int token_id);

    // Sampling utilities
    int sample_argmax(const std::vector<f32>& logits) const;
    int sample_top_k_top_p(const std::vector<f32>& logits, int top_k, f32 top_p, f32 temp, std::mt19937& rng) const;

    // High-level generate
    std::string generate(std::string_view prompt, int max_new_tokens, f32 temp, int top_k, f32 top_p, bool stream);
};
