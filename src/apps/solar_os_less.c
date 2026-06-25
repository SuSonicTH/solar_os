#include "solar_os_less.h"

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
#include "esp_heap_caps.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"

#define LESS_MAX_BYTES (2U * 1024U * 1024U)
#define LESS_SEARCH_MAX 64
#define LESS_MESSAGE_MAX 72
#define LESS_TAB_WIDTH 4
#define READER_SOFT_LINE_MIN 40U
#define READER_STATE_DIR ".reader"
#define READER_POSITIONS_FILE "positions"
#define READER_POSITIONS_TMP_FILE "positions.tmp"
#define READER_POSITION_LINE_MAX (SOLAR_OS_STORAGE_PATH_MAX + 64)
#define READER_TEXT_SIZE_COUNT 5

typedef enum {
    LESS_INPUT_NORMAL,
    LESS_INPUT_SEARCH,
} less_input_mode_t;

typedef struct {
    char *buffer;
    size_t len;
    size_t top_offset;
    size_t match_offset;
    bool match_valid;
    bool error_only;
    bool remember_position;
    bool saved_text_size_valid;
    bool reader_text_size_valid;
    less_input_mode_t input_mode;
    solar_os_terminal_text_size_t saved_text_size;
    solar_os_terminal_text_size_t reader_text_size;
    const char *app_name;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char display_name[SOLAR_OS_STORAGE_PATH_MAX];
    char search[LESS_SEARCH_MAX];
    size_t search_len;
    char message[LESS_MESSAGE_MAX];
} less_state_t;

static less_state_t less_state;
static solar_os_shell_io_t less_fallback_io;

static solar_os_shell_io_t *less_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&less_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &less_fallback_io);
        io = &less_fallback_io;
    }
    return io;
}

static solar_os_terminal_t *less_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

static const char *less_app_name(void)
{
    return less_state.app_name != NULL ? less_state.app_name : "less";
}

static bool less_is_printable(uint8_t ch)
{
    return isprint(ch) || ch >= 0xa0;
}

static void less_set_message(const char *message)
{
    strlcpy(less_state.message, message != NULL ? message : "", sizeof(less_state.message));
}

static size_t less_text_rows(solar_os_shell_io_t *io)
{
    const size_t rows = solar_os_shell_io_rows(io);
    return rows > 1 ? rows - 1 : 1;
}

static size_t less_cols_or_one(solar_os_shell_io_t *io)
{
    const size_t cols = solar_os_shell_io_cols(io);
    return cols > 0 ? cols : 1;
}

static size_t less_line_end(size_t offset)
{
    while (offset < less_state.len &&
           less_state.buffer[offset] != '\n' &&
           less_state.buffer[offset] != '\r') {
        offset++;
    }
    return offset;
}

static size_t less_next_line_start(size_t offset)
{
    size_t end = less_line_end(offset);
    if (end >= less_state.len) {
        return less_state.len;
    }
    if (less_state.buffer[end] == '\r' &&
        end + 1 < less_state.len &&
        less_state.buffer[end + 1] == '\n') {
        return end + 2;
    }
    return end + 1;
}

static size_t less_previous_line_start(size_t offset)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    if (offset == 0) {
        return 0;
    }

    if (less_state.buffer[offset - 1] == '\n') {
        offset--;
        if (offset > 0 && less_state.buffer[offset - 1] == '\r') {
            offset--;
        }
    } else if (less_state.buffer[offset - 1] == '\r') {
        offset--;
    } else {
        while (offset > 0 &&
               less_state.buffer[offset - 1] != '\n' &&
               less_state.buffer[offset - 1] != '\r') {
            offset--;
        }
        if (offset == 0) {
            return 0;
        }
        if (less_state.buffer[offset - 1] == '\n') {
            offset--;
            if (offset > 0 && less_state.buffer[offset - 1] == '\r') {
                offset--;
            }
        } else if (less_state.buffer[offset - 1] == '\r') {
            offset--;
        }
    }

    while (offset > 0 &&
           less_state.buffer[offset - 1] != '\n' &&
           less_state.buffer[offset - 1] != '\r') {
        offset--;
    }
    return offset;
}

static size_t less_line_start_for(size_t offset)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    while (offset > 0 &&
           less_state.buffer[offset - 1] != '\n' &&
           less_state.buffer[offset - 1] != '\r') {
        offset--;
    }
    return offset;
}

static bool less_is_wrap_space(uint8_t ch)
{
    return ch == ' ' || ch == '\t';
}

static size_t less_cell_width(uint8_t ch, size_t col)
{
    if (ch == '\t') {
        return LESS_TAB_WIDTH - (col % LESS_TAB_WIDTH);
    }
    return 1;
}

