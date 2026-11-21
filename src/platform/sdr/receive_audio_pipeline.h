/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "audio_output.h"
#include "auto_notch.h"
#include "iq_types.h"
#include "receive_demodulator.h"

#include "base_model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct agc_rrrf_s;

namespace platform::sdr {

class ReceiveAudioPipeline {
public:
    explicit ReceiveAudioPipeline(audio::MonoAudioSink& output);
    ~ReceiveAudioPipeline();

    ReceiveAudioPipeline(const ReceiveAudioPipeline&) = delete;
    ReceiveAudioPipeline& operator=(const ReceiveAudioPipeline&) = delete;

    bool configure(const model::BaseModel& model, std::string& error);
    bool configure(const DemodulatorSettings& settings, std::string& error);
    bool configure_if_changed(const DemodulatorSettings& settings, std::string& error);
    // Demodulates and plays one block of channel-rate I/Q.
    bool process_iq(const IqSample* samples, size_t count, std::string& error);
    std::optional<float> signal_level_dbfs() const;
    void set_auto_notch(bool enabled);
    void reset();

private:
    audio::MonoAudioSink& output_;
    ReceiveDemodulator demodulator_;
    std::optional<DemodulatorSettings> settings_;
    agc_rrrf_s* audio_agc_{nullptr};
    AutoNotch notch_;
    bool notch_enabled_{false};
    float audio_level_{0.0f};
    size_t fade_samples_{0};
    size_t fade_position_{0};
    std::vector<float> audio_float_;
    std::vector<int16_t> audio_pcm_;
};

} // namespace platform::sdr