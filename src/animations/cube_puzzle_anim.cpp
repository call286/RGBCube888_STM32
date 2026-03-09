#include "cube_puzzle_anim.h"

#include "../channel_mapper.h"
#include "../refresh_engine.h"

namespace {

static const uint8_t CUBE_COUNT = 6;
static const uint8_t SLOT_COUNT = 8;
static const uint8_t EMPTY_COUNT = 2;
static const uint8_t NO_CUBE = 0xFF;
static const uint8_t MOVE_FRAMES = 8;
static const uint8_t HOLD_FRAMES = 10;

// Binary RGB palette for this driver (no black).
static const uint8_t PALETTE[7][3] = {
    {1, 0, 0}, // red
    {0, 1, 0}, // green
    {0, 0, 1}, // blue
    {1, 1, 0}, // yellow
    {1, 0, 1}, // magenta
    {0, 1, 1}, // cyan
    {1, 1, 1}  // white
};

struct CubeState {
  uint8_t slot;
  uint8_t color;
};

static CubeState cubes[CUBE_COUNT];
static uint8_t occupiedBy[SLOT_COUNT];
static uint8_t emptySlots[EMPTY_COUNT];

static bool swapRG = true;

static bool moving = false;
static uint8_t movingCube = NO_CUBE;
static uint8_t fromSlot = 0;
static uint8_t toSlot = 0;
static uint8_t moveFrame = 0;
static uint8_t holdTick = 0;

static inline uint8_t slotX(uint8_t s) { return s & 1u; }
static inline uint8_t slotY(uint8_t s) { return (s >> 1) & 1u; }
static inline uint8_t slotZ(uint8_t s) { return (s >> 2) & 1u; }

static bool isNeighbor(uint8_t a, uint8_t b) {
  int8_t dx = (int8_t)slotX(a) - (int8_t)slotX(b);
  int8_t dy = (int8_t)slotY(a) - (int8_t)slotY(b);
  int8_t dz = (int8_t)slotZ(a) - (int8_t)slotZ(b);
  if (dx < 0)
    dx = (int8_t)-dx;
  if (dy < 0)
    dy = (int8_t)-dy;
  if (dz < 0)
    dz = (int8_t)-dz;
  return (dx + dy + dz) == 1;
}

static void setRgbVoxel(uint8_t x, uint8_t y, uint8_t z, uint8_t colorIdx) {
  uint16_t chR = channelFor(x, y, 0);
  uint16_t chG = channelFor(x, y, 1);
  uint16_t chB = channelFor(x, y, 2);

  bool onR = PALETTE[colorIdx][0] != 0;
  bool onG = PALETTE[colorIdx][1] != 0;
  bool onB = PALETTE[colorIdx][2] != 0;

  if (swapRG) {
    bool tmp = onR;
    onR = onG;
    onG = tmp;
  }

  setChanInBack(z, chR, onR);
  setChanInBack(z, chG, onG);
  setChanInBack(z, chB, onB);
}

static void renderBlockAtBase(uint8_t baseX, uint8_t baseY, uint8_t baseZ, uint8_t colorIdx) {
  for (uint8_t z = 0; z < 4; z++) {
    for (uint8_t y = 0; y < 4; y++) {
      for (uint8_t x = 0; x < 4; x++) {
        setRgbVoxel((uint8_t)(baseX + x), (uint8_t)(baseY + y), (uint8_t)(baseZ + z), colorIdx);
      }
    }
  }
}

static void renderCubeInSlot(uint8_t cubeIndex, uint8_t slot) {
  uint8_t bx = (uint8_t)(slotX(slot) * 4);
  uint8_t by = (uint8_t)(slotY(slot) * 4);
  uint8_t bz = (uint8_t)(slotZ(slot) * 4);
  renderBlockAtBase(bx, by, bz, cubes[cubeIndex].color);
}

static void renderMovingCube() {
  uint8_t fx = (uint8_t)(slotX(fromSlot) * 4);
  uint8_t fy = (uint8_t)(slotY(fromSlot) * 4);
  uint8_t fz = (uint8_t)(slotZ(fromSlot) * 4);
  uint8_t tx = (uint8_t)(slotX(toSlot) * 4);
  uint8_t ty = (uint8_t)(slotY(toSlot) * 4);
  uint8_t tz = (uint8_t)(slotZ(toSlot) * 4);

  // tQ8: 0..256 over the move, then smoothstep easing for softer slide.
  uint16_t tQ8 = (uint16_t)((uint16_t)moveFrame * 256U / MOVE_FRAMES);
  if (tQ8 > 256U) {
    tQ8 = 256U;
  }
  uint32_t t2 = (uint32_t)tQ8 * (uint32_t)tQ8;
  uint16_t smoothQ8 = (uint16_t)((t2 * (uint32_t)(768U - 2U * tQ8)) >> 16); // 3t^2-2t^3

  int16_t bx = (int16_t)fx + (int16_t)(((int16_t)tx - (int16_t)fx) * (int16_t)smoothQ8 + 128) / 256;
  int16_t by = (int16_t)fy + (int16_t)(((int16_t)ty - (int16_t)fy) * (int16_t)smoothQ8 + 128) / 256;
  int16_t bz = (int16_t)fz + (int16_t)(((int16_t)tz - (int16_t)fz) * (int16_t)smoothQ8 + 128) / 256;

  renderBlockAtBase((uint8_t)bx, (uint8_t)by, (uint8_t)bz, cubes[movingCube].color);
}

static void shuffle(uint8_t *arr, uint8_t n) {
  for (int8_t i = (int8_t)n - 1; i > 0; i--) {
    uint8_t j = (uint8_t)random(0, i + 1);
    uint8_t t = arr[i];
    arr[i] = arr[j];
    arr[j] = t;
  }
}

static void startRandomMove() {
  uint8_t emptyOrder[EMPTY_COUNT] = {0, 1};
  shuffle(emptyOrder, EMPTY_COUNT);

  for (uint8_t eo = 0; eo < EMPTY_COUNT; eo++) {
    uint8_t targetEmptySlot = emptySlots[emptyOrder[eo]];

    uint8_t candidates[3];
    uint8_t cc = 0;
    for (uint8_t c = 0; c < CUBE_COUNT; c++) {
      uint8_t s = cubes[c].slot;
      if (isNeighbor(s, targetEmptySlot)) {
        candidates[cc++] = c;
      }
    }

    if (cc == 0) {
      continue;
    }

    movingCube = candidates[random(0, cc)];
    fromSlot = cubes[movingCube].slot;
    toSlot = targetEmptySlot;
    moveFrame = 0;
    moving = true;
    return;
  }
}

static void finalizeMove() {
  occupiedBy[fromSlot] = NO_CUBE;
  occupiedBy[toSlot] = movingCube;
  cubes[movingCube].slot = toSlot;

  // Color stays stable while moving; it may change only after a completed move.
  if (random(0, 4) == 0) {
    cubes[movingCube].color = (uint8_t)random(0, 7);
  }

  for (uint8_t i = 0; i < EMPTY_COUNT; i++) {
    if (emptySlots[i] == toSlot) {
      emptySlots[i] = fromSlot;
      break;
    }
  }

  moving = false;
  movingCube = NO_CUBE;
  holdTick = 0;
}

} // namespace

