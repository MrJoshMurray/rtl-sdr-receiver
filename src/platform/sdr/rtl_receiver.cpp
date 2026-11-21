/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "rtl_receiver.h"

#include "iq_types.h"
#include "radio_settings_adapter.h"
#include "receive_audio_pipeline.h"
#include "spectrum_simulator.h"

#include <liquid/liquid.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>


namespace platform::sdr {
namespace {

constexpr uint32_t kCaptureRateHz = 960000;
constexpr uint32_t kMidRateHz = 240000;
constexpr int kTunerOffsetHz = 200000;
constexpr uint64_t kTunerMinHz = 24000000;
constexpr size_t kMaxWriteFrames = 4096;
constexpr size_t kReadBytes = 16384;
constexpr float kPi = 3.14159265358979f;

struct RtlApi {
    void* handle{nullptr};
    uint32_t (*get_device_count)(void){nullptr};
    int (*open)(void**, uint32_t){nullptr};
    int (*close)(void*){nullptr};
    int (*set_sample_rate)(void*, uint32_t){nullptr};
    int (*set_center_freq)(void*, uint32_t){nullptr};
    int (*set_tuner_gain_mode)(void*, int){nullptr};
    int (*set_agc_mode)(void*, int){nullptr};
    int (*set_direct_sampling)(void*, int){nullptr};
    int (*reset_buffer)(void*){nullptr};
    int (*read_sync)(void*, void*, int, int*){nullptr};

    bool load() {
        for (const char* name : {"librtlsdr.so.0", "librtlsdr.so", "librtlsdr.dylib"}) {
            handle = dlopen(name, RTLD_NOW);
            if (handle) break;
        }
        if (!handle) return false;
        auto sym = [&](auto& fn, const char* n) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, n));
            return fn != nullptr;
        };
        return sym(get_device_count, "rtlsdr_get_device_count")
            && sym(open, "rtlsdr_open")
            && sym(close, "rtlsdr_close")
            && sym(set_sample_rate, "rtlsdr_set_sample_rate")
            && sym(set_center_freq, "rtlsdr_set_center_freq")
            && sym(set_tuner_gain_mode, "rtlsdr_set_tuner_gain_mode")
            && sym(set_agc_mode, "rtlsdr_set_agc_mode")
            && sym(set_direct_sampling, "rtlsdr_set_direct_sampling")
            && sym(reset_buffer, "rtlsdr_reset_buffer")
            && sym(read_sync, "rtlsdr_read_sync");
    }
};

} // namespace

RtlReceiver::~RtlReceiver() {
    stop();
}

void RtlReceiver::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { run(); });
}

void RtlReceiver::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void RtlReceiver::set_receive_settings(model::RadioMode mode, uint32_t bandwidth_hz) {
    mode_.store(static_cast<int>(mode));
    bandwidth_hz_.store(bandwidth_hz);
}

model::RadioMeasurements RtlReceiver::measurements() const {
    std::lock_guard<std::mutex> lock(measurements_mutex_);
    return measurements_;
}

