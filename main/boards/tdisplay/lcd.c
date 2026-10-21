/* TTGO T-Display ST7789 (135x240 panel in a 240x320 controller, used as
 * 240x135 landscape) on SPI3 (VSPI). Its pins (MOSI 19, SCLK 18, CS 5) are
 * VSPI's own IO_MUX pins, so 80 MHz is possible besides the safe 40 MHz
 * (CONFIG_ESP_SDR_TDISPLAY_SPI_MHZ). Only panel IO is needed: init list,
 * window, RAMWR. The visible area sits at an offset inside the controller's
 * RAM (TFT_eSPI rotation 1: columns +40, rows +53). */
#include "sdkconfig.h"
#include "td_board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PIN_MOSI 19
#define PIN_SCLK 18
#define PIN_CS 5
#define PIN_DC 16
#define PIN_RST 23
#define PIN_BL 4
#define LCD_HOST SPI3_HOST

/* MADCTL MX|MV, RGB order. Upside down: use 0xA0 (MY|MV) with ROW_OFFSET 52.
 * Red and blue swapped: add 0x08 (BGR). */
#define MADCTL 0x60
#define COL_OFFSET 40
#define ROW_OFFSET 53

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t free_strips;
static uint16_t *strips[2];
static unsigned next_strip;

static bool on_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    (void)h; (void)e; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(free_strips, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t *data, size_t n) { esp_lcd_panel_io_tx_param(io, c, data, n); }

void td_lcd_init(void) {
    free_strips = xSemaphoreCreateCounting(2, 2);
    for (int i = 0; i < 2; i++) {
        strips[i] = heap_caps_malloc(TD_LCD_W * TD_STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(strips[i]);
    }
    /* Backlight PWM, dark until the first frame is drawn. */
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

    const gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = TD_LCD_W * TD_STRIP_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS, .dc_gpio_num = PIN_DC, .spi_mode = 0,
        .pclk_hz = CONFIG_ESP_SDR_TDISPLAY_SPI_MHZ * 1000 * 1000, .trans_queue_depth = 4,
        .on_color_trans_done = on_done, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    static const struct { uint8_t cmd, n, data[14]; uint16_t delay_ms; } init[] = {
        {0x01, 0, {0}, 150},                                   /* SWRESET */
        {0x11, 0, {0}, 120},                                   /* SLPOUT */
        {0x3A, 1, {0x55}, 10},                                 /* COLMOD: 16 bit */
        {0x36, 1, {MADCTL}, 0},                                /* MADCTL: landscape */
        {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0},          /* Porch */
        {0xB7, 1, {0x35}, 0},                                  /* Gate control */
        {0xBB, 1, {0x19}, 0},                                  /* VCOM */
        {0xC0, 1, {0x2C}, 0},                                  /* LCM control */
        {0xC2, 1, {0x01}, 0},                                  /* VDV/VRH enable */
        {0xC3, 1, {0x12}, 0},                                  /* VRH */
        {0xC4, 1, {0x20}, 0},                                  /* VDV */
        {0xC6, 1, {0x0F}, 0},                                  /* 60 Hz */
        {0xD0, 2, {0xA4, 0xA1}, 0},                            /* Power control */
        {0xE0, 14, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 0},
        {0xE1, 14, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 0},
        {0x21, 0, {0}, 0},                                     /* INVON: this panel needs it */
        {0x13, 0, {0}, 10},                                    /* NORON */
    };
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        cmd(init[i].cmd, init[i].n ? init[i].data : NULL, init[i].n);
        if (init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init[i].delay_ms));
    }
    /* Clear before the display and backlight come on so no noise flashes. */
    for (int y = 0; y < TD_LCD_H; y += TD_STRIP_H) {
        int h = TD_LCD_H - y < TD_STRIP_H ? TD_LCD_H - y : TD_STRIP_H;
        uint16_t *s = td_lcd_strip();
        for (unsigned i = 0; i < TD_LCD_W * TD_STRIP_H; i++) s[i] = 0;
        td_lcd_blit(0, y, TD_LCD_W, h, s);
    }
    cmd(0x29, NULL, 0); /* DISPON */
}

void td_lcd_backlight(unsigned percent) {
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint16_t *td_lcd_strip(void) {
    xSemaphoreTake(free_strips, pdMS_TO_TICKS(200));
    uint16_t *s = strips[next_strip];
    next_strip ^= 1;
    return s;
}

void td_lcd_idle(void) {
    for (int i = 0; i < 2; i++) xSemaphoreTake(free_strips, pdMS_TO_TICKS(100));
    for (int i = 0; i < 2; i++) xSemaphoreGive(free_strips);
}

void td_lcd_blit(int x, int y, int w, int h, uint16_t *strip) {
    if (w <= 0 || h <= 0) {
        xSemaphoreGive(free_strips);
        return;
    }
    const int x0 = x + COL_OFFSET, y0 = y + ROW_OFFSET;
    const int x1 = x0 + w - 1, y1 = y0 + h - 1;
    const uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    cmd(0x2A, ca, 4); /* CASET */
    cmd(0x2B, ra, 4); /* RASET */
    esp_lcd_panel_io_tx_color(io, 0x2C, strip, (size_t)w * h * 2);
}
