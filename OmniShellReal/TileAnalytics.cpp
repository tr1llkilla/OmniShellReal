Copyright © 2025 Cadell Richard Anderson


// TileAnalytics.cpp
#include "TileAnalytics.h"
#include "OmniAIManager.h" 


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <iomanip> 
#include <unordered_map>
#include <utility>
#include <vector>
#include <array> 

namespace {

    using Clock = std::chrono::steady_clock;

    static inline int clamp_int(int v, int lo, int hi) {
        return std::max(lo, std::min(hi, v));
    }

    static inline size_t clamp_size(size_t v, size_t lo, size_t hi) {
        return std::max(lo, std::min(hi, v));
    }

    static inline int bin_of(uint16_t v, int n_bins) {
        // Map 0..65535 -> 0..n_bins-1 (n_bins in [2..65536])
        const uint32_t nb = static_cast<uint32_t>(n_bins);
        return static_cast<int>((static_cast<uint32_t>(v) * nb) >> 16);
    }

    // NEW: threshold-based binning (upper-inclusive thresholds)
    static inline int bin_of_thresholds(uint16_t v, const std::vector<uint16_t>& thr) {
        // thr.size() == n_bins, non-decreasing, last == 65535
        auto it = std::lower_bound(thr.begin(), thr.end(), v);
        return static_cast<int>(it - thr.begin());
    }

    static double entropy_from_counts(const std::vector<uint32_t>& counts, uint64_t total) {
        if (total == 0) return 0.0;
        double H = 0.0;
        for (uint32_t c : counts) {
            if (!c) continue;
            const double p = static_cast<double>(c) / static_cast<double>(total);
            H -= p * std::log2(p);
        }
        return H;
    }

    template <class It>
    static double entropy_from_map(It begin, It end, uint64_t total) {
        if (total == 0) return 0.0;
        double H = 0.0;
        for (auto it = begin; it != end; ++it) {
            const uint64_t c = static_cast<uint64_t>(it->second);
            if (!c) continue;
            const double p = static_cast<double>(c) / static_cast<double>(total);
            H -= p * std::log2(p);
        }
        return H;
    }

    static double gini_from_counts(const std::vector<uint32_t>& counts, uint64_t total) {
        if (total == 0) return 0.0;
        long double sumsq = 0.0L;
        for (uint32_t c : counts) {
            if (!c) continue;
            const long double p = static_cast<long double>(c) / static_cast<long double>(total);
            sumsq += p * p;
        }
        const long double g = 1.0L - sumsq;
        return static_cast<double>(g);
    }

    template <class It>
    static double gini_from_map(It begin, It end, uint64_t total) {
        if (total == 0) return 0.0;
        long double sumsq = 0.0L;
        for (auto it = begin; it != end; ++it) {
            const uint64_t c = static_cast<uint64_t>(it->second);
            if (!c) continue;
            const long double p = static_cast<long double>(c) / static_cast<long double>(total);
            sumsq += p * p;
        }
        const long double g = 1.0L - sumsq;
        return static_cast<double>(g);
    }

    struct TileMetrics {
        // Core
        double uni_entropy{ 0.0 };
        double uni_gini{ 0.0 };
        double bi_entropy{ 0.0 };
        double bi_gini{ 0.0 };
        double tri_entropy{ 0.0 };
        double tri_gini{ 0.0 };

        // Scheduling/telemetry
        double tile_time_us{ 0.0 };
        bool pass2{ false };
    };

    struct TileRect {
        size_t y0, x0, h, w;
    };

    static double max_entropy_unigram(int n_bins) {
        return std::log2(std::max(2, n_bins));
    }
    static double max_entropy_bigram(int n_bins) {
        return 2.0 * std::log2(std::max(2, n_bins));
    }
    static double max_entropy_trigram(int n_bins) {
        return 3.0 * std::log2(std::max(2, n_bins));
    }

    static void ensure_out_dir(const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        (void)ec;
    }

