/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>

namespace model {

const RadioMeasurements& BaseModel::radio_measurements() const {
    return radio_measurements_;
}

void BaseModel::adjust_rit(int direction) {
    rit_hz_ = std::clamp(rit_hz_ + (direction < 0 ? -50 : 50), -2000, 2000);
}

void BaseModel::set_radio_measurements(const RadioMeasurements& measurements) {
    radio_measurements_ = measurements;
}

namespace {

struct BandPlanEntry {
    const char* name;
    uint64_t low_hz;
    uint64_t high_hz;
    uint64_t default_hz;
};

struct Band {
    const char* name;
    uint64_t low_hz;
    uint64_t high_hz;
    uint64_t default_hz;
    RadioMode mode;
};

// Sorted by frequency. Broadcast, aviation, marine and amateur allocations (UK-oriented).
constexpr std::array<Band, 33> kBands = {{
    {"LW", 153000, 279000, 198000, RadioMode::Am},
    {"MW", 531000, 1602000, 909000, RadioMode::Am},
    {"160m", 1810000, 2000000, 1900000, RadioMode::Lsb},
    {"80m", 3500000, 3800000, 3700000, RadioMode::Lsb},
    {"60m SW", 4750000, 5060000, 4900000, RadioMode::Am},
    {"60m", 5258500, 5406500, 5355000, RadioMode::Usb},
    {"49m SW", 5900000, 6200000, 6100000, RadioMode::Am},
    {"40m", 7000000, 7200000, 7100000, RadioMode::Lsb},
    {"41m SW", 7200000, 7450000, 7300000, RadioMode::Am},
    {"31m SW", 9400000, 9900000, 9600000, RadioMode::Am},
    {"30m", 10100000, 10150000, 10125000, RadioMode::Cw},
    {"25m SW", 11600000, 12100000, 11800000, RadioMode::Am},
    {"22m SW", 13570000, 13870000, 13700000, RadioMode::Am},
    {"20m", 14000000, 14350000, 14200000, RadioMode::Usb},
    {"19m SW", 15100000, 15800000, 15300000, RadioMode::Am},
    {"16m SW", 17480000, 17900000, 17600000, RadioMode::Am},
    {"17m", 18068000, 18168000, 18100000, RadioMode::Usb},
    {"15m", 21000000, 21450000, 21200000, RadioMode::Usb},
    {"13m SW", 21450000, 21850000, 21600000, RadioMode::Am},
    {"12m", 24890000, 24990000, 24940000, RadioMode::Usb},
    {"CB", 26965000, 27405000, 27185000, RadioMode::Nfm},
    {"10m", 28000000, 29700000, 28500000, RadioMode::Usb},
    {"6m", 50000000, 52000000, 50200000, RadioMode::Usb},
    {"4m", 70000000, 70500000, 70450000, RadioMode::Nfm},
    {"FM", 87500000, 108000000, 98000000, RadioMode::Wfm},
    {"Airband", 118000000, 137000000, 121500000, RadioMode::Am},
    {"2m", 144000000, 146000000, 145500000, RadioMode::Nfm},
    {"Marine", 156000000, 162025000, 156800000, RadioMode::Nfm},
    {"Mil Air", 225000000, 400000000, 243000000, RadioMode::Am},
    {"70cm", 430000000, 440000000, 433500000, RadioMode::Nfm},
    {"PMR446", 446000000, 446200000, 446100000, RadioMode::Nfm},
    {"ADS-B", 1085000000, 1095000000, 1090000000, RadioMode::Am},
    {"23cm", 1240000000, 1300000000, 1296200000, RadioMode::Nfm},
}};

constexpr std::array<uint32_t, 12> kTuningStepsHz = {
    10, 100, 500, 1000, 5000, 8333, 9000, 10000, 12500, 25000, 100000, 200000,
};

constexpr std::array<uint32_t, 6> kSsbBandwidthsHz = {
    1800, 2100, 2400, 2700, 2800, 3000,
};

constexpr std::array<uint32_t, 5> kAmBandwidthsHz = {
    3000, 4000, 4500, 6000, 8000,
};

constexpr std::array<uint32_t, 6> kCwBandwidthsHz = {
    100, 250, 500, 700, 900, 1200,
};

constexpr std::array<uint32_t, 5> kNfmBandwidthsHz = {
    8000, 10000, 12500, 15000, 20000,
};

uint32_t default_step_hz(RadioMode mode, uint64_t frequency_hz) {
    switch (mode) {
    case RadioMode::Wfm:
        return 100000;
    case RadioMode::Nfm:
        if (frequency_hz >= 156000000 && frequency_hz <= 163000000) return 25000;
        if (frequency_hz < 30000000) return 10000;
        return 12500;
    case RadioMode::Am:
        if (frequency_hz < 1700000) return 9000;
        if (frequency_hz < 30000000) return 5000;
        if (frequency_hz > 1000000000) return 100000;
        return 25000;
    case RadioMode::Cw:
        return 100;
    case RadioMode::Usb:
    case RadioMode::Lsb:
        return 1000;
    }
    return 1000;
}

size_t step_index_for(uint32_t step_hz) {
    for (size_t i = 0; i < kTuningStepsHz.size(); ++i) {
        if (kTuningStepsHz[i] == step_hz) return i;
    }
    return 3;
}

uint32_t default_bandwidth_hz(RadioMode mode) {
    switch (mode) {
    case RadioMode::Usb:
    case RadioMode::Lsb:
        return 2800;
    case RadioMode::Am:
        return 4500;
    case RadioMode::Cw:
        return 900;
    case RadioMode::Nfm:
        return 12500;
    case RadioMode::Wfm:
        return 180000;
    }
    return 2800;
}

const Band* find_band(uint64_t hz) {
    for (const auto& band : kBands) {
        if (hz >= band.low_hz && hz <= band.high_hz) return &band;
    }
    return nullptr;
}

template <size_t Size>
uint32_t cycle_value(uint32_t current, const std::array<uint32_t, Size>& values, int direction) {
    size_t index = 0;
    for (size_t candidate = 0; candidate < values.size(); ++candidate) {
        if (values[candidate] == current) {
            index = candidate;
            break;
        }
    }
    index = direction < 0 ? (index + values.size() - 1) % values.size()
                          : (index + 1) % values.size();
    return values[index];
}

} // namespace

const char* BaseModel::app_title() const {
    return "RTL SDR";
}

const char* BaseModel::greeting() const {
    return "Radio unavailable";
}

const char* BaseModel::radio_mode_name() const {
    switch (radio_mode_) {
    case RadioMode::Usb: return "USB";
    case RadioMode::Lsb: return "LSB";
    case RadioMode::Am: return "AM";
    case RadioMode::Cw: return "CW";
    case RadioMode::Nfm: return "FMN";
    case RadioMode::Wfm: return "FMW";
    }
    return "USB";
}

const char* BaseModel::radio_mode_description() const {
    switch (radio_mode_) {
    case RadioMode::Usb: return "Upper sideband";
    case RadioMode::Lsb: return "Lower sideband";
    case RadioMode::Am: return "Amplitude modulation";
    case RadioMode::Cw: return "Morse / 700 Hz tone";
    case RadioMode::Nfm: return "Narrow FM";
    case RadioMode::Wfm: return "Broadcast FM";
    }
    return "Upper sideband";
}

const char* BaseModel::band_name() const {
    const Band* band = find_band(frequency_hz_);
    return band ? band->name : "---";
}

uint64_t BaseModel::frequency_hz() const {
    return frequency_hz_;
}

uint32_t BaseModel::tuning_step_hz() const {
    return kTuningStepsHz[tuning_step_index_];
}

uint32_t BaseModel::filter_bandwidth_hz() const {
    return filter_bandwidth_hz_;
}

bool BaseModel::dark_mode() const {
    return dark_mode_;
}

void BaseModel::set_dark_mode(bool enabled) {
    dark_mode_ = enabled;
}

void BaseModel::toggle_dark_mode() {
    dark_mode_ = !dark_mode_;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}


void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
}

