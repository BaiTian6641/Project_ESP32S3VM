/*
 * GDMA emulation for recent ESP32-series chip (ESP32-S3 and newer)
 *
 * Copyright (c) 2023-2025 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"

#define TYPE_ESP_GDMA "esp.gdma"
#define ESP_GDMA(obj)               OBJECT_CHECK(ESPGdmaState, (obj), TYPE_ESP_GDMA)
#define ESP_GDMA_GET_CLASS(obj)     OBJECT_GET_CLASS(ESPGdmaClass, obj, TYPE_ESP_GDMA)
#define ESP_GDMA_CLASS(klass)       OBJECT_CLASS_CHECK(ESPGdmaClass, klass, TYPE_ESP_GDMA)


#define ESP_GDMA_IN_IDX     0
#define ESP_GDMA_OUT_IDX    1
#define ESP_GDMA_CONF_COUNT (ESP_GDMA_OUT_IDX + 1)


#define ESP_GDMA_RAM_ADDR   0x3FC80000

/*
 * Descriptor walk bounds (docs/plans/peripherals-electrical.md CORE-04).
 *
 * Descriptors are re-fetched from guest memory following the hardware rules
 * (TRM chapter 3 "GDMA Controller"), so a malicious self-referential chain
 * must never loop the host thread forever:
 *
 * - ESP_GDMA_WORK_QUANTUM bounds one scheduled slice of descriptor fetches.
 *   A chain that still has work left after a slice stays armed (LINK.PARK
 *   reads 0) and continues from the next zero-delay timer deadline, which
 *   keeps valid streaming rings alive without ever blocking the host.
 * - ESP_GDMA_MAX_WALK_DESCRIPTORS is an absolute per-arm safety valve; a
 *   chain that reaches it raises DSCR_ERR and halts.
 * - ESP_GDMA_MAX_IDLE_DESCRIPTORS rejects non-progressing cycles: that many
 *   consecutive descriptor fetches without moving a single byte raise
 *   DSCR_ERR and halt (zero-length self-loops).
 */
#define ESP_GDMA_WORK_QUANTUM        256u
#define ESP_GDMA_MAX_WALK_DESCRIPTORS 65536u
#define ESP_GDMA_MAX_IDLE_DESCRIPTORS   16u

/* Upper bound of channels accepted by the generic engine */
#define ESP_GDMA_MAX_CHANNELS          16u

/**
 * @brief Names for the IN and OUT IRQs pins, can be passed to `qdev_connect_gpio_out_named`.
 */
#define ESP_GDMA_IRQ_IN_NAME    "CHAN_IN"
#define ESP_GDMA_IRQ_OUT_NAME   "CHAN_OUT"


/**
 * @brief Number for each peripheral that can access GDMA
 */
typedef enum {
    GDMA_SPI2   = 0,
    GDMA_SPI3   = 1,
    GDMA_UHCI0  = 2,
    GDMA_I2S0   = 3,
    GDMA_I2S1   = 4,
    GDMA_LCDCAM = 5,
    GDMA_AES    = 6,
    GDMA_SHA    = 7,
    GDMA_ADC    = 8,
    GDMA_RMT    = 9,
    GDMA_LAST   = GDMA_RMT,
} GdmaPeripheral;


/**
 * @brief Size of the interrupt registers, in bytes, for a single channel
 */
#define ESP_GDMA_INT_REGS_SIZE  0x10

typedef struct {
    uint32_t raw;
    uint32_t st;
    uint32_t ena;
    qemu_irq irq;
} DmaIntState;


/* Define the generic/virtual registers the inherited class can pass to this class */
typedef enum {
    GDMA_UNKNOWN = -1,
    GDMA_CONF0_REG = 0,
    GDMA_CONF1_REG,
    GDMA_INT_RAW_REG,
    GDMA_INT_ST_REG,
    GDMA_INT_ENA_REG,
    GDMA_INT_CLR_REG,
    GDMA_FIFO_ST_REG,
    GDMA_POP_REG,
    GDMA_LINK_REG,
    GDMA_STATE_REG,
    GDMA_SUC_EOF_DESC_REG,
    GDMA_ERR_EOF_DESC_REG,
    GDMA_DESC_ADDR_REG,
    GDMA_BF0_DESC_ADDR_REG,
    GDMA_BF1_DESC_ADDR_REG,
    GDMA_DUMMY_3C_REG,
    GDMA_DUMMY_40_REG,
    GDMA_PRIORITY_REG,
    GDMA_PERI_SEL_REG,
    GDMA_MISC_REG,
} DmaRegister;


