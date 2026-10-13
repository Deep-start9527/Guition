/* SPDX-License-Identifier: MIT */
#include "host_platform.h"
#include "esp_lcd_touch_gsl3680.h"
#include <stdio.h>
#include <string.h>

static uint8_t report[44];
static esp_err_t read_error, status_error, gpio_error, interrupt_error;
static bool bad_status, allocation_failure;
static unsigned writes, fail_write, firmware_pages, firmware_words;
static const esp_lcd_panel_io_handle_t io = (void *)(uintptr_t)1;

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return allocation_failure ? NULL : calloc(count, size);
}

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t handle, int reg, void *data, size_t size)
{
    assert(handle == io);
    if (reg == 0xb0) {
        assert(size == 4);
        memset(data, bad_status ? 0 : 0x5a, size);
        return status_error;
    }
    assert(reg == 0x80 && size == sizeof(report));
    if (read_error != ESP_OK) {
        return read_error; /* Leave destination untouched to detect bad reads. */
    }
    memcpy(data, report, size);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t handle, int reg, const void *data, size_t size)
{
    assert(handle == io && data != NULL && reg >= 0 && reg <= 255);
    assert(size == 1 || size == 4);
    ++writes;
    if (reg == 0xf0) {
        assert(size == 1); /* Page writes must not depend on host char signedness. */
        ++firmware_pages;
    } else if (size == 4 && reg != 0xbc) {
        ++firmware_words;
    }
    return writes == fail_write ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return gpio_error; }
esp_err_t gpio_set_level(gpio_num_t pin, unsigned value) { assert(pin >= 0 && value <= 1); return gpio_error; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { assert(pin >= 0); return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { assert(pin >= 0); return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t cb, void *arg) { assert(pin >= 0 && cb && arg); return interrupt_error; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return interrupt_error; }
esp_err_t gpio_intr_enable(gpio_num_t pin) { assert(pin >= 0); return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { assert(pin >= 0); return ESP_OK; }

static esp_lcd_touch_config_t config(void)
{
    return (esp_lcd_touch_config_t) {
        .x_max = 800, .y_max = 1280,
        .rst_gpio_num = 22, .int_gpio_num = 21,
        .levels = {.reset = 0, .interrupt = 0},
    };
}

static void callback(esp_lcd_touch_handle_t tp) { (void)tp; }

static esp_lcd_touch_handle_t create(void)
{
    esp_lcd_touch_config_t cfg = config();
    esp_lcd_touch_handle_t tp = NULL;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_OK);
    assert(tp && firmware_pages > 100 && firmware_words > 1000);
    return tp;
}

static void contact(unsigned index, uint16_t sensor_x, uint16_t sensor_y, uint8_t hardware_id)
{
    assert(index < 10);
    uint8_t *p = report + 4 + 4 * index;
    p[0] = sensor_y & 0xff;
    p[1] = sensor_y >> 8;
    p[2] = sensor_x & 0xff;
    p[3] = ((sensor_x >> 8) & 0xf) | (hardware_id << 4);
}

static uint8_t get(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points, uint8_t capacity)
{
    uint8_t count = 255;
    assert(esp_lcd_touch_get_data(tp, points, &count, capacity) == ESP_OK);
    assert(count <= capacity);
    for (uint8_t i = 0; i < count; ++i) {
        assert(points[i].track_id >= 1 && points[i].track_id <= 10);
        assert(points[i].strength == 0);
        assert(points[i].x > 0 && points[i].x < tp->config.x_max);
        assert(points[i].y > 0 && points[i].y < tp->config.y_max);
        for (uint8_t j = 0; j < i; ++j) {
            assert(points[i].track_id != points[j].track_id);
        }
    }
    return count;
}

static void release(esp_lcd_touch_handle_t tp)
{
    memset(report, 0, sizeof(report));
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    esp_lcd_touch_point_data_t points[10];
    assert(get(tp, points, 10) == 0);
}

static void test_initialization(void)
{
    esp_lcd_touch_config_t cfg = config();
    esp_lcd_touch_handle_t tp = (void *)(uintptr_t)1;
    assert(esp_lcd_touch_new_i2c_gsl3680(NULL, &cfg, &tp) == ESP_ERR_INVALID_ARG && !tp);
    cfg.x_max = 0;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_ERR_INVALID_ARG && !tp);
    cfg = config();
    allocation_failure = true;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_ERR_NO_MEM && !tp);
    allocation_failure = false;
    gpio_error = ESP_FAIL;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_FAIL && !tp);
    gpio_error = ESP_OK;
    fail_write = writes + 8; /* Fail during firmware loading, not GPIO setup. */
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_ERR_TIMEOUT && !tp);
    fail_write = 0;
    status_error = ESP_ERR_TIMEOUT;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_ERR_TIMEOUT && !tp);
    status_error = ESP_OK;
    bad_status = true;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_FAIL && !tp);
    bad_status = false;
    cfg.interrupt_callback = callback;
    interrupt_error = ESP_FAIL;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_FAIL && !tp);
    interrupt_error = ESP_OK;
}

