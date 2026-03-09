#include "stream_3d8.h"

#include <ctype.h>
#include <string.h>

#include "channel_mapper.h"
#include "cube_config.h"
#include "refresh_engine.h"

namespace {

static const uint16_t BYTES_PER_VOXEL_3D8 = 6; // ASCII hex: RRGGBB
static const uint16_t FRAME_CHARS_3D8 = 8u * 8u * 8u * BYTES_PER_VOXEL_3D8;
static const uint16_t FRAME_TIMED_CHARS_3D8 = (uint16_t)(FRAME_CHARS_3D8 + 4u); // +TTTT
static const uint16_t CHANNELS_PER_LAYER = CubeConfig::BYTES_PER_CHAIN * 16u;
static const uint16_t STREAM_RENDER_TICK_MS = 8;
static const uint16_t STREAM_DEFAULT_DURATION_MS = 20;
static const uint16_t STREAM_TRANSITION_MS = 80;

struct StreamFrameState {
  uint8_t channels[CubeConfig::LAYERS][CHANNELS_PER_LAYER];
  uint16_t durationMs;
  bool valid;
};

static bool streamEnabled = false;
static bool loggingEnabled = false;
static bool frameReady = false;
static uint32_t frameCount = 0;
static uint16_t ingestLen = 0;
static bool ingestFrameActive = false;
static uint16_t ingestInvalidChars = 0;
static bool ingestOverflow = false;
static char ingestBuf[FRAME_TIMED_CHARS_3D8];
static StreamFrameState renderFrom;
static StreamFrameState renderTo;
static uint32_t phaseStartMs = 0;

static int8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return (int8_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int8_t)(10 + (c - 'a'));
  }
  if (c >= 'A' && c <= 'F') {
    return (int8_t)(10 + (c - 'A'));
  }
  return -1;
}

static bool parseHexByte(char hi, char lo, uint8_t &out) {
  int8_t h = hexNibble(hi);
  int8_t l = hexNibble(lo);
  if (h < 0 || l < 0) {
    return false;
  }
  out = (uint8_t)((h << 4) | l);
  return true;
}

static uint8_t map3d8ColorToHwColor(uint8_t logicalColor) {
  if (logicalColor == 0u) {
    return CubeConfig::COLOR_SWIZZLE_B; // .3D8 red -> hardware blue
  }
  if (logicalColor == 1u) {
    return CubeConfig::COLOR_SWIZZLE_G;
  }
  return CubeConfig::COLOR_SWIZZLE_R; // .3D8 blue -> hardware red
}

static uint8_t blendBrightnessLevel(uint8_t a, uint8_t b, uint16_t progress256) {
  if (progress256 >= 256u) {
    return b;
  }
  uint32_t inv = (uint32_t)(256u - progress256);
  uint32_t mixed = (uint32_t)a * inv + (uint32_t)b * (uint32_t)progress256;
  return (uint8_t)((mixed + 128u) >> 8);
}

static bool decodeHexFrameToState(const char *buf, uint16_t len, StreamFrameState &out) {
  if (buf == nullptr || len < FRAME_CHARS_3D8) {
    return false;
  }

  memset(out.channels, 0, sizeof(out.channels));

  // Legacy AuraCube .3D8 frame layout:
  // For each z-plane: 64 voxels, each voxel as 6 ASCII hex chars (RRGGBB),
  // with X mirrored inside each row.
  for (uint8_t z = 0; z < 8; z++) {
    uint16_t zBase = (uint16_t)z * 64u * BYTES_PER_VOXEL_3D8;
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        uint16_t voxelInPlane = (uint16_t)(7u - x + (y << 3));
        uint16_t base = (uint16_t)(zBase + voxelInPlane * BYTES_PER_VOXEL_3D8);

        uint8_t r = 0, g = 0, b = 0;
        if (!parseHexByte(buf[base + 0], buf[base + 1], r) ||
            !parseHexByte(buf[base + 2], buf[base + 3], g) ||
            !parseHexByte(buf[base + 4], buf[base + 5], b)) {
          // Match SD behavior: malformed voxel payload drops only this voxel.
          continue;
        }

        // Rotate imported .3D8 content by -90 deg around X:
        // X stays, Y <- Z, Z <- (7 - Y). This makes the former front plane become top.
        uint8_t rx = x;
        uint8_t ry = z;
        uint8_t rz = (uint8_t)(7u - y);

        if (r > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(0));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < r) {
              out.channels[rz][ch - 1] = r;
            }
          }
        }
        if (g > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(1));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < g) {
              out.channels[rz][ch - 1] = g;
            }
          }
        }
        if (b > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(2));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < b) {
              out.channels[rz][ch - 1] = b;
            }
          }
        }
      }
    }
  }

  out.durationMs = STREAM_DEFAULT_DURATION_MS;
  if (len >= FRAME_TIMED_CHARS_3D8) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (parseHexByte(buf[FRAME_CHARS_3D8 + 0], buf[FRAME_CHARS_3D8 + 1], hi) &&
        parseHexByte(buf[FRAME_CHARS_3D8 + 2], buf[FRAME_CHARS_3D8 + 3], lo)) {
      uint16_t ms = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
      out.durationMs = (ms == 0u) ? 1u : ms;
    }
  }
  out.valid = true;
  return true;
}

