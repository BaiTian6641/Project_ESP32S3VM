/*
 * ESP GDMA emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 *
 * Bounded descriptor walking, persistent descriptor + byte cursors,
 * channel error/halt states and peripheral handshake hooks added for the
 * ESP32S3VM project (docs/plans/peripherals-electrical.md, CORE-04).
 * Semantics follow the ESP32-S3 TRM chapter 3 ("GDMA Controller") and the
 * IDF GDMA HAL (components/esp_hal_dma/esp32s3/include/hal/gdma_ll.h).
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "sysemu/dma.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/dma/esp_gdma.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/xtensa/esp32s3_reset_domain.h"
#include "qemu/main-loop.h"

#define GDMA_WARNING 0
#define GDMA_DEBUG   0

/* Descriptor word 0 fields, TRM chapter 3 section "Linked List" */
#define GDMA_DESC_OWNER_MASK    (0x1u << 31)
#define GDMA_DESC_SUC_EOF_MASK  (0x1u << 30)
#define GDMA_DESC_ERR_EOF_MASK  (0x1u << 28)
#define GDMA_DESC_LENGTH_SHIFT  12
#define GDMA_DESC_LENGTH_MASK   0xfffu
#define GDMA_DESC_SIZE_MASK     0xfffu

/* Memory windows GDMA is allowed to move data to/from, TRM 3.4.8 and 3.4.9 */
#define GDMA_INTRAM_BASE        0x3FC88000u
#define GDMA_INTRAM_LAST        0x3FCFFFFFu
#define GDMA_EXTRAM_BASE        0x3C000000u
#define GDMA_EXTRAM_LAST        0x3DFFFFFFu

/* Largest single-descriptor payload, both size and length are 12-bit fields */
#define GDMA_DESC_MAX_BYTES     4095u


/**
 * @brief Structure defining how linked lists are represented in hardware for the GDMA module
 * (TRM chapter 3, section "Linked List": DW0 fields, DW1 buffer address, DW2 next descriptor).
 */
typedef struct GdmaLinkedList {
    union {
        struct {
            uint32_t size: 12;   // Size of the buffer (mainly used in a receive transaction)
            uint32_t length: 12; // Number of valid bytes in the buffer. In a transmit, written by software.
                                 // In receive, written by hardware.
            uint32_t rsvd_24: 4; // Reserved
            uint32_t err_eof: 1; // Set if received data has errors. Used with UHCI0 only.
            uint32_t rsvd_29: 1; // Reserved
            uint32_t suc_eof: 1; // Set if curent node is the last one (of the list). Set by software in a transmit transaction,
                                 // Set by the hardware in case of a receive transaction.
            uint32_t owner: 1;   // 0: CPU can access the buffer, 1: GDMA can access the buffer. Cleared automatically
                                 // by hardware in a transmit descriptor. In a receive descriptor, cleared by hardware
                                 // only if GDMA_OUT_AUTO_WRBACK_CHn is set to 1.
        };
        uint32_t val;
    } config;
    uint32_t buf_addr;
    uint32_t next_addr;
} GdmaLinkedList;

/**
 * @brief Zero-delay continuation state of a quantum-bounded M2M walk (one
 *        per channel, see ESP_GDMA_WORK_QUANTUM). A bottom half is used
 *        instead of a zero-delay timer: a scheduled BH runs at most once
 *        per main-loop iteration, so QMP/vCPU/chardev always get service
 *        between quanta even when the virtual clock is running (a
 *        self-re-armed zero-delay timer would be re-dispatched inside the
 *        same timerlist loop and starve the main loop).
 */
struct ESPGdmaQuantum {
    QEMUBH *bh;
    ESPGdmaState *s;
    uint32_t chan;
};


/**
 * @brief Check whether a data buffer address (descriptor DW1) points at a
 *        memory window the GDMA controller can access (TRM 3.4.6).
 */
static bool esp_gdma_data_addr_valid(uint32_t addr)
{
    return (addr >= GDMA_INTRAM_BASE && addr <= GDMA_INTRAM_LAST) ||
           (addr >= GDMA_EXTRAM_BASE && addr <= GDMA_EXTRAM_LAST);
}

/**
 * @brief Check whether a descriptor address is usable. Descriptors live in
 *        internal RAM only (TRM 3.4.6 note); address 0 means "end of list"
 *        and is handled by the callers.
 */
static bool esp_gdma_desc_addr_valid(uint32_t addr)
{
    return addr != 0 && addr >= GDMA_INTRAM_BASE && addr <= GDMA_INTRAM_LAST;
}

static bool esp_gdma_read_descr(ESPGdmaState *s, uint32_t addr, GdmaLinkedList* out)
{
    MemTxResult res = dma_memory_read(&s->dma_as, addr, out, sizeof(GdmaLinkedList), MEMTXATTRS_UNSPECIFIED);
    return res == MEMTX_OK;
}

static bool esp_gdma_write_descr(ESPGdmaState *s, uint32_t addr, GdmaLinkedList* in)
{
    MemTxResult res = dma_memory_write(&s->dma_as, addr, in, sizeof(GdmaLinkedList), MEMTXATTRS_UNSPECIFIED);
    return res == MEMTX_OK;
}

static bool esp_gdma_read_guest(ESPGdmaState *s, uint32_t addr, void* data, uint32_t len)
{
    MemTxResult res = dma_memory_read(&s->dma_as, addr, data, len, MEMTXATTRS_UNSPECIFIED);
    return res == MEMTX_OK;
}

static bool esp_gdma_write_guest(ESPGdmaState *s, uint32_t addr, void* data, uint32_t len)
{
    MemTxResult res = dma_memory_write(&s->dma_as, addr, data, len, MEMTXATTRS_UNSPECIFIED);
    return res == MEMTX_OK;
}

/**
 * @brief Check whether the new status of any interrupt should trigger an interrupt
 */
static void esp_gdma_check_interrupt_status(DmaIntState* int_st)
{
    const uint32_t former = int_st->st;

    /* Calculate the new status and check for any difference */
    int_st->st = int_st->raw & int_st->ena;

    if (former != int_st->st) {
        /* If all the status bits became low, lower the IRQ pin, else, raise it  */
        qemu_set_irq(int_st->irq, int_st->st ? 1 : 0);
    }
}

/**
 * @brief Set the status bit for the given channel. If the status triggers an interrupt, the corresponding
 * IRQ will be set.
*/
static void esp_gdma_set_status(DmaIntState* state, uint32_t mask)
{
    state->raw |= mask;
    esp_gdma_check_interrupt_status(state);
}

/**
 * @brief Clear the status bit for the given channel
*/
static void esp_gdma_clear_status(DmaIntState* state, uint32_t mask)
{
    state->raw &= ~mask;
    esp_gdma_check_interrupt_status(state);
}

/**
 * @brief Function called when a write to a channel interrupt register is performed
 */
static void esp_gdma_write_int_state(DmaIntState* state, DmaRegister reg, uint32_t value)
{
    switch (reg) {
        case GDMA_INT_ENA_REG:
            state->ena = value;
            break;

        case GDMA_INT_RAW_REG:
        case GDMA_INT_CLR_REG:
            /* Clear the bits that are set to 1, keep the remaining to their original value */
            state->raw &= ~value;
            break;

        default:
            /* Nothing to do, read-only register, return directly */
            return;
    }

    /* Update the status and check if any interrupt needs to occur */
    esp_gdma_check_interrupt_status(state);
}

