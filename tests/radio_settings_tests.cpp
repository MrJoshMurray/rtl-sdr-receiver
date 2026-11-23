/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <cassert>
#include <string>

int main() {
    model::BaseModel model;
    assert(model.frequency_hz() == 98000000);
    assert(model.radio_mode() == model::RadioMode::Wfm);
    assert(std::string(model.band_name()) == "FM");
    assert(model.squelch_threshold_db() == 0);
    for (int threshold = 3; threshold <= 27; threshold += 3) {
        model.cycle_squelch(1);
        assert(model.squelch_threshold_db() == threshold);
    }
    model.cycle_squelch(1);
    assert(model.squelch_threshold_db() == 0);
    model.cycle_squelch(-1);
    assert(model.squelch_threshold_db() == 27);
    model.cycle_squelch(1);

    model.cycle_band(1);
    assert(model.frequency_hz() > 98000000);
    model.cycle_band(-1);
    assert(std::string(model.band_name()) == "FM");

    model.entry_clear();
    for (char c : std::string("118.5")) model.entry_append(c);
    assert(model.entry_text() == "118.5");
    assert(model.entry_commit());
    assert(model.frequency_hz() == 118500000);
    assert(model.radio_mode() == model::RadioMode::Am);
    assert(std::string(model.band_name()) == "Airband");

    model.entry_clear();
    for (char c : std::string("5000")) model.entry_append(c);
    assert(!model.entry_commit());
    assert(model.frequency_hz() == 118500000);

    model.entry_clear();
    for (char c : std::string("145.5")) model.entry_append(c);
    assert(model.entry_commit());
    assert(model.frequency_hz() == 145500000);
    assert(model.radio_mode() == model::RadioMode::Nfm);

    model.entry_backspace();
    return 0;
}
