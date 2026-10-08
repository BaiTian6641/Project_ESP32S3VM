/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_ESP32S3_I2S_PDM_H
#define HW_MISC_ESP32S3_I2S_PDM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum Esp32s3I2sPdmResult {
    ESP32S3_I2S_PDM_OK,
    ESP32S3_I2S_PDM_EMPTY,
    ESP32S3_I2S_PDM_BUSY,
    ESP32S3_I2S_PDM_INVALID_CONFIG,
    ESP32S3_I2S_PDM_MISSING_SOURCE,
    ESP32S3_I2S_PDM_CONVERTER_UNDOCUMENTED,
    ESP32S3_I2S_PDM_UNSUPPORTED_PORT,
    ESP32S3_I2S_PDM_MAPPING_UNDOCUMENTED,
} Esp32s3I2sPdmResult;

/* Separate instance for TX and RX. No heap, shared history, or sample fallback. */
typedef struct Esp32s3I2sPdmRawState {
    uint16_t words[8];
    uint8_t counts[8];
    uint8_t ready;
    uint8_t enabled;
    uint8_t port;
} Esp32s3I2sPdmRawState;

void esp32s3_i2s_pdm_raw_reset(Esp32s3I2sPdmRawState *s, unsigned port);

/*
 * cfg/conf1 are TX_CONF/TX_CONF1 or RX_CONF/RX_CONF1, not converter
 * configuration registers. pdm_cfg is TX_PCM2PDM_CONF. A converter requested
 * on I2S0 returns CONVERTER_UNDOCUMENTED; I2S1 returns UNSUPPORTED_PORT.
 * Reset before switching direction or changing format/active channels.
 * Raw packing requires PCM_BYPASS=1; A-law/u-law is not PDM conversion.
 */
Esp32s3I2sPdmResult esp32s3_i2s_pdm_tx_mode(unsigned port, uint32_t cfg,
                                         uint32_t conf1, uint32_t pdm_cfg);
Esp32s3I2sPdmResult esp32s3_i2s_pdm_rx_mode(unsigned port, uint32_t cfg,
                                         uint32_t conf1, uint32_t tdm_ctrl);

/*
 * Feed one complete DMA frame: two consecutive 16-bit units in stereo,
 * first only in mono. Units are little-endian DMA values before BIG_ENDIAN.
 * single_data is the actual SINGLE_DATA register, never a synthetic sample.
 */
Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_tx_load(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t conf1,
    uint32_t pdm_cfg, uint16_t first, uint16_t second, uint16_t single_data);

/*
 * Caller invokes once per falling BCK edge, after updating WS. WS is the
 * externally routed PDM CLK (raw BCK = 2 * CLK). Codec: low WS is left and
 * high WS is right, inverted by WS_IDLE_POL. DAC outputs hold one bit over
 * both halves. line_mask identifies real driven lines; outputs are untouched
 * on failure. Caller owns actual clocks, GPIO matrix and powered peers.
 */
Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_tx_edge(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t pdm_cfg, bool ws,
    uint8_t *line_bits, uint8_t *line_mask);

/*
 * Caller invokes once per rising BCK edge with current WS and physical line
 * levels. line_valid must come from powered, registered electrical sources;
 * missing enabled inputs fail atomically instead of producing zeros.
 * Channel 2*line = left, 2*line+1 = right. Only enabled TDM_CTRL bits 0..7
 * are captured; RX_MONO further chooses first/second half. One full frame
 * remains buffered until popped; BUSY does not overwrite it.
 */
Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_rx_edge(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint32_t conf1,
    uint32_t tdm_ctrl, bool ws, uint8_t line_bits, uint8_t line_valid);

/* Pop complete frames in ascending channel order, as 16-bit DMA units. */
Esp32s3I2sPdmResult esp32s3_i2s_pdm_raw_rx_pop(
    Esp32s3I2sPdmRawState *s, uint32_t cfg, uint8_t *channel, uint16_t *word);

#endif
