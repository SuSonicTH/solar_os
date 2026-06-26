#include "solar_os_fontdemo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"

#ifndef SOLAR_OS_HAS_GENERATED_FONTS
#define SOLAR_OS_HAS_GENERATED_FONTS 0
#endif

#define FONTDEMO_HEADER_H 16
#define FONTDEMO_LABEL_H 9
#define FONTDEMO_BLOCK_GAP 9
#define FONTDEMO_MARGIN_X 4

#if SOLAR_OS_HAS_GENERATED_FONTS
extern const uint8_t u8g2_font_solar_os_default_r_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_20_tf[];
#endif

typedef struct {
    const char *label;
    uint8_t size;
    uint8_t cell_width;
    uint8_t cell_height;
    uint8_t ascent;
    solar_os_gfx_font_t builtin_font;
    const uint8_t *font;
} fontdemo_face_t;

typedef struct {
    int scroll_y;
    int content_height;
} fontdemo_state_t;

static fontdemo_state_t fontdemo;

#if SOLAR_OS_HAS_GENERATED_FONTS
static const fontdemo_face_t fontdemo_faces[] = {
    {"12 regular", 12, 6, 13, 10, SOLAR_OS_GFX_FONT_MONO_12, u8g2_font_solar_os_default_r_12_tf},
    {"12 bold", 12, 6, 13, 10, SOLAR_OS_GFX_FONT_BOLD_12, u8g2_font_solar_os_default_b_12_tf},
    {"12 italic", 12, 6, 13, 10, SOLAR_OS_GFX_FONT_ITALIC_12, u8g2_font_solar_os_default_i_12_tf},
    {"12 bold italic", 12, 6, 13, 10, SOLAR_OS_GFX_FONT_BOLD_ITALIC_12, u8g2_font_solar_os_default_bi_12_tf},
    {"14 regular", 14, 7, 15, 12, SOLAR_OS_GFX_FONT_MONO_14, u8g2_font_solar_os_default_r_14_tf},
    {"14 bold", 14, 7, 15, 12, SOLAR_OS_GFX_FONT_BOLD_14, u8g2_font_solar_os_default_b_14_tf},
    {"14 italic", 14, 7, 15, 12, SOLAR_OS_GFX_FONT_ITALIC_14, u8g2_font_solar_os_default_i_14_tf},
    {"14 bold italic", 14, 7, 15, 12, SOLAR_OS_GFX_FONT_BOLD_ITALIC_14, u8g2_font_solar_os_default_bi_14_tf},
    {"16 regular", 16, 8, 17, 14, SOLAR_OS_GFX_FONT_MONO_16, u8g2_font_solar_os_default_r_16_tf},
    {"16 bold", 16, 8, 17, 14, SOLAR_OS_GFX_FONT_BOLD_16, u8g2_font_solar_os_default_b_16_tf},
    {"16 italic", 16, 8, 17, 14, SOLAR_OS_GFX_FONT_ITALIC_16, u8g2_font_solar_os_default_i_16_tf},
    {"16 bold italic", 16, 8, 17, 14, SOLAR_OS_GFX_FONT_BOLD_ITALIC_16, u8g2_font_solar_os_default_bi_16_tf},
    {"18 regular", 18, 9, 19, 15, SOLAR_OS_GFX_FONT_MONO_18, u8g2_font_solar_os_default_r_18_tf},
    {"18 bold", 18, 9, 19, 15, SOLAR_OS_GFX_FONT_BOLD_18, u8g2_font_solar_os_default_b_18_tf},
    {"18 italic", 18, 9, 19, 15, SOLAR_OS_GFX_FONT_ITALIC_18, u8g2_font_solar_os_default_i_18_tf},
    {"18 bold italic", 18, 9, 19, 15, SOLAR_OS_GFX_FONT_BOLD_ITALIC_18, u8g2_font_solar_os_default_bi_18_tf},
    {"20 regular", 20, 10, 21, 17, SOLAR_OS_GFX_FONT_MONO_20, u8g2_font_solar_os_default_r_20_tf},
    {"20 bold", 20, 10, 21, 17, SOLAR_OS_GFX_FONT_BOLD_20, u8g2_font_solar_os_default_b_20_tf},
    {"20 italic", 20, 10, 21, 17, SOLAR_OS_GFX_FONT_ITALIC_20, u8g2_font_solar_os_default_i_20_tf},
    {"20 bold italic", 20, 10, 21, 17, SOLAR_OS_GFX_FONT_BOLD_ITALIC_20, u8g2_font_solar_os_default_bi_20_tf},
};
#endif

static int fontdemo_max_scroll(solar_os_gfx_t *gfx)
{
    const int view_h = (int)solar_os_gfx_height(gfx) - FONTDEMO_HEADER_H;
    const int max_scroll = fontdemo.content_height - (view_h > 0 ? view_h : 0);
    return max_scroll > 0 ? max_scroll : 0;
}

static void fontdemo_clamp_scroll(solar_os_gfx_t *gfx)
{
    const int max_scroll = fontdemo_max_scroll(gfx);
    if (fontdemo.scroll_y < 0) {
        fontdemo.scroll_y = 0;
    } else if (fontdemo.scroll_y > max_scroll) {
        fontdemo.scroll_y = max_scroll;
    }
}

