/* TTGO T-Display inputs: the two buttons, GPIO0 (left, with the board's
 * pull-up) and GPIO35 (right, input-only with an external one). */
#include "td_board.h"
#include "driver/gpio.h"

#define PIN_LEFT 0
#define PIN_RIGHT 35

void td_io_init(void) {
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_LEFT) | (1ULL << PIN_RIGHT), .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_pull_mode(PIN_LEFT, GPIO_PULLUP_ONLY);
}

unsigned td_keys(void) {
    unsigned held = 0;
    if (!gpio_get_level(PIN_LEFT)) held |= TD_NEXT;
    if (!gpio_get_level(PIN_RIGHT)) held |= TD_SCAN;
    return held;
}

/* No touch on this board; TDSTAT? prints zeros. */
void td_touch_raw(unsigned *a, unsigned *b, unsigned *c, unsigned *d) { *a = *b = *c = *d = 0; }

uint64_t td_board_pins(void) {
    /* LCD bus, CS/DC/RST/backlight and the GPIO0 button. */
    const unsigned pins[] = {19, 18, 5, 16, 23, 4, 0};
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) mask |= 1ULL << pins[i];
    return mask;
}