static size_t less_physical_visual_end(size_t offset, size_t cols, size_t *next_offset)
{
    if (cols == 0) {
        cols = 1;
    }
    if (next_offset != NULL) {
        *next_offset = offset;
    }
    if (offset >= less_state.len) {
        if (next_offset != NULL) {
            *next_offset = less_state.len;
        }
        return less_state.len;
    }

    const size_t line_end = less_line_end(offset);
    if (offset >= line_end) {
        const size_t next = less_next_line_start(offset);
        if (next_offset != NULL) {
            *next_offset = next > offset ? next : less_state.len;
        }
        return offset;
    }

    size_t col = 0;
    size_t end = offset;
    size_t last_space = SIZE_MAX;
    bool saw_nonspace = false;

    for (size_t i = offset; i < line_end; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        const size_t width = less_cell_width(byte, col);
        if (col > 0 && col + width > cols) {
            break;
        }

        col += width;
        end = i + 1;

        if (less_is_wrap_space(byte)) {
            if (saw_nonspace) {
                last_space = i;
            }
        } else {
            saw_nonspace = true;
        }

        if (col >= cols) {
            break;
        }
    }

    if (end >= line_end) {
        if (next_offset != NULL) {
            *next_offset = less_next_line_start(offset);
        }
        return line_end;
    }

    if (last_space != SIZE_MAX && last_space > offset) {
        size_t next = last_space + 1;
        while (next < line_end && less_is_wrap_space((uint8_t)less_state.buffer[next])) {
            next++;
        }
        if (next > offset) {
            if (next_offset != NULL) {
                *next_offset = next;
            }
            return last_space;
        }
    }

    if (end <= offset) {
        end = offset + 1;
    }
    if (next_offset != NULL) {
        size_t next = end;
        if (end > offset) {
            while (next < line_end && less_is_wrap_space((uint8_t)less_state.buffer[next])) {
                next++;
            }
        }
        *next_offset = next > offset ? next : end;
    }
    return end;
}

static bool reader_line_is_blank(size_t line_start)
{
    const size_t end = less_line_end(line_start);
    for (size_t i = line_start; i < end; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        if (byte != ' ' && byte != '\t') {
            return false;
        }
    }
    return true;
}

static size_t reader_line_trim_start(size_t line_start)
{
    const size_t end = less_line_end(line_start);
    while (line_start < end) {
        const uint8_t byte = (uint8_t)less_state.buffer[line_start];
        if (byte != ' ' && byte != '\t') {
            break;
        }
        line_start++;
    }
    return line_start;
}

static size_t reader_line_trim_end(size_t line_start)
{
    const size_t raw_end = less_line_end(line_start);
    size_t end = raw_end;
    while (end > line_start) {
        const uint8_t byte = (uint8_t)less_state.buffer[end - 1U];
        if (byte != ' ' && byte != '\t') {
            break;
        }
        end--;
    }
    return end;
}

static size_t reader_line_text_len(size_t line_start)
{
    const size_t start = reader_line_trim_start(line_start);
    const size_t end = reader_line_trim_end(line_start);
    return end > start ? end - start : 0;
}

static bool reader_line_is_indented(size_t line_start)
{
    const size_t end = less_line_end(line_start);
    return line_start < end &&
        (less_state.buffer[line_start] == ' ' || less_state.buffer[line_start] == '\t');
}

static bool reader_line_has_tab(size_t line_start)
{
    const size_t end = less_line_end(line_start);
    for (size_t i = line_start; i < end; i++) {
        if (less_state.buffer[i] == '\t') {
            return true;
        }
    }
    return false;
}

static bool reader_line_starts_list(size_t line_start)
{
    const size_t start = reader_line_trim_start(line_start);
    const size_t end = less_line_end(line_start);
    if (start >= end) {
        return false;
    }

    const uint8_t first = (uint8_t)less_state.buffer[start];
    if ((first == '-' || first == '*' || first == '+') &&
        start + 1U < end &&
        (less_state.buffer[start + 1U] == ' ' || less_state.buffer[start + 1U] == '\t')) {
        return true;
    }
    if (first == '>' || first == '#') {
        return true;
    }
    if (isdigit(first)) {
        size_t p = start + 1U;
        while (p < end && isdigit((unsigned char)less_state.buffer[p])) {
            p++;
        }
        return p > start &&
            p + 1U < end &&
            (less_state.buffer[p] == '.' || less_state.buffer[p] == ')') &&
            (less_state.buffer[p + 1U] == ' ' || less_state.buffer[p + 1U] == '\t');
    }
    return false;
}

static bool reader_lines_joinable(size_t current_line, size_t next_line)
{
    if (current_line >= less_state.len ||
        next_line >= less_state.len ||
        reader_line_is_blank(current_line) ||
        reader_line_is_blank(next_line) ||
        reader_line_is_indented(current_line) ||
        reader_line_is_indented(next_line) ||
        reader_line_has_tab(current_line) ||
        reader_line_has_tab(next_line) ||
        reader_line_starts_list(current_line) ||
        reader_line_starts_list(next_line) ||
        reader_line_text_len(current_line) < READER_SOFT_LINE_MIN) {
        return false;
    }
    return true;
}

static bool reader_flow_space(uint8_t byte)
{
    return byte == ' ' || byte == '\r' || byte == '\n';
}

static size_t reader_paragraph_start(size_t offset)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }

    size_t line = less_line_start_for(offset);
    if (line >= less_state.len || reader_line_is_blank(line)) {
        return line;
    }

    while (line > 0) {
        const size_t previous = less_previous_line_start(line);
        if (previous == line || !reader_lines_joinable(previous, line)) {
            break;
        }
        line = previous;
    }
    return line;
}

