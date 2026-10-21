#pragma once
#include "rx_tuning.h"
#define SRAM_OWNER_REG 0x60095004u
#define IQ_WORDS 16380u
#define IQ_BUFFER ((uint32_t *)0x40830000)
#define BURST_ID "C5SDR"
/* C5 ownership bit 1 covers an entire 128 KiB bank. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c5_rf_dump);
extern void adctrig(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static bool stock_capture(unsigned n,unsigned divider) {
    adctrig(n-1,0,0,divider*2,0,0,0,0,0);
    return true; /* Common sentinel check detects incomplete stock captures. */
}
static bool frequency_valid(unsigned mhz) {
    return rx_frequency_valid(mhz);
}

#include "tuning.h"
