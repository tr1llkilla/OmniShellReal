Copyright © 2025 Cadell Richard Anderson

// ai_engine.cpp
#include "ai_engine.h"
#include "scratch_engine.h"


#include <algorithm>

namespace ai {

    namespace {
        // Normalizes a backend name to lowercase for flexible matching
        static std::string normalize_name(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }
    }

    std::unique_ptr<IEngine> make_engine_from(const std::string& name) {
        const std::string backend = normalize_name(name);

        if (backend == "scratch") {
            return make_scratch_engine();
        }
        throw std::runtime_error("Unknown backend: " + name);
    }

    // Optionally, expose a list of available backends so CommandRouter
    // or OmniAIManager can present them to the user without hardcoding.
    std::vector<std::string> list_available_backends() {
        return {
            "scratch",
            // "workflow",
            // "scripts",
            // "network",
            // "ddc"
        };
    }
    std::string backend_capabilities(const std::string& name) {
        try {
            auto engine = make_engine_from(name);
            return engine->capabilities();
        }
        catch (const std::exception& e) {
            return std::string{ "<error: " } + e.what() + ">";
        }
    }

} // namespace ai
