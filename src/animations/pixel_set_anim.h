#pragma once

#include <Arduino.h>

struct PixelSetMoveResult {
  uint16_t kept;
  uint16_t dropped;
};

void pixelSetInit();
void pixelSetRenderFrame();
void pixelSetToggleSwapRG();

void pixelSetClear();
bool pixelSetAdd(uint8_t x, uint8_t y, uint8_t z, uint8_t r, uint8_t g, uint8_t b);
bool pixelSetRemove(uint8_t x, uint8_t y, uint8_t z);
uint16_t pixelSetGetCount();
PixelSetMoveResult pixelSetTranslate(int8_t dx, int8_t dy, int8_t dz);
bool pixelSetPaintPlane(char axis, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
bool pixelSetPaintLine(char axis, uint8_t a, uint8_t b, uint8_t r, uint8_t g, uint8_t bColor);