static size_t reader_paragraph_end(size_t offset)
{
    if (offset >= less_state.len) {
        return less_state.len;
    }

    size_t line = less_line_start_for(offset);
    if (reader_line_is_blank(line)) {
        return less_line_end(line);
    }

    while (line < less_state.len && !reader_line_is_blank(line)) {
        const size_t next = less_next_line_start(line);
        if (next <= line) {
            break;
        }
        if (!reader_lines_joinable(line, next)) {
            break;
        }
        line = next;
    }
    return less_line_end(line);
}

static size_t reader_skip_flow_space(size_t offset, size_t limit)
{
    while (offset < limit && reader_flow_space((uint8_t)less_state.buffer[offset])) {
        offset++;
    }
    return offset;
}

static size_t reader_word_end(size_t offset, size_t limit)
{
    while (offset < limit && !reader_flow_space((uint8_t)less_state.buffer[offset])) {
        offset++;
    }
    return offset;
}

static size_t reader_source_width(size_t start, size_t end, size_t col)
{
    size_t width = 0;
    for (size_t i = start; i < end; i++) {
        const size_t span = less_cell_width((uint8_t)less_state.buffer[i], col + width);
        width += span > 0 ? span : 1U;
    }
    return width;
}

static size_t reader_source_fit(size_t start, size_t end, size_t cols)
{
    size_t col = 0;
    size_t fit = start;

    for (size_t i = start; i < end; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        const size_t span = less_cell_width(byte, col);
        if (col > 0 && col + span > cols) {
            break;
        }
        col += span;
        fit = i + 1U;
        if (col >= cols) {
            break;
        }
    }

    return fit > start ? fit : (start < end ? start + 1U : start);
}

static size_t reader_visual_end(size_t offset, size_t cols, size_t *next_offset)
{
    if (cols == 0) {
        cols = 1;
    }
    if (next_offset != NULL) {
        *next_offset = offset;
    }
    if (offset >= less_state.len) {
        if (next_offset != NULL) {
            *next_offset = less_state.len;
        }
        return less_state.len;
    }

    const size_t line_start = less_line_start_for(offset);
    if (reader_line_is_blank(line_start)) {
        if (next_offset != NULL) {
            *next_offset = less_next_line_start(line_start);
        }
        return line_start;
    }
    if (reader_line_is_indented(line_start) ||
        reader_line_has_tab(line_start) ||
        reader_line_starts_list(line_start)) {
        return less_physical_visual_end(offset, cols, next_offset);
    }

    const size_t paragraph_end = reader_paragraph_end(offset);
    size_t pos = reader_skip_flow_space(offset, paragraph_end);
    size_t end = pos;
    size_t col = 0;

    while (pos < paragraph_end) {
        const size_t word_start = reader_skip_flow_space(pos, paragraph_end);
        if (word_start >= paragraph_end) {
            break;
        }
        const size_t word_end = reader_word_end(word_start, paragraph_end);
        const size_t word_width = reader_source_width(word_start, word_end, col > 0 ? col + 1U : col);
        const size_t prefix = col > 0 ? 1U : 0U;

        if (col > 0 && col + prefix + word_width > cols) {
            if (next_offset != NULL) {
                *next_offset = word_start;
            }
            return end;
        }

        if (col == 0 && word_width > cols) {
            const size_t split = reader_source_fit(word_start, word_end, cols);
            if (next_offset != NULL) {
                *next_offset = split;
            }
            return split;
        }

        col += prefix + word_width;
        end = word_end;
        pos = word_end;
    }

    if (next_offset != NULL) {
        const size_t next = reader_skip_flow_space(end, paragraph_end);
        *next_offset = next < paragraph_end ? next : less_next_line_start(less_line_start_for(paragraph_end));
    }
    return end;
}

static size_t less_visual_end(size_t offset, size_t cols, size_t *next_offset)
{
    if (less_state.remember_position) {
        return reader_visual_end(offset, cols, next_offset);
    }

    return less_physical_visual_end(offset, cols, next_offset);
}

static size_t less_next_visual_start(size_t offset, size_t cols)
{
    size_t next = offset;
    (void)less_visual_end(offset, cols, &next);
    if (next <= offset && offset < less_state.len) {
        next = offset + 1;
    }
    return next;
}

static size_t less_visual_start_for_offset(size_t offset, size_t cols)
{
    if (offset > less_state.len) {
        offset = less_state.len;
    }

    size_t current = less_state.remember_position ?
        reader_paragraph_start(offset) :
        less_line_start_for(offset);
    while (current < offset) {
        const size_t next = less_next_visual_start(current, cols);
        if (next > offset || next <= current) {
            break;
        }
        current = next;
    }
    return current;
}

static size_t reader_previous_visual_start(size_t offset, size_t cols)
{
    if (offset == 0 || less_state.len == 0) {
        return 0;
    }
    if (offset > less_state.len) {
        offset = less_state.len;
    }

    size_t start = reader_paragraph_start(offset);
    if (start >= offset && offset > 0) {
        const size_t previous_line = less_previous_line_start(offset);
        if (previous_line < offset) {
            start = reader_paragraph_start(previous_line);
        } else {
            start = 0;
        }
    }

    size_t previous = start;
    size_t current = start;
    while (current < offset) {
        const size_t next = less_next_visual_start(current, cols);
        if (next >= offset || next <= current) {
            return previous;
        }
        previous = current;
        current = next;
    }
    return previous;
}

