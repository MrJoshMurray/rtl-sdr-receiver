/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "audio_output.h"
#include "base_model.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace platform::sdr {

// Receives from an RTL-SDR USB dongle (librtlsdr is loaded at run time) and plays the
// demodulated audio. Runs its own thread; all setters are thread safe.
class RtlReceiver {
public:
    ~RtlReceiver();

    void start();
    void stop();

    void set_frequency(uint64_t frequency_hz) { frequency_hz_.store(frequency_hz); }
    void set_receive_settings(model::RadioMode mode, uint32_t bandwidth_hz);
    void set_auto_notch(bool enabled) { auto_notch_.store(enabled); }
    void set_squelch_threshold_db(int threshold_db) { squelch_threshold_db_.store(threshold_db); }
    void set_rit_hz(int hz) { rit_hz_.store(hz); }
    void request_band_edge_tone() { output_.request_band_edge_tone(); }

    model::RadioConnectionState status() const {
        return static_cast<model::RadioConnectionState>(status_.load());
    }
    model::RadioDiscoveryState discovery_state() const {
        return static_cast<model::RadioDiscoveryState>(discovery_state_.load());
    }
    model::RadioMeasurements measurements() const;

private:
    void run();

    audio::AudioOutput output_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> status_{static_cast<int>(model::RadioConnectionState::Disconnected)};
    std::atomic<int> discovery_state_{static_cast<int>(model::RadioDiscoveryState::Searching)};
    std::atomic<uint64_t> frequency_hz_{98000000};
    std::atomic<int> mode_{static_cast<int>(model::RadioMode::Wfm)};
    std::atomic<uint32_t> bandwidth_hz_{180000};
    std::atomic<bool> auto_notch_{false};
    std::atomic<int> rit_hz_{0};
    std::atomic<int> squelch_threshold_db_{0};
    mutable std::mutex measurements_mutex_;
    model::RadioMeasurements measurements_{};
};

} // namespace platform::sdr
