#pragma once

#include <Arduino.h>

namespace CubeConfig {

// STM32F401 pin mapping (Arduino_Core_STM32 names)
static const uint8_t RGB_A0 = PA6;
static const uint8_t RGB_A1 = PA5;
static const uint8_t RGB_A2 = PA4;
static const uint8_t RGB_DAT_A = PB4;
static const uint8_t RGB_DAT_B = PB5;
static const uint8_t RGB_OE = PB0;
static const uint8_t RGB_CLK = PB6;
static const uint8_t RGB_ST = PB7;

// ESP8266 UART bridge on STM32 (separate project: rgbcube-esp-bridge)
static const uint8_t ESP_UART_RX = PA10; // STM RX <- ESP TX
static const uint8_t ESP_UART_TX = PA9;  // STM TX -> ESP RX
static const uint32_t ESP_UART_BAUD = 115200;
static const uint8_t IR_RX = PA8; // IR demodulator output (active LOW)

// SD card (SPI mode):
// PB12 is labeled "CD" on your table, but used here as SPI chip-select (CS).
static const uint8_t SD_SPI_CS = PB12;
static const uint8_t SD_SPI_SCK = PB13;
static const uint8_t SD_SPI_MISO = PB14;
static const uint8_t SD_SPI_MOSI = PB15;
static const uint8_t SD_SPI_MHZ = 12;

static const uint16_t FRAME_HZ = 120;
static const uint8_t LAYERS = 8;
static const uint8_t LATCH_EDGES = 0;
static const uint8_t BYTES_PER_CHAIN = 12;

// Global logical->physical color swizzle used by channel mapper.
// Current hardware behavior with LATCH_EDGES=0:
// logical R appears as G, logical G appears as B, logical B appears as R.
// This swizzle corrects that globally.
static const uint8_t COLOR_SWIZZLE_R = 2;
static const uint8_t COLOR_SWIZZLE_G = 0;
static const uint8_t COLOR_SWIZZLE_B = 1;

} // namespace CubeConfig
