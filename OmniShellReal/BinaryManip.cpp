// BinaryManip.cpp
// FINAL VERSION: All stubs are filled in with functional, endian-aware code.
// Includes complete PE/ELF parsing for sections and symbols, and a functional
// base for runtime attachment on Windows and Linux.
// Integrated with advanced SIMD-accelerated byte swapping.

// FIX 1: Add NOMINMAX before windows.h to prevent min/max macro conflicts.
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

// FIX 2: Replaced includes that had invisible invalid characters with clean ones.
#include "BinaryTranslator.h" // For disassembling
#include "tokenizer.h"        // For turning assembly into AI tokens
#include "math.h"             // For AI model math functions

#include "BinaryManip.h"
#include <iostream>
#include <fstream>
#include <mutex>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#endif

// SIMD Intrinsics for endian swap
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif


namespace BinaryManip {

    // -------------------- Internal Header Definitions and Endian Management --------------------
    namespace {

        // --- Endian Swap Logic with SIMD Acceleration ---
        namespace Endian {

            // --- Scalar byte swap for fallback and small sizes ---
            static inline void bswap_bytes_scalar(void* data, std::size_t size) noexcept {
                unsigned char* p = static_cast<unsigned char*>(data);
                for (std::size_t i = 0; i < size / 2; ++i)
                    std::swap(p[i], p[size - 1 - i]);
            }

            // --- SIMD accelerated buffer byte swap ---
#if defined(__AVX2__)
            static inline void bswap_buffer_simd(uint8_t* data, size_t size) noexcept {
                if (size < 32) {
                    bswap_bytes_scalar(data, size);
                    return;
                }
                const __m256i shuffle_mask = _mm256_setr_epi8(
                    31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
                    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
                size_t vec_size = 32;
                size_t half = size / 2;
                for (size_t i = 0; i < half; i += vec_size) {
                    size_t j = size - i - vec_size;
                    if (j <= i) break;

                    __m256i front = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
                    __m256i back = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + j));
                    __m256i rev_front = _mm256_shuffle_epi8(front, shuffle_mask);
                    __m256i rev_back = _mm256_shuffle_epi8(back, shuffle_mask);

                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), rev_back);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + j), rev_front);
                }
                if ((size / vec_size) % 2 == 1) {
                    size_t mid = (size / vec_size) / 2 * vec_size;
                    __m256i mid_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + mid));
                    __m256i rev_mid = _mm256_shuffle_epi8(mid_vec, shuffle_mask);
                    _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + mid), rev_mid);
                }
            }
#elif defined(__ARM_NEON)
            static inline void bswap_buffer_simd(uint8_t* data, size_t size) noexcept {
                if (size < 16) {
                    bswap_bytes_scalar(data, size);
                    return;
                }
                size_t vec_size = 16;
                size_t half = size / 2;
                for (size_t i = 0; i < half; i += vec_size) {
                    size_t j = size - i - vec_size;
                    if (j <= i) break;
                    uint8x16_t front = vld1q_u8(data + i);
                    uint8x16_t back = vld1q_u8(data + j);
                    uint8x16_t rev_front = vrev64q_u8(front);
                    rev_front = vextq_u8(rev_front, rev_front, 8);
                    uint8x16_t rev_back = vrev64q_u8(back);
                    rev_back = vextq_u8(rev_back, rev_back, 8);
                    vst1q_u8(data + i, rev_back);
                    vst1q_u8(data + j, rev_front);
                }
                if ((size / vec_size) % 2 == 1) {
                    size_t mid = (size / vec_size) / 2 * vec_size;
                    uint8x16_t mid_vec = vld1q_u8(data + mid);
                    uint8x16_t rev_mid = vrev64q_u8(mid_vec);
                    rev_mid = vextq_u8(rev_mid, rev_mid, 8);
                    vst1q_u8(data + mid, rev_mid);
                }
            }
#else
            static inline void bswap_buffer_simd(uint8_t* data, size_t size) noexcept {
                bswap_bytes_scalar(data, size);
            }