static bool decodeRgbFrameToState(const uint8_t *rgb, uint16_t rgbLen, uint16_t durationMs,
                                  StreamFrameState &out) {
  if (rgb == nullptr || rgbLen < (8u * 8u * 8u * 3u)) {
    return false;
  }

  memset(out.channels, 0, sizeof(out.channels));

  // Same voxel ordering as .3D8 hex content, but packed as RGB bytes.
  for (uint8_t z = 0; z < 8; z++) {
    uint16_t zBase = (uint16_t)z * 64u * 3u;
    for (uint8_t y = 0; y < 8; y++) {
      for (uint8_t x = 0; x < 8; x++) {
        uint16_t voxelInPlane = (uint16_t)(7u - x + (y << 3));
        uint16_t base = (uint16_t)(zBase + voxelInPlane * 3u);

        uint8_t r = rgb[base + 0];
        uint8_t g = rgb[base + 1];
        uint8_t b = rgb[base + 2];

        uint8_t rx = x;
        uint8_t ry = z;
        uint8_t rz = (uint8_t)(7u - y);

        if (r > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(0));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < r) {
              out.channels[rz][ch - 1] = r;
            }
          }
        }
        if (g > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(1));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < g) {
              out.channels[rz][ch - 1] = g;
            }
          }
        }
        if (b > 0u) {
          uint16_t ch = channelForRaw(rx, ry, map3d8ColorToHwColor(2));
          if (ch > 0 && ch <= CHANNELS_PER_LAYER) {
            if (out.channels[rz][ch - 1] < b) {
              out.channels[rz][ch - 1] = b;
            }
          }
        }
      }
    }
  }

  out.durationMs = (durationMs == 0u) ? 1u : durationMs;
  out.valid = true;
  return true;
}

static void renderState(const StreamFrameState &state) {
  clearBack();
  for (uint8_t z = 0; z < CubeConfig::LAYERS; z++) {
    for (uint16_t ch = 0; ch < CHANNELS_PER_LAYER; ch++) {
      uint8_t level = state.channels[z][ch];
      if (level > 0u) {
        setChanLevelInBack(z, (uint16_t)(ch + 1u), level);
      }
    }
  }
}

static void renderTransition(const StreamFrameState &from, const StreamFrameState &to, uint16_t progress256) {
  clearBack();
  for (uint8_t z = 0; z < CubeConfig::LAYERS; z++) {
    for (uint16_t ch = 0; ch < CHANNELS_PER_LAYER; ch++) {
      uint8_t a = from.channels[z][ch];
      uint8_t b = to.channels[z][ch];
      uint8_t mixed = blendBrightnessLevel(a, b, progress256);
      if (mixed > 0u) {
        setChanLevelInBack(z, (uint16_t)(ch + 1u), mixed);
      }
    }
  }
}

} // namespace

void stream3d8Init() {
  streamEnabled = false;
  loggingEnabled = false;
  stream3d8Reset();
}

void stream3d8Reset() {
  frameReady = false;
  frameCount = 0;
  ingestLen = 0;
  ingestFrameActive = false;
  ingestInvalidChars = 0;
  ingestOverflow = false;
  memset(ingestBuf, 0, sizeof(ingestBuf));
  memset(&renderFrom, 0, sizeof(renderFrom));
  memset(&renderTo, 0, sizeof(renderTo));
  phaseStartMs = 0;
}

void stream3d8SetEnabled(bool enabled) { streamEnabled = enabled; }

bool stream3d8IsEnabled() { return streamEnabled; }

void stream3d8SetLogging(bool enabled) { loggingEnabled = enabled; }

bool stream3d8GetLogging() { return loggingEnabled; }