typedef struct ESPGdmaState ESPGdmaState;

typedef void (*EspGdmaPeripheralNotify)(ESPGdmaState *s,
                                       GdmaPeripheral peripheral,
                                       uint32_t channel, int direction,
                                       void *opaque);
typedef struct EspGdmaTxInfo {
    uint32_t served; /* Actual bytes fetched, valid even on failure. */
    bool eof_reached;
    bool list_done;
} EspGdmaTxInfo;

/* Defined in hw/dma/esp_gdma.c; one per channel for the M2M work quantum */
typedef struct ESPGdmaQuantum ESPGdmaQuantum;

typedef struct {
    /* Configuration registers */
    uint32_t conf0;
    uint32_t conf1;
    uint32_t status;
    uint32_t push_pop;
    uint32_t link;
    /* Status registers */
    uint32_t state;             /* Lower 18 bits of the pre-read (next) descriptor address */
    uint32_t suc_eof_desc_addr; /* Address of descriptor when EOF bit is 1 */
    uint32_t err_eof_desc_addr; /* Address of descriptor when error occurs (UHCI0 only) */
    uint32_t desc_addr;         /* Address of the next descriptor (n + 1) */
    uint32_t bfr_desc_addr;     /* Address of the current descriptor (n) */
    uint32_t bfr_bfr_desc_addr; /* Address of the previous descriptor (n - 1) */
    uint32_t priority;
    uint32_t peripheral;
    /* Interrupt related registers */
    DmaIntState int_state;
    /*
     * Persistent descriptor + byte cursor (CORE-04). These fields survive
     * across peripheral pump calls and M2M work-quantum slices so a transfer
     * always continues exactly where it stopped, instead of restarting from
     * the link address.
     */
    uint32_t cur_desc_addr; /* Guest address of the descriptor being processed */
    uint32_t cur_dw0;       /* Word 0 of the current descriptor, as fetched */
    uint32_t cur_buf_addr;  /* DW1 buffer address of the current descriptor */
    uint32_t cur_next_addr; /* DW2 next-descriptor address of the current descriptor */
    uint32_t cur_buf_off;   /* Byte cursor inside the current descriptor buffer */
    uint32_t cur_buf_cap;   /* Capacity of the current descriptor (OUT: length, IN: size) */
    bool cur_suc_eof;       /* suc_eof flag of the current descriptor */
    uint32_t rx_last_desc_addr; /* Last descriptor that actually received bytes */
    uint32_t rx_last_prev_addr; /* Its previous descriptor, before prefetch */
    bool rx_packet_ended;      /* Terminal peripheral EOF already committed */
    bool rx_segment_pending;  /* Actual stores since the last segment EOF. */
    uint32_t walk_count;    /* Descriptors fetched since the chain was armed */
    uint32_t idle_count;    /* Consecutive fetches that moved no byte (cycle detection) */
    bool fsm_active;        /* Chain armed and unfinished; LINK.PARK reads !fsm_active */
    bool halted;            /* Stopped by a descriptor error: needs RST or a new START */
    bool pending_start;     /* START written, waiting for the transfer to be serviced */
    bool pending_restart;   /* RESTART written, waiting for the transfer to be serviced */
} DmaConfigState;


typedef struct ESPGdmaState {
    SysBusDevice parent_object;

    DmaConfigState* ch_conf[ESP_GDMA_CONF_COUNT];
    /* Use this register mainly for enabling and disabling priorities */
    uint32_t misc_conf;
    /* Keep a pointer to the SoC DRAM */
    MemoryRegion* soc_mr;
    AddressSpace dma_as;
    /* ESP32-S3 SoC, used to gate the hold phase by reset domain */
    DeviceState *soc_reset;
    /* M2M work-quantum bottom halves, one per channel (see ESP_GDMA_WORK_QUANTUM) */
    ESPGdmaQuantum* quantum;
    struct {
        EspGdmaPeripheralNotify notify;
        void *opaque;
    } peripheral_notify[GDMA_LAST + 1];
} ESPGdmaState;


typedef struct ESPGdmaClass {
    SysBusDeviceClass parent_class;

    /* All the attributes and method that are common to all instances must be stored here */
    size_t m_channel_count;

    /* Virtual methods */
    /**
     * @brief Checks if the given peripheral, from GdmaPeripheral enumeration, is invalid/reserved.
     *        If NULL, all the peripherals are considered valid.
     */
    bool (*is_periph_invalid)(ESPGdmaState *s, GdmaPeripheral per);
} ESPGdmaClass;


