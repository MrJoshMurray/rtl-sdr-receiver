/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {
std::atomic<int> reads_active{0};
std::atomic<int> tuning_active{0};
std::atomic<int> overlaps{0};
std::atomic<uint32_t> frequency{0};
std::atomic<int> frequency_changes{0};
std::atomic<int> direct_sampling{0};
std::atomic<uint32_t> signal_frequency{0};
double phase = 0.0;
uint32_t random_state = 1;
}

extern "C" {
uint32_t rtlsdr_get_device_count() { return 1; }
int rtlsdr_open(void** dev, uint32_t) { *dev = &frequency; return 0; }
int rtlsdr_close(void*) { return 0; }
int rtlsdr_set_sample_rate(void*, uint32_t) { return 0; }
int rtlsdr_set_tuner_gain_mode(void*, int) { return 0; }
int rtlsdr_set_agc_mode(void*, int) { return 0; }
int rtlsdr_set_direct_sampling(void*, int mode) { direct_sampling.store(mode); return 0; }
int rtlsdr_reset_buffer(void*) { return 0; }
int rtlsdr_set_center_freq(void*, uint32_t hz) {
    ++tuning_active;
    if (reads_active.load()) ++overlaps;
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    frequency.store(hz);
    ++frequency_changes;
    --tuning_active;
    return 0;
}
int rtlsdr_read_sync(void*, void* buffer, int size, int* got) {
    ++reads_active;
    if (tuning_active.load()) ++overlaps;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    auto* bytes = static_cast<uint8_t*>(buffer);
    const uint32_t signal = signal_frequency.load();
    if (signal == 0) {
        std::fill_n(bytes, size, uint8_t{127});
    } else {
        constexpr double pi = 3.14159265358979323846;
        const double step = 2.0 * pi * (static_cast<int64_t>(signal) - frequency.load()) / 960000.0;
        for (int i = 0; i + 1 < size; i += 2) {
            random_state = random_state * 1664525u + 1013904223u;
            const double noise_i = static_cast<int>(random_state >> 29) - 3.5;
            random_state = random_state * 1664525u + 1013904223u;
            const double noise_q = static_cast<int>(random_state >> 29) - 3.5;
            bytes[i] = static_cast<uint8_t>(127.5 + 60.0 * std::cos(phase) + noise_i);
            bytes[i + 1] = static_cast<uint8_t>(127.5 + 60.0 * std::sin(phase) + noise_q);
            phase = std::remainder(phase + step, 2.0 * pi);
        }
    }
    *got = size;
    --reads_active;
    return 0;
}
int fake_rtlsdr_overlaps() { return overlaps.load(); }
uint32_t fake_rtlsdr_frequency() { return frequency.load(); }
int fake_rtlsdr_frequency_changes() { return frequency_changes.load(); }
int fake_rtlsdr_direct_sampling() { return direct_sampling.load(); }
void fake_rtlsdr_set_signal(uint32_t hz) { signal_frequency.store(hz); }
}
