<!-- SPDX-License-Identifier: MIT -->

# ESP LCD Touch GSL3680 Controller

[简体中文](README.md) | **English**

GSL3680 I2C driver for the Guition JC8012P4A1C panel, using `esp_lcd_touch`.
Supports ESP-IDF 6.1.x and `espressif/esp_lcd_touch` 1.2.1.
The bundled firmware and coordinate mapping are specific to this panel.

## Add to project

Copy this directory to your project's `components/esp_lcd_touch_gsl3680`.
The component manager resolves the dependencies in `idf_component.yml`.

## Example use

Create an I2C master bus (JC8012P4A1C: SDA 7, SCL 8), then initialize:

```c
#include "esp_lcd_touch_gsl3680.h"

esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
esp_lcd_panel_io_handle_t io = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &io));

esp_lcd_touch_config_t config = {
    .x_max = 800,
    .y_max = 1280,
    .rst_gpio_num = 22,
    .int_gpio_num = 21,
    .levels = { .reset = 0, .interrupt = 0 },
    .flags = { .mirror_x = true },
};
esp_lcd_touch_handle_t touch = NULL;
ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gsl3680(io, &config, &touch));
```

Poll regularly and retrieve the latest points:

```c
esp_lcd_touch_point_data_t points[3];
uint8_t count = 0;
ESP_ERROR_CHECK(esp_lcd_touch_read_data(touch));
ESP_ERROR_CHECK(esp_lcd_touch_get_data(touch, points, &count, 3));
```

The I2C address is 7-bit `0x40`, with a default clock of 100 kHz. The caller
owns the bus and IO: delete the touch handle before deleting either of them.
Serialize read/get calls per handle. The legacy coordinate getter also works.
See [examples/touch_polling](examples/touch_polling) for a complete application.

## Behavior

- Reads up to 10 contacts, bounded by `CONFIG_ESP_LCD_TOUCH_MAX_POINTS`.
- Maps the bundled firmware's sensor coordinates to `x_max`/`y_max`; the touch
  framework applies mirror/swap flags. Pressure is reported as zero.
- Uses lightweight software IDs and smoothing. This does not reproduce the
  original Silead algorithm; verify corners, dragging and multi-touch on hardware.
- Clears cached contacts on read errors, invalid packets and sleep. Waking
  through the reset GPIO reloads controller firmware.
