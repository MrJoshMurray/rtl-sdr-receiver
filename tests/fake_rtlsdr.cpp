/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {
std::atomic<int> reads_active{0};
std::atomic<int> tuning_active{0};
std::atomic<int> overlaps{0};
std::atomic<uint32_t> frequency{0};
std::atomic<int> frequency_changes{0};
}

extern "C" {
uint32_t rtlsdr_get_device_count() { return 1; }
int rtlsdr_open(void** dev, uint32_t) { *dev = &frequency; return 0; }
int rtlsdr_close(void*) { return 0; }
int rtlsdr_set_sample_rate(void*, uint32_t) { return 0; }
int rtlsdr_set_tuner_gain_mode(void*, int) { return 0; }
int rtlsdr_set_agc_mode(void*, int) { return 0; }
int rtlsdr_set_direct_sampling(void*, int) { return 0; }
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
    std::fill_n(static_cast<uint8_t*>(buffer), size, uint8_t{127});
    *got = size;
    --reads_active;
    return 0;
}
int fake_rtlsdr_overlaps() { return overlaps.load(); }
uint32_t fake_rtlsdr_frequency() { return frequency.load(); }
int fake_rtlsdr_frequency_changes() { return frequency_changes.load(); }
}