void BaseModel::toggle_page() {
    current_page_ = current_page_ == AppPage::Apple ? AppPage::Butter : AppPage::Apple;
}

RadioMode BaseModel::radio_mode() const {
    return radio_mode_;
}

void BaseModel::cycle_radio_mode(int direction) {
    if (direction == 0) {
        return;
    }
    constexpr int kModeCount = 6;
    const int current = static_cast<int>(radio_mode_);
    const int step = direction < 0 ? kModeCount - 1 : 1;
    radio_mode_ = static_cast<RadioMode>((current + step) % kModeCount);
    filter_bandwidth_hz_ = default_bandwidth_hz(radio_mode_);
    tuning_step_index_ = static_cast<uint8_t>(step_index_for(default_step_hz(radio_mode_, frequency_hz_)));
}

bool BaseModel::tune_frequency(int direction) {
    if (direction == 0) {
        return false;
    }
    const uint64_t previous_hz = frequency_hz_;
    if (direction < 0) {
        frequency_hz_ = frequency_hz_ > kMinFrequencyHz + tuning_step_hz()
                      ? frequency_hz_ - tuning_step_hz()
                      : kMinFrequencyHz;
    }
    else {
        frequency_hz_ = std::min<uint64_t>(frequency_hz_ + tuning_step_hz(), kMaxFrequencyHz);
    }
    for (const auto& band : kBands) {
        for (const auto edge : {band.low_hz, band.high_hz}) {
            if ((previous_hz < edge && frequency_hz_ >= edge)
                || (previous_hz > edge && frequency_hz_ <= edge)) {
                return true;
            }
        }
    }
    return false;
}