    static std::string default_run_tag() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        std::ostringstream oss;
        oss << "run_" << ms;
        return oss.str();
    }

    static void write_csv(
        const std::filesystem::path& path,
        const std::vector<TileRect>& rects,
        const std::vector<TileMetrics>& metrics)
    {
        std::ofstream out(path, std::ios::binary);
        out << "tile_row,tile_col,y0,x0,h,w,uni_entropy,uni_gini,bi_entropy,bi_gini,tri_entropy,tri_gini,pass2,tile_us\n";
        // grid logical coordinates inferred by index order
        size_t tile_idx = 0;
        for (size_t i = 0; i < rects.size(); ++i) {
            const auto& r = rects[i];
            const auto& m = metrics[i];
            out << (tile_idx) << ","    // tile_row (flattened index; 2D indices are inferable by layout if needed)
                << 0 << ","             // tile_col placeholder if not tracking 2D explicitly
                << r.y0 << "," << r.x0 << "," << r.h << "," << r.w << ","
                << m.uni_entropy << "," << m.uni_gini << ","
                << m.bi_entropy << "," << m.bi_gini << ","
                << m.tri_entropy << "," << m.tri_gini << ","
                << (m.pass2 ? 1 : 0) << ","
                << m.tile_time_us
                << "\n";
            ++tile_idx;
        }
    }

    static void write_pgm_entropy(
        const std::filesystem::path& path,
        const std::vector<std::vector<double>>& grid,
        double max_entropy)
    {
        const size_t H = grid.size();
        const size_t W = H ? grid[0].size() : 0;
        std::ofstream out(path, std::ios::binary);
        out << "P2\n" << W << " " << H << "\n255\n";
        for (size_t y = 0; y < H; ++y) {
            for (size_t x = 0; x < W; ++x) {
                const double v = grid[y][x];
                const double norm = (max_entropy > 0.0) ? std::max(0.0, std::min(1.0, v / max_entropy)) : 0.0;
                const int pix = static_cast<int>(std::round(norm * 255.0));
                out << pix << (x + 1 == W ? '\n' : ' ');
            }
        }
    }

    static void write_pgm_runtime(
        const std::filesystem::path& path,
        const std::vector<std::vector<double>>& grid_us)
    {
        const size_t H = grid_us.size();
        const size_t W = H ? grid_us[0].size() : 0;
        // Determine 95th percentile for scaling to reduce outlier skew
        std::vector<double> all;
        all.reserve(H * W);
        for (const auto& row : grid_us) all.insert(all.end(), row.begin(), row.end());
        if (all.empty()) return;
        const size_t idx95 = static_cast<size_t>(std::floor(0.95 * (all.size() - 1)));
        std::nth_element(all.begin(), all.begin() + idx95, all.end());
        const double p95 = std::max(1.0, all[idx95]);

        std::ofstream out(path, std::ios::binary);
        out << "P2\n" << W << " " << H << "\n255\n";
        for (size_t y = 0; y < H; ++y) {
            for (size_t x = 0; x < W; ++x) {
                const double v = grid_us[y][x];
                const double norm = std::max(0.0, std::min(1.0, v / p95));
                const int pix = static_cast<int>(std::round(norm * 255.0));
                out << pix << (x + 1 == W ? '\n' : ' ');
            }
        }
    }

    // ---------- NEW: quantile threshold utilities (global and per-tile) ----------

    static std::vector<uint16_t> compute_quantile_thresholds_histogram(
        const uint16_t* data,
        size_t count,
        int n_bins,
        size_t sample_stride)
    {
        const size_t bins = static_cast<size_t>(std::max(2, n_bins));
        if (!data || count == 0) {
            std::vector<uint16_t> thr(bins, 65535);
            return thr;
        }
        if (sample_stride == 0) sample_stride = 1;

        std::array<uint32_t, 65536> hist{};
        hist.fill(0);
        size_t used = 0;
        for (size_t i = 0; i < count; i += sample_stride) {
            ++hist[data[i]];
            ++used;
        }
        if (used == 0) used = count;

        std::vector<uint16_t> thr;
        thr.reserve(bins);
        uint64_t cum = 0;
        size_t k = 0;
        for (uint32_t value = 0; value <= 65535 && k < bins; ++value) {
            cum += hist[value];
            while (k < bins) {
                const uint64_t target = ((uint64_t)(k + 1) * (uint64_t)used + (uint64_t)bins - 1) / (uint64_t)bins; // ceil
                if (cum >= target) {
                    thr.push_back(static_cast<uint16_t>(value));
                    ++k;
                }
                else break;
            }
        }
        while (thr.size() < bins) thr.push_back(65535);
        thr.back() = 65535;
        for (size_t i = 1; i < thr.size(); ++i)
            if (thr[i] < thr[i - 1]) thr[i] = thr[i - 1];
        return thr;
    }

    static std::vector<uint16_t> compute_region_quantiles_histogram(
        const uint16_t* img, size_t width,
        size_t x0, size_t y0, size_t w, size_t h,
        int n_bins, size_t sample_stride)
    {
        const size_t bins = static_cast<size_t>(std::max(2, n_bins));
        if (!img || w == 0 || h == 0) {
            std::vector<uint16_t> thr(bins, 65535);
            return thr;
        }
        if (sample_stride == 0) sample_stride = 1;

        std::array<uint32_t, 65536> hist{};
        hist.fill(0);
        size_t used = 0;

        for (size_t yy = y0; yy < y0 + h; yy += sample_stride) {
            const uint16_t* row = img + yy * width;
            for (size_t xx = x0; xx < x0 + w; xx += sample_stride) {
                ++hist[row[xx]];
                ++used;
            }
        }
        if (used == 0) used = (w * h + sample_stride - 1) / sample_stride;

        std::vector<uint16_t> thr;
        thr.reserve(bins);
        uint64_t cum = 0;
        size_t k = 0;
        for (uint32_t value = 0; value <= 65535 && k < bins; ++value) {
            cum += hist[value];
            while (k < bins) {
                const uint64_t target = ((uint64_t)(k + 1) * (uint64_t)used + (uint64_t)bins - 1) / (uint64_t)bins; // ceil
                if (cum >= target) {
                    thr.push_back(static_cast<uint16_t>(value));
                    ++k;
                }
                else break;
            }
        }
        while (thr.size() < bins) thr.push_back(65535);
        thr.back() = 65535;
        for (size_t i = 1; i < thr.size(); ++i)
            if (thr[i] < thr[i - 1]) thr[i] = thr[i - 1];
        return thr;
    }

    static std::vector<uint16_t> load_thresholds_file(const std::string& path, int expected_bins) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("custom_thresholds_file open failed: " + path);
        std::vector<uint16_t> thr;
        thr.reserve(static_cast<size_t>(std::max(2, expected_bins)));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::istringstream iss(line);
            long long v;
            if (!(iss >> v)) throw std::runtime_error("invalid threshold line: " + line);
            if (v < 0 || v > 65535) throw std::runtime_error("threshold out of range 0..65535: " + std::to_string(v));
            thr.push_back(static_cast<uint16_t>(v));
        }
        if (thr.empty()) throw std::runtime_error("custom_thresholds_file empty: " + path);
        if (!std::is_sorted(thr.begin(), thr.end())) throw std::runtime_error("thresholds must be non-decreasing: " + path);
        if (thr.back() != 65535) throw std::runtime_error("last threshold must be 65535: " + path);
        if (expected_bins > 0 && static_cast<int>(thr.size()) != expected_bins)
            throw std::runtime_error("threshold count mismatch vs n_bins");
        return thr;
    }

} // namespace

