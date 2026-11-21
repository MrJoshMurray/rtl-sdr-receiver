/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "spectrum_simulator.h"

#include <liquid/liquid.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <mutex>

namespace platform::sdr {
namespace {

std::mutex live_spectrum_mutex;
std::atomic<bool> live_spectrum_blanked{false};
std::atomic<uint32_t> live_spectrum_span{kDefaultRxSampleRateHz};
SpectrumFrame live_spectrum_frame{};
std::array<float, kSpectrumBinCount> live_spectrum_power{};
constexpr size_t kLiveSpectrumSamples = 2048;
constexpr float kSpectrumDisplayOffsetDb = 75.0f;
std::array<IqSample, kLiveSpectrumSamples> live_iq_buffer{};
size_t live_iq_count = 0;
size_t live_iq_write_index = 0;
int32_t live_iq_shift_hz = 0;

struct SpectrumFft {
    std::array<liquid_float_complex, kLiveSpectrumSamples> input{};
    std::array<liquid_float_complex, kLiveSpectrumSamples> output{};
    fftplan plan = fft_create_plan(kLiveSpectrumSamples, input.data(), output.data(),
                                   LIQUID_FFT_FORWARD, 0);

    SpectrumFft() {
        if (!plan) std::fprintf(stderr, "Failed to create spectrum FFT plan\n");
    }
    ~SpectrumFft() {
        if (plan) fft_destroy_plan(plan);
    }
};

const std::array<float, kLiveSpectrumSamples>& hann_window() noexcept {
    static const auto window = [] {
        std::array<float, kLiveSpectrumSamples> values{};
        constexpr float kPi = 3.14159265358979323846f;
        for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
            values[sample] = 0.5f - 0.5f * std::cos(
                2.0f * kPi * static_cast<float>(sample)
                / static_cast<float>(kLiveSpectrumSamples));
        }
        return values;
    }();
    return window;
}

uint8_t peak_level(size_t bin, size_t center, size_t radius, uint8_t top) noexcept {
    const auto distance = static_cast<size_t>(std::abs(static_cast<int>(bin) - static_cast<int>(center)));
    if (distance > radius) {
        return 0;
    }
    return static_cast<uint8_t>(top - (top / (radius + 1)) * distance);
}

} // namespace

SpectrumFrame make_simulated_spectrum(uint32_t frame_index) noexcept {
    SpectrumFrame spectrum{};
    const size_t first_center = 18 + (frame_index % 9);
    const size_t second_center = 68 - (frame_index % 13);

    for (size_t bin = 0; bin < spectrum.size(); ++bin) {
        const auto noise = static_cast<uint8_t>(12 + ((bin * 73 + frame_index * 29) % 18));
        spectrum[bin] = std::max({noise,
                                  peak_level(bin, first_center, 4, 220),
                                  peak_level(bin, second_center, 7, 184)});
    }

    return spectrum;
}