static size_t less_previous_visual_start(size_t offset, size_t cols)
{
    if (offset == 0 || less_state.len == 0) {
        return 0;
    }
    if (offset > less_state.len) {
        offset = less_state.len;
    }
    if (less_state.remember_position) {
        return reader_previous_visual_start(offset, cols);
    }

    size_t line_start = less_line_start_for(offset);
    if (line_start == offset) {
        const size_t previous_line = less_previous_line_start(offset);
        if (previous_line == offset) {
            return offset;
        }
        line_start = previous_line;
    }

    size_t previous = line_start;
    size_t current = line_start;
    while (current < offset) {
        const size_t next = less_next_visual_start(current, cols);
        if (next >= offset || next <= current) {
            return previous;
        }
        previous = current;
        current = next;
    }
    return previous;
}

static void less_write_inverse_line(solar_os_shell_io_t *io, const char *text)
{
    const size_t cols = solar_os_shell_io_cols(io);
    size_t written = 0;

    solar_os_shell_io_set_cursor(io, 0, 0);
    solar_os_shell_io_set_inverse(io, true);
    while (text != NULL && text[written] != '\0' && written < cols) {
        solar_os_shell_io_put_char(io,
                                   less_is_printable((uint8_t)text[written]) ?
                                       text[written] :
                                       '.');
        written++;
    }
    while (written < cols) {
        solar_os_shell_io_put_char(io, ' ');
        written++;
    }
    solar_os_shell_io_set_inverse(io, false);
}

static void less_render_header(solar_os_shell_io_t *io)
{
    char header[SOLAR_OS_STORAGE_PATH_MAX + LESS_SEARCH_MAX + LESS_MESSAGE_MAX + 32];
    const unsigned percent =
        less_state.len == 0 ? 100U : (unsigned)(((uint64_t)less_state.top_offset * 100ULL) /
                                                (uint64_t)less_state.len);

    if (less_state.input_mode == LESS_INPUT_SEARCH) {
        snprintf(header,
                 sizeof(header),
                 "%s %s  /%s",
                 less_app_name(),
                 less_state.display_name,
                 less_state.search);
    } else if (less_state.message[0] != '\0') {
        snprintf(header,
                 sizeof(header),
                 "%s %s  %u%%  %s",
                 less_app_name(),
                 less_state.display_name,
                 percent,
                 less_state.message);
    } else {
        snprintf(header,
                 sizeof(header),
                 "%s %s  %u%%",
                 less_app_name(),
                 less_state.display_name,
                 percent);
    }

    less_write_inverse_line(io, header);
}

static bool less_source_highlighted(size_t offset)
{
    return less_state.match_valid &&
        offset >= less_state.match_offset &&
        offset < less_state.match_offset + less_state.search_len;
}

static void less_write_source_byte(solar_os_shell_io_t *io,
                                   uint8_t byte,
                                   size_t source_offset,
                                   size_t *visual_col,
                                   size_t *written,
                                   size_t cols)
{
    const bool is_tab = byte == '\t';
    const size_t span = less_cell_width(byte, *visual_col);

    solar_os_shell_io_set_inverse(io, less_source_highlighted(source_offset));
    for (size_t j = 0; j < span && *written < cols; j++) {
        if (is_tab) {
            solar_os_shell_io_put_char(io, ' ');
        } else {
            solar_os_shell_io_put_char(io, less_is_printable(byte) ? (char)byte : '.');
        }
        (*written)++;
        (*visual_col)++;
    }
}

static void less_render_physical_text_row(solar_os_shell_io_t *io, size_t row, size_t row_start)
{
    const size_t cols = less_cols_or_one(io);
    size_t next = row_start;
    const size_t row_end = less_physical_visual_end(row_start, cols, &next);
    size_t visual_col = 0;
    size_t written = 0;

    solar_os_shell_io_set_cursor(io, row, 0);
    for (size_t i = row_start; i < row_end && written < cols; i++) {
        const uint8_t byte = (uint8_t)less_state.buffer[i];
        less_write_source_byte(io, byte, i, &visual_col, &written, cols);
    }
    solar_os_shell_io_set_inverse(io, false);
}

