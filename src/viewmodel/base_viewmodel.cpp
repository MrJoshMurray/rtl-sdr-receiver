/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

namespace viewmodel {
namespace {

int page_to_int(model::AppPage page) {
    return static_cast<int>(page);
}

void format_frequency(uint64_t frequency_hz, char* output, size_t output_size) {
    std::snprintf(output,
                  output_size,
                  "%llu.%03llu.%03llu",
                  static_cast<unsigned long long>(frequency_hz / 1000000),
                  static_cast<unsigned long long>((frequency_hz / 1000) % 1000),
                  static_cast<unsigned long long>(frequency_hz % 1000));
}

void format_hz(uint32_t hz, char* output, size_t output_size) {
    if (hz >= 1000 && hz % 100 == 0) {
        std::snprintf(output, output_size, "%u.%u kHz", hz / 1000, (hz % 1000) / 100);
    } else if (hz >= 1000) {
        std::snprintf(output, output_size, "%.2f kHz", static_cast<double>(hz) / 1000.0);
    } else {
        std::snprintf(output, output_size, "%u Hz", hz);
    }
}

void format_settings(const model::BaseModel& model, char* output, size_t output_size) {
    char step[16]{};
    char bandwidth[16]{};
    format_hz(model.tuning_step_hz(), step, sizeof(step));
    format_hz(model.filter_bandwidth_hz(), bandwidth, sizeof(bandwidth));
    std::snprintf(output, output_size, "Step %s / BW %s", step, bandwidth);
}

void format_band_mode(const model::BaseModel& model, char* output, size_t output_size) {
    std::snprintf(output,
                  output_size,
                  "%s %s",
                  model.band_name(),
                  model.radio_mode_name());
}

} // namespace

BaseViewModel::BaseViewModel()
    : title_subject_(model_.app_title()),
      greeting_subject_(model_.greeting()),
      radio_mode_subject_(model_.radio_mode_name()),
      radio_mode_description_subject_(model_.radio_mode_description()),
      frequency_subject_(""),
      band_mode_subject_(""),
      receiver_settings_subject_(""),
      signal_level_subject_("SNR --"),
      radio_alert_subject_(""),
      setting_title_subjects_{{reactive::StringSubject<24>(""), reactive::StringSubject<24>(""),
                               reactive::StringSubject<24>(""), reactive::StringSubject<24>(""),
                               reactive::StringSubject<24>("")}},
      setting_value_subjects_{{reactive::StringSubject<40>(""), reactive::StringSubject<40>(""),
                               reactive::StringSubject<40>(""), reactive::StringSubject<40>(""),
                               reactive::StringSubject<40>("")}},
      entry_subject_(""),
      entry_hint_subject_(""),
      radio_connection_state_subject_(static_cast<int>(model::RadioConnectionState::Disconnected)),
      dark_mode_subject_(model_.dark_mode()),
      current_page_subject_(page_to_int(model_.current_page())),
      counter_subject_(0),
      bold_text_subject_(false),
    info_visible_subject_(false),
    quit_requested_subject_(false) {
    entry_reset();
    publish_all();
}

BaseViewModel::~BaseViewModel() = default;

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::greeting_subject() {
    return greeting_subject_.native();
}

lv_subject_t* BaseViewModel::radio_mode_subject() {
    return radio_mode_subject_.native();
}

lv_subject_t* BaseViewModel::radio_mode_description_subject() {
    return radio_mode_description_subject_.native();
}

lv_subject_t* BaseViewModel::frequency_subject() {
    return frequency_subject_.native();
}

lv_subject_t* BaseViewModel::band_mode_subject() {
    return band_mode_subject_.native();
}

lv_subject_t* BaseViewModel::receiver_settings_subject() {
    return receiver_settings_subject_.native();
}

lv_subject_t* BaseViewModel::signal_level_subject() { return signal_level_subject_.native(); }
lv_subject_t* BaseViewModel::radio_alert_subject() { return radio_alert_subject_.native(); }
lv_subject_t* BaseViewModel::setting_title_subject(int row) {
    return setting_title_subjects_[static_cast<size_t>(std::clamp(row, 0, model::BaseModel::kSettingRows - 1))].native();
}

lv_subject_t* BaseViewModel::setting_value_subject(int row) {
    return setting_value_subjects_[static_cast<size_t>(std::clamp(row, 0, model::BaseModel::kSettingRows - 1))].native();
}

lv_subject_t* BaseViewModel::entry_subject() { return entry_subject_.native(); }
lv_subject_t* BaseViewModel::entry_hint_subject() { return entry_hint_subject_.native(); }

lv_subject_t* BaseViewModel::radio_connection_state_subject() {
    return radio_connection_state_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::counter_subject() {
    return counter_subject_.native();
}

lv_subject_t* BaseViewModel::bold_text_subject() {
    return bold_text_subject_.native();
}

lv_subject_t* BaseViewModel::info_visible_subject() {
    return info_visible_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

bool BaseViewModel::is_dark_mode() const {
    return model_.dark_mode();
}

uint64_t BaseViewModel::frequency_hz() const {
    return model_.frequency_hz();
}

model::RadioMode BaseViewModel::radio_mode() const {
    return model_.radio_mode();
}

uint32_t BaseViewModel::filter_bandwidth_hz() const {
    return model_.filter_bandwidth_hz();
}

void BaseViewModel::set_radio_connection_state(model::RadioConnectionState state) {
    lv_subject_set_int(radio_connection_state_subject_.native(), static_cast<int>(state));
}

void BaseViewModel::set_radio_measurements(const model::RadioMeasurements& measurements) {
    model_.set_radio_measurements(measurements);
    const auto& current = model_.radio_measurements();
    char text[64]{};
    if (current.signal_to_noise_db) {
        if (current.signal_below_noise) std::snprintf(text, sizeof(text), "SNR <0 dB");
        else std::snprintf(text, sizeof(text), "SNR ~%.0f dB", *current.signal_to_noise_db);
    } else {
        std::snprintf(text, sizeof(text), "SNR --");
    }
    if (std::strcmp(signal_level_subject_.value(), text) != 0) signal_level_subject_.set(text);
    publish_alert();
}

bool BaseViewModel::auto_notch() const { return model_.auto_notch(); }
int BaseViewModel::rit_hz() const { return model_.rit_hz(); }

void BaseViewModel::toggle_auto_notch() {
    model_.toggle_auto_notch();
    publish_alert();
}

void BaseViewModel::adjust_rit(int direction) {
    model_.adjust_rit(direction);
    publish_alert();
}

bool BaseViewModel::rit_active() const { return model_.rit_active(); }

void BaseViewModel::toggle_rit() {
    model_.toggle_rit();
    publish_alert();
}

void BaseViewModel::clear_rit() {
    model_.clear_rit();
    publish_alert();
}

// Status flags beside the band: ADC clipping, RIT offset and the auto notch.
void BaseViewModel::publish_alert() {
    const auto& current = model_.radio_measurements();
    std::string text;
    if (current.adc_overload && *current.adc_overload) text += "CLIP ";
    if (model_.rit_active()) {
        char rit[16];
        std::snprintf(rit, sizeof(rit), "R%+d ", model_.rit_hz());
        text += rit;
    }
    if (model_.auto_notch()) text += "NOTCH";
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (std::strcmp(radio_alert_subject_.value(), text.c_str()) != 0) radio_alert_subject_.set(text.c_str());
}

void BaseViewModel::set_dark_mode(bool enabled) {
    model_.set_dark_mode(enabled);
    publish_all();
}

void BaseViewModel::toggle_dark_mode() {
    model_.toggle_dark_mode();
    publish_all();
}

model::AppPage BaseViewModel::current_page() const {
    return model_.current_page();
}

void BaseViewModel::show_apple_page() {
    model_.set_current_page(model::AppPage::Apple);
    publish_all();
}

void BaseViewModel::show_butter_page() {
    entry_reset();
    model_.set_current_page(model::AppPage::Butter);
    publish_all();
}

void BaseViewModel::toggle_page() {
    entry_reset();
    model_.toggle_page();
    publish_all();
}

void BaseViewModel::select_next_radio_mode() {
    model_.cycle_radio_mode(1);
    publish_all();
}

void BaseViewModel::select_previous_radio_mode() {
    model_.cycle_radio_mode(-1);
    publish_all();
}

bool BaseViewModel::tune_frequency(int direction) {
    const bool crossed_edge = model_.tune_frequency(direction);
    publish_all();
    return crossed_edge;
}

void BaseViewModel::cycle_band(int direction) {
    model_.cycle_band(direction);
    publish_all();
}

void BaseViewModel::cycle_tuning_step(int direction) {
    model_.cycle_tuning_step(direction);
    publish_all();
}

void BaseViewModel::cycle_filter_bandwidth(int direction) {
    model_.cycle_filter_bandwidth(direction);
    publish_all();
}

void BaseViewModel::select_setting(int direction) {
    model_.select_setting(direction);
    publish_all();
}

void BaseViewModel::adjust_setting(int direction) {
    model_.adjust_setting(direction);
    publish_all();
}

int BaseViewModel::cycle_squelch(int direction) {
    model_.cycle_squelch(direction);
    publish_all();
    return model_.squelch_threshold_db();
}

void BaseViewModel::publish_settings() {
    static constexpr const char* kTitles[model::BaseModel::kSettingRows] = {"Frequency", "Mode", "Bandwidth", "Step", "Squelch"};
    char values[model::BaseModel::kSettingRows][40]{};
    if (model_.entry_text().empty()) {
        std::snprintf(values[0], sizeof(values[0]), "%.3f MHz",
                      static_cast<double>(model_.frequency_hz()) / 1.0e6);
    } else {
        std::snprintf(values[0], sizeof(values[0]), "%s_ MHz", model_.entry_text().c_str());
    }
    std::snprintf(values[1], sizeof(values[1]), "%s", model_.radio_mode_name());
    if (model_.radio_mode() == model::RadioMode::Wfm) {
        std::snprintf(values[2], sizeof(values[2]), "Wide");
    } else {
        format_hz(model_.filter_bandwidth_hz(), values[2], sizeof(values[2]));
    }
    format_hz(model_.tuning_step_hz(), values[3], sizeof(values[3]));
    if (model_.squelch_threshold_db() == 0) {
        std::snprintf(values[4], sizeof(values[4]), "Off");
    } else {
        std::snprintf(values[4], sizeof(values[4]), "%d dB", model_.squelch_threshold_db());
    }
    for (int i = 0; i < model::BaseModel::kSettingRows; ++i) {
        char title[24]{};
        std::snprintf(title, sizeof(title), "%s %s", i == model_.selected_setting() ? ">" : " ", kTitles[i]);
        auto& title_subject = setting_title_subjects_[static_cast<size_t>(i)];
        auto& value_subject = setting_value_subjects_[static_cast<size_t>(i)];
        if (std::strcmp(title_subject.value(), title) != 0) title_subject.set(title);
        if (std::strcmp(value_subject.value(), values[i]) != 0) value_subject.set(values[i]);
    }
}

void BaseViewModel::increment_counter() {
    counter_subject_.set(counter_subject_.value() + 1);
}

void BaseViewModel::decrement_counter() {
    const auto next = counter_subject_.value() - 1;
    counter_subject_.set(next < 0 ? 0 : next);
}

void BaseViewModel::toggle_bold_text() {
    bold_text_subject_.toggle();
}

void BaseViewModel::toggle_info() {
    info_visible_subject_.toggle();
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

void BaseViewModel::publish_entry(const char* hint) {
    char text[24]{};
    std::snprintf(text, sizeof(text), "%s_", model_.entry_text().c_str());
    if (std::strcmp(entry_subject_.value(), text) != 0) entry_subject_.set(text);
    if (std::strcmp(entry_hint_subject_.value(), hint) != 0) entry_hint_subject_.set(hint);
    publish_settings();
}

void BaseViewModel::entry_append(char c) {
    model_.entry_append(c);
    publish_entry("Enter = tune");
}

void BaseViewModel::entry_backspace() {
    model_.entry_backspace();
    publish_entry("Enter = tune");
}

void BaseViewModel::entry_reset() {
    model_.entry_clear();
    publish_entry("MHz, e.g. 98.5 or 0.909");
}

bool BaseViewModel::entry_commit() {
    if (!model_.entry_commit()) {
        publish_entry("Out of range (0.1 - 1766 MHz)");
        return false;
    }
    model_.set_current_page(model::AppPage::Apple);
    publish_entry("MHz, e.g. 98.5 or 0.909");
    publish_all();
    return true;
}

void BaseViewModel::publish_all() {
    title_subject_.set(model_.app_title());
    greeting_subject_.set(model_.greeting());
    radio_mode_subject_.set(model_.radio_mode_name());
    radio_mode_description_subject_.set(model_.radio_mode_description());
    char text[32]{};
    format_frequency(model_.frequency_hz(), text, sizeof(text));
    frequency_subject_.set(text);
    format_band_mode(model_, text, sizeof(text));
    band_mode_subject_.set(text);
    format_settings(model_, text, sizeof(text));
    receiver_settings_subject_.set(text);
    publish_settings();
    set_radio_measurements(model_.radio_measurements());
    dark_mode_subject_.set(model_.dark_mode());
    current_page_subject_.set(page_to_int(model_.current_page()));
}

} // namespace viewmodel
