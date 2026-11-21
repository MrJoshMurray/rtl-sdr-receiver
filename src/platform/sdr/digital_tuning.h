/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>

namespace platform::sdr {

// Narrowband's 200 kHz tuner offset stays at least 100 kHz from the channel,
// while the full 240 kHz displayed span remains inside the 960 kHz capture.
constexpr int kNarrowTunerOffsetHz = 200000;
constexpr uint64_t kWfmDigitalTuningLimitHz = 200000;
constexpr uint64_t kNarrowDigitalTuningLimitHz = 100000;

inline std::optional<int> digital_tuning_offset(uint64_t target_hz, uint64_t capture_target_hz,
                                                bool capture_direct, int capture_offset_hz,
                                                bool desired_direct, int desired_offset_hz) {
    if (capture_target_hz == 0 || capture_direct != desired_direct
        || capture_offset_hz != desired_offset_hz)
        return std::nullopt;
    if (desired_direct && desired_offset_hz != 0) return std::nullopt;
    if (desired_offset_hz != 0 && desired_offset_hz != kNarrowTunerOffsetHz)
        return std::nullopt;
    const uint64_t limit = desired_offset_hz == 0 ? kWfmDigitalTuningLimitHz
                                                   : kNarrowDigitalTuningLimitHz;
    int difference = 0;
    if (target_hz >= capture_target_hz) {
        const uint64_t distance = target_hz - capture_target_hz;
        if (distance > limit) return std::nullopt;
        difference = static_cast<int>(distance);
    } else {
        const uint64_t distance = capture_target_hz - target_hz;
        if (distance > limit) return std::nullopt;
        difference = -static_cast<int>(distance);
    }
    return difference - capture_offset_hz;
}

} // namespace platform::sdr
