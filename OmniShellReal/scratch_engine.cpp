// src/ai/engines/scratch_engine.cpp
// Glue layer: Tokenizer + TransformerModel + streaming sampler + telemetry hooks.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>

#include "ai_engine.h"     // IEngine, Sampling, TokenCallback, EngineInfo
#include "tokenizer.h"          // Your tokenizer API
#include "model.h"              // Your Transformer model API
#include "types.h"               // Your scalar/typedefs (e.g., f32)
#include "math.h"               // Your math kernels (softmax, etc.)

// Optional: compile-time integration hooks.
// Define these macros at build time if you have the headers available.
#if defined(HAVE_PMU_H)
#include "PMU.h"
#endif
#if defined(HAVE_TILE_BRIDGE_H)
#include "TileAnalyticsBridge.h"  // e.g., ProcessTileAnalytics(...)
#endif
#if defined(HAVE_OMNI_AI_MANAGER_H)
#include "OmniAIManager.h"
#endif

namespace ai {

    // ----------------------------
    // Telemetry RAII (optional)
    // ----------------------------
    class AiRunScope {
        using Clock = std::chrono::steady_clock;
        Clock::time_point t0_;
        std::string tag_;
        std::string prompt_digest_;
    public:
        AiRunScope(std::string tag, std::string prompt_sample)
            : t0_(Clock::now()), tag_(std::move(tag)) {
            // Compact prompt for logs
            if (prompt_sample.size() > 120) {
                prompt_sample.resize(117);
                prompt_sample += "...";
            }
            prompt_digest_ = std::move(prompt_sample);
            // Begin hook
#if defined(HAVE_OMNI_AI_MANAGER_H)
            OmniAIManager::setRecentAiSummary("AI start [" + tag_ + "]: " + prompt_digest_);
#endif
#if defined(HAVE_PMU_H)
            // Optional: mark start (no-op if your PMU lacks this API)
            // PMU::MarkEvent("AI_START:" + tag_);
#endif
        }

        ~AiRunScope() {
            using namespace std::chrono;
            auto dt = duration_cast<milliseconds>(Clock::now() - t0_).count();
            // Collect a PMU snapshot if available
            std::string pmu;
#if defined(HAVE_PMU_H)
            // Prefer a non-blocking accessor you already have
            // If not available, leave empty
            try {
                pmu = PMU::getRecentPmuSummary();
            }
            catch (...) {}
#endif

            // Optionally trigger tiles on policy (user may wire this differently)
#if defined(HAVE_TILE_BRIDGE_H)
// If you have a captureLatestBuffer(), use it here. Otherwise, comment out.
// auto buf = captureLatestBuffer(); // user-provided
// ProcessTileAnalytics(buf.data(), ROWS, COLS);
#endif

#if defined(HAVE_OMNI_AI_MANAGER_H)
            OmniAIManager::setRecentAiSummary(
                "AI done [" + tag_ + "]: " + std::to_string(dt) + "ms"
                + (pmu.empty() ? "" : " | PMU: " + pmu));
#endif
        }
    };

    // ----------------------------
    // Utility: Token rate tracker
    // ----------------------------
    struct TokenRate {
        using Clock = std::chrono::steady_clock;
        Clock::time_point start{ Clock::now() };
        size_t tokens{ 0 };

        void add(size_t n = 1) { tokens += n; }
        double tps() const {
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(Clock::now() - start).count();
            if (ms <= 0) return 0.0;
            return static_cast<double>(tokens) / (static_cast<double>(ms) / 1000.0);
        }
    };

