/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "butter_screen.h"

#include "asset_manager.h"
#include "bindings.h"
namespace screen {

ButterScreen::ButterScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void ButterScreen::build_content(lv_obj_t* content) {
    auto* regular = assets().load_standard_font(14, app::StandardFontWeight::Regular);
    auto* bold = assets().load_standard_font(14, app::StandardFontWeight::Bold);
    auto* bold_large = assets().load_standard_font(28, app::StandardFontWeight::Bold);

    for (int row = 0; row < model::BaseModel::kSettingRows; ++row) {
        auto* title = lv_label_create(content);
        lv_label_bind_text(title, view_model().setting_title_subject(row), nullptr);
        lv_obj_set_style_text_font(title, bold ? bold : &lv_font_montserrat_14, 0);
        lv_obj_set_pos(title, 8, 4 + row * 19);
        reactive::bind_theme(title, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

        auto* value = lv_label_create(content);
        lv_label_bind_text(value, view_model().setting_value_subject(row), nullptr);
        lv_obj_set_style_text_font(value, regular ? regular : &lv_font_montserrat_14, 0);
        lv_obj_set_pos(value, 130, 4 + row * 19);
        reactive::bind_theme(value, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    }

    auto* keys = lv_label_create(content);
    lv_label_set_text(keys, "F/X select   Z/C change\n0-9 B(.) type  ENTER tune");
    lv_obj_set_style_text_font(keys, regular ? regular : &lv_font_montserrat_14, 0);
    lv_obj_set_pos(keys, 8, 102);
    reactive::bind_theme(keys, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
}

} // namespace screen