static void reader_render_text_row(solar_os_shell_io_t *io, size_t row, size_t row_start)
{
    const size_t cols = less_cols_or_one(io);
    size_t visual_col = 0;
    size_t written = 0;

    solar_os_shell_io_set_cursor(io, row, 0);
    if (row_start >= less_state.len) {
        return;
    }

    const size_t line_start = less_line_start_for(row_start);
    if (reader_line_is_blank(line_start)) {
        return;
    }
    if (reader_line_is_indented(line_start) ||
        reader_line_has_tab(line_start) ||
        reader_line_starts_list(line_start)) {
        less_render_physical_text_row(io, row, row_start);
        return;
    }

    const size_t paragraph_end = reader_paragraph_end(row_start);
    size_t pos = reader_skip_flow_space(row_start, paragraph_end);

    while (pos < paragraph_end && written < cols) {
        const size_t word_start = reader_skip_flow_space(pos, paragraph_end);
        if (word_start >= paragraph_end) {
            break;
        }
        const size_t word_end = reader_word_end(word_start, paragraph_end);
        const size_t word_width = reader_source_width(word_start,
                                                      word_end,
                                                      visual_col > 0 ? visual_col + 1U : visual_col);
        const size_t prefix = visual_col > 0 ? 1U : 0U;

        if (visual_col > 0 && visual_col + prefix + word_width > cols) {
            break;
        }

        if (visual_col > 0) {
            solar_os_shell_io_set_inverse(io, false);
            solar_os_shell_io_put_char(io, ' ');
            visual_col++;
            written++;
        }

        for (size_t i = word_start; i < word_end && written < cols; i++) {
            less_write_source_byte(io,
                                   (uint8_t)less_state.buffer[i],
                                   i,
                                   &visual_col,
                                   &written,
                                   cols);
        }

        pos = word_end;
        if (prefix == 0U && word_width > cols) {
            break;
        }
    }
    solar_os_shell_io_set_inverse(io, false);
}

static void less_render_text_row(solar_os_shell_io_t *io, size_t row, size_t row_start)
{
    if (less_state.remember_position) {
        reader_render_text_row(io, row, row_start);
        return;
    }

    less_render_physical_text_row(io, row, row_start);
}

static void less_render_error(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = less_io(ctx);

    solar_os_shell_io_clear(io);
    less_write_inverse_line(io, less_app_name());
    solar_os_shell_io_set_cursor(io, 1, 0);
    if (less_state.message[0] != '\0') {
        solar_os_shell_io_writeln(io, less_state.message);
    }
    solar_os_shell_io_printf(io, "usage: %s <file>\n", less_app_name());
    solar_os_shell_io_writeln(io, "keys: arrows, PgUp/PgDn, / search, n/N, q");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    solar_os_shell_io_flush(io);
}

static void less_render(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = less_io(ctx);

    if (less_state.error_only) {
        less_render_error(ctx);
        return;
    }

    solar_os_shell_io_clear(io);
    less_render_header(io);

    size_t offset = less_state.top_offset;
    const size_t rows = less_text_rows(io);
    const size_t cols = less_cols_or_one(io);
    for (size_t row = 0; row < rows; row++) {
        if (offset < less_state.len || (less_state.len == 0 && row == 0)) {
            less_render_text_row(io, row + 1, offset);
        }
        if (offset >= less_state.len) {
            continue;
        }
        offset = less_next_visual_start(offset, cols);
    }
    solar_os_shell_io_flush(io);
}

static void less_move_down(solar_os_context_t *ctx, size_t lines)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    for (size_t i = 0; i < lines; i++) {
        const size_t next = less_next_visual_start(less_state.top_offset, cols);
        if (next >= less_state.len) {
            break;
        }
        less_state.top_offset = next;
    }
    less_set_message("");
}

static void less_move_up(solar_os_context_t *ctx, size_t lines)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    for (size_t i = 0; i < lines; i++) {
        const size_t previous = less_previous_visual_start(less_state.top_offset, cols);
        if (previous == less_state.top_offset) {
            break;
        }
        less_state.top_offset = previous;
        if (less_state.top_offset == 0) {
            break;
        }
    }
    less_set_message("");
}

static void less_page_down(solar_os_context_t *ctx)
{
    const size_t rows = less_text_rows(less_io(ctx));
    less_move_down(ctx, rows > 1 ? rows - 1 : 1);
}

static void less_page_up(solar_os_context_t *ctx)
{
    const size_t rows = less_text_rows(less_io(ctx));
    less_move_up(ctx, rows > 1 ? rows - 1 : 1);
}

static void less_move_bottom(solar_os_context_t *ctx)
{
    size_t offset = less_state.len;
    solar_os_shell_io_t *io = less_io(ctx);
    const size_t rows = less_text_rows(io);
    const size_t cols = less_cols_or_one(io);

    for (size_t i = 0; i < rows; i++) {
        const size_t previous = less_previous_visual_start(offset, cols);
        offset = previous;
        if (offset == 0) {
            break;
        }
    }
    less_state.top_offset = offset;
    less_set_message("");
}

static bool reader_adjust_text_size(solar_os_context_t *ctx, int delta)
{
    static const solar_os_terminal_text_size_t text_sizes[READER_TEXT_SIZE_COUNT] = {
        SOLAR_OS_TERMINAL_TEXT_SIZE_12,
        SOLAR_OS_TERMINAL_TEXT_SIZE_14,
        SOLAR_OS_TERMINAL_TEXT_SIZE_16,
        SOLAR_OS_TERMINAL_TEXT_SIZE_18,
        SOLAR_OS_TERMINAL_TEXT_SIZE_20,
    };

    if (!less_state.remember_position) {
        return false;
    }
    if (solar_os_shell_io_kind(less_io(ctx)) != SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        less_set_message("textsize display only");
        return true;
    }

    solar_os_terminal_t *term = less_terminal(ctx);
    const solar_os_terminal_text_size_t current = solar_os_terminal_text_size(term);
    size_t index = 0;
    for (size_t i = 0; i < READER_TEXT_SIZE_COUNT; i++) {
        if (text_sizes[i] == current) {
            index = i;
            break;
        }
    }

    size_t next = index;
    if (delta > 0) {
        if (next + 1 < READER_TEXT_SIZE_COUNT) {
            next++;
        }
    } else if (delta < 0 && next > 0) {
        next--;
    }

    if (next == index) {
        less_set_message(delta > 0 ? "textsize max" : "textsize min");
        return true;
    }

    const size_t anchor = less_state.top_offset;
    if (solar_os_terminal_set_text_size_transient(term, text_sizes[next]) != ESP_OK) {
        less_set_message("textsize failed");
        return true;
    }

    less_state.reader_text_size = text_sizes[next];
    less_state.reader_text_size_valid = true;
    less_state.top_offset = less_visual_start_for_offset(anchor, less_cols_or_one(less_io(ctx)));
    char message[LESS_MESSAGE_MAX];
    snprintf(message,
             sizeof(message),
             "textsize %s",
             solar_os_terminal_text_size_name(text_sizes[next]));
    less_set_message(message);
    return true;
}

