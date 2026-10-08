/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Raw packing only. See ../pdm-provenance.json for authoritative contracts
 * and the missing PCM converter specification. Never substitute an unrelated
 * sigma-delta recurrence for the ESP32-S3 hardware converter.
 */
#ifdef ESP32S3_I2S_PDM_STANDALONE
#include <string.h>
#include "esp32s3_i2s_pdm.h"
#else
#include "qemu/osdep.h"
#include "hw/misc/esp32s3_i2s_pdm.h"
#endif

#define CFG_MONO             (1U << 5)
#define CFG_BIG_ENDIAN       (1U << 7)
#define CFG_FIRST_VALID      (1U << 9)
#define CFG_PCM_BYPASS       (1U << 12)
#define CFG_WS_POL           (1U << 17)
#define CFG_LSB_FIRST        (1U << 18)
#define CFG_TDM              (1U << 19)
#define CFG_PDM              (1U << 20)
#define CFG_RX_CONVERTER     (1U << 21)
#define PDM_DAC_TWO          (1U << 23)
#define PDM_DAC              (1U << 24)
#define PDM_TX_CONVERTER     (1U << 25)

static uint16_t dma_word(uint16_t word, uint32_t cfg)
{
    return cfg & CFG_BIG_ENDIAN ? (uint16_t)((word << 8) | (word >> 8)) : word;
}

static uint8_t rx_enabled(uint32_t cfg, uint32_t tdm_ctrl)
{
    uint8_t mask = tdm_ctrl & 0xff;

    if (cfg & CFG_MONO) {
        bool first = cfg & CFG_FIRST_VALID;
        bool pol = cfg & CFG_WS_POL;
        /* FIRST_VALID selects the first temporal half, not a fixed channel. */
        mask &= first != pol ? 0x55 : 0xaa;
    }
    return mask;
}

static Esp32s3I2sPdmResult common_mode(unsigned port, uint32_t cfg,
                                      uint32_t conf1, bool tx)
{
    unsigned bits = ((conf1 >> 13) & 31) + 1;
    unsigned half = ((conf1 >> 18) & 63) + 1;

    if (port > 1) {
        return ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
    }
    if (!(cfg & CFG_PDM) || (cfg & CFG_TDM) || !(cfg & CFG_PCM_BYPASS) ||
        half != 16 ||
        (bits != 16 && !(tx && !(cfg & CFG_MONO) && bits == 32))) {
        return ESP32S3_I2S_PDM_INVALID_CONFIG;
    }
    return ESP32S3_I2S_PDM_OK;
}

