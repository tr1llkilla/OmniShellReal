Copyright © 2025 Cadell Richard Anderson

// model.cpp 
#include "model.h"
#include "math.h"  // added for softmax_inplace, top_k_filter
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>
#include <iostream>

namespace ai::gen {

    void apply_repetition_penalty(std::vector<f32>& vals,
        const std::vector<token_id>& sequence,
        f32 penalty) {
        if (penalty <= 1.0f || sequence.empty()) return;
        for (token_id id : sequence) {
            if (id >= 0 && static_cast<size_t>(id) < vals.size()) {
                vals[static_cast<size_t>(id)] /= penalty;
            }
        }
    }

    // ADDITION: proper out-of-line min_prob_filter definition
    void min_prob_filter(std::vector<f32>& probs, f32 min_prob) {
        if (min_prob <= 0.0f) return;
        for (auto& p : probs) {
            if (p < min_prob) p = 0.0f;
        }
        f32 sum = 0.f;
        for (auto p : probs) sum += p;
        if (sum > 0.f) {
            const f32 inv = 1.0f / sum;
            for (auto& p : probs) p *= inv;
        }
    }

    void top_p_filter(std::vector<f32>& probs, f32 top_p) {
        if (top_p <= 0.0f || top_p >= 1.0f) return;
        const size_t V = probs.size();
        std::vector<size_t> idx(V);
        for (size_t i = 0; i < V; ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return probs[a] > probs[b];
            });
        f32 cum = 0.f;
        size_t cutoff = 0;
        for (; cutoff < V; ++cutoff) {
            cum += probs[idx[cutoff]];
            if (cum >= top_p) { ++cutoff; break; }
        }
        for (size_t i = cutoff; i < V; ++i) probs[idx[i]] = 0.0f;
        f32 sum = 0.f;
        for (auto p : probs) sum += p;
        if (sum > 0.f) {
            const f32 inv = 1.0f / sum;
            for (auto& p : probs) p *= inv;
        }
    }

    // Non-template generate driving an ILogitModel — matches model.h decl
    std::vector<token_id> generate(ILogitModel& model,
        const std::vector<token_id>& prompt,
        const GenerationConfig& cfg,
        const GenerationCallbacks& cb) {

        std::vector<token_id> seq = prompt;
        std::vector<f32> logits = model.prefill(prompt);
        if (cb.on_logits) cb.on_logits(static_cast<int>(prompt.size()) - 1, logits);

        std::mt19937 rng{ std::random_device{}() };

        for (int step = 0; step < cfg.max_new_tokens; ++step) {
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
            if (cfg.sampling.repetition_penalty > 1.0f) {
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
                f32 sum = 0.f; for (auto p : probs) sum += p;
                if (sum <= 0.f) {
                    next = static_cast<token_id>(std::max_element(probs.begin(), probs.end()) - probs.begin());
                }
                else {
                    std::uniform_real_distribution<f32> dist(0.f, sum);
                    f32 r = dist(rng), c = 0.f;
                    for (size_t i = 0; i < probs.size(); ++i) { c += probs[i]; if (r <= c) { next = (token_id)i; break; } }
                }
            }

            if (cfg.eos_id >= 0 && next == cfg.eos_id) {
                if (cfg.allow_empty_output || !seq.empty()) break;
            }

            seq.push_back(next);
            if (cb.on_token) cb.on_token(next, {});

            logits = model.forward_next(next);
            if (cb.on_logits) cb.on_logits(static_cast<int>(prompt.size() + step), logits);
        }
        return seq;
    }

} // namespace ai::gen

namespace {
    constexpr u32 CLLF_MAGIC = 0x464C4C43; // 'C''L''L''F'

    inline int clamp_int(int v, int lo, int hi) {
        return std::max(lo, std::min(hi, v));
    }

    std::vector<f32> softmax_scaled(const std::vector<f32>& logits, f32 temp) {
        const f32 t = std::max<f32>(temp, 1e-6f);
        std::vector<f32> probs(logits.size());
        f32 mx = -std::numeric_limits<f32>::infinity();
        for (f32 x : logits) mx = std::max(mx, x);
        f32 sum = 0.0f;
        for (size_t i = 0; i < logits.size(); ++i) {
            f32 e = std::exp((logits[i] - mx) / t);
            probs[i] = e;
            sum += e;
        }
        if (sum > 0.0f) {
            for (f32& p : probs) p /= sum;
        }
        return probs;
    }