/**
 * @brief Function called when a reset FIFO is requested
 */
static void esp_gdma_reset_fifo(DmaConfigState* s)
{
    /* Set the FIFO empty bit to 1, full bit to 0, and number of bytes of data to 0 */
    s->status = R_GDMA_INFIFO_STATUS_FIFO_EMPTY_MASK;
}

/**
 * @brief Halt a channel after a descriptor error (TRM 3.4.6): raise the
 *        direction DSCR_ERR interrupt and remember the offending descriptor
 *        address. The channel stays halted until it is reset or re-started.
 */
static void esp_gdma_desc_error(ESPGdmaState *s, uint32_t chan, uint32_t dir, uint32_t desc_addr)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];
    const uint32_t err_mask = (dir == ESP_GDMA_IN_IDX) ? R_GDMA_INTERRUPT_IN_DSCR_ERR_MASK
                                                       : R_GDMA_INTERRUPT_OUT_DSCR_ERR_MASK;

    st->halted = true;
    st->fsm_active = false;
    st->err_eof_desc_addr = desc_addr;
    esp_gdma_set_status(&st->int_state, err_mask);
}

/**
 * @brief Park the descriptor FSM of a direction (LINK.PARK reads 1 again).
 *        Parking is the normal end-of-list state and does not raise errors.
 */
static void esp_gdma_park(DmaConfigState* st)
{
    st->fsm_active = false;
}

/**
 * @brief Reset the descriptor FSM and cursors of one direction, TRM 3.6.2
 *        step 1 ("GDMA_x_RST resets the state machine of the channel and the
 *        FIFO pointer"). Interrupt registers are left untouched.
 */
static void esp_gdma_fsm_reset(DmaConfigState* st)
{
    st->cur_desc_addr = 0;
    st->cur_dw0 = 0;
    st->cur_buf_addr = 0;
    st->cur_next_addr = 0;
    st->cur_buf_off = 0;
    st->cur_buf_cap = 0;
    st->cur_suc_eof = false;
    st->rx_last_desc_addr = 0;
    st->rx_last_prev_addr = 0;
    st->rx_packet_ended = false;
    st->rx_segment_pending = false;
    st->walk_count = 0;
    st->idle_count = 0;
    st->fsm_active = false;
    st->halted = false;
    st->pending_start = false;
    st->pending_restart = false;
    st->state = 0;
    st->suc_eof_desc_addr = 0;
    st->err_eof_desc_addr = 0;
    st->desc_addr = 0;
    st->bfr_desc_addr = 0;
    st->bfr_bfr_desc_addr = 0;
    esp_gdma_reset_fifo(st);
}

/**
 * @brief Validate a fetched descriptor per TRM 3.4.6: owner bit (when
 *        GDMA_x_CHECK_OWNER is set), buffer address pointer (DW1) window,
 *        burst-mode alignment (TRM table 3.4-2/3.4-3) and the next
 *        descriptor address (DW2 must stay in internal RAM, 0 ends the list).
 */
static bool esp_gdma_desc_check(const DmaConfigState* st, uint32_t dir, const GdmaLinkedList* d)
{
    const bool owner_check = (dir == ESP_GDMA_IN_IDX)
        ? FIELD_EX32(st->conf1, GDMA_IN_CONF1, CHECK_OWNER)
        : FIELD_EX32(st->conf1, GDMA_OUT_CONF1, CHECK_OWNER);

    if (owner_check && !d->config.owner) {
        return false;
    }

    if (!esp_gdma_data_addr_valid(d->buf_addr)) {
        return false;
    }

    if (d->next_addr != 0 && !esp_gdma_desc_addr_valid(d->next_addr)) {
        return false;
    }

    /*
     * Burst alignment, TRM table 3.4-2 (internal RAM) and 3.4-3 (external
     * RAM): only receive (inlink) descriptors constrain size and buffer
     * address; transmit descriptors are never required to be aligned.
     * NOTE: the internal-RAM window (0x3FC88000..) lies numerically above
     * the external window base (0x3C000000), so the internal window must
     * be tested first.
     */
    if (dir == ESP_GDMA_IN_IDX) {
        const bool burst = FIELD_EX32(st->conf0, GDMA_IN_CONF0, DATA_BURST_EN) ||
                           FIELD_EX32(st->conf0, GDMA_IN_CONF0, DSCR_BURST_EN);
        if (burst) {
            const bool internal = d->buf_addr >= GDMA_INTRAM_BASE &&
                                  d->buf_addr <= GDMA_INTRAM_LAST;
            if (internal) {
                /* Internal RAM: size and buffer address word-aligned */
                if ((d->config.size & 3u) != 0 || (d->buf_addr & 3u) != 0) {
                    return false;
                }
            } else {
                /* External RAM: block-aligned per GDMA_IN_EXT_MEM_BK_SIZE
                 * (0 = 16 bytes, 1 = 32 bytes, 2 = 64 bytes) */
                const uint32_t bk = FIELD_EX32(st->conf1, GDMA_IN_CONF1, EXT_MEM_BK_SIZE);
                const uint32_t align = (bk > 2) ? 16u : (16u << bk);
                const uint32_t mask = align - 1u;
                if ((d->config.size & mask) != 0 || (d->buf_addr & mask) != 0) {
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * @brief Fetch a descriptor from guest memory and load it into the
 *        direction cursor. Also maintains the descriptor trail registers
 *        (GDMA_x_DSCR, DSCR_BF0, DSCR_BF1, STATE) with the semantics of
 *        TRM registers 3.24, 3.27..3.29 and 3.31/3.34..3.36.
 *
 * @returns true on success; on failure the channel is put in the DSCR_ERR
 *          halted state and false is returned.
 */
static bool esp_gdma_load_descriptor(ESPGdmaState *s, uint32_t chan, uint32_t dir, uint32_t addr)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];
    GdmaLinkedList d = { 0 };

    if (!esp_gdma_desc_addr_valid(addr) || !esp_gdma_read_descr(s, addr, &d) ||
        !esp_gdma_desc_check(st, dir, &d)) {
        esp_gdma_desc_error(s, chan, dir, addr);
        return false;
    }

    st->walk_count++;
    st->bfr_bfr_desc_addr = st->bfr_desc_addr;
    st->bfr_desc_addr = addr;
    st->desc_addr = d.next_addr;
    st->state = d.next_addr & R_GDMA_OUT_STATE_LINK_DSCR_ADDR_MASK;

    st->cur_desc_addr = addr;
    st->cur_dw0 = d.config.val;
    st->cur_buf_addr = d.buf_addr;
    st->cur_next_addr = d.next_addr;
    st->cur_buf_off = 0;
    st->cur_buf_cap = (dir == ESP_GDMA_OUT_IDX) ? d.config.length : d.config.size;
    st->cur_suc_eof = d.config.suc_eof;
    return true;
}

/**
 * @brief Bound check done before fetching the next descriptor of a chain.
 *
 * @returns true when the walk must stop (error flagged), false to proceed.
 */
static bool esp_gdma_walk_bound_hit(ESPGdmaState *s, uint32_t chan, uint32_t dir)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];

    if (st->walk_count >= ESP_GDMA_MAX_WALK_DESCRIPTORS) {
        /* Absolute safety valve: malformed cyclic chain */
        esp_gdma_desc_error(s, chan, dir, st->cur_next_addr);
        return true;
    }
    return false;
}

/**
 * @brief Non-progress detection: called after a walk step that moved no
 *        byte, rejects zero-length cycles (TRM lists where no data flows
 *        can never finish and are treated as descriptor errors).
 */
static bool esp_gdma_idle_bound_hit(ESPGdmaState *s, uint32_t chan, uint32_t dir)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];

    st->idle_count++;
    if (st->idle_count > ESP_GDMA_MAX_IDLE_DESCRIPTORS) {
        esp_gdma_desc_error(s, chan, dir, st->cur_desc_addr);
        return true;
    }
    return false;
}

