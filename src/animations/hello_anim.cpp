#include "hello_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

static const char WORD[] = "HELLO";
static const uint8_t WORD_LEN = 5;

// 5x7 bitmaps, bit0 is top row.
static const uint8_t GLYPH_H[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
static const uint8_t GLYPH_E[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
static const uint8_t GLYPH_L[5] = {0x7F, 0x40, 0x40, 0x40, 0x40};
static const uint8_t GLYPH_O[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};

static const uint8_t COLOR_PALETTE[6][3] = {
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};

enum HelloStage : uint8_t {
  STAGE_FLY_IN = 0,
  STAGE_HOLD = 1,
  STAGE_FLY_OUT = 2,
  STAGE_GAP = 3
};

static uint8_t spinStep = 7;
static uint8_t flyFrames = 16;
static uint8_t holdFrames = 10;
static uint8_t gapFrames = 4;
static bool swapRG = true;

static uint8_t letterIndex = 0;
static uint8_t stage = STAGE_FLY_IN;
static uint16_t stageTick = 0;
static uint8_t angle = 0;

static const uint8_t SIN_LUT_Q7[16] = {
    0,  49,  90, 117, 127, 117, 90, 49,
    0,  207, 166, 139, 129, 139, 166, 207};

static int8_t sinQ7(uint8_t a) {
  uint8_t idx = (uint8_t)(a >> 4);
  return (int8_t)SIN_LUT_Q7[idx];
}

static int8_t cosQ7(uint8_t a) { return sinQ7((uint8_t)(a + 64)); }

static const uint8_t *glyphFor(char c) {
  if (c == 'H')
    return GLYPH_H;
  if (c == 'E')
    return GLYPH_E;
  if (c == 'L')
    return GLYPH_L;
  return GLYPH_O;
}

static int8_t interpLinear(int8_t a, int8_t b, uint16_t t, uint16_t total) {
  if (total == 0)
    return b;
  int16_t da = (int16_t)b - (int16_t)a;
  return (int8_t)(a + (int16_t)((int32_t)da * t / total));
}

static void setRgbVoxel(uint8_t x, uint8_t y, uint8_t z, uint8_t colorIdx) {
  uint16_t chR = channelFor(x, y, 0);
  uint16_t chG = channelFor(x, y, 1);
  uint16_t chB = channelFor(x, y, 2);

  bool onR = COLOR_PALETTE[colorIdx][0] != 0;
  bool onG = COLOR_PALETTE[colorIdx][1] != 0;
  bool onB = COLOR_PALETTE[colorIdx][2] != 0;

  if (swapRG) {
    bool tmp = onR;
    onR = onG;
    onG = tmp;
  }

  setChanInBack(z, chR, onR);
  setChanInBack(z, chG, onG);
  setChanInBack(z, chB, onB);
}

static void renderLetter(char c, int8_t y2Offset, uint8_t rotAngle, uint8_t colorIdx) {
  const uint8_t *glyph = glyphFor(c);
  int8_t s = sinQ7(rotAngle);
  int8_t cs = cosQ7(rotAngle);

  for (uint8_t gx = 0; gx < 5; gx++) {
    uint8_t col = glyph[gx];
    for (uint8_t gz = 0; gz < 7; gz++) {
      if (((col >> gz) & 0x01u) == 0)
        continue;

      int8_t x2 = (int8_t)((int16_t)gx * 2 - 4);
      int8_t z2 = (int8_t)((int16_t)gz * 2 - 6);
      int8_t y2 = y2Offset;

      int16_t xr2 = ((int16_t)x2 * cs - (int16_t)y2 * s) / 127;
      int16_t yr2 = ((int16_t)x2 * s + (int16_t)y2 * cs) / 127;

      int16_t xi = (xr2 + 8) / 2 + 1;
      int16_t yi = (yr2 + 8) / 2 + 1;
      int16_t zi = (z2 + 8) / 2;

      if (xi < 0 || xi > 7 || yi < 0 || yi > 7 || zi < 0 || zi > 7)
        continue;

      setRgbVoxel((uint8_t)xi, (uint8_t)yi, (uint8_t)zi, colorIdx);
    }
  }
}

void helloInit() {
  letterIndex = 0;
  stage = STAGE_FLY_IN;
  stageTick = 0;
  angle = 0;
}

void helloRenderFrame() {
  clearBack();

  char c = WORD[letterIndex];
  uint8_t color = letterIndex % 6;

  if (stage == STAGE_FLY_IN) {
    int8_t y2 = interpLinear(20, 0, stageTick, flyFrames);
    renderLetter(c, y2, angle, color);
    stageTick++;
    if (stageTick > flyFrames) {
      stageTick = 0;
      stage = STAGE_HOLD;
    }
  } else if (stage == STAGE_HOLD) {
    renderLetter(c, 0, angle, color);
    stageTick++;
    if (stageTick >= holdFrames) {
      stageTick = 0;
      stage = STAGE_FLY_OUT;
    }
  } else if (stage == STAGE_FLY_OUT) {
    int8_t y2 = interpLinear(0, -20, stageTick, flyFrames);
    renderLetter(c, y2, angle, color);
    stageTick++;
    if (stageTick > flyFrames) {
      stageTick = 0;
      stage = STAGE_GAP;
    }
  } else {
    stageTick++;
    if (stageTick >= gapFrames) {
      stageTick = 0;
      stage = STAGE_FLY_IN;
      letterIndex = (uint8_t)((letterIndex + 1) % WORD_LEN);
    }
  }

  angle = (uint8_t)(angle + spinStep);
}

uint8_t helloGetSpinStep() { return spinStep; }
uint8_t helloGetFlyFrames() { return flyFrames; }
uint8_t helloGetHoldFrames() { return holdFrames; }
uint8_t helloGetGapFrames() { return gapFrames; }
bool helloGetSwapRG() { return swapRG; }

void helloSetSpinStep(uint8_t v) {
  if (v < 1)
    v = 1;
  if (v > 24)
    v = 24;
  spinStep = v;
}

void helloSetFlyFrames(uint8_t v) {
  if (v < 4)
    v = 4;
  if (v > 40)
    v = 40;
  flyFrames = v;
}

void helloSetHoldFrames(uint8_t v) {
  if (v > 40)
    v = 40;
  holdFrames = v;
}

void helloSetGapFrames(uint8_t v) {
  if (v > 30)
    v = 30;
  gapFrames = v;
}

void helloToggleSwapRG() { swapRG = !swapRG; }