    // ----------------------------
    // Sampling: nucleus/top-k/temp/repetition penalty
    // ----------------------------
    static int sample_from_logits(std::vector<float> logits,
        const std::deque<int>& recent_ids,
        const Sampling& s,
        std::mt19937& rng)
    {
        if (logits.empty()) return -1;

        // 1) Repetition penalty (simple variant)
        // Penalize tokens seen in recent_ids by dividing their logits.
        // You can swap to frequency-based or presence-based penalties as desired.
        if (s.repeat_penalty > 1.0f && !recent_ids.empty()) {
            // Use a small recent window (e.g., 64) for speed
            const size_t window = std::min<size_t>(recent_ids.size(), 64);
            for (size_t i = recent_ids.size() > window ? recent_ids.size() - window : 0; i < recent_ids.size(); ++i) {
                int tid = recent_ids[i];
                if (tid >= 0 && static_cast<size_t>(tid) < logits.size()) {
                    logits[tid] /= s.repeat_penalty;
                }
            }
        }

        // 2) Temperature
        float temp = std::max(1e-6f, s.temperature);
        for (auto& v : logits) v /= temp;

        // 3) Convert to probabilities with softmax in a numerically stable way
        // Optionally restrict to top-k first for efficiency.
        std::vector<int> indices(logits.size());
        std::iota(indices.begin(), indices.end(), 0);

        // Top-k filter (keep K best logits)
        int top_k = std::max(0, s.top_k);
        if (top_k > 0 && static_cast<size_t>(top_k) < logits.size()) {
            std::nth_element(indices.begin(), indices.begin() + top_k, indices.end(),
                [&](int a, int b) { return logits[a] > logits[b]; });
            indices.resize(top_k);
        }

        // Gather filtered logits
        std::vector<float> fl;
        fl.reserve(indices.size());
        for (int idx : indices) fl.push_back(logits[idx]);

        // Softmax
        float maxlog = *std::max_element(fl.begin(), fl.end());
        double sum = 0.0;
        for (auto& v : fl) {
            // promote to double for the exp, then keep sum in double
            v = static_cast<float>(std::exp(static_cast<double>(v - maxlog)));
            sum += static_cast<double>(v);
        }
        if (sum <= 0.0) {
            // fallback to argmax
            int best = indices[0];
            float bestv = logits[best];
            for (int idx : indices) {
                if (logits[idx] > bestv) { best = idx; bestv = logits[idx]; }
            }
            return best;
        }
        // make sum a float once for reuse to avoid repeated casts
        float sum_f = static_cast<float>(sum);
        for (auto& v : fl) {
            v = static_cast<float>(v / sum_f);
        }

        // 4) Top-p (nucleus): sort by prob desc, keep cumulative <= p
        float top_p = std::clamp(s.top_p, 0.0f, 1.0f);
        if (top_p > 0.0f && top_p < 1.0f) {
            std::vector<size_t> order(fl.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fl[a] > fl[b]; });
            double csum = 0.0;
            size_t keep = 0;
            for (; keep < order.size(); ++keep) { csum += fl[order[keep]]; if (csum >= top_p) break; }
            ++keep; // include the token that crossed p
            // Renormalize kept
            std::vector<int> new_indices;
            new_indices.reserve(keep);
            std::vector<float> new_probs;
            new_probs.reserve(keep);
            double s2 = 0.0;
            for (size_t i = 0; i < keep; ++i) {
                new_indices.push_back(indices[order[i]]);
                new_probs.push_back(fl[order[i]]);
                s2 += new_probs.back();
            }
            for (auto& v : new_probs) v = static_cast<float>(v / s2);
            indices.swap(new_indices);
            fl.swap(new_probs);
        }

