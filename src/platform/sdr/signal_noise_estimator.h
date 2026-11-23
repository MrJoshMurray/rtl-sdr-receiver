/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_model.h"
#include "iq_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace platform::sdr {

struct SignalNoiseEstimate {
    float db{0.0f};
    bool below_noise{false};
};

class SignalNoiseEstimator {
public:
    SignalNoiseEstimator();
    ~SignalNoiseEstimator();

    SignalNoiseEstimator(const SignalNoiseEstimator&) = delete;
    SignalNoiseEstimator& operator=(const SignalNoiseEstimator&) = delete;

    void reset(uint32_t sample_rate_hz, uint32_t bandwidth_hz, model::RadioMode mode,
               int32_t capture_shift_hz = 0);
    void push(const IqSample* samples, size_t count);
    std::optional<SignalNoiseEstimate> estimate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform::sdr
