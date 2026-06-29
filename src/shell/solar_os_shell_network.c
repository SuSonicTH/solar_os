#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_config.h"
#include "solar_os_keys.h"
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_net.h"
#endif
#include "solar_os_port.h"
#include "solar_os_terminal.h"
#include "solar_os_time.h"
#include "solar_os_tui.h"
#include "solar_os_wifi.h"

#define WIFI_TUI_STATUS_MAX 96
#define WIFI_TUI_REFRESH_MS 1000
#define NETSCAN_MAX_PORTS 128
#define NETSCAN_MAX_HOSTS 256
#define NETSCAN_TIMEOUT_MS 350U

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static solar_os_terminal_t *display_terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_display_terminal(ctx);
}

static bool parse_size_arg(const char *text, size_t min, size_t max, size_t *value)
{
    return solar_os_shell_parse_size_arg(text, min, max, value);
}

static void wifi_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  wifi [status]");
    solar_os_shell_io_writeln(term, "  wifi on");
    solar_os_shell_io_writeln(term, "  wifi off");
    solar_os_shell_io_writeln(term, "  wifi scan");
    solar_os_shell_io_writeln(term, "  wifi connect [ssid [password]]");
    solar_os_shell_io_writeln(term, "  wifi disconnect");
    solar_os_shell_io_writeln(term, "  wifi known");
    solar_os_shell_io_writeln(term, "  wifi forget [ssid|all]");
    solar_os_shell_io_writeln(term, "  wifi nat [status|on|off]");
    solar_os_shell_io_writeln(term, "  wifi ap [status]");
    solar_os_shell_io_writeln(term, "  wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
    solar_os_shell_io_writeln(term, "  wifi ap off");
}

static void wifi_print_nat_status(solar_os_shell_io_t *term, const solar_os_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (!status->nat_enabled) {
        solar_os_shell_io_writeln(term, "NAT: off");
        return;
    }
    if (status->nat_active) {
        solar_os_shell_io_writeln(term, "NAT: active");
        return;
    }
    if (status->nat_last_error != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "NAT: error %s\n",
                                 esp_err_to_name(status->nat_last_error));
        return;
    }

    solar_os_shell_io_writeln(term, "NAT: waiting for APSTA link");
}

static void wifi_print_status(solar_os_shell_io_t *term)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    solar_os_shell_io_printf(term,
                             "WiFi: %s%s\n",
                             solar_os_wifi_state_name(status.state),
                             status.started ? "" : " (radio off)");
    if (status.ssid[0] != '\0') {
        solar_os_shell_io_printf(term, "SSID: %s\n", status.ssid);
    }
    if (status.has_ip) {
        solar_os_shell_io_printf(term, "IP: %s\n", status.ip);
        solar_os_shell_io_printf(term, "Gateway: %s\n", status.gateway);
        solar_os_shell_io_printf(term, "Netmask: %s\n", status.netmask);
    }
    if (status.connected) {
        solar_os_shell_io_printf(term,
                                 "Link: ch %u, RSSI %d dBm\n",
                                 (unsigned)status.channel,
                                 (int)status.rssi);
    }
    if (status.has_saved_config) {
        solar_os_shell_io_printf(term,
                                 "Saved: %u, preferred %s\n",
                                 (unsigned)status.saved_profile_count,
                                 status.saved_ssid);
    } else {
        solar_os_shell_io_writeln(term, "Saved: none");
    }
    if (status.has_saved_ap_config) {
        solar_os_shell_io_printf(term,
                                 "Saved AP: %s (%s)\n",
                                 status.saved_ap_ssid,
                                 status.saved_ap_auth[0] != '\0' ? status.saved_ap_auth : "open");
    } else {
        solar_os_shell_io_writeln(term, "Saved AP: none");
    }
    if (status.ap_enabled || status.ap_running) {
        solar_os_shell_io_printf(term, "AP: %s\n", status.ap_running ? "on" : "starting");
        if (status.ap_ssid[0] != '\0') {
            solar_os_shell_io_printf(term, "AP SSID: %s\n", status.ap_ssid);
        }
        if (status.ap_ip[0] != '\0') {
            solar_os_shell_io_printf(term, "AP IP: %s\n", status.ap_ip);
        }
        solar_os_shell_io_printf(term,
                                 "AP Link: ch %u, %s, clients %u/%u\n",
                                 (unsigned)status.ap_channel,
                                 status.ap_auth[0] != '\0' ? status.ap_auth : "open",
                                 (unsigned)status.ap_station_count,
                                 (unsigned)status.ap_max_connections);
    } else {
        solar_os_shell_io_writeln(term, "AP: off");
    }
    wifi_print_nat_status(term, &status);
    if (status.disconnect_reason != 0) {
        solar_os_shell_io_printf(term,
                                 "Last disconnect reason: %u\n",
                                 (unsigned)status.disconnect_reason);
    }
}

static void wifi_cmd_scan(solar_os_shell_io_t *term)
{
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t found = 0;
    const esp_err_t err = solar_os_wifi_scan(aps, sizeof(aps) / sizeof(aps[0]), &found);

    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi scan failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_writeln(term, "RSSI CH AUTH       K SSID");
    for (size_t i = 0; i < found; i++) {
        const bool known = !aps[i].hidden && solar_os_wifi_is_known_ssid(aps[i].ssid);
        solar_os_shell_io_printf(term,
                                 "%4d %2u %-10s %c %s\n",
                                 (int)aps[i].rssi,
                                 (unsigned)aps[i].channel,
                                 aps[i].auth,
                                 known ? '*' : '-',
                                 aps[i].ssid);
    }
    solar_os_shell_io_printf(term,
                             "%u network%s shown\n",
                             (unsigned)found,
                             found == 1 ? "" : "s");
}

