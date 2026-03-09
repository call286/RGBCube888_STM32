#include "refresh_engine.h"

#include <HardwareTimer.h>
#include <string.h>

static FrameBuffer fb0;
static FrameBuffer fb1;
static volatile FrameBuffer *g_front = &fb0;
static FrameBuffer *g_back = &fb1;
static volatile uint8_t g_layer = 0;
static volatile uint8_t g_pwmPhase = 0;
static const uint8_t PWM_STEPS = REFRESH_PWM_STEPS;
static const uint8_t PWM_ISR_MULTIPLIER = 4u;

static HardwareTimer *g_refreshTimer = nullptr;

static inline uint8_t level8ToPwm(uint8_t level) {
  if (level == 0u) {
    return 0u;
  }
  // Perceptual remap for 8-step PWM to increase separability of common
  // low/mid .3D8 levels (e.g. 16/48/112/240).
  if (PWM_STEPS == 8u) {
    if (level <= 23u) {
      return 1u;
    }
    if (level <= 63u) {
      return 3u;
    }
    if (level <= 143u) {
      return 5u;
    }
    if (level <= 223u) {
      return 7u;
    }
    return 8u;
  }

  // Perceptual remap for 4-step PWM:
  if (PWM_STEPS == 4u) {
    if (level <= 31u) {
      return 1u;
    }
    if (level <= 95u) {
      return 2u;
    }
    if (level <= 191u) {
      return 3u;
    }
    return 4u;
  }

  uint16_t scaled = (uint16_t)(((uint16_t)level * PWM_STEPS + 127u) / 255u);
  if (scaled == 0u) {
    scaled = 1u;
  } else if (scaled > PWM_STEPS) {
    scaled = PWM_STEPS;
  }
  return (uint8_t)scaled;
}

static inline void channelToPacked(uint16_t ch, bool &toChainA, uint8_t &byteIndex, uint8_t &mask) {
  toChainA = false;
  byteIndex = 0;
  mask = 0;
  if (ch < 1u || ch > 192u) {
    return;
  }

  uint16_t idx = 0;
  if (ch <= 96u) {
    idx = (uint16_t)(ch - 1u);
    toChainA = false;
  } else {
    idx = (uint16_t)(ch - 97u);
    toChainA = true;
  }

  uint8_t group = (uint8_t)(idx >> 4);
  uint8_t chip = (uint8_t)(5u - group);
  uint8_t out = (uint8_t)(idx & 0x0Fu);

  uint8_t streamChipPos = (uint8_t)(5u - chip);
  uint8_t byteBase = (uint8_t)(streamChipPos * 2u);

  byteIndex = (out >= 8u) ? byteBase : (uint8_t)(byteBase + 1u);
  uint8_t bitInByte = (out >= 8u) ? (uint8_t)(out - 8u) : out;
  mask = (uint8_t)(1u << bitInByte);
}

static inline void clkLow() { digitalWrite(CubeConfig::RGB_CLK, LOW); }
static inline void clkHigh() { digitalWrite(CubeConfig::RGB_CLK, HIGH); }
static inline void leLow() { digitalWrite(CubeConfig::RGB_ST, LOW); }
static inline void leHigh() { digitalWrite(CubeConfig::RGB_ST, HIGH); }

// OE active LOW: LOW=ON, HIGH=OFF
static inline void oeOff() { digitalWrite(CubeConfig::RGB_OE, HIGH); }
static inline void oeOn() { digitalWrite(CubeConfig::RGB_OE, LOW); }

static inline void setLayerAddr(uint8_t z) {
  uint8_t a = (z & 7);
  digitalWrite(CubeConfig::RGB_A0, (a & 0x01) ? HIGH : LOW);
  digitalWrite(CubeConfig::RGB_A1, (a & 0x02) ? HIGH : LOW);
  digitalWrite(CubeConfig::RGB_A2, (a & 0x04) ? HIGH : LOW);
}

static inline void shiftBit(bool a, bool b) {
  digitalWrite(CubeConfig::RGB_DAT_A, a ? HIGH : LOW);
  digitalWrite(CubeConfig::RGB_DAT_B, b ? HIGH : LOW);
  clkHigh();
  clkLow();
}

static inline void shiftBytePair(uint8_t aByte, uint8_t bByte) {
  for (uint8_t m = 0x80; m; m >>= 1) {
    shiftBit((aByte & m) != 0, (bByte & m) != 0);
  }
}

