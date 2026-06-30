#include "solar_os_com.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_log.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_port.h"
#include "solar_os_terminal.h"
#include "solar_os_uart.h"

#define COM_RX_BUFFER_SIZE 128
#define COM_RX_CHUNKS_PER_TICK 16

typedef struct {
    bool active;
    bool alt_prefix_pending;
    bool port_claimed;
    solar_os_port_handle_t port;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} com_app_state_t;

static const char *TAG = "solar_os_com";
static com_app_state_t com_app;

static solar_os_terminal_t *com_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

static void com_render_header(solar_os_context_t *ctx)
{
    solar_os_terminal_t *term = com_terminal(ctx);
    solar_os_uart_status_t status;
    solar_os_uart_get_status(&status);

    solar_os_terminal_clear(term);
    solar_os_terminal_printf_bold(term,
                                  "COM UART%d %" PRIu32 " %s\n",
                                  status.port_num,
                                  status.baud_rate,
                                  solar_os_uart_mode_name(status.mode));
    solar_os_terminal_printf(term, "TX %d RX %d\n", status.tx_pin, status.rx_pin);
    solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
    solar_os_terminal_put_char(term, '\n');
}

static esp_err_t com_write_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    size_t written = 0;
    const esp_err_t err = solar_os_port_write(&com_app.port, data, len, &written);
    if (err == ESP_OK) {
        com_app.tx_bytes += written;
    } else {
        SOLAR_OS_LOGW(TAG, "UART write failed: %s", esp_err_to_name(err));
    }
    if (written != len) {
        SOLAR_OS_LOGW(TAG, "UART write incomplete: %u/%u", (unsigned)written, (unsigned)len);
    }
    return err;
}

static void com_send_key(solar_os_context_t *ctx, char ch)
{
    solar_os_terminal_t *term = com_terminal(ctx);
    const uint8_t key = (uint8_t)ch;
    const char *seq = NULL;
    uint8_t data[2] = {0};
    size_t len = 0;

    switch (key) {
    case SOLAR_OS_KEY_ALT_PREFIX:
        com_app.alt_prefix_pending = true;
        return;
    case SOLAR_OS_KEY_UP:
        seq = "\x1b[A";
        break;
    case SOLAR_OS_KEY_DOWN:
        seq = "\x1b[B";
        break;
    case SOLAR_OS_KEY_RIGHT:
        seq = "\x1b[C";
        break;
    case SOLAR_OS_KEY_LEFT:
        seq = "\x1b[D";
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        seq = "\x1b[5~";
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        seq = "\x1b[6~";
        break;
    case SOLAR_OS_KEY_HOME:
        seq = "\x1b[H";
        break;
    case SOLAR_OS_KEY_END:
        seq = "\x1b[F";
        break;
    case SOLAR_OS_KEY_DELETE:
        seq = "\x1b[3~";
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
        seq = "\x1b[1;2A";
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
        seq = "\x1b[1;2B";
        break;
    case SOLAR_OS_KEY_SHIFT_RIGHT:
        seq = "\x1b[1;2C";
        break;
    case SOLAR_OS_KEY_SHIFT_LEFT:
        seq = "\x1b[1;2D";
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP:
        seq = "\x1b[5;2~";
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
        seq = "\x1b[6;2~";
        break;
    case SOLAR_OS_KEY_SHIFT_HOME:
        seq = "\x1b[1;2H";
        break;
    case SOLAR_OS_KEY_SHIFT_END:
        seq = "\x1b[1;2F";
        break;
    case SOLAR_OS_KEY_CTRL_UP:
        seq = "\x1b[1;5A";
        break;
    case SOLAR_OS_KEY_CTRL_DOWN:
        seq = "\x1b[1;5B";
        break;
    case SOLAR_OS_KEY_CTRL_RIGHT:
        seq = "\x1b[1;5C";
        break;
    case SOLAR_OS_KEY_CTRL_LEFT:
        seq = "\x1b[1;5D";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_UP:
        seq = "\x1b[1;6A";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
        seq = "\x1b[1;6B";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
        seq = "\x1b[1;6C";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
        seq = "\x1b[1;6D";
        break;
    case SOLAR_OS_KEY_CTRL_HOME:
        seq = "\x1b[1;5H";
        break;
    case SOLAR_OS_KEY_CTRL_END:
        seq = "\x1b[1;5F";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
        seq = "\x1b[1;6H";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_END:
        seq = "\x1b[1;6F";
        break;
    case SOLAR_OS_KEY_F1:
        seq = "\x1b[11~";
        break;
    case SOLAR_OS_KEY_F2:
        seq = "\x1b[12~";
        break;
    case SOLAR_OS_KEY_F3:
        seq = "\x1b[13~";
        break;
    case SOLAR_OS_KEY_F4:
        seq = "\x1b[14~";
        break;
    case SOLAR_OS_KEY_F5:
        seq = "\x1b[15~";
        break;
    case SOLAR_OS_KEY_F6:
        seq = "\x1b[17~";
        break;
    case SOLAR_OS_KEY_F7:
        seq = "\x1b[18~";
        break;
    case SOLAR_OS_KEY_F8:
        seq = "\x1b[19~";
        break;
    case SOLAR_OS_KEY_F9:
        seq = "\x1b[20~";
        break;
    case SOLAR_OS_KEY_F10:
        seq = "\x1b[21~";
        break;
    case SOLAR_OS_KEY_F11:
        seq = "\x1b[23~";
        break;
    case SOLAR_OS_KEY_F12:
        seq = "\x1b[24~";
        break;
    case '\b':
        data[0] = 0x7f;
        len = 1;
        break;
    case '\n':
    case '\r':
        data[0] = '\r';
        len = 1;
        break;
    default:
        if (key >= 0x80) {
            return;
        }
        data[0] = key;
        len = 1;
        break;
    }

    if (com_app.alt_prefix_pending) {
        com_app.alt_prefix_pending = false;
        if (com_write_bytes((const uint8_t *)"\x1b", 1) != ESP_OK) {
            solar_os_terminal_writeln(term, "\ncom: UART write failed");
            return;
        }
    }

    const esp_err_t err = seq != NULL ?
        com_write_bytes((const uint8_t *)seq, strlen(seq)) :
        com_write_bytes(data, len);
    if (err != ESP_OK) {
        solar_os_terminal_writeln(term, "\ncom: UART write failed");
    }
}