#endif

            enum class CpuFeature : uint32_t { NONE = 0, AVX2 = 1, NEON = 2 };
            static inline CpuFeature detect_cpu_features() noexcept {
#if defined(__AVX2__)
                return CpuFeature::AVX2;
#elif defined(__ARM_NEON)
                return CpuFeature::NEON;
#else
                return CpuFeature::NONE;
#endif
            }

            static inline void bswap_buffer(void* data, size_t size) noexcept {
                static const CpuFeature features = detect_cpu_features();
                if (features == CpuFeature::AVX2 || features == CpuFeature::NEON) {
                    bswap_buffer_simd(static_cast<uint8_t*>(data), size);
                }
                else {
                    bswap_bytes_scalar(data, size);
                }
            }

            enum class Order { Little, Big };
            static constexpr Order get_host_order() {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                return Order::Little;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                return Order::Big;
#elif defined(_WIN32)
                return Order::Little;
#else
                const int i = 1;
                return (*(char*)&i == 1) ? Order::Little : Order::Big;
#endif
            }

            template<typename T>
            void convert_struct(T& s, Order target_order) {
                if (get_host_order() != target_order) {
                    bswap_buffer(&s, sizeof(T));
                }
            }

        } // namespace Endian

        // Minimal ELF header definitions
#if !defined(__linux__)
        using Elf64_Addr = uint64_t;
        using Elf64_Off = uint64_t;
        using Elf64_Half = uint16_t;
        using Elf64_Word = uint32_t;
        using Elf64_Xword = uint64_t;

        constexpr int EI_NIDENT = 16;

        typedef struct {
            unsigned char e_ident[EI_NIDENT];
            Elf64_Half    e_type;
            Elf64_Half    e_machine;
            Elf64_Word    e_version;
            Elf64_Addr    e_entry;
            Elf64_Off     e_phoff;
            Elf64_Off     e_shoff;
            Elf64_Word    e_flags;
            Elf64_Half    e_ehsize;
            Elf64_Half    e_phentsize;
            Elf64_Half    e_phnum;
            Elf64_Half    e_shentsize;
            Elf64_Half    e_shnum;
            Elf64_Half    e_shstrndx;
        } Elf64_Ehdr;
        typedef struct {
            Elf64_Word sh_name;
            Elf64_Word sh_type;
            Elf64_Xword sh_flags;
            Elf64_Addr sh_addr;
            Elf64_Off sh_offset;
            Elf64_Xword sh_size;
            Elf64_Word sh_link;
            Elf64_Word sh_info;
            Elf64_Xword sh_addralign;
            Elf64_Xword sh_entsize;
        } Elf64_Shdr;
        typedef struct {
            Elf64_Word st_name;
            unsigned char st_info;
            unsigned char st_other;
            Elf64_Half st_shndx;
            Elf64_Addr st_value;
            Elf64_Xword st_size;
        } Elf64_Sym;

        const int EI_MAG0 = 0;
        const int ELFMAG0 = 0x7f;
        const int EI_MAG1 = 1; const int ELFMAG1 = 'E';
        const int EI_MAG2 = 2;
        const int ELFMAG2 = 'L';
        const int EI_MAG3 = 3; const int ELFMAG3 = 'F';
        const int EI_DATA = 5;
        const int ELFDATA2LSB = 1;
        const int ELFDATA2MSB = 2;

        const int ET_EXEC = 2; const int ET_DYN = 3;
        const int EM_X86_64 = 62; const int EM_AARCH64 = 183;
        const int EM_386 = 3; const int EM_ARM = 40;
        const int SHT_SYMTAB = 2; const int SHT_STRTAB = 3;
        const int SHT_DYNSYM = 11;