/**
 * @brief Write a transmit descriptor back to guest memory. The owner bit is
 *        only cleared when GDMA_OUT_AUTO_WRBACK is set (TRM 3.4.1).
 */
static void esp_gdma_writeback_out(ESPGdmaState *s, uint32_t chan)
{
    DmaConfigState* st = &s->ch_conf[ESP_GDMA_OUT_IDX][chan];
    GdmaLinkedList d;

    if (!FIELD_EX32(st->conf0, GDMA_OUT_CONF0, AUTO_WRBACK)) {
        return;
    }

    d.config.val = st->cur_dw0 & ~GDMA_DESC_OWNER_MASK;
    d.buf_addr = st->cur_buf_addr;
    d.next_addr = st->cur_next_addr;

    if (!esp_gdma_write_descr(s, st->cur_desc_addr, &d)) {
        esp_gdma_desc_error(s, chan, ESP_GDMA_OUT_IDX, st->cur_desc_addr);
    }
}

/**
 * @brief Write a receive descriptor back to guest memory. The owner bit is
 *        always cleared by hardware (TRM 3.4.1), the length field receives
 *        the number of bytes actually stored, and suc_eof is set by
 *        hardware when the received data contained the EOF flag
 *        (@final_eof, TRM 3.4.1).
 */
static void esp_gdma_writeback_in(ESPGdmaState *s, uint32_t chan, bool final_eof)
{
    DmaConfigState* st = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    GdmaLinkedList d;

    d.config.val = st->cur_dw0 & ~(GDMA_DESC_OWNER_MASK | GDMA_DESC_SUC_EOF_MASK);
    d.config.length = st->cur_buf_off & GDMA_DESC_LENGTH_MASK;
    d.buf_addr = st->cur_buf_addr;
    d.next_addr = st->cur_next_addr;
    if (final_eof) {
        d.config.val |= GDMA_DESC_SUC_EOF_MASK;
    }

    if (!esp_gdma_write_descr(s, st->cur_desc_addr, &d)) {
        esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->cur_desc_addr);
    }
}

/**
 * @brief Restart address per TRM 3.4.5: hardware re-reads the last
 *        processed descriptor (DSCR_BF0) and takes its (possibly rewritten)
 *        DW2 as the address of the first descriptor of the new list.
 *
 * @returns 0 when no descriptor was ever processed (restart is a no-op).
 */
static uint32_t esp_gdma_restart_addr(ESPGdmaState *s, uint32_t chan, uint32_t dir)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];
    GdmaLinkedList d = { 0 };

    if (st->bfr_desc_addr == 0) {
        return 0;
    }
    if (!esp_gdma_read_descr(s, st->bfr_desc_addr, &d)) {
        esp_gdma_desc_error(s, chan, dir, st->bfr_desc_addr);
        return 0;
    }
    return d.next_addr;
}

void esp_gdma_set_peripheral_notify(ESPGdmaState *s, GdmaPeripheral periph,
                                    EspGdmaPeripheralNotify notify, void *opaque)
{
    if (!s || (unsigned)periph > GDMA_LAST) {
        return;
    }
    s->peripheral_notify[periph].notify = notify;
    s->peripheral_notify[periph].opaque = opaque;
}

static void esp_gdma_notify_arm(ESPGdmaState *s, uint32_t chan, uint32_t dir)
{
    unsigned periph = FIELD_EX32(s->ch_conf[dir][chan].peripheral,
                                 GDMA_PERI_SEL, PERI_SEL);
    if (periph <= GDMA_LAST &&
        !FIELD_EX32(s->ch_conf[ESP_GDMA_IN_IDX][chan].conf0,
                    GDMA_IN_CONF0, MEM_TRANS_EN) &&
        s->peripheral_notify[periph].notify) {
        s->peripheral_notify[periph].notify(s, periph, chan, dir,
                                           s->peripheral_notify[periph].opaque);
    }
}

/**
 * @brief Arm a chain after software wrote GDMA_xLINK (START or RESTART) and
 *        the channel is not in memory-to-memory mode. The first descriptor
 *        is fetched and checked immediately, so configuration errors are
 *        observable as soon as the channel is enabled (TRM 3.4.6).
 */
static void esp_gdma_arm_chain(ESPGdmaState *s, uint32_t chan, uint32_t dir, bool restart)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];
    uint32_t addr;

    if (st->halted) {
        /*
         * TRM 3.4.6: after a descriptor error, software must reset the
         * channel before enabling it again. A RESTART does not clear the
         * halted state, a fresh START does.
         */
        if (!restart) {
            esp_gdma_fsm_reset(st);
        } else {
            return;
        }
    }

    if (restart) {
        /*
         * If the walk stopped in the middle of a descriptor (work quantum
         * boundary or STOP), continue from the byte cursor instead of the
         * restart address; otherwise continue after the last processed
         * descriptor (TRM 3.4.5).
         */
        if (st->cur_desc_addr != 0 && st->cur_buf_off < st->cur_buf_cap) {
            st->fsm_active = true;
            st->walk_count = 0;
            st->idle_count = 0;
            esp_gdma_notify_arm(s, chan, dir);
            return;
        }
        addr = esp_gdma_restart_addr(s, chan, dir);
        if (addr == 0) {
            /* Nothing was ever processed, RESTART has nothing to continue */
            return;
        }
        st->walk_count = 0;
        st->idle_count = 0;
    } else {
        /* Fresh START: the 20-bit LINK.ADDR field is appended to the DRAM base */
        addr = (ESP_GDMA_RAM_ADDR & ~(R_GDMA_OUT_LINK_ADDR_MASK)) |
               FIELD_EX32(st->link, GDMA_OUT_LINK, ADDR);
        esp_gdma_fsm_reset(st);
    }

    if (dir == ESP_GDMA_IN_IDX) {
        st->rx_last_desc_addr = 0;
        st->rx_last_prev_addr = 0;
        st->rx_packet_ended = false;
        st->rx_segment_pending = false;
    }
    st->fsm_active = true;
    if (!esp_gdma_load_descriptor(s, chan, dir, addr)) {
        /* esp_gdma_load_descriptor flagged DSCR_ERR and halted the channel */
        return;
    }
    esp_gdma_notify_arm(s, chan, dir);
}

/**
 * @brief Serve a work-quantum slice of a memory-to-memory transfer.
 *
 * Both directions must be armed. The slice moves at most
 * ESP_GDMA_WORK_QUANTUM descriptor fetches; if the transfer is still
 * running afterwards it is rescheduled with a zero-delay timer so valid
 * streaming rings continue without blocking the host (CORE-04 "stop at a
 * scheduled quantum instead of looping forever").
 *
 * @param chan channel index
 * @param allow_reschedule when false, an unfinished transfer simply parks
 */
