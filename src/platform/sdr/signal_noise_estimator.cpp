/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "signal_noise_estimator.h"

#include <liquid/liquid.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace platform::sdr {
namespace {

constexpr size_t kFftSize = 2048;
constexpr size_t kMaxFrames = 4;
constexpr size_t kNoiseGroupBins = 16;
constexpr size_t kMinNoiseGroups = 8;
constexpr double kPi = 3.14159265358979323846;

} // namespace

struct SignalNoiseEstimator::Impl {
    std::array<IqSample, kFftSize * kMaxFrames> samples{};
    std::array<liquid_float_complex, kFftSize> input{};
    std::array<liquid_float_complex, kFftSize> output{};
    std::array<float, kFftSize> window{};
    fftplan fft{nullptr};
    size_t written{0};
    size_t count{0};
    uint32_t sample_rate_hz{0};
    uint32_t bandwidth_hz{0};
    model::RadioMode mode{model::RadioMode::Wfm};
    int32_t capture_shift_hz{0};

    Impl() {
        for (size_t i = 0; i < window.size(); ++i)
            window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / kFftSize));
        fft = fft_create_plan(kFftSize, input.data(), output.data(), LIQUID_FFT_FORWARD, 0);
        if (!fft) std::fprintf(stderr, "RTL-SDR signal/noise FFT allocation failed\n");
    }

    ~Impl() {
        if (fft) fft_destroy_plan(fft);
    }
};

SignalNoiseEstimator::SignalNoiseEstimator() : impl_(std::make_unique<Impl>()) {}
SignalNoiseEstimator::~SignalNoiseEstimator() = default;

void SignalNoiseEstimator::reset(uint32_t sample_rate_hz, uint32_t bandwidth_hz,
                                 model::RadioMode mode, int32_t capture_shift_hz) {
    impl_->sample_rate_hz = sample_rate_hz;
    impl_->bandwidth_hz = bandwidth_hz;
    impl_->mode = mode;
    impl_->capture_shift_hz = capture_shift_hz;
    impl_->written = 0;
    impl_->count = 0;
}

void SignalNoiseEstimator::push(const IqSample* samples, size_t count) {
    if (!samples) return;
    for (size_t i = 0; i < count; ++i) {
        impl_->samples[impl_->written] = samples[i];
        impl_->written = (impl_->written + 1) % impl_->samples.size();
        impl_->count = std::min(impl_->count + 1, impl_->samples.size());
    }
}

std::optional<SignalNoiseEstimate> SignalNoiseEstimator::estimate() {
    auto& state = *impl_;
    if (!state.fft || !state.sample_rate_hz || !state.bandwidth_hz
        || state.bandwidth_hz >= state.sample_rate_hz || state.count < 2 * kFftSize) {
        return std::nullopt;
    }

    const size_t frames = std::min(state.count / kFftSize, kMaxFrames);
    const size_t start = (state.written + state.samples.size() - frames * kFftSize)
                       % state.samples.size();
    std::array<double, kFftSize> power{};
    for (size_t frame = 0; frame < frames; ++frame) {
        for (size_t i = 0; i < kFftSize; ++i) {
            const auto& iq = state.samples[(start + frame * kFftSize + i) % state.samples.size()];
            const float scale = state.window[i] / 8388608.0f;
            state.input[i] = {iq.i * scale, iq.q * scale};
        }
        if (fft_execute(state.fft) != LIQUID_OK) {
            std::fprintf(stderr, "RTL-SDR signal/noise FFT failed\n");
            return std::nullopt;
        }
        for (size_t i = 0; i < kFftSize; ++i) {
            const auto& bin = state.output[i];
            power[i] += (static_cast<double>(bin.real) * bin.real
                       + static_cast<double>(bin.imag) * bin.imag)
                      / (frames * static_cast<double>(kFftSize) * kFftSize);
        }
    }

    const double bin_hz = static_cast<double>(state.sample_rate_hz) / kFftSize;
    double low_hz = -static_cast<double>(state.bandwidth_hz) / 2.0;
    double high_hz = -low_hz;
    if (state.mode == model::RadioMode::Usb) {
        low_hz = -static_cast<double>(state.bandwidth_hz);
        high_hz = 0.0;
    } else if (state.mode == model::RadioMode::Lsb) {
        low_hz = 0.0;
        high_hz = state.bandwidth_hz;
    }
    const double guard_hz = std::max(std::min(state.bandwidth_hz * 0.15, 2500.0), 4.0 * bin_hz);
    const double usable_low = -0.4 * state.sample_rate_hz - state.capture_shift_hz;
    const double usable_high = 0.4 * state.sample_rate_hz - state.capture_shift_hz;
    if (low_hz < usable_low || high_hz > usable_high) return std::nullopt;
    double channel_power = 0.0;
    size_t channel_bins = 0;
    double group_power = 0.0;
    size_t group_count = 0;
    std::vector<double> noise_groups;
    for (int bin = -static_cast<int>(kFftSize / 2); bin < static_cast<int>(kFftSize / 2); ++bin) {
        const double hz = bin * bin_hz;
        const size_t fft_bin = (bin + static_cast<int>(kFftSize)) % kFftSize;
        const double value = power[fft_bin];
        const bool tuner_dc = state.mode == model::RadioMode::Wfm
                           && std::abs(hz + state.capture_shift_hz) <= 1.5 * bin_hz;
        if (!tuner_dc && hz >= low_hz && hz <= high_hz) {
            channel_power += value;
            ++channel_bins;
        }
        if (tuner_dc || hz < usable_low || hz > usable_high
            || (hz >= low_hz - guard_hz && hz <= high_hz + guard_hz)) {
            group_power = 0.0;
            group_count = 0;
            continue;
        }
        group_power += value;
        if (++group_count == kNoiseGroupBins) {
            noise_groups.push_back(group_power / kNoiseGroupBins);
            group_power = 0.0;
            group_count = 0;
        }
    }
    if (channel_bins < 3 || noise_groups.size() < kMinNoiseGroups) return std::nullopt;
    std::sort(noise_groups.begin(), noise_groups.end());
    // The lower quartile avoids letting a few occupied adjacent channels set the noise reference.
    const double noise_per_bin = noise_groups[noise_groups.size() / 4];
    const double median = noise_groups[noise_groups.size() / 2];
    if (!std::isfinite(noise_per_bin) || noise_per_bin <= 1e-12
        || median > noise_per_bin * 4.0 || !std::isfinite(channel_power)) {
        return std::nullopt;
    }
    const double noise = noise_per_bin * channel_bins;
    const double signal = channel_power - noise;
    if (signal <= 0.0) return SignalNoiseEstimate{0.0f, true};
    return SignalNoiseEstimate{static_cast<float>(10.0 * std::log10(signal / noise)), false};
}

} // namespace platform::sdr
