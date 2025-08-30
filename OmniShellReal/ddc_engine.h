Copyright © 2025 Cadell Richard Anderson

//ddc_engine.h

#pragma once
// A reference implementation of a Digital Down-Converter (DDC).
// This class handles frequency shifting, filtering, and decimation of IQ data streams.
#include "types.h"
#include <vector>
#include <complex>

namespace ironrouter {

    class DDCEngine {
    public:
        // @param fs_in Input sample rate in Hz.
        // @param fs_out Target output sample rate in Hz.
        // @param center_offset_hz The frequency offset to shift to baseband.
        DDCEngine(f64 fs_in, f64 fs_out, f64 center_offset_hz);
        ~DDCEngine();

        // Processes a block of interleaved 16-bit integer IQ samples.
        // @return The number of output samples produced.
        size_t process_block(const i16* in_iq_interleaved, size_t in_samples, std::vector<std::complex<f32>>& out);

        // Dynamically change the frequency offset.
        void set_center_offset(f64 hz);
        // Dynamically change the decimation factor.
        void set_decimation(i32 factor);

    private:
        f64 fs_in;
        f64 fs_out;
        f64 center_offset;
        i32 decimation;
        f64 phase;      // Current phase of the Numerically Controlled Oscillator (NCO).
        f64 phase_inc;  // Phase increment per sample.

        std::vector<f32> fir_taps; // Coefficients for the low-pass FIR filter.
        std::vector<std::complex<f32>> filter_state; // State for the FIR filter between calls.

        // Designs a basic windowed-sinc low-pass filter.
        void design_lowpass_filter();
    };

} // namespace ironrouter
