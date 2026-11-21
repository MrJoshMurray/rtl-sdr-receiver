/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/receive_demodulator.h"
#include "sdr/receive_audio_pipeline.h"
#include "sdr/capture_queue.h"
#include "sdr/spectrum_simulator.h"
#include "sdr/wfm_tuning.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

namespace {

constexpr uint32_t kSampleRateHz = 48000;
constexpr size_t kSampleCount = 12000;
constexpr size_t kTransientSamples = 2000;
constexpr float kPi = 3.14159265358979323846f;

int32_t quantize(float sample) {
    const float clipped = std::clamp(sample, -1.0f, 0.99999988f);
    return static_cast<int32_t>(std::lround(clipped * 8388608.0f));
}

float rms(const std::vector<float>& samples) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t index = kTransientSamples; index < samples.size(); ++index) {
        sum += static_cast<double>(samples[index]) * samples[index];
        ++count;
    }
    return static_cast<float>(std::sqrt(sum / count));
}

float tone_amplitude(const std::vector<float>& samples, float frequency_hz) {
    double real = 0.0;
    double imaginary = 0.0;
    size_t count = 0;
    for (size_t index = kTransientSamples; index < samples.size(); ++index) {
        const double phase = 2.0 * kPi * frequency_hz * index / kSampleRateHz;
        real += samples[index] * std::cos(phase);
        imaginary += samples[index] * std::sin(phase);
        ++count;
    }
    return static_cast<float>(2.0 * std::hypot(real, imaginary) / count);
}

void test_am_ssb_mode(platform::sdr::DemodMode mode,
                      liquid_ampmodem_type modulation,
                      int suppressed_carrier) {
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    auto modulator = ampmodem_create(0.5f, modulation, suppressed_carrier);
    assert(modulator != nullptr);

    for (size_t index = 0; index < kSampleCount; ++index) {
        const float input = 0.25f * std::sin(2.0f * kPi * 1000.0f * index / kSampleRateHz);
        liquid_float_complex signal{};
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);

    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = mode;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.01f);
    assert(tone_amplitude(audio, 1000.0f) > 0.03f);
    assert(tone_amplitude(audio, 5000.0f) < tone_amplitude(audio, 1000.0f) * 0.25f);
}

void test_cw_mode() {
    std::vector<platform::sdr::IqSample> iq(kSampleCount, {5000000, 0});
    std::vector<float> audio(kSampleCount);
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Cw;
    settings.cw_pitch_hz = 700;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.05f);
    assert(tone_amplitude(audio, 700.0f) > 0.1f);
    assert(tone_amplitude(audio, 1200.0f) < tone_amplitude(audio, 700.0f) * 0.25f);

    settings.audio_bandwidth_hz = 100;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(tone_amplitude(audio, 700.0f) > 0.05f);
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(tone_amplitude(audio, 700.0f) > 0.05f);
    for (size_t index = 0; index < iq.size(); ++index) {
        const float phase = 2.0f * kPi * 1000.0f * index / kSampleRateHz;
        iq[index] = {quantize(0.5f * std::cos(phase)), quantize(0.5f * std::sin(phase))};
    }
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) < 0.01f);
    for (size_t index = 0; index < iq.size(); ++index) {
        const float phase = 2.0f * kPi * 150.0f * index / kSampleRateHz;
        iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
    }
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float narrow = rms(audio);
    settings.audio_bandwidth_hz = 1200;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.05f);
    assert(narrow < rms(audio) * 0.1f);
}

void test_am_bandwidth() {
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Am;
    settings.audio_bandwidth_hz = 3000;
    std::string error;
    assert(demodulator.configure(settings, error));
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    const auto measure_tone = [&](float frequency) {
        demodulator.reset();
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * frequency * index / kSampleRateHz;
            iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return *demodulator.signal_level_dbfs();
    };
    const float in_band = measure_tone(500.0f);
    assert(measure_tone(3000.0f) < in_band - 20.0f);

    const auto measure_modulation = [&](uint32_t bandwidth) {
        settings.audio_bandwidth_hz = bandwidth;
        assert(demodulator.configure(settings, error));
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * 3000.0f * index / kSampleRateHz;
            iq[index] = {quantize(0.3f + 0.05f * std::cos(phase)), 0};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return tone_amplitude(audio, 3000.0f);
    };
    const float narrow_audio = measure_modulation(3000);
    const float wide_audio = measure_modulation(8000);
    assert(wide_audio > 0.02f);
    assert(narrow_audio < wide_audio * 0.1f);
}

