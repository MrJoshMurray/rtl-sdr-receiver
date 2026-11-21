/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/rtl_receiver.h"
#include "sdr/spectrum_simulator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
    void* fake = dlopen(argv[1], RTLD_NOW);
    assert(fake);
    auto overlaps = reinterpret_cast<int (*)()>(dlsym(fake, "fake_rtlsdr_overlaps"));
    auto frequency = reinterpret_cast<uint32_t (*)()>(dlsym(fake, "fake_rtlsdr_frequency"));
    auto frequency_changes = reinterpret_cast<int (*)()>(dlsym(fake, "fake_rtlsdr_frequency_changes"));
    auto direct_sampling = reinterpret_cast<int (*)()>(dlsym(fake, "fake_rtlsdr_direct_sampling"));
    auto set_signal = reinterpret_cast<void (*)(uint32_t)>(dlsym(fake, "fake_rtlsdr_set_signal"));
    assert(overlaps && frequency && frequency_changes && direct_sampling && set_signal);

    platform::sdr::RtlReceiver receiver;
    receiver.set_receive_settings(model::RadioMode::Wfm, 180000);
    receiver.set_frequency(98000000);
    receiver.start();
    for (int i = 0; i < 200 && receiver.status() != model::RadioConnectionState::Receiving; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(receiver.status() == model::RadioConnectionState::Receiving);

    const auto wait_for_frequency = [&](uint32_t expected) {
        for (int i = 0; i < 200 && frequency() != expected; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(frequency() == expected);
    };
    wait_for_frequency(98000000);
    const int initial_changes = frequency_changes();
    for (uint32_t step = 1; step <= 20; ++step) {
        receiver.set_frequency(98000000 + step * 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(frequency_changes() == initial_changes);
    receiver.set_frequency(98200000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(frequency_changes() == initial_changes);
    receiver.set_frequency(98200010);
    wait_for_frequency(98200010);
    assert(frequency_changes() == initial_changes + 1);
    receiver.set_frequency(98000010);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(frequency_changes() == initial_changes + 1);
    receiver.set_frequency(98000000);
    wait_for_frequency(98000000);

    for (uint32_t step = 1; step <= 20; ++step) {
        receiver.set_frequency(98000000 + step * 100000);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(frequency() <= 100000000);
    assert(100000000 - frequency() <= 200000);
    assert(frequency_changes() < initial_changes + 15);

    const auto wait_for_peak = [&](int expected) {
        platform::sdr::SpectrumFrame frame{};
        bool found = false;
        int last_peak = -1;
        for (int i = 0; i < 200 && !found; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!platform::sdr::LiveSpectrumBuffer::snapshot(frame)) continue;
            const auto peak = std::max_element(frame.begin(), frame.end()) - frame.begin();
            last_peak = static_cast<int>(peak);
            found = frame[peak] > 0 && std::abs(static_cast<int>(peak) - expected) <= 3;
        }
        if (!found) std::fprintf(stderr, "Expected spectrum bin %d, got %d (span %u)\n",
                                 expected, last_peak, platform::sdr::LiveSpectrumBuffer::span_hz());
        assert(found);
    };
    const model::RadioMode modes[] = {model::RadioMode::Am, model::RadioMode::Nfm,
                                      model::RadioMode::Usb, model::RadioMode::Lsb,
                                      model::RadioMode::Cw};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        const uint32_t base = modes[i] == model::RadioMode::Nfm ? 145000000
                                                               : 121000000 + i * 1000000;
        receiver.set_receive_settings(modes[i], modes[i] == model::RadioMode::Nfm ? 12500 : 4500);
        receiver.set_frequency(base);
        wait_for_frequency(base + 200000);
        assert(direct_sampling() == 0);
        const int before = frequency_changes();
        set_signal(base + 20000);
        wait_for_peak(168);
        receiver.set_frequency(base + 25000);
        wait_for_peak(138);
        assert(frequency_changes() == before);
        receiver.set_frequency(base + 100000);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        assert(frequency_changes() == before);
        receiver.set_frequency(base + 100001);
        wait_for_frequency(base + 300001);
        assert(frequency_changes() == before + 1);
        set_signal(0);
    }

    receiver.set_receive_settings(model::RadioMode::Am, 4500);
    receiver.set_frequency(7000000);
    wait_for_frequency(7000000);
    assert(direct_sampling() == 2);
    const int direct_changes = frequency_changes();
    set_signal(7020000);
    wait_for_peak(168);
    receiver.set_frequency(7025000);
    wait_for_peak(138);
    assert(frequency_changes() == direct_changes);
    set_signal(0);
    receiver.set_frequency(7200000);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    assert(frequency_changes() == direct_changes);
    receiver.set_frequency(7200001);
    wait_for_frequency(7200001);
    assert(frequency_changes() == direct_changes + 1);
    receiver.set_frequency(24000000);
    wait_for_frequency(24200000);
    assert(direct_sampling() == 0);
    receiver.set_receive_settings(model::RadioMode::Wfm, 180000);
    wait_for_frequency(24000000);
    receiver.stop();
    assert(overlaps() == 0);
    dlclose(fake);
}
