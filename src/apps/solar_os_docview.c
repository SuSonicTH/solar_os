#include "solar_os_docview.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "solar_os_doc.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_storage.h"

#define DOCVIEW_HEADER_H 16
#define DOCVIEW_MARGIN_X 8
#define DOCVIEW_MARGIN_Y 5
#define DOCVIEW_MESSAGE_MAX 96
#define DOCVIEW_MAX_ZOOM 4
#define DOCVIEW_STATE_DIR ".docview"
#define DOCVIEW_POSITIONS_FILE "positions"
#define DOCVIEW_POSITIONS_TMP_FILE "positions.tmp"
#define DOCVIEW_POSITION_LINE_MAX (SOLAR_OS_STORAGE_PATH_MAX + 64)

typedef struct {
    solar_os_doc_t doc;
    solar_os_doc_layout_t layout;
    bool loaded;
    bool error_only;
    bool layout_valid;
    int scroll_y;
    int zoom;
    int content_height;
    int layout_width;
    int layout_zoom;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char message[DOCVIEW_MESSAGE_MAX];
} docview_state_t;

static docview_state_t docview;

static esp_err_t docview_state_path(char *path, size_t path_len, const char *leaf)
{
    if (path == NULL || path_len == 0 || !solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *mount = solar_os_storage_mount_point();
    const int written = leaf == NULL ?
        snprintf(path, path_len, "%s/%s", mount, DOCVIEW_STATE_DIR) :
        snprintf(path, path_len, "%s/%s/%s", mount, DOCVIEW_STATE_DIR, leaf);
    return written >= 0 && (size_t)written < path_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t docview_ensure_state_dir(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = docview_state_path(dir, sizeof(dir), NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (mkdir(dir, 0777) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static bool docview_file_exists(const char *path)
{
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool docview_path_has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }

    const size_t path_len = strlen(path);
    const size_t suffix_len = strlen(suffix);
    if (suffix_len > path_len) {
        return false;
    }

    const char *tail = &path[path_len - suffix_len];
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool docview_path_is_markdown(const char *path)
{
    return docview_path_has_suffix(path, ".md") ||
        docview_path_has_suffix(path, ".markdown");
}

static void docview_draw_text_clipped(solar_os_gfx_t *gfx,
                                      int x,
                                      int baseline_y,
                                      int width,
                                      const char *text,
                                      int char_w)
{
    if (gfx == NULL || text == NULL || width <= 0 || char_w <= 0) {
        return;
    }

    char clipped[128];
    size_t max_chars = (size_t)(width / char_w);
    if (max_chars >= sizeof(clipped)) {
        max_chars = sizeof(clipped) - 1U;
    }

    const size_t len = strlen(text);
    const size_t n = len < max_chars ? len : max_chars;
    if (n > 0) {
        memcpy(clipped, text, n);
    }
    clipped[n] = '\0';
    solar_os_gfx_text(gfx, x, baseline_y, clipped);
}

static int docview_content_area_height(solar_os_gfx_t *gfx)
{
    const int screen_h = gfx != NULL ? (int)solar_os_gfx_height(gfx) : 0;
    const int height = screen_h - DOCVIEW_HEADER_H - (2 * DOCVIEW_MARGIN_Y);
    return height > 8 ? height : 8;
}

static int docview_content_width(solar_os_gfx_t *gfx)
{
    const int screen_w = gfx != NULL ? (int)solar_os_gfx_width(gfx) : 0;
    const int width = screen_w - (2 * DOCVIEW_MARGIN_X) - 4;
    return width > 8 ? width : 8;
}

static int docview_max_scroll(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !docview.loaded) {
        return 0;
    }

    const int view_h = docview_content_area_height(gfx);
    const int max_scroll = docview.content_height - view_h;
    return max_scroll > 0 ? max_scroll : 0;
}

static void docview_clamp_scroll(solar_os_gfx_t *gfx)
{
    const int max_scroll = docview_max_scroll(gfx);
    if (docview.scroll_y < 0) {
        docview.scroll_y = 0;
    } else if (docview.scroll_y > max_scroll) {
        docview.scroll_y = max_scroll;
    }
}

static void docview_update_measure(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || !docview.loaded) {
        docview.content_height = 0;
        return;
    }

    const int width = docview_content_width(gfx);
    if (!docview.layout_valid ||
        docview.layout_width != width ||
        docview.layout_zoom != docview.zoom) {
        const esp_err_t err = solar_os_doc_layout_build(&docview.layout,
                                                        &docview.doc,
                                                        width,
                                                        docview.zoom);
        if (err != ESP_OK) {
            docview.layout_valid = false;
            docview.content_height = 0;
            snprintf(docview.message,
                     sizeof(docview.message),
                     "layout failed: %s",
                     esp_err_to_name(err));
            return;
        }
        docview.layout_valid = true;
        docview.layout_width = width;
        docview.layout_zoom = docview.zoom;
    }
    docview.content_height = docview.layout.height;
    docview_clamp_scroll(gfx);
}

static bool docview_parse_position_line(char *line,
                                        uint64_t *offset,
                                        uint64_t *len,
                                        int *zoom,
                                        char **path)
{
    int path_index = 0;

    if (line == NULL || offset == NULL || len == NULL || zoom == NULL || path == NULL) {
        return false;
    }
    if (sscanf(line,
               "%" SCNu64 " %" SCNu64 " z=%d %n",
               offset,
               len,
               zoom,
               &path_index) != 3 ||
        path_index <= 0 ||
        line[path_index] == '\0') {
        return false;
    }

    *path = &line[path_index];
    while (isspace((unsigned char)**path)) {
        (*path)++;
    }
    (*path)[strcspn(*path, "\r\n")] = '\0';
    return (*path)[0] != '\0';
}

static bool docview_same_position_path(const char *line)
{
    char copy[DOCVIEW_POSITION_LINE_MAX];
    uint64_t offset = 0;
    uint64_t len = 0;
    int zoom = 0;
    char *path = NULL;

    if (line == NULL) {
        return false;
    }

    strlcpy(copy, line, sizeof(copy));
    return docview_parse_position_line(copy, &offset, &len, &zoom, &path) &&
        strcmp(path, docview.path) == 0;
}

static size_t docview_anchor_offset_for_scroll(void)
{
    if (!docview.layout_valid || docview.layout.line_count == 0) {
        return 0;
    }

    const solar_os_doc_layout_line_t *best = &docview.layout.lines[0];
    for (size_t i = 0; i < docview.layout.line_count; i++) {
        const solar_os_doc_layout_line_t *line = &docview.layout.lines[i];
        if (line->y + line->height > docview.scroll_y) {
            best = line;
            break;
        }
        best = line;
    }

    if (best->source_start != SIZE_MAX) {
        return best->source_start <= docview.doc.source_len ? best->source_start : docview.doc.source_len;
    }
    return 0;
}

static void docview_scroll_to_anchor(solar_os_context_t *ctx, uint64_t saved_offset)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    docview_update_measure(gfx);
    if (!docview.layout_valid) {
        return;
    }

    size_t offset = saved_offset > docview.doc.source_len ?
        docview.doc.source_len :
        (size_t)saved_offset;
    int y = 0;
    int height = 0;
    if (solar_os_doc_layout_source_to_xy(&docview.layout, offset, NULL, &y, &height)) {
        docview.scroll_y = y;
        docview_clamp_scroll(gfx);
    }
}

static void docview_load_position(solar_os_context_t *ctx)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[DOCVIEW_POSITION_LINE_MAX];

    if (docview_state_path(positions_path, sizeof(positions_path), DOCVIEW_POSITIONS_FILE) != ESP_OK) {
        return;
    }

    FILE *file = fopen(positions_path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        uint64_t saved_offset = 0;
        uint64_t saved_len = 0;
        int saved_zoom = 0;
        char *saved_path = NULL;
        if (!docview_parse_position_line(line, &saved_offset, &saved_len, &saved_zoom, &saved_path) ||
            strcmp(saved_path, docview.path) != 0) {
            continue;
        }

        if (saved_zoom >= 0 && saved_zoom <= DOCVIEW_MAX_ZOOM) {
            docview.zoom = saved_zoom;
        }
        docview.layout_valid = false;
        docview_scroll_to_anchor(ctx, saved_offset);
        snprintf(docview.message,
                 sizeof(docview.message),
                 saved_len == docview.doc.source_len ? "resumed" : "resumed, file changed");
        break;
    }

    fclose(file);
}

static void docview_save_position(solar_os_context_t *ctx)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char tmp_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[DOCVIEW_POSITION_LINE_MAX];

    if (ctx == NULL ||
        !docview.loaded ||
        docview.error_only ||
        docview.path[0] == '\0' ||
        docview_ensure_state_dir() != ESP_OK ||
        docview_state_path(positions_path, sizeof(positions_path), DOCVIEW_POSITIONS_FILE) != ESP_OK ||
        docview_state_path(tmp_path, sizeof(tmp_path), DOCVIEW_POSITIONS_TMP_FILE) != ESP_OK) {
        return;
    }

    docview_update_measure(solar_os_context_gfx(ctx));
    const size_t offset = docview_anchor_offset_for_scroll();

    FILE *source = fopen(positions_path, "r");
    FILE *dest = fopen(tmp_path, "w");
    if (dest == NULL) {
        if (source != NULL) {
            fclose(source);
        }
        return;
    }

    if (source != NULL) {
        while (fgets(line, sizeof(line), source) != NULL) {
            if (!docview_same_position_path(line)) {
                fputs(line, dest);
            }
        }
        fclose(source);
    }

    fprintf(dest,
            "%" PRIu64 " %" PRIu64 " z=%d %s\n",
            (uint64_t)offset,
            (uint64_t)docview.doc.source_len,
            docview.zoom,
            docview.path);

    const bool ok = ferror(dest) == 0;
    fclose(dest);
    if (!ok) {
        (void)remove(tmp_path);
        return;
    }

    (void)remove(positions_path);
    if (rename(tmp_path, positions_path) != 0) {
        (void)remove(tmp_path);
    }
}

