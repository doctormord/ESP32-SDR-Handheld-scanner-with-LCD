/* JC3248W535 inputs: the AXS15231B's capacitive touch on I2C (SDA 4, SCL 8,
 * address 0x3B) and the BOOT button (GPIO0). The touch read is the chip's
 * 11-byte request followed by 8 bytes: [1] points, [2..3] x, [4..5] y in
 * panel coordinates (portrait, 320x480). The left half of the landscape
 * picture acts as the left button, the right half as the right one. */
#include "td_board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"

#define PIN_SDA 4
#define PIN_SCL 8
#define PIN_BOOT 0
#define TOUCH_ADDR 0x3B
#define PANEL_H 480
#define READ_US 15000           /* poll the touch at most this often */
/* Set together with the display's ROTATE_180. */
#define ROTATE_180 0

static i2c_master_dev_handle_t touch;
static unsigned last_x, last_y, last_n, held;
static int64_t read_at;

void td_io_init(void) {
    const gpio_config_t in = {.pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&in));
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) return;
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TOUCH_ADDR, .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev, &touch) != ESP_OK) touch = NULL;
}

static unsigned touch_keys(void) {
    if (!touch) return 0;
    static const uint8_t req[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    uint8_t d[8] = {0};
    if (i2c_master_transmit_receive(touch, req, sizeof(req), d, sizeof(d), 20) != ESP_OK) return 0;
    const unsigned n = d[1];
    if (!n || n > 2) {last_n = 0; return 0;}
    last_x = (unsigned)(d[2] & 0x0F) << 8 | d[3];
    last_y = (unsigned)(d[4] & 0x0F) << 8 | d[5];
    last_n = n;
    /* Landscape x runs along the panel's long side (its y). */
    unsigned lx = last_y;
    if (ROTATE_180) lx = PANEL_H - 1 - lx;
    return lx < PANEL_H / 2 ? TD_NEXT : TD_SCAN;
}

unsigned td_keys(void) {
    const int64_t now = esp_timer_get_time();
    if (now - read_at >= READ_US) {
        read_at = now;
        held = touch_keys();
    }
    return held | (gpio_get_level(PIN_BOOT) ? 0 : TD_NEXT);
}

void td_touch_raw(unsigned *x, unsigned *y, unsigned *points, unsigned *unused) {
    *x = last_x;
    *y = last_y;
    *points = last_n;
    *unused = 0;
}

uint64_t td_board_pins(void) {
    /* QSPI panel, backlight, touch I2C, touch interrupt (3) and reset (12,
     * shared with the SD card), BOOT. */
    const unsigned pins[] = {45, 47, 21, 48, 40, 39, 1, 4, 8, 3, 12, 0};
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) mask |= 1ULL << pins[i];
    return mask;
}
