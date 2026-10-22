// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// The rack's layout, generated from the design scene the photograph gui/assets/panel.png was rendered from;
// a hand edit has to keep the two in register.
// Every length is in panel millimetres (x from the panel's centre, z up from its bottom edge); the photograph is
// the panel framed exactly.
#pragma once
namespace hawk_layout {
constexpr double PANEL_W = 482.6;
constexpr double PANEL_H = 65.87500000000001;
constexpr int PHOTO_W = 2400, PHOTO_H = 328;
constexpr double SCREEN_Z0 = 20.44;
constexpr double SCREEN_Z1 = 45.44;
constexpr double NOTE_SCREEN[4]  = {-206.3, 21.44, -154.3, 44.44};  // x0, z0, x1, z1: the lit area
constexpr double METER_SCREEN[4] = {-144.3, 21.44, 122.3, 44.44};
constexpr double LEVEL_SCREEN[4] = {-144.3, 55.0, 122.3, 56.4};
constexpr double MCX = -11.000000000000007;
constexpr double MHALF = 129.3;
constexpr double CZ = 32.94;
constexpr int NCH = 9;
constexpr double CGAP = 6.465000000000001;
constexpr double STEP = 13.648333333333333;
constexpr double CHEV_W = 10.918666666666667;
constexpr double CHEV_H = 5.499999999999999;
constexpr double STROKE = 1.25;
constexpr double BAR_W = 2.0;
constexpr double BAR_H = 14.849999999999998;
constexpr double RANGE_C = 50.0;
constexpr double SIDE_FADE = 0.35;
constexpr double ROW_Z = 10.22;
constexpr double ROW_SIZE = 12.0;
constexpr double ROW_D_MAX = 122.47583333333334;
constexpr double ROW_GAP_HALF = 36.0;
constexpr double ROW_PAD = 8.0;
constexpr double ROW_GAP_MAX_HALF = 60.0;
constexpr double MENU_Z = 10.22;
constexpr double MENU_SIZE = 9.5;
constexpr double TUNING_SIZE = 8.0;
constexpr double CENTS_X = -207.3;
constexpr double A4_X = -153.3;
constexpr double NOTE_CX = -180.3;
constexpr double GLYPH_Z = 24.189999999999998;
constexpr double GLYPH_H = 17.499999999999996;
constexpr double NAME_X0 = 131.3;
constexpr double NAME_X1 = 184.3;
constexpr double NAME_Z = 10.22;
constexpr double NAME_SIZE = 14.0;
constexpr double LOGO_CX = 200.70000000000002;
constexpr double LOGO_CZ = 32.93750000000001;
constexpr double LOGO_R = 11.4;  // the ring's outer edge: the bypass's hit area
constexpr int LEVEL_SEGMENTS = 40;
constexpr double LEVEL_PAD = 1.2;
constexpr double LEVEL_FILL = 0.72;
constexpr double LEVEL_HEIGHT = 0.6;
constexpr double DISC_HEIGHT = 0.68;  // of the meter screen's lit height
struct Button { const char* item; double x0, z0, x1, z1; };
constexpr Button BUTTONS[4] = {
    {"MUTE", 131.3, 34.94, 155.8, 45.44},
    {"TONE", 159.8, 34.94, 184.3, 45.44},
    {"STROBE", 131.3, 20.439999999999998, 155.8, 30.939999999999998},
    {"SETTINGS", 159.8, 20.439999999999998, 184.3, 30.939999999999998},
};
struct Sprite { const char* file; int x, y; };   // pixel origin in the photograph
constexpr Sprite SPRITES[7] = {
    {"btn_mute.png", 1841, 89},
    {"btn_tone.png", 1982, 89},
    {"btn_strobe.png", 1841, 161},
    {"btn_settings.png", 1982, 161},
    {"logo_on.png", 2129, 95},
    {"logo_lit.png", 2129, 95},
    {"logo_bypass.png", 2129, 95},
};
constexpr double SET_X0 = -145.3;
constexpr double SET_X1 = 123.3;
constexpr double SET_Z0 = 1.0;
constexpr double SET_Z1 = 64.9;
constexpr double SET_R = 2.6;
constexpr double SET_Y = -6.0;
constexpr double SET_TINT = 0.35;
constexpr double SET_SMOKE = 0.82;
constexpr double SET_SHADOW = 0.22;
constexpr double SET_BORDER = 0.45;
constexpr double SET_LEVEL = 0.22;
constexpr double SET_TEXT_LEVEL = 0.3;
constexpr double SET_TITLE_SIZE = 9.5;
constexpr double SET_TITLE_Z = 56.5;
constexpr double SET_TEXT_SIZE = 8.0;
constexpr double SET_LABEL_X = -139.0;
constexpr double SET_VALUE_X = 117.0;
constexpr double SET_TRACK_X0 = -80.0;
constexpr double SET_TRACK_X1 = 72.0;
constexpr double SET_TRACK_W = 1.3;
constexpr double SET_KNOB_R = 2.5;
constexpr double SET_PING_Z = 11.5;
constexpr double SET_PING_X = -119.0;
constexpr double SET_PILL_W = 11.0;
constexpr double SET_PILL_H = 5.6;
constexpr double SET_CLOSE_X0 = 84.0;
constexpr double SET_CLOSE_X1 = 117.0;
constexpr double SET_CLOSE_H = 9.0;
constexpr double SET_ROW_Z[3] = {45.0, 34.5, 24.0};
constexpr double DET_X0 = -145.3;
constexpr double DET_X1 = 123.3;
constexpr double DET_Z0 = 1.0;
constexpr double DET_Z1 = 64.9;
constexpr double DET_TITLE_Z = 56.5;
constexpr double DET_NOTES_Z = 41.0;
constexpr double DET_MATCH_Z = 28.5;
constexpr double DET_BUTTON_Z = 11.5;
constexpr double DET_BUTTON_W = 40.0;
constexpr double DET_BUTTON_GAP = 10.0;
constexpr double DDT_X0 = -66.0;
constexpr double DDT_X1 = 44.0;
constexpr double DDT_Z0 = 1.0;
constexpr double DDT_Z1 = 64.9;
constexpr double DDT_PAD = 3.0;
constexpr double DDT_TEXT_SIZE = 9.5;
constexpr double DDT_INDENT = 12.0;
constexpr double DDT_INDENT_HEAD = 6.0;
constexpr double DDA_X0 = -207.3;
constexpr double DDA_X1 = -150.0;
constexpr double DDA_Z0 = 1.0;
constexpr double DDA_Z1 = 64.9;
constexpr double DDA_PAD = 3.0;
constexpr double DDA_TEXT_SIZE = 9.5;
constexpr double DDA_INDENT = 12.0;
constexpr double DDA_INDENT_HEAD = 6.0;
constexpr double SETTINGS_WIN[4] = {-145.3, 1.0, 123.3, 64.9};  // x0, z0, x1, z1
constexpr double DETECT_WIN[4] = {-145.3, 1.0, 123.3, 64.9};  // x0, z0, x1, z1
constexpr double DD_TUNING_WIN[4] = {-66.0, 1.0, 44.0, 64.9};  // x0, z0, x1, z1
constexpr double DD_A4_WIN[4] = {-207.3, 1.0, -150.0, 64.9};  // x0, z0, x1, z1
}  // namespace hawk_layout