static void esp_gdma_m2m_slice(ESPGdmaState *s, uint32_t chan, bool allow_reschedule)
{
    DmaConfigState* st_in = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    DmaConfigState* st_out = &s->ch_conf[ESP_GDMA_OUT_IDX][chan];
    uint8_t chunk[GDMA_DESC_MAX_BYTES];
    uint32_t budget = ESP_GDMA_WORK_QUANTUM;
    bool error = false;
    bool done = false;

    /*
     * Walk bounds are per scheduled slice (and per pump call below), never
     * cumulative over the life of the chain: a valid EOF-less streaming
     * ring runs forever at quantum boundaries and must not trip the
     * absolute bound. The non-progress criterion (idle_count) is what
     * catches zero-length/malformed cycles.
     */
    st_in->walk_count = 0;
    st_out->walk_count = 0;
    st_in->idle_count = 0;
    st_out->idle_count = 0;

    while (!error && !done) {
        uint32_t out_avail = st_out->cur_buf_cap - st_out->cur_buf_off;
        uint32_t in_space = st_in->cur_buf_cap - st_in->cur_buf_off;
        uint32_t take = MIN(out_avail, in_space);

        if (take != 0) {
            if (!esp_gdma_read_guest(s, st_out->cur_buf_addr + st_out->cur_buf_off,
                                     chunk, take)) {
                esp_gdma_desc_error(s, chan, ESP_GDMA_OUT_IDX, st_out->cur_desc_addr);
                error = true;
                break;
            }
            if (!esp_gdma_write_guest(s, st_in->cur_buf_addr + st_in->cur_buf_off,
                                      chunk, take)) {
                esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st_in->cur_desc_addr);
                error = true;
                break;
            }
            st_out->cur_buf_off += take;
            st_in->cur_buf_off += take;
            st_out->idle_count = 0;
            st_in->idle_count = 0;
        }

        /* Close and advance the exhausted OUT descriptor first */
        if (st_out->cur_buf_off == st_out->cur_buf_cap) {
            const bool eof = st_out->cur_suc_eof;

            esp_gdma_writeback_out(s, chan);
            if (st_out->halted) {
                error = true;
                break;
            }
            if (eof) {
                /*
                 * TRM 3.5: OUT_EOF fires when the data of the EOF-flagged
                 * descriptor has been transmitted, OUT_TOTAL_EOF when the
                 * whole linked list has been sent.
                 */
                st_out->suc_eof_desc_addr = st_out->cur_desc_addr;
                /* OUT EOF_BFR_DES_ADDR: descriptor before the EOF one */
                st_out->err_eof_desc_addr = st_out->bfr_bfr_desc_addr;
                esp_gdma_set_status(&st_out->int_state, R_GDMA_INTERRUPT_OUT_EOF_MASK);
                if (st_out->cur_next_addr == 0) {
                    esp_gdma_set_status(&st_out->int_state,
                                        R_GDMA_INTERRUPT_OUT_TOTAL_EOF_MASK);
                }
                done = true;
                break;
            }
            if (st_out->cur_next_addr == 0) {
                /*
                 * End of the outlink without EOF flag: the channel parks
                 * and software may append descriptors by rewriting DW2 of
                 * the last descriptor and issuing a RESTART (TRM 3.4.5).
                 */
                esp_gdma_park(st_out);
                esp_gdma_park(st_in);
                break;
            }
            if (esp_gdma_walk_bound_hit(s, chan, ESP_GDMA_OUT_IDX)) {
                error = true;
                break;
            }
            if (take == 0 && esp_gdma_idle_bound_hit(s, chan, ESP_GDMA_OUT_IDX)) {
                error = true;
                break;
            }
            if (!esp_gdma_load_descriptor(s, chan, ESP_GDMA_OUT_IDX, st_out->cur_next_addr)) {
                error = true;
                break;
            }
        }

        /* Then close and advance the full IN descriptor */
        if (st_in->cur_buf_off == st_in->cur_buf_cap) {
            if (st_in->cur_suc_eof) {
                esp_gdma_set_status(&st_in->int_state, R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);
            }
            esp_gdma_writeback_in(s, chan, false);
            if (st_in->halted) {
                error = true;
                break;
            }
            if (st_in->cur_next_addr == 0) {
                /*
                 * TRM 3.5: the receive buffers could not hold all the data
                 * and there is no more inlink: IN_DSCR_EMPTY, the transfer
                 * parks unfinished.
                 */
                esp_gdma_set_status(&st_in->int_state, R_GDMA_INTERRUPT_IN_DSCR_EMPTY_MASK);
                esp_gdma_park(st_in);
                esp_gdma_park(st_out);
                break;
            }
            if (esp_gdma_walk_bound_hit(s, chan, ESP_GDMA_IN_IDX)) {
                error = true;
                break;
            }
            if (take == 0 && esp_gdma_idle_bound_hit(s, chan, ESP_GDMA_IN_IDX)) {
                error = true;
                break;
            }
            if (!esp_gdma_load_descriptor(s, chan, ESP_GDMA_IN_IDX, st_in->cur_next_addr)) {
                error = true;
                break;
            }
        }

        if (--budget == 0) {
            /* Scheduled quantum boundary: cursors stay where they are */
            if (allow_reschedule) {
                ESPGdmaQuantum* q = &s->quantum[chan];
                qemu_bh_schedule(q->bh);
            }
            return;
        }
    }

    if (done && !st_in->halted && !st_out->halted) {
        /* Final receive writeback: hardware sets suc_eof on the last buffer */
        esp_gdma_writeback_in(s, chan, true);
        if (!st_in->halted) {
            st_in->suc_eof_desc_addr = st_in->cur_desc_addr;
            esp_gdma_park(st_in);
            esp_gdma_park(st_out);
            esp_gdma_set_status(&st_in->int_state,
                                R_GDMA_INTERRUPT_IN_DONE_MASK |
                                R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);
            esp_gdma_set_status(&st_out->int_state,
                                R_GDMA_INTERRUPT_OUT_DONE_MASK);
        }
    }
    /* else: parked or halted, nothing else to do */
}

/**
 * @brief Zero-delay timer callback continuing a quantum-bounded M2M walk.
 */
static void esp_gdma_quantum_cb(void *opaque)
{
    ESPGdmaQuantum* q = opaque;
    esp_gdma_m2m_slice(q->s, q->chan, true);
}

/**
 * @brief Stop a direction: data stops flowing, cursors are kept so a
 *        RESTART can continue the same chain (TRM: GDMA_OUTLINK_STOP).
 */
static void esp_gdma_stop_dir(ESPGdmaState *s, uint32_t chan, uint32_t dir)
{
    DmaConfigState* st = &s->ch_conf[dir][chan];
    esp_gdma_park(st);
    st->pending_start = false;
    st->pending_restart = false;
}

/**
 * @brief Disarm both directions of a channel and cancel its quantum timer.
 *        Used by RST (either direction) so a half-reset channel never keeps
 *        a quantum-bounded transfer running.
 */
static void esp_gdma_channel_disarm(ESPGdmaState *s, uint32_t chan)
{
    ESPGdmaQuantum* q = &s->quantum[chan];

    qemu_bh_cancel(q->bh);
    for (uint32_t dir = 0; dir < ESP_GDMA_CONF_COUNT; dir++) {
        DmaConfigState* st = &s->ch_conf[dir][chan];
        esp_gdma_park(st);
        st->pending_start = false;
        st->pending_restart = false;
        st->rx_last_desc_addr = 0;
        st->rx_last_prev_addr = 0;
        st->rx_packet_ended = false;
        st->rx_segment_pending = false;
    }
}

