Copyright © 2025 Cadell Richard Anderson

// =================================================================
// BinaryTranslator.cpp
// =================================================================
#include "BinaryTranslator.h"
#include "BinaryManip.h"    
#include <capstone/capstone.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <windows.h>

#pragma comment(lib, "capstone.lib")

std::string BinaryTranslator::ExtractMetadata(const std::string& binaryPath) {
    auto info = BinaryManip::Probe(binaryPath);
    std::ostringstream meta;
    meta << "[Metadata for: " << binaryPath << "]\n";

    if (!info) {
        meta << "  Error: Could not probe binary file.\n";
        return meta.str();
    }

    // Extract file size
    std::ifstream file(binaryPath, std::ios::binary | std::ios::ate);
    if (file) {
        meta << "  Size: " << file.tellg() << " bytes\n";
    }

    meta << "  OS: " << (info->os == BinaryManip::OS::Windows ? "Windows" : "Linux/Other") << "\n";
    meta << "  Arch: " << (info->arch == BinaryManip::Arch::X64 ? "x64" : "x86/Other") << "\n";
    meta << "  Type: " << (info->isLibrary ? "Library (DLL/Shared Object)" : "Executable") << "\n";
    meta << "  Entry Point RVA: 0x" << std::hex << info->entryRVA << "\n";
    meta << "  Image Base: 0x" << std::hex << info->imageBase << "\n";
    meta << "  Stripped: " << (info->stripped ? "Yes" : "No") << "\n";

    return meta.str();
}

std::string BinaryTranslator::DisassembleCapstone(const std::string& binaryPath) {
    std::ifstream file(binaryPath, std::ios::binary);
    if (!file) return "[Error: Could not open binary file]";
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), {});

    csh handle;
    cs_insn* insn;
    size_t count;

    std::ostringstream output;

    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
        return "[Error initializing Capstone]";

    count = cs_disasm(handle, code.data(), code.size(), 0x1000, 0, &insn);
    if (count > 0) {
        for (size_t i = 0; i < count; i++) {
            output << "0x" << std::hex << insn[i].address << ":\t" << insn[i].mnemonic << "\t" << insn[i].op_str << "\n";
        }
        cs_free(insn, count);
    }
    else {
        output << "[Failed to disassemble binary]";
    }

    cs_close(&handle);
    return output.str();
}

std::string BinaryTranslator::Decompile(const std::string& binaryPath) {
    std::ostringstream oss;
    std::string asmCode = DisassembleCapstone(binaryPath);
    oss << "// Decompiled pseudocode of " << binaryPath << "\n";
    oss << asmCode << "\n";
    oss << ClassifyMalwareBehavior(asmCode) << "\n";
    oss << ReconstructControlFlow(asmCode);
    return oss.str();
}

std::string BinaryTranslator::ClassifyMalwareBehavior(const std::string& asmCode) {
    std::ostringstream report;
    if (asmCode.find("CreateRemoteThread") != std::string::npos) report << "[!] Detected Injection Behavior\n";
    if (asmCode.find("GetProcAddress") != std::string::npos) report << "[!] Detected Dynamic API Resolution\n";
    if (asmCode.find("WriteProcessMemory") != std::string::npos) report << "[!] Memory Tampering Detected\n";
    return report.str();
}

std::string BinaryTranslator::ReconstructControlFlow(const std::string& asmCode) {
    std::ostringstream graph;
    graph << "\n// Control Flow Graph (approximate)\n";
    graph << "main -> sub_func_1 -> sub_func_2 -> exit\n"; // placeholder
    return graph.str();
}