static void docview_draw_header(solar_os_gfx_t *gfx)
{
    const int screen_w = (int)solar_os_gfx_width(gfx);
    char title[192];

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, DOCVIEW_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    if (docview.message[0] != '\0') {
        snprintf(title, sizeof(title), "docview z%d %s", docview.zoom, docview.message);
    } else {
        snprintf(title, sizeof(title), "docview z%d %s", docview.zoom, docview.display_name);
    }
    docview_draw_text_clipped(gfx, 3, 11, screen_w - 6, title, 6);
}

static void docview_draw_scrollbar(solar_os_gfx_t *gfx, const solar_os_doc_view_t *view)
{
    const int max_scroll = docview_max_scroll(gfx);
    if (gfx == NULL || view == NULL || max_scroll <= 0 || view->height <= 4) {
        return;
    }

    const int x = view->x + view->width + 2;
    if (x >= (int)solar_os_gfx_width(gfx)) {
        return;
    }

    int thumb_h = (view->height * view->height) / docview.content_height;
    if (thumb_h < 6) {
        thumb_h = 6;
    }
    if (thumb_h > view->height) {
        thumb_h = view->height;
    }
    const int track = view->height - thumb_h;
    const int thumb_y = view->y + ((track * docview.scroll_y) / max_scroll);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_line(gfx, x, view->y, x, view->y + view->height - 1);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, x - 1, thumb_y, 3, thumb_h);
}

