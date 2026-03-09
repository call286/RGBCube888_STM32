#include "wave_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

static uint8_t sqrtLUT[148];

static uint16_t phase = 0;
static uint8_t waveSpeed = 3;
static uint8_t waveScale = 22;
static uint8_t waveThreshold = 140;
static bool swapRG = true;

static void buildSqrtLUT() {
  for (uint16_t n = 0; n < 148; n++) {
    uint8_t r = 0;
    while ((uint16_t)(r + 1) * (uint16_t)(r + 1) <= n) {
      r++;
    }
    sqrtLUT[n] = r;
  }
}

static inline uint8_t tri8(uint8_t x) {
  uint8_t t = x & 0x7F;
  uint8_t up = (x & 0x80) ? (uint8_t)(127 - t) : t;
  return (uint8_t)(up << 1);
}

void waveInit() { buildSqrtLUT(); }

void waveRenderFrame() {
  clearBack();

  for (uint8_t z = 0; z < 8; z++) {
    int8_t dz2 = (int8_t)(z * 2 - 7);
    for (uint8_t y = 0; y < 8; y++) {
      int8_t dy2 = (int8_t)(y * 2 - 7);
      for (uint8_t x = 0; x < 8; x++) {
        int8_t dx2 = (int8_t)(x * 2 - 7);

        uint16_t dist2 = (uint16_t)(dx2 * dx2) + (uint16_t)(dy2 * dy2) +
                         (uint16_t)(dz2 * dz2);
        uint8_t dist = sqrtLUT[dist2];

        uint8_t p = (uint8_t)((phase + (uint16_t)dist * waveScale) & 0xFF);
        uint8_t v = tri8(p);

        if (v > waveThreshold) {
          uint16_t chR = channelFor(x, y, 0);
          uint16_t chG = channelFor(x, y, 1);
          uint16_t chB = channelFor(x, y, 2);

          if (swapRG) {
            uint16_t tmp = chR;
            chR = chG;
            chG = tmp;
          }

          setChanInBack(z, chR, true);
          setChanInBack(z, chG, true);
          setChanInBack(z, chB, true);
        }
      }
    }
  }

  phase += waveSpeed;
}

uint8_t waveGetSpeed() { return waveSpeed; }
uint8_t waveGetScale() { return waveScale; }
uint8_t waveGetThreshold() { return waveThreshold; }
bool waveGetSwapRG() { return swapRG; }

void waveSetSpeed(uint8_t v) { waveSpeed = v; }
void waveSetScale(uint8_t v) { waveScale = v; }
void waveSetThreshold(uint8_t v) { waveThreshold = v; }
void waveToggleSwapRG() { swapRG = !swapRG; }