static void test_mapping_and_api(esp_lcd_touch_handle_t tp)
{
    esp_lcd_touch_point_data_t points[10];
    release(tp);
    report[0] = 1;
    contact(0, 832, 450, 15);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == 1);
    assert(points[0].x >= 398 && points[0].x <= 403);
    assert(points[0].y >= 638 && points[0].y <= 645);
    uint8_t id = points[0].track_id;
    contact(0, 832, 450, 0); /* Firmware ID changes must not change a stationary contact. */
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == 1 && points[0].track_id == id);

    uint16_t old_x = points[0].x, old_y = points[0].y;
    assert(esp_lcd_touch_set_mirror_x(tp, true) == ESP_OK);
    assert(esp_lcd_touch_set_mirror_y(tp, true) == ESP_OK);
    assert(esp_lcd_touch_set_swap_xy(tp, true) == ESP_OK);
    assert(get(tp, points, 10) == 1);
    assert(points[0].x == 1280 - old_y && points[0].y == 800 - old_x);
    uint16_t x, y;
    uint8_t count;
    assert(esp_lcd_touch_get_coordinates(tp, &x, &y, NULL, &count, 1));
    assert(count == 1 && x == points[0].x && y == points[0].y);
    esp_lcd_touch_set_mirror_x(tp, false);
    esp_lcd_touch_set_mirror_y(tp, false);
    esp_lcd_touch_set_swap_xy(tp, false);

    release(tp);
    report[0] = 1;
    contact(0, 0, 0, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == 1 && points[0].x == 1 && points[0].y == 1);
    release(tp);
    report[0] = 1;
    contact(0, 4095, 4095, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == 1 && points[0].x == 799 && points[0].y == 1279);
    release(tp);
    report[0] = 1;
    contact(0, 500, 0x1000 | 400, 0); /* Key record must not become a ghost touch. */
    assert(esp_lcd_touch_read_data(tp) == ESP_OK && get(tp, points, 10) == 0);
}

static void test_tracking(esp_lcd_touch_handle_t tp)
{
    esp_lcd_touch_point_data_t points[10];
    release(tp);
    report[0] = 2;
    contact(0, 300, 150, 0);
    contact(1, 1200, 700, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    uint8_t count = get(tp, points, 10);
    assert(count == (CONFIG_ESP_LCD_TOUCH_MAX_POINTS == 1 ? 1 : 2));
    esp_lcd_touch_point_data_t primary = points[0];
    uint8_t second_id = count > 1 ? points[1].track_id : 0;
    contact(0, 1210, 705, 0);
    contact(1, 310, 155, 0); /* Packet order changes, including with capacity=1. */
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == count);
    assert(points[0].track_id == primary.track_id);
    assert(points[0].x < 200 && points[0].y < 300);
    assert(points[0].x > primary.x && points[0].y > primary.y);
    if (count > 1) {
        assert(points[1].track_id == second_id);
        report[0] = 1;
        contact(0, 1210, 705, 0);
        assert(esp_lcd_touch_read_data(tp) == ESP_OK);
        assert(get(tp, points, 10) == 1 && points[0].track_id == second_id);
        report[0] = 2;
        contact(0, 300, 150, 0); /* New contact must not replace surviving primary. */
        contact(1, 1210, 705, 0);
        assert(esp_lcd_touch_read_data(tp) == ESP_OK);
        assert(get(tp, points, 10) == 2 && points[0].track_id == second_id);
    }
    release(tp);
    report[0] = 10;
    for (unsigned i = 0; i < 10; ++i) {
        contact(i, 100 + i * 140, 50 + i * 70, 0);
    }
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == (CONFIG_ESP_LCD_TOUCH_MAX_POINTS < 10 ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : 10));
    assert(get(tp, points, 1) == 1); /* Caller capacity is also respected. */
    release(tp);
    /* Replace a whole ten-contact frame: released IDs must be reusable even
     * if every previous ID was reserved and no positions match. */
    report[0] = 10;
    for (unsigned i = 0; i < 10; ++i) {
        contact(i, 100 + i * 10, 50 + i * 10, 0);
    }
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    for (unsigned i = 0; i < 10; ++i) {
        contact(i, 1400 + i * 10, 700 + i * 10, 0);
    }
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(get(tp, points, 10) == (CONFIG_ESP_LCD_TOUCH_MAX_POINTS < 10 ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : 10));
    assert(points[0].x > 600 && points[0].y > 1000);
    release(tp);

    /* Exercise distance arithmetic across the full uint16_t output range. */
    tp->config.x_max = tp->config.y_max = UINT16_MAX;
    report[0] = 1;
    contact(0, 0, 0, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK && get(tp, points, 10) == 1);
    uint8_t old_id = points[0].track_id;
    contact(0, 4095, 4095, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK && get(tp, points, 10) == 1);
    assert(points[0].x == UINT16_MAX - 1 && points[0].y == UINT16_MAX - 1);
    assert(points[0].track_id != old_id);
    release(tp);
    tp->config.x_max = 800;
    tp->config.y_max = 1280;
}

