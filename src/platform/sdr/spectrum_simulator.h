/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "iq_types.h"

namespace platform::sdr {

constexpr size_t kSpectrumBinCount = 288;

using SpectrumFrame = std::array<uint8_t, kSpectrumBinCount>;

SpectrumFrame make_simulated_spectrum(uint32_t frame_index) noexcept;

class LiveSpectrumBuffer {
public:
	static void push_iq(const IqSample* samples, size_t sample_count, int32_t shift_hz = 0) noexcept;
	static bool snapshot(SpectrumFrame& frame) noexcept;
	// While blanked, snapshots return an empty spectrum and drop the smoothing history.
	static void set_blanked(bool blanked) noexcept;
	// Width in Hz of the I/Q fed to push_iq (the spectrum's full span).
	static void set_span_hz(uint32_t span_hz) noexcept;
	static uint32_t span_hz() noexcept;
};

} // namespace platform::sdr