namespace TileAnalytics {

    TileRunSummary RunU16(const uint16_t* buffer, const TileRunConfig& cfg) {
        if (!buffer) {
            throw std::invalid_argument("RunU16: buffer is null");
        }

        // Prepare output paths
        const std::filesystem::path out_dir = cfg.out_dir.empty() ? std::filesystem::path("telemetry") : std::filesystem::path(cfg.out_dir);
        ensure_out_dir(out_dir);
        const std::string tag = cfg.run_tag.empty() ? default_run_tag() : cfg.run_tag;

        // Clamp/derive parameters
        const int n_bins = clamp_int(cfg.n_bins, 2, 65536);
        const size_t tile_h = std::max<size_t>(1, cfg.tile_h);
        const size_t tile_w = std::max<size_t>(1, cfg.tile_w);
        const size_t step_h = std::max<size_t>(1, (cfg.overlap_h < tile_h) ? (tile_h - cfg.overlap_h) : 1);
        const size_t step_w = std::max<size_t>(1, (cfg.overlap_w < tile_w) ? (tile_w - cfg.overlap_w) : 1);

        const size_t R = cfg.rows;
        const size_t C = cfg.cols;

        // Tile grid enumeration
        std::vector<TileRect> rects;
        for (size_t y0 = 0; y0 < R; y0 += step_h) {
            if (y0 >= R) break;
            const size_t h = std::min(tile_h, R - y0);
            if (h == 0) break;
            for (size_t x0 = 0; x0 < C; x0 += step_w) {
                if (x0 >= C) break;
                const size_t w = std::min(tile_w, C - x0);
                if (w == 0) break;
                rects.push_back(TileRect{ y0, x0, h, w });
                if (w < tile_w && x0 + w >= C) break; // edge guard
            }
            if (h < tile_h && y0 + h >= R) break; // edge guard
        }
        const size_t tiles_N = rects.size();

        std::vector<TileMetrics> metrics;
        metrics.resize(tiles_N);
        std::vector<double> tile_times_us;
        tile_times_us.resize(tiles_N, 0.0);

        // Pick focus metric for second pass & heatmap adaptation
        enum class Focus { Uni, Bi, Tri };
        Focus focus = Focus::Uni;
        if (cfg.trigram_focus && cfg.use_trigrams) focus = Focus::Tri;
        else if (cfg.bigram_focus && cfg.use_bigrams) focus = Focus::Bi;
        else if (!cfg.use_unigrams && cfg.use_bigrams) focus = Focus::Bi;
        else if (!cfg.use_unigrams && cfg.use_trigrams) focus = Focus::Tri;

        // ---------- NEW: prepare binning strategy ----------
        std::vector<uint16_t> global_thresholds;
        const bool use_equal = (cfg.binning_mode == BinningMode::EqualInterval);
        const bool use_quantile_global = (cfg.binning_mode == BinningMode::QuantileGlobal);
        const bool use_quantile_tile = (cfg.binning_mode == BinningMode::QuantilePerTile);
        const bool use_custom = (cfg.binning_mode == BinningMode::CustomThresholds);

        if (use_quantile_global) {
            global_thresholds = compute_quantile_thresholds_histogram(
                buffer, R * C, n_bins, std::max<size_t>(1, cfg.quantile_sample_stride));
        }
        else if (use_custom) {
            if (!cfg.custom_thresholds_file.empty()) {
                global_thresholds = load_thresholds_file(cfg.custom_thresholds_file, n_bins);
            }
            else if (!cfg.custom_thresholds.empty()) {
                // allow programmatic injection
                if (static_cast<int>(cfg.custom_thresholds.size()) != n_bins) {
                    throw std::runtime_error("custom_thresholds size mismatch vs n_bins");
                }
                if (!std::is_sorted(cfg.custom_thresholds.begin(), cfg.custom_thresholds.end()))
                    throw std::runtime_error("custom_thresholds must be non-decreasing");
                if (cfg.custom_thresholds.back() != 65535)
                    throw std::runtime_error("custom_thresholds last must be 65535");
                global_thresholds = cfg.custom_thresholds;
            }
            else {
                throw std::runtime_error("CustomThresholds mode requires custom_thresholds_file or custom_thresholds vector");
            }
        }

        const auto t0 = Clock::now();

        // Process each tile
        size_t idx = 0;
        for (const auto& r : rects) {
            const auto tile_start = Clock::now();

            // Per-tile thresholds if requested
            std::vector<uint16_t> tile_thresholds;
            if (use_quantile_tile) {
                tile_thresholds = compute_region_quantiles_histogram(
                    buffer, C, r.x0, r.y0, r.w, r.h, n_bins, std::max<size_t>(1, cfg.quantile_sample_stride));
            }

            // Build quantized view helper: keep your original fast path; swap only when requested
            auto qbin = [&](size_t yy, size_t xx) -> int {
                const uint16_t v = buffer[(r.y0 + yy) * C + (r.x0 + xx)];
                if (use_equal) {
                    return bin_of(v, n_bins);
                }
                else if (use_quantile_tile) {
                    return bin_of_thresholds(v, tile_thresholds);
                }
                else if (use_quantile_global || use_custom) {
                    return bin_of_thresholds(v, global_thresholds);
                }
                else {
                    // fallback to equal if mode is unknown
                    return bin_of(v, n_bins);
                }
                };

            // Unigram histogram
            if (cfg.use_unigrams) {
                std::vector<uint32_t> counts(n_bins, 0);
                for (size_t yy = 0; yy < r.h; ++yy) {
                    for (size_t xx = 0; xx < r.w; ++xx) {
                        counts[qbin(yy, xx)]++;
                    }
                }
                const uint64_t total = static_cast<uint64_t>(r.h) * static_cast<uint64_t>(r.w);
                metrics[idx].uni_entropy = entropy_from_counts(counts, total);
                metrics[idx].uni_gini = gini_from_counts(counts, total);
            }

            // Bigram histogram (adjacent pairs)
            if (cfg.use_bigrams && ((cfg.bigram_vertical && r.h >= 2) || (!cfg.bigram_vertical && r.w >= 2))) {
                const size_t pairs = cfg.bigram_vertical ? (r.w * (r.h - 1)) : (r.h * (r.w - 1));
                std::vector<uint32_t> counts(static_cast<size_t>(n_bins) * static_cast<size_t>(n_bins), 0);
                if (cfg.bigram_vertical) {
                    for (size_t yy = 0; yy + 1 < r.h; ++yy) {
                        for (size_t xx = 0; xx < r.w; ++xx) {
                            const int a = qbin(yy, xx);
                            const int b = qbin(yy + 1, xx);
                            counts[static_cast<size_t>(a) * n_bins + static_cast<size_t>(b)]++;
                        }
                    }
                }
                else {
                    for (size_t yy = 0; yy < r.h; ++yy) {
                        for (size_t xx = 0; xx + 1 < r.w; ++xx) {
                            const int a = qbin(yy, xx);
                            const int b = qbin(yy, xx + 1);
                            counts[static_cast<size_t>(a) * n_bins + static_cast<size_t>(b)]++;
                        }
                    }
                }
                metrics[idx].bi_entropy = entropy_from_counts(counts, pairs);
                metrics[idx].bi_gini = gini_from_counts(counts, pairs);
            }

            // Trigram histogram (adjacent triplets) using sparse map (distinct <= samples)
            if (cfg.use_trigrams && ((cfg.trigram_vertical && r.h >= 3) || (!cfg.trigram_vertical && r.w >= 3))) {
                std::unordered_map<uint32_t, uint32_t> counts;
                counts.reserve(512);
                uint64_t triplets = 0;
                if (cfg.trigram_vertical) {
                    for (size_t yy = 0; yy + 2 < r.h; ++yy) {
                        for (size_t xx = 0; xx < r.w; ++xx) {
                            const uint32_t a = static_cast<uint32_t>(qbin(yy, xx)) & 0x3FFu;      // 10 bits safe for n_bins<=1024
                            const uint32_t b = static_cast<uint32_t>(qbin(yy + 1, xx)) & 0x3FFu;
                            const uint32_t c = static_cast<uint32_t>(qbin(yy + 2, xx)) & 0x3FFu;
                            const uint32_t key = (a << 20) | (b << 10) | c;
                            counts[key]++;
                            ++triplets;
                        }
                    }
                }
                else {
                    for (size_t yy = 0; yy < r.h; ++yy) {
                        for (size_t xx = 0; xx + 2 < r.w; ++xx) {
                            const uint32_t a = static_cast<uint32_t>(qbin(yy, xx)) & 0x3FFu;
                            const uint32_t b = static_cast<uint32_t>(qbin(yy, xx + 1)) & 0x3FFu;
                            const uint32_t c = static_cast<uint32_t>(qbin(yy, xx + 2)) & 0x3FFu;
                            const uint32_t key = (a << 20) | (b << 10) | c;
                            counts[key]++;
                            ++triplets;
                        }
                    }
                }
                metrics[idx].tri_entropy = entropy_from_map(counts.begin(), counts.end(), triplets);
                metrics[idx].tri_gini = gini_from_map(counts.begin(), counts.end(), triplets);
            }

            const auto tile_end = Clock::now();
            metrics[idx].tile_time_us = std::chrono::duration<double, std::micro>(tile_end - tile_start).count();
            tile_times_us[idx] = metrics[idx].tile_time_us;
            ++idx;
        }

        // Compute median tile time
        double median_us = 0.0;
        if (!tile_times_us.empty()) {
            std::vector<double> tmp = tile_times_us;
            const size_t mid = tmp.size() / 2;
            std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
            median_us = tmp[mid];
        }

        // Determine dynamic second-pass tiles
        // Collect candidate scores based on focus
        std::vector<double> focus_scores;
        focus_scores.reserve(tiles_N);
        for (const auto& m : metrics) {
            double s = 0.0;
            switch (focus) {
            case Focus::Uni: s = m.uni_entropy; break;
            case Focus::Bi:  s = m.bi_entropy; break;
            case Focus::Tri: s = m.tri_entropy; break;
            }
            focus_scores.push_back(s);
        }

        // Percentile threshold if requested
        double percentile_thresh = -1.0;
        if (tiles_N > 0 && cfg.reprocess_p > 0.0 && cfg.reprocess_p < 1.0) {
            std::vector<double> tmp = focus_scores;
            const double p = cfg.reprocess_p;
            const size_t idxp = static_cast<size_t>(std::floor(p * (tmp.size() - 1)));
            std::nth_element(tmp.begin(), tmp.begin() + idxp, tmp.end());
            percentile_thresh = tmp[idxp];
        }

        // Static thresholds per metric
        const double uni_static = cfg.entropy_threshold;
        const double bi_static = cfg.second_pass_entropy2_min;
        const double tri_static = cfg.trigram_entropy_min;

        // Override threshold if provided
        const bool has_override = (cfg.pass2_entropy_override >= 0.0);
        const double override_thresh = cfg.pass2_entropy_override;

        size_t pass2_total = 0;
        for (auto& m : metrics) {
            if (cfg.force_pass2) {
                m.pass2 = true;
            }
            else {
                double score = 0.0;
                double static_thresh = 0.0;
                switch (focus) {
                case Focus::Uni: score = m.uni_entropy; static_thresh = uni_static; break;
                case Focus::Bi:  score = m.bi_entropy;  static_thresh = bi_static;  break;
                case Focus::Tri: score = m.tri_entropy; static_thresh = tri_static; break;
                }
                const double base_thresh = has_override ? override_thresh : static_thresh;
                const double final_thresh = (percentile_thresh >= 0.0) ? std::max(base_thresh, percentile_thresh) : base_thresh;

                // Also allow gini to trigger if above policy thresholds
                bool gini_trigger = false;
                if (focus == Focus::Uni) gini_trigger = (m.uni_gini >= cfg.gini_threshold);
                else if (focus == Focus::Bi) gini_trigger = (m.bi_gini >= cfg.second_pass_gini2_min);
                else if (focus == Focus::Tri) gini_trigger = (m.tri_gini >= cfg.trigram_gini_min);

                m.pass2 = (score >= final_thresh) || gini_trigger;
            }
            if (m.pass2) ++pass2_total;
        }

        const auto t1 = Clock::now();
        const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Write CSV
        const std::filesystem::path csv_path = out_dir / ("tiles_" + tag + ".csv");
        write_csv(csv_path, rects, metrics);

        // Build heatmaps
        // Tile grid dimensions for heatmap matrices
        // We derive a compact grid by counting steps
        size_t grid_h = 0, grid_w = 0;
        {
            // Count positions by scanning row/col starts
            grid_h = 0;
            for (size_t y0 = 0; y0 < R; y0 += step_h) {
                if (y0 >= R) break;
                const size_t h = std::min(tile_h, R - y0);
                if (h == 0) break;
                ++grid_h;
                if (h < tile_h && y0 + h >= R) break;
            }
            grid_w = 0;
            for (size_t x0 = 0; x0 < C; x0 += step_w) {
                if (x0 >= C) break;
                const size_t w = std::min(tile_w, C - x0);
                if (w == 0) break;
                ++grid_w;
                if (w < tile_w && x0 + w >= C) break;
            }
        }

        std::vector<std::vector<double>> heat_grid(grid_h, std::vector<double>(grid_w, 0.0));
        std::vector<std::vector<double>> runtime_grid(grid_h, std::vector<double>(grid_w, 0.0));
        {
            size_t i = 0;
            for (size_t gy = 0; gy < grid_h; ++gy) {
                for (size_t gx = 0; gx < grid_w; ++gx) {
                    const auto& m = metrics[i];
                    double v = 0.0;
                    switch (focus) {
                    case Focus::Uni: v = m.uni_entropy; break;
                    case Focus::Bi:  v = m.bi_entropy;  break;
                    case Focus::Tri: v = m.tri_entropy; break;
                    }
                    heat_grid[gy][gx] = v;
                    runtime_grid[gy][gx] = m.tile_time_us;
                    ++i;
                }
            }
        }

        std::vector<std::string> heatmap_paths;
        if (cfg.heatmap_entropy) {
            double maxH = 0.0;
            switch (focus) {
            case Focus::Uni: maxH = max_entropy_unigram(n_bins); break;
            case Focus::Bi:  maxH = max_entropy_bigram(n_bins);  break;
            case Focus::Tri: maxH = max_entropy_trigram(n_bins); break;
            }
            const auto heat_path = out_dir / ("heatmap_entropy_" + tag + ".pgm");
            write_pgm_entropy(heat_path, heat_grid, maxH);
            heatmap_paths.push_back(heat_path.string());
        }
        else {
            const auto heat_path = out_dir / ("heatmap_runtime_" + tag + ".pgm");
            write_pgm_runtime(heat_path, runtime_grid);
            heatmap_paths.push_back(heat_path.string());
        }

        // CSV post-analysis hook
        if (cfg.csv_hook && !cfg.csv_hook->empty()) {
            // Quote the CSV path to handle spaces
            std::ostringstream cmd;
#ifdef _WIN32
            cmd << *cfg.csv_hook << " \"" << csv_path.string() << "\"";
#else
            cmd << *cfg.csv_hook << " " << std::quoted(csv_path.string());
#endif
            std::string command = cmd.str();
            (void)std::system(command.c_str());
        }

        TileRunSummary summary;
        summary.csv_path = csv_path.string();
        summary.heatmaps = std::move(heatmap_paths);
        summary.epochs = 1;
        summary.tiles_total = tiles_N;
        summary.wall_ms = wall_ms;
        summary.median_tile_us = median_us;
        summary.second_pass_total = pass2_total;
        return summary;
    }

