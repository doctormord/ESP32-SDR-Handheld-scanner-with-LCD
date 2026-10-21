#pragma once
#define SRAM_OWNER_REG 0x60095004u
#define IQ_WORDS 16380u
#define IQ_BUFFER ((uint32_t *)0x40830000)
#define BURST_ID "C61SDR"
/* sensor-firmware/main/iq/sensor_iq.c documents the per-64-KiB ownership
 * selector and mandatory readback. Use bank 3, leaving bank 4's live ROM
 * data accessible. This also avoids a revision-specific ROM reservation. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c61_rf_dump);
#define DUMP_CTRL_REG 0x600a9004u
#define DUMP_MODE_REG 0x600a9008u
#define DUMP_WRITER_REG 0x600a900cu
#define DUMP_PACK_REG 0x600a9018u
static bool stock_capture(unsigned n,unsigned divider) {
    const uint32_t owner=REG_READ(SRAM_OWNER_REG);
    REG_WRITE(DUMP_CTRL_REG,0);
    REG_WRITE(DUMP_WRITER_REG,0);
    REG_WRITE(0x600a9c04,0xffffffffu); /* Dump clocks, matching stock adctrig. */
    REG_SET_BIT(0x600a0800,4);
    REG_WRITE(DUMP_MODE_REG,(REG_READ(DUMP_MODE_REG)&~0x00fe0000u)|
              (15u<<17)|(divider<<21));
    REG_WRITE(DUMP_PACK_REG,(REG_READ(DUMP_PACK_REG)&~0x01ffffffu)|
              24u|(25u<<6)|(26u<<12)|(27u<<18)|(1u<<24));
    REG_CLR_BIT(0x600a20b4,1); /* Selected dump source gate. */
    REG_WRITE(SRAM_OWNER_REG,(owner&~0x11f00u)|(8u<<8));
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    (void)REG_READ(SRAM_OWNER_REG);
    /* Software-triggered finite snapshot: continuous gate (bit 17) stays off.
     * C61 completion is bit 18 (sensor-firmware and hardware verified). */
    REG_WRITE(DUMP_CTRL_REG,n|(1u<<18));
    REG_WRITE(DUMP_CTRL_REG,n);
    REG_WRITE(DUMP_CTRL_REG,n|(1u<<31));
    REG_WRITE(DUMP_CTRL_REG,n|(1u<<31)|(1u<<19));
    REG_WRITE(DUMP_CTRL_REG,n|(1u<<31));
    const int64_t deadline=esp_timer_get_time()+20000;
    bool done;
    do { done=(REG_READ(DUMP_CTRL_REG)&(1u<<18))!=0; }
    while(!done && esp_timer_get_time()<deadline);
    REG_WRITE(DUMP_CTRL_REG,0);
    REG_WRITE(SRAM_OWNER_REG,owner);
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    (void)REG_READ(SRAM_OWNER_REG);
    return done;
}
#include "tuning.h"
