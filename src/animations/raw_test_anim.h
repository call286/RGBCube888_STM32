#pragma once

#include <Arduino.h>

void rawTestInit();
void rawTestRenderFrame();

void rawTestClear();
void rawTestSetOnlyChannel(uint8_t z, uint16_t ch);
void rawTestSetOnlyMapped(uint8_t x, uint8_t y, uint8_t z, uint8_t color);
void rawTestSetOnlyRawLogical(uint8_t x, uint8_t y, uint8_t z, uint8_t hwColor);
