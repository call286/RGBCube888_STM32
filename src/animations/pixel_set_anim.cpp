#include "pixel_set_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

namespace {

static uint8_t voxels[8][8][8]; // bit0=R, bit1=G, bit2=B, 0=off
static bool swapRG = false;

static bool inRange(uint8_t x, uint8_t y, uint8_t z) {
  return x < 8 && y < 8 && z < 8;
}

static uint8_t rgbToMask(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t mask = (r ? 0x01u : 0u) | (g ? 0x02u : 0u) | (b ? 0x04u : 0u);
  if (mask == 0) {
    mask = 0x04u; // Default to blue if all channels are 0.
  }
  return mask;
}

static void setVoxelColor(uint8_t x, uint8_t y, uint8_t z) {
  uint8_t c = voxels[z][y][x];
  uint8_t colorR = swapRG ? 1 : 0;
  uint8_t colorG = swapRG ? 0 : 1;

  if (c & 0x01u) {
    setChanInBack(z, channelFor(x, y, colorR), true);
  }
  if (c & 0x02u) {
    setChanInBack(z, channelFor(x, y, colorG), true);
  }
  if (c & 0x04u)
    setChanInBack(z, channelFor(x, y, 2), true);
}

} // namespace

void pixelSetInit() { pixelSetClear(); }

void pixelSetRenderFrame() {
  clearBack();
  for (uint8_t z = 0; z < 8; z++) {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        if (voxels[z][y][x] != 0) {
          setVoxelColor(x, y, z);
        }
      }
    }
  }
}

void pixelSetToggleSwapRG() {
  swapRG = !swapRG;
}

void pixelSetClear() {
  for (uint8_t z = 0; z < 8; z++) {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        voxels[z][y][x] = 0;
      }
    }
  }
}

bool pixelSetAdd(uint8_t x, uint8_t y, uint8_t z, uint8_t r, uint8_t g, uint8_t b) {
  if (!inRange(x, y, z)) {
    return false;
  }
  voxels[z][y][x] = rgbToMask(r, g, b);
  return true;
}

bool pixelSetRemove(uint8_t x, uint8_t y, uint8_t z) {
  if (!inRange(x, y, z)) {
    return false;
  }
  voxels[z][y][x] = 0;
  return true;
}

uint16_t pixelSetGetCount() {
  uint16_t c = 0;
  for (uint8_t z = 0; z < 8; z++) {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        if (voxels[z][y][x] != 0) {
          c++;
        }
      }
    }
  }
  return c;
}

PixelSetMoveResult pixelSetTranslate(int8_t dx, int8_t dy, int8_t dz) {
  uint8_t moved[8][8][8] = {{{0}}};
  PixelSetMoveResult res = {0, 0};

  for (uint8_t z = 0; z < 8; z++) {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        if (voxels[z][y][x] == 0) {
          continue;
        }

        int8_t nx = (int8_t)x + dx;
        int8_t ny = (int8_t)y + dy;
        int8_t nz = (int8_t)z + dz;

        if (nx < 0 || nx > 7 || ny < 0 || ny > 7 || nz < 0 || nz > 7) {
          res.dropped++;
          continue;
        }

        uint8_t &dst = moved[(uint8_t)nz][(uint8_t)ny][(uint8_t)nx];
        if (dst == 0) {
          res.kept++;
        }
        dst |= voxels[z][y][x];
      }
    }
  }

  for (uint8_t z = 0; z < 8; z++) {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        voxels[z][y][x] = moved[z][y][x];
      }
    }
  }

  return res;
}

bool pixelSetPaintPlane(char axis, uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index > 7) {
    return false;
  }

  uint8_t mask = rgbToMask(r, g, b);
  if (axis == 'x') {
    for (uint8_t z = 0; z < 8; z++) {
      for (uint8_t y = 0; y < 8; y++) {
        voxels[z][y][index] = mask;
      }
    }
    return true;
  }
  if (axis == 'y') {
    for (uint8_t z = 0; z < 8; z++) {
      for (uint8_t x = 0; x < 8; x++) {
        voxels[z][index][x] = mask;
      }
    }
    return true;
  }
  if (axis == 'z') {
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        voxels[index][y][x] = mask;
      }
    }
    return true;
  }
  return false;
}

bool pixelSetPaintLine(char axis, uint8_t a, uint8_t b, uint8_t r, uint8_t g, uint8_t bColor) {
  if (a > 7 || b > 7) {
    return false;
  }

  uint8_t mask = rgbToMask(r, g, bColor);
  if (axis == 'x') {
    for (uint8_t x = 0; x < 8; x++) {
      voxels[b][a][x] = mask;
    }
    return true;
  }
  if (axis == 'y') {
    for (uint8_t y = 0; y < 8; y++) {
      voxels[b][y][a] = mask;
    }
    return true;
  }
  if (axis == 'z') {
    for (uint8_t z = 0; z < 8; z++) {
      voxels[z][b][a] = mask;
    }
    return true;
  }
  return false;
}
