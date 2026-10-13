/* SPDX-License-Identifier: MIT
 * Covers the component authors' new/reworked contributions; see NOTICE for
 * retained upstream material, including the separately supplied firmware.
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gsl3680.h"
#include "gsl3680_firmware.h"

static const char *TAG = "gsl3680";

#define GSL3680_DATA_REG             0x80
#define GSL3680_MAX_CONTACTS         10
#define GSL3680_PACKET_SIZE          (4 + 4 * GSL3680_MAX_CONTACTS)
#define GSL3680_MAX_POINTS           ((CONFIG_ESP_LCD_TOUCH_MAX_POINTS < GSL3680_MAX_CONTACTS) ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : GSL3680_MAX_CONTACTS)

/* JC8012P4A1C parameters from the original configuration. The firmware reports
 * sensor coordinates: 26 drive and 14 sense channels, 64 units per channel.
 * The reporting axes are reversed. These are bundled-firmware parameters,
 * not a universal GSL3680 coordinate range. */
#define GSL3680_RAW_X_RANGE          (26 * 64)
#define GSL3680_RAW_Y_RANGE          (14 * 64)
#define GSL3680_RAW_X_GAIN           4113
#define GSL3680_RAW_Y_GAIN           4119
#define GSL3680_RAW_Y_OFFSET         2
#define GSL3680_GAIN_DIVISOR         4096

static esp_err_t gsl3680_read_data(esp_lcd_touch_handle_t tp);
static bool gsl3680_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t gsl3680_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num);
static esp_err_t gsl3680_del(esp_lcd_touch_handle_t tp);
static esp_err_t gsl3680_enter_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t gsl3680_exit_sleep(esp_lcd_touch_handle_t tp);
static esp_err_t gsl3680_init(esp_lcd_touch_handle_t tp);
static esp_err_t gsl3680_check_firmware(esp_lcd_touch_handle_t tp);

static esp_err_t gsl3680_write(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t value)
{
    return esp_lcd_panel_io_tx_param(tp->io, reg, &value, sizeof(value));
}

static void gsl3680_clear_points(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    memset(tp->data.coords, 0, sizeof(tp->data.coords));
    portEXIT_CRITICAL(&tp->data.lock);
}

esp_err_t esp_lcd_touch_new_i2c_gsl3680(esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_touch_config_t *config,
                                      esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(out_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "Missing output handle");
    *out_touch = NULL;
    ESP_RETURN_ON_FALSE(io != NULL && config != NULL, ESP_ERR_INVALID_ARG, TAG, "Missing IO or config");
    ESP_RETURN_ON_FALSE(config->x_max > 1 && config->y_max > 1, ESP_ERR_INVALID_ARG, TAG, "Invalid coordinate range");

    const esp_lcd_touch_io_gsl3680_config_t *address_config = config->driver_data;
    ESP_RETURN_ON_FALSE(address_config == NULL || address_config->dev_addr == ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS,
                        ESP_ERR_INVALID_ARG, TAG, "Unsupported I2C address");

    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t tp = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(tp != NULL, ESP_ERR_NO_MEM, TAG, "No memory for touch controller");
    tp->io = io;
    tp->config = *config;
    tp->data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    tp->read_data = gsl3680_read_data;
    tp->get_xy = gsl3680_get_xy;
    tp->get_track_id = gsl3680_get_track_id;
    tp->del = gsl3680_del;
    tp->enter_sleep = gsl3680_enter_sleep;
    tp->exit_sleep = gsl3680_exit_sleep;

    if (config->rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t reset_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(config->rst_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&reset_config), err, TAG, "Reset GPIO setup failed");
    }

    /* Preserve the existing optional INT-high address selection sequence. */
    if (address_config && config->rst_gpio_num != GPIO_NUM_NC && config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = true,
            .pin_bit_mask = BIT64(config->int_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_config), err, TAG, "INT GPIO setup failed");
        ESP_GOTO_ON_ERROR(gpio_set_level(config->rst_gpio_num, config->levels.reset), err, TAG, "Reset failed");
        ESP_GOTO_ON_ERROR(gpio_set_level(config->int_gpio_num, 1), err, TAG, "Address selection failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_GOTO_ON_ERROR(gpio_set_level(config->int_gpio_num, 1), err, TAG, "Address selection failed");
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_GOTO_ON_ERROR(gpio_set_level(config->rst_gpio_num, !config->levels.reset), err, TAG, "Reset release failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_GOTO_ON_ERROR(gsl3680_init(tp), err, TAG, "Controller initialization failed");
    ESP_GOTO_ON_ERROR(gsl3680_check_firmware(tp), err, TAG, "Firmware check failed");

    if (config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = config->levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
            .pin_bit_mask = BIT64(config->int_gpio_num),
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_config), err, TAG, "INT GPIO setup failed");
        if (config->interrupt_callback) {
            ESP_GOTO_ON_ERROR(esp_lcd_touch_register_interrupt_callback(tp, config->interrupt_callback), err, TAG, "Interrupt registration failed");
        }
    }

    *out_touch = tp;
    return ESP_OK;

