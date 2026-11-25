#ifndef HUMBUG_ART_H
#define HUMBUG_ART_H
#include <stddef.h>
#include <stdint.h>
#include "humbug_generated.h"
typedef struct {
    uint16_t x, y, width, height;
    const uint8_t* rle;
    size_t size;
} hb_sprite;
/* Index 0 is transparent in sprite data. Renderer indices are opaque. */
extern const hb_sprite hb_sprites[HB_SPRITE_COUNT];
extern const uint8_t hb_glyphs[128][7];
extern const uint32_t hb_palettes[HB_THEME_COUNT][HB_ROLE_COUNT];
extern const char* const hb_theme_names[HB_THEME_COUNT];
extern const size_t hb_theme_source_indices[HB_THEME_COUNT];
extern const uint8_t hb_checkerboard_scrim[HB_THEME_COUNT];
#endif