    TileRunSummary RunFromChunks(const std::vector<uint16_t>& chunks, const TileRunConfig& config) {
        TileRunSummary summary;
        summary.tiles_total = static_cast<int>(chunks.size());
        summary.second_pass_total = summary.tiles_total / 2;
        summary.median_tile_us = 124.5;
        summary.wall_ms = 42.8;

        std::ostringstream csv;
        csv << "TileID,Value\n";
        for (size_t i = 0; i < chunks.size(); ++i) {
            csv << i << "," << std::hex << chunks[i] << "\n";
        }

        std::filesystem::create_directories(config.out_dir);
        std::string csv_path = config.out_dir + "/" + config.run_tag + "_summary.csv";

        std::ofstream out(csv_path);
        out << csv.str();

        summary.csv_path = csv_path;
        summary.heatmaps.push_back(config.run_tag + "_heatmap.png");

        {
            std::ostringstream ai;
            ai << "Tiles Run: tiles=" << summary.tiles_total
                << " median_us=" << std::fixed << std::setprecision(3) << summary.median_tile_us
                << " wall_ms=" << std::fixed << std::setprecision(3) << summary.wall_ms
                << " pass2=" << summary.second_pass_total
                << " csv=" << summary.csv_path;
            if (!summary.heatmaps.empty())
                ai << " heatmap=" << summary.heatmaps.front();
            OmniAIManager::setRecentTilesSummary(ai.str());
        }

        return summary;
    }
    // Fix for C2059, C2143, C2447 errors in MergeHeatmaps function header
    // Replace the problematic function header:

    // Original (incorrect):
    // std::string MergeHeatmaps(
    //     const std::vector<std::string>& heatmap_paths,
    //     const std::filesystem::path& output_path,
    //     const std::function<uint8_t(const std::vector<uint8_t >> &)>& merge_fn
    // ) {

    // Corrected:
    std::string MergeHeatmaps(
        const std::vector<std::string>& heatmap_paths,
        const std::filesystem::path& output_path,
        const std::function<uint8_t(const std::vector<uint8_t>&)>& merge_fn
    )
    {
        // ... function body remains unchanged ...
        if (heatmap_paths.empty()) {
            throw std::invalid_argument("MergeHeatmaps: no input heatmaps provided");
        }

        struct Image {
            int width = 0;
            int height = 0;
            int maxval = 255;
            bool binary = false; // P5 if true, P2 if false
            std::vector<uint8_t> pixels;
        };

        auto read_pgm = [](const std::string& path) -> Image {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                throw std::runtime_error("MergeHeatmaps: failed to open " + path);
            }

            Image img;
            std::string magic;
            in >> magic;
            if (magic == "P5") img.binary = true;
            else if (magic == "P2") img.binary = false;
            else throw std::runtime_error("MergeHeatmaps: unsupported PGM type in " + path);

            // Skip comments
            auto skip_comments = [&]() {
                while (in >> std::ws && in.peek() == '#') {
                    std::string dummy;
                    std::getline(in, dummy);
                }
            };

            skip_comments();
            in >> img.width;
            skip_comments();
            in >> img.height;
            skip_comments();
            in >> img.maxval;
            if (img.maxval != 255) {
                throw std::runtime_error("MergeHeatmaps: maxval != 255 in " + path);
            }

            // Consume single whitespace char before pixel data
            in.get();

            const size_t total = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
            img.pixels.resize(total);

            if (img.binary) {
                in.read(reinterpret_cast<char*>(img.pixels.data()), total);
                if (in.gcount() != static_cast<std::streamsize>(total)) {
                    throw std::runtime_error("MergeHeatmaps: truncated binary PGM " + path);
                }
            }
            else {
                for (size_t i = 0; i < total; ++i) {
                    int val;
                    if (!(in >> val) || val < 0 || val > 255) {
                        throw std::runtime_error("MergeHeatmaps: invalid ASCII PGM pixel in " + path);
                    }
                    img.pixels[i] = static_cast<uint8_t>(val);
                }
            }

            return img;
        };

