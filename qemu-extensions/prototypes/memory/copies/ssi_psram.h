#pragma once

#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"
#include "exec/memory.h"

/* Byte-level SSI phases; lane width is not carried by the official bus. */
typedef enum PsramState {
    ST_IDLE = 0,
    ST_CMD_ADDR0,
    ST_CMD_ADDR1,
    ST_CMD_ADDR2,
    ST_DUMMY_CYCLE,
    ST_PROCESSING,
    ST_READ_ID,
    ST_CONTROL,
    ST_REJECTED,
} PsramState;

typedef struct SsiPsramState {
    SSIPeripheral parent_obj;
    /* Existing machine/cache interface: actual RAM allocation, in MiB. */
    uint32_t size_mbytes;
    bool is_octal;
    MemoryRegion data_mr;

    uint8_t command;
    uint32_t addr;
    uint64_t byte_count;
    uint8_t dummy_cycles;
    bool qpi;
    bool wrap32;
    bool reset_enabled;
    bool reset_requested;
    PsramState state;
} SsiPsramState;

#define TYPE_SSI_PSRAM "ssi_psram"
OBJECT_DECLARE_SIMPLE_TYPE(SsiPsramState, SSI_PSRAM)