static inline void latchEdges(uint8_t edges) {
  clkLow();
  leHigh();
  for (uint8_t i = 0; i < edges; i++) {
    clkHigh();
    clkLow();
  }
  leLow();
}

static void refreshISR() {
  const volatile FrameBuffer *fb = g_front;

  oeOff();
  setLayerAddr(g_layer);

  for (uint8_t i = 0; i < CubeConfig::BYTES_PER_CHAIN; i++) {
    shiftBytePair(fb->chainA[g_pwmPhase][g_layer][i], fb->chainB[g_pwmPhase][g_layer][i]);
  }

  latchEdges(CubeConfig::LATCH_EDGES);
  oeOn();

  g_layer = (g_layer + 1) & 7;
  if (g_layer == 0u) {
    g_pwmPhase = (uint8_t)((g_pwmPhase + 1u) % PWM_STEPS);
  }
}

void refreshInitPins() {
  pinMode(CubeConfig::RGB_A0, OUTPUT);
  pinMode(CubeConfig::RGB_A1, OUTPUT);
  pinMode(CubeConfig::RGB_A2, OUTPUT);
  pinMode(CubeConfig::RGB_DAT_A, OUTPUT);
  pinMode(CubeConfig::RGB_DAT_B, OUTPUT);
  pinMode(CubeConfig::RGB_OE, OUTPUT);
  pinMode(CubeConfig::RGB_CLK, OUTPUT);
  pinMode(CubeConfig::RGB_ST, OUTPUT);

  clkLow();
  leLow();
  oeOff();
  digitalWrite(CubeConfig::RGB_DAT_A, LOW);
  digitalWrite(CubeConfig::RGB_DAT_B, LOW);
}

void initFrameBuffers() {
  noInterrupts();
  memset((void *)&fb0, 0x00, sizeof(fb0));
  memset((void *)&fb1, 0x00, sizeof(fb1));
  g_front = &fb0;
  g_back = &fb1;
  g_layer = 0;
  g_pwmPhase = 0;
  interrupts();
}

void refreshStart(uint16_t frameHz) {
  const uint32_t layerHz = (uint32_t)frameHz * CubeConfig::LAYERS * PWM_ISR_MULTIPLIER;

  if (g_refreshTimer == nullptr) {
    g_refreshTimer = new HardwareTimer(TIM2);
  }

  g_refreshTimer->pause();
  g_refreshTimer->setOverflow(layerHz, HERTZ_FORMAT);
  g_refreshTimer->attachInterrupt(refreshISR);
  g_refreshTimer->resume();
}

void clearBack() {
  memset(g_back, 0x00, sizeof(*g_back));
}

void fillBackAll(bool on) {
  uint8_t v = on ? PWM_STEPS : 0u;
  memset(g_back->channels, v, sizeof(g_back->channels));
  uint8_t p = on ? 0xFFu : 0x00u;
  memset(g_back->chainA, p, sizeof(g_back->chainA));
  memset(g_back->chainB, p, sizeof(g_back->chainB));
}

void setChanLevelInBack(uint8_t z, uint16_t ch, uint8_t level) {
  if (z > 7 || ch < 1 || ch > 192) {
    return;
  }
  uint8_t pwm = level8ToPwm(level);
  g_back->channels[z][ch - 1u] = pwm;

  bool toChainA = false;
  uint8_t byteIndex = 0;
  uint8_t mask = 0;
  channelToPacked(ch, toChainA, byteIndex, mask);
  if (mask == 0u) {
    return;
  }

  for (uint8_t phase = 0; phase < PWM_STEPS; phase++) {
    bool on = pwm > phase;
    if (toChainA) {
      if (on) {
        g_back->chainA[phase][z][byteIndex] |= mask;
      } else {
        g_back->chainA[phase][z][byteIndex] &= (uint8_t)~mask;
      }
    } else {
      if (on) {
        g_back->chainB[phase][z][byteIndex] |= mask;
      } else {
        g_back->chainB[phase][z][byteIndex] &= (uint8_t)~mask;
      }
    }
  }
}

void setChanInBack(uint8_t z, uint16_t ch, bool on) { setChanLevelInBack(z, ch, on ? 255u : 0u); }

void commitBackToFront() {
  noInterrupts();
  volatile FrameBuffer *oldFront = g_front;
  g_front = g_back;
  interrupts();
  g_back = (FrameBuffer *)oldFront;
}