/**
 * @brief Check whether a memory-to-memory transfer can be started and start
 *        it if possible (TRM 3.4.3: MEM_TRANS_EN connects OUT n to IN n).
 *        Otherwise, service the pending START/RESTART of the non-M2M
 *        direction as a peripheral handshake chain.
 */
static void esp_gdma_try_transfer(ESPGdmaState *s, uint32_t chan)
{
    DmaConfigState* st_in = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    DmaConfigState* st_out = &s->ch_conf[ESP_GDMA_OUT_IDX][chan];
    const bool in_pending = st_in->pending_start || st_in->pending_restart;
    const bool out_pending = st_out->pending_start || st_out->pending_restart;
    const bool m2m = FIELD_EX32(st_in->conf0, GDMA_IN_CONF0, MEM_TRANS_EN);

    if (m2m) {
        if (in_pending && out_pending) {
            const bool in_restart = !st_in->pending_start && st_in->pending_restart;
            const bool out_restart = !st_out->pending_start && st_out->pending_restart;

            st_in->pending_start = false;
            st_in->pending_restart = false;
            st_out->pending_start = false;
            st_out->pending_restart = false;

            /* Clear DONE/EOF statuses of the previous transfer */
            esp_gdma_clear_status(&st_in->int_state,
                                  R_GDMA_INTERRUPT_IN_DONE_MASK |
                                  R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);
            esp_gdma_clear_status(&st_out->int_state,
                                  R_GDMA_INTERRUPT_OUT_DONE_MASK |
                                  R_GDMA_INTERRUPT_OUT_EOF_MASK |
                                  R_GDMA_INTERRUPT_OUT_TOTAL_EOF_MASK);

            /* Arm both directions (fresh START or RESTART, TRM 3.6.4) */
            esp_gdma_arm_chain(s, chan, ESP_GDMA_OUT_IDX, out_restart);
            if (!st_out->halted) {
                esp_gdma_arm_chain(s, chan, ESP_GDMA_IN_IDX, in_restart);
            }
            if (st_in->fsm_active && st_out->fsm_active) {
                esp_gdma_m2m_slice(s, chan, true);
            }
        }
        return;
    }

    /* Peripheral handshake chains: the pump is driven by esp_gdma_read/write_channel */
    if (st_in->pending_start) {
        st_in->pending_start = false;
        st_in->pending_restart = false;
        esp_gdma_arm_chain(s, chan, ESP_GDMA_IN_IDX, false);
    } else if (st_in->pending_restart) {
        st_in->pending_restart = false;
        esp_gdma_arm_chain(s, chan, ESP_GDMA_IN_IDX, true);
    }
    if (st_out->pending_start) {
        st_out->pending_start = false;
        st_out->pending_restart = false;
        esp_gdma_arm_chain(s, chan, ESP_GDMA_OUT_IDX, false);
    } else if (st_out->pending_restart) {
        st_out->pending_restart = false;
        esp_gdma_arm_chain(s, chan, ESP_GDMA_OUT_IDX, true);
    }
}

/**
 * @brief Function called when a writable configuration register is being written to.
 */
static void esp_gdma_write_chan_conf(ESPGdmaState *state, uint32_t dir, uint32_t chan,
                                     DmaRegister reg, uint32_t value)
{
    DmaConfigState* s = &state->ch_conf[dir][chan];

    switch(reg) {

        case GDMA_CONF0_REG: {
            /* Check the reset bit, reset the FSM on the 1 -> 0 edge (TRM 3.6.2) */
            if (FIELD_EX32(value,  GDMA_IN_CONF0, RST) == 0 &&
                FIELD_EX32(s->conf0, GDMA_IN_CONF0, RST) != 0)
            {
                esp_gdma_channel_disarm(state, chan);
                esp_gdma_fsm_reset(s);
            }
            /* Update the register before going further */
            s->conf0 = value;
            /* A memory transfer may have just been enabled (only on IN channels) */
            if (dir == ESP_GDMA_IN_IDX &&
                FIELD_EX32(value, GDMA_IN_CONF0, MEM_TRANS_EN))
            {
                esp_gdma_try_transfer(state, chan);
            }
            break;
        }

        case GDMA_LINK_REG: {
            const uint32_t start_mask = (dir == ESP_GDMA_IN_IDX) ? R_GDMA_IN_LINK_START_MASK
                                                                 : R_GDMA_OUT_LINK_START_MASK;
            const uint32_t restart_mask = (dir == ESP_GDMA_IN_IDX) ? R_GDMA_IN_LINK_RESTART_MASK
                                                                   : R_GDMA_OUT_LINK_RESTART_MASK;
            const uint32_t stop_mask = (dir == ESP_GDMA_IN_IDX) ? R_GDMA_IN_LINK_STOP_MASK
                                                                : R_GDMA_OUT_LINK_STOP_MASK;

            /*
             * START/RESTART/STOP are self-clearing and PARK is computed
             * from the FSM state on read, so only the address (and the
             * INLINK AUTO_RET control bit) are stored.
             */
            const uint32_t keep_mask = (dir == ESP_GDMA_IN_IDX)
                ? (R_GDMA_OUT_LINK_ADDR_MASK | R_GDMA_IN_LINK_AUTO_RET_MASK)
                : R_GDMA_OUT_LINK_ADDR_MASK;
            s->link = value & keep_mask;

            if (value & stop_mask) {
                esp_gdma_stop_dir(state, chan, dir);
            }
            if (value & start_mask) {
                s->pending_start = true;
                s->pending_restart = false;
            } else if (value & restart_mask) {
                s->pending_restart = true;
            }
            if ((value & start_mask) || (value & restart_mask)) {
                esp_gdma_try_transfer(state, chan);
            }
            break;
        }

        case GDMA_CONF1_REG:
            s->conf1 = value;
            break;
        case GDMA_POP_REG:
            s->push_pop = value;
            break;
        case GDMA_PRIORITY_REG:
            s->priority = value;
            break;
        case GDMA_PERI_SEL_REG:
            /* GDMA_x_PERI_SEL is a 6-bit field, reserved bits read as zero */
            s->peripheral = value & R_GDMA_PERI_SEL_PERI_SEL_MASK;
            break;

        default:
            break;
    }
}

/**
 * @brief Write a virtual register of a channel. This function can be called by the child classes.
 */
void esp_gdma_write_chan_register(ESPGdmaState* s, uint32_t dir, uint32_t chan, DmaRegister reg, uint32_t value)
{
    ESPGdmaClass *class = ESP_GDMA_GET_CLASS(s);
    assert(s != NULL && chan < class->m_channel_count && dir < ESP_GDMA_CONF_COUNT);

    switch (reg) {
        /* Interrupt related */
        case GDMA_INT_RAW_REG:
        case GDMA_INT_ENA_REG:
        case GDMA_INT_CLR_REG:
            esp_gdma_write_int_state(&s->ch_conf[dir][chan].int_state, reg, value);
            break;

        /* Configuration related */
        case GDMA_CONF0_REG:
        case GDMA_CONF1_REG:
        case GDMA_POP_REG:
        case GDMA_LINK_REG:
        case GDMA_PRIORITY_REG:
        case GDMA_PERI_SEL_REG:
            esp_gdma_write_chan_conf(s, dir, chan, reg, value);
            break;

        default:
            /* RO registers or invalid register */
            break;
    }
}

