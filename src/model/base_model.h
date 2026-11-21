/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace model {

// S-meter reference: the level reported for S9.
constexpr int kS9ReferenceDbfs = -40;

enum class AppPage {
    Apple  = 0,
    Butter = 1,
};

enum class RadioMode {
    Usb = 0,
    Lsb,
    Am,
    Cw,
    Nfm,
    Wfm,
};

enum class RadioConnectionState {
    Disconnected = 0,
    Receiving,
};

enum class RadioDiscoveryState {
    Searching,
    NotFound,
    Found,
    Error,
};

struct RadioMeasurements {
    std::optional<int> signal_level_dbfs;
    std::optional<bool> adc_overload;
};

class BaseModel {
public:
    const char* app_title() const;
    const char* greeting() const;
    const char* radio_mode_name() const;
    const char* radio_mode_description() const;
    const char* band_name() const;
    uint64_t frequency_hz() const;
    uint32_t tuning_step_hz() const;
    uint32_t filter_bandwidth_hz() const;
    const RadioMeasurements& radio_measurements() const;
    void set_radio_measurements(const RadioMeasurements& measurements);

    static constexpr uint64_t kMinFrequencyHz = 100000;
    static constexpr uint64_t kMaxFrequencyHz = 1766000000;

    // "Go to frequency" entry, typed in MHz.
    const std::string& entry_text() const { return entry_; }
    void entry_append(char c);
    void entry_backspace();
    void entry_clear() { entry_.clear(); }
    // Tunes to the typed frequency; false if empty or outside the dongle's range.
    bool entry_commit();

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);
    void toggle_page();
    RadioMode radio_mode() const;
    void cycle_radio_mode(int direction);
    bool tune_frequency(int direction);
    void cycle_band(int direction);
    void cycle_tuning_step(int direction);
    bool auto_notch() const { return auto_notch_; }
    void toggle_auto_notch() { auto_notch_ = !auto_notch_; }
    // Receive offset in Hz (RIT); transmit is unaffected.
    int rit_hz() const { return rit_hz_; }
    void adjust_rit(int direction);
    void clear_rit() { rit_hz_ = 0; }
    bool rit_active() const { return rit_active_; }
    void toggle_rit() {
        rit_active_ = !rit_active_;
        if (!rit_active_) rit_hz_ = 0;
    }
    void cycle_filter_bandwidth(int direction);

    static constexpr int kSettingRows = 5;
    // Squelch opens at this many S units (3 dB each) above the noise floor; 0 = off.
    static constexpr int kMaxSquelch = 9;
    int squelch() const { return squelch_; }
    void cycle_squelch(int direction);
    int selected_setting() const { return selected_setting_; }
    void select_setting(int direction);
    // Frequency steps by the tuning step; Mode and Bandwidth cycle.
    void adjust_setting(int direction);

private:
    int selected_setting_{0};
    int squelch_{0};
    bool dark_mode_ = true;
    AppPage current_page_ = AppPage::Apple;
    RadioMode radio_mode_ = RadioMode::Wfm;
    uint8_t tuning_step_index_{10};
    uint64_t frequency_hz_{98000000};
    uint32_t filter_bandwidth_hz_{180000};
    RadioMeasurements radio_measurements_{};
    bool auto_notch_{false};
    int rit_hz_{0};
    bool rit_active_{false};
    std::string entry_;
};

} // namespace model
