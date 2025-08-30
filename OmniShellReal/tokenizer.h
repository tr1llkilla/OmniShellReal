//tokenizer.h

#pragma once
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <unordered_map>

namespace ai::tokext {

    template <typename Tokenizer>
    inline constexpr bool has_direct_decoder = false;

    template <typename Tokenizer>
    inline std::string decode_piece_fallback(const Tokenizer& tok, token_id id) {
        std::vector<int> one{ static_cast<int>(id) };
        return tok.decode(one);
    }

    template <typename Tokenizer>
    inline std::string decode_piece(const Tokenizer& tok, token_id id) {
        if constexpr (has_direct_decoder<Tokenizer>) {
            auto it = tok.decoder.find(id);
            return it != tok.decoder.end() ? it->second : std::string{};
        }
        else {
            return decode_piece_fallback(tok, id);
        }
    }

    template <typename Tokenizer>
    class TokenStreamDecoder {
    public:
        using PieceCallback = std::function<void(token_id, const std::string&)>;

        explicit TokenStreamDecoder(const Tokenizer& tokenizer)
            : tok_(tokenizer) {
        }

        std::string append(token_id id) {
            const std::string piece = decode_piece(tok_, id);
            if (on_piece_) on_piece_(id, piece);
            buffer_ += piece;
            return piece;
        }

        const std::string& text() const { return buffer_; }
        void set_callback(PieceCallback cb) { on_piece_ = std::move(cb); }
        void clear() { buffer_.clear(); }

    private:
        const Tokenizer& tok_;
        std::string buffer_;
        PieceCallback on_piece_{};
    };

} // namespace ai::tokext

struct Tokenizer {
    int id_bos = 1;
    int id_eos = 2;
    int id_unk = 3;

    int vocab_size_ = 256 + 4;
    size_t vocab_size() const { return static_cast<size_t>(vocab_size_); }

    // NEW: direct decoder map
    std::unordered_map<token_id, std::string> decoder;

    // NEW: constructor to initialise decoder map
    Tokenizer() {
        for (int i = 0; i < 256; ++i) {
            decoder[(token_id)i] = std::string(1, static_cast<char>(i));
        }
        decoder[(token_id)id_bos] = "";
        decoder[(token_id)id_eos] = "";
        decoder[(token_id)id_unk] = "<unk>";
    }

    std::vector<int> encode_bytes(std::string_view s, bool add_bos = true) const {
        std::vector<int> ids;
        if (add_bos) ids.push_back(id_bos);
        for (unsigned char c : s) ids.push_back(static_cast<int>(c));
        return ids;
    }

    std::string decode_bytes(const std::vector<int>& ids) const {
        std::string out;
        for (int id : ids) {
            if (id == id_bos || id == id_eos || id == id_unk) continue;
            if (0 <= id && id <= 255) out.push_back(static_cast<char>(id));
        }
        return out;
    }

    void load_vocab(const std::string&, const std::string&) {}

    std::vector<int> tokenize(const std::string& text) const {
        return encode_bytes(text, true);
    }

    std::string decode(const std::vector<int>& ids) const {
        return decode_bytes(ids);
    }

    bool is_eos(int token_id) const {
        return token_id == id_eos;
    }
};

// NEW: mark Tokenizer as having a direct decoder
namespace ai::tokext {
    template <>
    inline constexpr bool has_direct_decoder<::Tokenizer> = true;
}
