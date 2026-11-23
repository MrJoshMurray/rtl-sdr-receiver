/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

#include <cassert>
#include <string>

int main() {
    lv_init();
    auto* display = lv_display_create(320, 170);
    assert(display);
    {
        viewmodel::BaseViewModel view_model;
        auto* label = lv_label_create(lv_screen_active());
        lv_obj_set_size(label, 120, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_label_bind_text(label, view_model.signal_level_subject(), nullptr);
        const auto check = [&](const char* expected) {
            assert(std::string(lv_subject_get_string(view_model.signal_level_subject())) == expected);
            lv_obj_update_layout(label);
            assert(lv_obj_get_height(label) <= lv_font_get_line_height(&lv_font_montserrat_12));
        };
        check("SNR --");
        model::RadioMeasurements measurements;
        measurements.signal_to_noise_db = 12.0f;
        view_model.set_radio_measurements(measurements);
        check("SNR ~12 dB");
        measurements.signal_to_noise_db = -10.0f;
        view_model.set_radio_measurements(measurements);
        check("SNR ~-10 dB");
        measurements.signal_to_noise_db = -100.0f;
        view_model.set_radio_measurements(measurements);
        check("SNR ~-100 dB");
        measurements.signal_to_noise_db = 100.0f;
        view_model.set_radio_measurements(measurements);
        check("SNR ~100 dB");
        measurements.signal_below_noise = true;
        view_model.set_radio_measurements(measurements);
        check("SNR <0 dB");
        view_model.set_dark_mode(false);
        check("SNR <0 dB");
        assert(view_model.cycle_squelch(1) == 3);
        assert(std::string(lv_subject_get_string(view_model.setting_value_subject(4))) == "3 dB");
        lv_obj_delete(label);
    }
    lv_display_delete(display);
    lv_deinit();
}
