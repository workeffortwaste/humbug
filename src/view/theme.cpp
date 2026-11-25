/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "theme.h"

namespace view {

ThemePalette palette(bool dark_mode) {
    const auto c = lv_color_hex;
    if (dark_mode) {
        return {
            c(0x140e0b), c(0x201612), c(0x3c2920), c(0x33231d), c(0xa78a70), c(0xfff0d7),
            c(0xb9a18b), c(0xffdbac), c(0xe4c3a3), c(0xcde1be), c(0xffd095), c(0xff9d82),
        };
    }

    return {
        c(0x15110e), c(0x251c15), c(0x433126), c(0x38291f), c(0xb79a7a), c(0xfff5e1),
        c(0xc3ac97), c(0xffebc9), c(0xe1c7aa), c(0xc2e6c8), c(0xfbd39c), c(0xffa58d),
    };
}

void apply_command_style(lv_obj_t* obj, const ThemePalette& colors, bool block) {
    const auto ink = block ? colors.error : colors.primary;
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, block ? ink : colors.border, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, colors.button, 0);
    lv_obj_set_style_text_color(obj, block ? ink : colors.text, 0);
    lv_obj_set_style_bg_color(obj, ink, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(obj, colors.background, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, ink, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(obj, colors.background, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, colors.border, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(obj, colors.text_disabled, LV_STATE_DISABLED);
}

void apply_lvgl_theme(lv_display_t* display, bool dark_mode, const lv_font_t* font) {
    if (!display) {
        return;
    }

    const auto colors = palette(dark_mode);
    lv_theme_t* theme = lv_theme_default_init(display, colors.primary, colors.info, dark_mode, font);
    lv_display_set_theme(display, theme);
}

} // namespace view