static bool docview_line_for_scroll(size_t *line_index)
{
    if (line_index == NULL || !docview.layout_valid || docview.layout.line_count == 0) {
        return false;
    }

    size_t best = 0;
    for (size_t i = 0; i < docview.layout.line_count; i++) {
        const solar_os_doc_layout_line_t *line = &docview.layout.lines[i];
        if (line->y + line->height > docview.scroll_y) {
            *line_index = i;
            return true;
        }
        best = i;
    }

    *line_index = best;
    return true;
}

static void docview_scroll_to_line(solar_os_context_t *ctx, size_t line_index)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    docview_update_measure(gfx);
    if (!docview.layout_valid || docview.layout.line_count == 0) {
        return;
    }
    if (line_index >= docview.layout.line_count) {
        line_index = docview.layout.line_count - 1U;
    }
    docview.scroll_y = docview.layout.lines[line_index].y;
    docview_clamp_scroll(gfx);
}

static void docview_scroll_lines(solar_os_context_t *ctx, int delta_lines)
{
    docview_update_measure(solar_os_context_gfx(ctx));
    if (!docview.layout_valid || docview.layout.line_count == 0) {
        return;
    }

    size_t line_index = 0;
    if (!docview_line_for_scroll(&line_index)) {
        return;
    }

    int target = (int)line_index + delta_lines;
    if (target < 0) {
        target = 0;
    } else if (target >= (int)docview.layout.line_count) {
        target = (int)docview.layout.line_count - 1;
    }
    docview_scroll_to_line(ctx, (size_t)target);
}

