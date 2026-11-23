/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "signal_noise_estimator.h"

#include <optional>

namespace platform::sdr {

inline bool squelch_is_open(bool was_open, int threshold_db,
                            const std::optional<SignalNoiseEstimate>& estimate) {
    if (threshold_db == 0) return true;
    if (!estimate || estimate->below_noise) return false;
    if (estimate->db >= threshold_db) return true;
    if (estimate->db < threshold_db - 1.5f) return false;
    return was_open;
}

} // namespace platform::sdr