void stream3d8BeginFrame() {
  ingestFrameActive = true;
  ingestLen = 0;
  ingestInvalidChars = 0;
  ingestOverflow = false;
}

void stream3d8ClearPartial() {
  ingestLen = 0;
  ingestInvalidChars = 0;
  ingestOverflow = false;
}

bool stream3d8FeedChunk(const char *chunk, Stream3d8FeedResult &out) {
  out.acceptedChars = 0;
  out.ignoredChars = 0;
  out.invalidChars = 0;
  out.completedFrames = 0;
  out.partialChars = ingestLen;

  if (chunk == nullptr) {
    return false;
  }
  if (!ingestFrameActive) {
    // Backward-compatible implicit frame start.
    stream3d8BeginFrame();
  }

  bool sawAny = false;
  while (*chunk != '\0') {
    char c = *chunk++;
    if (hexNibble(c) >= 0) {
      if (ingestLen < FRAME_TIMED_CHARS_3D8) {
        ingestBuf[ingestLen++] = c;
        out.acceptedChars++;
      } else {
        ingestOverflow = true;
      }
      sawAny = true;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' || c == '|') {
      out.ignoredChars++;
      continue;
    }

    out.invalidChars++;
    ingestInvalidChars++;
  }

  out.partialChars = ingestLen;
  return sawAny;
}

bool stream3d8FinalizeFrame(Stream3d8FinalizeResult &out) {
  out.receivedChars = ingestLen;
  out.invalidChars = ingestInvalidChars;
  out.overflowed = ingestOverflow;
  out.committed = false;

  bool hasValidLength = (ingestLen == FRAME_CHARS_3D8 || ingestLen == FRAME_TIMED_CHARS_3D8);
  StreamFrameState decoded = {};
  bool ok = ingestFrameActive && !ingestOverflow && hasValidLength &&
            decodeHexFrameToState(ingestBuf, ingestLen, decoded);

  if (ok) {
    if (!renderFrom.valid) {
      renderFrom = decoded;
      renderTo.valid = false;
      phaseStartMs = millis();
    } else {
      renderTo = decoded;
    }
    frameReady = true;
    frameCount++;
  }

  ingestFrameActive = false;
  ingestLen = 0;
  ingestInvalidChars = 0;
  ingestOverflow = false;

  out.committed = ok;
  return ok;
}

bool stream3d8CommitRgbFrame(const uint8_t *rgb, uint16_t rgbLen, uint16_t durationMs) {
  StreamFrameState decoded = {};
  if (!decodeRgbFrameToState(rgb, rgbLen, durationMs, decoded)) {
    return false;
  }

  if (!renderFrom.valid) {
    renderFrom = decoded;
    renderTo.valid = false;
    phaseStartMs = millis();
  } else {
    renderTo = decoded;
  }
  frameReady = true;
  frameCount++;
  return true;
}

bool stream3d8RenderLatestFrame() {
  if (!streamEnabled || !frameReady || !renderFrom.valid) {
    return false;
  }

  uint32_t now = millis();
  uint16_t phaseStepMs = (renderFrom.durationMs == 0u) ? 1u : renderFrom.durationMs;
  uint32_t elapsed = (uint32_t)(now - phaseStartMs);

  while (renderTo.valid && elapsed >= phaseStepMs) {
    renderFrom = renderTo;
    renderTo.valid = false;
    phaseStartMs += phaseStepMs;
    phaseStepMs = (renderFrom.durationMs == 0u) ? 1u : renderFrom.durationMs;
    elapsed = (uint32_t)(now - phaseStartMs);
  }

  if (!renderTo.valid) {
    renderState(renderFrom);
    return true;
  }

  uint16_t transitionMs = (STREAM_TRANSITION_MS > phaseStepMs) ? phaseStepMs : STREAM_TRANSITION_MS;
  if (transitionMs == 0u) {
    renderState(renderFrom);
    return true;
  }
  if (elapsed >= transitionMs) {
    renderState(renderTo);
    return true;
  }

  uint16_t progress256 = (uint16_t)(((uint32_t)elapsed * 256u) / transitionMs);
  if (progress256 > 256u) {
    progress256 = 256u;
  }
  renderTransition(renderFrom, renderTo, progress256);
  return true;
}

bool stream3d8HasFrame() { return frameReady && renderFrom.valid; }

uint32_t stream3d8GetFrameCount() { return frameCount; }

uint16_t stream3d8GetPartialChars() { return ingestLen; }

uint16_t stream3d8GetRenderTickMs() { return STREAM_RENDER_TICK_MS; }
