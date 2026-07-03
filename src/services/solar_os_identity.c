#include "solar_os_identity.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_storage.h"

#define SOLAR_OS_IDENTITY_DIR ".solar"
#define SOLAR_OS_IDENTITY_USER_FILE "user"
#define SOLAR_OS_IDENTITY_HOSTNAME_FILE "hostname"

static bool identity_char_is_valid(char ch)
{
    const unsigned char value = (unsigned char)ch;
    return isalnum(value) || ch == '-' || ch == '_' || ch == '.';
}

static bool identity_value_is_valid(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    for (const char *p = value; *p != '\0'; p++) {
        if (!identity_char_is_valid(*p)) {
            return false;
        }
    }
    return true;
}

static void identity_trim(char *value)
{
    if (value == NULL) {
        return;
    }

    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[--len] = '\0';
    }

    char *start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }
}

static void identity_read_file(const char *name,
                               const char *fallback,
                               char *buffer,
                               size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    strlcpy(buffer, fallback, len);
    if (!solar_os_storage_is_mounted()) {
        return;
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path(SOLAR_OS_IDENTITY_DIR, dir, sizeof(dir)) != ESP_OK ||
        solar_os_storage_join_path(dir, name, path, sizeof(path)) != ESP_OK) {
        return;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    char value[64];
    if (fgets(value, sizeof(value), file) != NULL) {
        identity_trim(value);
        if (identity_value_is_valid(value)) {
            strlcpy(buffer, value, len);
        }
    }
    fclose(file);
}

void solar_os_identity_get_user(char *buffer, size_t len)
{
    identity_read_file(SOLAR_OS_IDENTITY_USER_FILE,
                       SOLAR_OS_IDENTITY_DEFAULT_USER,
                       buffer,
                       len);
}

void solar_os_identity_get_hostname(char *buffer, size_t len)
{
    identity_read_file(SOLAR_OS_IDENTITY_HOSTNAME_FILE,
                       SOLAR_OS_IDENTITY_DEFAULT_HOSTNAME,
                       buffer,
                       len);
}

void solar_os_identity_format(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    char user[SOLAR_OS_IDENTITY_USER_MAX];
    char hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX];
    solar_os_identity_get_user(user, sizeof(user));
    solar_os_identity_get_hostname(hostname, sizeof(hostname));
    snprintf(buffer, len, "%s@%s", user, hostname);
}