/**
 * @brief Get the channel configured for the given peripheral
 *
 * @param s GDMA state
 * @param periph Peripheral to search
 * @param dir Direction from the GDMA point of view: ESP_GDMA_IN_IDX or ESP_GDMA_OUT_IDX.
 *            For example, to find a channel that needs to be written to, use ESP_GDMA_IN_IDX
 *            (because GDMA receives the data)
 * @param chan Returned channel index linked to the peripheral
 *
 * @returns true  if the peripheral was found and the index of the GDMA channel it is bound to is stored in `chan`,
 *          false if the peripheral was not found or invalid.
 */
bool esp_gdma_get_channel_periph(ESPGdmaState *s, GdmaPeripheral periph, int dir,
                                     uint32_t* chan);

bool esp_gdma_read_channel(ESPGdmaState *s, uint32_t chan, uint8_t* buffer, uint32_t size);
bool esp_gdma_write_channel(ESPGdmaState *s, uint32_t chan, uint8_t* buffer, uint32_t size);
/* The extended TX pump stops at the first packet EOF when info is non-NULL.
 * The legacy pump remains a contiguous byte-stream consumer. Metadata remains
 * valid after an advance/writeback fault; fetched bytes are never un-fetched. */
bool esp_gdma_read_channel_ex(ESPGdmaState *s, uint32_t chan, uint8_t *buffer,
                              uint32_t size, EspGdmaTxInfo *info);
/* Reports actual stores even if the request was only partly satisfied. */
bool esp_gdma_write_channel_ex(ESPGdmaState *s, uint32_t chan, uint8_t *buffer,
                               uint32_t size, uint32_t *served);
/* One fixed-slot notification per hardware peripheral; no hot-path allocation.
 * Called only after successful peripheral START/RESTART, never M2M/failed arm.
 * Bindings survive device reset; unregister before destroying the consumer. */
void esp_gdma_set_peripheral_notify(ESPGdmaState *s, GdmaPeripheral peripheral,
                                    EspGdmaPeripheralNotify notify, void *opaque);
/* Actual peripheral RX frame end. Writes exact partial length, clears owner,
 * marks the last received descriptor EOF and parks only this IN channel.
 * No bytes/halted/reset cursor => false, no write/IRQ; repeat end => true.
 * STOP keeps a finalizable cursor; reset discards it without writeback. */
bool esp_gdma_finish_rx_channel(ESPGdmaState *s, uint32_t chan);
/* UHCI0-only terminal malformed packet: exact length, owner=0,
 * err_eof+suc_eof, IN_ERR_EOF+IN_SUC_EOF+IN_DONE, ERR_EOF_DES_ADDR. */
bool esp_gdma_finish_rx_channel_err(ESPGdmaState *s, uint32_t chan);
/* Nonterminal receive segment EOF (I2S RX_EOF_NUM). Exact partial/full boundary
 * writeback and IN_SUC_EOF; continues directly at DW2, preserving an already
 * prefetched empty next descriptor. No IN_DONE, no terminal packet marker.
 * Returns false for no new stores/halted/reset, true once EOF is committed
 * even if loading the next descriptor then halts with DSCR_ERR. */
bool esp_gdma_finish_rx_segment(ESPGdmaState *s, uint32_t chan);


/**
 * @brief Function only meant to be used by inherited classes
 */
void esp_gdma_write_chan_register(ESPGdmaState* s, uint32_t dir, uint32_t chan, DmaRegister reg, uint32_t value);
void esp_gdma_write_register(ESPGdmaState* s, DmaRegister reg, uint32_t value);

uint64_t esp_gdma_read_chan_register(ESPGdmaState* state, uint32_t dir, uint32_t chan, DmaRegister reg);
uint64_t esp_gdma_read_register(ESPGdmaState* s, DmaRegister reg);


/**
 * @brief Define virtual registers and their generic fields for the I/Os.
 * The addresses for the registers are arbitrary, they don't respect any real hardware
 * address, however, the fields correspond to the real ESP targets ones.
 * We can define here since they are (mostly) the same for all the supported atrgets (C3 and S3).
 * When porting this GDMA component to a new target, make SURE that these bitfields are valid!
 * If any of these bits is undefined/reserved on the target, make sure to mask it before
 * passing it to this generic GDMA component.
 */