void test_sideband_selection() {
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    auto modulator = ampmodem_create(0.5f, LIQUID_AMPMODEM_LSB, 1);
    assert(modulator);
    for (size_t index = 0; index < iq.size(); ++index) {
        liquid_float_complex signal{};
        const float input = 0.25f * std::sin(2.0f * kPi * 1000.0f * index / kSampleRateHz);
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    std::string error;
    settings.mode = platform::sdr::DemodMode::Usb;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float selected = tone_amplitude(audio, 1000.0f);
    settings.mode = platform::sdr::DemodMode::Lsb;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(selected > 0.03f);
    assert(tone_amplitude(audio, 1000.0f) < selected * 0.1f);

    modulator = ampmodem_create(0.5f, LIQUID_AMPMODEM_LSB, 1);
    assert(modulator);
    for (size_t index = 0; index < iq.size(); ++index) {
        liquid_float_complex signal{};
        const float input = 0.25f * std::sin(2.0f * kPi * 2400.0f * index / kSampleRateHz);
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);
    settings.mode = platform::sdr::DemodMode::Usb;
    settings.audio_bandwidth_hz = 1800;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float narrow = tone_amplitude(audio, 2400.0f);
    settings.audio_bandwidth_hz = 3000;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float wide = tone_amplitude(audio, 2400.0f);
    assert(wide > 0.03f);
    assert(narrow < wide * 0.1f);
}

void test_live_spectrum_tones() {
    constexpr size_t sample_count = 2048;
    std::vector<platform::sdr::IqSample> iq(sample_count);
    platform::sdr::SpectrumFrame frame{};
    assert(!platform::sdr::LiveSpectrumBuffer::snapshot(frame));

    for (const int direction : {1, -1}) {
        for (size_t sample = 0; sample < iq.size(); ++sample) {
            const float phase = 2.0f * kPi * direction * 0.25f * sample;
            iq[sample] = {quantize(0.15f * std::cos(phase)),
                          quantize(0.15f * std::sin(phase))};
        }
        platform::sdr::LiveSpectrumBuffer::push_iq(iq.data(), iq.size());
        for (int iteration = 0; iteration < 40; ++iteration) {
            assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        }
        const auto peak = std::max_element(frame.begin(), frame.end()) - frame.begin();
        assert(std::abs(static_cast<int>(peak) - (direction > 0 ? 216 : 72)) <= 6);
        assert(frame[peak] > 0);
    }
}

void test_live_spectrum_display_bins() {
    constexpr size_t sample_count = 2048;
    std::vector<platform::sdr::IqSample> iq(sample_count);
    platform::sdr::SpectrumFrame frame{};
    for (const int bin : {4, 71, 105, 183, 270, 284}) {
        platform::sdr::LiveSpectrumBuffer::set_blanked(true);
        assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        platform::sdr::LiveSpectrumBuffer::set_blanked(false);
        const float frequency = static_cast<float>(bin) / frame.size() - 0.5f;
        uint32_t random = 1;
        const auto noise = [&] {
            random = random * 1664525u + 1013904223u;
            return (static_cast<float>(random >> 8) / 16777216.0f - 0.5f) * 0.02f;
        };
        for (size_t sample = 0; sample < iq.size(); ++sample) {
            const float phase = 2.0f * kPi * frequency * sample;
            iq[sample] = {quantize(0.15f * std::cos(phase) + noise()),
                          quantize(0.15f * std::sin(phase) + noise())};
        }
        platform::sdr::LiveSpectrumBuffer::push_iq(iq.data(), iq.size());
        for (int iteration = 0; iteration < 60; ++iteration)
            assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        const auto peak = std::max_element(frame.begin(), frame.end()) - frame.begin();
        assert(std::abs(static_cast<int>(peak) - bin) <= 1);
        assert(frame[peak] > 0);
    }
}

void test_wfm_digital_tuning() {
    using platform::sdr::wfm_digital_offset;
    constexpr uint64_t center = 98000000;
    assert(wfm_digital_offset(center, center, false, 0) == 0);
    assert(wfm_digital_offset(center + 200000, center, false, 0) == 200000);
    assert(wfm_digital_offset(center - 200000, center, false, 0) == -200000);
    assert(!wfm_digital_offset(center + 200001, center, false, 0));
    assert(!wfm_digital_offset(center - 200001, center, false, 0));
    assert(!wfm_digital_offset(center, 0, false, 0));
    assert(!wfm_digital_offset(center, center, true, 0));
    assert(!wfm_digital_offset(center, center, false, 200000));

    for (const int offset : {-200000, -10, 10, 200000}) {
        auto nco = nco_crcf_create(LIQUID_NCO);
        assert(nco);
        const auto actual = wfm_digital_offset(center + offset, center, false, 0);
        assert(actual && *actual == offset);
        assert(nco_crcf_set_frequency(nco, 2.0f * kPi * *actual / 960000.0f) == LIQUID_OK);
        std::vector<liquid_float_complex> input(4096);
        std::vector<liquid_float_complex> output(input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            const float phase = 2.0f * kPi * offset * i / 960000.0f;
            input[i] = {std::cos(phase), std::sin(phase)};
        }
        assert(nco_crcf_mix_block_down(nco, input.data(), output.data(), output.size()) == LIQUID_OK);
        double mean_real = 0.0;
        double mean_imag = 0.0;
        for (const auto& sample : output) {
            mean_real += sample.real;
            mean_imag += sample.imag;
        }
        assert(std::hypot(mean_real, mean_imag) / output.size() > 0.99);
        nco_crcf_destroy(nco);
    }
}

void test_shifted_spectrum_edges() {
    std::vector<platform::sdr::IqSample> iq(2048);
    platform::sdr::SpectrumFrame frame{};
    platform::sdr::LiveSpectrumBuffer::set_span_hz(960000);
    for (const int direction : {-1, 1}) {
        const int valid_bin = direction > 0 ? 180 : 108;
        const int wrapped_bin = direction > 0 ? 230 : 58;
        for (size_t sample = 0; sample < iq.size(); ++sample) {
            const float valid_phase = 2.0f * kPi * (static_cast<float>(valid_bin) / 288 - 0.5f) * sample;
            const float wrapped_phase = 2.0f * kPi * (static_cast<float>(wrapped_bin) / 288 - 0.5f) * sample;
            iq[sample] = {quantize(0.15f * std::cos(valid_phase) + 0.3f * std::cos(wrapped_phase)),
                          quantize(0.15f * std::sin(valid_phase) + 0.3f * std::sin(wrapped_phase))};
        }
        platform::sdr::LiveSpectrumBuffer::set_blanked(true);
        assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        platform::sdr::LiveSpectrumBuffer::set_blanked(false);
        platform::sdr::LiveSpectrumBuffer::push_iq(iq.data(), iq.size(), direction * 200000);
        for (int i = 0; i < 40; ++i)
            assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        assert(frame[valid_bin] > 0);
        assert(frame[wrapped_bin] == 0);
    }
}

void test_retune_capture_queue() {
    platform::sdr::CaptureQueue queue;
    for (uint8_t value = 0; value < 10; ++value)
        queue.push(&value, 1, queue.generation(), 98000000, 0, false);
    const auto old_generation = queue.generation();
    queue.retune(true);
    for (uint8_t value = 6; value < 10; ++value) {
        const auto chunk = queue.pop();
        assert(chunk.bytes[0] == value);
        assert(chunk.generation != queue.generation());
        assert(chunk.target_hz == 98000000 && chunk.offset_hz == 0 && !chunk.direct);
    }
    const uint8_t late = 10;
    queue.push(&late, 1, old_generation, 98000000, 0, false);
    assert(queue.pop().generation != queue.generation());
    const uint8_t fresh = 11;
    queue.push(&fresh, 1, queue.generation(), 98100000, 200000, false);
    const auto new_chunk = queue.pop();
    assert(new_chunk.generation == queue.generation());
    assert(new_chunk.target_hz == 98100000 && new_chunk.offset_hz == 200000);
    queue.push(&fresh, 1, queue.generation(), 98100000, 200000, false);
    const auto previous = queue.generation();
    queue.retune(false);
    queue.push(&late, 1, previous, 98100000, 200000, false);
    assert(queue.empty());
    queue.push(&fresh, 1, queue.generation(), 9800000, 0, true);
    assert(queue.pop().generation == queue.generation());
}

class RecordingSink : public platform::audio::MonoAudioSink {
public:
    bool write(const int16_t* samples, size_t count, std::string&) override {
        audio.assign(samples, samples + count);
        return true;
    }
    std::vector<int16_t> audio;
};

void test_retune_audio_fade() {
    RecordingSink sink;
    platform::sdr::ReceiveAudioPipeline pipeline(sink);
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Am;
    std::string error;
    assert(pipeline.configure(settings, error));
    std::vector<platform::sdr::IqSample> iq(kSampleRateHz * 3 / 5);
    for (size_t sample = 0; sample < iq.size(); ++sample) {
        const float phase = 2.0f * kPi * 1000.0f * sample / kSampleRateHz;
        iq[sample] = {quantize(0.3f + 0.1f * std::sin(phase)), 0};
    }
    assert(pipeline.process_iq(iq.data(), iq.size(), error));
    const auto audio_rms = [&](size_t first, size_t last) {
        double energy = 0;
        for (size_t i = first; i < last; ++i)
            energy += static_cast<double>(sink.audio[i]) * sink.audio[i];
        return std::sqrt(energy / (last - first));
    };
    const double steady = audio_rms(iq.size() - 4800, iq.size());
    assert(steady > 1.0);
    for (int retune = 0; retune < 4; ++retune) {
        pipeline.reset();
        assert(pipeline.process_iq(iq.data(), 5760, error));
        assert(audio_rms(2880, 5760) > steady * 0.4);
    }
}

void test_signal_level() {
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Usb;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(!demodulator.signal_level_dbfs());
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    const auto measure_tone = [&](float frequency) {
        demodulator.reset();
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * frequency * index / kSampleRateHz;
            iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return *demodulator.signal_level_dbfs();
    };
    const float in_band = measure_tone(-1400.0f);
    assert(in_band > -22.0f && in_band < -18.0f);
    assert(measure_tone(6000.0f) < in_band - 30.0f);
    assert(measure_tone(-6000.0f) < in_band - 30.0f);
    assert(measure_tone(1400.0f) < in_band - 20.0f);
    demodulator.reset();
    assert(!demodulator.signal_level_dbfs());
    settings.mode = platform::sdr::DemodMode::Lsb;
    assert(demodulator.configure(settings, error));
    assert(measure_tone(1400.0f) > -22.0f);
    assert(measure_tone(-1400.0f) < -40.0f);
    settings.mode = platform::sdr::DemodMode::Am;
    assert(demodulator.configure(settings, error));
    assert(measure_tone(0.0f) > -22.0f);
}

} // namespace

int main() {
    test_am_ssb_mode(platform::sdr::DemodMode::Usb, LIQUID_AMPMODEM_LSB, 1);
    test_am_ssb_mode(platform::sdr::DemodMode::Lsb, LIQUID_AMPMODEM_USB, 1);
    test_am_ssb_mode(platform::sdr::DemodMode::Am, LIQUID_AMPMODEM_DSB, 0);
    test_cw_mode();
    test_am_bandwidth();
    test_sideband_selection();
    test_live_spectrum_tones();
    test_live_spectrum_display_bins();
    test_wfm_digital_tuning();
    test_shifted_spectrum_edges();
    test_retune_capture_queue();
    test_retune_audio_fade();
    test_signal_level();
}