/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone packing tests; deliberately do not assert invented PCM vectors. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esp32s3_i2s_pdm.h"
#include "esp32s3-i2s-pdm-vectors.h"

#define RAW_CFG ((1U << 20) | (1U << 12))
#define RAW_CONF1 ((15U << 13) | (15U << 18))

static void test_tx_vectors(void)
{
    Esp32s3I2sPdmRawState state;
    unsigned vector;

    for (vector = 0; vector < sizeof(pdm_vectors) / sizeof(pdm_vectors[0]); vector++) {
        const struct PdmVector *v = &pdm_vectors[vector];
        uint8_t bits = 0xd5, mask = 0xc3;
        unsigned edge;

        esp32s3_i2s_pdm_raw_reset(&state, 0);
        assert(esp32s3_i2s_pdm_raw_tx_edge(&state, v->cfg, v->pdm_cfg,
                                        false, &bits, &mask) == ESP32S3_I2S_PDM_EMPTY);
        assert(bits == 0xd5 && mask == 0xc3);
        assert(esp32s3_i2s_pdm_raw_tx_load(&state, v->cfg, v->conf1, v->pdm_cfg,
                                        v->first, v->second, v->single) == ESP32S3_I2S_PDM_OK);
        assert(esp32s3_i2s_pdm_raw_tx_load(&state, v->cfg, v->conf1, v->pdm_cfg,
                                        0, 0, 0) == ESP32S3_I2S_PDM_BUSY);
        for (edge = 0; edge < 32; edge++) {
            bool ws = !!(v->cfg & (1U << 17)) != !!(edge % 2);
            assert(esp32s3_i2s_pdm_raw_tx_edge(&state, v->cfg, v->pdm_cfg,
                                            ws, &bits, &mask) == ESP32S3_I2S_PDM_OK);
            assert(bits == v->edges[edge]);
            assert(mask == v->line_mask);
        }
        assert(state.ready == 0);
        assert(esp32s3_i2s_pdm_raw_tx_load(&state, v->cfg, v->conf1, v->pdm_cfg,
                                        v->first, v->second, v->single) == ESP32S3_I2S_PDM_OK);
    }
}

static void test_rx_frames(void)
{
    /* Author independently from the TX converter: one textual bitstream/channel. */
    static const char *const streams[8] = {
        "0001001000110100", "0101011001111000",
        "1001101010111100", "1101111011110000",
        "1000000000000001", "0111111111111110",
        "1010101001010101", "0011001111001100",
    };
    static const uint16_t words[8] = {
        0x1234, 0x5678, 0x9abc, 0xdef0, 0x8001, 0x7ffe, 0xaa55, 0x33cc,
    };
    Esp32s3I2sPdmRawState state, before;
    unsigned frame, bit, phase, line, ch;
    uint8_t channel;
    uint16_t word;

    esp32s3_i2s_pdm_raw_reset(&state, 0);
    for (frame = 0; frame < 3; frame++) {
        for (bit = 0; bit < 16; bit++) {
            for (phase = 0; phase < 2; phase++) {
                uint8_t inputs = 0;
                for (line = 0; line < 4; line++) {
                    inputs |= (streams[2 * line + phase][bit] == '1') << line;
                }
                before = state;
                assert(esp32s3_i2s_pdm_raw_rx_edge(&state, RAW_CFG, RAW_CONF1,
                                                 0xff, phase, inputs, 7) ==
                       ESP32S3_I2S_PDM_MISSING_SOURCE);
                assert(memcmp(&before, &state, sizeof(state)) == 0);
                assert(esp32s3_i2s_pdm_raw_rx_edge(&state, RAW_CFG, RAW_CONF1,
                                                 0xff, phase, inputs, 15) == ESP32S3_I2S_PDM_OK);
                if (bit != 15 || phase != 1) {
                    assert(esp32s3_i2s_pdm_raw_rx_pop(&state, RAW_CFG, &channel, &word) ==
                           ESP32S3_I2S_PDM_EMPTY);
                }
            }
        }
        before = state;
        assert(esp32s3_i2s_pdm_raw_rx_edge(&state, RAW_CFG, RAW_CONF1,
                                         0xff, false, 0, 15) == ESP32S3_I2S_PDM_BUSY);
        assert(memcmp(&before, &state, sizeof(state)) == 0);
        for (ch = 0; ch < 8; ch++) {
            assert(esp32s3_i2s_pdm_raw_rx_pop(&state, RAW_CFG, &channel, &word) == ESP32S3_I2S_PDM_OK);
            assert(channel == ch && word == words[ch]);
        }
        assert(esp32s3_i2s_pdm_raw_rx_pop(&state, RAW_CFG, &channel, &word) == ESP32S3_I2S_PDM_EMPTY);
    }
}