static void fontdemo_raw_text(solar_os_gfx_t *gfx,
                              const uint8_t *font,
                              int x,
                              int baseline_y,
                              const char *text)
{
    if (gfx == NULL || gfx->u8g2 == NULL || font == NULL || text == NULL) {
        return;
    }

    u8g2_SetDrawColor(gfx->u8g2, 0);
    u8g2_SetFontMode(gfx->u8g2, 1);
    u8g2_SetFontDirection(gfx->u8g2, 0);
    u8g2_SetFontPosBaseline(gfx->u8g2);
    u8g2_SetFont(gfx->u8g2, font);
    u8g2_DrawUTF8(gfx->u8g2, (u8g2_uint_t)x, (u8g2_uint_t)baseline_y, text);
    gfx->dirty = true;
}

static void fontdemo_draw_title(solar_os_gfx_t *gfx)
{
    char title[48];
    snprintf(title, sizeof(title), "fontdemo  scroll %d/%d", fontdemo.scroll_y, fontdemo_max_scroll(gfx));

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, (int)solar_os_gfx_width(gfx), FONTDEMO_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, 3, 12, title);
}

static int fontdemo_block_height(const fontdemo_face_t *face)
{
    return FONTDEMO_LABEL_H + ((int)face->cell_height * 2) + FONTDEMO_BLOCK_GAP + 4;
}

static void fontdemo_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    fontdemo_clamp_scroll(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

#if SOLAR_OS_HAS_GENERATED_FONTS
    int y = FONTDEMO_HEADER_H + 4 - fontdemo.scroll_y;
    const int screen_h = (int)solar_os_gfx_height(gfx);
    for (size_t i = 0; i < sizeof(fontdemo_faces) / sizeof(fontdemo_faces[0]); i++) {
        const fontdemo_face_t *face = &fontdemo_faces[i];
        const int block_h = fontdemo_block_height(face);
        if (y + block_h >= FONTDEMO_HEADER_H && y < screen_h) {
            char label[48];
            snprintf(label,
                     sizeof(label),
                     "%s  %ux%u",
                     face->label,
                     (unsigned)face->cell_width,
                     (unsigned)face->cell_height);

            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
            solar_os_gfx_text(gfx, FONTDEMO_MARGIN_X, y + 8, label);

            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_line(gfx,
                              FONTDEMO_MARGIN_X,
                              y + FONTDEMO_LABEL_H,
                              (int)solar_os_gfx_width(gfx) - FONTDEMO_MARGIN_X,
                              y + FONTDEMO_LABEL_H);

            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_set_font(gfx, face->builtin_font);
            solar_os_gfx_text(gfx,
                              FONTDEMO_MARGIN_X,
                              y + FONTDEMO_LABEL_H + face->ascent,
                              "u8g2:   SolarOS iIl1 MWmw []{}");

            fontdemo_raw_text(gfx,
                              face->font,
                              FONTDEMO_MARGIN_X,
                              y + FONTDEMO_LABEL_H + face->cell_height + face->ascent + 2,
                              "default: SolarOS iIl1 MWmw []{}");
        }
        y += block_h;
    }
#else
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    solar_os_gfx_text(gfx, 8, 44, "generated default fonts not built");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, 8, 64, "make -C fonts/tools/bdfconv");
    solar_os_gfx_text(gfx, 8, 80, "python3 fonts/generate_u8g2_fonts.py");
#endif

    fontdemo_draw_title(gfx);
    solar_os_gfx_present(gfx);
}

static esp_err_t fontdemo_start(solar_os_context_t *ctx)
{
#if !SOLAR_OS_HAS_GENERATED_FONTS
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_write(io, "fontdemo: generated fonts are not built\n");
        solar_os_shell_io_write(io, "fontdemo: run make -C fonts/tools/bdfconv\n");
        solar_os_shell_io_write(io, "fontdemo: run python3 fonts/generate_u8g2_fonts.py\n");
        solar_os_shell_io_flush(io);
        solar_os_context_request_terminal_preserve(ctx);
    }
    return ESP_ERR_NOT_FOUND;
#else
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    fontdemo.scroll_y = 0;
    fontdemo.content_height = 0;
    for (size_t i = 0; i < sizeof(fontdemo_faces) / sizeof(fontdemo_faces[0]); i++) {
        fontdemo.content_height += fontdemo_block_height(&fontdemo_faces[i]);
    }
    solar_os_context_set_graphics_active(ctx, true);
    fontdemo_render(ctx);
    fontdemo_clamp_scroll(gfx);
    return ESP_OK;
#endif
}

static void fontdemo_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool fontdemo_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        fontdemo_render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    const int page = (int)solar_os_gfx_height(gfx) > 40 ? (int)solar_os_gfx_height(gfx) - 40 : 40;
    switch (ch) {
    case SOLAR_OS_KEY_APP_EXIT:
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_UP:
        fontdemo.scroll_y -= 12;
        break;
    case SOLAR_OS_KEY_DOWN:
        fontdemo.scroll_y += 12;
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        fontdemo.scroll_y -= page;
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
    case ' ':
        fontdemo.scroll_y += page;
        break;
    case SOLAR_OS_KEY_HOME:
        fontdemo.scroll_y = 0;
        break;
    case SOLAR_OS_KEY_END:
        fontdemo.scroll_y = fontdemo_max_scroll(gfx);
        break;
    default:
        return false;
    }

    fontdemo_render(ctx);
    return true;
}

const solar_os_app_t solar_os_fontdemo_app = {
    .name = "fontdemo",
    .summary = "generated font preview",
    .start = fontdemo_start,
    .stop = fontdemo_stop,
    .event = fontdemo_event,
};