static void com_drain_rx(solar_os_context_t *ctx)
{
    if (!com_app.active) {
        return;
    }

    solar_os_terminal_t *term = com_terminal(ctx);
    uint8_t buffer[COM_RX_BUFFER_SIZE];

    for (size_t chunk = 0; chunk < COM_RX_CHUNKS_PER_TICK; chunk++) {
        size_t read_len = 0;
        const esp_err_t err =
            solar_os_port_read(&com_app.port, buffer, sizeof(buffer), 0, &read_len);
        if (err != ESP_OK) {
            solar_os_terminal_printf(term, "\ncom: UART read failed: %s\n", esp_err_to_name(err));
            SOLAR_OS_LOGW(TAG, "UART read failed: %s", esp_err_to_name(err));
            com_app.active = false;
            return;
        }
        if (read_len == 0) {
            break;
        }

        com_app.rx_bytes += read_len;
        for (size_t i = 0; i < read_len; i++) {
            solar_os_terminal_put_utf8_byte(term, buffer[i]);
        }
    }
}

static esp_err_t com_start(solar_os_context_t *ctx)
{
    memset(&com_app, 0, sizeof(com_app));
    com_app.port = (solar_os_port_handle_t)SOLAR_OS_PORT_HANDLE_INIT;

    if (solar_os_context_argc(ctx) != 1) {
        solar_os_terminal_t *term = com_terminal(ctx);
        solar_os_terminal_clear(term);
        solar_os_terminal_writeln_bold(term, "com");
        solar_os_terminal_writeln(term, "usage: com");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
        return ESP_OK;
    }

    const esp_err_t err = solar_os_uart_init();
    if (err != ESP_OK) {
        com_render_header(ctx);
        solar_os_terminal_printf(com_terminal(ctx), "com: UART unavailable: %s\n", esp_err_to_name(err));
        SOLAR_OS_LOGW(TAG, "UART init failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    const esp_err_t claim_err = solar_os_port_claim(SOLAR_OS_UART_PORT_NAME, "com", &com_app.port);
    com_render_header(ctx);
    if (claim_err != ESP_OK) {
        solar_os_port_info_t info;
        if (claim_err == ESP_ERR_INVALID_STATE &&
            solar_os_port_get_info(SOLAR_OS_UART_PORT_NAME, &info) == ESP_OK &&
            info.claimed) {
            solar_os_terminal_printf(com_terminal(ctx),
                                     "com: %s is busy: %s\n",
                                     SOLAR_OS_UART_PORT_NAME,
                                     info.owner);
        } else {
            solar_os_terminal_printf(com_terminal(ctx),
                                     "com: %s claim failed: %s\n",
                                     SOLAR_OS_UART_PORT_NAME,
                                     esp_err_to_name(claim_err));
        }
        return ESP_OK;
    }

    com_app.port_claimed = true;
    com_app.active = true;
    SOLAR_OS_LOGI(TAG, "COM app started");
    return ESP_OK;
}

static void com_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    SOLAR_OS_LOGI(TAG,
             "COM app stopped: tx=%" PRIu32 " rx=%" PRIu32,
             com_app.tx_bytes,
             com_app.rx_bytes);
    if (com_app.port_claimed) {
        (void)solar_os_port_release(&com_app.port);
    }
    memset(&com_app, 0, sizeof(com_app));
}

static void com_resume(solar_os_context_t *ctx)
{
    com_drain_rx(ctx);
}

static void com_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "com uart0", buffer_len);
}

static bool com_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        com_drain_rx(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (com_app.active) {
        com_send_key(ctx, ch);
        com_drain_rx(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_com_app = {
    .name = "com",
    .summary = "serial terminal",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = com_start,
    .resume = com_resume,
    .stop = com_stop,
    .event = com_event,
    .title = com_title,
};
