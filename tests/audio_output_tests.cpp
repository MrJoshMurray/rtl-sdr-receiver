/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "audio_output.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>

int main() {
    using platform::audio::detail::open_output_candidates;
    std::vector<std::string> attempts;
    std::string candidate_error;
    auto default_success = [&](const char* name, std::string&) {
        attempts.emplace_back(name);
        return true;
    };
    assert(open_output_candidates("", default_success, candidate_error));
    assert((attempts == std::vector<std::string>{"default"}));
    assert(candidate_error.empty());

    attempts.clear();
    auto pulse_success = [&](const char* name, std::string& error) {
        attempts.emplace_back(name);
        if (std::string(name) == "pulse") return true;
        error = "default setup -524";
        return false;
    };
    assert(open_output_candidates("", pulse_success, candidate_error));
    assert((attempts == std::vector<std::string>{"default", "pulse"}));
    assert(candidate_error.empty());

    attempts.clear();
    auto both_fail = [&](const char* name, std::string& error) {
        attempts.emplace_back(name);
        error = std::string(name) + " error";
        return false;
    };
    assert(!open_output_candidates("", both_fail, candidate_error));
    assert((attempts == std::vector<std::string>{"default", "pulse"}));
    assert(candidate_error == "default error; pulse error");

    attempts.clear();
    assert(!open_output_candidates("custom", both_fail, candidate_error));
    assert((attempts == std::vector<std::string>{"custom"}));
    assert(candidate_error == "custom error");
    attempts.clear();
    assert(!open_output_candidates("default", both_fail, candidate_error));
    assert((attempts == std::vector<std::string>{"default"}));

    platform::audio::BandEdgeTone tone;
    assert(tone.next_sample(123, 48000) == 123);
    tone.request();
    bool audible = false;
    for (int frame = 0; frame < 3840; ++frame) {
        const auto sample = tone.next_sample(123, 48000);
        assert(sample >= -655 && sample <= 655);
        audible = audible || sample != 0;
    }
    assert(audible);
    assert(tone.next_sample(123, 48000) == 123);

    std::promise<void> completed;
    auto finished = completed.get_future();
    std::promise<void> release_handler;
    auto release = release_handler.get_future();
    std::vector<platform::audio::VolumeAction> actions;
    platform::audio::SystemVolumeControl volume([&](platform::audio::VolumeAction action,
                                                    platform::audio::VolumeState& state,
                                                    std::string& error) {
        actions.push_back(action);
        if (action == platform::audio::VolumeAction::ToggleMute) {
            error = "test mixer failure";
            return false;
        }
        if (actions.size() == 5) {
            completed.set_value();
            release.wait();
        }
        state = {42, true};
        return true;
    });
    assert(volume.request(platform::audio::VolumeAction::Down));
    assert(volume.request(platform::audio::VolumeAction::Up));
    assert(volume.request(platform::audio::VolumeAction::ToggleMute));
    assert(volume.request(platform::audio::VolumeAction::Down));
    assert(volume.request(platform::audio::VolumeAction::Up));
    assert(finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert((actions == std::vector<platform::audio::VolumeAction>{
        platform::audio::VolumeAction::Down,
        platform::audio::VolumeAction::Up,
        platform::audio::VolumeAction::ToggleMute,
        platform::audio::VolumeAction::Down,
        platform::audio::VolumeAction::Up}));
    platform::audio::VolumeState state;
    assert(volume.take_state(state));
    assert(state.percent == 42 && state.muted);
    assert(!volume.take_state(state));
    std::string volume_error;
    assert(volume.take_error(volume_error));
    assert(volume_error == "test mixer failure");
    assert(!volume.take_error(volume_error));
    release_handler.set_value();

#if USE_DESKTOP
    setenv("SDL_AUDIODRIVER", "dummy", 0);

    platform::audio::AudioOutput output;
    std::string error;
    assert(output.open("", 48000, error));

    const int16_t samples[] = {0, 1200, -1200, 2400, -2400};
    assert(output.write(samples, sizeof(samples) / sizeof(samples[0]), error));
    assert(output.is_open());
    output.close();
    assert(!output.is_open());
#endif
    return 0;
}