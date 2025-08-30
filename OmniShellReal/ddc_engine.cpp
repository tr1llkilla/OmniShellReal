// ddc_engine.cpp
#include "ddc_engine.h"
#define _USE_MATH_DEFINES // Ensures M_PI is defined in <cmath>
#include <cmath>
#include <cassert>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ironrouter {

    // Helper to convert a 16-bit signed integer to a float in the range [-1.0, 1.0].
    static inline f32 int16_to_float(i16 v) {
        return static_cast<f32>(v) / 32768.0f;
    }

    DDCEngine::DDCEngine(f64 fs_in_, f64 fs_out_, f64 center_offset_hz_) :
        fs_in(fs_in_), fs_out(fs_out_), center_offset(center_offset_hz_), decimation(1), phase(0.0)
    {
        if (fs_out > 0) {
            decimation = static_cast<i32>(std::max(1.0, round(fs_in / fs_out)));
        }
        phase_inc = -2.0 * M_PI * center_offset / fs_in; // Negative for down-conversion
        design_lowpass_filter();
    }

    DDCEngine::~DDCEngine() {}

    void DDCEngine::set_center_offset(f64 hz) {
        center_offset = hz;
        phase_inc = -2.0 * M_PI * center_offset / fs_in;
    }

    void DDCEngine::set_decimation(i32 factor) {
        decimation = (factor < 1) ? 1 : factor;
        design_lowpass_filter(); // Re-design filter for new decimation rate
    }

    void DDCEngine::design_lowpass_filter() {
        const i32 num_taps = 65; // Odd number for integer delay
        fir_taps.assign(num_taps, 0.0f);
        f64 cutoff_freq = 0.5 / decimation; // Normalized cutoff frequency

        for (i32 n = 0; n < num_taps; ++n) {
            i32 m = n - (num_taps - 1) / 2;
            f64 sinc_val = (m == 0) ? 2.0 * M_PI * cutoff_freq : sin(2.0 * M_PI * cutoff_freq * m) / m;

            // Apply a Hamming window to reduce spectral leakage
            f64 window_val = 0.54 - 0.46 * cos(2.0 * M_PI * n / (num_taps - 1));
            fir_taps[n] = static_cast<f32>((sinc_val / M_PI) * window_val);
        }
        filter_state.assign(num_taps - 1, std::complex<f32>(0.0f, 0.0f));
    }

    size_t DDCEngine::process_block(const i16* in_iq_interleaved, size_t in_samples, std::vector<std::complex<f32>>& out) {
        assert(in_samples > 0);
        out.clear();
        out.reserve(in_samples / decimation + 1);

        std::vector<std::complex<f32>> mixed_signal;
        mixed_signal.reserve(in_samples);

        // Step 1: NCO Mix (Frequency Shift) and convert to float
        for (size_t i = 0; i < in_samples; i++) {
            std::complex<f32> sample(int16_to_float(in_iq_interleaved[2 * i]), int16_to_float(in_iq_interleaved[2 * i + 1]));
            std::complex<f32> mixer(static_cast<f32>(cos(phase)), static_cast<f32>(sin(phase)));
            mixed_signal.push_back(sample * mixer);

            phase += phase_inc;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            if (phase < -2.0 * M_PI) phase += 2.0 * M_PI;
        }

        // Step 2: Low-Pass Filter and Decimate
        const i32 num_taps = static_cast<i32>(fir_taps.size());
        std::vector<std::complex<f32>> convolution_buffer = filter_state;
        convolution_buffer.insert(convolution_buffer.end(), mixed_signal.begin(), mixed_signal.end());

        size_t output_count = 0;
        for (size_t i = 0; (i + num_taps) <= convolution_buffer.size(); i += decimation) {
            std::complex<f32> acc(0.0f, 0.0f);
            for (i32 k = 0; k < num_taps; ++k) {
                acc += convolution_buffer[i + k] * fir_taps[k];
            }
            out.push_back(acc);
            output_count++;
        }

        // Save the end of the buffer as the state for the next block
        if (convolution_buffer.size() >= (size_t)(num_taps - 1)) {
            filter_state.assign(convolution_buffer.end() - (num_taps - 1), convolution_buffer.end());
        }
        else {
            filter_state.assign(convolution_buffer.begin(), convolution_buffer.end());
        }

        return output_count;
    }

} // namespace ironrouter