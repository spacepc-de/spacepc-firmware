// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

namespace Board {
constexpr uint16_t DISPLAY_WIDTH = 800;
constexpr uint16_t DISPLAY_HEIGHT = 480;

// Good Display ESP32-L + DESPI-C02 + GDEY075Z08
constexpr uint8_t EPD_BUSY = 13;
constexpr uint8_t EPD_RST = 12;
constexpr uint8_t EPD_DC = 14;
constexpr uint8_t EPD_CS = 27;
constexpr uint8_t EPD_SCK = 18;
constexpr uint8_t EPD_MISO = 19;
constexpr uint8_t EPD_MOSI = 23;
}  // namespace Board