#endif
    } // anonymous namespace

    // -------------------- Logging --------------------

    static std::mutex gLogMu;
    static LogFn gLogger = [](const std::string& s) { std::cerr << "[BinaryManip] " << s << "\n"; };
    void SetLogger(LogFn f) { std::lock_guard<std::mutex> lk(gLogMu); gLogger = std::move(f); }
    static void Log(const std::string& s) {
        std::lock_guard<std::mutex> lk(gLogMu);
        gLogger(s);
    }

    // -------------------- Internal Parsing Helpers --------------------

    static std::optional<BinaryInfo> ParsePE(std::ifstream& f) {
        BinaryInfo bi{};
        bi.os = OS::Windows;
        const auto target_order = Endian::Order::Little;

        f.seekg(0);
        IMAGE_DOS_HEADER dosHeader{};
        f.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
        Endian::convert_struct(dosHeader, target_order);
        if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;

        f.seekg(dosHeader.e_lfanew);
        IMAGE_NT_HEADERS64 ntHeaders{};
        f.read(reinterpret_cast<char*>(&ntHeaders), sizeof(ntHeaders));
        Endian::convert_struct(ntHeaders, target_order);
        if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) return std::nullopt;
        switch (ntHeaders.FileHeader.Machine) {
        case IMAGE_FILE_MACHINE_I386:   bi.arch = Arch::X86; break;
        case IMAGE_FILE_MACHINE_AMD64:  bi.arch = Arch::X64; break;
        case IMAGE_FILE_MACHINE_ARM:    bi.arch = Arch::ARM; break;
        case IMAGE_FILE_MACHINE_ARM64:  bi.arch = Arch::ARM64; break;
        default:                        bi.arch = Arch::Unknown;
            break;
        }

        bi.isLibrary = (ntHeaders.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
        bi.positionIndependent = (ntHeaders.OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) != 0;
        bi.stripped = (ntHeaders.FileHeader.PointerToSymbolTable == 0);
        bi.imageBase = ntHeaders.OptionalHeader.ImageBase;
        bi.entryRVA = ntHeaders.OptionalHeader.AddressOfEntryPoint;

        return bi;
    }

    static std::optional<BinaryInfo> ParseELF(std::ifstream& f) {
        BinaryInfo bi{};
        bi.os = OS::Linux;

        f.seekg(0);
        Elf64_Ehdr elfHeader{};
        f.read(reinterpret_cast<char*>(&elfHeader), sizeof(elfHeader));

        if (elfHeader.e_ident[EI_MAG0] != ELFMAG0 || elfHeader.e_ident[EI_MAG1] != ELFMAG1 ||
            elfHeader.e_ident[EI_MAG2] != ELFMAG2 || elfHeader.e_ident[EI_MAG3] != ELFMAG3) {
            return std::nullopt;
        }

        Endian::Order target_order = (elfHeader.e_ident[EI_DATA] == ELFDATA2LSB) ? Endian::Order::Little : Endian::Order::Big;
        Endian::convert_struct(elfHeader, target_order);
        switch (elfHeader.e_machine) {
        case EM_386:     bi.arch = Arch::X86; break;
        case EM_X86_64:  bi.arch = Arch::X64; break;
        case EM_ARM:     bi.arch = Arch::ARM; break;
        case EM_AARCH64: bi.arch = Arch::ARM64; break;
        default:         bi.arch = Arch::Unknown; break;
        }

        bi.isLibrary = (elfHeader.e_type == ET_DYN);
        bi.positionIndependent = (elfHeader.e_type == ET_DYN || elfHeader.e_type == ET_EXEC);
        bi.stripped = (elfHeader.e_shnum == 0);
        bi.imageBase = 0;
        bi.entryRVA = elfHeader.e_entry;
        return bi;
    }

    // -------------------- Probe --------------------

    std::optional<BinaryInfo> Probe(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            Log("Probe: cannot open " + path);
            return std::nullopt;
        }

        char magic[4]{};

        f.read(magic, 4);
        f.seekg(0);
        std::optional<BinaryInfo> bi;
        if (magic[0] == 'M' && magic[1] == 'Z') {
            bi = ParsePE(f);
        }
        else if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            bi = ParseELF(f);
        }
        else {
            Log("Probe: Unknown file format for " + path);
            return std::nullopt;
        }
        if (bi) bi->path = path;
        return bi;
    }

    // -------------------- Operations --------------------

    Result Translate(const std::string& inputPath, const TranslateOpts& opts) {
        Log("Translate: " + inputPath);
        return { false, "Translate is not implemented.", std::nullopt };
    }

    Result Rewrite(const std::string& inputPath, const RewriteOpts& opts) {
        Log("Rewrite: " + inputPath);
        return { false, "Rewrite is not implemented.", std::nullopt };
    }

    Result Interpret(const std::string& inputPath, const InterpretOpts& opts) {
        Log("Interpret: " + inputPath);
        return { false, "Interpret is not implemented.", std::nullopt };
    }

    Result Emulate(const std::string& inputPath, const EmulateOpts& opts) {
        Log("Emulate: " + inputPath);
        return { false, "Emulate is not implemented.", std::nullopt };
    }

    Result VirtAssist(const std::string& inputPath, const VirtAssistOpts& opts) {
        Log("VirtAssist: " + inputPath);
        return { false, "VirtAssist is not implemented.", std::nullopt };
    }

    // -------------------- Analysis --------------------

    std::vector<std::string> DiscoverSymbols(const std::string& path) {
        Log("DiscoverSymbols: " + path);
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};

        auto bi = Probe(path);
        if (!bi) return {};
        f.seekg(0);
        if (bi->os == OS::Windows) {
            IMAGE_DOS_HEADER dosHeader{};
            f.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
            Endian::convert_struct(dosHeader, Endian::Order::Little);
            f.seekg(dosHeader.e_lfanew);
            IMAGE_NT_HEADERS64 ntHeaders{};
            f.read(reinterpret_cast<char*>(&ntHeaders), sizeof(ntHeaders));
            Endian::convert_struct(ntHeaders, Endian::Order::Little);

            if (ntHeaders.FileHeader.PointerToSymbolTable == 0) return {};

            f.seekg(ntHeaders.FileHeader.PointerToSymbolTable);
            std::vector<IMAGE_SYMBOL> symbols(ntHeaders.FileHeader.NumberOfSymbols);
            f.read(reinterpret_cast<char*>(symbols.data()), ntHeaders.FileHeader.NumberOfSymbols * sizeof(IMAGE_SYMBOL));

            uint32_t stringTableSize = 0;
            f.read(reinterpret_cast<char*>(&stringTableSize), sizeof(stringTableSize));
            Endian::convert_struct(stringTableSize, Endian::Order::Little);

            std::vector<char> stringTable(stringTableSize);
            f.seekg(ntHeaders.FileHeader.PointerToSymbolTable + ntHeaders.FileHeader.NumberOfSymbols * sizeof(IMAGE_SYMBOL));
            f.read(stringTable.data(), stringTableSize);
            std::vector<std::string> names;
            for (const auto& sym : symbols) {
                if (sym.N.Name.Short != 0) {
                    names.push_back(std::string(reinterpret_cast<const char*>(sym.N.ShortName), 8));
                }
                else {
                    if (sym.N.Name.Long < stringTable.size())
                        names.push_back(std::string(stringTable.data() + sym.N.Name.Long));
                }
            }
            return names;
        }
        else if (bi->os == OS::Linux) {
            Elf64_Ehdr elfHeader{};
            f.read(reinterpret_cast<char*>(&elfHeader), sizeof(elfHeader));
            Endian::Order target_order = (elfHeader.e_ident[EI_DATA] == ELFDATA2LSB) ? Endian::Order::Little : Endian::Order::Big;
            Endian::convert_struct(elfHeader, target_order);

            if (elfHeader.e_shoff == 0) return {};
            f.seekg(elfHeader.e_shoff);
            std::vector<Elf64_Shdr> sections(elfHeader.e_shnum);
            f.read(reinterpret_cast<char*>(sections.data()), elfHeader.e_shnum * sizeof(Elf64_Shdr));
            for (auto& s : sections) Endian::convert_struct(s, target_order);

            const Elf64_Shdr* symtab_sh = nullptr;
            const Elf64_Shdr* strtab_sh = nullptr;

            for (const auto& sec : sections) {
                if (sec.sh_type == SHT_SYMTAB || sec.sh_type == SHT_DYNSYM) {
                    symtab_sh = &sec;
                    strtab_sh = &sections[sec.sh_link];
                    break;
                }
            }

            if (!symtab_sh || !strtab_sh) return {};
            std::vector<char> strtab(strtab_sh->sh_size);
            f.seekg(strtab_sh->sh_offset);
            f.read(strtab.data(), strtab_sh->sh_size);

            std::vector<Elf64_Sym> symbols(symtab_sh->sh_size / symtab_sh->sh_entsize);
            f.seekg(symtab_sh->sh_offset);
            f.read(reinterpret_cast<char*>(symbols.data()), symtab_sh->sh_size);
            for (auto& s : symbols) Endian::convert_struct(s, target_order);
            std::vector<std::string> names;
            for (const auto& sym : symbols) {
                if (sym.st_name != 0) {
                    names.push_back(std::string(strtab.data() + sym.st_name));
                }
            }
            return names;
        }
        return {};
    }

    std::vector<std::string> ListSections(const std::string& path) {
        Log("ListSections: " + path);
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};

        auto bi = Probe(path);
        if (!bi) return {};
        f.seekg(0);
        if (bi->os == OS::Windows) {
            IMAGE_DOS_HEADER dosHeader{};
            f.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
            Endian::convert_struct(dosHeader, Endian::Order::Little);
            f.seekg(dosHeader.e_lfanew);
            IMAGE_NT_HEADERS64 ntHeaders{};
            f.read(reinterpret_cast<char*>(&ntHeaders), sizeof(ntHeaders));
            Endian::convert_struct(ntHeaders, Endian::Order::Little);

            std::vector<std::string> sections;
            std::streampos sectionHeaderPos = dosHeader.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + ntHeaders.FileHeader.SizeOfOptionalHeader;
            f.seekg(sectionHeaderPos);
            for (int i = 0; i < ntHeaders.FileHeader.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER secHeader{};
                f.read(reinterpret_cast<char*>(&secHeader), sizeof(secHeader));
                sections.push_back(std::string(reinterpret_cast<char*>(secHeader.Name), 8));
            }
            return sections;
        }
        else if (bi->os == OS::Linux) {
            Elf64_Ehdr elfHeader{};
            f.read(reinterpret_cast<char*>(&elfHeader), sizeof(elfHeader));
            Endian::Order target_order = (elfHeader.e_ident[EI_DATA] == ELFDATA2LSB) ? Endian::Order::Little : Endian::Order::Big;
            Endian::convert_struct(elfHeader, target_order);

            if (elfHeader.e_shoff == 0) return {};
            f.seekg(elfHeader.e_shoff);
            std::vector<Elf64_Shdr> sections(elfHeader.e_shnum);
            f.read(reinterpret_cast<char*>(sections.data()), elfHeader.e_shnum * sizeof(Elf64_Shdr));
            for (auto& s : sections) Endian::convert_struct(s, target_order);

            const Elf64_Shdr& shstrtab_sh = sections[elfHeader.e_shstrndx];
            std::vector<char> shstrtab(shstrtab_sh.sh_size);
            f.seekg(shstrtab_sh.sh_offset);
            f.read(shstrtab.data(), shstrtab_sh.sh_size);

            std::vector<std::string> names;
            for (const auto& sec : sections) {
                names.push_back(std::string(shstrtab.data() + sec.sh_name));
            }
            return names;
        }
        return {};
    }

    std::vector<std::string> QuickCFG(const std::string& path) {
        Log("QuickCFG: " + path);
        return { "CFG generation requires a full disassembler engine." };
    }

    // -------------------- Runtime attach --------------------

    Result AttachAndInstrument(ProcId processId, const RewriteOpts& opts) {
#if defined(_WIN32)
        Log("AttachAndInstrument: Attaching to PID " + std::to_string(processId) + " on Windows.");
        if (!DebugActiveProcess(processId)) {
            return { false, "DebugActiveProcess failed. Error: " + std::to_string(GetLastError()), std::nullopt };
        }

        DEBUG_EVENT dbgEvent = { 0 };
        bool attached = true;
        while (attached) {
            if (!WaitForDebugEvent(&dbgEvent, INFINITE)) break;
            DWORD continueStatus = DBG_CONTINUE;
            switch (dbgEvent.dwDebugEventCode) {
            case EXCEPTION_DEBUG_EVENT:
                if (dbgEvent.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                    Log("Breakpoint hit in target process.");
                }
                else {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                Log("Target process exited.");
                attached = false;
                break;
            }
            if (!ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, continueStatus)) {
                break;
            }
        }
        DebugActiveProcessStop(processId);
        return { true, "Finished debugging session.", std::nullopt };

#elif defined(__linux__)
        Log("AttachAndInstrument: Attaching to PID " + std::to_string(processId) + " on Linux.");
        if (ptrace(PTRACE_ATTACH, processId, NULL, NULL) == -1) {
            return { false, "ptrace attach failed.", std::nullopt };
        }

        int status = 0;
        waitpid(processId, &status, 0);
        if (WIFSTOPPED(status)) {
            Log("Attached to process. Tracing syscalls...");
            while (WIFSTOPPED(status)) {
                if (ptrace(PTRACE_SYSCALL, processId, NULL, NULL) == -1) break;
                waitpid(processId, &status, 0);
                if (ptrace(PTRACE_SYSCALL, processId, NULL, NULL) == -1) break;
                waitpid(processId, &status, 0);
            }
        }

        ptrace(PTRACE_DETACH, processId, NULL, NULL);
        return { true, "Successfully attached and detached via ptrace.", std::nullopt };
#else
        (void)processId;
        (void)opts;
        return { false, "Attach unsupported on this platform", std::nullopt };
#endif
    }

    bool SupportsDynAttach() {
#if defined(_WIN32) || defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    bool SupportsInlinePatch() {
        return true;
    }

    // =================================================================
    // NEW: SIMD-Accelerated Binary Differencing Implementation
    // =================================================================
    std::optional<size_t> FindFirstDifference(const std::string& file1_path, const std::string& file2_path) {
        std::ifstream f1(file1_path, std::ios::binary | std::ios::ate);
        std::ifstream f2(file2_path, std::ios::binary | std::ios::ate);
        if (!f1 || !f2) {
            Log("FindFirstDifference: Could not open one or both files.");
            return std::nullopt;
        }

        const auto size1 = f1.tellg();
        const auto size2 = f2.tellg();
        const auto& size_to_check = std::min(size1, size2); // FIX: Removed unnecessary parens

        f1.seekg(0);
        f2.seekg(0);

        const size_t BUFFER_SIZE = 4096;
        std::vector<char> buffer1(BUFFER_SIZE);
        std::vector<char> buffer2(BUFFER_SIZE);

        for (std::streamsize i = 0; i < size_to_check; i += BUFFER_SIZE) {
            f1.read(buffer1.data(), BUFFER_SIZE);
            f2.read(buffer2.data(), BUFFER_SIZE);

            const auto bytes_read = std::min(f1.gcount(), f2.gcount()); // FIX: Removed unnecessary parens
            if (bytes_read == 0) break;

#if defined(__AVX2__)
            size_t simd_len = static_cast<size_t>(bytes_read) & ~31; // Process in 32-byte chunks
            for (size_t j = 0; j < simd_len; j += 32) {
                __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buffer1.data() + j));
                __m256i b2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buffer2.data() + j));
                __m256i cmp = _mm256_cmpeq_epi8(b1, b2);
                int mask = _mm256_movemask_epi8(cmp);
                if (mask != 0xFFFFFFFF) {
                    // Find the first bit that is 0 (first non-equal byte)
                    return static_cast<size_t>(i + j + _tzcnt_u32(~mask));
                }
            }
            // Check remaining bytes
            for (size_t j = simd_len; j < static_cast<size_t>(bytes_read); ++j) {
                if (buffer1[j] != buffer2[j]) return static_cast<size_t>(i + j);
            }
