Copyright © 2025 Cadell Richard Anderson

// TileAnalytics.h
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>

enum class BinningMode {
    EqualInterval,     // original fast path: linear map via shift
    QuantileGlobal,    // compute thresholds over full buffer, reuse for all tiles
    QuantilePerTile,   // compute thresholds per-tile (optionally subsampled)
    CustomThresholds   // load thresholds (upper bounds) from file or vector
};

struct TileRunConfig {
    // Buffer geometry (input)
    size_t rows{ 256 };
    size_t cols{ 256 };

    // Scheduling/telemetry targets
    double target_time_ms{ 0.8 };  // 0.2–2.0 typical

    // Overlap between adjacent tiles (in samples)
    size_t overlap_h{ 1 };
    size_t overlap_w{ 1 };

    // Prior logic (preserved)
    double high_prio_fraction{ 0.25 };
    double entropy_threshold{ 7.5 };
    double reprocess_p{ 0.95 };    // Percentile for second-pass thresholding (e.g., P95)

    // Heatmap mode (preserved)
    bool heatmap_entropy{ true };  // true: entropy heatmap; false: runtime heatmap

    // Output controls
    std::string out_dir{ "telemetry" };
    std::string run_tag; // optional disambiguator

    // --- Additions for advanced analytics and CLI dynamics ---

    // Tiling geometry (per-tile size; defaults are non-breaking)
    size_t tile_h{ 16 };
    size_t tile_w{ 16 };

    // Quantization for 16-bit sample values
    int n_bins{ 256 }; // maps 0..65535 -> 0..n_bins-1

    // Analysis toggles
    bool use_unigrams{ true };       // unigram histogram (single-sample)
    bool use_bigrams{ true };        // bigram histogram (adjacent pairs)
    bool bigram_vertical{ false };   // bigram scan direction

    // New: trigram analysis (adjacent triplets)
    bool use_trigrams{ false };
    bool trigram_vertical{ false };  // trigram scan direction

    // Thresholds/policies
    double gini_threshold{ 0.90 };           // promote if >= this gini (optional)
    double second_pass_entropy2_min{ 6.0 };  // bigram entropy minimum for pass2
    double second_pass_gini2_min{ 0.95 };    // bigram gini minimum for pass2

    // New: trigram-specific thresholds
    double trigram_entropy_min{ 5.0 };
    double trigram_gini_min{ 0.85 };

    // New: dynamic second-pass invocation controls
    bool force_pass2{ false };               // --force-pass2
    double pass2_entropy_override{ -1.0 };   // --pass2-threshold=ENT (applies to focused metric if >=0)

    // New: adaptive heatmap policies
    bool bigram_focus{ false };              // --bigram-focus (prefer bigram metrics)
    bool trigram_focus{ false };             // optional: prefer trigram metrics if enabled

    // New: CSV post-analysis hook (script receives CSV path as first arg)
    std::optional<std::string> csv_hook;     // --csv-hook=script.sh (or .bat/.exe on Windows)

    // -------- NEW: binning configuration --------
    BinningMode binning_mode{ BinningMode::EqualInterval }; // default preserves original behavior
    size_t quantile_sample_stride{ 1 };                     // >=1; decimate for speed vs accuracy
    std::string custom_thresholds_file;                     // path to thresholds file (upper bounds)
    std::vector<uint16_t> custom_thresholds;               // optional programmatic thresholds (upper bounds, size==n_bins, last==65535)
};

struct TileRunSummary {
    std::string csv_path;
    std::vector<std::string> heatmaps; // PGM files
    size_t epochs{ 0 };
    size_t tiles_total{ 0 };
    double wall_ms{ 0.0 };
    double median_tile_us{ 0.0 };
    size_t second_pass_total{ 0 };
};

namespace TileAnalytics {

    // Run a buffer through the tiler pipeline with telemetry.
    TileRunSummary RunU16(const uint16_t* buffer, const TileRunConfig& cfg);

    // Convenience: seed from 16-bit chunks repeating.
    TileRunSummary RunFromChunks(const std::vector<uint16_t>& chunks, const TileRunConfig& cfg);

    // NEW: Merge multiple per-tile heatmap images into a single combined heatmap.
    // All input heatmaps must have identical dimensions and format (e.g., 8-bit PGM paths).
    // The merge_fn takes the vector of pixel values (one per source) for a given coordinate
    // and returns the merged pixel value (0..255).
    //
    // Example merge_fn: average, max, min, or a custom policy.
    std::string MergeHeatmaps(
        const std::vector<std::string>& heatmap_paths,
        const std::filesystem::path& output_path,
        const std::function<uint8_t(const std::vector<uint8_t>&)>& merge_fn
    );

} // namespace TileAnalytics