        // 5) Sample
        std::discrete_distribution<int> dist(fl.begin(), fl.end());
        int pick = dist(rng);
        return indices[pick];
    }

    // ----------------------------
    // Optional prompt templating
    // ----------------------------
    static std::string apply_template_if_any(const std::optional<std::string>& tmpl, std::string_view user_prompt) {
        if (!tmpl || tmpl->empty()) return std::string(user_prompt);
        // Very simple replacement: replace "{prompt}" placeholder
        std::string s = *tmpl;
        auto pos = s.find("{prompt}");
        if (pos != std::string::npos) s.replace(pos, std::strlen("{prompt}"), user_prompt);
        return s;
    }

    class TransformerModel {
        CLLF core_;
    public:
        bool load_weights(const std::string& path) {
            return core_.load(path);
        }
        void reset_cache(size_t) {
            core_.reset_session();
        }
        void prefill(const std::vector<int>& ids) {
            (void)core_.prefill(ids);
        }
        void step(int token_id, std::vector<float>& logits_out) {
            auto logits = core_.decode_step(token_id);
            logits_out.assign(logits.begin(), logits.end());
        }
        bool supports_embeddings() const { return false; }
        std::vector<float> embed_mean_pool(const std::vector<int>&) { return {}; }
    };

    // ----------------------------
 // ScratchEngine implementation
 // ----------------------------
    class ScratchEngine final : public IEngine {
        Tokenizer tokenizer_;
        TransformerModel model_;
        EngineInfo info_{};

        // State
        size_t ctx_len_{ 4096 };
        std::optional<std::string> tmpl_name_;
        std::mt19937 rng_{ std::random_device{}() };
        std::deque<int> recent_ids_; // for repetition penalty
    public:
        ~ScratchEngine() override { std::string e; unload(e); }

        EngineInfo info() const override { return info_; }

        bool load(const LoadOptions& opt, std::string& err) override {
            try {
                ctx_len_ = opt.ctx_len;
                tmpl_name_ = opt.template_name;

                // 1) Tokenizer
                tokenizer_.load_vocab("vocab.json", "merges.txt");

                // 2) Model weights
                model_.load_weights(opt.model_path);

                // 3) Reset/allocate KV cache inside the model
                model_.reset_cache(ctx_len_);

                info_ = EngineInfo{
                    .name = "ScratchEngine",
                    .version = "0.1",
                    .backend = "scratch",
                    .ctx_len = ctx_len_,
                    .vocab_size = tokenizer_.vocab_size()
                };
                return true;
            }
            catch (const std::exception& e) {
                err = e.what();
                return false;
            }
        }

        bool unload(std::string& /*err*/) override {
            recent_ids_.clear();
            return true;
        }

        bool chat(const std::string& user_prompt,
            const Sampling& s,
            const TokenCallback& on_token,
            std::string& err) override
        {
            AiRunScope scope("scratch.chat", user_prompt.substr(0, 120));
            TokenRate tr;
            try {
                recent_ids_.clear();

                // Optional prompt template
                std::optional<std::string> tmpl;
                if (tmpl_name_) {
                    if (*tmpl_name_ == "chat") tmpl = std::string("User: {prompt}\nAssistant:");
                }
                const std::string prompt = apply_template_if_any(tmpl, user_prompt);

                // Tokenize
                std::vector<int> ids = tokenizer_.tokenize(prompt);
                if (ids.empty()) { err = "Tokenizer produced empty input."; return false; }

                // Prefill full prompt
                model_.prefill(ids);

                // Seed with last token
                int last_id = ids.back();
                for (int tok : ids) recent_ids_.push_back(tok);
                while (recent_ids_.size() > ctx_len_) recent_ids_.pop_front();

                // Decode loop
                std::vector<float> logits;
                logits.reserve(tokenizer_.vocab_size());
                for (int i = 0; i < s.max_tokens; ++i) {
                    logits.clear();
                    model_.step(last_id, logits);

                    if (logits.empty()) { err = "Model returned empty logits."; return false; }

                    int next_id = sample_from_logits(logits, recent_ids_, s, rng_);
                    if (next_id < 0) { err = "Sampling failed."; return false; }

                    if (tokenizer_.is_eos(next_id)) {
                        if (s.stream) on_token(TokenEvent{ "", true, tr.tps() });
                        break;
                    }

                    std::string piece = tokenizer_.decode({ next_id });
                    if (s.stream && !piece.empty()) {
                        on_token(TokenEvent{ piece, false, tr.tps() });
                    }

                    recent_ids_.push_back(next_id);
                    if (recent_ids_.size() > ctx_len_) recent_ids_.pop_front();
                    last_id = next_id;
                    tr.add(1);
                }

                if (s.stream) on_token(TokenEvent{ "", true, tr.tps() });
                return true;
            }
            catch (const std::exception& e) {
                err = e.what();
                return false;
            }
        }

        bool embed(const std::string& text, EmbedResult& out, std::string& err) override {
            AiRunScope scope("scratch.embed", text.substr(0, 120));
            try {
                std::vector<int> ids = tokenizer_.tokenize(text);
                if (ids.empty()) { err = "Tokenizer produced empty input for embeddings."; return false; }

                if (!model_.supports_embeddings()) {
                    err = "Embeddings are not supported by this scratch model yet.";
                    return false;
                }

                out.vector = model_.embed_mean_pool(ids);
                return true;
            }
            catch (const std::exception& e) {
                err = e.what();
                return false;
            }
        }

        std::string capabilities() const override {
            // ASCII-safe to avoid MSVC codepage errors
            return
                "Supports basic prompt->completion chat with streaming output; "
                "configurable sampling (temperature, top-k, top-p, repetition penalty); "
                "context length up to " + std::to_string(ctx_len_) +
                " tokens; embeddings: " +
                (model_.supports_embeddings() ? "yes" : "no");
        }
    };

    // ----------------------------
    // Factory hook (optional)
    // ----------------------------
    // If you already have a central factory, register there instead.
    std::unique_ptr<IEngine> make_scratch_engine() {
        return std::make_unique<ScratchEngine>();
    }

} // namespace ai