void esp_gdma_write_register(ESPGdmaState* s, DmaRegister reg, uint32_t value)
{
    if (reg == GDMA_MISC_REG) {
        s->misc_conf = value;
    }
}

/**
 * @brief Read a virtual register of a channel. This function can be called by the child classes.
 */
uint64_t esp_gdma_read_chan_register(ESPGdmaState* state, uint32_t dir, uint32_t chan, DmaRegister reg)
{
    ESPGdmaClass *class = ESP_GDMA_GET_CLASS(state);
    assert(state != NULL && chan < class->m_channel_count && dir < ESP_GDMA_CONF_COUNT);
    /* In theory, we can simply cast the DmaConfigState structure into a `uint32_t` array, but let's make
     * it modular and not bound to any hardware representation. */
    const DmaConfigState* s = &state->ch_conf[dir][chan];

    switch (reg) {
        /* Interrupt related */
        case GDMA_INT_RAW_REG:          return s->int_state.raw;
        case GDMA_INT_ST_REG:           return s->int_state.st;
        case GDMA_INT_ENA_REG:          return s->int_state.ena;

        /* Configuration related */
        case GDMA_CONF0_REG:            return s->conf0;
        case GDMA_CONF1_REG:            return s->conf1;
        case GDMA_FIFO_ST_REG:          return s->status;
        case GDMA_POP_REG:              return s->push_pop;
        case GDMA_LINK_REG: {
            /*
             * GDMA_xLINK_PARK reads 1 while the descriptor FSM is idle,
             * 0 while a chain is armed and being served (TRM registers
             * 3.4 and 3.8; reset value of PARK is 1).
             */
            const uint32_t park_mask = (dir == ESP_GDMA_IN_IDX) ? R_GDMA_IN_LINK_PARK_MASK
                                                                : R_GDMA_OUT_LINK_PARK_MASK;
            return (s->link & ~park_mask) | (s->fsm_active ? 0 : park_mask);
        }
        case GDMA_STATE_REG:            return s->state;
        case GDMA_SUC_EOF_DESC_REG:     return s->suc_eof_desc_addr;
        case GDMA_ERR_EOF_DESC_REG:     return s->err_eof_desc_addr;
        case GDMA_DESC_ADDR_REG:        return s->desc_addr;
        case GDMA_BF0_DESC_ADDR_REG:    return s->bfr_desc_addr;
        case GDMA_BF1_DESC_ADDR_REG:    return s->bfr_bfr_desc_addr;
        case GDMA_PRIORITY_REG:         return s->priority;
        case GDMA_PERI_SEL_REG:         return s->peripheral;
        default:
            /* WO registers or invalid register */
            return 0;
    }
}

/**
 * @brief Read a virtual register that is NOT part of a GDMA channel.
 */
uint64_t esp_gdma_read_register(ESPGdmaState* s, DmaRegister reg)
{
    uint64_t r = 0;

    if (reg == GDMA_MISC_REG) {
        r = s->misc_conf;
    }

    return r;
}

/**
 * Check the header file for more info about this function
 */
bool esp_gdma_get_channel_periph(ESPGdmaState *s, GdmaPeripheral periph, int dir, uint32_t* chan)
{
    const ESPGdmaClass* class = ESP_GDMA_GET_CLASS(s);

    /* If the state, the peripheral or the direction is invalid, return directly */
    if (s == NULL || chan == NULL || periph > GDMA_LAST || dir < 0 || dir >= ESP_GDMA_CONF_COUNT ||
        (class->is_periph_invalid && class->is_periph_invalid(s, periph)))
    {
        return false;
    }

    /*
     * Look for the channel of the given direction that is both configured
     * for this peripheral (GDMA_x_PERI_SEL) and has an armed chain waiting
     * to be serviced. A channel configured for another peripheral can never
     * be consumed, even if it is started (CORE-04).
     */
    for (int i = 0; i < class->m_channel_count; i++) {
        if (FIELD_EX32(s->ch_conf[dir][i].peripheral, GDMA_PERI_SEL, PERI_SEL) == periph &&
            s->ch_conf[dir][i].fsm_active && !s->ch_conf[dir][i].halted) {
            *chan = i;
            return true;
        }
    }

    return false;
}

/**
 * @brief Serve a transmit (memory -> peripheral) handshake: pull up to
 *        `size` bytes out of the guest outlink into `buffer`, continuing
 *        from the persistent descriptor + byte cursor.
 */
bool esp_gdma_read_channel_ex(ESPGdmaState *s, uint32_t chan, uint8_t *buffer,
                              uint32_t size, EspGdmaTxInfo *info)
{
    if (info) {
        memset(info, 0, sizeof(*info));
    }
    if (!s || !buffer || chan >= ESP_GDMA_GET_CLASS(s)->m_channel_count) {
        return false;
    }

    DmaConfigState* st = &s->ch_conf[ESP_GDMA_OUT_IDX][chan];
    if (size == 0) {
        return true;
    }
    if (!st->fsm_active || st->halted) {
        /* No armed outlink to serve: the peripheral must not invent data */
        return false;
    }

    /* Bounds are per pump call (see esp_gdma_m2m_slice) */
    st->walk_count = 0;
    st->idle_count = 0;

    uint32_t consumed = 0;
    while (consumed < size && !st->halted) {
        const uint32_t avail = st->cur_buf_cap - st->cur_buf_off;
        const uint32_t take = MIN(avail, size - consumed);

        if (take != 0) {
            if (!esp_gdma_read_guest(s, st->cur_buf_addr + st->cur_buf_off,
                                     buffer + consumed, take)) {
                esp_gdma_desc_error(s, chan, ESP_GDMA_OUT_IDX, st->cur_desc_addr);
                break;
            }
            st->cur_buf_off += take;
            consumed += take;
            if (info) {
                info->served = consumed;
            }
            st->idle_count = 0;
        }

        if (st->cur_buf_off < st->cur_buf_cap) {
            /* Request satisfied in the middle of the descriptor, cursor kept */
            break;
        }

        /* Descriptor exhausted: close it, honor its EOF flag, advance */
        const bool eof = st->cur_suc_eof;
        if (info && eof) {
            info->eof_reached = true;
        }
        esp_gdma_writeback_out(s, chan);
        if (st->halted) {
            break;
        }
        if (eof) {
            st->suc_eof_desc_addr = st->cur_desc_addr;
            st->err_eof_desc_addr = st->bfr_bfr_desc_addr;
            esp_gdma_set_status(&st->int_state, R_GDMA_INTERRUPT_OUT_EOF_MASK);
            if (st->cur_next_addr == 0) {
                esp_gdma_set_status(&st->int_state,
                                    R_GDMA_INTERRUPT_OUT_TOTAL_EOF_MASK);
            }
        }
        if (st->cur_next_addr == 0) {
            /* Outlink exhausted: park. The caller sees a short transfer. */
            esp_gdma_park(st);
            if (info) {
                info->list_done = true;
            }
            break;
        }
        if (esp_gdma_walk_bound_hit(s, chan, ESP_GDMA_OUT_IDX)) {
            break;
        }
        if (take == 0 && esp_gdma_idle_bound_hit(s, chan, ESP_GDMA_OUT_IDX)) {
            break;
        }
        if (!esp_gdma_load_descriptor(s, chan, ESP_GDMA_OUT_IDX, st->cur_next_addr)) {
            break;
        }
        if (info && eof) {
            break;
        }
    }

    if (consumed == size) {
        /*
         * Fulfilled requests stay fulfilled: bytes already pulled into the
         * consumer buffer are not un-served by a next-descriptor fault,
         * and OUT_DONE reflects the completed transfer.
         */
        esp_gdma_set_status(&st->int_state, R_GDMA_INTERRUPT_OUT_DONE_MASK);
    }

    return consumed == size;
}

