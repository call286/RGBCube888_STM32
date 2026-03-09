#include "ir_receiver.h"

#include "cube_config.h"

namespace {

static const uint8_t IR_FRAME_QUEUE_LEN = 8;
static const uint8_t IR_EDGE_QUEUE_LEN = 96;

// NEC timing windows based on falling-edge to falling-edge intervals.
static const uint16_t IR_FALL_FULL_MIN_US = 12000;  // ~13.5ms
static const uint16_t IR_FALL_FULL_MAX_US = 15000;
static const uint16_t IR_FALL_REPEAT_MIN_US = 10000; // ~11.25ms
static const uint16_t IR_FALL_REPEAT_MAX_US = 12300;
static const uint16_t IR_FALL_BIT0_MIN_US = 900;    // ~1.12ms
static const uint16_t IR_FALL_BIT0_MAX_US = 1500;
static const uint16_t IR_FALL_BIT1_MIN_US = 1800;   // ~2.25ms
static const uint16_t IR_FALL_BIT1_MAX_US = 2900;

static IrFrame frameQueue[IR_FRAME_QUEUE_LEN];
static uint8_t frameHead = 0;
static uint8_t frameTail = 0;
static uint8_t frameCount = 0;

static volatile uint32_t edgeTimesUs[IR_EDGE_QUEUE_LEN];
static volatile uint8_t edgeHead = 0;
static volatile uint8_t edgeTail = 0;
static volatile bool edgeOverflow = false;

static uint32_t lastFallingUs = 0;
static bool haveLastFalling = false;

static bool inFrame = false;
static uint32_t rawAccum = 0;
static uint8_t bitIndex = 0;

static bool matchRange(uint32_t value, uint16_t lo, uint16_t hi) {
  return value >= lo && value <= hi;
}

static void resetDecoder() {
  inFrame = false;
  rawAccum = 0;
  bitIndex = 0;
}

static void enqueueFrame(uint32_t raw, bool isRepeat) {
  IrFrame f = {raw, isRepeat};
  if (frameCount < IR_FRAME_QUEUE_LEN) {
    frameQueue[frameHead] = f;
    frameHead = (uint8_t)((frameHead + 1) % IR_FRAME_QUEUE_LEN);
    frameCount++;
    return;
  }

  // Queue full: drop oldest and keep newest.
  frameQueue[frameHead] = f;
  frameHead = (uint8_t)((frameHead + 1) % IR_FRAME_QUEUE_LEN);
  frameTail = (uint8_t)((frameTail + 1) % IR_FRAME_QUEUE_LEN);
}

static void printDecoded(uint32_t raw, bool isRepeat, bool printFrames) {
  if (!printFrames) {
    return;
  }

  if (isRepeat) {
    Serial.println(F("[IR] NEC repeat"));
    return;
  }

  uint8_t a = (uint8_t)(raw & 0xFF);
  uint8_t aInv = (uint8_t)((raw >> 8) & 0xFF);
  uint8_t c = (uint8_t)((raw >> 16) & 0xFF);
  uint8_t cInv = (uint8_t)((raw >> 24) & 0xFF);
  bool standard = ((uint8_t)(a ^ aInv) == 0xFF) && ((uint8_t)(c ^ cInv) == 0xFF);

  char hexRaw[11];
  snprintf(hexRaw, sizeof(hexRaw), "0x%08lX", (unsigned long)raw);

  Serial.print(F("[IR] raw="));
  Serial.print(hexRaw);

  if (standard) {
    Serial.print(F(" addr=0x"));
    if (a < 16) {
      Serial.print('0');
    }
    Serial.print(a, HEX);
    Serial.print(F(" cmd=0x"));
    if (c < 16) {
      Serial.print('0');
    }
    Serial.print(c, HEX);
    Serial.println(F(" (NEC)"));
  } else {
    uint16_t extAddr = (uint16_t)(raw & 0xFFFF);
    Serial.print(F(" addr16=0x"));
    if (extAddr < 0x1000)
      Serial.print('0');
    if (extAddr < 0x100)
      Serial.print('0');
    if (extAddr < 0x10)
      Serial.print('0');
    Serial.print(extAddr, HEX);
    Serial.print(F(" cmd=0x"));
    if (c < 16) {
      Serial.print('0');
    }
    Serial.print(c, HEX);
    Serial.println(F(" (non-standard NEC/check failed)"));
  }
}

static void onIrFallingEdge() {
  uint32_t nowUs = micros();
  uint8_t nextHead = (uint8_t)((edgeHead + 1) % IR_EDGE_QUEUE_LEN);
  if (nextHead == edgeTail) {
    edgeOverflow = true;
    return;
  }

  edgeTimesUs[edgeHead] = nowUs;
  edgeHead = nextHead;
}

static void consumeEdge(uint32_t tUs, bool printFrames) {
  if (!haveLastFalling) {
    lastFallingUs = tUs;
    haveLastFalling = true;
    return;
  }

  uint32_t dt = tUs - lastFallingUs;
  lastFallingUs = tUs;

  // Long break resets any partial decode.
  if (dt > 20000UL) {
    resetDecoder();
    return;
  }

  if (matchRange(dt, IR_FALL_REPEAT_MIN_US, IR_FALL_REPEAT_MAX_US)) {
    resetDecoder();
    enqueueFrame(0, true);
    printDecoded(0, true, printFrames);
    return;
  }

  if (matchRange(dt, IR_FALL_FULL_MIN_US, IR_FALL_FULL_MAX_US)) {
    inFrame = true;
    rawAccum = 0;
    bitIndex = 0;
    return;
  }

  if (!inFrame) {
    return;
  }

  if (matchRange(dt, IR_FALL_BIT0_MIN_US, IR_FALL_BIT0_MAX_US)) {
    // LSB 0 -> no bit set
  } else if (matchRange(dt, IR_FALL_BIT1_MIN_US, IR_FALL_BIT1_MAX_US)) {
    rawAccum |= (1UL << bitIndex);
  } else {
    resetDecoder();
    return;
  }

  bitIndex++;
  if (bitIndex >= 32) {
    uint32_t raw = rawAccum;
    resetDecoder();
    enqueueFrame(raw, false);
    printDecoded(raw, false, printFrames);
  }
}

} // namespace