void cubePuzzleInit() {
  randomSeed(micros());

  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    occupiedBy[i] = NO_CUBE;
  }

  uint8_t slots[SLOT_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7};
  shuffle(slots, SLOT_COUNT);

  for (uint8_t c = 0; c < CUBE_COUNT; c++) {
    cubes[c].slot = slots[c];
    cubes[c].color = (uint8_t)random(0, 7);
    occupiedBy[cubes[c].slot] = c;
  }

  emptySlots[0] = slots[6];
  emptySlots[1] = slots[7];

  moving = false;
  movingCube = NO_CUBE;
  moveFrame = 0;
  holdTick = 0;
}

void cubePuzzleRenderFrame() {
  clearBack();

  if (!moving) {
    if (holdTick < HOLD_FRAMES) {
      holdTick++;
    } else {
      startRandomMove();
    }
  }

  for (uint8_t c = 0; c < CUBE_COUNT; c++) {
    if (moving && c == movingCube) {
      continue;
    }
    renderCubeInSlot(c, cubes[c].slot);
  }

  if (moving) {
    renderMovingCube();
    moveFrame++;
    if (moveFrame > MOVE_FRAMES) {
      finalizeMove();
    }
  }
}

void cubePuzzleToggleSwapRG() { swapRG = !swapRG; }

bool cubePuzzleGetSwapRG() { return swapRG; }
