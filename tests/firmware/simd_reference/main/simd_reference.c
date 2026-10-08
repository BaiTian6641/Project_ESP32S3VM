#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SEED UINT32_C(0x5a17c3e9)
#define CASES 12
#define OPERATIONS 7
#define CAPTURE_GRACE_MS 15000

typedef struct __attribute__((aligned(16))) {
    uint8_t q[128];
    uint8_t qacc[40];
    uint32_t aux[9]; /* SAR, SAR_BYTE, FFT_BIT_WIDTH, ACCX0/1, UA_STATE0..3 */
    uint32_t ar_deltas[2];
} pie_state;

_Static_assert(offsetof(pie_state, qacc) == 128, "assembly layout");
_Static_assert(offsetof(pie_state, aux) == 168, "assembly layout");
_Static_assert(offsetof(pie_state, ar_deltas) == 204, "assembly layout");

extern void pie_reference(unsigned op, const pie_state *input, pie_state *before, pie_state *after);
static pie_state input, before, after;

static uint32_t random32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void put20(uint8_t *out, unsigned lane, uint32_t value)
{
    for (unsigned bit = 0; bit < 20; ++bit) {
        unsigned position = lane * 20 + bit;
        uint8_t mask = (uint8_t)(1u << (position % 8));
        if ((value >> bit) & 1u) out[position / 8] |= mask;
        else out[position / 8] &= (uint8_t)~mask;
    }
}

static void hex(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

static void state_json(const pie_state *state)
{
    printf("{\"q\":\""); hex(state->q, sizeof(state->q));
    printf("\",\"qacc\":\""); hex(state->qacc, sizeof(state->qacc));
    printf("\",\"aux\":\""); hex(state->aux, sizeof(state->aux));
    printf("\",\"ar_deltas\":\""); hex(state->ar_deltas, sizeof(state->ar_deltas));
    printf("\"}");
}

static void make_input(unsigned index, uint32_t vector_seed)
{
    uint32_t rng = vector_seed;
    memset(&input, 0, sizeof(input));
    for (unsigned i = 0; i < sizeof(input.q); ++i) input.q[i] = (uint8_t)random32(&rng);
    for (unsigned lane = 0; lane < 16; ++lane) put20(input.qacc, lane, random32(&rng) & 0xfffff);
    if (index == 0) memset(&input, 0, sizeof(input));
    if (index == 1) {
        memset(input.q, 0xff, 32);
        for (unsigned lane = 0; lane < 16; ++lane) put20(input.qacc, lane, 0xfffff - 10);
    }
    if (index == 2) {
        for (unsigned lane = 0; lane < 16; ++lane) {
            input.q[lane] = lane & 1 ? 0x80 : 0x7f;
            input.q[16 + lane] = 1;
            put20(input.qacc, lane, lane & 1 ? 0x80000 + 5 : 0x7ffff - 5);
        }
    }
    if (index == 3) {
        for (unsigned i = 0; i < sizeof(input.q); ++i) input.q[i] = (uint8_t)i;
    }
}

void app_main(void)
{
    /* Ordinary application delay: give the operator time to open the USB/UART
     * capture after an explicitly approved deployment and reset. */
    vTaskDelay(pdMS_TO_TICKS(CAPTURE_GRACE_MS));
    static const char *operations[] = {"ee.vmulas.u8.qacc", "ee.vadds.s8", "ee.vadds.s8.alias_q0",
        "mv.qr", "ee.vsubs.s8", "ee.vmulas.s8.qacc", "ee.vadds.s16.alias_q1"};
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    unsigned sequence = 0;
    printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"begin\",\"seq\":%u,\"fixture\":\"simd_reference\","
           "\"seed\":%lu,\"expected_records\":%u,\"capture_grace_ms\":%u,\"idf\":\"%s\",\"chip_revision\":%u,\"cores\":%u,"
           "\"core\":%d,\"source_sha256\":\"%s\",\"elf_digest\":\"",
           sequence++, (unsigned long)SEED, CASES * OPERATIONS, CAPTURE_GRACE_MS, IDF_VER,
           chip.revision, chip.cores, xPortGetCoreID(), REFERENCE_SOURCE_SHA256);
    hex(app->app_elf_sha256, 32);
    printf("\"}\n");
    fflush(stdout);
    for (unsigned op = 0; op < OPERATIONS; ++op) {
        for (unsigned index = 0; index < CASES; ++index) {
            uint32_t vector_seed = SEED ^ (op << 16) ^ index;
            make_input(index, vector_seed);
            memset(&before, 0, sizeof(before));
            memset(&after, 0, sizeof(after));
            /* Prevent task migration/interrupt use from disturbing captured PIE state. */
            portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
            portENTER_CRITICAL(&lock);
            pie_reference(op, &input, &before, &after);
            portEXIT_CRITICAL(&lock);
            printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"vector\",\"seq\":%u,\"case\":\"op%u-case%u\","
                   "\"operation\":\"%s\",\"vector_seed\":%lu,\"input_q\":\"",
                   sequence++, op, index, operations[op], (unsigned long)vector_seed);
            hex(input.q, sizeof(input.q));
            printf("\",\"input_qacc\":\""); hex(input.qacc, sizeof(input.qacc));
            printf("\",\"before\":"); state_json(&before);
            printf(",\"after\":"); state_json(&after);
            printf("}\n");
            fflush(stdout);
            vTaskDelay(1);
        }
    }
    printf("ESP32S3VM_REF {\"schema\":1,\"type\":\"end\",\"seq\":%u,\"records\":%u,\"complete\":true}\n",
           sequence, CASES * OPERATIONS);
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
