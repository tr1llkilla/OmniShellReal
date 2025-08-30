Copyright © 2025 Cadell Richard Anderson

//ai_engine.h
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>

namespace ai {

    struct EngineInfo {
        std::string name;
        std::string version;
        std::string backend;
        size_t ctx_len{ 0 };
        size_t vocab_size{ 0 };
    };

    struct Sampling {
        float temperature{ 1.0f };
        int   top_k{ 40 };
        float top_p{ 0.95f };
        float repeat_penalty{ 1.1f };
        int   max_tokens{ 256 };
        bool  stream{ true };
    };

    struct TokenEvent {
        std::string text;
        bool   is_final{ false };
        double tkns_per_s{ 0.0 };
    };

    using TokenCallback = std::function<void(const TokenEvent&)>;

    struct EmbedResult {
        std::vector<float> vector;
    };

    struct LoadOptions {
        std::string model_path;
        size_t n_threads{ 0 };
        size_t n_gpu_layers{ 0 };
        size_t ctx_len{ 4096 };
        std::optional<std::string> template_name;
    };

    class IEngine {
    public:
        virtual ~IEngine() = default;
        virtual EngineInfo info() const = 0;
        virtual bool load(const LoadOptions& opt, std::string& err) = 0;
        virtual bool unload(std::string& err) = 0;
        virtual bool chat(const std::string& prompt,
            const Sampling& s,
            const TokenCallback& on_token,
            std::string& err) = 0;
        virtual bool embed(const std::string& text,
            EmbedResult& out,
            std::string& err) = 0;

        // NEW: textual description of what this backend supports
        virtual std::string capabilities() const = 0;
    };

    // Factory function to create an engine instance by backend name.
    std::unique_ptr<IEngine> make_engine_from(const std::string& name);

    // Utility to enumerate available backends.
    std::vector<std::string> list_available_backends();

    // NEW: helper to get capabilities for a backend without CLI instantiation logic.
    std::string backend_capabilities(const std::string& name);

} // namespace ai
