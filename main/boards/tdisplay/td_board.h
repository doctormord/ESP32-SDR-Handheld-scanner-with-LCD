/* LILYGO TTGO T-Display (ESP32-D0WDQ6, 4 MB flash): on-device Wi-Fi survey on
 * the 1.14 inch ST7789 (135x240, used landscape) and two buttons. Pin map from LILYGO's TTGO-T-Display repository (TFT_eSPI Setup25). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
/* Where rows y.. are drawn: the frame itself when there is one. */
uint16_t *td_lcd_strip_at(int y);
/* Strip mode: queues w*h big-endian RGB565 pixels from a strip (busy until
 * sent, take the next with td_lcd_strip()). Frame mode: puts them into the
 * frame, which td_lcd_idle() sends. */
void td_lcd_blit(int x, int y, int w, int h, uint16_t *strip);
/* Sends a changed frame (not while deferred) and waits until the bus is
 * quiet. Call before every RF capture: SPI traffic to the panel during a
 * capture shows up as a comb of spurs. */
void td_lcd_idle(void);
/* Frame mode: while deferred, td_lcd_idle() does not send (the frame is
 * being drawn on the other core). */
bool td_lcd_has_frame(void);
/* Takes the frame RAM if at least keep bytes stay free (call last). */
bool td_lcd_try_frame(size_t keep);
void td_lcd_defer(bool on);
#define TD_LCD_CAN_DEFER 1
/* Diagnostics: display traffic during a capture (TDSPINOISE). */
void td_lcd_noise_burst(void);
void td_lcd_noise_settle(void);
#define TD_LCD_NOISE_TEST 1

/* Inputs. NEXT: button GPIO0 (left). SCAN: button GPIO35 (right). */
enum { TD_NEXT = 1u << 0, TD_SCAN = 1u << 1 };
void td_io_init(void);
unsigned td_keys(void); /* held now */
/* No touch on this board: zeros (kept for TDSTAT?, shared with the JC3248). */
void td_touch_raw(unsigned *a, unsigned *b, unsigned *c, unsigned *d);
/* Output pins the board drivers own (reserved from GPIO discovery). */
uint64_t td_board_pins(void);
