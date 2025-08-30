// =================================================================
// BinaryTranslator.h
// =================================================================
#pragma once
#include <string>

namespace BinaryTranslator {
    std::string Decompile(const std::string& binaryPath);
    std::string ExtractMetadata(const std::string& binaryPath);
    std::string DisassembleCapstone(const std::string& binaryPath);
    std::string ClassifyMalwareBehavior(const std::string& asmCode);
    std::string ReconstructControlFlow(const std::string& asmCode);
}