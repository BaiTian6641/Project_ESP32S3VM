/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hostbus-json.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct { const char *text; bool valid; } vectors[] = {
        {"{\"x\":1,\"y\":[true,false,null]}", true},
        {"{\"x\":\"apostrophe ' inside a string\"}", true},
        {"{\"x\":\"escaped \\\" quote\"}", true},
        {"{\"x\":\"\\b\\f\\n\\r\\t\\/\\\\\"}", true},
        {"{\"x\":\"\\u0000\\uD83D\\uDE00\"}", true},
        {" \t\r\n{\"x\":0} ", true},
        {"{'x':1}", false},
        {"{\"x\":'y'}", false},
        {"{\"x\":\"\\'\"}", false},
        {"{\"x\":\"\\a\"}", false},
        {"{\"x\":\"\\u123\"}", false},
        {"{\"x\":\"\\u12gg\"}", false},
        {"{\"x\":\"raw\ttab\"}", false},
        {"{\"x\":\"raw\nnewline\"}", false},
        {"{\"x\":1}\v", false},
        {"[}", false}, {"}", false}, {"[", false},
        {"\"unclosed", false}, {"\"trailing\\", false},
    };
    unsigned checks = 0;
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        if (hostbus_json_prescan((const uint8_t *)vectors[i].text,
                                strlen(vectors[i].text)) != vectors[i].valid) {
            fprintf(stderr, "prescan vector %u failed\n", i);
            return 1;
        }
        ++checks;
    }
    for (unsigned depth = 63; depth <= 65; ++depth) {
        char text[132];
        memset(text, '[', depth); text[depth] = '0';
        memset(text + depth + 1, ']', depth);
        if (hostbus_json_prescan((const uint8_t *)text, depth * 2 + 1) != (depth <= 64)) {
            fprintf(stderr, "depth %u boundary failed\n", depth);
            return 1;
        }
        ++checks;
    }
    const uint8_t nul[] = {'{', '}', 0};
    if (hostbus_json_prescan(nul, sizeof(nul))) { return 1; }
    printf("%u strict lexical/depth checks passed (full parser is separate)\n", checks + 1);
    return 0;
}