    void alloc_minimal_weights(Weights& W) {
        const i32 V = std::max<i32>(W.cfg.vocab_size, 2);
        const i32 D = std::max<i32>(W.cfg.d_model, 2);
        const i32 L = std::max<i32>(W.cfg.n_layers, 1);
        const i32 FF = std::max<i32>(W.cfg.d_ff, 2);

        // ADD: Decide W1 columns based on mlp_kind (0 = ReLU, 1 = SwiGLU)
        const bool use_swiglu = (W.cfg.mlp_kind == 1);
        const i32 W1_cols = use_swiglu ? (2 * FF) : FF;

        W.tok_emb.assign(static_cast<size_t>(V) * D, 0.0f);
        W.lm_head.assign(static_cast<size_t>(D) * V, 0.0f);
        W.ln_f_g.assign(D, 1.0f);
        W.ln_f_b.assign(D, 0.0f);

        W.layers.resize(L);
        for (i32 l = 0; l < L; ++l) {
            auto& ly = W.layers[l];
            ly.Wq.assign(static_cast<size_t>(D) * D, 0.0f);
            ly.Wk.assign(static_cast<size_t>(D) * D, 0.0f);
            ly.Wv.assign(static_cast<size_t>(D) * D, 0.0f);
            ly.Wo.assign(static_cast<size_t>(D) * D, 0.0f);
            // ADD: Use W1_cols instead of FF for SwiGLU
            ly.W1.assign(static_cast<size_t>(D) * W1_cols, 0.0f);
            ly.b1.assign(static_cast<size_t>(W1_cols), 0.0f);
            ly.W2.assign(static_cast<size_t>(FF) * D, 0.0f);
            ly.b2.assign(static_cast<size_t>(D), 0.0f);
            ly.ln1_g.assign(D, 1.0f);
            ly.ln1_b.assign(D, 0.0f);
            ly.ln2_g.assign(D, 1.0f);
            ly.ln2_b.assign(D, 0.0f);
        }
    }

    void alloc_kv(Runtime& Rt) {
        const i32 D = std::max<i32>(Rt.W->cfg.d_model, 2);
        const i32 H = std::max<i32>(Rt.W->cfg.n_heads, 1);
        const i32 T = std::max<i32>(Rt.W->cfg.max_seq, 8);
        const i32 hd = std::max<i32>(D / H, 1);

        Rt.kv.resize(std::max<i32>(Rt.W->cfg.n_layers, 1));
        for (auto& kv : Rt.kv) {
            kv.head_dim = hd;
            kv.max_seq = T;
            kv.n_heads = H;
            kv.K.assign(static_cast<size_t>(H) * T * hd, 0.0f);
            kv.V.assign(static_cast<size_t>(H) * T * hd, 0.0f);
        }
        Rt.seq_len = 0;
    }

    // --- Backend-agnostic GEMM hook (swap later with ggml/BLAS) ---
    inline void gemm_mm(const f32* A, const f32* B, f32* C, i32 M, i32 K, i32 N) {
        // Default to your existing matmul
        matmul(A, B, C, M, K, N);
    }

    // --- RMSNorm (Gemma/Mistral style) ---
    inline void rmsnorm_row(f32* x, const f32* weight, i32 d, f32 eps = 1e-6f) {
        f64 ss = 0.0;
        for (i32 i = 0; i < d; ++i) ss += f64(x[i]) * f64(x[i]);
        const f32 inv = 1.0f / std::sqrt(static_cast<f32>(ss / d) + eps);
        for (i32 i = 0; i < d; ++i) x[i] = (x[i] * inv) * (weight ? weight[i] : 1.0f);
    }

    // --- SwiGLU (expects two parallel projections) ---
    inline f32 sigmoidf(f32 x) { return 1.0f / (1.0f + std::exp(-x)); }

    inline void swiglu_pointwise(const std::vector<f32>& a,
        const std::vector<f32>& b,
        std::vector<f32>& out) {
        const i32 n = static_cast<i32>(a.size());
        out.resize(n);
        for (i32 i = 0; i < n; ++i) {
            out[i] = a[i] * sigmoidf(b[i]); // common gate form
        }
    }