static void wifi_cmd_known(solar_os_shell_io_t *term)
{
    solar_os_wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;
    const esp_err_t err = solar_os_wifi_known(profiles,
                                              sizeof(profiles) / sizeof(profiles[0]),
                                              &count);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi known failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (count == 0) {
        solar_os_shell_io_writeln(term, "no known networks");
        return;
    }

    solar_os_shell_io_writeln(term, "P SSID");
    const size_t shown = count < SOLAR_OS_WIFI_PROFILE_MAX ? count : SOLAR_OS_WIFI_PROFILE_MAX;
    for (size_t i = 0; i < shown; i++) {
        solar_os_shell_io_printf(term,
                                 "%c %s\n",
                                 profiles[i].preferred ? '*' : '-',
                                 profiles[i].ssid);
    }
    if (count > shown) {
        solar_os_shell_io_printf(term, "%u more not shown\n", (unsigned)(count - shown));
    }
}

static void wifi_cmd_ap(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap [status]");
            return;
        }
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[2], "on") == 0) {
        if (argc > 6) {
            solar_os_shell_io_writeln(
                term,
                "usage: wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]");
            return;
        }

        const char *ssid = argc >= 4 ? argv[3] : NULL;
        const char *password = argc >= 5 ? argv[4] : NULL;
        const char *auth = argc >= 6 ? argv[5] : NULL;
        const esp_err_t err = solar_os_wifi_ap_start(ssid, password, auth);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi AP on: %s (%s)\n",
                                     status.ap_ssid,
                                     status.ap_auth[0] != '\0' ? status.ap_auth : "open");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi ap: WEP is not supported in SoftAP mode");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi ap: invalid SSID, password, or auth mode");
        } else {
            solar_os_shell_io_printf(term, "wifi ap on failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi ap off");
            return;
        }

        const esp_err_t err = solar_os_wifi_ap_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi AP off");
        } else {
            solar_os_shell_io_printf(term, "wifi ap off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

static void wifi_cmd_nat(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2 || strcmp(argv[2], "status") == 0) {
        if (argc > 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }
        solar_os_wifi_status_t status;
        solar_os_wifi_get_status(&status);
        wifi_print_nat_status(term, &status);
        return;
    }

    if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
        if (argc != 3) {
            solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
            return;
        }

        const bool enabled = strcmp(argv[2], "on") == 0;
        const esp_err_t err = solar_os_wifi_nat_set(enabled);
        if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            wifi_print_nat_status(term, &status);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(term, "wifi nat: NAT is not supported in this build");
        } else {
            solar_os_shell_io_printf(term, "wifi nat failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    solar_os_shell_io_writeln(term, "usage: wifi nat [status|on|off]");
}

static void wifi_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    esp_err_t err;

    if (argc == 2) {
        err = solar_os_wifi_connect_saved();
        if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: no saved network");
        } else if (err == ESP_OK) {
            solar_os_wifi_status_t status;
            solar_os_wifi_get_status(&status);
            solar_os_shell_io_printf(term,
                                     "WiFi connecting to %s\n",
                                     status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        } else {
            solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: wifi connect [ssid [password]]");
        return;
    }

    const char *ssid = argv[2];
    const char *password = argc == 4 ? argv[3] : "";
    err = solar_os_wifi_connect(ssid, password);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "WiFi connecting to %s\n", ssid);
    } else if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "wifi: invalid SSID or password length");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

static void wifi_cmd_on(solar_os_shell_io_t *term)
{
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "wifi on failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    if (!status.has_saved_config) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        solar_os_shell_io_printf(term,
                                 "WiFi radio on, connecting to %s\n",
                                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_writeln(term, "WiFi radio on");
    } else {
        solar_os_shell_io_printf(term, "wifi connect failed: %s\n", esp_err_to_name(err));
    }
}

typedef enum {
    WIFI_TUI_RADIO,
    WIFI_TUI_STATION,
    WIFI_TUI_DISCONNECT,
    WIFI_TUI_AP,
    WIFI_TUI_NAT,
    WIFI_TUI_SCAN,
    WIFI_TUI_SAVED_STA,
    WIFI_TUI_SAVED_AP,
    WIFI_TUI_ITEM_COUNT,
} wifi_tui_item_t;

typedef struct {
    const char *label;
} wifi_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    char status[WIFI_TUI_STATUS_MAX];
    solar_os_wifi_ap_t scan_aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t scan_count;
    bool scan_valid;
    uint32_t last_refresh_ms;
} wifi_tui_state_t;

static wifi_tui_state_t wifi_tui;

static const wifi_tui_item_def_t wifi_tui_items[] = {
    [WIFI_TUI_RADIO] = {.label = "radio"},
    [WIFI_TUI_STATION] = {.label = "station"},
    [WIFI_TUI_DISCONNECT] = {.label = "disconnect"},
    [WIFI_TUI_AP] = {.label = "ap"},
    [WIFI_TUI_NAT] = {.label = "nat"},
    [WIFI_TUI_SCAN] = {.label = "scan"},
    [WIFI_TUI_SAVED_STA] = {.label = "saved sta"},
    [WIFI_TUI_SAVED_AP] = {.label = "saved ap"},
};

static size_t wifi_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void wifi_tui_set_status(const char *status)
{
    strlcpy(wifi_tui.status, status != NULL ? status : "", sizeof(wifi_tui.status));
}