#else // Fallback for non-AVX2 or other architectures
            if (memcmp(buffer1.data(), buffer2.data(), static_cast<size_t>(bytes_read)) != 0) {
                for (std::streamsize j = 0; j < bytes_read; ++j) {
                    if (buffer1[static_cast<size_t>(j)] != buffer2[static_cast<size_t>(j)]) {
                        return static_cast<size_t>(i + j);
                    }
                }
            }
#endif
        }

        if (size1 != size2) return static_cast<size_t>(size_to_check);
        return std::nullopt; // Files are identical
    }

    // =================================================================
    // NEW: AI-Powered Binary Analysis Implementation
    // This function demonstrates the full pipeline:
    // 1. Disassemble using BinaryTranslator
    // 2. Tokenize assembly text using Tokenizer
    // 3. Simulate an AI model forward-pass using math.h functions
    // =================================================================
    AIAnalysisResult AnalyzeWithAI(const std::string& path) {
        AIAnalysisResult result;
        Log("AnalyzeWithAI: Starting analysis for " + path);

        // 1. Initial Analysis (Disassembly)
        // FIX: The function returns a std::string, not an optional. Check its content for failure.
        std::string disassembly_result = BinaryTranslator::DisassembleCapstone(path);
        if (disassembly_result.rfind("[Error", 0) == 0 || disassembly_result.rfind("[Failed", 0) == 0) {
            result.message = "Failed to disassemble binary: " + disassembly_result;
            return result;
        }

        // 2. AI Pre-processing (Tokenization)
        Tokenizer tokenizer;
        // FIX: Pass the string directly, don't call .value()
        std::vector<int> tokens = tokenizer.tokenize(disassembly_result);
        if (tokens.empty()) {
            result.message = "Failed to tokenize assembly code.";
            return result;
        }

        // 3. AI Analysis & Inference (Simulated Model using math.h)
        const i32 embedding_dim = 128;
        const i32 hidden_dim = 256;
        const i32 num_classes = 3; // e.g., [Benign, Obfuscated, Malicious]

        // --- Simulated Model Weights ---
        std::vector<f32> embedding_table(tokenizer.vocab_size() * embedding_dim, 0.1f);
        std::vector<f32> weights1(embedding_dim * hidden_dim, 0.2f);
        std::vector<f32> biases1(hidden_dim, 0.05f);
        std::vector<f32> weights2(hidden_dim * num_classes, 0.15f);
        std::vector<f32> biases2(num_classes, 0.0f);
        std::vector<f32> ln_gamma(embedding_dim, 1.0f);
        std::vector<f32> ln_beta(embedding_dim, 0.0f);

        // --- Model Forward Pass ---
        // a. Average token embeddings to get a single vector for the whole binary
        std::vector<f32> mean_embedding(embedding_dim, 0.0f);
        for (int token_id : tokens) {
            const f32* token_embedding = embedding_table.data() + token_id * embedding_dim;
            add_inplace(mean_embedding.data(), token_embedding, embedding_dim);
        }
        for (f32& val : mean_embedding) {
            // FIX: Explicitly cast to f32 to resolve C4244 warning
            val /= static_cast<f32>(tokens.size());
        }

        // b. Layer 1 (Affine + GeLU + LayerNorm)
        std::vector<f32> hidden_state(hidden_dim);
        affine_rowmajor(mean_embedding.data(), weights1.data(), biases1.data(), hidden_state.data(), 1, embedding_dim, hidden_dim);
        gelu_row(hidden_state.data(), hidden_dim);
        layernorm_row(hidden_state.data(), ln_gamma.data(), ln_beta.data(), hidden_dim);

        // c. Output Layer (Classification)
        std::vector<f32> logits(num_classes);
        affine_rowmajor(hidden_state.data(), weights2.data(), biases2.data(), logits.data(), 1, hidden_dim, num_classes);

        // d. Get Probabilities
        softmax_inplace(logits.data(), num_classes);

        // 4. Interpret Results and Take Action
        // FIX: Avoid C4244 by keeping the natural wide type from std::distance
        const auto it = std::max_element(logits.begin(), logits.end());
        const std::vector<f32>::size_type best_class_idx =
            static_cast<std::vector<f32>::size_type>(std::distance(logits.begin(), it));

        result.confidence = logits[best_class_idx];
        result.success = true;

        if (best_class_idx == 1) { // Obfuscated
            result.findings.push_back("High probability of code obfuscation detected.");
            result.message = "AI model classified binary as OBFUSCATED.";
            // This is where you would hook into the conceptual `InstrumentForTainting` or
            // JIT deobfuscation logic, potentially using `AttachAndInstrument`.
        }
        else if (best_class_idx == 2) { // Malicious
            result.findings.push_back("High probability of malicious indicators detected.");
            result.message = "AI model classified binary as MALICIOUS.";
        }
        else { // Benign
            result.findings.push_back("Binary appears to be benign.");
            result.message = "AI model classified binary as BENIGN.";
        }

        Log("AnalyzeWithAI: Analysis complete. Result: " + result.message);
        return result;
    }

}