err:
    gsl3680_del(tp);
    return ret;
}

static uint16_t gsl3680_scale_coordinate(int32_t raw, uint16_t gain, uint16_t raw_range, uint16_t extent)
{
    if (raw < 0) {
        raw = 0;
    }
    uint32_t corrected = ((uint32_t)raw * gain + GSL3680_GAIN_DIVISOR / 2) / GSL3680_GAIN_DIVISOR;
    uint32_t pixel = corrected * extent / raw_range;
    /* Keep both normal and software-mirrored coordinates inside the panel. */
    if (pixel == 0) {
        pixel = 1;
    }
    if (pixel >= extent) {
        pixel = extent - 1;
    }
    return (uint16_t)pixel;
}

static uint64_t gsl3680_distance(const esp_lcd_touch_point_data_t *a, const esp_lcd_touch_point_data_t *b)
{
    int64_t dx = (int32_t)a->x - b->x;
    int64_t dy = (int32_t)a->y - b->y;
    return (uint64_t)(dx * dx + dy * dy);
}

/* Match positions ourselves: do not assume the firmware's ID nibble is valid.
 * Keep surviving contacts in their previous order, including LVGL's primary
 * pointer. This small tracker does not reproduce Silead's prediction, crossing,
 * nonlinear correction, edge compensation or adaptive tuning algorithm. */
static uint8_t gsl3680_track_points(esp_lcd_touch_handle_t tp,
                                   esp_lcd_touch_point_data_t *current, uint8_t count,
                                   esp_lcd_touch_point_data_t *next)
{
    esp_lcd_touch_point_data_t previous[GSL3680_MAX_POINTS];
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t previous_count = tp->data.points;
    memcpy(previous, tp->data.coords, previous_count * sizeof(previous[0]));
    portEXIT_CRITICAL(&tp->data.lock);

    int8_t matches[GSL3680_MAX_POINTS];
    memset(matches, -1, sizeof(matches));
    bool taken[GSL3680_MAX_CONTACTS] = {0};
    uint16_t extent = tp->config.x_max > tp->config.y_max ? tp->config.x_max : tp->config.y_max;
    uint32_t radius = extent / 8;
    if (radius < 16) {
        radius = 16;
    }
    uint64_t limit = (uint64_t)radius * radius;
    for (;;) {
        uint64_t best = limit + 1;
        int old_index = -1;
        int new_index = -1;
        for (uint8_t i = 0; i < previous_count; ++i) {
            if (matches[i] >= 0) {
                continue;
            }
            for (uint8_t j = 0; j < count; ++j) {
                if (!taken[j]) {
                    uint64_t distance = gsl3680_distance(&previous[i], &current[j]);
                    if (distance < best) {
                        best = distance;
                        old_index = i;
                        new_index = j;
                    }
                }
            }
        }
        if (old_index < 0) {
            break;
        }
        matches[old_index] = new_index;
        taken[new_index] = true;
    }

    uint8_t next_count = 0;
    uint16_t reserved_ids = 0;
    uint16_t active_ids = 0;
    for (uint8_t i = 0; i < previous_count; ++i) {
        reserved_ids |= 1U << previous[i].track_id;
        if (matches[i] >= 0) {
            esp_lcd_touch_point_data_t point = current[matches[i]];
            point.track_id = previous[i].track_id;
            /* Only smooth existing contacts. New touches and releases are
             * reported immediately, without invented samples. */
            point.x = (3U * point.x + previous[i].x + 2) / 4;
            point.y = (3U * point.y + previous[i].y + 2) / 4;
            next[next_count++] = point;
            active_ids |= 1U << point.track_id;
        }
    }
    for (uint8_t j = 0; j < count && next_count < GSL3680_MAX_POINTS; ++j) {
        if (taken[j]) {
            continue;
        }
        uint8_t id = 1;
        while (id <= GSL3680_MAX_CONTACTS && (reserved_ids & (1U << id))) {
            ++id;
        }
        if (id > GSL3680_MAX_CONTACTS) {
            /* A full old frame may leave no fresh IDs. Reuse a released ID. */
            id = 1;
            while (active_ids & (1U << id)) {
                ++id;
            }
        }
        next[next_count] = current[j];
        next[next_count++].track_id = id;
        reserved_ids |= 1U << id;
        active_ids |= 1U << id;
    }
    return next_count;
}