static void wifi_tui_write_cell(size_t row,
                                size_t col,
                                size_t width,
                                const char *text,
                                uint8_t attr)
{
    char clipped[WIFI_TUI_STATUS_MAX];
    size_t len = 0;

    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&wifi_tui.tui, row, col, 1, width, ' ', attr);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (text[len] != '\0' && len + 1 < sizeof(clipped) && len < width) {
        clipped[len] = text[len];
        len++;
    }
    clipped[len] = '\0';
    solar_os_tui_addstr(&wifi_tui.tui, row, col, clipped, attr);
}

static void wifi_tui_nat_value(const solar_os_wifi_status_t *status,
                               char *buffer,
                               size_t buffer_len)
{
    if (!status->nat_enabled) {
        strlcpy(buffer, "off", buffer_len);
    } else if (status->nat_active) {
        strlcpy(buffer, "active", buffer_len);
    } else if (status->nat_last_error != ESP_OK) {
        snprintf(buffer, buffer_len, "error %s", esp_err_to_name(status->nat_last_error));
    } else {
        strlcpy(buffer, "waiting", buffer_len);
    }
}

static void wifi_tui_current_value(wifi_tui_item_t item,
                                   const solar_os_wifi_status_t *status,
                                   char *buffer,
                                   size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0 || status == NULL) {
        return;
    }

    switch (item) {
    case WIFI_TUI_RADIO:
        strlcpy(buffer, status->started ? "on" : "off", buffer_len);
        break;
    case WIFI_TUI_STATION:
        if (status->connected && status->has_ip) {
            snprintf(buffer,
                     buffer_len,
                     "%s %s",
                     status->ssid[0] != '\0' ? status->ssid : "connected",
                     status->ip);
        } else if (status->state == SOLAR_OS_WIFI_STATE_CONNECTING) {
            snprintf(buffer,
                     buffer_len,
                     "connecting %s",
                     status->ssid[0] != '\0' ? status->ssid : status->saved_ssid);
        } else {
            strlcpy(buffer, solar_os_wifi_state_name(status->state), buffer_len);
        }
        break;
    case WIFI_TUI_DISCONNECT:
        strlcpy(buffer, status->connected ? "ready" : "-", buffer_len);
        break;
    case WIFI_TUI_AP:
        if (status->ap_running) {
            snprintf(buffer,
                     buffer_len,
                     "on %s",
                     status->ap_ssid[0] != '\0' ? status->ap_ssid : status->ap_ip);
        } else if (status->ap_enabled) {
            strlcpy(buffer, "starting", buffer_len);
        } else if (status->has_saved_ap_config) {
            snprintf(buffer, buffer_len, "off saved %s", status->saved_ap_ssid);
        } else {
            strlcpy(buffer, "off", buffer_len);
        }
        break;
    case WIFI_TUI_NAT:
        wifi_tui_nat_value(status, buffer, buffer_len);
        break;
    case WIFI_TUI_SCAN:
        if (wifi_tui.scan_valid) {
            snprintf(buffer, buffer_len, "%u shown", (unsigned)wifi_tui.scan_count);
        } else {
            strlcpy(buffer, "enter", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_STA:
        if (status->has_saved_config) {
            snprintf(buffer,
                     buffer_len,
                     "%u %s",
                     (unsigned)status->saved_profile_count,
                     status->saved_ssid);
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    case WIFI_TUI_SAVED_AP:
        if (status->has_saved_ap_config) {
            snprintf(buffer,
                     buffer_len,
                     "%s (%s)",
                     status->saved_ap_ssid,
                     status->saved_ap_auth[0] != '\0' ? status->saved_ap_auth : "open");
        } else {
            strlcpy(buffer, "none", buffer_len);
        }
        break;
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static void wifi_tui_render_scan(size_t start_row, size_t rows, size_t cols)
{
    if (start_row >= rows || cols == 0 || !wifi_tui.scan_valid) {
        return;
    }

    wifi_tui_write_cell(start_row,
                        0,
                        cols,
                        wifi_tui.scan_count == 0 ? "scan: no networks" : "scan: rssi ch auth k ssid",
                        SOLAR_OS_TUI_ATTR_BOLD);

    for (size_t i = 0; i < wifi_tui.scan_count && start_row + i + 1 < rows; i++) {
        char line[WIFI_TUI_STATUS_MAX];
        const bool known = !wifi_tui.scan_aps[i].hidden &&
            solar_os_wifi_is_known_ssid(wifi_tui.scan_aps[i].ssid);
        snprintf(line,
                 sizeof(line),
                 "%4d %2u %-10s %c %s",
                 (int)wifi_tui.scan_aps[i].rssi,
                 (unsigned)wifi_tui.scan_aps[i].channel,
                 wifi_tui.scan_aps[i].auth,
                 known ? '*' : '-',
                 wifi_tui.scan_aps[i].ssid);
        wifi_tui_write_cell(start_row + i + 1, 0, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void wifi_tui_render(void)
{
    solar_os_tui_t *tui = &wifi_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    solar_os_wifi_status_t status;

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_wifi_get_status(&status);
    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    wifi_tui_write_cell(0,
                        0,
                        split,
                        "wifi",
                        SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        wifi_tui_write_cell(0,
                            split + 1,
                            wifi_tui_visible_width(cols, split + 1),
                            "value",
                            SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t value_col = split + 1;
    const size_t value_width = wifi_tui_visible_width(cols, value_col);
    for (size_t i = 0; i < WIFI_TUI_ITEM_COUNT && i + 1 < rows; i++) {
        char value[WIFI_TUI_STATUS_MAX];
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;

        if (i == wifi_tui.selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }

        wifi_tui_current_value((wifi_tui_item_t)i, &status, value, sizeof(value));
        wifi_tui_write_cell(i + 1, 0, split, wifi_tui_items[i].label, label_attr);
        if (value_width > 0) {
            wifi_tui_write_cell(i + 1, value_col, value_width, value, value_attr);
        }
    }

    const size_t scan_row = WIFI_TUI_ITEM_COUNT + 2;
    const size_t status_row = rows > 1 ? rows - 1 : 0;
    if (scan_row < status_row) {
        wifi_tui_render_scan(scan_row, status_row, cols);
    }

    if (wifi_tui.status[0] != '\0' && rows > 1) {
        wifi_tui_write_cell(status_row, 0, cols, wifi_tui.status, SOLAR_OS_TUI_ATTR_INVERSE);
    }

    solar_os_terminal_set_cursor_visible(tui->terminal, false);
    solar_os_tui_refresh(tui);
}

static void wifi_tui_start_radio(void)
{
    solar_os_wifi_status_t status;
    esp_err_t err = solar_os_wifi_start();
    if (err != ESP_OK) {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "wifi on failed: %s", esp_err_to_name(err));
        wifi_tui_set_status(message);
        return;
    }

    solar_os_wifi_get_status(&status);
    if (status.connected || status.state == SOLAR_OS_WIFI_STATE_CONNECTING ||
        !status.has_saved_config) {
        wifi_tui_set_status("radio on");
        return;
    }

    err = solar_os_wifi_connect_saved();
    if (err == ESP_OK) {
        solar_os_wifi_get_status(&status);
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message,
                 sizeof(message),
                 "connecting %s",
                 status.ssid[0] != '\0' ? status.ssid : status.saved_ssid);
        wifi_tui_set_status(message);
    } else if (err == ESP_ERR_NOT_FOUND) {
        wifi_tui_set_status("radio on");
    } else {
        char message[WIFI_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
        wifi_tui_set_status(message);
    }
}

static void wifi_tui_apply_selected(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    switch ((wifi_tui_item_t)wifi_tui.selected) {
    case WIFI_TUI_RADIO:
        if (status.started) {
            const esp_err_t err = solar_os_wifi_stop();
            wifi_tui_set_status(err == ESP_OK ? "radio off" : esp_err_to_name(err));
        } else {
            wifi_tui_start_radio();
        }
        break;
    case WIFI_TUI_STATION: {
        const esp_err_t err = solar_os_wifi_connect_saved();
        if (err == ESP_OK) {
            solar_os_wifi_get_status(&status);
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message,
                     sizeof(message),
                     "connecting %s",
                     status.saved_ssid[0] != '\0' ? status.saved_ssid : status.ssid);
            wifi_tui_set_status(message);
        } else if (err == ESP_ERR_NOT_FOUND) {
            wifi_tui_set_status("no saved station");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "connect failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_DISCONNECT: {
        const esp_err_t err = solar_os_wifi_disconnect();
        wifi_tui_set_status(err == ESP_OK ? "station disconnected" : esp_err_to_name(err));
        break;
    }
    case WIFI_TUI_AP: {
        const esp_err_t err =
            (status.ap_running || status.ap_enabled) ?
            solar_os_wifi_ap_stop() :
            solar_os_wifi_ap_start(NULL, NULL, NULL);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.ap_running || status.ap_enabled ? "ap off" : "ap on");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "ap failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_NAT: {
        const esp_err_t err = solar_os_wifi_nat_set(!status.nat_enabled);
        if (err == ESP_OK) {
            wifi_tui_set_status(status.nat_enabled ? "nat off" : "nat on");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            wifi_tui_set_status("nat unsupported");
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "nat failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SCAN: {
        size_t found = 0;
        wifi_tui_set_status("scanning...");
        wifi_tui.scan_valid = false;
        wifi_tui_render();
        const esp_err_t err = solar_os_wifi_scan(wifi_tui.scan_aps,
                                                 sizeof(wifi_tui.scan_aps) / sizeof(wifi_tui.scan_aps[0]),
                                                 &found);
        if (err == ESP_OK) {
            wifi_tui.scan_count = found;
            wifi_tui.scan_valid = true;
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "%u network%s", (unsigned)found, found == 1 ? "" : "s");
            wifi_tui_set_status(message);
        } else {
            char message[WIFI_TUI_STATUS_MAX];
            snprintf(message, sizeof(message), "scan failed: %s", esp_err_to_name(err));
            wifi_tui_set_status(message);
        }
        break;
    }
    case WIFI_TUI_SAVED_STA:
    case WIFI_TUI_SAVED_AP:
        wifi_tui_set_status("read only");
        break;
    default:
        break;
    }

    wifi_tui_render();
}

static esp_err_t wifi_tui_start(solar_os_context_t *ctx)
{
    memset(&wifi_tui, 0, sizeof(wifi_tui));
    wifi_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_begin(&wifi_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    wifi_tui_set_status("enter acts, esc exits");
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), false);
    wifi_tui_render();
    return ESP_OK;
}

static void wifi_tui_stop(solar_os_context_t *ctx)
{
    solar_os_terminal_set_cursor_visible(display_terminal(ctx), true);
    solar_os_terminal_clear(display_terminal(ctx));
    solar_os_context_request_terminal_preserve(ctx);
}

static bool wifi_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (wifi_tui.last_refresh_ms == 0) {
            wifi_tui.last_refresh_ms = now_ms;
            return true;
        }
        if ((now_ms - wifi_tui.last_refresh_ms) >= WIFI_TUI_REFRESH_MS) {
            wifi_tui.last_refresh_ms = now_ms;
            wifi_tui_render();
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(wifi_tui.ctx);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (wifi_tui.selected > 0) {
            wifi_tui.selected--;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (wifi_tui.selected + 1 < WIFI_TUI_ITEM_COUNT) {
            wifi_tui.selected++;
            wifi_tui_set_status("");
            wifi_tui_render();
        }
        break;
    case '\r':
    case '\n':
        wifi_tui_apply_selected();
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t wifi_tui_app = {
    .name = "wifi",
    .summary = "Wi-Fi control",
    .start = wifi_tui_start,
    .stop = wifi_tui_stop,
    .event = wifi_tui_event,
};

void solar_os_shell_cmd_wifi(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        if (solar_os_shell_io_kind(term) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            solar_os_shell_io_writeln(term, "wifi: TUI is only available on the display shell");
            solar_os_shell_io_writeln(term, "usage: wifi status|on|off|scan|connect|disconnect|known|forget|ap|nat");
            return;
        }
        const esp_err_t err = solar_os_context_request_launch(ctx, &wifi_tui_app, 0, NULL);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "wifi: launch failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        wifi_print_status(term);
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        wifi_cmd_on(term);
        return;
    }

    if (strcmp(argv[1], "off") == 0) {
        const esp_err_t err = solar_os_wifi_stop();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi radio off");
        } else {
            solar_os_shell_io_printf(term, "wifi off failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "ap") == 0) {
        wifi_cmd_ap(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "nat") == 0) {
        wifi_cmd_nat(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi scan");
            return;
        }
        wifi_cmd_scan(term);
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        wifi_cmd_connect(term, argc, argv);
        return;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        const esp_err_t err = solar_os_wifi_disconnect();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "WiFi disconnected");
        } else {
            solar_os_shell_io_printf(term, "wifi disconnect failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    if (strcmp(argv[1], "known") == 0) {
        if (argc != 2) {
            solar_os_shell_io_writeln(term, "usage: wifi known");
            return;
        }
        wifi_cmd_known(term);
        return;
    }

    if (strcmp(argv[1], "forget") == 0) {
        esp_err_t err;
        bool forgetting_all = false;
        if (argc == 2) {
            err = solar_os_wifi_forget();
        } else if (argc == 3 && strcmp(argv[2], "all") == 0) {
            forgetting_all = true;
            err = solar_os_wifi_forget_all();
        } else if (argc == 3) {
            err = solar_os_wifi_forget_ssid(argv[2]);
        } else {
            solar_os_shell_io_writeln(term, "usage: wifi forget [ssid|all]");
            return;
        }

        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term,
                                      forgetting_all ? "WiFi profiles forgotten" : "WiFi profile forgotten");
        } else if (err == ESP_ERR_NOT_FOUND) {
            solar_os_shell_io_writeln(term, "wifi: profile not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            solar_os_shell_io_writeln(term, "wifi: invalid SSID");
        } else {
            solar_os_shell_io_printf(term, "wifi forget failed: %s\n", esp_err_to_name(err));
        }
        return;
    }

    wifi_print_usage(term);
}

#if SOLAR_OS_PACKAGE_NET
static void ping_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: ping <host> [count]");
    solar_os_shell_io_printf(term,
                             "%s stops a running ping\n",
                             solar_os_shell_io_app_exit_key(term));
}

static bool shell_read_app_exit_key(void *user)
{
    solar_os_shell_io_t *term = (solar_os_shell_io_t *)user;
    char chars[8];
    size_t count;

    while ((count = solar_os_ble_keyboard_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            const uint8_t ch = (uint8_t)chars[i];
            if (ch == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    }

    if (term == NULL ||
        solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&term->port)) {
        return false;
    }

    uint8_t port_chars[8];
    do {
        count = 0;
        const esp_err_t err = solar_os_port_read(&term->port,
                                                 port_chars,
                                                 sizeof(port_chars),
                                                 0,
                                                 &count);
        if (err != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (port_chars[i] == 0x1d ||
                port_chars[i] == SOLAR_OS_KEY_APP_EXIT) {
                return true;
            }
        }
    } while (count > 0);

    return false;
}

static void ping_print_event(const solar_os_net_ping_event_t *event, void *user)
{
    solar_os_shell_io_t *term = (solar_os_shell_io_t *)user;

    if (event == NULL || term == NULL) {
        return;
    }

    if (event->type == SOLAR_OS_NET_PING_REPLY) {
        solar_os_shell_io_printf(term,
                                 "%" PRIu32 "B from %s seq=%u ttl=%u time=%" PRIu32 "ms\n",
                                 event->bytes,
                                 event->from,
                                 (unsigned)event->seqno,
                                 (unsigned)event->ttl,
                                 event->elapsed_ms);
    } else {
        solar_os_shell_io_printf(term,
                                 "timeout from %s seq=%u\n",
                                 event->from,
                                 (unsigned)event->seqno);
    }
    solar_os_shell_io_flush(term);
}

void solar_os_shell_cmd_ping(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    size_t count = SOLAR_OS_NET_PING_FOREVER;

    if (argc < 2 || argc > 3) {
        ping_print_usage(term);
        return;
    }

    if (argc == 3 &&
        !parse_size_arg(argv[2], 1, SOLAR_OS_NET_PING_MAX_COUNT, &count)) {
        solar_os_shell_io_printf(term,
                                 "ping count: 1..%u\n",
                                 (unsigned)SOLAR_OS_NET_PING_MAX_COUNT);
        return;
    }

    const char *host = argv[1];
    solar_os_net_ping_options_t options = {
        .count = (uint32_t)count,
    };
    solar_os_net_ping_result_t result;

    if (count == SOLAR_OS_NET_PING_FOREVER) {
        solar_os_shell_io_printf(term,
                                 "ping %s, %s to stop\n",
                                 host,
                                 solar_os_shell_io_app_exit_key(term));
    } else {
        solar_os_shell_io_printf(term, "ping %s (%u packets)\n", host, (unsigned)count);
    }
    solar_os_shell_io_flush(term);

    const esp_err_t err = solar_os_net_ping(host,
                                            &options,
                                            ping_print_event,
                                            term,
                                            shell_read_app_exit_key,
                                            term,
                                            &result);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "ping: WiFi not connected");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "ping: unknown host: %s\n", host);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        ping_print_usage(term);
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ping failed: %s\n", esp_err_to_name(err));
        return;
    }

    if (result.interrupted) {
        solar_os_shell_io_writeln(term, "ping: stopped");
    }
    solar_os_shell_io_printf(term,
                             "%" PRIu32 " tx, %" PRIu32 " rx, %" PRIu32 "%% loss, %" PRIu32 "ms\n",
                             result.transmitted,
                             result.received,
                             result.loss_percent,
                             result.total_time_ms);
    if (result.received > 0) {
        solar_os_shell_io_printf(term,
                                 "rtt min/avg/max %" PRIu32 "/%" PRIu32 "/%" PRIu32 " ms\n",
                                 result.min_time_ms,
                                 result.avg_time_ms,
                                 result.max_time_ms);
    }
}

static void netscan_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage: netscan <host|range> [ports]");
    solar_os_shell_io_writeln(term, "  target: host, 192.168.1.20, 192.168.1.1-32, 192.168.1.0/24");
    solar_os_shell_io_writeln(term, "  ports: 22,80,443 or 1-128");
    solar_os_shell_io_writeln(term, "  default: 22,80,443,1883,8080");
    solar_os_shell_io_printf(term, "%s stops a running scan\n", solar_os_shell_io_app_exit_key(term));
}

typedef struct {
    bool range;
    char label[SOLAR_OS_NET_HOST_MAX];
    char single_ip[SOLAR_OS_NET_ADDR_MAX];
    uint8_t prefix[3];
    uint8_t first;
    uint8_t last;
    size_t count;
} netscan_target_spec_t;

static bool netscan_parse_ipv4_octet(const char **cursor, uint8_t *octet)
{
    if (cursor == NULL || *cursor == NULL || octet == NULL || !isdigit((unsigned char)**cursor)) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(*cursor, &end, 10);
    if (errno != 0 || end == *cursor || parsed > 255UL) {
        return false;
    }

    *octet = (uint8_t)parsed;
    *cursor = end;
    return true;
}

static bool netscan_parse_ipv4_address(const char *text, uint8_t octets[4], const char **end)
{
    if (text == NULL || octets == NULL) {
        return false;
    }

    const char *cursor = text;
    for (size_t i = 0; i < 4; i++) {
        if (!netscan_parse_ipv4_octet(&cursor, &octets[i])) {
            return false;
        }
        if (i < 3) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
    }

    if (end != NULL) {
        *end = cursor;
    }
    return true;
}

static void netscan_format_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    snprintf(buffer,
             len,
             "%u.%u.%u.%u",
             (unsigned)a,
             (unsigned)b,
             (unsigned)c,
             (unsigned)d);
}

static bool netscan_target_set_range(netscan_target_spec_t *target,
                                     const uint8_t octets[4],
                                     uint8_t first,
                                     uint8_t last)
{
    if (target == NULL || octets == NULL || last < first) {
        return false;
    }

    const size_t count = (size_t)last - (size_t)first + 1U;
    if (count == 0 || count > NETSCAN_MAX_HOSTS) {
        return false;
    }

    target->range = true;
    target->prefix[0] = octets[0];
    target->prefix[1] = octets[1];
    target->prefix[2] = octets[2];
    target->first = first;
    target->last = last;
    target->count = count;
    snprintf(target->label,
             sizeof(target->label),
             "%u.%u.%u.%u-%u",
             (unsigned)target->prefix[0],
             (unsigned)target->prefix[1],
             (unsigned)target->prefix[2],
             (unsigned)first,
             (unsigned)last);
    return true;
}

static esp_err_t netscan_parse_target(const char *text, netscan_target_spec_t *target)
{
    if (text == NULL || text[0] == '\0' || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(target, 0, sizeof(*target));
    strlcpy(target->label, text, sizeof(target->label));

    uint8_t octets[4] = {0};
    const char *end = NULL;
    if (netscan_parse_ipv4_address(text, octets, &end)) {
        if (*end == '\0') {
            target->range = false;
            target->count = 1;
            netscan_format_ipv4(octets[0],
                                octets[1],
                                octets[2],
                                octets[3],
                                target->single_ip,
                                sizeof(target->single_ip));
            return ESP_OK;
        }

        if (*end == '/') {
            char *mask_end = NULL;
            errno = 0;
            unsigned long mask = strtoul(end + 1, &mask_end, 10);
            if (errno != 0 || mask_end == end + 1 || *mask_end != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            if (mask != 24UL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return netscan_target_set_range(target, octets, 1, 254) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }

        if (*end == '-') {
            const char *range_end = NULL;
            uint8_t last_octets[4] = {0};
            if (netscan_parse_ipv4_address(end + 1, last_octets, &range_end)) {
                if (*range_end != '\0' ||
                    last_octets[0] != octets[0] ||
                    last_octets[1] != octets[1] ||
                    last_octets[2] != octets[2]) {
                    return ESP_ERR_INVALID_ARG;
                }
                return netscan_target_set_range(target, octets, octets[3], last_octets[3])
                           ? ESP_OK
                           : ESP_ERR_INVALID_SIZE;
            }

            const char *last_cursor = end + 1;
            uint8_t last = 0;
            if (!netscan_parse_ipv4_octet(&last_cursor, &last) || *last_cursor != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            return netscan_target_set_range(target, octets, octets[3], last)
                       ? ESP_OK
                       : ESP_ERR_INVALID_SIZE;
        }

        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t resolve_err = solar_os_net_resolve_host(text,
                                                            target->single_ip,
                                                            sizeof(target->single_ip));
    if (resolve_err != ESP_OK) {
        return resolve_err;
    }
    target->range = false;
    target->count = 1;
    return ESP_OK;
}

static void netscan_target_ip(const netscan_target_spec_t *target,
                              size_t index,
                              char *ip,
                              size_t ip_len)
{
    if (target == NULL || ip == NULL || ip_len == 0) {
        return;
    }

    if (!target->range) {
        strlcpy(ip, target->single_ip, ip_len);
        return;
    }

    const uint8_t host = (uint8_t)((size_t)target->first + index);
    netscan_format_ipv4(target->prefix[0],
                        target->prefix[1],
                        target->prefix[2],
                        host,
                        ip,
                        ip_len);
}

static bool netscan_parse_port_value(const char *text, char **end, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }

    errno = 0;
    unsigned long parsed = strtoul(text, end, 10);
    if (errno != 0 || *end == text || parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }

    *port = (uint16_t)parsed;
    return true;
}

static bool netscan_add_port(uint16_t *ports, size_t *count, uint16_t port)
{
    if (ports == NULL || count == NULL || port == 0) {
        return false;
    }

    for (size_t i = 0; i < *count; i++) {
        if (ports[i] == port) {
            return true;
        }
    }
    if (*count >= NETSCAN_MAX_PORTS) {
        return false;
    }

    ports[(*count)++] = port;
    return true;
}

static bool netscan_parse_ports(const char *text, uint16_t *ports, size_t *count)
{
    if (ports == NULL || count == NULL) {
        return false;
    }

    *count = 0;
    if (text == NULL || text[0] == '\0') {
        static const uint16_t defaults[] = {22, 80, 443, 1883, 8080};
        for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
            if (!netscan_add_port(ports, count, defaults[i])) {
                return false;
            }
        }
        return true;
    }

    char buffer[192];
    if (strlcpy(buffer, text, sizeof(buffer)) >= sizeof(buffer)) {
        return false;
    }

    char *saveptr = NULL;
    char *token = strtok_r(buffer, ",", &saveptr);
    while (token != NULL) {
        uint16_t first = 0;
        uint16_t last = 0;
        char *end = NULL;
        if (!netscan_parse_port_value(token, &end, &first)) {
            return false;
        }
        if (*end == '-') {
            if (!netscan_parse_port_value(end + 1, &end, &last) || last < first) {
                return false;
            }
        } else {
            last = first;
        }
        if (*end != '\0') {
            return false;
        }

        for (uint32_t port = first; port <= last; port++) {
            if (!netscan_add_port(ports, count, (uint16_t)port)) {
                return false;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    return *count > 0;
}

static const char *netscan_service_name(uint16_t port)
{
    switch (port) {
    case 21:
        return "ftp";
    case 22:
        return "ssh";
    case 23:
        return "telnet";
    case 25:
        return "smtp";
    case 53:
        return "dns";
    case 80:
        return "http";
    case 110:
        return "pop3";
    case 143:
        return "imap";
    case 443:
        return "https";
    case 587:
        return "submission";
    case 993:
        return "imaps";
    case 995:
        return "pop3s";
    case 1883:
        return "mqtt";
    case 3306:
        return "mysql";
    case 5432:
        return "postgres";
    case 8080:
        return "http-alt";
    default:
        return "";
    }
}

static bool netscan_probe_tcp(const char *ip, uint16_t port, uint32_t timeout_ms, uint32_t *elapsed_ms)
{
    if (elapsed_ms != NULL) {
        *elapsed_ms = 0;
    }

    const TickType_t start = xTaskGetTickCount();
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {
            .s_addr = inet_addr(ip),
        },
    };

    bool open = false;
    int rc = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        open = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        struct timeval timeout = {
            .tv_sec = (time_t)(timeout_ms / 1000U),
            .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
        };

        rc = select(sock + 1, NULL, &writefds, NULL, &timeout);
        if (rc > 0 && FD_ISSET(sock, &writefds)) {
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);
            if (getsockopt(sock,
                           SOL_SOCKET,
                           SO_ERROR,
                           &so_error,
                           &so_error_len) == 0 &&
                so_error == 0) {
                open = true;
            }
        }
    }

    close(sock);
    if (elapsed_ms != NULL) {
        *elapsed_ms = (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
    }
    return open;
}

static void netscan_update_progress(solar_os_shell_io_t *term,
                                    size_t row,
                                    const char *ip,
                                    uint16_t port,
                                    size_t probe_index,
                                    size_t total_probes)
{
    static const char frames[] = "|/-\\";
    const char frame = frames[probe_index % (sizeof(frames) - 1U)];

    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
    solar_os_shell_io_printf(term,
                             "%c %s:%u %u/%u",
                             frame,
                             ip,
                             (unsigned)port,
                             (unsigned)(probe_index + 1U),
                             (unsigned)total_probes);
    solar_os_shell_io_flush(term);
}

static void netscan_clear_progress(solar_os_shell_io_t *term, size_t row)
{
    solar_os_shell_io_set_cursor(term, row, 0);
    solar_os_shell_io_clear_line_from(term, row, 0);
}

void solar_os_shell_cmd_netscan(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2 || argc > 3) {
        netscan_print_usage(term);
        return;
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip) {
        solar_os_shell_io_writeln(term, "netscan: WiFi not connected");
        return;
    }

    uint16_t ports[NETSCAN_MAX_PORTS];
    size_t port_count = 0;
    if (!netscan_parse_ports(argc == 3 ? argv[2] : NULL, ports, &port_count)) {
        solar_os_shell_io_printf(term,
                                 "netscan: invalid ports or too many ports, max %u\n",
                                 (unsigned)NETSCAN_MAX_PORTS);
        return;
    }

    netscan_target_spec_t target;
    const esp_err_t target_err = netscan_parse_target(argv[1], &target);
    if (target_err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "netscan: unknown host: %s\n", argv[1]);
        return;
    }
    if (target_err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "netscan: only IPv4 /24 ranges are supported");
        return;
    }
    if (target_err == ESP_ERR_INVALID_SIZE) {
        solar_os_shell_io_printf(term,
                                 "netscan: too many hosts, max %u\n",
                                 (unsigned)NETSCAN_MAX_HOSTS);
        return;
    }
    if (target_err != ESP_OK) {
        solar_os_shell_io_printf(term, "netscan: invalid target: %s\n", esp_err_to_name(target_err));
        return;
    }

    char first_ip[SOLAR_OS_NET_ADDR_MAX];
    netscan_target_ip(&target, 0, first_ip, sizeof(first_ip));
    solar_os_shell_io_printf(term,
                             "netscan %s (%s), %u host%s, %u ports, %s to stop\n",
                             target.label,
                             first_ip,
                             (unsigned)target.count,
                             target.count == 1 ? "" : "s",
                             (unsigned)port_count,
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_writeln(term, "HOST             PORT     STATE  SERVICE");
    solar_os_shell_io_flush(term);

    size_t open_count = 0;
    size_t probe_count = 0;
    size_t progress_row = solar_os_shell_io_cursor_row(term);
    const size_t total_probes = target.count * port_count;
    const bool cursor_was_visible = solar_os_shell_io_cursor_visible(term);
    solar_os_shell_io_set_cursor_visible(term, false);
    bool stopped = false;
    for (size_t host_index = 0; host_index < target.count; host_index++) {
        char ip[SOLAR_OS_NET_ADDR_MAX];
        netscan_target_ip(&target, host_index, ip, sizeof(ip));

        for (size_t port_index = 0; port_index < port_count; port_index++) {
            if (shell_read_app_exit_key(term)) {
                stopped = true;
                break;
            }

            uint32_t elapsed_ms = 0;
            const uint16_t port = ports[port_index];
            netscan_update_progress(term, progress_row, ip, port, probe_count, total_probes);
            probe_count++;
            if (netscan_probe_tcp(ip, port, NETSCAN_TIMEOUT_MS, &elapsed_ms)) {
                netscan_clear_progress(term, progress_row);
                solar_os_shell_io_printf(term,
                                         "%-15s %-8u open   %s",
                                         ip,
                                         (unsigned)port,
                                         netscan_service_name(port));
                if (elapsed_ms > 0) {
                    solar_os_shell_io_printf(term, " (%" PRIu32 "ms)", elapsed_ms);
                }
                solar_os_shell_io_put_char(term, '\n');
                solar_os_shell_io_flush(term);
                open_count++;
                progress_row = solar_os_shell_io_cursor_row(term);
            }

            vTaskDelay(1);
        }
        if (stopped) {
            break;
        }
    }

    netscan_clear_progress(term, progress_row);
    if (stopped) {
        solar_os_shell_io_writeln(term, "netscan: stopped");
    }
    solar_os_shell_io_printf(term,
                             "netscan: %u open, %u probes\n",
                             (unsigned)open_count,
                             (unsigned)probe_count);
    solar_os_shell_io_set_cursor_visible(term, cursor_was_visible);
}
#endif

static void print_datetime_line(solar_os_shell_io_t *term,
                                const char *label,
                                const solar_os_datetime_t *datetime)
{
    solar_os_shell_io_printf(term,
                             "%s: %04u-%02u-%02u %02u:%02u:%02u\n",
                             label,
                             (unsigned)datetime->year,
                             (unsigned)datetime->month,
                             (unsigned)datetime->day,
                             (unsigned)datetime->hour,
                             (unsigned)datetime->minute,
                             (unsigned)datetime->second);
}

void solar_os_shell_cmd_ntp(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc > 2) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        solar_os_shell_io_writeln(term, "ntp: WiFi is not connected");
        return;
    }

    const char *server = argc == 2 ? argv[1] : SOLAR_OS_NTP_DEFAULT_SERVER;
    solar_os_shell_io_printf(term, "ntp: syncing with %s\n", server);

    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    const esp_err_t err = solar_os_time_ntp_sync(server,
                                                 SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS,
                                                 &utc,
                                                 &local);
    if (err == ESP_ERR_TIMEOUT) {
        solar_os_shell_io_writeln(term, "ntp: sync timed out");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "usage: ntp [server]");
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "ntp: RTC not available on this board");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "ntp: sync failed: %s\n", esp_err_to_name(err));
        return;
    }

    char timezone[SOLAR_OS_TIMEZONE_NAME_MAX];
    solar_os_time_get_timezone(timezone, sizeof(timezone), NULL, 0);
    print_datetime_line(term, "UTC", &utc);
    print_datetime_line(term, timezone, &local);
}
