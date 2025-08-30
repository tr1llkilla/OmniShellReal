// =================================================================
// OmniEditorIDE.h
// =================================================================
#pragma once
#include <string>
#include <vector>

namespace OmniEditorIDE {
    void OpenBuffer(const std::string& name, const std::string& contents);
    void LaunchInteractiveUI();
    void RenderActiveBufferPaged(size_t maxLines = 200);  // ← updated
    void SwitchToBuffer(size_t index);
    void SearchInBuffer(const std::string& query);
    void Highlight(const std::string& token);
    void CompileCurrentBuffer();
    void ExportBufferToFile(size_t bufferId, const std::string& filepath);
    void displayUI(const std::string& filename, const std::vector<std::string>& lines, bool isDirty);
    void open(const std::string& filename);
    void saveFile(const std::string& filename, const std::vector<std::string>& lines);
}