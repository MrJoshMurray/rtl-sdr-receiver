/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>

namespace platform::sdr {

// At 960 kS/s this leaves at least 280 kHz on each side of the tuned
// frequency, including the 180 kHz broadcast FM channel and filter margin.
constexpr uint64_t kWfmDigitalTuningLimitHz = 200000;

inline std::optional<int> wfm_digital_offset(uint64_t target_hz, uint64_t center_hz,
                                             bool direct, int hardware_offset_hz) {
    if (center_hz == 0 || direct || hardware_offset_hz != 0) return std::nullopt;
    if (target_hz >= center_hz) {
        const uint64_t difference = target_hz - center_hz;
        if (difference <= kWfmDigitalTuningLimitHz) return static_cast<int>(difference);
    } else {
        const uint64_t difference = center_hz - target_hz;
        if (difference <= kWfmDigitalTuningLimitHz) return -static_cast<int>(difference);
    }
    return std::nullopt;
}

} // namespace platform::sdr