    // --- Rotary Positional Embedding (RoPE) on a single head slice ---
    inline void rope_apply_head_scaled(f32* q_head, f32* k_head, i32 head_dim, i32 pos, f32 theta_base, f32 freq_scale) {
        const i32 half = head_dim / 2;
        for (i32 i = 0; i < half; ++i) {
            const f32 inv_freq = std::pow(theta_base, -2.0f * i / static_cast<f32>(head_dim));
            const f32 ang = (pos * freq_scale) * inv_freq;
            const f32 c = std::cos(ang);
            const f32 s = std::sin(ang);

            f32 q0 = q_head[i];
            f32 q1 = q_head[i + half];
            f32 k0 = k_head[i];
            f32 k1 = k_head[i + half];

            q_head[i] = q0 * c - q1 * s;
            q_head[i + half] = q0 * s + q1 * c;
            k_head[i] = k0 * c - k1 * s;
            k_head[i + half] = k0 * s + k1 * c;
        }
    }

    inline void rope_apply_all_heads_scaled(std::vector<f32>& Q, std::vector<f32>& K, i32 H, i32 hd, i32 pos, f32 theta_base, f32 freq_scale) {
        for (i32 h = 0; h < H; ++h) {
            rope_apply_head_scaled(&Q[h * hd], &K[h * hd], hd, pos, theta_base, freq_scale);
        }
    }
} // namespace

bool CLLF::load(std::string_view path) {
    // Reset weights/runtime to defaults
    W = Weights{};
    Rt = Runtime{};
    W.cfg = ModelConfig{};

    // Initialise architecture knobs with defaults
    W.cfg.mlp_kind = 0;            // 0 = ReLU, 1 = SwiGLU
    W.cfg.norm_kind = 1;            // 0 = LayerNorm, 1 = RMSNorm
    W.cfg.rope_theta_base = 10000.0f;     // LLaMA/Mistral default
    W.cfg.rope_freq_scale = 1.0f;         // optional scaled RoPE

    // Best-effort: if file looks like a CLLF header, adopt config
    if (!path.empty()) {
        std::ifstream fin(std::string(path), std::ios::binary);
        if (fin) {
            CLLFHeader h{};
            fin.read(reinterpret_cast<char*>(&h), sizeof(h));
            if (fin && h.magic == CLLF_MAGIC && h.version >= 1 && h.endian == 1) { // [UPDATED] >=1 for forward compat
                W.cfg.vocab_size = static_cast<i32>(h.vocab_size);
                W.cfg.d_model = static_cast<i32>(h.d_model);
                W.cfg.n_heads = static_cast<i32>(h.n_heads);
                W.cfg.n_layers = static_cast<i32>(h.n_layers);
                W.cfg.d_ff = static_cast<i32>(h.d_ff);
                W.cfg.max_seq = static_cast<i32>(h.max_seq);

                // [ADDED] Only override defaults for new fields if header contains them (version >= 2)
                if (h.version >= 2) {
                    W.cfg.mlp_kind = h.mlp_kind;
                    W.cfg.norm_kind = h.norm_kind;
                    W.cfg.rope_theta_base = h.rope_theta_base;
                    W.cfg.rope_freq_scale = h.rope_freq_scale;
                }
                // else: keep defaults set at top of this function
            }
        }
    }

    alloc_minimal_weights(W);
    Rt.W = &W;
    alloc_kv(Rt);
    return true; // stub always "loads"
}

void CLLF::reset_session() {
    if (!Rt.W) Rt.W = &W;
    Rt.seq_len = 0;
    for (auto& kv : Rt.kv) {
        std::fill(kv.K.begin(), kv.K.end(), 0.0f);
        std::fill(kv.V.begin(), kv.V.end(), 0.0f);
    }
}

