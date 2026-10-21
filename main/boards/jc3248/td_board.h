/* Guition JC3248W535 (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM): the Wi-Fi
 * survey UI on the 3.5 inch 320x480 AXS15231B QSPI panel, used landscape:
 * the UI draws 480x320 at UI_SCALE 2 (layout and fonts twice the T-Display's,
 * data at full resolution); the driver turns it into the panel's portrait
 * frame. Touch (same chip,
 * I2C) stands in for the two buttons. Pins from the board's ESPHome support
 * (mipi_spi model JC3248W535) and community pin maps. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdr_local.h"
#include "sdr_view.h"
#include "zb.h"
#include "esp_attr.h"

/* Large zero-initialised UI arrays go to PSRAM: the S3 keeps its RF ring
 * right after the internal BSS. */
#define UI_BSS EXT_RAM_BSS_ATTR
/* The drawing task's stack too. */
#define UI_TASK_CAPS MALLOC_CAP_SPIRAM

#define TD_LCD_W 480
#define TD_LCD_H 320
#define UI_SCALE 2
#define TD_STRIP_H 16
void td_lcd_init(void);
void td_lcd_backlight(unsigned percent);
/* A TD_LCD_W * TD_STRIP_H buffer to draw rows y.. into: here straight into
 * the PSRAM frame, so td_lcd_blit() has nothing left to copy. */
uint16_t *td_lcd_strip(void);
uint16_t *td_lcd_strip_at(int y);
/* Puts w*h big-endian RGB565 pixels into the frame (scaled and rotated); the
 * frame goes out on the next td_lcd_idle(). */
void td_lcd_blit(int x, int y, int w, int h, uint16_t *strip);
/* Sends the frame if anything changed and waits until the bus is quiet.
 * Call before every RF capture: SPI traffic during a capture shows up as
 * spurs. */
void td_lcd_idle(void);

/* Inputs: left half of the screen or BOOT = NEXT, right half = SCAN. */
enum { TD_NEXT = 1u << 0, TD_SCAN = 1u << 1 };
void td_io_init(void);
unsigned td_keys(void); /* held now */
/* Last touch point (panel coordinates) and point count, for diagnostics. */
void td_touch_raw(unsigned *x, unsigned *y, unsigned *points, unsigned *unused);
uint64_t td_board_pins(void);