void RtlReceiver::run() {
    RtlApi api;
    void* dev = nullptr;
    std::string error;

    // Wait for a dongle, retrying so it can be plugged in later.
    while (running_.load()) {
        if (!api.load()) {
            discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Error));
        } else if (api.get_device_count() == 0) {
            discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::NotFound));
        } else if (api.open(&dev, 0) != 0) {
            dev = nullptr;
            discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Error));
        } else {
            break;
        }
        for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!running_.load() || !dev) return;

    api.set_sample_rate(dev, kCaptureRateHz);
    api.set_tuner_gain_mode(dev, 0);
    api.set_agc_mode(dev, 1);
    api.reset_buffer(dev);

    // The sound server may still be starting after boot, so retry for a while.
    bool audio_ready = false;
    for (int attempt = 0; attempt < 60 && running_.load() && !audio_ready; ++attempt) {
        audio_ready = output_.open("", kDefaultRxSampleRateHz, error);
        if (!audio_ready) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!audio_ready) {
        api.close(dev);
        discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Error));
        running_.store(false);
        return;
    }
    discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Found));
    status_.store(static_cast<int>(model::RadioConnectionState::Receiving));

    ReceiveAudioPipeline pipeline(output_);
    nco_crcf shifter = nco_crcf_create(LIQUID_NCO);
    msresamp_crcf decim = msresamp_crcf_create(static_cast<float>(kMidRateHz) / kCaptureRateHz, 40.0f);
    msresamp_crcf narrow = msresamp_crcf_create(
        static_cast<float>(kDefaultRxSampleRateHz) / kMidRateHz, 40.0f);
    msresamp_rrrf audio_down = msresamp_rrrf_create(
        static_cast<float>(kDefaultRxSampleRateHz) / 240000.0f, 40.0f);
    firfilt_crcf nfm_filter = nullptr;
    uint32_t nfm_filter_bw = 0;
    freqdem nfm_demod = freqdem_create(5000.0f / kDefaultRxSampleRateHz);
    freqdem wfm_demod = freqdem_create(75000.0f / 240000.0f);

    std::vector<uint8_t> raw(kReadBytes);
    std::vector<liquid_float_complex> input(kReadBytes / 2);
    std::vector<liquid_float_complex> narrow_iq(kReadBytes / 2);
    std::vector<liquid_float_complex> wide_iq(kReadBytes / 2);
    std::vector<liquid_float_complex> mid_iq(kReadBytes / 2);
    std::vector<IqSample> pipeline_iq;
    std::vector<IqSample> spectrum_iq;
    std::vector<float> fm_audio;
    std::vector<float> fm_audio_48k;
    std::vector<int16_t> pcm;

    uint64_t tuned_hz = 0;
    int tuned_offset = 0;
    bool tuned_direct = false;
    int last_mode = -1;
    uint32_t last_bw = 0;
    size_t fade_in = 0;
    int settle_chunks = 0;
    double noise_floor_db = 0.0;
    bool noise_floor_valid = false;
    bool squelch_open = true;
    float gate = 1.0f;
    float deemph = 0.0f;
    auto last_publish = std::chrono::steady_clock::now();
    double level_sum = 0.0;
    size_t level_count = 0;

    // Reading in its own thread keeps USB transfers back to back while the
    // worker processes, otherwise the dongle overruns and audio underruns.
    std::mutex chunk_mutex;
    std::condition_variable chunk_cv;
    std::deque<std::vector<uint8_t>> chunks;
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> reader_failed{false};
    std::thread reader([&] {
        std::vector<uint8_t> buf(kReadBytes);
        int read_failures = 0;
        while (!reader_stop.load()) {
            int got = 0;
            if (api.read_sync(dev, buf.data(), static_cast<int>(buf.size()), &got) != 0 || got <= 0) {
                if (++read_failures > 20) break;
                continue;
            }
            read_failures = 0;
            {
                std::lock_guard<std::mutex> lock(chunk_mutex);
                if (chunks.size() < 64) chunks.emplace_back(buf.begin(), buf.begin() + got);
            }
            chunk_cv.notify_one();
        }
        reader_failed.store(true);
        chunk_cv.notify_one();
    });

    while (running_.load()) {
        const uint64_t frequency_hz = frequency_hz_.load();
        const int rit = rit_hz_.load();
        const auto mode = static_cast<model::RadioMode>(mode_.load());
        const uint32_t bandwidth = bandwidth_hz_.load();
        const bool direct = frequency_hz < kTunerMinHz;
        const int offset = (direct || mode == model::RadioMode::Wfm) ? 0 : kTunerOffsetHz;
        const uint64_t target = frequency_hz + rit;

        if (target != tuned_hz || offset != tuned_offset || direct != tuned_direct) {
            if (direct != tuned_direct) api.set_direct_sampling(dev, direct ? 2 : 0);
            api.set_center_freq(dev, static_cast<uint32_t>(target + offset));
            nco_crcf_set_frequency(shifter, 2.0f * kPi * static_cast<float>(offset) / kCaptureRateHz);
            if (target != tuned_hz) {
                pipeline.reset();
                freqdem_reset(nfm_demod);
                freqdem_reset(wfm_demod);
                deemph = 0.0f;
                fade_in = kDefaultRxSampleRateHz / 200;
                settle_chunks = 4;
                noise_floor_valid = false;
            }
            tuned_hz = target;
            tuned_offset = offset;
            tuned_direct = direct;
        }
        if (static_cast<int>(mode) != last_mode || bandwidth != last_bw) {
            last_mode = static_cast<int>(mode);
            last_bw = bandwidth;
            LiveSpectrumBuffer::set_span_hz(mode == model::RadioMode::Wfm ? kCaptureRateHz : kMidRateHz);
            if (mode != model::RadioMode::Wfm && mode != model::RadioMode::Nfm) {
                pipeline.configure_if_changed(make_demodulator_settings(mode, bandwidth), error);
            }
        }
        pipeline.set_auto_notch(auto_notch_.load());

        int n_read = 0;
        {
            std::unique_lock<std::mutex> lock(chunk_mutex);
            chunk_cv.wait_for(lock, std::chrono::milliseconds(200),
                              [&] { return !chunks.empty() || reader_failed.load(); });
            if (chunks.empty()) {
                if (reader_failed.load()) break;
                continue;
            }
            n_read = static_cast<int>(chunks.front().size());
            std::copy(chunks.front().begin(), chunks.front().end(), raw.begin());
            chunks.pop_front();
        }
        // Audio keeps flowing through a retune; only the waterfall skips the settling data.
        const bool settling = settle_chunks > 0;
        if (settling) --settle_chunks;
        const size_t count = static_cast<size_t>(n_read) / 2;
        for (size_t i = 0; i < count; ++i) {
            input[i] = liquid_float_complex{(raw[2 * i] - 127.5f) / 127.5f,
                                            (raw[2 * i + 1] - 127.5f) / 127.5f};
        }
        if (offset != 0) nco_crcf_mix_block_down(shifter, input.data(), input.data(), static_cast<unsigned>(count));

        const bool wfm = mode == model::RadioMode::Wfm;
        unsigned mid_count = 0;
        msresamp_crcf_execute(decim, input.data(), static_cast<unsigned>(count), mid_iq.data(), &mid_count);
        if (mid_count == 0) continue;

        const liquid_float_complex* view_src = wfm ? input.data() : mid_iq.data();
        const size_t view_count = wfm ? count : mid_count;
        spectrum_iq.resize(view_count);
        for (size_t i = 0; i < view_count; ++i) {
            spectrum_iq[i] = {static_cast<int32_t>(view_src[i].real * 8388607.0f),
                              static_cast<int32_t>(view_src[i].imag * 8388607.0f)};
        }
        if (!settling) LiveSpectrumBuffer::push_iq(spectrum_iq.data(), view_count);

        unsigned produced = 0;
        auto& channel = wfm ? wide_iq : narrow_iq;
        if (wfm) {
            std::copy(mid_iq.begin(), mid_iq.begin() + mid_count, channel.begin());
            produced = mid_count;
        } else {
            msresamp_crcf_execute(narrow, mid_iq.data(), mid_count, channel.data(), &produced);
        }
        if (produced == 0) continue;

        pipeline_iq.resize(produced);
        for (unsigned i = 0; i < produced; ++i) {
            level_sum += channel[i].real * channel[i].real + channel[i].imag * channel[i].imag;
            pipeline_iq[i] = {static_cast<int32_t>(channel[i].real * 8388607.0f),
                              static_cast<int32_t>(channel[i].imag * 8388607.0f)};
        }
        level_count += produced;

        if (mode == model::RadioMode::Wfm || mode == model::RadioMode::Nfm) {
            std::vector<float>* audio = &fm_audio;
            fm_audio.resize(produced);
            if (mode == model::RadioMode::Nfm) {
                if (!nfm_filter || nfm_filter_bw != bandwidth) {
                    if (nfm_filter) firfilt_crcf_destroy(nfm_filter);
                    nfm_filter = firfilt_crcf_create_kaiser(
                        51, std::min(0.45f, bandwidth / 2.0f / kDefaultRxSampleRateHz), 60.0f, 0.0f);
                    nfm_filter_bw = bandwidth;
                }
                firfilt_crcf_execute_block(nfm_filter, channel.data(), produced, narrow_iq.data());
                freqdem_demodulate_block(nfm_demod, narrow_iq.data(), produced, fm_audio.data());
            } else {
                freqdem_demodulate_block(wfm_demod, channel.data(), produced, fm_audio.data());
                constexpr float alpha = 0.08f; // 50 us de-emphasis at 240 kS/s
                for (auto& s : fm_audio) {
                    deemph += alpha * (s - deemph);
                    s = deemph;
                }
                fm_audio_48k.resize(produced / 4 + 16);
                unsigned out = 0;
                msresamp_rrrf_execute(audio_down, fm_audio.data(), produced, fm_audio_48k.data(), &out);
                fm_audio_48k.resize(out);
                audio = &fm_audio_48k;
            }
            pcm.resize(audio->size());
            for (size_t i = 0; i < audio->size(); ++i) {
                float gain = 0.5f;
                if (fade_in > 0) {
                    gain *= 1.0f - static_cast<float>(fade_in) / (kDefaultRxSampleRateHz / 200);
                    --fade_in;
                }
                gate += ((squelch_open ? 1.0f : 0.0f) - gate) * 0.01f;
                const float v = std::clamp((*audio)[i] * gain * gate, -1.0f, 1.0f);
                pcm[i] = static_cast<int16_t>(v * 32767.0f);
            }
            for (size_t off = 0; off < pcm.size(); off += kMaxWriteFrames) {
                output_.write(pcm.data() + off, std::min(kMaxWriteFrames, pcm.size() - off), error);
            }
        } else {
            for (auto& s : pipeline_iq) {
                gate += ((squelch_open ? 1.0f : 0.0f) - gate) * 0.01f;
                s.i = static_cast<int32_t>(s.i * gate);
                s.q = static_cast<int32_t>(s.q * gate);
            }
            pipeline.process_iq(pipeline_iq.data(), produced, error);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_publish >= std::chrono::milliseconds(100) && level_count > 0) {
            model::RadioMeasurements next;
            const double rms = std::sqrt(level_sum / level_count);
            // The dongle's AGC keeps absolute level near-constant, so report the level
            // above a tracked noise floor (drops fast, creeps up slowly), 3 dB per S unit.
            const double level_db = 20.0 * std::log10(std::max(rms, 1e-6));
            if (!noise_floor_valid || level_db < noise_floor_db) {
                noise_floor_db = level_db;
                noise_floor_valid = true;
            } else {
                noise_floor_db += 0.05;
            }
            const int squelch_level = squelch_.load();
            const double above_floor_db = level_db - noise_floor_db;
            const double threshold_db = 3.0 * squelch_level;
            if (squelch_level == 0) squelch_open = true;
            else if (above_floor_db >= threshold_db) squelch_open = true;
            else if (above_floor_db < threshold_db - 1.5) squelch_open = false;
            constexpr double kS9AboveFloorDb = 27.0;
            next.signal_level_dbfs = static_cast<int>(std::lround(
                model::kS9ReferenceDbfs + (level_db - noise_floor_db) - kS9AboveFloorDb));
            level_sum = 0.0;
            level_count = 0;
            last_publish = now;
            std::lock_guard<std::mutex> lock(measurements_mutex_);
            measurements_ = next;
        }
    }

    status_.store(static_cast<int>(model::RadioConnectionState::Disconnected));
    if (nfm_filter) firfilt_crcf_destroy(nfm_filter);
    freqdem_destroy(nfm_demod);
    freqdem_destroy(wfm_demod);
    reader_stop.store(true);
    reader.join();
    msresamp_rrrf_destroy(audio_down);
    msresamp_crcf_destroy(narrow);
    msresamp_crcf_destroy(decim);
    nco_crcf_destroy(shifter);
    output_.close();
    api.close(dev);
    {
        std::lock_guard<std::mutex> lock(measurements_mutex_);
        measurements_ = {};
    }
    discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Error));
}

} // namespace platform::sdr