bool esp_gdma_read_channel(ESPGdmaState *s, uint32_t chan,
                           uint8_t *buffer, uint32_t size)
{
    return esp_gdma_read_channel_ex(s, chan, buffer, size, NULL);
}

/**
 * @brief Serve a receive (peripheral -> memory) handshake: push up to
 *        `size` bytes from `buffer` into the guest inlink, continuing from
 *        the persistent descriptor + byte cursor.
 */
bool esp_gdma_write_channel_ex(ESPGdmaState *s, uint32_t chan, uint8_t *buffer,
                               uint32_t size, uint32_t *served)
{
    if (served) {
        *served = 0;
    }
    if (!s || !buffer || chan >= ESP_GDMA_GET_CLASS(s)->m_channel_count) {
        return false;
    }

    DmaConfigState* st = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    if (size == 0) {
        return true;
    }
    if (!st->fsm_active || st->halted) {
        /* No armed inlink to fill: do not fabricate guest memory content */
        return false;
    }

    /* Bounds are per pump call (see esp_gdma_m2m_slice) */
    st->walk_count = 0;
    st->idle_count = 0;

    uint32_t consumed = 0;
    while (consumed < size && !st->halted) {
        const uint32_t space = st->cur_buf_cap - st->cur_buf_off;
        const uint32_t take = MIN(space, size - consumed);

        if (take != 0) {
            if (!esp_gdma_write_guest(s, st->cur_buf_addr + st->cur_buf_off,
                                      buffer + consumed, take)) {
                esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->cur_desc_addr);
                break;
            }
            st->cur_buf_off += take;
            consumed += take;
            if (served) {
                *served = consumed;
            }
            st->rx_last_desc_addr = st->cur_desc_addr;
            st->rx_last_prev_addr = st->bfr_bfr_desc_addr;
            st->rx_segment_pending = true;
            st->idle_count = 0;
        }

        if (st->cur_buf_off < st->cur_buf_cap) {
            /* Descriptor partially filled, cursor kept for the next pump */
            break;
        }

        /* Descriptor full: write it back with the received length */
        if (st->cur_suc_eof) {
            esp_gdma_set_status(&st->int_state, R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);
        }
        esp_gdma_writeback_in(s, chan, false);
        if (st->halted) {
            /*
             * The received bytes are already stored in guest memory: a
             * fault while advancing to the NEXT descriptor must not
             * un-serve them. The channel is flagged DSCR_ERR; the caller
             * still sees the stored byte count.
             */
            break;
        }
        if (st->cur_next_addr == 0) {
            /*
             * No more inlink while the peripheral still has data: TRM 3.5
             * GDMA_IN_DSCR_EMPTY ("receiving data is not completed").
             * When the request was fulfilled exactly, the transfer is
             * complete and no DSCR_EMPTY is raised. The channel parks,
             * the caller sees a short transfer only if data remains.
             */
            if (consumed < size) {
                esp_gdma_set_status(&st->int_state,
                                    R_GDMA_INTERRUPT_IN_DSCR_EMPTY_MASK);
            }
            esp_gdma_park(st);
            break;
        }
        if (esp_gdma_walk_bound_hit(s, chan, ESP_GDMA_IN_IDX)) {
            break;
        }
        if (take == 0 && esp_gdma_idle_bound_hit(s, chan, ESP_GDMA_IN_IDX)) {
            break;
        }
        if (!esp_gdma_load_descriptor(s, chan, ESP_GDMA_IN_IDX, st->cur_next_addr)) {
            break;
        }
    }

    if (consumed == size) {
        esp_gdma_set_status(&st->int_state, R_GDMA_INTERRUPT_IN_DONE_MASK);
    }

    return consumed == size;
}

bool esp_gdma_write_channel(ESPGdmaState *s, uint32_t chan,
                            uint8_t *buffer, uint32_t size)
{
    return esp_gdma_write_channel_ex(s, chan, buffer, size, NULL);
}

static bool esp_gdma_finish_rx_common(ESPGdmaState *s, uint32_t chan, bool error)
{
    if (!s || chan >= ESP_GDMA_GET_CLASS(s)->m_channel_count) {
        return false;
    }
    DmaConfigState *st = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    if (error && FIELD_EX32(st->peripheral, GDMA_PERI_SEL, PERI_SEL) != GDMA_UHCI0) {
        return false;
    }
    if (st->halted || !st->rx_last_desc_addr) {
        return false;
    }
    if (st->rx_packet_ended) {
        return true;
    }
    if (st->cur_desc_addr == st->rx_last_desc_addr &&
        st->cur_buf_off > 0 && st->cur_buf_off < st->cur_buf_cap) {
        st->cur_dw0 &= ~GDMA_DESC_ERR_EOF_MASK;
        if (error) {
            st->cur_dw0 |= GDMA_DESC_ERR_EOF_MASK;
        }
        esp_gdma_writeback_in(s, chan, true);
        if (st->halted) {
            return false;
        }
        /* RESTART follows DW2 after terminal EOF, not unused allocation. */
        st->cur_buf_off = st->cur_buf_cap;
    } else {
        /* Never mark the preloaded, empty next descriptor as packet EOF. */
        GdmaLinkedList d;
        if (!esp_gdma_read_descr(s, st->rx_last_desc_addr, &d)) {
            esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->rx_last_desc_addr);
            return false;
        }
        d.config.val &= ~GDMA_DESC_ERR_EOF_MASK;
        if (error) {
            d.config.val |= GDMA_DESC_ERR_EOF_MASK;
        }
        d.config.val |= GDMA_DESC_SUC_EOF_MASK;
        if (!esp_gdma_write_descr(s, st->rx_last_desc_addr, &d)) {
            esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->rx_last_desc_addr);
            return false;
        }
        st->cur_desc_addr = st->rx_last_desc_addr;
        st->cur_dw0 = d.config.val;
        st->cur_buf_addr = d.buf_addr;
        st->cur_next_addr = d.next_addr;
        st->cur_buf_cap = st->cur_buf_off = d.config.length;
        st->bfr_desc_addr = st->rx_last_desc_addr;
        st->bfr_bfr_desc_addr = st->rx_last_prev_addr;
        st->desc_addr = d.next_addr;
        st->state = d.next_addr & R_GDMA_OUT_STATE_LINK_DSCR_ADDR_MASK;
    }
    st->rx_packet_ended = true;
    st->rx_segment_pending = false;
    st->suc_eof_desc_addr = st->rx_last_desc_addr;
    if (error) {
        st->err_eof_desc_addr = st->rx_last_desc_addr;
    }
    esp_gdma_park(st);
    esp_gdma_set_status(&st->int_state,
                        R_GDMA_INTERRUPT_IN_DONE_MASK |
                        R_GDMA_INTERRUPT_IN_SUC_EOF_MASK |
                        (error ? R_GDMA_INTERRUPT_IN_ERR_EOF_MASK : 0));
    return true;
}

