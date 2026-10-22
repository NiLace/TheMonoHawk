// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
//
// hawk_ui_fonts.h -- the panel's printed type (Oswald, OFL) loaded from the bundle through FreeType.
//
// The .ttf travels inside the LV2 bundle and is loaded here directly, bypassing fontconfig, so the panel reads the
// same on every machine (cairo_ft_font_face_create_for_pattern would apply the user's own hinting rules).
//
// Each cairo_font_face_t owns a PRIVATE FT_Library holding its one FT_Face, hung on the face as user data whose
// destructor is FreeType's own FT_Done_FreeType. Cairo keeps faces in a process-wide cache and may drop this one long
// after the editor closed and the host unloaded this module: the destructor it then calls must not be code of this
// module, or the host jumps into unmapped memory (the crash measured on the Omega Cake).
// FT_Done_FreeType (FT_Error(*)(FT_Library)) called as cairo's void(*)(void*) is a conversion the C++ standard leaves
// undefined and the x86-64 System V ABI makes exact (the pointer travels in the same register, the result is
// discarded); a wrapper of the right type would be, again, code in this module.
#pragma once

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <string>

namespace hawk_fonts {

inline cairo_user_data_key_t* library_key() {
    static cairo_user_data_key_t key;
    return &key;
}

// HAWK_UI_FONT_CALLBACK_IN_MODULE=1 restores the crashing way, for the unload gate's NEGATIVE arm only
// (tools/ui_unload_probe.cpp): the library released by a function of this module.
#if HAWK_UI_FONT_CALLBACK_IN_MODULE
extern "C" inline void hawk_release_library(void* p) { if (p) FT_Done_FreeType(static_cast<FT_Library>(p)); }
#endif

// One face from one file, owning its own FreeType library; null if the file is missing or unreadable.
inline cairo_font_face_t* load_face(const std::string& path) {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Face(lib, path.c_str(), 0, &face) != 0) { FT_Done_FreeType(lib); return nullptr; }
    cairo_font_face_t* cf = cairo_ft_font_face_create_for_ft_face(face, 0);
    if (cairo_font_face_status(cf) != CAIRO_STATUS_SUCCESS) {
        cairo_font_face_destroy(cf);
        FT_Done_FreeType(lib);
        return nullptr;
    }
#if HAWK_UI_FONT_CALLBACK_IN_MODULE
    const cairo_destroy_func_t release = &hawk_release_library;
#else
    const cairo_destroy_func_t release =
        reinterpret_cast<cairo_destroy_func_t>(reinterpret_cast<void (*)()>(&FT_Done_FreeType));
#endif
    if (cairo_font_face_set_user_data(cf, library_key(), lib, release) != CAIRO_STATUS_SUCCESS) {
        cairo_font_face_destroy(cf);
        FT_Done_FreeType(lib);
        return nullptr;
    }
    return cf;
}

}  // namespace hawk_fonts
