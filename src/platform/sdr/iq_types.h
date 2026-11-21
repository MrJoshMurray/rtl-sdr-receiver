/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace platform::sdr {

constexpr uint32_t kDefaultRxSampleRateHz = 48000;

// I/Q scaled to signed 24-bit full scale (+/-8388608).
struct IqSample {
    int32_t i{0};
    int32_t q{0};
};

} // namespace platform::sdr