void BaseModel::cycle_band(int direction) {
    if (direction == 0) {
        return;
    }
    // Next band above (or below) the current frequency, wrapping around.
    const Band* chosen = nullptr;
    if (direction > 0) {
        for (const auto& band : kBands) {
            if (band.default_hz > frequency_hz_) { chosen = &band; break; }
        }
        if (!chosen) chosen = &kBands.front();
    } else {
        for (auto it = kBands.rbegin(); it != kBands.rend(); ++it) {
            if (it->default_hz < frequency_hz_) { chosen = &*it; break; }
        }
        if (!chosen) chosen = &kBands.back();
    }
    frequency_hz_ = chosen->default_hz;
    radio_mode_ = chosen->mode;
    filter_bandwidth_hz_ = default_bandwidth_hz(radio_mode_);
    tuning_step_index_ = static_cast<uint8_t>(step_index_for(default_step_hz(radio_mode_, frequency_hz_)));
}

void BaseModel::entry_append(char c) {
    if (c == '.') {
        if (entry_.find('.') != std::string::npos || entry_.empty()) {
            if (!entry_.empty()) return;
            entry_ = "0";
        }
    } else if (c < '0' || c > '9') {
        return;
    }
    if (entry_.size() < 10) entry_.push_back(c);
}

void BaseModel::entry_backspace() {
    if (!entry_.empty()) entry_.pop_back();
}

bool BaseModel::entry_commit() {
    if (entry_.empty()) return false;
    const double mhz = std::strtod(entry_.c_str(), nullptr);
    const auto hz = static_cast<uint64_t>(mhz * 1.0e6 + 0.5);
    if (hz < kMinFrequencyHz || hz > kMaxFrequencyHz) return false;
    frequency_hz_ = hz;
    if (const Band* band = find_band(hz)) {
        radio_mode_ = band->mode;
    }
    filter_bandwidth_hz_ = default_bandwidth_hz(radio_mode_);
    tuning_step_index_ = static_cast<uint8_t>(step_index_for(default_step_hz(radio_mode_, frequency_hz_)));
    entry_.clear();
    return true;
}

void BaseModel::cycle_tuning_step(int direction) {
    if (direction == 0) {
        return;
    }
    tuning_step_index_ = static_cast<uint8_t>(direction < 0
                         ? (tuning_step_index_ + kTuningStepsHz.size() - 1) % kTuningStepsHz.size()
                         : (tuning_step_index_ + 1) % kTuningStepsHz.size());
}

void BaseModel::cycle_filter_bandwidth(int direction) {
    if (direction == 0) {
        return;
    }
    switch (radio_mode_) {
    case RadioMode::Usb:
    case RadioMode::Lsb:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kSsbBandwidthsHz, direction);
        break;
    case RadioMode::Am:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kAmBandwidthsHz, direction);
        break;
    case RadioMode::Cw:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kCwBandwidthsHz, direction);
        break;
    case RadioMode::Nfm:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kNfmBandwidthsHz, direction);
        break;
    case RadioMode::Wfm:
        break;
    }
}

void BaseModel::select_setting(int direction) {
    if (direction == 0) {
        return;
    }
    selected_setting_ = (selected_setting_ + (direction < 0 ? kSettingRows - 1 : 1)) % kSettingRows;
}

void BaseModel::adjust_setting(int direction) {
    switch (selected_setting_) {
    case 0:
        tune_frequency(direction);
        break;
    case 1:
        cycle_radio_mode(direction);
        break;
    case 2:
        cycle_filter_bandwidth(direction);
        break;
    case 3:
        cycle_tuning_step(direction);
        break;
    default:
        cycle_squelch(direction);
        break;
    }
}

void BaseModel::cycle_squelch(int direction) {
    if (direction == 0) {
        return;
    }
    squelch_ = (squelch_ + (direction < 0 ? kMaxSquelch : 1)) % (kMaxSquelch + 1);
}

} // namespace model
