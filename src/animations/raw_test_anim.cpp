#include "raw_test_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

namespace {

static bool enabled[8][193];

static bool inRange(uint8_t z, uint16_t ch) {
  return z < 8 && ch >= 1 && ch <= 192;
}

} // namespace

void rawTestInit() { rawTestClear(); }

void rawTestRenderFrame() {
  clearBack();
  for (uint8_t z = 0; z < 8; z++) {
    for (uint16_t ch = 1; ch <= 192; ch++) {
      if (enabled[z][ch]) {
        setChanInBack(z, ch, true);
      }
    }
  }
}

void rawTestClear() {
  for (uint8_t z = 0; z < 8; z++) {
    for (uint16_t ch = 1; ch <= 192; ch++) {
      enabled[z][ch] = false;
    }
  }
}

void rawTestSetOnlyChannel(uint8_t z, uint16_t ch) {
  rawTestClear();
  if (inRange(z, ch)) {
    enabled[z][ch] = true;
  }
}

void rawTestSetOnlyMapped(uint8_t x, uint8_t y, uint8_t z, uint8_t color) {
  if (x > 7 || y > 7 || z > 7 || color > 2) {
    rawTestClear();
    return;
  }
  rawTestSetOnlyChannel(z, channelFor(x, y, color));
}

void rawTestSetOnlyRawLogical(uint8_t x, uint8_t y, uint8_t z, uint8_t hwColor) {
  if (x > 7 || y > 7 || z > 7 || hwColor > 2) {
    rawTestClear();
    return;
  }
  rawTestSetOnlyChannel(z, channelForRaw(x, y, hwColor));
}
