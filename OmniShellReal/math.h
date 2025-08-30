#pragma once
#include "types.h"
#include <numeric>
#include <vector>      // Required for std::vector
#include <functional>  // Required for std::greater
#include <cmath>       // Required for std::exp, std::sqrt, std::tanh

// Existing naive row-major matmul
inline void matmul(const f32* A, const f32* B, f32* C, i32 M, i32 K, i32 N) {
    for (i32 i = 0; i < M; ++i) {
        for (i32 j = 0; j < N; ++j) {
            f32 acc = 0.f;
            const f32* a = A + i * K;
            const f32* b = B + j;
            for (i32 k = 0; k < K; ++k) acc += a[k] * b[k * N];
            C[i * N + j] = acc;
        }
    }
}

// Existing affine
inline void affine_rowmajor(const f32* X, const f32* W, const f32* b,
    f32* Y, i32 rows, i32 in_dim, i32 out_dim) {
    matmul(X, W, Y, rows, in_dim, out_dim);
    if (b) {
        for (i32 r = 0; r < rows; ++r)
            for (i32 j = 0; j < out_dim; ++j)
                Y[r * out_dim + j] += b[j];
    }
}

// Existing softmax
inline void softmax_inplace(f32* x, i32 n) {
    f32 m = -std::numeric_limits<f32>::infinity();
    for (i32 i = 0; i < n; ++i) m = (std::max)(m, x[i]); // ✅ FIX: Disambiguate from Windows macro
    f64 sum = 0.0;
    for (i32 i = 0; i < n; ++i) { x[i] = std::exp(x[i] - m); sum += x[i]; }
    const f32 inv = static_cast<f32>(1.0 / sum);
    for (i32 i = 0; i < n; ++i) x[i] *= inv;
}

// ==== NEW: softmax with temperature ====
inline void softmax_inplace(f32* x, i32 n, f32 temperature) {
    f32 m = -std::numeric_limits<f32>::infinity();
    for (i32 i = 0; i < n; ++i) m = (std::max)(m, x[i]); // ✅ FIX: Disambiguate from Windows macro
    f64 sum = 0.0;
    for (i32 i = 0; i < n; ++i) { x[i] = std::exp((x[i] - m) / temperature); sum += x[i]; }
    const f32 inv = static_cast<f32>(1.0 / sum);
    for (i32 i = 0; i < n; ++i) x[i] *= inv;
}

// ==== NEW: top-k probability filter ====
inline void top_k_filter(f32* probs, i32 n, i32 k) {
    if (k <= 0 || k >= n) return;
    std::vector<f32> sorted(probs, probs + n);
    std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end(), std::greater<f32>());
    f32 thresh = sorted[k - 1];
    for (i32 i = 0; i < n; ++i)
        if (probs[i] < thresh) probs[i] = 0.0f;
}

// ==== NEW: L2 normalisation ====
inline void l2_normalize(f32* vec, i32 n) {
    f32 norm = std::sqrt(std::inner_product(vec, vec + n, vec, 0.0f));
    if (norm > 0) for (i32 i = 0; i < n; ++i) vec[i] /= norm;
}

// Existing LayerNorm
inline void layernorm_row(f32* x, const f32* gamma, const f32* beta, i32 d, f32 eps = 1e-5f) {
    f64 mean = 0.0;
    for (i32 i = 0; i < d; ++i) mean += x[i];
    mean /= d;
    f64 var = 0.0;
    for (i32 i = 0; i < d; ++i) { f64 z = x[i] - mean; var += z * z; }
    var /= d;
    const f32 inv = 1.0f / std::sqrt(static_cast<f32>(var) + eps);
    for (i32 i = 0; i < d; ++i) {
        f32 nh = (x[i] - static_cast<f32>(mean)) * inv;
        x[i] = nh * gamma[i] + beta[i];
    }
}

// Existing GELU
inline f32 gelu(f32 x) {
    const f32 a = 0.7978845608028654f;
    const f32 b = 0.044715f;
    const f32 x3 = x * x * x;
    return 0.5f * x * (1.0f + std::tanh(a * (x + b * x3)));
}

// Existing row GELU
inline void gelu_row(f32* x, i32 d) {
    for (i32 i = 0; i < d; ++i) x[i] = gelu(x[i]);
}

// ==== NEW: inplace add and mul for vectorised ops ====
inline void add_inplace(f32* dst, const f32* src, i32 n) {
    for (i32 i = 0; i < n; ++i) dst[i] += src[i];
}

inline void mul_inplace(f32* dst, const f32* src, i32 n) {
    for (i32 i = 0; i < n; ++i) dst[i] *= src[i];
}