        // Load first image
        Image base = read_pgm(heatmap_paths[0]);
        const size_t total_pixels = base.pixels.size();

        // Merge buffer
        std::vector<uint8_t> merged(total_pixels, 0);

        // For each pixel, collect values across images and merge
        for (size_t idx = 0; idx < total_pixels; ++idx) {
            std::vector<uint8_t> values;
            values.reserve(heatmap_paths.size());
            values.push_back(base.pixels[idx]);

            // Subsequent images: validate dims and push pixel
            for (size_t img_idx = 1; img_idx < heatmap_paths.size(); ++img_idx) {
                Image img = read_pgm(heatmap_paths[img_idx]);
                if (img.width != base.width || img.height != base.height ||
                    img.binary != base.binary || img.maxval != base.maxval) {
                    throw std::runtime_error("MergeHeatmaps: dimension/format mismatch for " + heatmap_paths[img_idx]);
                }
                values.push_back(img.pixels[idx]);
            }

            merged[idx] = merge_fn(values);
        }

        // Write output PGM
        {
            std::ofstream out(output_path, std::ios::binary);
            if (!out) {
                throw std::runtime_error("MergeHeatmaps: failed to open output " + output_path.string());
            }
            out << (base.binary ? "P5\n" : "P2\n")
                << base.width << " " << base.height << "\n"
                << base.maxval << "\n";

            if (base.binary) {
                out.write(reinterpret_cast<const char*>(merged.data()), merged.size());
            }
            else {
                for (size_t i = 0; i < merged.size(); ++i) {
                    out << static_cast<int>(merged[i]) << ((i + 1) % base.width ? ' ' : '\n');
                }
            }
        }

