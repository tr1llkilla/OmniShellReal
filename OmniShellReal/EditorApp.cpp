// EditorApp.cpp

#include "EditorApp.h"
#include <cstdlib>
#include <iostream>

namespace EditorApp {

    int Launch(const std::string& path, bool blocking) {
#if defined(_WIN32)
        // Simple: launch Notepad (you can swap with "code", "notepad++", etc.)
        std::string cmd = "notepad \"" + path + "\"";
        if (!blocking) {
            cmd += " &";
        }
        int result = std::system(cmd.c_str());
        return result;

#elif defined(__linux__)
        // On Linux: try blocking nano or non-blocking xdg-open
        std::string cmd;
        if (blocking) {
            cmd = "nano \"" + path + "\"";
        }
        else {
            cmd = "xdg-open \"" + path + "\" &";
        }
        int result = std::system(cmd.c_str());
        return result;

#else
        std::cerr << "EditorApp::Launch not supported on this platform.\n";
        return -1;
#endif
    }

    bool Supported() {
#if defined(_WIN32) || defined(__linux__)
        return true;
#else
        return false;
#endif
    }

} // namespace EditorApp


std::optional<std::vector<std::string>>
LaunchEditorWindow(const std::string& filename,
    const std::vector<std::string>& lines,
    int initialLine,
    int initialCol)
{
    EditorLaunchOptions opts;
    opts.title = "OmniEditor - " + filename;
    opts.lines = lines;
    opts.initialLine = initialLine;
    opts.initialCol = initialCol;
    opts.wordWrap = false;
    opts.showLineNumbers = true;
    opts.monospace = true;
    return LaunchEditorWindow(opts);
}

std::optional<std::vector<std::string>>
LaunchEditorWindow(const EditorLaunchOptions& opts)
{
#if defined(_WIN32)
    return LaunchEditorWindow_Win32(opts);
#elif defined(__linux__)
    return LaunchEditorWindow_X11(opts);
#else
    // No GUI backend available on this platform
    return std::optional<std::vector<std::string>>{};
#endif
}