static void test_rx_mask_and_order(void)
{
    Esp32s3I2sPdmRawState state;
    unsigned bit;
    uint8_t channel;
    uint16_t word;
    uint32_t cfg = RAW_CFG | (1U << 17) | (1U << 18) | (1U << 7);
    const char *stream = "0010110001001000"; /* 0x1234 LSB-first */

    esp32s3_i2s_pdm_raw_reset(&state, 1);
    for (bit = 0; bit < 16; bit++) {
        assert(esp32s3_i2s_pdm_raw_rx_edge(&state, cfg, RAW_CONF1, 1,
                                         true, stream[bit] == '1', 1) == ESP32S3_I2S_PDM_OK);
        if (bit != 15) {
            /* Unselected WS phase needs no powered input. */
            assert(esp32s3_i2s_pdm_raw_rx_edge(&state, cfg, RAW_CONF1, 1,
                                             false, 0, 0) == ESP32S3_I2S_PDM_OK);
        }
    }
    assert(esp32s3_i2s_pdm_raw_rx_pop(&state, cfg, &channel, &word) == ESP32S3_I2S_PDM_OK);
    assert(channel == 0 && word == 0x3412);
    esp32s3_i2s_pdm_raw_reset(&state, 1);
    assert(state.ready == 0 && state.counts[0] == 0 && state.words[0] == 0);
}

static void test_capability_qualification(void)
{
    Esp32s3I2sPdmRawState state;
    uint8_t bits = 0xab, mask = 0xcd;

    assert(esp32s3_i2s_pdm_tx_mode(0, RAW_CFG, RAW_CONF1, 1U << 25) ==
           ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED);
    assert(esp32s3_i2s_pdm_tx_mode(1, RAW_CFG, RAW_CONF1, 1U << 25) ==
           ESP32S3_I2S_PDM_UNSUPPORTED_PORT);
    assert(esp32s3_i2s_pdm_rx_mode(0, RAW_CFG | (1U << 21), RAW_CONF1, 3) ==
           ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED);
    assert(esp32s3_i2s_pdm_rx_mode(1, RAW_CFG | (1U << 21), RAW_CONF1, 3) ==
           ESP32S3_I2S_PDM_UNSUPPORTED_PORT);
    assert(esp32s3_i2s_pdm_rx_mode(1, RAW_CFG, RAW_CONF1, 4) ==
           ESP32S3_I2S_PDM_UNSUPPORTED_PORT);
    assert(esp32s3_i2s_pdm_rx_mode(0, RAW_CFG, RAW_CONF1, 0xff) == ESP32S3_I2S_PDM_OK);
    assert(esp32s3_i2s_pdm_tx_mode(1, RAW_CFG, RAW_CONF1, 0) == ESP32S3_I2S_PDM_OK);
    assert(esp32s3_i2s_pdm_tx_mode(0, RAW_CFG | (1U << 19), RAW_CONF1, 0) ==
           ESP32S3_I2S_PDM_INVALID_CONFIG);
    assert(esp32s3_i2s_pdm_rx_mode(0, RAW_CFG, 0, 3) == ESP32S3_I2S_PDM_INVALID_CONFIG);
    esp32s3_i2s_pdm_raw_reset(&state, 0);
    assert(esp32s3_i2s_pdm_raw_tx_edge(&state, RAW_CFG, 1U << 25,
                                    false, &bits, &mask) == ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED);
    assert(bits == 0xab && mask == 0xcd);
}

int main(void)
{
    test_tx_vectors();
    test_rx_frames();
    test_rx_mask_and_order();
    test_capability_qualification();
    return 0;
}