void irReceiverInit() {
  detachInterrupt(digitalPinToInterrupt(CubeConfig::IR_RX));
  pinMode(CubeConfig::IR_RX, INPUT_PULLUP);

  frameHead = 0;
  frameTail = 0;
  frameCount = 0;

  edgeHead = 0;
  edgeTail = 0;
  edgeOverflow = false;

  haveLastFalling = false;
  lastFallingUs = 0;
  resetDecoder();

  attachInterrupt(digitalPinToInterrupt(CubeConfig::IR_RX), onIrFallingEdge, FALLING);
}

void irReceiverHandle(bool printFrames) {
  while (true) {
    uint32_t tUs = 0;
    bool haveEdge = false;

    noInterrupts();
    if (edgeTail != edgeHead) {
      tUs = edgeTimesUs[edgeTail];
      edgeTail = (uint8_t)((edgeTail + 1) % IR_EDGE_QUEUE_LEN);
      haveEdge = true;
    }
    bool overflowed = edgeOverflow;
    edgeOverflow = false;
    interrupts();

    if (overflowed) {
      // Drop partial decode if we lost edge history.
      resetDecoder();
      haveLastFalling = false;
    }

    if (!haveEdge) {
      break;
    }

    consumeEdge(tUs, printFrames);
  }
}

bool irReceiverPopFrame(IrFrame &out) {
  if (frameCount == 0) {
    return false;
  }

  out = frameQueue[frameTail];
  frameTail = (uint8_t)((frameTail + 1) % IR_FRAME_QUEUE_LEN);
  frameCount--;
  return true;
}

void irReceiverFlushFrames() {
  frameHead = 0;
  frameTail = 0;
  frameCount = 0;
}
