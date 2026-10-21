/* TTGO T-Display inputs: the two buttons (GPIO0 with the board's pull-up,
 * GPIO35 input-only with an external one) and two capacitive touch pads on
 * the header, T9 = GPIO32 and T8 = GPIO33. On the ESP32 a finger raises the
 * pad's capacitance, so the reading drops; it is compared with a baseline
 * that follows slow drift while the pad is not touched. */
#include "td_board.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_LEFT 0
#define PIN_RIGHT 35
#define TOUCH_RATIO 0.75f /* touched below this share of the baseline */

static const touch_pad_t pads[2] = {TOUCH_PAD_NUM9 /* GPIO32 */, TOUCH_PAD_NUM8 /* GPIO33 */};
static bool touch_ok;
static float base[2];
static unsigned last[2];

static unsigned touch_read(int i) {
    uint16_t v = 0;
    if (touch_pad_read(pads[i], &v) != ESP_OK) return 0;
    return v;
}

void td_io_init(void) {
    const gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_LEFT) | (1ULL << PIN_RIGHT), .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_pull_mode(PIN_LEFT, GPIO_PULLUP_ONLY);

    touch_ok = touch_pad_init() == ESP_OK;
    if (!touch_ok) return;
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    for (int i = 0; i < 2; i++) touch_pad_config(pads[i], 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 2; i++) {
        unsigned sum = 0;
        for (int n = 0; n < 8; n++) {
            sum += touch_read(i);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        base[i] = sum / 8.0f;
    }
}

static bool touched(int i) {
    if (!touch_ok) return false;
    unsigned v = touch_read(i);
    last[i] = v;
    if (!v) return false;
    if (base[i] < 1) base[i] = v;
    bool on = v < base[i] * TOUCH_RATIO;
    /* Follow drift (temperature, a wire plugged on) only while untouched;
     * a reading above the baseline is taken quickly. */
    if (!on) base[i] += (v - base[i]) * (v > base[i] ? 0.2f : 0.01f);
    return on;
}

unsigned td_keys(void) {
    unsigned held = 0;
    if (!gpio_get_level(PIN_LEFT)) held |= TD_NEXT;
    if (!gpio_get_level(PIN_RIGHT)) held |= TD_SCAN;
    if (touched(0)) held |= TD_NEXT;
    if (touched(1)) held |= TD_SCAN;
    return held;
}

void td_touch_raw(unsigned *t9, unsigned *t8, unsigned *base9, unsigned *base8) {
    *t9 = last[0];
    *t8 = last[1];
    *base9 = (unsigned)base[0];
    *base8 = (unsigned)base[1];
}

uint64_t td_board_pins(void) {
    /* LCD bus, CS/DC/RST/backlight, the GPIO0 button and both touch pads. */
    const unsigned pins[] = {19, 18, 5, 16, 23, 4, 0, 32, 33};
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) mask |= 1ULL << pins[i];
    return mask;
}
