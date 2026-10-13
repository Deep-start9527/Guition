<!-- SPDX-License-Identifier: MIT -->

# ESP LCD Touch GSL3680 触摸驱动

**简体中文** | [English](README_EN.md)

基于 `esp_lcd_touch` 的 GSL3680 I2C 触摸驱动，适用于 Guition JC8012P4A1C 面板。
支持 ESP-IDF 6.1.x 和 `espressif/esp_lcd_touch` 1.2.1。
内置固件和坐标映射参数仅针对该面板。

## 添加到工程

将本目录复制到工程的 `components/esp_lcd_touch_gsl3680` 目录下。
组件管理器会根据 `idf_component.yml` 解析依赖。

## 使用示例

先创建 I2C 主机总线（JC8012P4A1C：SDA 7、SCL 8），然后初始化触摸驱动：

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

定期读取触摸数据并获取最新触点：

```c
esp_lcd_touch_point_data_t points[3];
uint8_t count = 0;
ESP_ERROR_CHECK(esp_lcd_touch_read_data(touch));
ESP_ERROR_CHECK(esp_lcd_touch_get_data(touch, points, &count, 3));
```

I2C 使用 7 位地址 `0x40`，默认时钟为 100 kHz。总线和 IO 由调用方管理，
释放时应先删除触摸句柄，再删除 IO 和总线。同一句柄的读取和获取操作应串行执行，
也支持旧版坐标获取接口。
完整应用请参阅 [examples/touch_polling](examples/touch_polling)。

## 驱动行为

- 最多读取 10 个触点，实际数量受 `CONFIG_ESP_LCD_TOUCH_MAX_POINTS` 限制。
- 将内置固件的传感器坐标映射到 `x_max`/`y_max`；镜像和轴交换由触摸框架处理。
  压力值固定为零。
- 使用轻量软件触点 ID 跟踪和平滑处理，不复现原 Silead 算法。
  请在实机上验证屏幕四角、拖动和多点触摸。
- 读取失败、数据包无效或进入休眠时清除缓存触点。
  通过复位 GPIO 唤醒时，会重新加载控制器固件。