std::vector<f32> CLLF::prefill(const std::vector<int>& tokens) {
    if (!Rt.W) Rt.W = &W;
    const i32 V = std::max<i32>(W.cfg.vocab_size, 2);
    const i32 D = W.cfg.d_model;
    const i32 H = W.cfg.n_heads;
    const i32 hd = std::max<i32>(D / H, 1);
    const i32 L = std::max<i32>(W.cfg.n_layers, 1);
    const i32 T = std::max<i32>(W.cfg.max_seq, 8);

    const i32 N = std::min<i32>((i32)tokens.size(), T);
    Rt.seq_len = 0; // we are going to fill from 0..N-1

    std::vector<f32> x(D, 0.0f);

    for (i32 pos = 0; pos < N; ++pos) {
        // 1) embed
        std::fill(x.begin(), x.end(), 0.0f);
        const int tok = tokens[pos];
        if (tok >= 0 && tok < V) {
            std::copy_n(&W.tok_emb[tok * D], D, x.begin());
        }

        // 2) pass through all layers, filling KV at pos
        for (i32 l = 0; l < L; ++l) {
            const LayerWeights& ly = W.layers[l];
            KVCache& kv = Rt.kv[l];

            // Pre-attn norm
            if (W.cfg.norm_kind == 1)
                rmsnorm_row(x.data(), ly.ln1_g.data(), D);
            else
                layernorm_row(x.data(), ly.ln1_g.data(), ly.ln1_b.data(), D);

            // QKV projections
            std::vector<f32> Q(D), K(D), Vv(D);
            gemm_mm(x.data(), ly.Wq.data(), Q.data(), 1, D, D);
            gemm_mm(x.data(), ly.Wk.data(), K.data(), 1, D, D);
            gemm_mm(x.data(), ly.Wv.data(), Vv.data(), 1, D, D);

            // RoPE
            rope_apply_all_heads_scaled(Q, K, H, hd, pos, W.cfg.rope_theta_base, W.cfg.rope_freq_scale);

            // Write K/V to cache
            for (i32 h = 0; h < H; ++h) {
                std::copy_n(&K[h * hd], hd, kv.K_ptr(h, pos));
                std::copy_n(&Vv[h * hd], hd, kv.V_ptr(h, pos));
            }

            // Attention against 0..pos
            std::vector<f32> attn_out(D, 0.0f);
            const f32 scale = 1.0f / std::sqrt((f32)hd);
            for (i32 h = 0; h < H; ++h) {
                const f32* qh = &Q[h * hd];
                std::vector<f32> scores(pos + 1);
                for (i32 t = 0; t <= pos; ++t) {
                    const f32* kh = kv.K_ptr(h, t);
                    scores[t] = scale * std::inner_product(qh, qh + hd, kh, 0.0f);
                }
                softmax_inplace(scores.data(), (i32)scores.size());
                for (i32 t = 0; t <= pos; ++t) {
                    const f32* vh = kv.V_ptr(h, t);
                    for (i32 d0 = 0; d0 < hd; ++d0) {
                        attn_out[h * hd + d0] += scores[t] * vh[d0];
                    }
                }
            }

            // Output projection + residual
            std::vector<f32> attn_proj(D, 0.0f);
            gemm_mm(attn_out.data(), ly.Wo.data(), attn_proj.data(), 1, D, D);
            for (i32 i = 0; i < D; ++i) x[i] += attn_proj[i];

            // Pre-MLP norm (use ln2)
            if (W.cfg.norm_kind == 1)
                rmsnorm_row(x.data(), ly.ln2_g.data(), D);
            else
                layernorm_row(x.data(), ly.ln2_g.data(), ly.ln2_b.data(), D);

            // MLP: SwiGLU if W1 is [D x (2*FF)], else ReLU
            const i32 W1_cols = static_cast<i32>(ly.W1.size() / D);
            const i32 FF = W.cfg.d_ff;

            if (W1_cols == 2 * FF) {
                const f32* W1a = ly.W1.data();
                const f32* W1b = ly.W1.data() + (size_t)D * FF;

                std::vector<f32> a(FF), b(FF), gate(FF), ff2(D);
                gemm_mm(x.data(), W1a, a.data(), 1, D, FF);
                gemm_mm(x.data(), W1b, b.data(), 1, D, FF);
                swiglu_pointwise(a, b, gate);
                gemm_mm(gate.data(), ly.W2.data(), ff2.data(), 1, FF, D);
                for (i32 i = 0; i < D; ++i) x[i] += ff2[i];
            }
            else {
                std::vector<f32> ff1(FF), ff2(D);
                gemm_mm(x.data(), ly.W1.data(), ff1.data(), 1, D, FF);
                for (auto& v : ff1) v = std::max(0.0f, v);
                gemm_mm(ff1.data(), ly.W2.data(), ff2.data(), 1, FF, D);
                for (i32 i = 0; i < D; ++i) x[i] += ff2[i];
            }
        }
    }

    Rt.seq_len = N;

    // Final norm + head on last token's representation (use final norm weights)
    if (W.cfg.norm_kind == 1)
        rmsnorm_row(x.data(), W.ln_f_g.data(), D);
    else
        layernorm_row(x.data(), W.ln_f_g.data(), W.ln_f_b.data(), D);

    std::vector<f32> logits(static_cast<size_t>(V), 0.0f);
    gemm_mm(x.data(), W.lm_head.data(), logits.data(), 1, D, V);
    return logits;
}

