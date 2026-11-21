/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/rtl_receiver.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
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
    assert(overlaps && frequency && frequency_changes);

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
    receiver.stop();
    assert(overlaps() == 0);
    dlclose(fake);
}
