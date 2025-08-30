// =================================================================
// CommandRouter.h
// Drop-in header for CommandRouter
// =================================================================
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <map>

class CommandRouter {
public:
    // ---------------- Command Metadata ----------------
    struct CommandMeta {
        std::string category;       // e.g., "Core", "Diagnostics"
        std::string usage;          // Short syntax, e.g., "cd [path|-]"
        std::string summary;        // One-line description
        bool platform_win{ false };
        bool platform_linux{ false };
        bool platform_mac{ false };
    };

    using Args = std::vector<std::string>;
    using CommandFunction = std::function<std::string(const Args&)>;

    CommandRouter();

    // Register a command handler
    void registerCommand(const std::string& name, CommandFunction func);

    // Dispatch a line of input to a command or the shell fallback
    std::string dispatch(const std::string& input);

    // Accessor for metadata registry
    static const std::map<std::string, CommandMeta>& GetCommandMetadata();

private:
    // Tokenize input into argv-like tokens
    static std::vector<std::string> tokenize(const std::string& input);

    // Lowercase a command for normalization
    static std::string normalize(const std::string& cmd);

    // Map of normalized command -> handler
    std::unordered_map<std::string, CommandFunction> commandMap;
};