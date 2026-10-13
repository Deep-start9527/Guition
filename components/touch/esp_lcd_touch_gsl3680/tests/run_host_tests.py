#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the actual driver and touch framework against simulated IDF IO."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

COMPONENT = Path(__file__).resolve().parents[1]
STUB_HEADERS = (
    "sdkconfig.h", "freertos/FreeRTOS.h", "freertos/task.h",
    "freertos/semphr.h", "driver/gpio.h", "esp_err.h", "esp_heap_caps.h",
    "esp_lcd_panel_io.h", "esp_log.h", "esp_check.h", "esp_system.h",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--touch-component", type=Path, required=True,
                        help="Path to espressif/esp_lcd_touch 1.2.1")
    framework = parser.parse_args().touch_component.resolve()
    if not (framework / "esp_lcd_touch.c").is_file() or not (framework / "include/esp_lcd_touch.h").is_file():
        parser.error("--touch-component must contain esp_lcd_touch.c and include/esp_lcd_touch.h")
    compiler = os.environ.get("CC", "gcc")
    with tempfile.TemporaryDirectory(prefix="gsl3680-host-") as temp:
        work = Path(temp)
        for header in STUB_HEADERS:
            stub = work / header
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.write_text('#include "host_platform.h"\n')
        for capacity in (1, 5, 10, 12):
            binary = work / f"test-{capacity}"
            command = [
                compiler, "-std=gnu2x", "-g", "-O1", "-Wall", "-Wextra",
                "-Werror", "-Wno-deprecated-declarations",
                "-fsigned-char" if capacity == 5 else "-funsigned-char",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-fno-pie", "-no-pie", f"-DCONFIG_ESP_LCD_TOUCH_MAX_POINTS={capacity}",
                "-I", str(work), "-I", str(COMPONENT / "tests"),
                "-I", str(COMPONENT / "include"),
                "-I", str(COMPONENT / "private_include"),
                "-I", str(framework / "include"),
                str(COMPONENT / "esp_lcd_touch_gsl3680.c"),
                str(framework / "esp_lcd_touch.c"),
                str(COMPONENT / "tests/test_driver.c"), "-o", str(binary),
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
