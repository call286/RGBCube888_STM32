#pragma once

#include <Arduino.h>

uint16_t channelFor(uint8_t x, uint8_t y, uint8_t color);
uint16_t channelForRaw(uint8_t x, uint8_t y, uint8_t hwColor);
uint16_t channelMapGet(uint8_t x, uint8_t y, uint8_t color);
bool channelMapSet(uint8_t x, uint8_t y, uint8_t color, uint16_t ch);
void channelMapResetDefaults();
