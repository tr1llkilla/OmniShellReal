//EditorApp.h
#pragma once

#include <optional>
#include <string>
#include <vector>

//
// EditorApp namespace: generic platform-agnostic helpers
//
namespace EditorApp {

    // Launch the editor for a given file path.
    // If blocking == true, call will not return until editor is closed.
    // Returns 0 on success, non-zero on failure (e.g., unsupported platform).
    int Launch(const std::string& path, bool blocking = false);

    // Query whether the current platform/editor backend is available.
    // Useful for help text and feature discovery.
    bool Supported();
}

//
// Options structure for high‑level editor launches.
//
struct EditorLaunchOptions {
    std::string title;                  // Window title or identifier
    std::vector<std::string> lines;     // Initial file contents (UTF-8)

    int initialLine = 0;                 // 0‑based starting line
    int initialCol = 0;                  // 0‑based starting column

    bool wordWrap = false;               // Reserved for future wrapping logic
    bool showLineNumbers = true;         // Whether to display gutter line numbers
    bool monospace = true;               // Use monospace font rendering
};

//
// High-level convenience API: launch by filename + content.
//
std::optional<std::vector<std::string>>
LaunchEditorWindow(const std::string& filename,
    const std::vector<std::string>& lines,
    int initialLine,
    int initialCol);

//
// Direct API: launch with an EditorLaunchOptions struct.
//
std::optional<std::vector<std::string>>
LaunchEditorWindow(const EditorLaunchOptions& opts);

//
// Platform‑specific entry points (implemented in respective .cpp files).
//
#if defined(_WIN32)
std::optional<std::vector<std::string>>
LaunchEditorWindow_Win32(const EditorLaunchOptions& opts);
#endif

#if defined(__linux__)
std::optional<std::vector<std::string>>
LaunchEditorWindow_X11(const EditorLaunchOptions& opts);
#endif