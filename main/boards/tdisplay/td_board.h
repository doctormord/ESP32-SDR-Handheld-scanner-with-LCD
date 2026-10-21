/* LILYGO TTGO T-Display (ESP32-D0WDQ6, 4 MB flash): on-device Wi-Fi survey on
 * the 1.14 inch ST7789 (135x240, used landscape), two buttons and two touch
 * pins. Pin map from LILYGO's TTGO-T-Display repository (TFT_eSPI Setup25). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdr_local.h"
#include "sdr_view.h"
#include "zb.h"

#define UI_BSS /* internal RAM */
#define UI_SCALE 1

/* ST7789 240x135 landscape on SPI2. */
#define TD_LCD_W 240
#define TD_LCD_H 135
#define TD_STRIP_H 16
void td_lcd_init(void);
void td_lcd_backlight(unsigned percent);
/* A free TD_LCD_W * TD_STRIP_H DMA strip (two alternate); waits while both
 * are still being sent. */
uint16_t *td_lcd_strip(void);
#define td_lcd_strip_at(y) td_lcd_strip()
/* Queues w*h big-endian RGB565 pixels from a strip; the strip is busy until
 * the transfer ends, so take the next one with td_lcd_strip(). */
void td_lcd_blit(int x, int y, int w, int h, uint16_t *strip);
/* Waits until no strip is being sent. Call before every RF capture: SPI
 * traffic to the panel during a capture shows up as a comb of spurs. */
void td_lcd_idle(void);

/* Inputs. NEXT: button GPIO0 (left) or a finger on GPIO32 (touch T9).
 * SCAN: button GPIO35 (right) or a finger on GPIO33 (touch T8). Touch needs
 * nothing soldered: the bare header pin, its pad or a jumper wire on it. */
enum { TD_NEXT = 1u << 0, TD_SCAN = 1u << 1 };
void td_io_init(void);
unsigned td_keys(void); /* held now */
/* Last touch readings and baselines, for diagnostics (lower = touched). */
void td_touch_raw(unsigned *t9, unsigned *t8, unsigned *base9, unsigned *base8);
/* Output pins the board drivers own (reserved from GPIO discovery). */
uint64_t td_board_pins(void);