REG32(GDMA_IN_CONF0, 0x000)
    FIELD(GDMA_IN_CONF0, MEM_TRANS_EN,  4, 1)
    FIELD(GDMA_IN_CONF0, DATA_BURST_EN, 3, 1)
    FIELD(GDMA_IN_CONF0, DSCR_BURST_EN, 2, 1)
    FIELD(GDMA_IN_CONF0, LOOP_TEST,     1, 1)
    FIELD(GDMA_IN_CONF0, RST,           0, 1)


REG32(GDMA_IN_CONF1, 0x000)
    FIELD(GDMA_IN_CONF1, EXT_MEM_BK_SIZE, 13, 2) // Reserved on C3
    FIELD(GDMA_IN_CONF1, CHECK_OWNER,     12, 1)
    FIELD(GDMA_IN_CONF1, FIFO_FULL_THRS,  0, 12) // Reserved on C3


REG32(GDMA_OUT_CONF0, 0x000)
    FIELD(GDMA_OUT_CONF0, DATA_BURST_EN, 5, 1)
    FIELD(GDMA_OUT_CONF0, DSCR_BURST_EN, 4, 1)
    FIELD(GDMA_OUT_CONF0, EOF_MODE,      3, 1)
    FIELD(GDMA_OUT_CONF0, AUTO_WRBACK,   2, 1)
    FIELD(GDMA_OUT_CONF0, LOOP_TEST,     1, 1)
    FIELD(GDMA_OUT_CONF0, RST,           0, 1)


REG32(GDMA_OUT_CONF1, 0x000)
    FIELD(GDMA_OUT_CONF1, EXT_MEM_BK_SIZE, 13, 2) // Reserved on C3
    FIELD(GDMA_OUT_CONF1, CHECK_OWNER,     12, 1)


REG32(GDMA_IN_LINK, 0x000)
    FIELD(GDMA_IN_LINK, PARK,     24, 1)
    FIELD(GDMA_IN_LINK, RESTART,  23, 1)
    FIELD(GDMA_IN_LINK, START,    22, 1)
    FIELD(GDMA_IN_LINK, STOP,     21, 1)
    FIELD(GDMA_IN_LINK, AUTO_RET, 20, 1)
    FIELD(GDMA_IN_LINK, ADDR,     0, 20)


REG32(GDMA_OUT_LINK, 0x000)
    FIELD(GDMA_OUT_LINK, PARK,    23, 1)
    FIELD(GDMA_OUT_LINK, RESTART, 22, 1)
    FIELD(GDMA_OUT_LINK, START,   21, 1)
    FIELD(GDMA_OUT_LINK, STOP,    20, 1)
    FIELD(GDMA_OUT_LINK, ADDR,    0, 20)


REG32(GDMA_INFIFO_STATUS, 0x000)
    FIELD(GDMA_INFIFO_STATUS, FIFO_EMPTY, 1, 1)
    

REG32(GDMA_OUT_STATE, 0x000)
    FIELD(GDMA_OUT_STATE, STATE,          20, 3)
    FIELD(GDMA_OUT_STATE, DSCR_STATE,     18, 2)
    FIELD(GDMA_OUT_STATE, LINK_DSCR_ADDR, 0, 18)


/* IN/OUT PERI registers have the same organization, define a common register */
REG32(GDMA_PERI_SEL, 0x000)
    FIELD(GDMA_PERI_SEL, PERI_SEL, 0, 6)


REG32(GDMA_MISC_CONF, 0x000)


REG32(GDMA_INTERRUPT, 0x000)
    FIELD(GDMA_INTERRUPT, IN_DSCR_EMPTY, 4, 1)
    FIELD(GDMA_INTERRUPT, IN_DSCR_ERR,   3, 1)
    FIELD(GDMA_INTERRUPT, IN_ERR_EOF,    2, 1)
    FIELD(GDMA_INTERRUPT, IN_SUC_EOF,    1, 1)
    FIELD(GDMA_INTERRUPT, IN_DONE,       0, 1)

    FIELD(GDMA_INTERRUPT, OUT_TOTAL_EOF,  3, 1)
    FIELD(GDMA_INTERRUPT, OUT_DSCR_ERR,   2, 1)
    FIELD(GDMA_INTERRUPT, OUT_EOF,        1, 1)
    FIELD(GDMA_INTERRUPT, OUT_DONE,       0, 1)