void LiveSpectrumBuffer::push_iq(const IqSample* samples, size_t sample_count, int32_t shift_hz) noexcept {
    if (!samples || sample_count == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(live_spectrum_mutex);
    live_iq_shift_hz = shift_hz;
    for (size_t sample = 0; sample < sample_count; ++sample) {
        live_iq_buffer[live_iq_write_index] = samples[sample];
        live_iq_write_index = (live_iq_write_index + 1) % kLiveSpectrumSamples;
        live_iq_count = std::min(live_iq_count + 1, kLiveSpectrumSamples);
    }
}

void LiveSpectrumBuffer::set_span_hz(uint32_t span_hz) noexcept {
    live_spectrum_span.store(span_hz);
}

uint32_t LiveSpectrumBuffer::span_hz() noexcept {
    return live_spectrum_span.load();
}

void LiveSpectrumBuffer::set_blanked(bool blanked) noexcept {
    live_spectrum_blanked.store(blanked);
}

bool LiveSpectrumBuffer::snapshot(SpectrumFrame& frame) noexcept {
    if (live_spectrum_blanked.load()) {
        live_spectrum_power.fill(0.0f);
        live_spectrum_frame = {};
        frame = live_spectrum_frame;
        return true;
    }
    std::array<IqSample, kLiveSpectrumSamples> samples{};
    int32_t shift_hz = 0;
    {
        std::lock_guard<std::mutex> lock(live_spectrum_mutex);
        if (live_iq_count < kLiveSpectrumSamples) {
            return false;
        }

        const size_t first_sample = live_iq_write_index;
        shift_hz = live_iq_shift_hz;
        for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
            samples[sample] = live_iq_buffer[
                (first_sample + sample) % kLiveSpectrumSamples];
        }
    }

    constexpr float kFullScale = 8388608.0f;
    const auto& window = hann_window();
    std::array<float, kSpectrumBinCount> power{};
    float mean_i = 0.0f;
    float mean_q = 0.0f;
    for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
        const auto& iq = samples[sample];
        mean_i += static_cast<float>(iq.i) / kFullScale;
        mean_q += static_cast<float>(iq.q) / kFullScale;
    }
    mean_i /= static_cast<float>(kLiveSpectrumSamples);
    mean_q /= static_cast<float>(kLiveSpectrumSamples);

    static SpectrumFft fft;
    if (!fft.plan) return false;
    for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
        fft.input[sample] = {
            (static_cast<float>(samples[sample].i) / kFullScale - mean_i) * window[sample],
            (static_cast<float>(samples[sample].q) / kFullScale - mean_q) * window[sample]};
    }
    if (fft_execute(fft.plan) != LIQUID_OK) {
        std::fprintf(stderr, "Spectrum FFT failed\n");
        return false;
    }
    for (size_t bin = 0; bin < kSpectrumBinCount; ++bin) {
        const size_t fft_bin = ((bin * kLiveSpectrumSamples + kSpectrumBinCount / 2)
                                / kSpectrumBinCount + kLiveSpectrumSamples / 2)
                               % kLiveSpectrumSamples;
        const auto value = fft.output[fft_bin];
        power[bin] = (value.real * value.real + value.imag * value.imag)
                   / static_cast<float>(kLiveSpectrumSamples * kLiveSpectrumSamples);
    }

    SpectrumFrame next{};
    std::array<float, kSpectrumBinCount> decibels{};
    std::array<bool, kSpectrumBinCount> valid{};
    std::array<float, kSpectrumBinCount> sorted{};
    size_t valid_count = 0;
    const int64_t span = live_spectrum_span.load();
    const int64_t half_span_scaled = span * static_cast<int64_t>(kSpectrumBinCount);
    for (size_t bin = 0; bin < next.size(); ++bin) {
        const int64_t frequency_scaled = (2 * static_cast<int64_t>(bin)
                                         - static_cast<int64_t>(kSpectrumBinCount)) * span
                                       + 2 * static_cast<int64_t>(kSpectrumBinCount) * shift_hz;
        valid[bin] = frequency_scaled >= -half_span_scaled && frequency_scaled < half_span_scaled;
        if (!valid[bin]) {
            live_spectrum_power[bin] = 0.0f;
            continue;
        }
        live_spectrum_power[bin] = live_spectrum_power[bin] * 0.6f + power[bin] * 0.4f;
        decibels[bin] = 10.0f * std::log10(std::max(live_spectrum_power[bin], 1.0e-16f));
        sorted[valid_count++] = decibels[bin];
    }
    // Auto-scale: track the noise floor (25th percentile) so the display stays useful at any gain.
    if (valid_count == 0) return false;
    std::nth_element(sorted.begin(), sorted.begin() + valid_count / 4, sorted.begin() + valid_count);
    static float floor_db = sorted[valid_count / 4];
    floor_db += 0.1f * (sorted[valid_count / 4] - floor_db);
    for (size_t bin = 0; bin < next.size(); ++bin) {
        if (!valid[bin]) continue;
        const float scaled = std::clamp((decibels[bin] - floor_db + 6.0f) * 5.0f, 0.0f, 255.0f);
        next[bin] = static_cast<uint8_t>(scaled);
    }
    live_spectrum_frame = next;
    frame = live_spectrum_frame;
    return true;
}

} // namespace platform::sdr