static size_t docview_line_for_target_y(int target_y, bool forward)
{
    if (!docview.layout_valid || docview.layout.line_count == 0) {
        return 0;
    }

    size_t result = 0;
    if (forward) {
        for (size_t i = 0; i < docview.layout.line_count; i++) {
            if (docview.layout.lines[i].y >= target_y) {
                return i;
            }
            result = i;
        }
        return result;
    }

    for (size_t i = 0; i < docview.layout.line_count; i++) {
        if (docview.layout.lines[i].y > target_y) {
            break;
        }
        result = i;
    }
    return result;
}

static void docview_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    solar_os_doc_view_t view = {
        .x = DOCVIEW_MARGIN_X,
        .y = DOCVIEW_HEADER_H + DOCVIEW_MARGIN_Y,
        .width = screen_w - (2 * DOCVIEW_MARGIN_X) - 4,
        .height = screen_h - DOCVIEW_HEADER_H - (2 * DOCVIEW_MARGIN_Y),
        .scroll_y = docview.scroll_y,
        .zoom = docview.zoom,
    };
    if (view.width < 8) {
        view.width = 8;
    }
    if (view.height < 8) {
        view.height = 8;
    }

    if (docview.error_only) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        docview_draw_text_clipped(gfx,
                                  DOCVIEW_MARGIN_X,
                                  DOCVIEW_HEADER_H + 28,
                                  screen_w - (2 * DOCVIEW_MARGIN_X),
                                  docview.message,
                                  7);
    } else {
        docview_update_measure(gfx);
        view.scroll_y = docview.scroll_y;
        if (docview.layout_valid) {
            solar_os_doc_layout_render(gfx, &docview.doc, &docview.layout, &view);
        }
        docview_draw_scrollbar(gfx, &view);
    }

    docview_draw_header(gfx);
    solar_os_gfx_present(gfx);
}

static esp_err_t docview_load_doc_file(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!docview_file_exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_doc_load_path_as(&docview.doc, path, docview_path_is_markdown(path));
}

static esp_err_t docview_start(solar_os_context_t *ctx)
{
    memset(&docview, 0, sizeof(docview));
    solar_os_doc_init(&docview.doc);
    solar_os_doc_layout_init(&docview.layout);
    docview.zoom = 1;

    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        docview.error_only = true;
        snprintf(docview.message, sizeof(docview.message), "usage: docview <file.md|file.txt>");
        solar_os_context_set_graphics_active(ctx, true);
        docview_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    strlcpy(docview.display_name, arg != NULL ? arg : "", sizeof(docview.display_name));

    esp_err_t err = solar_os_storage_resolve_path(arg, docview.path, sizeof(docview.path));
    if (err != ESP_OK) {
        docview.error_only = true;
        snprintf(docview.message, sizeof(docview.message), "invalid path: %s", esp_err_to_name(err));
        solar_os_context_set_graphics_active(ctx, true);
        docview_render(ctx);
        return ESP_OK;
    }

    if (docview.display_name[0] == '\0') {
        strlcpy(docview.display_name, docview.path, sizeof(docview.display_name));
    }

    err = docview_load_doc_file(docview.path);
    if (err != ESP_OK) {
        docview.error_only = true;
        snprintf(docview.message, sizeof(docview.message), "load failed: %s", esp_err_to_name(err));
        solar_os_context_set_graphics_active(ctx, true);
        docview_render(ctx);
        return ESP_OK;
    }

    docview.loaded = true;
    solar_os_context_set_graphics_active(ctx, true);
    docview_load_position(ctx);
    docview_render(ctx);
    return ESP_OK;
}