std::vector<f32> CLLF::decode_step(int token_id) {
    if (!Rt.W) Rt.W = &W;
    const i32 V = std::max<i32>(W.cfg.vocab_size, 2);
    const i32 D = W.cfg.d_model;
    const i32 H = W.cfg.n_heads;
    const i32 hd = std::max<i32>(D / H, 1);
    const i32 L = std::max<i32>(W.cfg.n_layers, 1);

    // Step position for this token in cache
    const i32 pos = Rt.seq_len;
    if (Rt.seq_len < std::numeric_limits<i32>::max()) ++Rt.seq_len;

    // === Start with token embedding ===
    std::vector<f32> x(D, 0.0f);
    if (token_id >= 0 && token_id < V) {
        std::copy_n(&W.tok_emb[token_id * D], D, x.begin());
    }

    // === Loop over all decoder layers ===
    for (i32 l = 0; l < L; ++l) {
        const LayerWeights& ly = W.layers[l];
        KVCache& kv = Rt.kv[l];

        // ---- Pre-attn norm ----
        if (W.cfg.norm_kind == 1)
            rmsnorm_row(x.data(), ly.ln1_g.data(), D);
        else
            layernorm_row(x.data(), ly.ln1_g.data(), ly.ln1_b.data(), D);

        // ---- Project to Q/K/V ----
        std::vector<f32> Q(D), K(D), Vv(D);
        gemm_mm(x.data(), ly.Wq.data(), Q.data(), 1, D, D);
        gemm_mm(x.data(), ly.Wk.data(), K.data(), 1, D, D);
        gemm_mm(x.data(), ly.Wv.data(), Vv.data(), 1, D, D);

        // ---- RoPE on Q/K per head ----
        rope_apply_all_heads_scaled(Q, K, H, hd, pos, W.cfg.rope_theta_base, W.cfg.rope_freq_scale);

        // ---- Store K/V in layer's cache at this pos ----
        for (i32 h = 0; h < H; ++h) {
            std::copy_n(&K[h * hd], hd, kv.K_ptr(h, pos));
            std::copy_n(&Vv[h * hd], hd, kv.V_ptr(h, pos));
        }

        // ---- Scaled dot-product attention ----
        std::vector<f32> attn_out(D, 0.0f);
        const f32 scale = 1.0f / std::sqrt((f32)hd);
        for (i32 h = 0; h < H; ++h) {
            const f32* qh = &Q[h * hd];
            std::vector<f32> scores(Rt.seq_len);
            for (i32 t = 0; t < Rt.seq_len; ++t) {
                const f32* kh = kv.K_ptr(h, t);
                scores[t] = scale * std::inner_product(qh, qh + hd, kh, 0.0f);
            }
            softmax_inplace(scores.data(), (i32)scores.size());
            for (i32 t = 0; t < Rt.seq_len; ++t) {
                const f32* vh = kv.V_ptr(h, t);
                for (i32 d0 = 0; d0 < hd; ++d0) {
                    attn_out[h * hd + d0] += scores[t] * vh[d0];
                }
            }
        }

        // ---- Output projection + residual ----
        std::vector<f32> attn_proj(D, 0.0f);
        gemm_mm(attn_out.data(), ly.Wo.data(), attn_proj.data(), 1, D, D);
        for (i32 i = 0; i < D; ++i) x[i] += attn_proj[i];

        // ---- Pre-MLP norm (use ln2) ----
        if (W.cfg.norm_kind == 1)
            rmsnorm_row(x.data(), ly.ln2_g.data(), D);
        else
            layernorm_row(x.data(), ly.ln2_g.data(), ly.ln2_b.data(), D);

        // ---- SwiGLU if W1 is [D x (2*FF)], else ReLU ----
        const i32 W1_cols = static_cast<i32>(ly.W1.size() / D);
        const i32 FF = W.cfg.d_ff;

        if (W1_cols == 2 * FF) {
            const f32* W1a = ly.W1.data();
            const f32* W1b = ly.W1.data() + (size_t)D * FF;

            std::vector<f32> a(FF), b(FF), gate(FF), ff2(D);
            gemm_mm(x.data(), W1a, a.data(), 1, D, FF);
            gemm_mm(x.data(), W1b, b.data(), 1, D, FF);
            swiglu_pointwise(a, b, gate);
            gemm_mm(gate.data(), ly.W2.data(), ff2.data(), 1, FF, D);

            for (i32 i = 0; i < D; ++i) x[i] += ff2[i];
        }
        else {
            std::vector<f32> ff1(FF), ff2(D);
            gemm_mm(x.data(), ly.W1.data(), ff1.data(), 1, D, FF);
            for (auto& v : ff1) v = std::max(0.0f, v);
            gemm_mm(ff1.data(), ly.W2.data(), ff2.data(), 1, FF, D);
            for (i32 i = 0; i < D; ++i) x[i] += ff2[i];
        }
    }

    // === Final norm and LM head ===
    if (W.cfg.norm_kind == 1)
        rmsnorm_row(x.data(), W.ln_f_g.data(), D);
    else
        layernorm_row(x.data(), W.ln_f_g.data(), W.ln_f_b.data(), D);

    std::vector<f32> logits(static_cast<size_t>(V), 0.0f);
    gemm_mm(x.data(), W.lm_head.data(), logits.data(), 1, D, V);

    return logits;
}