static bool less_search_char_equal(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool less_search_matches_at(size_t offset)
{
    if (less_state.search_len == 0 || offset + less_state.search_len > less_state.len) {
        return false;
    }

    for (size_t i = 0; i < less_state.search_len; i++) {
        if (!less_search_char_equal(less_state.buffer[offset + i], less_state.search[i])) {
            return false;
        }
    }
    return true;
}

static bool less_find_forward(size_t start, size_t *found)
{
    if (less_state.search_len == 0 || less_state.search_len > less_state.len) {
        return false;
    }
    if (start > less_state.len - less_state.search_len) {
        return false;
    }

    for (size_t i = start; i <= less_state.len - less_state.search_len; i++) {
        if (less_search_matches_at(i)) {
            *found = i;
            return true;
        }
    }
    return false;
}

static bool less_find_backward(size_t start, size_t *found)
{
    if (less_state.search_len == 0 || less_state.search_len > less_state.len) {
        return false;
    }
    if (start > less_state.len - less_state.search_len) {
        start = less_state.len - less_state.search_len;
    }

    for (size_t i = start + 1; i > 0; i--) {
        const size_t offset = i - 1;
        if (less_search_matches_at(offset)) {
            *found = offset;
            return true;
        }
    }
    return false;
}

static void less_apply_match(solar_os_context_t *ctx, size_t offset)
{
    const size_t cols = less_cols_or_one(less_io(ctx));

    less_state.match_offset = offset;
    less_state.match_valid = true;
    less_state.top_offset = less_visual_start_for_offset(offset, cols);
}

static bool less_find_search(solar_os_context_t *ctx, bool forward, bool next)
{
    size_t found = 0;
    bool wrapped = false;
    bool ok = false;

    if (less_state.search_len == 0) {
        less_set_message("empty search");
        return false;
    }

    if (forward) {
        size_t start = less_state.top_offset;
        if (next && less_state.match_valid && less_state.match_offset + 1 < less_state.len) {
            start = less_state.match_offset + 1;
        }
        ok = less_find_forward(start, &found);
        if (!ok && start > 0) {
            wrapped = true;
            ok = less_find_forward(0, &found);
        }
    } else {
        size_t start = less_state.top_offset;
        if (next && less_state.match_valid && less_state.match_offset > 0) {
            start = less_state.match_offset - 1;
        } else if (start > 0) {
            start--;
        }
        ok = less_find_backward(start, &found);
        if (!ok && less_state.len >= less_state.search_len) {
            wrapped = true;
            ok = less_find_backward(less_state.len - less_state.search_len, &found);
        }
    }

    if (!ok) {
        less_state.match_valid = false;
        less_set_message("not found");
        return false;
    }

    less_apply_match(ctx, found);
    less_set_message(wrapped ? "wrapped" : "");
    return true;
}

static void less_start_search(void)
{
    less_state.input_mode = LESS_INPUT_SEARCH;
    less_state.search_len = 0;
    less_state.search[0] = '\0';
    less_set_message("");
}

static void less_submit_search(solar_os_context_t *ctx)
{
    less_state.input_mode = LESS_INPUT_NORMAL;
    less_state.match_valid = false;
    (void)less_find_search(ctx, true, false);
}

static void less_cancel_search(void)
{
    less_state.input_mode = LESS_INPUT_NORMAL;
    less_set_message("");
}

static bool less_handle_search_input(solar_os_context_t *ctx, uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        less_cancel_search();
        break;
    case '\r':
    case '\n':
        less_submit_search(ctx);
        break;
    case '\b':
        if (less_state.search_len > 0) {
            less_state.search[--less_state.search_len] = '\0';
        }
        break;
    default:
        if (less_is_printable(ch) && less_state.search_len + 1 < sizeof(less_state.search)) {
            less_state.search[less_state.search_len++] = (char)ch;
            less_state.search[less_state.search_len] = '\0';
        }
        break;
    }

    less_render(ctx);
    return true;
}

