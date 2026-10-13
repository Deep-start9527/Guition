/* SPDX-License-Identifier: MIT */
/* Minimal host replacements for IDF hardware APIs. The real touch API header
 * and real esp_lcd_touch.c are compiled by run_host_tests.py. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

#ifndef CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#endif
#define CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS 1

typedef struct { int depth; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(lock) (++(lock)->depth)
#define portEXIT_CRITICAL(lock) do { assert((lock)->depth > 0); --(lock)->depth; } while (0)
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }

#define MALLOC_CAP_DEFAULT 0
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);

typedef int gpio_num_t;
typedef void (*gpio_isr_t)(void *);
#define GPIO_NUM_NC (-1)
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_INTR_NEGEDGE 1
#define GPIO_INTR_POSEDGE 2
#define BIT64(n) (UINT64_C(1) << (n))
typedef struct {
    uint64_t pin_bit_mask;
    unsigned mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, unsigned value);
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t callback, void *arg);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);

typedef void *esp_lcd_panel_io_handle_t;
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int reg, void *data, size_t size);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int reg, const void *data, size_t size);

#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_FALSE(cond, value, tag, ...) do { if (!(cond)) { ESP_LOGE(tag, __VA_ARGS__); return (value); } } while (0)
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { esp_err_t host_err = (expr); if (host_err != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return host_err; } } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, tag, ...) do { esp_err_t host_err = (expr); if (host_err != ESP_OK) { ret = host_err; ESP_LOGE(tag, __VA_ARGS__); goto label; } } while (0)