void esp32s3_i2s_pdm_raw_reset(Esp32s3I2sPdmRawState *s, unsigned port)
{
    memset(s, 0, sizeof(*s));
    s->port = port > 1 ? UINT8_MAX : port;
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_tx_mode(unsigned port, uint32_t cfg,
                                         uint32_t conf1, uint32_t pdm_cfg)
{
    Esp32s3I2sPdmResult result = common_mode(port, cfg, conf1, true);

    if (result != ESP32S3_I2S_PDM_OK) {
        return result;
    }
    if (pdm_cfg & PDM_TX_CONVERTER) {
        return port == 0 ? ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED :
                           ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
    }
    if (((cfg >> 24) & 7) > 4) {
        return ESP32S3_I2S_PDM_INVALID_CONFIG;
    }
    if (pdm_cfg & PDM_DAC) {
        if (port != 0) {
            return ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
        }
        /* Vendor table does not define stereo single-DAC or raw mono dual-DAC. */
        if (!!(pdm_cfg & PDM_DAC_TWO) == !!(cfg & CFG_MONO)) {
            return ESP32S3_I2S_PDM_MAPPING_UNDOCUMENTED;
        }
    }
    return ESP32S3_I2S_PDM_OK;
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_rx_mode(unsigned port, uint32_t cfg,
                                         uint32_t conf1, uint32_t tdm_ctrl)
{
    Esp32s3I2sPdmResult result = common_mode(port, cfg, conf1, false);
    uint8_t mask = rx_enabled(cfg, tdm_ctrl);

    if (result != ESP32S3_I2S_PDM_OK) {
        return result;
    }
    if (cfg & CFG_RX_CONVERTER) {
        return port == 0 ? ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED :
                           ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
    }
    if (port == 1 && (mask & 0xfc)) {
        return ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
    }
    return ESP32S3_I2S_PDM_OK;
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_tx_load(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t conf1,
    uint32_t pdm_cfg, uint16_t first, uint16_t second, uint16_t single_data)
{
    Esp32s3I2sPdmResult result = esp32s3_i2s_pdm_tx_mode(
        s->port, cfg, conf1, pdm_cfg);
    uint16_t left, right;
    bool pol = cfg & CFG_WS_POL;
    unsigned mode = (cfg >> 24) & 7;

    if (result != ESP32S3_I2S_PDM_OK) {
        return result;
    }
    if (s->ready) {
        return ESP32S3_I2S_PDM_BUSY;
    }
    first = dma_word(first, cfg);
    second = dma_word(second, cfg);
    if (cfg & CFG_MONO) {
        bool left_valid = !!(cfg & CFG_FIRST_VALID) != pol;
        left = left_valid ? first : single_data;
        right = left_valid ? single_data : first;
    } else {
        left = pol ? second : first;
        right = pol ? first : second;
    }
    switch (mode) {
    case 1:
        if (pol) {
            left = right;
        } else {
            right = left;
        }
        break;
    case 2:
        if (pol) {
            right = left;
        } else {
            left = right;
        }
        break;
    case 3:
        if (pol) {
            right = single_data;
        } else {
            left = single_data;
        }
        break;
    case 4:
        if (pol) {
            left = single_data;
        } else {
            right = single_data;
        }
        break;
    default:
        break;
    }
    if ((pdm_cfg & PDM_DAC) && !(pdm_cfg & PDM_DAC_TWO)) {
        /* Single-DAC mono consumes the sole DMA unit, not an inactive slot. */
        left = right = first;
    }
    s->words[0] = left;
    s->words[1] = right;
    s->counts[0] = s->counts[1] = 0;
    s->ready = 3;
    return ESP32S3_I2S_PDM_OK;
}

static unsigned tx_bit(const Esp32s3I2sPdmRawState *s, uint32_t cfg,
                       unsigned channel)
{
    unsigned bit = cfg & CFG_LSB_FIRST ? s->counts[channel] :
                                        15 - s->counts[channel];
    return (s->words[channel] >> bit) & 1;
}

static void tx_advance(Esp32s3I2sPdmRawState *s, unsigned channel)
{
    if (++s->counts[channel] == 16) {
        s->ready &= ~(1U << channel);
    }
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_tx_edge(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t pdm_cfg, bool ws,
    uint8_t *line_bits, uint8_t *line_mask)
{
    unsigned phase = ws != !!(cfg & CFG_WS_POL);

    if (pdm_cfg & PDM_TX_CONVERTER) {
        return s->port == 0 ? ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED :
                              ESP32S3_I2S_PDM_UNSUPPORTED_PORT;
    }
    if (pdm_cfg & PDM_DAC) {
        if (s->ready != 3) {
            return ESP32S3_I2S_PDM_EMPTY;
        }
        /* Dual-DAC: primary DOUT carries right, DOUT2 carries left. */
        *line_bits = tx_bit(s, cfg, 1);
        *line_mask = 1;
        if (pdm_cfg & PDM_DAC_TWO) {
            *line_bits |= tx_bit(s, cfg, 0) << 1;
            *line_mask = 3;
        }
        if (phase == 1) {
            tx_advance(s, 0);
            tx_advance(s, 1);
        }
    } else {
        if (!(s->ready & (1U << phase))) {
            return ESP32S3_I2S_PDM_EMPTY;
        }
        *line_bits = tx_bit(s, cfg, phase);
        *line_mask = 1;
        tx_advance(s, phase);
    }
    return ESP32S3_I2S_PDM_OK;
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_rx_edge(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t conf1,
    uint32_t tdm_ctrl, bool ws, uint8_t line_bits, uint8_t line_valid)
{
    Esp32s3I2sPdmResult result = esp32s3_i2s_pdm_rx_mode(
        s->port, cfg, conf1, tdm_ctrl);
    unsigned phase = ws != !!(cfg & CFG_WS_POL);
    uint8_t enabled = rx_enabled(cfg, tdm_ctrl);
    unsigned line;

    if (result != ESP32S3_I2S_PDM_OK) {
        return result;
    }
    if (s->ready) {
        return ESP32S3_I2S_PDM_BUSY;
    }
    for (line = 0; line < 4; line++) {
        unsigned channel = 2 * line + phase;
        if ((enabled & (1U << channel)) && !(line_valid & (1U << line))) {
            return ESP32S3_I2S_PDM_MISSING_SOURCE;
        }
        if ((enabled & (1U << channel)) && s->counts[channel] == 16) {
            return ESP32S3_I2S_PDM_BUSY;
        }
    }
    s->enabled = enabled;
    for (line = 0; line < 4; line++) {
        unsigned channel = 2 * line + phase;
        unsigned bit;
        if (!(enabled & (1U << channel))) {
            continue;
        }
        bit = cfg & CFG_LSB_FIRST ? s->counts[channel] :
                                   15 - s->counts[channel];
        s->words[channel] |= ((line_bits >> line) & 1) << bit;
        s->counts[channel]++;
    }
    for (line = 0; line < 8; line++) {
        if ((enabled & (1U << line)) && s->counts[line] != 16) {
            return ESP32S3_I2S_PDM_OK;
        }
    }
    s->ready = enabled;
    return ESP32S3_I2S_PDM_OK;
}

Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_rx_pop(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint8_t *channel, uint16_t *word)
{
    unsigned ch;

    if (!s->ready) {
        return ESP32S3_I2S_PDM_EMPTY;
    }
    for (ch = 0; ch < 8; ch++) {
        if (s->ready & (1U << ch)) {
            *channel = ch;
            *word = dma_word(s->words[ch], cfg);
            s->ready &= ~(1U << ch);
            s->words[ch] = 0;
            s->counts[ch] = 0;
            return ESP32S3_I2S_PDM_OK;
        }
    }
    return ESP32S3_I2S_PDM_EMPTY;
}