static esp_err_t less_load_file(void)
{
    struct stat st;
    if (stat(less_state.path, &st) != 0 || !S_ISREG(st.st_mode)) {
        less_set_message("not a file");
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > LESS_MAX_BYTES) {
        less_set_message("file too large");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t len = (size_t)st.st_size;
    less_state.buffer = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (less_state.buffer == NULL) {
        less_state.buffer = heap_caps_malloc(len + 1, MALLOC_CAP_8BIT);
    }
    if (less_state.buffer == NULL) {
        less_set_message("out of memory");
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(less_state.path, "rb");
    if (file == NULL) {
        char message[sizeof(less_state.message)];
        snprintf(message, sizeof(message), "open failed: %s", strerror(errno));
        less_set_message(message);
        return ESP_FAIL;
    }

    const size_t read_len = len > 0 ? fread(less_state.buffer, 1, len, file) : 0;
    const bool read_error = ferror(file);
    fclose(file);
    if (read_error || read_len != len) {
        less_set_message("read failed");
        return ESP_FAIL;
    }

    less_state.buffer[len] = '\0';
    less_state.len = len;
    return ESP_OK;
}

static esp_err_t reader_state_path(char *path, size_t path_len, const char *leaf)
{
    if (path == NULL || path_len == 0 || !solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *mount = solar_os_storage_mount_point();
    const int written = leaf == NULL ?
        snprintf(path, path_len, "%s/%s", mount, READER_STATE_DIR) :
        snprintf(path, path_len, "%s/%s/%s", mount, READER_STATE_DIR, leaf);
    return written >= 0 && (size_t)written < path_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t reader_ensure_state_dir(void)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = reader_state_path(dir, sizeof(dir), NULL);
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

static bool reader_parse_position_line(char *line,
                                       uint64_t *offset,
                                       uint64_t *len,
                                       solar_os_terminal_text_size_t *text_size,
                                       bool *text_size_valid,
                                       char **path)
{
    int path_index = 0;

    if (line == NULL ||
        offset == NULL ||
        len == NULL ||
        text_size == NULL ||
        text_size_valid == NULL ||
        path == NULL) {
        return false;
    }
    *text_size_valid = false;
    if (sscanf(line, "%" SCNu64 " %" SCNu64 " %n", offset, len, &path_index) != 2 ||
        path_index <= 0 ||
        line[path_index] == '\0') {
        return false;
    }

    char *rest = &line[path_index];
    if (strncmp(rest, "ts=", 3) == 0) {
        char *token = rest + 3;
        char *token_end = token;
        while (*token_end != '\0' && !isspace((unsigned char)*token_end)) {
            token_end++;
        }
        if (*token_end != '\0') {
            *token_end++ = '\0';
            while (isspace((unsigned char)*token_end)) {
                token_end++;
            }
            if (solar_os_terminal_parse_text_size(token, text_size)) {
                *text_size_valid = true;
                rest = token_end;
            }
        }
    }

    *path = rest;
    (*path)[strcspn(*path, "\r\n")] = '\0';
    return (*path)[0] != '\0';
}

static void reader_load_position(solar_os_context_t *ctx)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[READER_POSITION_LINE_MAX];

    if (!less_state.remember_position ||
        reader_state_path(positions_path, sizeof(positions_path), READER_POSITIONS_FILE) != ESP_OK) {
        return;
    }

    FILE *file = fopen(positions_path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        uint64_t saved_offset = 0;
        uint64_t saved_len = 0;
        solar_os_terminal_text_size_t saved_text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;
        bool saved_text_size_valid = false;
        char *saved_path = NULL;
        if (!reader_parse_position_line(line,
                                        &saved_offset,
                                        &saved_len,
                                        &saved_text_size,
                                        &saved_text_size_valid,
                                        &saved_path) ||
            strcmp(saved_path, less_state.path) != 0) {
            continue;
        }

        if (saved_text_size_valid &&
            solar_os_shell_io_kind(less_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_TERMINAL &&
            solar_os_terminal_set_text_size_transient(less_terminal(ctx), saved_text_size) == ESP_OK) {
            less_state.reader_text_size = saved_text_size;
            less_state.reader_text_size_valid = true;
        }
        if (saved_offset > less_state.len) {
            saved_offset = less_state.len;
        }
        less_state.top_offset =
            less_visual_start_for_offset((size_t)saved_offset,
                                         less_cols_or_one(less_io(ctx)));
        less_set_message(saved_len == less_state.len ? "resumed" : "resumed, file changed");
        break;
    }

    fclose(file);
}

static bool reader_same_position_path(const char *line)
{
    char copy[READER_POSITION_LINE_MAX];
    uint64_t offset = 0;
    uint64_t len = 0;
    solar_os_terminal_text_size_t text_size = SOLAR_OS_TERMINAL_TEXT_SIZE_14;
    bool text_size_valid = false;
    char *path = NULL;

    if (line == NULL) {
        return false;
    }

    strlcpy(copy, line, sizeof(copy));
    return reader_parse_position_line(copy, &offset, &len, &text_size, &text_size_valid, &path) &&
        strcmp(path, less_state.path) == 0;
}

static void reader_save_position(void)
{
    char positions_path[SOLAR_OS_STORAGE_PATH_MAX];
    char tmp_path[SOLAR_OS_STORAGE_PATH_MAX];
    char line[READER_POSITION_LINE_MAX];

    if (!less_state.remember_position ||
        less_state.error_only ||
        less_state.path[0] == '\0' ||
        reader_ensure_state_dir() != ESP_OK ||
        reader_state_path(positions_path, sizeof(positions_path), READER_POSITIONS_FILE) != ESP_OK ||
        reader_state_path(tmp_path, sizeof(tmp_path), READER_POSITIONS_TMP_FILE) != ESP_OK) {
        return;
    }

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
            if (!reader_same_position_path(line)) {
                fputs(line, dest);
            }
        }
        fclose(source);
    }

    if (less_state.reader_text_size_valid) {
        fprintf(dest,
                "%" PRIu64 " %" PRIu64 " ts=%s %s\n",
                (uint64_t)less_state.top_offset,
                (uint64_t)less_state.len,
                solar_os_terminal_text_size_name(less_state.reader_text_size),
                less_state.path);
    } else {
        fprintf(dest,
                "%" PRIu64 " %" PRIu64 " %s\n",
                (uint64_t)less_state.top_offset,
                (uint64_t)less_state.len,
                less_state.path);
    }
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

static esp_err_t less_start_common(solar_os_context_t *ctx,
                                   const char *app_name,
                                   bool remember_position)
{
    memset(&less_state, 0, sizeof(less_state));
    less_state.app_name = app_name;
    less_state.remember_position = remember_position;
    if (remember_position &&
        solar_os_shell_io_kind(less_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        const solar_os_terminal_text_size_t current_text_size =
            solar_os_terminal_text_size(less_terminal(ctx));
        less_state.saved_text_size = current_text_size;
        less_state.saved_text_size_valid = true;
        less_state.reader_text_size = current_text_size;
        less_state.reader_text_size_valid = true;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc != 2) {
        less_state.error_only = true;
        char message[LESS_MESSAGE_MAX];
        snprintf(message, sizeof(message), "usage: %s <file>", less_app_name());
        less_set_message(message);
        less_render(ctx);
        return ESP_OK;
    }

    const char *arg = solar_os_context_argv(ctx, 1);
    strlcpy(less_state.display_name, arg != NULL ? arg : "", sizeof(less_state.display_name));

    esp_err_t err = solar_os_storage_resolve_path(arg, less_state.path, sizeof(less_state.path));
    if (err != ESP_OK) {
        less_state.error_only = true;
        less_set_message(err == ESP_ERR_INVALID_SIZE ? "path too long" : "invalid path");
        less_render(ctx);
        return ESP_OK;
    }

    err = less_load_file();
    if (err != ESP_OK) {
        less_state.error_only = true;
        less_render(ctx);
        return ESP_OK;
    }

    less_state.top_offset = 0;
    reader_load_position(ctx);
    less_render(ctx);
    return ESP_OK;
}

static esp_err_t less_start(solar_os_context_t *ctx)
{
    return less_start_common(ctx, "less", false);
}

static esp_err_t reader_start(solar_os_context_t *ctx)
{
    return less_start_common(ctx, "reader", true);
}

static void less_stop(solar_os_context_t *ctx)
{
    reader_save_position();
    if (less_state.remember_position &&
        less_state.saved_text_size_valid &&
        solar_os_shell_io_kind(less_io(ctx)) == SOLAR_OS_SHELL_IO_KIND_TERMINAL) {
        (void)solar_os_terminal_set_text_size_transient(less_terminal(ctx),
                                                       less_state.saved_text_size);
    }
    heap_caps_free(less_state.buffer);
    memset(&less_state, 0, sizeof(less_state));
}

static bool less_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (less_state.error_only) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
        }
        return true;
    }

    if (ch == SOLAR_OS_KEY_CTRL_PLUS) {
        if (reader_adjust_text_size(ctx, 1)) {
            less_render(ctx);
        }
        return true;
    }
    if (ch == 0x1f) {
        if (reader_adjust_text_size(ctx, -1)) {
            less_render(ctx);
        }
        return true;
    }

    if (less_state.input_mode == LESS_INPUT_SEARCH) {
        return less_handle_search_input(ctx, ch);
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case 'q':
    case 'Q':
        solar_os_context_request_exit(ctx);
        return true;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        less_move_down(ctx, 1);
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        less_move_up(ctx, 1);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
    case ' ':
    case 0x06:
        less_page_down(ctx);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
    case 'b':
    case 'B':
    case 0x02:
        less_page_up(ctx);
        break;
    case SOLAR_OS_KEY_HOME:
    case 'g':
        less_state.top_offset = 0;
        less_set_message("");
        break;
    case SOLAR_OS_KEY_END:
    case 'G':
        less_move_bottom(ctx);
        break;
    case '/':
        less_start_search();
        break;
    case 'n':
        (void)less_find_search(ctx, true, true);
        break;
    case 'N':
        (void)less_find_search(ctx, false, true);
        break;
    default:
        return true;
    }

    less_render(ctx);
    return true;
}

const solar_os_app_t solar_os_less_app = {
    .name = "less",
    .summary = "text file pager",
    .start = less_start,
    .stop = less_stop,
    .event = less_event,
};

const solar_os_app_t solar_os_reader_app = {
    .name = "reader",
    .summary = "resumable text reader",
    .start = reader_start,
    .stop = less_stop,
    .event = less_event,
};