static void test_instance_isolation(esp_lcd_touch_handle_t first)
{
    /* Independent IO would belong to separate devices on hardware. The mock
     * shares only the input packet; each handle must retain its own snapshot. */
    esp_lcd_touch_config_t cfg = config();
    cfg.rst_gpio_num = GPIO_NUM_NC;
    cfg.int_gpio_num = GPIO_NUM_NC;
    esp_lcd_touch_handle_t second = NULL;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &second) == ESP_OK);
    release(first);
    report[0] = 1;
    contact(0, 300, 150, 0);
    assert(esp_lcd_touch_read_data(first) == ESP_OK);
    esp_lcd_touch_point_data_t a[10], b[10];
    assert(get(first, a, 10) == 1 && get(second, b, 10) == 0);
    contact(0, 1200, 700, 0);
    assert(esp_lcd_touch_read_data(second) == ESP_OK);
    assert(get(second, b, 10) == 1);
    assert(b[0].x > a[0].x && b[0].y > a[0].y);
    uint16_t first_x = a[0].x;
    assert(get(first, a, 10) == 1 && a[0].x == first_x);
    assert(esp_lcd_touch_del(second) == ESP_OK);
    assert(get(first, a, 10) == 1 && a[0].x == first_x);
    release(first);
}

static void test_errors_and_lifecycle(esp_lcd_touch_handle_t tp)
{
    esp_lcd_touch_point_data_t points[10];
    report[0] = 1;
    contact(0, 832, 450, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    read_error = ESP_ERR_TIMEOUT;
    assert(esp_lcd_touch_read_data(tp) == ESP_ERR_TIMEOUT && get(tp, points, 10) == 0);
    read_error = ESP_OK;
    for (unsigned count = 11; count <= 255; ++count) {
        report[0] = count;
        assert(esp_lcd_touch_read_data(tp) == ESP_ERR_INVALID_SIZE);
        assert(get(tp, points, 10) == 0);
    }
    esp_lcd_touch_config_t cfg = config();
    release(tp);
    report[0] = 1;
    contact(0, 832, 450, 0);
    assert(esp_lcd_touch_read_data(tp) == ESP_OK);
    assert(esp_lcd_touch_enter_sleep(tp) == ESP_OK && get(tp, points, 10) == 0);
    unsigned old_pages = firmware_pages;
    assert(esp_lcd_touch_exit_sleep(tp) == ESP_OK && firmware_pages > old_pages);
    fail_write = writes + 1;
    assert(esp_lcd_touch_exit_sleep(tp) == ESP_ERR_TIMEOUT);
    fail_write = 0;
    assert(esp_lcd_touch_del(tp) == ESP_OK);
    tp = create();
    assert(esp_lcd_touch_del(tp) == ESP_OK);
    cfg.rst_gpio_num = GPIO_NUM_NC;
    cfg.int_gpio_num = GPIO_NUM_NC;
    assert(esp_lcd_touch_new_i2c_gsl3680(io, &cfg, &tp) == ESP_OK);
    assert(esp_lcd_touch_enter_sleep(tp) == ESP_ERR_NOT_SUPPORTED);
    assert(esp_lcd_touch_exit_sleep(tp) == ESP_ERR_NOT_SUPPORTED);
    assert(esp_lcd_touch_del(tp) == ESP_OK);
}

int main(void)
{
    test_initialization();
    esp_lcd_touch_handle_t tp = create();
    test_mapping_and_api(tp);
    test_tracking(tp);
    test_instance_isolation(tp);
    test_errors_and_lifecycle(tp);
    printf("GSL3680 host scenarios passed (capacity=%d, ASan/UBSan)\n", CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    return 0;
}