int CLLF::sample_argmax(const std::vector<f32>& logits) const {
    if (logits.empty()) return 0;
    return static_cast<int>(std::distance(logits.begin(), std::max_element(logits.begin(), logits.end())));
}

int CLLF::sample_top_k_top_p(const std::vector<f32>& logits, int top_k, f32 top_p, f32 temp, std::mt19937& rng) const {
    if (logits.empty()) return 0;
    std::vector<f32> probs = softmax_scaled(logits, temp);

    struct Pair { int id; f32 p; };
    std::vector<Pair> items;
    items.reserve(probs.size());
    for (int i = 0; i < static_cast<int>(probs.size()); ++i)
        items.push_back({ i, probs[static_cast<size_t>(i)] });

    std::sort(items.begin(), items.end(), [](const Pair& a, const Pair& b) { return a.p > b.p; });

    if (top_k > 0 && top_k < static_cast<int>(items.size())) {
        items.resize(static_cast<size_t>(top_k));
    }

    if (top_p > 0.0f && top_p < 1.0f) {
        f32 cum = 0.0f;
        size_t keep = 0;
        for (; keep < items.size(); ++keep) {
            cum += items[keep].p;
            if (cum >= top_p) { ++keep; break; }
        }
        if (keep > 0 && keep < items.size()) items.resize(keep);
    }

    f32 sum = 0.0f;
    for (auto& e : items) sum += e.p;
    if (sum <= 0.0f) return 0;

    std::vector<f32> dist;
    dist.reserve(items.size());
    for (auto& e : items) dist.push_back(e.p / sum);

    std::discrete_distribution<int> dd(dist.begin(), dist.end());
    int pick = dd(rng);
    if (pick < 0 || pick >= static_cast<int>(items.size())) return items.front().id;
    return items[static_cast<size_t>(pick)].id;
}

std::string CLLF::generate(std::string_view prompt, int max_new_tokens, f32 temp, int top_k, f32 top_p, bool stream) {
    // Byte-level fallback tokenization
    std::vector<int> tokens;
    tokens.reserve(prompt.size());
    for (unsigned char c : prompt) tokens.push_back(static_cast<int>(c));

    auto logits = prefill(tokens);
    std::mt19937 rng{ std::random_device{}() };

    const int steps = std::max(0, max_new_tokens);
    std::string out;
    out.reserve(static_cast<size_t>(steps));

    for (int i = 0; i < steps; ++i) {
        int next_id = (temp <= 0.0f) ? sample_argmax(logits) : sample_top_k_top_p(logits, top_k, top_p, temp, rng);
        char ch = (next_id >= 0 && next_id <= 255) ? static_cast<char>(next_id) : '?';
        if (stream) {
            std::cout << ch << std::flush;
        }
        else {
            out.push_back(ch);
        }
        logits = decode_step(next_id);
    }

    if (stream) {
        std::cout << std::endl;
        return std::string();
    }
    return out;
}