static esp_err_t gsl3680_read_data(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);
    uint8_t packet[GSL3680_PACKET_SIZE];
    esp_err_t ret = esp_lcd_panel_io_rx_param(tp->io, GSL3680_DATA_REG, packet, sizeof(packet));
    if (ret != ESP_OK) {
        gsl3680_clear_points(tp);
        return ret;
    }
    if (packet[0] > GSL3680_MAX_CONTACTS) {
        gsl3680_clear_points(tp);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_lcd_touch_point_data_t current[GSL3680_MAX_CONTACTS] = {0};
    esp_lcd_touch_point_data_t next[GSL3680_MAX_POINTS] = {0};
    uint8_t count = 0;
    for (uint8_t i = 0; i < packet[0]; ++i) {
        const uint8_t *p = packet + 4 + 4 * i;
        /* The high Y bits are flags/key data, not touchscreen coordinates. */
        if (p[1] & 0xf0) {
            continue;
        }
        uint16_t raw_y = p[0] | ((uint16_t)(p[1] & 0x0f) << 8);
        uint16_t raw_x = p[2] | ((uint16_t)(p[3] & 0x0f) << 8);
        current[count].x = gsl3680_scale_coordinate((int32_t)raw_y - GSL3680_RAW_Y_OFFSET,
                                                   GSL3680_RAW_Y_GAIN, GSL3680_RAW_Y_RANGE, tp->config.x_max);
        current[count].y = gsl3680_scale_coordinate(raw_x, GSL3680_RAW_X_GAIN,
                                                   GSL3680_RAW_X_RANGE, tp->config.y_max);
        ++count;
    }
    uint8_t next_count = gsl3680_track_points(tp, current, count, next);
    portENTER_CRITICAL(&tp->data.lock);
    memcpy(tp->data.coords, next, next_count * sizeof(next[0]));
    memset(tp->data.coords + next_count, 0, (CONFIG_ESP_LCD_TOUCH_MAX_POINTS - next_count) * sizeof(next[0]));
    tp->data.points = next_count;
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool gsl3680_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                          uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    assert(tp != NULL && x != NULL && y != NULL && point_num != NULL && max_point_num > 0);
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points < max_point_num ? tp->data.points : max_point_num;
    for (uint8_t i = 0; i < *point_num; ++i) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = 0; /* This packet format does not expose pressure. */
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t gsl3680_get_track_id(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    ESP_RETURN_ON_FALSE(tp != NULL && track_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Missing track ID buffer");
    portENTER_CRITICAL(&tp->data.lock);
    if (point_num > tp->data.points) {
        portEXIT_CRITICAL(&tp->data.lock);
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < point_num; ++i) {
        track_id[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static esp_err_t gsl3680_enter_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp->config.rst_gpio_num != GPIO_NUM_NC, ESP_ERR_NOT_SUPPORTED, TAG, "Sleep requires reset GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "Sleep failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    gsl3680_clear_points(tp);
    return ESP_OK;
}

static esp_err_t gsl3680_exit_sleep(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_FALSE(tp->config.rst_gpio_num != GPIO_NUM_NC, ESP_ERR_NOT_SUPPORTED, TAG, "Wake requires reset GPIO");
    gsl3680_clear_points(tp);
    /* Reset-based sleep may lose RAM firmware. Reload before reporting data. */
    ESP_RETURN_ON_ERROR(gsl3680_init(tp), TAG, "Wake initialization failed");
    return gsl3680_check_firmware(tp);
}

static esp_err_t gsl3680_del(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t gsl3680_reset_registers(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0xe0, 0x88), TAG, "Reset write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0xe4, 0x04), TAG, "Clock write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t zero[4] = {0};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(tp->io, 0xbc, zero, sizeof(zero)), TAG, "Power write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t gsl3680_startup(esp_lcd_touch_handle_t tp)
{
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0xe0, 0x00), TAG, "Startup failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t gsl3680_init(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "Reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "Reset release failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0x88, 0x01), TAG, "Clear register failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0xe4, 0x04), TAG, "Clock write failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl3680_write(tp, 0xe0, 0x00), TAG, "Clear reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gsl3680_reset_registers(tp), TAG, "Controller reset failed");

    for (size_t i = 0; i < sizeof(GSLX680_FW) / sizeof(GSLX680_FW[0]); ++i) {
        uint8_t offset = (uint8_t)GSLX680_FW[i].offset;
        uint32_t value = GSLX680_FW[i].val;
        uint8_t bytes[4] = {value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff};
        /* Preserve the existing firmware loader's one-byte page writes. */
        size_t length = offset == 0xf0 ? 1 : sizeof(bytes);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(tp->io, offset, bytes, length), TAG, "Firmware write failed");
    }
    ESP_RETURN_ON_ERROR(gsl3680_startup(tp), TAG, "Controller startup failed");
    ESP_RETURN_ON_ERROR(gsl3680_reset_registers(tp), TAG, "Controller reset failed");
    return gsl3680_startup(tp);
}

static esp_err_t gsl3680_check_firmware(esp_lcd_touch_handle_t tp)
{
    uint8_t status[4];
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, 0xb0, status, sizeof(status)), TAG, "Firmware status read failed");
    ESP_RETURN_ON_FALSE(status[0] == 0x5a && status[1] == 0x5a && status[2] == 0x5a && status[3] == 0x5a,
                        ESP_FAIL, TAG, "Invalid firmware status");
    return ESP_OK;
}
