Copyright © 2025 Cadell Richard Anderson

//scratch_engine.h
#pragma once
#include "ai_engine.h"
#include "tokenizer.h"
#include <deque>
#include <optional>
#include <memory>
#include <random>

namespace ai {

    // Forward declaration — actual adapter is in scratch_engine.cpp
    class TransformerModel;

    class ScratchEngine final : public IEngine {
        Tokenizer tokenizer_;
        std::unique_ptr<TransformerModel> model_;
        EngineInfo info_{};

        size_t ctx_len_{ 4096 };
        std::optional<std::string> tmpl_name_;
        std::mt19937 rng_{ std::random_device{}() };
        std::deque<int> recent_ids_;

    public:
        ~ScratchEngine() override;

        EngineInfo info() const override;
        bool load(const LoadOptions& opt, std::string& err) override;
        bool unload(std::string& err) override;
        bool chat(const std::string& prompt,
            const Sampling& s,
            const TokenCallback& on_token,
            std::string& err) override;
        bool embed(const std::string& text,
            EmbedResult& out,
            std::string& err) override;

        // NEW
        std::string capabilities() const override;
    };

    std::unique_ptr<IEngine> make_scratch_engine();

} // namespace ai
