<!-- SPDX-License-Identifier: MIT -->
# Changelog

## Unreleased

- Follow the GT911 component layout and standard per-handle touch API.
- Delete the unused Silead algorithm, its private headers, GPL license,
  duplicate MIT text and stale distribution snapshots.
- Remove the unused `esp_while_read()` API and global instance restriction.
- Retain firmware loading, coordinate mapping, software tracking and sleep.
- Simplify usage and licensing documents; software uses MIT, with firmware
  provenance recorded separately in NOTICE.
- Make the host test framework path explicit for standalone component use.

## 0.1.0 — 2026-10-12

- Prepare `guition/esp_lcd_touch_gsl3680` as a standalone registry component.
- Add explicit public touch and private IDF build dependencies.
- Move the original firmware and implementation headers out of the public API.
- Preserve the supplied firmware, calibration parameters and Silead algorithm.
- Add a polling example, usage instructions and scoped licensing notices.
- Scope initial support to ESP32-P4 with ESP-IDF 6.1.x and touch 1.2.1.
