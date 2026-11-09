// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// The rack's layout, generated from the design scene the photograph gui/assets/panel.png was rendered from;
// a hand edit has to keep the two in register.
// Every length is in panel millimetres (x from the panel's centre, z up from its bottom edge); the photograph is
// the panel framed exactly. Every name is prefixed L_ (the layout).
#pragma once
static const double L_PANEL_W = 482.6;
static const double L_PANEL_H = 65.87500000000001;
enum { L_PHOTO_W = 2400, L_PHOTO_H = 328 };
static const double L_SCREEN_Z0 = 20.44;
static const double L_SCREEN_Z1 = 45.44;
static const double L_NOTE_SCREEN[4]  = {-206.3, 21.44, -154.3, 44.44};  // x0, z0, x1, z1: the lit area
static const double L_METER_SCREEN[4] = {-144.3, 21.44, 122.3, 44.44};
static const double L_LEVEL_SCREEN[4] = {-144.3, 55.0, 122.3, 56.4};
static const double L_MCX = -11.000000000000007;
static const double L_MHALF = 129.3;
static const double L_CZ = 32.94;
enum { L_NCH = 9 };
static const double L_CGAP = 6.465000000000001;
static const double L_STEP = 13.648333333333333;
static const double L_CHEV_W = 10.918666666666667;
static const double L_CHEV_H = 5.499999999999999;
static const double L_STROKE = 1.25;
static const double L_BAR_W = 2.0;
static const double L_BAR_H = 14.849999999999998;
static const double L_RANGE_C = 50.0;
static const double L_SIDE_FADE = 0.35;
static const double L_ROW_Z = 10.22;
static const double L_ROW_SIZE = 12.0;
static const double L_ROW_D_MAX = 122.47583333333334;
static const double L_ROW_GAP_HALF = 36.0;
static const double L_ROW_PAD = 8.0;
static const double L_ROW_GAP_MAX_HALF = 60.0;
static const double L_MENU_Z = 10.22;
static const double L_MENU_SIZE = 9.5;
static const double L_TUNING_SIZE = 8.0;
static const double L_CENTS_X = -207.3;
static const double L_A4_X = -153.3;
static const double L_NOTE_CX = -180.3;
static const double L_GLYPH_Z = 24.189999999999998;
static const double L_GLYPH_H = 17.499999999999996;
static const double L_NAME_X0 = 131.3;
static const double L_NAME_X1 = 184.3;
static const double L_NAME_Z = 10.22;
static const double L_NAME_SIZE = 14.0;
static const double L_LOGO_CX = 200.70000000000002;
static const double L_LOGO_CZ = 32.93750000000001;
static const double L_LOGO_R = 11.4;  // the ring's outer edge: the bypass's hit area
enum { L_LEVEL_SEGMENTS = 40 };
static const double L_LEVEL_PAD = 1.2;
static const double L_LEVEL_FILL = 0.72;
static const double L_LEVEL_HEIGHT = 0.6;
static const double L_DISC_HEIGHT = 0.68;  // of the meter screen's lit height
typedef struct L_Button { const char* item; double x0, z0, x1, z1; } L_Button;
static const L_Button L_BUTTONS[4] = {
    {"MUTE", 131.3, 34.94, 155.8, 45.44},
    {"TONE", 159.8, 34.94, 184.3, 45.44},
    {"STROBE", 131.3, 20.439999999999998, 155.8, 30.939999999999998},
    {"SETTINGS", 159.8, 20.439999999999998, 184.3, 30.939999999999998},
};
typedef struct L_Sprite { const char* file; int x, y; } L_Sprite;   // pixel origin in the photograph
static const L_Sprite L_SPRITES[7] = {
    {"btn_mute.png", 1841, 89},
    {"btn_tone.png", 1982, 89},
    {"btn_strobe.png", 1841, 161},
    {"btn_settings.png", 1982, 161},
    {"logo_on.png", 2129, 95},
    {"logo_lit.png", 2129, 95},
    {"logo_bypass.png", 2129, 95},
};
static const double L_SET_X0 = -145.3;
static const double L_SET_X1 = 123.3;
static const double L_SET_Z0 = 1.0;
static const double L_SET_Z1 = 64.9;
static const double L_SET_R = 2.6;
static const double L_SET_Y = -6.0;
static const double L_SET_TINT = 0.35;
static const double L_SET_SMOKE = 0.82;
static const double L_SET_SHADOW = 0.22;
static const double L_SET_BORDER = 0.45;
static const double L_SET_LEVEL = 0.22;
static const double L_SET_TEXT_LEVEL = 0.3;
static const double L_SET_TITLE_SIZE = 9.5;
static const double L_SET_TITLE_Z = 56.5;
static const double L_SET_TEXT_SIZE = 8.0;
static const double L_SET_LABEL_X = -139.0;
static const double L_SET_VALUE_X = 117.0;
static const double L_SET_TRACK_X0 = -80.0;
static const double L_SET_TRACK_X1 = 72.0;
static const double L_SET_TRACK_W = 1.3;
static const double L_SET_KNOB_R = 2.5;
static const double L_SET_PING_Z = 11.5;
static const double L_SET_PING_X = -119.0;
static const double L_SET_PILL_W = 11.0;
static const double L_SET_PILL_H = 5.6;
static const double L_SET_CLOSE_X0 = 84.0;
static const double L_SET_CLOSE_X1 = 117.0;
static const double L_SET_CLOSE_H = 9.0;
static const double L_SET_ROW_Z[3] = {45.0, 34.5, 24.0};
static const double L_DET_X0 = -145.3;
static const double L_DET_X1 = 123.3;
static const double L_DET_Z0 = 1.0;
static const double L_DET_Z1 = 64.9;
static const double L_DET_TITLE_Z = 56.5;
static const double L_DET_NOTES_Z = 41.0;
static const double L_DET_MATCH_Z = 28.5;
static const double L_DET_BUTTON_Z = 11.5;
static const double L_DET_BUTTON_W = 40.0;
static const double L_DET_BUTTON_GAP = 10.0;
static const double L_DDT_X0 = -66.0;
static const double L_DDT_X1 = 44.0;
static const double L_DDT_Z0 = 1.0;
static const double L_DDT_Z1 = 64.9;
static const double L_DDT_PAD = 3.0;
static const double L_DDT_TEXT_SIZE = 9.5;
static const double L_DDT_INDENT = 12.0;
static const double L_DDT_INDENT_HEAD = 6.0;
static const double L_DDA_X0 = -207.3;
static const double L_DDA_X1 = -150.0;
static const double L_DDA_Z0 = 1.0;
static const double L_DDA_Z1 = 64.9;
static const double L_DDA_PAD = 3.0;
static const double L_DDA_TEXT_SIZE = 9.5;
static const double L_DDA_INDENT = 12.0;
static const double L_DDA_INDENT_HEAD = 6.0;
static const double L_SETTINGS_WIN[4] = {-145.3, 1.0, 123.3, 64.9};  // x0, z0, x1, z1
static const double L_DETECT_WIN[4] = {-145.3, 1.0, 123.3, 64.9};  // x0, z0, x1, z1
static const double L_DD_TUNING_WIN[4] = {-66.0, 1.0, 44.0, 64.9};  // x0, z0, x1, z1
static const double L_DD_A4_WIN[4] = {-207.3, 1.0, -150.0, 64.9};  // x0, z0, x1, z1