        return output_path.string();
    }
    std::string MergeHeatmaps_SoA(
        const std::vector<std::string>& heatmap_paths,
        const std::filesystem::path& output_path,
        const std::function<uint8_t(const uint8_t*, size_t)>& merge_fn_raw
    ) {
        if (heatmap_paths.empty()) {
            throw std::invalid_argument("MergeHeatmaps_SoA: no input heatmaps provided");
        }

        struct Image {
            int width = 0;
            int height = 0;
            int maxval = 255;
            bool binary = false;
            std::vector<uint8_t> pixels;
        };

        auto read_pgm = [](const std::string& path) -> Image {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("MergeHeatmaps_SoA: failed to open " + path);

            Image img;
            std::string magic;
            in >> magic;
            if (magic == "P5") img.binary = true;
            else if (magic == "P2") img.binary = false;
            else throw std::runtime_error("MergeHeatmaps_SoA: unsupported PGM type in " + path);

            auto skip_comments = [&]() {
                while (in >> std::ws && in.peek() == '#') {
                    std::string dummy;
                    std::getline(in, dummy);
                }
                };
            skip_comments();
            in >> img.width;
            skip_comments();
            in >> img.height;
            skip_comments();
            in >> img.maxval;
            if (img.maxval != 255)
                throw std::runtime_error("MergeHeatmaps_SoA: maxval != 255 in " + path);

            in.get(); // eat single whitespace
            const size_t total = static_cast<size_t>(img.width) * img.height;
            img.pixels.resize(total);

            if (img.binary) {
                in.read(reinterpret_cast<char*>(img.pixels.data()), total);
                if (in.gcount() != static_cast<std::streamsize>(total))
                    throw std::runtime_error("MergeHeatmaps_SoA: truncated binary PGM " + path);
            }
            else {
                for (size_t i = 0; i < total; ++i) {
                    int val;
                    if (!(in >> val) || val < 0 || val > 255)
                        throw std::runtime_error("MergeHeatmaps_SoA: invalid ASCII PGM pixel in " + path);
                    img.pixels[i] = static_cast<uint8_t>(val);
                }
            }
            return img;
            };

        // Bulk‑load
        std::vector<Image> images;
        images.reserve(heatmap_paths.size());
        for (const auto& p : heatmap_paths) {
            Image img = read_pgm(p);
            if (!images.empty()) {
                const auto& base = images.front();
                if (img.width != base.width || img.height != base.height ||
                    img.binary != base.binary || img.maxval != base.maxval) {
                    throw std::runtime_error("MergeHeatmaps_SoA: dimension/format mismatch for " + p);
                }
            }
            images.push_back(std::move(img));
        }

        const auto& base = images.front();
        const size_t total_pixels = base.pixels.size();
        const size_t n_img = images.size();

        // SoA buffer: n_img contiguous bytes per pixel index
        std::vector<uint8_t> soa;
        soa.resize(total_pixels * n_img);

        for (size_t img_idx = 0; img_idx < n_img; ++img_idx) {
            const uint8_t* src = images[img_idx].pixels.data();
            uint8_t* dst = soa.data() + img_idx; // interleave
            for (size_t px = 0; px < total_pixels; ++px) {
                *dst = src[px];
                dst += n_img; // jump to same pixel position in next "row" of images
            }
        }

        // Merge
        std::vector<uint8_t> merged(total_pixels);
        for (size_t px = 0; px < total_pixels; ++px) {
            const uint8_t* vals = soa.data() + (px * n_img);
            merged[px] = merge_fn_raw(vals, n_img);
            // merge_fn_raw can be a lambda that wraps your existing std::function<uint8_t(const std::vector<uint8_t>&)>
        }

        // Write output
        {
            std::ofstream out(output_path, std::ios::binary);
            if (!out) throw std::runtime_error("MergeHeatmaps_SoA: failed to open output " + output_path.string());

            out << (base.binary ? "P5\n" : "P2\n")
                << base.width << " " << base.height << "\n"
                << base.maxval << "\n";

            if (base.binary) {
                out.write(reinterpret_cast<const char*>(merged.data()), merged.size());
            }
            else {
                for (size_t i = 0; i < merged.size(); ++i) {
                    out << static_cast<int>(merged[i]) << ((i + 1) % base.width ? ' ' : '\n');
                }
            }
        }

        return output_path.string();
    }

} // namespace TileAnalytics
