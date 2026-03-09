#include "channel_mapper.h"

static constexpr uint8_t OFFS_EVEN[8][3] = {
    {0, 1, 2},    {3, 4, 5},    {6, 7, 8},    {9, 10, 11},
    {12, 13, 14}, {15, 23, 22}, {21, 20, 19}, {18, 17, 16},
};

static constexpr uint8_t OFFS_ODD[8][3] = {
    {8, 9, 10},   {11, 12, 13}, {14, 15, 16}, {17, 18, 19},
    {20, 21, 22}, {23, 0, 1},   {2, 3, 4},    {5, 6, 7},
};

namespace {

static uint16_t g_map[8][8][3];
static bool g_mapInitialized = false;

static uint16_t channelFromMapped(uint8_t mx, uint8_t my, uint8_t hwColor) {
  if (mx > 7 || my > 7 || hwColor > 2) {
    return 0;
  }

  uint16_t base = 1 + (uint16_t)(my >> 1) * 48 + (uint16_t)(my & 1) * 24;
  if ((my & 1u) == 0u) {
    return (uint16_t)(base + OFFS_EVEN[mx][hwColor]);
  }
  return (uint16_t)(base + OFFS_ODD[mx][hwColor]);
}

static uint16_t rawChannelForLogical(uint8_t x, uint8_t y, uint8_t hwColor) {
  if (x > 7 || y > 7 || hwColor > 2) {
    return 0;
  }
  uint8_t mx = y;
  uint8_t my = (uint8_t)(7 - x);
  return channelFromMapped(mx, my, hwColor);
}

static void buildDefaultMap() {
  for (uint8_t x = 0; x < 8; x++) {
    for (uint8_t y = 0; y < 8; y++) {
      // Baseline logical mapping:
      // R -> raw 0
      // G -> raw 2 sampled from previous Y (observed +1 shift compensation)
      // B -> raw 1
      g_map[x][y][0] = rawChannelForLogical(x, y, 0);
      g_map[x][y][1] = rawChannelForLogical(x, (uint8_t)((y + 7) & 0x07), 2);
      g_map[x][y][2] = rawChannelForLogical(x, y, 1);
    }
  }

  // Hardware defect workaround:
  // (0,0,*) has no clean red/green output.
  g_map[0][0][0] = 0;
  g_map[0][0][1] = 0;
}

static void ensureMapInitialized() {
  if (!g_mapInitialized) {
    buildDefaultMap();
    g_mapInitialized = true;
  }
}

} // namespace

uint16_t channelFor(uint8_t x, uint8_t y, uint8_t color) {
  if (x > 7 || y > 7 || color > 2) {
    return 0;
  }
  ensureMapInitialized();
  return g_map[x][y][color];
}

uint16_t channelForRaw(uint8_t x, uint8_t y, uint8_t hwColor) {
  return rawChannelForLogical(x, y, hwColor);
}

uint16_t channelMapGet(uint8_t x, uint8_t y, uint8_t color) {
  if (x > 7 || y > 7 || color > 2) {
    return 0;
  }
  ensureMapInitialized();
  return g_map[x][y][color];
}

bool channelMapSet(uint8_t x, uint8_t y, uint8_t color, uint16_t ch) {
  if (x > 7 || y > 7 || color > 2 || ch > 192) {
    return false;
  }
  ensureMapInitialized();
  g_map[x][y][color] = ch;
  return true;
}

void channelMapResetDefaults() {
  buildDefaultMap();
  g_mapInitialized = true;
}
