/* JC3248W535 AXS15231B panel, 320x480 portrait, on QSPI (SPI2, 40 MHz,
 * mode 3). Commands go as opcode 0x02 + (cmd << 8) on one line, pixels as
 * opcode 0x32 + (RAMWR << 8) with the data on four lines (as Espressif's
 * esp_lcd_axs15231b and ESPHome's mipi_spi do). Some panels ignore window
 * commands, so the driver always sends the whole frame: RAMWR for the first
 * chunk, RAMWRC (0x3C) for the rest.
 * The UI draws a TD_LCD_W x TD_LCD_H landscape picture, kept as it is in
 * PSRAM (strips are copied in row by row). Scaling it to 480x320 (if it is
 * smaller) and turning it a quarter so the panel's long side runs left to
 * right happens while sending:
 * each chunk of panel rows is built in internal RAM while the chunk before
 * is still going out by DMA. (DMA straight from PSRAM, one transfer or ten,
 * stalled half way through the frame on this board; 80 MHz showed scan lines:
 * the panel pins are routed through the GPIO matrix.) */
#include "sdkconfig.h"
#include "td_board.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define PIN_CS 45
#define PIN_SCK 47
#define PIN_D0 21
#define PIN_D1 48
#define PIN_D2 40
#define PIN_D3 39
#define PIN_BL 1
#define LCD_HOST SPI2_HOST

#define PANEL_W 320
#define PANEL_H 480
#define CHUNK_ROWS 12           /* rows per DMA transfer (7.5 KiB; internal RAM is scarce) */
#define CHUNKS (PANEL_H / CHUNK_ROWS)
/* Set if the picture is upside down. */
#define ROTATE_180 0

#define OP_CMD 0x02
#define OP_COLOR 0x32
#define CMD(c) ((uint32_t)OP_CMD << 24 | (uint32_t)(c) << 8)
#define COLOR(c) ((uint32_t)OP_COLOR << 24 | (uint32_t)(c) << 8)

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t chunk_done;
static uint16_t *frame;         /* logical TD_LCD_W x TD_LCD_H, PSRAM */
static uint16_t *bounce[2];     /* internal DMA chunks */
UI_BSS static uint16_t strip[TD_LCD_W * TD_STRIP_H];
static bool dirty;
unsigned td_lcd_flush_us, td_lcd_blit_us; /* diagnostics */

static bool on_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)h; (void)e; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(chunk_done, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t *data, size_t n) { esp_lcd_panel_io_tx_param(io, CMD(c), data, n); }

/* Panel row pr shows logical column pr / SCALE; panel column pc shows logical
 * row (PANEL_W - 1 - pc) / SCALE (the picture turned a quarter). */
#define SCALE (PANEL_H / TD_LCD_W)
static int16_t src_row[PANEL_W]; /* per panel column: offset of its logical row */
static void build_chunk(uint16_t *dst, int pr0) {
    for (int r = 0; r < CHUNK_ROWS; r++, dst += PANEL_W) {
        int pr = pr0 + r;
        if (ROTATE_180) pr = PANEL_H - 1 - pr;
        const uint16_t *col = frame + pr / SCALE;
        for (int pc = 0; pc < PANEL_W; pc++) dst[pc] = col[src_row[ROTATE_180 ? PANEL_W - 1 - pc : pc] * TD_LCD_W];
    }
}

static void flush(void) {
    for (int i = 0; i < 2; i++) xSemaphoreTake(chunk_done, pdMS_TO_TICKS(100));
    for (int k = 0; k < CHUNKS; k++) {
        uint16_t *b = bounce[k & 1];
        if (k >= 2) xSemaphoreTake(chunk_done, pdMS_TO_TICKS(100));
        build_chunk(b, k * CHUNK_ROWS);
        esp_lcd_panel_io_tx_color(io, COLOR(k ? 0x3C : 0x2C), b, CHUNK_ROWS * PANEL_W * 2);
    }
    /* Back to two free bounce buffers once both last chunks are out. */
    for (int i = 0; i < 2; i++) xSemaphoreTake(chunk_done, pdMS_TO_TICKS(100));
    for (int i = 0; i < 2; i++) xSemaphoreGive(chunk_done);
}

void td_lcd_init(void) {
    chunk_done = xSemaphoreCreateCounting(2, 2);
    frame = heap_caps_calloc(TD_LCD_W * TD_LCD_H, 2, MALLOC_CAP_SPIRAM);
    for (int pc = 0; pc < PANEL_W; pc++) src_row[pc] = (int16_t)((PANEL_W - 1 - pc) / SCALE);
    for (int i = 0; i < 2; i++)
        bounce[i] = heap_caps_malloc(CHUNK_ROWS * PANEL_W * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(frame && bounce[0] && bounce[1]);

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t bl = {
        .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl));

    const spi_bus_config_t bus = {
        .data0_io_num = PIN_D0, .data1_io_num = PIN_D1, .sclk_io_num = PIN_SCK,
        .data2_io_num = PIN_D2, .data3_io_num = PIN_D3,
        .max_transfer_sz = CHUNK_ROWS * PANEL_W * 2 + 8,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = -1, .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 4, /* 80 MHz shows scan lines */
        .on_color_trans_done = on_done, .lcd_cmd_bits = 32, .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    /* Vendor unlock, power, lock (ESPHome's AXS15231 sequence), then the
     * usual: sleep out, 16-bit colour, RGB order, display on. */
    static const uint8_t unlock[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5};
    static const uint8_t lock[] = {0, 0, 0, 0, 0, 0, 0, 0};
    cmd(0xBB, unlock, sizeof(unlock));
    cmd(0xC1, (const uint8_t[]){0x33}, 1);
    cmd(0xBB, lock, sizeof(lock));
    cmd(0x11, NULL, 0);                         /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x3A, (const uint8_t[]){0x55}, 1);      /* COLMOD: 16 bit */
    cmd(0x36, (const uint8_t[]){0x00}, 1);      /* MADCTL: RGB */
    cmd(0x2A, (const uint8_t[]){0, 0, (PANEL_W - 1) >> 8, (PANEL_W - 1) & 255}, 4);
    cmd(0x2B, (const uint8_t[]){0, 0, (PANEL_H - 1) >> 8, (PANEL_H - 1) & 255}, 4);
    flush();                                    /* black before the light comes on */
    cmd(0x29, NULL, 0);                         /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));
}

void td_lcd_backlight(unsigned percent) {
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint16_t *td_lcd_strip(void) { return strip; }
uint16_t *td_lcd_strip_at(int y) { return y >= 0 && y < TD_LCD_H ? frame + y * TD_LCD_W : strip; }

/* Strips go into the logical picture unchanged. */
void td_lcd_blit(int x, int y, int w, int h, uint16_t *src) {
    const int64_t t0 = esp_timer_get_time();
    if (src != frame + y * TD_LCD_W) /* drawn elsewhere: copy it in */
    for (int r = 0; r < h && y + r < TD_LCD_H; r++)
        memcpy(frame + (y + r) * TD_LCD_W + x, src + r * w, (size_t)w * 2);
    dirty = true;
    td_lcd_blit_us += (unsigned)(esp_timer_get_time() - t0);
}

void td_lcd_idle(void) {
    if (dirty) {
        dirty = false;
        const int64_t t0 = esp_timer_get_time();
        flush();
        td_lcd_flush_us = (unsigned)(esp_timer_get_time() - t0);
    }
}