bool esp_gdma_finish_rx_channel(ESPGdmaState *s, uint32_t chan)
{
    return esp_gdma_finish_rx_common(s, chan, false);
}

bool esp_gdma_finish_rx_channel_err(ESPGdmaState *s, uint32_t chan)
{
    return esp_gdma_finish_rx_common(s, chan, true);
}

bool esp_gdma_finish_rx_segment(ESPGdmaState *s, uint32_t chan)
{
    if (!s || chan >= ESP_GDMA_GET_CLASS(s)->m_channel_count) {
        return false;
    }
    DmaConfigState *st = &s->ch_conf[ESP_GDMA_IN_IDX][chan];
    if (st->halted || !st->rx_segment_pending || !st->rx_last_desc_addr) {
        return false;
    }
    bool partial = st->cur_desc_addr == st->rx_last_desc_addr &&
                   st->cur_buf_off > 0 && st->cur_buf_off < st->cur_buf_cap;
    if (partial) {
        st->cur_dw0 &= ~GDMA_DESC_ERR_EOF_MASK;
        esp_gdma_writeback_in(s, chan, true);
        if (st->halted) {
            return false;
        }
    } else {
        /* The ordinary pump already closed the full descriptor and may have
         * prefetched DW2. Mark the last actual descriptor, never that empty
         * next allocation. Do not rewind the live byte/descriptor cursor. */
        GdmaLinkedList d;
        if (!esp_gdma_read_descr(s, st->rx_last_desc_addr, &d)) {
            esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->rx_last_desc_addr);
            return false;
        }
        d.config.val = (d.config.val & ~GDMA_DESC_ERR_EOF_MASK) |
                       GDMA_DESC_SUC_EOF_MASK;
        if (!esp_gdma_write_descr(s, st->rx_last_desc_addr, &d)) {
            esp_gdma_desc_error(s, chan, ESP_GDMA_IN_IDX, st->rx_last_desc_addr);
            return false;
        }
    }
    st->rx_segment_pending = false;
    st->suc_eof_desc_addr = st->rx_last_desc_addr;
    esp_gdma_set_status(&st->int_state, R_GDMA_INTERRUPT_IN_SUC_EOF_MASK);
    if (partial) {
        /* A segment consumes this allocation even if its unused tail remains.
         * RESTART after an actual end-of-list must therefore follow DW2. */
        st->cur_buf_off = st->cur_buf_cap;
        if (!st->cur_next_addr) {
            esp_gdma_park(st);
        } else {
            st->walk_count = st->idle_count = 0;
            if (!esp_gdma_walk_bound_hit(s, chan, ESP_GDMA_IN_IDX)) {
                esp_gdma_load_descriptor(s, chan, ESP_GDMA_IN_IDX, st->cur_next_addr);
            }
        }
    }
    return true;
}

static Property esp_gdma_properties[] = {
    DEFINE_PROP_LINK("soc_mr", ESPGdmaState, soc_mr, TYPE_MEMORY_REGION, MemoryRegion*),
    DEFINE_PROP_LINK("soc-reset", ESPGdmaState, soc_reset, TYPE_DEVICE, DeviceState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp_gdma_reset_hold(Object *obj, ResetType type)
{
    ESPGdmaState *s = ESP_GDMA(obj);
    ESPGdmaClass *klass = ESP_GDMA_GET_CLASS(obj);

    /* PERIPH reset domain: CPU-only resets leave the GDMA channel state */
    if (s->soc_reset && !esp32s3_reset_covers_periph(s->soc_reset)) {
        return;
    }

    for (int dir = 0; dir < ESP_GDMA_CONF_COUNT; dir++) {
        for (int chan = 0; chan < klass->m_channel_count; chan++) {
            DmaConfigState* config = &s->ch_conf[dir][chan];
            /* Backup IRQ since it's going to be erased by the `memset` */
            const qemu_irq irq = config->int_state.irq;
            memset(config, 0, sizeof(DmaConfigState));
            /* Lower the IRQ and restore it in the configuration structure */
            qemu_irq_lower(irq);
            config->int_state.irq = irq;
            /*
             * TRM register reset values: GDMA_INLINK_PARK and
             * GDMA_INLINK_AUTO_RET reset to 1, GDMA_OUTLINK_PARK to 1.
             * PARK is computed from the FSM state on read, only AUTO_RET
             * needs storing.
             */
            if (dir == ESP_GDMA_IN_IDX) {
                config->link = R_GDMA_IN_LINK_AUTO_RET_MASK;
            }
            esp_gdma_reset_fifo(config);
        }
    }

    if (s->quantum != NULL) {
        for (uint32_t chan = 0; chan < klass->m_channel_count; chan++) {
            qemu_bh_cancel(s->quantum[chan].bh);
        }
    }

    s->misc_conf = 0;
}

static void esp_gdma_realize(DeviceState *dev, Error **errp)
{
    ESPGdmaState *s = ESP_GDMA(dev);

    /* Make sure the DRAM MemoryRegion was set */
    assert(s->soc_mr != NULL);

    address_space_init(&s->dma_as, s->soc_mr, "esp.gdma");
}

static void esp_gdma_init(Object *obj)
{
    ESPGdmaState *s = ESP_GDMA(obj);
    ESPGdmaClass *klass = ESP_GDMA_GET_CLASS(obj);

    /* Make sure the number of channels passed by the child class is correct */
    if (klass->m_channel_count == 0 || klass->m_channel_count > ESP_GDMA_MAX_CHANNELS) {
        error_report("[GDMA] %s: invalid number of DMA channels (%zu)", __func__, klass->m_channel_count);
    }

    /* Initialize the DmaConfigState arrays */
    for (int dir = 0; dir < ESP_GDMA_CONF_COUNT; dir++) {
        s->ch_conf[dir] = g_malloc(sizeof(DmaConfigState) * klass->m_channel_count);
        if (s->ch_conf[dir] == NULL) {
            error_report("[GDMA] %s: could not allocate DmaConfigState", __func__);
        }
        const char* name = (dir == ESP_GDMA_OUT_IDX) ? ESP_GDMA_IRQ_OUT_NAME : ESP_GDMA_IRQ_IN_NAME;

        for (int chan = 0; chan < klass->m_channel_count; chan++) {
            qdev_init_gpio_out_named(DEVICE(obj), &s->ch_conf[dir][chan].int_state.irq, name, 1);
        }
    }

    /* One work-quantum bottom half per channel */
    s->quantum = g_new0(ESPGdmaQuantum, klass->m_channel_count);
    for (uint32_t chan = 0; chan < klass->m_channel_count; chan++) {
        ESPGdmaQuantum* q = &s->quantum[chan];
        q->s = s;
        q->chan = chan;
        q->bh = qemu_bh_new(esp_gdma_quantum_cb, q);
    }

    esp_gdma_reset_hold(obj, RESET_TYPE_COLD);
}

static void esp_gdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp_gdma_reset_hold;
    dc->realize = esp_gdma_realize;
    device_class_set_props(dc, esp_gdma_properties);
}

static const TypeInfo esp_gdma_info = {
        .name = TYPE_ESP_GDMA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ESPGdmaState),
        .instance_init = esp_gdma_init,
        .class_init = esp_gdma_class_init,
        .abstract = true,
};

static void esp_gdma_register_types(void)
{
    type_register_static(&esp_gdma_info);
}

type_init(esp_gdma_register_types)