static void docview_stop(solar_os_context_t *ctx)
{
    docview_save_position(ctx);
    solar_os_doc_free(&docview.doc);
    solar_os_doc_layout_free(&docview.layout);
    memset(&docview, 0, sizeof(docview));
    solar_os_context_set_graphics_active(ctx, false);
}

static void docview_page(solar_os_context_t *ctx, bool down)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    docview_update_measure(gfx);
    if (!docview.layout_valid || docview.layout.line_count == 0) {
        return;
    }

    size_t line_index = 0;
    if (!docview_line_for_scroll(&line_index)) {
        return;
    }

    const solar_os_doc_layout_line_t *line = &docview.layout.lines[line_index];
    const int view_h = docview_content_area_height(gfx);
    int overlap = line->height;
    if (overlap < 1) {
        overlap = 1;
    }
    int step = view_h - overlap;
    if (step < overlap) {
        step = view_h > 1 ? view_h - 1 : 1;
    }

    const int target_y = down ? docview.scroll_y + step : docview.scroll_y - step;
    const size_t target_line = docview_line_for_target_y(target_y, down);
    docview_scroll_to_line(ctx, target_line);
}

static void docview_zoom(solar_os_context_t *ctx, int delta)
{
    docview_update_measure(solar_os_context_gfx(ctx));
    const size_t anchor = docview_anchor_offset_for_scroll();
    const int old_zoom = docview.zoom;
    docview.zoom += delta;
    if (docview.zoom < 0) {
        docview.zoom = 0;
    } else if (docview.zoom > DOCVIEW_MAX_ZOOM) {
        docview.zoom = DOCVIEW_MAX_ZOOM;
    }
    if (docview.zoom != old_zoom) {
        docview.layout_valid = false;
        docview_update_measure(solar_os_context_gfx(ctx));
        docview_scroll_to_anchor(ctx, anchor);
    }
}

static bool docview_handle_char(solar_os_context_t *ctx, char raw_ch)
{
    const uint8_t ch = (uint8_t)raw_ch;

    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (docview.error_only) {
        return true;
    }

    switch (ch) {
    case SOLAR_OS_KEY_UP:
        docview_scroll_lines(ctx, -1);
        break;
    case SOLAR_OS_KEY_DOWN:
        docview_scroll_lines(ctx, 1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        docview_page(ctx, false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        docview_page(ctx, true);
        break;
    case SOLAR_OS_KEY_HOME:
        docview.scroll_y = 0;
        break;
    case SOLAR_OS_KEY_END:
        docview.scroll_y = docview_max_scroll(solar_os_context_gfx(ctx));
        break;
    case SOLAR_OS_KEY_CTRL_PLUS:
    case '+':
    case '=':
        docview_zoom(ctx, 1);
        break;
    case 0x1f:
    case '-':
        docview_zoom(ctx, -1);
        break;
    default:
        break;
    }
    return true;
}

static bool docview_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        docview_render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const bool handled = docview_handle_char(ctx, event->data.ch);
    if (handled) {
        docview_render(ctx);
    }
    return handled;
}

const solar_os_app_t solar_os_docview_app = {
    .name = "docview",
    .summary = "graphics Markdown/text viewer",
    .start = docview_start,
    .stop = docview_stop,
    .event = docview_event,
};
