#include "rain_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

static const uint8_t RAINBOW_COLORS = 7;
static const uint8_t MAX_DROPS = 64;

struct Drop {
  int8_t z;
  uint8_t x;
  uint8_t y;
  uint8_t hue;
  uint8_t cooldown;
  bool active;
};

// Binary RGB palette suitable for this cube driver.
static const uint8_t PALETTE[RAINBOW_COLORS][3] = {
    {1, 0, 0}, // red
    {1, 1, 0}, // yellow
    {0, 1, 0}, // green
    {0, 1, 1}, // cyan
    {0, 0, 1}, // blue
    {1, 0, 1}, // magenta
    {1, 1, 1}  // white
};

static Drop drops[MAX_DROPS];
static uint8_t dropCount = 16;
static uint8_t fallStepFrames = 2;
static uint8_t tailLength = 1;
static uint8_t rainbowStep = 1;
static uint8_t respawnDelay = 6;
static bool swapRG = true;

static uint8_t fallTick = 0;

static void spawnDrop(Drop &d) {
  d.x = (uint8_t)random(0, 8);
  d.y = (uint8_t)random(0, 8);
  d.z = 7;
  d.hue = (uint8_t)random(0, RAINBOW_COLORS);
  d.cooldown = 0;
  d.active = true;
}

static void renderVoxel(uint8_t x, uint8_t y, uint8_t z, uint8_t hue) {
  uint16_t chR = channelFor(x, y, 0);
  uint16_t chG = channelFor(x, y, 1);
  uint16_t chB = channelFor(x, y, 2);

  bool onR = PALETTE[hue][0] != 0;
  bool onG = PALETTE[hue][1] != 0;
  bool onB = PALETTE[hue][2] != 0;

  if (swapRG) {
    bool tmp = onR;
    onR = onG;
    onG = tmp;
  }

  setChanInBack(z, chR, onR);
  setChanInBack(z, chG, onG);
  setChanInBack(z, chB, onB);
}

void rainInit() {
  randomSeed(micros());
  for (uint8_t i = 0; i < MAX_DROPS; i++) {
    drops[i].active = false;
    drops[i].cooldown = 0;
  }
}

void rainRenderFrame() {
  clearBack();

  bool advance = false;
  fallTick++;
  if (fallTick >= fallStepFrames) {
    fallTick = 0;
    advance = true;
  }

  for (uint8_t i = 0; i < dropCount; i++) {
    Drop &d = drops[i];

    if (!d.active) {
      if (advance) {
        if (d.cooldown > 0) {
          d.cooldown--;
        }
        if (d.cooldown == 0) {
          spawnDrop(d);
        }
      }
      continue;
    }

    if (advance) {
      d.z--;
      d.hue = (uint8_t)((d.hue + rainbowStep) % RAINBOW_COLORS);
      if (d.z < 0) {
        d.active = false;
        d.cooldown = (uint8_t)random(0, (long)respawnDelay + 1L);
        continue;
      }
    }

    for (uint8_t t = 0; t <= tailLength; t++) {
      int8_t tz = (int8_t)(d.z + t);
      if (tz < 0 || tz > 7) {
        continue;
      }
      uint8_t hue = (uint8_t)((d.hue + t) % RAINBOW_COLORS);
      renderVoxel(d.x, d.y, (uint8_t)tz, hue);
    }
  }
}

uint8_t rainGetDropCount() { return dropCount; }
uint8_t rainGetFallStepFrames() { return fallStepFrames; }
uint8_t rainGetTailLength() { return tailLength; }
uint8_t rainGetRainbowStep() { return rainbowStep; }
uint8_t rainGetRespawnDelay() { return respawnDelay; }
bool rainGetSwapRG() { return swapRG; }

void rainSetDropCount(uint8_t v) {
  if (v < 1)
    v = 1;
  if (v > MAX_DROPS)
    v = MAX_DROPS;
  dropCount = v;
}

void rainSetFallStepFrames(uint8_t v) {
  if (v < 1)
    v = 1;
  if (v > 12)
    v = 12;
  fallStepFrames = v;
}

void rainSetTailLength(uint8_t v) {
  if (v > 7)
    v = 7;
  tailLength = v;
}

void rainSetRainbowStep(uint8_t v) {
  if (v < 1)
    v = 1;
  if (v > 6)
    v = 6;
  rainbowStep = v;
}

void rainSetRespawnDelay(uint8_t v) {
  if (v > 20)
    v = 20;
  respawnDelay = v;
}

void rainToggleSwapRG() { swapRG = !swapRG; }
