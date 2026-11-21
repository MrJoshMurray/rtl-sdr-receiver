/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "receive_audio_pipeline.h"

#include "radio_settings_adapter.h"

#include <algorithm>
#include <cmath>

#include <liquid/liquid.h>

namespace platform::sdr {
namespace {

constexpr float kAudioPeakLimit = 0.08f;
constexpr float kAudioAgcScale = 0.04f;
constexpr float kAudioAgcMaxGain = 100000.0f;
constexpr float kAudioAgcBandwidth = 0.0005f;

int16_t to_pcm16(float sample) {
    const float clipped = std::clamp(sample, -1.0f, 0.9999695f);
    return static_cast<int16_t>(std::lrint(clipped * 32768.0f));
}

} // namespace

ReceiveAudioPipeline::ReceiveAudioPipeline(audio::MonoAudioSink& output)
    : output_(output) {}

ReceiveAudioPipeline::~ReceiveAudioPipeline() {
    if (audio_agc_) agc_rrrf_destroy(audio_agc_);
}

bool ReceiveAudioPipeline::configure(const model::BaseModel& model, std::string& error) {
    return configure(make_demodulator_settings(model), error);
}

bool ReceiveAudioPipeline::configure(const DemodulatorSettings& settings, std::string& error) {
    if (!demodulator_.configure(settings, error)) return false;
    if (!audio_agc_) audio_agc_ = agc_rrrf_create();
    if (!audio_agc_) {
        error = "Liquid-DSP could not allocate the audio gain control";
        return false;
    }
    agc_rrrf_reset(audio_agc_);
    agc_rrrf_set_bandwidth(audio_agc_, kAudioAgcBandwidth);
    agc_rrrf_set_scale(audio_agc_, kAudioAgcScale);
    audio_level_ = 0.0f;
    fade_samples_ = settings.sample_rate_hz / 2;
    fade_position_ = 0;
    settings_ = settings;
    return true;
}

bool ReceiveAudioPipeline::configure_if_changed(const DemodulatorSettings& settings, std::string& error) {
    if (settings_ && settings_->mode == settings.mode
        && settings_->sample_rate_hz == settings.sample_rate_hz
        && settings_->cw_pitch_hz == settings.cw_pitch_hz
        && settings_->audio_bandwidth_hz == settings.audio_bandwidth_hz) {
        error.clear();
        return true;
    }
    return configure(settings, error);
}

bool ReceiveAudioPipeline::process_iq(const IqSample* samples, size_t count, std::string& error) {
    if (audio_float_.size() < count) {
        audio_float_.resize(count);
        audio_pcm_.resize(count);
    }
    if (!demodulator_.process(samples, count, audio_float_.data(), audio_float_.size(), error)) {
        return false;
    }

    if (notch_enabled_) notch_.process(audio_float_.data(), count);
    if (!audio_agc_) {
        error = "Audio gain control is not configured";
        return false;
    }
    if (count > 0) {
        float energy = 0.0f;
        for (size_t index = 0; index < count; ++index) {
            energy += audio_float_[index] * audio_float_[index];
        }
        const float packet_level = std::sqrt(energy / static_cast<float>(count));
        audio_level_ = std::max(packet_level, audio_level_ * 0.90f);
        agc_rrrf_set_signal_level(audio_agc_, std::max(audio_level_, 1.0f / kAudioAgcMaxGain));
    }
    for (size_t index = 0; index < count; ++index) {
        float adjusted = 0.0f;
        agc_rrrf_execute(audio_agc_, audio_float_[index], &adjusted);
        const float fade = static_cast<float>(fade_position_)
                         / static_cast<float>(fade_samples_);
        if (fade_position_ < fade_samples_) ++fade_position_;
        audio_pcm_[index] = to_pcm16(std::clamp(adjusted, -kAudioPeakLimit, kAudioPeakLimit) * fade);
    }
    return output_.write(audio_pcm_.data(), count, error);
}

std::optional<float> ReceiveAudioPipeline::signal_level_dbfs() const {
    return demodulator_.signal_level_dbfs();
}

void ReceiveAudioPipeline::set_auto_notch(bool enabled) {
    if (enabled && !notch_enabled_) notch_.reset();
    notch_enabled_ = enabled;
}

void ReceiveAudioPipeline::reset() {
    notch_.reset();
    demodulator_.reset();
    if (audio_agc_) {
        agc_rrrf_reset(audio_agc_);
        agc_rrrf_set_scale(audio_agc_, kAudioAgcScale);
    }
    audio_level_ = 0.0f;
    if (settings_) fade_samples_ = std::max<size_t>(1, settings_->sample_rate_hz / 200);
    fade_position_ = 0;
    std::fill(audio_float_.begin(), audio_float_.end(), 0.0f);
    std::fill(audio_pcm_.begin(), audio_pcm_.end(), int16_t{0});
}

} // namespace platform::sdr