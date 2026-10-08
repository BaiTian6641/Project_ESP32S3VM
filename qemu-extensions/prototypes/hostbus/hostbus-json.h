/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Strict transport lexical/depth gate. Full syntax/duplicate keys are still
 * checked by QEMU's parser; this disables its nonstandard string extensions. */
#ifndef ESP32S3VM_HOSTBUS_JSON_H
#define ESP32S3VM_HOSTBUS_JSON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool hostbus_json_prescan(const uint8_t *bytes, size_t length)
{
    char nesting[64];
    unsigned depth = 0;
    bool quoted = false;
    size_t i;
    for (i = 0; i < length; ++i) {
        uint8_t c = bytes[i];
        if (quoted) {
            if (c < 0x20) { return false; }
            if (c == '"') { quoted = false; continue; }
            if (c != '\\') { continue; }
            if (++i == length) { return false; }
            c = bytes[i];
            if (c == '"' || c == '\\' || c == '/' || c == 'b' ||
                c == 'f' || c == 'n' || c == 'r' || c == 't') {
                continue;
            }
            if (c != 'u' || length - i <= 4) { return false; }
            for (unsigned digit = 0; digit < 4; ++digit) {
                c = bytes[++i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) { return false; }
            }
        } else {
            if (c == '"') { quoted = true; }
            else if (c == '\'' || c == '\\' ||
                     (c < 0x20 && c != '\t' && c != '\r' && c != '\n')) {
                return false;
            } else if (c == '{' || c == '[') {
                if (depth == sizeof(nesting)) { return false; }
                nesting[depth++] = c;
            } else if (c == '}' || c == ']') {
                if (!depth || nesting[--depth] != (c == '}' ? '{' : '[')) {
                    return false;
                }
            }
        }
    }
    return !quoted && depth == 0;
}
#endif
