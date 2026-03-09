#include "serial_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animations/cube_puzzle_anim.h"
#include "animations/hello_anim.h"
#include "animations/pixel_set_anim.h"
#include "animations/raw_test_anim.h"
#include "animations/rain_anim.h"
#include "animations/wave_anim.h"
#include "channel_mapper.h"
#include "esp_at_bridge.h"
#include "sd_storage.h"
#include "stream_3d8.h"

static RenderMode renderMode = MODE_WAVE;
static uint16_t animStepMs = 33;
static uint16_t sdAnimFrameMs = 120;
static const uint16_t SD_ANIM_FRAME_MS_MIN = 20;
static const uint16_t SD_ANIM_FRAME_MS_MAX = 1000;
static uint16_t sdAnimTimedSpeedPct = 100;
static const uint16_t SD_ANIM_TIMED_SPEED_PCT_MIN = 25;
static const uint16_t SD_ANIM_TIMED_SPEED_PCT_MAX = 400;
static bool sdAnimTransitionEnabled = true;
static uint16_t sdAnimTransitionMs = 80;
static const uint16_t SD_ANIM_TRANSITION_MS_MIN = 0;
static const uint16_t SD_ANIM_TRANSITION_MS_MAX = 1000;
static const uint16_t SD_ANIM_RENDER_TICK_MS = 8;
static bool displayEnabled = true;

static char lineBuf[256];
static uint16_t lineLen = 0;
static Print *cliOut = &Serial;
static bool suppressCommandEcho = false;
static const uint16_t RX3D8_FRAME_HEX = 8u * 8u * 8u * 6u;
static const uint16_t RX3D8_TIMED_SUFFIX_HEX = 4u;
static const uint16_t RX3D8_TIMED_FRAME_HEX = (uint16_t)(RX3D8_FRAME_HEX + RX3D8_TIMED_SUFFIX_HEX);
static const uint8_t RX3D8_ROBUST_CHUNK_HEX = 64u;
static const uint8_t RX3D8_ROBUST_BASE_CHUNK_COUNT = (uint8_t)(RX3D8_FRAME_HEX / RX3D8_ROBUST_CHUNK_HEX);
static const uint8_t RX3D8_ROBUST_TIMED_SUFFIX_IDX = RX3D8_ROBUST_BASE_CHUNK_COUNT; // 0x30
static const uint8_t RX3D8_ROBUST_CHUNK_COUNT = (uint8_t)(RX3D8_ROBUST_BASE_CHUNK_COUNT + 1u);
static char robustFrameHex[RX3D8_TIMED_FRAME_HEX + 1];
static uint8_t robustChunkSeen[RX3D8_ROBUST_CHUNK_COUNT];
static uint8_t robustChunkCount = 0;

static bool equalsIgnoreCase(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static void printCliAndSerialLine(const char *line, bool allowWhenSuppressed = false) {
  if (line == nullptr) {
    return;
  }
  if (!suppressCommandEcho || allowWhenSuppressed) {
    cliOut->println(line);
  }
  if (cliOut != &Serial) {
    Serial.println(line);
  }
}

static void printCliAndSerialRxFinalize(const Stream3d8FinalizeResult &fin, bool ok,
                                        uint32_t frameCount) {
  char msg[96];
  if (ok) {
    (void)snprintf(msg, sizeof(msg), "[RX-3D8] frame ok #%lu", (unsigned long)frameCount);
  } else {
    (void)snprintf(msg, sizeof(msg),
                   "[RX-3D8] frame drop recv=%lu invalid=%lu overflow=%u",
                   (unsigned long)fin.receivedChars, (unsigned long)fin.invalidChars,
                   fin.overflowed ? 1U : 0U);
  }
  printCliAndSerialLine(msg);
}

static void printCliAndSerialRxChunkStats(const Stream3d8FeedResult &r) {
  char msg[72];
  (void)snprintf(msg, sizeof(msg), "[RX-3D8] +hex=%lu partial=%lu",
                 (unsigned long)r.acceptedChars, (unsigned long)r.partialChars);
  printCliAndSerialLine(msg);
}

static void printCliAndSerialRxInvalidChars(uint16_t invalidChars) {
  char msg[64];
  (void)snprintf(msg, sizeof(msg), "[RX-3D8] invalid chars ignored: %u", invalidChars);
  printCliAndSerialLine(msg);
}

static void serialResetLine() {
  lineLen = 0;
  lineBuf[0] = '\0';
}

static void printHelp() {
  cliOut->println(F("\nCommands (end each command with ENTER):"));
  cliOut->println(F("  h               help"));
  cliOut->println(F("  p               print current settings"));
  cliOut->println(F("  m <0..10>       mode (0=wave, 1=all on, 2=all off, 3=rain, 4=hello, 5=IR debug, 6=puzzle, 7=pixel-set, 8=raw-test, 9=sd-anim, 10=rx-3d8)"));
  cliOut->println(F("  f <10..80>      anim step ms (lower=faster)"));
  cliOut->println(F("  sf <20..1000>   sd-anim frame ms (lower=faster animation)"));
  cliOut->println(F("  ssp <25..400>   sd-anim timed speed % (100=from file, lower=faster)"));
  cliOut->println(F("  st <0|1>        sd-anim transition OFF/ON"));
  cliOut->println(F("  stt <0..1000>   sd-anim transition ms (0=hard cut)"));
  cliOut->println(F("  dp [0|1|t]      display OFF/ON/toggle (mode keeps running)"));
  cliOut->println(F("  r               toggle global R/G swap"));
  cliOut->println(F("  s <1..20>       wave speed"));
  cliOut->println(F("  k <1..60>       wave scale (wavelength)"));
  cliOut->println(F("  t <0..255>      wave threshold"));
  cliOut->println(F("  rn <1..64>      rain drop count"));
  cliOut->println(F("  rv <1..12>      rain fall-step frames (1=fast)"));
  cliOut->println(F("  rl <0..7>       rain tail length"));
  cliOut->println(F("  rr <1..6>       rain rainbow step per fall"));
  cliOut->println(F("  rd <0..20>      rain respawn delay"));
  cliOut->println(F("  hs <1..24>      hello spin step"));
  cliOut->println(F("  hf <4..40>      hello fly-in/out frames"));
  cliOut->println(F("  hh <0..40>      hello hold frames"));
  cliOut->println(F("  hb <0..30>      hello gap frames"));
  cliOut->println(F("  va x y z [r g b] pixel-set: add voxel + optional RGB (0|1)"));
  cliOut->println(F("  vr x y z        pixel-set: remove voxel"));
  cliOut->println(F("  vp T [r g b]    pixel-set: paint plane, T=xN|yN|zN|xyN|xzN|yzN"));
  cliOut->println(F("  vn A a b [r g b] pixel-set: paint line, A=x|y|z"));
  cliOut->println(F("  vc              pixel-set: clear all"));
  cliOut->println(F("  vl              pixel-set: print voxel count"));
  cliOut->println(F("  x+N/x-N         pixel-set: move set in X by N"));
  cliOut->println(F("  y+N/y-N         pixel-set: move set in Y by N"));
  cliOut->println(F("  z+N/z-N         pixel-set: move set in Z by N"));
  cliOut->println(F("  td z ch         raw-test: set only direct channel (z:0..7, ch:1..192)"));
  cliOut->println(F("  tm x y z c      raw-test: set only mapped channel (c:0=R,1=G,2=B)"));
  cliOut->println(F("  th x y z c      raw-test: set logical x/y using raw hw color index (c:0..2)"));
  cliOut->println(F("  tc              raw-test: clear test output"));
  cliOut->println(F("  mg x y          map: show mapped channels for voxel x/y"));
  cliOut->println(F("  mc x y c ch     map: set mapped channel (c:0=R,1=G,2=B, ch:0..192, 0=off)"));
  cliOut->println(F("  mr              map: reset mapping table to defaults"));
  cliOut->println(F("  eu <0|1>        ESP UART bridge off/on (0=high-Z on PA9/PA10)"));
  cliOut->println(F("  rst             reboot STM32 (full re-init, e.g. after SD reinsert)"));
  cliOut->println(F("  sd ...          SD: ls | anim | wifi | mqtt | bridge | log [0|1] | rw | cat <path> | write <path> <text>"));
  cliOut->println(F("  rx ...          RX stream: on | off | clr | fs | fe | p | log [0|1] | u <3D8-hex-chunk>"));
  cliOut->println(F("  rb/rk/rf        RX robust: rb, rk <idxHex> <hex> <crcHex>, rf (idx 00..2F:64hex, idx 30:4hex TTTT)"));
  cliOut->println(F("  IR +/-          in mode 9, adjust frame ms or timed speed %"));
  cliOut->println(F("\nExamples:  m 4   hs 8   hf 14"));
}

static void printStatus() {
  cliOut->print(F("mode="));
  cliOut->print((uint8_t)renderMode);
  cliOut->print(F(" display="));
  cliOut->print(displayEnabled ? F("ON") : F("OFF"));
  cliOut->print(F(" stepMs="));
  cliOut->print(animStepMs);
  cliOut->print(F(" sdFrameMs="));
  cliOut->print(sdAnimFrameMs);
  cliOut->print(F(" sdTimedSpeed="));
  cliOut->print(sdAnimTimedSpeedPct);
  cliOut->print('%');
  cliOut->print(F(" sdBlend="));
  cliOut->print(sdAnimTransitionEnabled ? F("ON") : F("OFF"));
  cliOut->print('@');
  cliOut->print(sdAnimTransitionMs);
  cliOut->print(F("ms"));

  cliOut->print(F(" | wave: speed="));
  cliOut->print(waveGetSpeed());
  cliOut->print(F(" scale="));
  cliOut->print(waveGetScale());
  cliOut->print(F(" threshold="));
  cliOut->print(waveGetThreshold());

  cliOut->print(F(" | rain: drops="));
  cliOut->print(rainGetDropCount());
  cliOut->print(F(" fallFrames="));
  cliOut->print(rainGetFallStepFrames());
  cliOut->print(F(" tail="));
  cliOut->print(rainGetTailLength());
  cliOut->print(F(" rainbowStep="));
  cliOut->print(rainGetRainbowStep());
  cliOut->print(F(" respawnDelay="));
  cliOut->print(rainGetRespawnDelay());

  cliOut->print(F(" | hello: spin="));
  cliOut->print(helloGetSpinStep());
  cliOut->print(F(" fly="));
  cliOut->print(helloGetFlyFrames());
  cliOut->print(F(" hold="));
  cliOut->print(helloGetHoldFrames());
  cliOut->print(F(" gap="));
  cliOut->print(helloGetGapFrames());

  cliOut->print(F(" | swapRG="));
  cliOut->print(waveGetSwapRG() ? F("ON") : F("OFF"));
  cliOut->print(F(" | espUart="));
  cliOut->print(espAtBridgeIsEnabled() ? F("ON") : F("OFF"));
  cliOut->print(F(" | sd="));
  cliOut->print(sdStorageIsReady() ? F("ON") : F("OFF"));
  cliOut->print(F(" | sdAnim="));
  cliOut->print(sdStorageGetAnimationCount());
  cliOut->print(F(" | sdLog="));
  cliOut->print(sdStorageGetPlaybackLogging() ? F("ON") : F("OFF"));
  cliOut->print(F(" | rx="));
  cliOut->print(stream3d8IsEnabled() ? F("ON") : F("OFF"));
  cliOut->print(F(" | rxFrame="));
  cliOut->print(stream3d8GetFrameCount());
  cliOut->print(F(" | rxPart="));
  cliOut->print(stream3d8GetPartialChars());
  cliOut->print(F(" | rxLog="));
  cliOut->print(stream3d8GetLogging() ? F("ON") : F("OFF"));
  cliOut->print(F(" | pxCount="));
  cliOut->println(pixelSetGetCount());

  if (renderMode == MODE_IR_DEBUG) {
    cliOut->println(F("IR debug active: point remote at PA8 receiver and watch [IR] lines."));
  }
}

static bool parseLongArg(const char *arg, long &out) {
  char *endp = nullptr;
  out = strtol(arg, &endp, 10);
  return endp != arg;
}

static bool parseBoolLike(const char *s, bool &out) {
  if (equalsIgnoreCase(s, "1") || equalsIgnoreCase(s, "true") || equalsIgnoreCase(s, "on") ||
      equalsIgnoreCase(s, "yes") || equalsIgnoreCase(s, "enabled")) {
    out = true;
    return true;
  }
  if (equalsIgnoreCase(s, "0") || equalsIgnoreCase(s, "false") || equalsIgnoreCase(s, "off") ||
      equalsIgnoreCase(s, "no") || equalsIgnoreCase(s, "disabled")) {
    out = false;
    return true;
  }
  return false;
}

static uint16_t clampSdAnimFrameMs(long v) {
  if (v < SD_ANIM_FRAME_MS_MIN) {
    return SD_ANIM_FRAME_MS_MIN;
  }
  if (v > SD_ANIM_FRAME_MS_MAX) {
    return SD_ANIM_FRAME_MS_MAX;
  }
  return (uint16_t)v;
}

static uint16_t clampSdAnimTimedSpeedPct(long v) {
  if (v < SD_ANIM_TIMED_SPEED_PCT_MIN) {
    return SD_ANIM_TIMED_SPEED_PCT_MIN;
  }
  if (v > SD_ANIM_TIMED_SPEED_PCT_MAX) {
    return SD_ANIM_TIMED_SPEED_PCT_MAX;
  }
  return (uint16_t)v;
}

static uint16_t clampSdAnimTransitionMs(long v) {
  if (v < SD_ANIM_TRANSITION_MS_MIN) {
    return SD_ANIM_TRANSITION_MS_MIN;
  }
  if (v > SD_ANIM_TRANSITION_MS_MAX) {
    return SD_ANIM_TRANSITION_MS_MAX;
  }
  return (uint16_t)v;
}

static uint8_t parseArgs(const char *arg, long *vals, uint8_t maxVals) {
  uint8_t n = 0;
  const char *p = arg;
  while (n < maxVals) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    char *endp = nullptr;
    long v = strtol(p, &endp, 10);
    if (endp == p) {
      break;
    }
    vals[n++] = v;
    p = endp;
  }
  return n;
}

static const char *skipWs(const char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  return s;
}

static bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

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

static bool parseHexByte2(char hi, char lo, uint8_t &out) {
  int8_t h = hexNibble(hi);
  int8_t l = hexNibble(lo);
  if (h < 0 || l < 0) {
    return false;
  }
  out = (uint8_t)((h << 4) | l);
  return true;
}

static void rx3d8RobustReset() {
  memset(robustFrameHex, '0', RX3D8_TIMED_FRAME_HEX);
  robustFrameHex[RX3D8_TIMED_FRAME_HEX] = '\0';
  memset(robustChunkSeen, 0, sizeof(robustChunkSeen));
  robustChunkCount = 0;
}

static bool rx3d8StoreRobustChunk(const char *args) {
  const char *p = skipWs(args);
  if (p[0] == '\0' || p[1] == '\0') {
    return false;
  }
  uint8_t idx = 0;
  if (!parseHexByte2(p[0], p[1], idx) || idx >= RX3D8_ROBUST_CHUNK_COUNT) {
    return false;
  }
  p = skipWs(p + 2);

  char payload[RX3D8_ROBUST_CHUNK_HEX + 1];
  uint8_t payloadLen = 0;
  while (isHexChar(p[payloadLen]) && payloadLen < RX3D8_ROBUST_CHUNK_HEX) {
    payload[payloadLen] = p[payloadLen];
    payloadLen++;
  }
  if (payloadLen == 0 || (payloadLen & 1u) != 0u) {
    return false;
  }
  if (isHexChar(p[payloadLen])) {
    // Payload too long for one robust chunk.
    return false;
  }
  payload[payloadLen] = '\0';
  p = skipWs(p + payloadLen);
  if (p[0] == '\0' || p[1] == '\0') {
    return false;
  }
  uint8_t sentCrc = 0;
  if (!parseHexByte2(p[0], p[1], sentCrc)) {
    return false;
  }

  uint8_t calcCrc = 0;
  for (uint8_t i = 0; i < payloadLen; i += 2) {
    uint8_t b = 0;
    if (!parseHexByte2(payload[i], payload[i + 1], b)) {
      return false;
    }
    calcCrc ^= b;
  }
  if (calcCrc != sentCrc) {
    return false;
  }

  uint8_t expectedPayloadLen = 0;
  uint16_t off = 0;
  if (idx < RX3D8_ROBUST_BASE_CHUNK_COUNT) {
    expectedPayloadLen = RX3D8_ROBUST_CHUNK_HEX;
    off = (uint16_t)idx * RX3D8_ROBUST_CHUNK_HEX;
  } else if (idx == RX3D8_ROBUST_TIMED_SUFFIX_IDX) {
    expectedPayloadLen = (uint8_t)RX3D8_TIMED_SUFFIX_HEX;
    off = RX3D8_FRAME_HEX;
  } else {
    return false;
  }
  if (payloadLen != expectedPayloadLen) {
    return false;
  }

  memcpy(&robustFrameHex[off], payload, payloadLen);
  if (robustChunkSeen[idx] == 0) {
    robustChunkSeen[idx] = 1;
    robustChunkCount++;
  }
  return true;
}

static bool looksLikeHexPayloadLine(const char *s) {
  if (s == nullptr) {
    return false;
  }
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  if (*s == '\0' || !isHexChar(*s)) {
    return false;
  }
  uint16_t hexCount = 0;
  uint16_t nonWsCount = 0;
  uint16_t invalidCount = 0;
  while (*s != '\0') {
    char c = *s++;
    if (c == ' ' || c == '\t') {
      continue;
    }
    nonWsCount++;
    if (isHexChar(c)) {
      hexCount++;
    } else {
      invalidCount++;
    }
  }
  if (hexCount < 16 || nonWsCount == 0) {
    return false;
  }
  if (invalidCount > 8) {
    return false;
  }
  return ((uint32_t)hexCount * 100u) >= ((uint32_t)nonWsCount * 75u);
}

static bool feedRxHexPayload(const char *payload) {
  if (payload == nullptr || *payload == '\0') {
    return false;
  }

  stream3d8SetEnabled(true);
  renderMode = MODE_RX_STREAM;

  Stream3d8FeedResult r = {};
  if (!stream3d8FeedChunk(payload, r)) {
    printCliAndSerialLine("[RX-3D8] no valid hex chars in chunk");
    return true;
  }

  if (r.invalidChars > 0) {
    printCliAndSerialRxInvalidChars(r.invalidChars);
  }
  if (stream3d8GetLogging() && (r.completedFrames > 0 || r.invalidChars > 0)) {
    printCliAndSerialRxChunkStats(r);
  }
  return true;
}

static bool rx3d8FinalizeRobustFrame() {
  for (uint8_t i = 0; i < RX3D8_ROBUST_BASE_CHUNK_COUNT; i++) {
    if (robustChunkSeen[i] == 0) {
      return false;
    }
  }

  uint16_t robustLen = RX3D8_FRAME_HEX;
  if (robustChunkSeen[RX3D8_ROBUST_TIMED_SUFFIX_IDX] != 0) {
    robustLen = RX3D8_TIMED_FRAME_HEX;
  }
  robustFrameHex[robustLen] = '\0';

  stream3d8SetEnabled(true);
  renderMode = MODE_RX_STREAM;
  stream3d8BeginFrame();
  Stream3d8FeedResult feed = {};
  (void)stream3d8FeedChunk(robustFrameHex, feed);
  Stream3d8FinalizeResult fin = {};
  bool ok = stream3d8FinalizeFrame(fin);
  if (ok) {
    printCliAndSerialRxFinalize(fin, true, stream3d8GetFrameCount());
  } else {
    printCliAndSerialRxFinalize(fin, false, stream3d8GetFrameCount());
  }
  return ok;
}

static void copyToken(const char *src, char *dst, size_t dstSize) {
  if (dstSize == 0) {
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && src[i] != ' ' && src[i] != '\t' && i < dstSize - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void applySdCommand(const char *args) {
  if (!sdStorageIsReady()) {
    cliOut->println(F("[SD] not mounted"));
    return;
  }

  args = skipWs(args);
  if (*args == '\0' || strcmp(args, "ls") == 0) {
    sdStoragePrintTree(*cliOut);
    return;
  }

  if (strcmp(args, "anim") == 0) {
    sdStorageScanAnimations();
    sdStoragePrintAnimations(*cliOut);
    return;
  }

  if (strcmp(args, "wifi") == 0) {
    if (!sdStorageLoadBridgeSettings()) {
      cliOut->println(F("[SD] no bridge settings found"));
      return;
    }
    bool ok = sdStoragePushBridgeSettingsToEsp();
    cliOut->println(ok ? F("[SD] bridge config sent to ESP")
                       : F("[SD] bridge config load OK, ESP send FAIL"));
    return;
  }

  if (strcmp(args, "mqtt") == 0 || strcmp(args, "bridge") == 0) {
    if (!sdStorageLoadBridgeSettings()) {
      cliOut->println(F("[SD] no bridge settings found"));
      return;
    }
    bool ok = sdStoragePushBridgeSettingsToEsp();
    cliOut->println(ok ? F("[SD] bridge config sent to ESP")
                       : F("[SD] bridge config load OK, ESP send FAIL"));
    return;
  }

  if (strcmp(args, "log") == 0) {
    cliOut->print(F("[SD] playback log "));
    cliOut->println(sdStorageGetPlaybackLogging() ? F("ON") : F("OFF"));
    return;
  }

  if (strncmp(args, "log ", 4) == 0) {
    long v = 0;
    const char *p = skipWs(args + 4);
    if (!parseLongArg(p, v) || (v != 0 && v != 1)) {
      cliOut->println(F("Use sd log [0|1]"));
      return;
    }
    sdStorageSetPlaybackLogging(v != 0);
    cliOut->print(F("[SD] playback log "));
    cliOut->println(sdStorageGetPlaybackLogging() ? F("ON") : F("OFF"));
    return;
  }

  if (strcmp(args, "rw") == 0) {
    (void)sdStorageReadWriteSelfTest(*cliOut);
    return;
  }

  if (strncmp(args, "cat ", 4) == 0) {
    const char *path = skipWs(args + 4);
    if (*path == '\0') {
      cliOut->println(F("Use sd cat <path>"));
      return;
    }

    char content[384];
    size_t bytes = 0;
    if (!sdStorageReadTextFile(path, content, sizeof(content), &bytes)) {
      cliOut->println(F("[SD] read failed"));
      return;
    }
    cliOut->print(F("[SD] cat "));
    cliOut->print(path);
    cliOut->print(F(" ("));
    cliOut->print((uint32_t)bytes);
    cliOut->println(F(" B):"));
    cliOut->println(content);
    if (bytes >= sizeof(content) - 1) {
      cliOut->println(F("[SD] output truncated"));
    }
    return;
  }

  if (strncmp(args, "write ", 6) == 0) {
    const char *p = skipWs(args + 6);
    char path[64];
    copyToken(p, path, sizeof(path));
    if (path[0] == '\0') {
      cliOut->println(F("Use sd write <path> <text>"));
      return;
    }
    p += strlen(path);
    p = skipWs(p);
    if (*p == '\0') {
      cliOut->println(F("Use sd write <path> <text>"));
      return;
    }
    bool ok = sdStorageWriteTextFile(path, p, false);
    cliOut->println(ok ? F("[SD] write OK") : F("[SD] write FAIL"));
    return;
  }

  cliOut->println(F("Use: sd ls | sd anim | sd wifi | sd mqtt | sd bridge | sd log [0|1] | sd rw | sd cat <path> | sd write <path> <text>"));
}

static void printRxStatus() {
  char msg[96];
  (void)snprintf(msg, sizeof(msg), "[RX-3D8] enabled=%s hasFrame=%s frames=%lu partial=%lu log=%s",
                 stream3d8IsEnabled() ? "ON" : "OFF", stream3d8HasFrame() ? "YES" : "NO",
                 (unsigned long)stream3d8GetFrameCount(),
                 (unsigned long)stream3d8GetPartialChars(),
                 stream3d8GetLogging() ? "ON" : "OFF");
  // Status is explicitly requested and should be returned on bridge too.
  printCliAndSerialLine(msg, true);
}

static void applyRxCommand(const char *args) {
  args = skipWs(args);
  if (*args == '\0' || strcmp(args, "p") == 0) {
    printRxStatus();
    return;
  }

  if (strcmp(args, "on") == 0) {
    stream3d8SetEnabled(true);
    renderMode = MODE_RX_STREAM;
    printCliAndSerialLine("[RX-3D8] stream ON, mode=10");
    return;
  }

  if (strcmp(args, "off") == 0) {
    stream3d8SetEnabled(false);
    if (renderMode == MODE_RX_STREAM) {
      renderMode = MODE_WAVE;
    }
    printCliAndSerialLine("[RX-3D8] stream OFF");
    return;
  }

  if (strcmp(args, "clr") == 0) {
    stream3d8Reset();
    printCliAndSerialLine("[RX-3D8] buffer cleared");
    return;
  }

  if (strcmp(args, "fs") == 0) {
    stream3d8BeginFrame();
    if (!suppressCommandEcho) {
      printCliAndSerialLine("[RX-3D8] frame sync start");
    }
    return;
  }

  if (strcmp(args, "fe") == 0) {
    Stream3d8FinalizeResult fin = {};
    if (stream3d8FinalizeFrame(fin)) {
      printCliAndSerialRxFinalize(fin, true, stream3d8GetFrameCount());
      return;
    }
    printCliAndSerialRxFinalize(fin, false, stream3d8GetFrameCount());
    return;
  }

  if (strcmp(args, "log") == 0) {
    printCliAndSerialLine(stream3d8GetLogging() ? "[RX-3D8] log ON" : "[RX-3D8] log OFF");
    return;
  }

  if (strncmp(args, "log ", 4) == 0) {
    long v = 0;
    const char *p = skipWs(args + 4);
    if (!parseLongArg(p, v) || (v != 0 && v != 1)) {
      printCliAndSerialLine("Use rx log [0|1]");
      return;
    }
    stream3d8SetLogging(v != 0);
    printCliAndSerialLine(stream3d8GetLogging() ? "[RX-3D8] log ON" : "[RX-3D8] log OFF");
    return;
  }

  if (strncmp(args, "u ", 2) == 0 || strncmp(args, "d ", 2) == 0) {
    const char *payload = skipWs(args + 2);
    if (*payload == '\0') {
      printCliAndSerialLine("Use rx u <3D8-hex-chunk>");
      return;
    }
    (void)feedRxHexPayload(payload);
    return;
  }

  // Recovery path for noisy UART links that drop the leading "u ".
  if (looksLikeHexPayloadLine(args)) {
    (void)feedRxHexPayload(args);
    return;
  }

  printCliAndSerialLine("Use: rx on | rx off | rx clr | rx fs | rx fe | rx p | rx log [0|1] | rx u <3D8-hex-chunk>");
}

static void applyCommand(const char *line) {
  const char *fullLine = line;
  if (!suppressCommandEcho) {
    cliOut->print(F("> "));
    cliOut->println(line);
  }

  while (*line == ' ' || *line == '\t') {
    line++;
  }

  if (*line == '\0') {
    return;
  }

  char cmd[4] = {0, 0, 0, 0};
  uint8_t ci = 0;
  while (*line != '\0' && *line != ' ' && *line != '\t' && ci < 3) {
    cmd[ci++] = *line++;
  }
  cmd[ci] = '\0';

  while (*line == ' ' || *line == '\t') {
    line++;
  }

  if (strcmp(cmd, "h") == 0) {
    printHelp();
    return;
  }

  if (strcmp(cmd, "p") == 0) {
    printStatus();
    return;
  }

  if (strcmp(cmd, "sd") == 0) {
    applySdCommand(line);
    return;
  }

  if (strcmp(cmd, "rx") == 0) {
    applyRxCommand(line);
    return;
  }

  if (strcmp(cmd, "rb") == 0) {
    rx3d8RobustReset();
    return;
  }

  if (strcmp(cmd, "rk") == 0) {
    (void)rx3d8StoreRobustChunk(line);
    return;
  }

  if (strcmp(cmd, "rf") == 0) {
    (void)rx3d8FinalizeRobustFrame();
    return;
  }

  if (strcmp(cmd, "rst") == 0) {
    cliOut->println(F("Rebooting STM32..."));
    Serial.flush();
    delay(20);
    NVIC_SystemReset();
    return;
  }

  if (strcmp(cmd, "dp") == 0) {
    const char *arg = skipWs(line);
    if (*arg == '\0') {
      cliOut->print(F("display="));
      cliOut->println(displayEnabled ? F("ON") : F("OFF"));
      return;
    }
    if ((arg[0] == 't' || arg[0] == 'T') && arg[1] == '\0') {
      displayEnabled = !displayEnabled;
      cliOut->print(F("display="));
      cliOut->println(displayEnabled ? F("ON") : F("OFF"));
      return;
    }
    bool enabled = false;
    if (!parseBoolLike(arg, enabled)) {
      cliOut->println(F("Use dp [0|1|t]"));
      return;
    }
    displayEnabled = enabled;
    cliOut->print(F("display="));
    cliOut->println(displayEnabled ? F("ON") : F("OFF"));
    return;
  }

  if (strcmp(cmd, "r") == 0) {
    waveToggleSwapRG();
    rainToggleSwapRG();
    helloToggleSwapRG();
    cubePuzzleToggleSwapRG();
    pixelSetToggleSwapRG();
    cliOut->print(F("swapRG="));
    cliOut->println(waveGetSwapRG() ? F("ON") : F("OFF"));
    return;
  }

  if (strcmp(cmd, "vc") == 0) {
    pixelSetClear();
    rawTestClear();
    cliOut->println(F("pixelSet/rawTest cleared"));
    return;
  }

  if (strcmp(cmd, "tc") == 0) {
    rawTestClear();
    cliOut->println(F("rawTest cleared"));
    return;
  }

  if (strcmp(cmd, "td") == 0) {
    long vals[2] = {0, 0};
    uint8_t n = parseArgs(line, vals, 2);
    if (n != 2) {
      cliOut->println(F("Use td z ch"));
      return;
    }
    if (vals[0] < 0 || vals[0] > 7 || vals[1] < 1 || vals[1] > 192) {
      cliOut->println(F("z must be 0..7 and ch must be 1..192"));
      return;
    }
    rawTestSetOnlyChannel((uint8_t)vals[0], (uint16_t)vals[1]);
    renderMode = MODE_RAW_TEST;
    cliOut->print(F("rawTest z="));
    cliOut->print((uint8_t)vals[0]);
    cliOut->print(F(" ch="));
    cliOut->println((uint16_t)vals[1]);
    return;
  }

  if (strcmp(cmd, "tm") == 0) {
    long vals[4] = {0, 0, 0, 0};
    uint8_t n = parseArgs(line, vals, 4);
    if (n != 4) {
      cliOut->println(F("Use tm x y z c"));
      return;
    }
    if (vals[0] < 0 || vals[0] > 7 || vals[1] < 0 || vals[1] > 7 || vals[2] < 0 ||
        vals[2] > 7 || vals[3] < 0 || vals[3] > 2) {
      cliOut->println(F("x/y/z must be 0..7 and c must be 0..2"));
      return;
    }

    uint8_t x = (uint8_t)vals[0];
    uint8_t y = (uint8_t)vals[1];
    uint8_t z = (uint8_t)vals[2];
    uint8_t c = (uint8_t)vals[3];
    uint16_t ch = channelFor(x, y, c);
    rawTestSetOnlyMapped(x, y, z, c);
    renderMode = MODE_RAW_TEST;
    cliOut->print(F("rawTest mapped x="));
    cliOut->print(x);
    cliOut->print(F(" y="));
    cliOut->print(y);
    cliOut->print(F(" z="));
    cliOut->print(z);
    cliOut->print(F(" c="));
    cliOut->print(c);
    cliOut->print(F(" -> ch="));
    cliOut->println(ch);
    return;
  }

  if (strcmp(cmd, "th") == 0) {
    long vals[4] = {0, 0, 0, 0};
    uint8_t n = parseArgs(line, vals, 4);
    if (n != 4) {
      cliOut->println(F("Use th x y z c"));
      return;
    }
    if (vals[0] < 0 || vals[0] > 7 || vals[1] < 0 || vals[1] > 7 || vals[2] < 0 ||
        vals[2] > 7 || vals[3] < 0 || vals[3] > 2) {
      cliOut->println(F("x/y/z must be 0..7 and c must be 0..2"));
      return;
    }

    uint8_t x = (uint8_t)vals[0];
    uint8_t y = (uint8_t)vals[1];
    uint8_t z = (uint8_t)vals[2];
    uint8_t c = (uint8_t)vals[3];
    uint16_t ch = channelForRaw(x, y, c);
    rawTestSetOnlyRawLogical(x, y, z, c);
    renderMode = MODE_RAW_TEST;
    cliOut->print(F("rawTest raw x="));
    cliOut->print(x);
    cliOut->print(F(" y="));
    cliOut->print(y);
    cliOut->print(F(" z="));
    cliOut->print(z);
    cliOut->print(F(" c="));
    cliOut->print(c);
    cliOut->print(F(" -> ch="));
    cliOut->println(ch);
    return;
  }

  if (strcmp(cmd, "mg") == 0) {
    long vals[2] = {0, 0};
    uint8_t n = parseArgs(line, vals, 2);
    if (n != 2 || vals[0] < 0 || vals[0] > 7 || vals[1] < 0 || vals[1] > 7) {
      cliOut->println(F("Use mg x y (x/y in 0..7)"));
      return;
    }
    uint8_t x = (uint8_t)vals[0];
    uint8_t y = (uint8_t)vals[1];
    cliOut->print(F("map x="));
    cliOut->print(x);
    cliOut->print(F(" y="));
    cliOut->print(y);
    cliOut->print(F(" R="));
    cliOut->print(channelMapGet(x, y, 0));
    cliOut->print(F(" G="));
    cliOut->print(channelMapGet(x, y, 1));
    cliOut->print(F(" B="));
    cliOut->println(channelMapGet(x, y, 2));
    return;
  }

  if (strcmp(cmd, "mr") == 0) {
    channelMapResetDefaults();
    cliOut->println(F("mapping reset to defaults"));
    return;
  }

  if (strcmp(cmd, "mc") == 0) {
    long vals[4] = {0, 0, 0, 0};
    uint8_t n = parseArgs(line, vals, 4);
    if (n != 4 || vals[0] < 0 || vals[0] > 7 || vals[1] < 0 || vals[1] > 7 || vals[2] < 0 ||
        vals[2] > 2 || vals[3] < 0 || vals[3] > 192) {
      cliOut->println(F("Use mc x y c ch (x/y 0..7, c 0..2, ch 0..192)"));
      return;
    }
    uint8_t x = (uint8_t)vals[0];
    uint8_t y = (uint8_t)vals[1];
    uint8_t c = (uint8_t)vals[2];
    uint16_t ch = (uint16_t)vals[3];
    if (!channelMapSet(x, y, c, ch)) {
      cliOut->println(F("map set failed"));
      return;
    }
    cliOut->print(F("map set x="));
    cliOut->print(x);
    cliOut->print(F(" y="));
    cliOut->print(y);
    cliOut->print(F(" c="));
    cliOut->print(c);
    cliOut->print(F(" -> ch="));
    cliOut->println(ch);
    return;
  }

  if (strcmp(cmd, "vl") == 0) {
    cliOut->print(F("pixelSet count="));
    cliOut->println(pixelSetGetCount());
    return;
  }

  if (strcmp(cmd, "vp") == 0) {
    char target[5] = {0, 0, 0, 0, 0};
    uint8_t ti = 0;
    const char *p = line;
    while (*p != '\0' && *p != ' ' && *p != '\t' && ti < sizeof(target) - 1) {
      target[ti++] = *p++;
    }
    target[ti] = '\0';

    while (*p == ' ' || *p == '\t') {
      p++;
    }

    long vals[4] = {0, 0, 0, 0};
    uint8_t n = parseArgs(p, vals, 4);

    char axis = 0;
    bool idxInTarget = false;
    uint8_t idx = 0;

    if (ti == 2 && (target[0] == 'x' || target[0] == 'y' || target[0] == 'z') &&
        target[1] >= '0' && target[1] <= '7') {
      axis = target[0];
      idx = (uint8_t)(target[1] - '0');
      idxInTarget = true;
    } else if (ti == 3 && target[2] >= '0' && target[2] <= '7') {
      if (target[0] == 'x' && target[1] == 'y') {
        axis = 'z';
      } else if (target[0] == 'x' && target[1] == 'z') {
        axis = 'y';
      } else if (target[0] == 'y' && target[1] == 'z') {
        axis = 'x';
      }
      if (axis != 0) {
        idx = (uint8_t)(target[2] - '0');
        idxInTarget = true;
      }
    }

    uint8_t rgbStart = 0;
    if (!idxInTarget) {
      if (ti == 2 && target[0] == 'x' && target[1] == 'y') {
        axis = 'z';
      } else if (ti == 2 && target[0] == 'x' && target[1] == 'z') {
        axis = 'y';
      } else if (ti == 2 && target[0] == 'y' && target[1] == 'z') {
        axis = 'x';
      } else if (ti == 1 && (target[0] == 'x' || target[0] == 'y' || target[0] == 'z')) {
        axis = target[0];
      }
      if (axis == 0 || n < 1 || vals[0] < 0 || vals[0] > 7) {
        cliOut->println(F("Use vp x3 [r g b] or vp yz4 [r g b]"));
        return;
      }
      idx = (uint8_t)vals[0];
      rgbStart = 1;
    }

    uint8_t r = 0, g = 0, b = 1;
    uint8_t rgbCount = (uint8_t)(n - rgbStart);
    if (rgbCount == 3) {
      long rr = vals[rgbStart + 0];
      long gg = vals[rgbStart + 1];
      long bb = vals[rgbStart + 2];
      if (rr < 0 || rr > 1 || gg < 0 || gg > 1 || bb < 0 || bb > 1) {
        cliOut->println(F("RGB values must be 0 or 1"));
        return;
      }
      r = (uint8_t)rr;
      g = (uint8_t)gg;
      b = (uint8_t)bb;
    } else if (rgbCount != 0) {
      cliOut->println(F("Use vp x3 [r g b]"));
      return;
    }

    bool ok = pixelSetPaintPlane(axis, idx, r, g, b);
    if (!ok) {
      cliOut->println(F("plane paint failed"));
      return;
    }
    renderMode = MODE_PIXEL_SET;
    cliOut->print(F("plane "));
    cliOut->print(axis);
    cliOut->print('=');
    cliOut->print(idx);
    cliOut->println(F(" OK"));
    return;
  }

  if (strcmp(cmd, "vn") == 0) {
    char axisToken[2] = {0, 0};
    uint8_t ai = 0;
    const char *p = line;
    while (*p != '\0' && *p != ' ' && *p != '\t' && ai < 1) {
      axisToken[ai++] = *p++;
    }
    axisToken[ai] = '\0';
    while (*p == ' ' || *p == '\t') {
      p++;
    }

    char axis = axisToken[0];
    if (!(axis == 'x' || axis == 'y' || axis == 'z')) {
      cliOut->println(F("Use vn x a b [r g b]"));
      return;
    }

    long vals[5] = {0, 0, 0, 0, 0};
    uint8_t n = parseArgs(p, vals, 5);
    if (n != 2 && n != 5) {
      cliOut->println(F("Use vn x a b [r g b]"));
      return;
    }
    if (vals[0] < 0 || vals[0] > 7 || vals[1] < 0 || vals[1] > 7) {
      cliOut->println(F("Line coordinates must be 0..7"));
      return;
    }

    uint8_t r = 0, g = 0, b = 1;
    if (n == 5) {
      if (vals[2] < 0 || vals[2] > 1 || vals[3] < 0 || vals[3] > 1 || vals[4] < 0 ||
          vals[4] > 1) {
        cliOut->println(F("RGB values must be 0 or 1"));
        return;
      }
      r = (uint8_t)vals[2];
      g = (uint8_t)vals[3];
      b = (uint8_t)vals[4];
    }

    bool ok = pixelSetPaintLine(axis, (uint8_t)vals[0], (uint8_t)vals[1], r, g, b);
    if (!ok) {
      cliOut->println(F("line paint failed"));
      return;
    }
    renderMode = MODE_PIXEL_SET;
    cliOut->print(F("line "));
    cliOut->print(axis);
    cliOut->print(F(" a="));
    cliOut->print((uint8_t)vals[0]);
    cliOut->print(F(" b="));
    cliOut->print((uint8_t)vals[1]);
    cliOut->println(F(" OK"));
    return;
  }

  if (strcmp(cmd, "va") == 0 || strcmp(cmd, "vr") == 0) {
    long vals[6] = {0, 0, 0, 0, 0, 0};
    uint8_t n = parseArgs(line, vals, 6);
    if (n < 3) {
      cliOut->println(F("Need at least 3 numbers. Example: va 1 2 3 1 0 1"));
      return;
    }

    long x = vals[0];
    long y = vals[1];
    long z = vals[2];
    if (x < 0 || x > 7 || y < 0 || y > 7 || z < 0 || z > 7) {
      cliOut->println(F("Coordinates must be 0..7"));
      return;
    }

    bool ok = false;
    if (strcmp(cmd, "va") == 0) {
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 1; // default blue
      if (n == 6) {
        if ((vals[3] < 0 || vals[3] > 1) || (vals[4] < 0 || vals[4] > 1) ||
            (vals[5] < 0 || vals[5] > 1)) {
          cliOut->println(F("RGB values must be 0 or 1"));
          return;
        }
        r = (uint8_t)vals[3];
        g = (uint8_t)vals[4];
        b = (uint8_t)vals[5];
      } else if (n != 3) {
        cliOut->println(F("Use va x y z or va x y z r g b"));
        return;
      }

      ok = pixelSetAdd((uint8_t)x, (uint8_t)y, (uint8_t)z, r, g, b);
      cliOut->print(F("pixel add "));
    } else {
      ok = pixelSetRemove((uint8_t)x, (uint8_t)y, (uint8_t)z);
      cliOut->print(F("pixel remove "));
    }
    cliOut->println(ok ? F("OK") : F("FAIL"));
    cliOut->print(F("pixelSet count="));
    cliOut->println(pixelSetGetCount());
    return;
  }

  if ((line[0] == 'x' || line[0] == 'y' || line[0] == 'z') &&
      (line[1] == '+' || line[1] == '-')) {
    long steps = 0;
    if (!parseLongArg(line + 2, steps)) {
      cliOut->println(F("Move format: x+4 / y-2 / z+1"));
      return;
    }
    if (steps < 0)
      steps = -steps;
    if (steps > 32)
      steps = 32;

    int8_t amount = (int8_t)steps;
    if (line[1] == '-') {
      amount = (int8_t)-amount;
    }

    int8_t dx = 0, dy = 0, dz = 0;
    if (line[0] == 'x')
      dx = amount;
    else if (line[0] == 'y')
      dy = amount;
    else
      dz = amount;

    PixelSetMoveResult r = pixelSetTranslate(dx, dy, dz);
    cliOut->print(F("pixelSet moved kept="));
    cliOut->print(r.kept);
    cliOut->print(F(" dropped="));
    cliOut->println(r.dropped);
    return;
  }

  long v = 0;
  if (!parseLongArg(line, v)) {
    if (!suppressCommandEcho) {
      cliOut->println(F("No number parsed. Example: rn 20"));
    }
    return;
  }

  if (strcmp(cmd, "m") == 0) {
    if (v < 0)
      v = 0;
    if (v > 10)
      v = 10;
    renderMode = (RenderMode)v;
    if (renderMode == MODE_SD_ANIM) {
      sdStorageResetAnimationPlayback();
    }
    cliOut->print(F("mode="));
    cliOut->println((uint8_t)renderMode);
    if (renderMode == MODE_IR_DEBUG) {
      cliOut->println(F("IR debug mode ON: decoded NEC frames will be printed as [IR] ..."));
    } else if (renderMode == MODE_SD_ANIM && !sdStorageHasAnimations()) {
      cliOut->println(F("SD animation mode ON but no .3D8 files found."));
    } else if (renderMode == MODE_RX_STREAM && !stream3d8HasFrame()) {
      cliOut->println(F("RX stream mode ON: waiting for rx u <3D8-hex-chunk>"));
    }
    return;
  }

  if (strcmp(cmd, "f") == 0) {
    if (v < 10)
      v = 10;
    if (v > 80)
      v = 80;
    animStepMs = (uint16_t)v;
    cliOut->print(F("animStepMs="));
    cliOut->println(animStepMs);
    return;
  }

  if (strcmp(cmd, "sf") == 0) {
    sdAnimFrameMs = clampSdAnimFrameMs(v);
    cliOut->print(F("sdAnimFrameMs="));
    cliOut->println(sdAnimFrameMs);
    return;
  }

  if (strcmp(cmd, "ssp") == 0) {
    sdAnimTimedSpeedPct = clampSdAnimTimedSpeedPct(v);
    cliOut->print(F("sdAnimTimedSpeedPct="));
    cliOut->print(sdAnimTimedSpeedPct);
    cliOut->println('%');
    return;
  }

  if (strcmp(cmd, "st") == 0) {
    sdAnimTransitionEnabled = (v != 0);
    cliOut->print(F("sdAnimTransition="));
    cliOut->println(sdAnimTransitionEnabled ? F("ON") : F("OFF"));
    return;
  }

  if (strcmp(cmd, "stt") == 0) {
    sdAnimTransitionMs = clampSdAnimTransitionMs(v);
    cliOut->print(F("sdAnimTransitionMs="));
    cliOut->println(sdAnimTransitionMs);
    return;
  }

  if (strcmp(cmd, "s") == 0) {
    if (v < 1)
      v = 1;
    if (v > 20)
      v = 20;
    waveSetSpeed((uint8_t)v);
    cliOut->print(F("waveSpeed="));
    cliOut->println(waveGetSpeed());
    return;
  }

  if (strcmp(cmd, "k") == 0) {
    if (v < 1)
      v = 1;
    if (v > 60)
      v = 60;
    waveSetScale((uint8_t)v);
    cliOut->print(F("waveScale="));
    cliOut->println(waveGetScale());
    return;
  }

  if (strcmp(cmd, "t") == 0) {
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    waveSetThreshold((uint8_t)v);
    cliOut->print(F("waveThreshold="));
    cliOut->println(waveGetThreshold());
    return;
  }

  if (strcmp(cmd, "rn") == 0) {
    rainSetDropCount((uint8_t)v);
    cliOut->print(F("rainDrops="));
    cliOut->println(rainGetDropCount());
    return;
  }

  if (strcmp(cmd, "rv") == 0) {
    rainSetFallStepFrames((uint8_t)v);
    cliOut->print(F("rainFallFrames="));
    cliOut->println(rainGetFallStepFrames());
    return;
  }

  if (strcmp(cmd, "rl") == 0) {
    rainSetTailLength((uint8_t)v);
    cliOut->print(F("rainTailLength="));
    cliOut->println(rainGetTailLength());
    return;
  }

  if (strcmp(cmd, "rr") == 0) {
    rainSetRainbowStep((uint8_t)v);
    cliOut->print(F("rainRainbowStep="));
    cliOut->println(rainGetRainbowStep());
    return;
  }

  if (strcmp(cmd, "rd") == 0) {
    rainSetRespawnDelay((uint8_t)v);
    cliOut->print(F("rainRespawnDelay="));
    cliOut->println(rainGetRespawnDelay());
    return;
  }

  if (strcmp(cmd, "hs") == 0) {
    helloSetSpinStep((uint8_t)v);
    cliOut->print(F("helloSpinStep="));
    cliOut->println(helloGetSpinStep());
    return;
  }

  if (strcmp(cmd, "hf") == 0) {
    helloSetFlyFrames((uint8_t)v);
    cliOut->print(F("helloFlyFrames="));
    cliOut->println(helloGetFlyFrames());
    return;
  }

  if (strcmp(cmd, "hh") == 0) {
    helloSetHoldFrames((uint8_t)v);
    cliOut->print(F("helloHoldFrames="));
    cliOut->println(helloGetHoldFrames());
    return;
  }

  if (strcmp(cmd, "hb") == 0) {
    helloSetGapFrames((uint8_t)v);
    cliOut->print(F("helloGapFrames="));
    cliOut->println(helloGetGapFrames());
    return;
  }

  if (strcmp(cmd, "eu") == 0) {
    espAtBridgeSetEnabled(v != 0);
    cliOut->print(F("espUart="));
    cliOut->println(espAtBridgeIsEnabled() ? F("ON") : F("OFF"));
    return;
  }

  // Recovery path: in RX mode allow plain hex payload lines (without "rx u " prefix).
  // Keep this late so explicit commands like rb/rk/rf are parsed first.
  if ((renderMode == MODE_RX_STREAM || stream3d8IsEnabled()) && looksLikeHexPayloadLine(fullLine)) {
    (void)feedRxHexPayload(fullLine);
    return;
  }

  if (!suppressCommandEcho) {
    cliOut->println(F("Unknown command. Type: h"));
  }
}

void serialCliInit() {
  serialResetLine();
  cliOut = &Serial;
  rx3d8RobustReset();
  printHelp();
}

void serialCliProcessChar(char c, Print &out, bool echoInput) {
  cliOut = &out;

  if (c == '\r') {
    return;
  }

  if (c == '\n') {
    lineBuf[lineLen] = '\0';
    suppressCommandEcho = !echoInput;
    applyCommand(lineBuf);
    suppressCommandEcho = false;
    serialResetLine();
    return;
  }

  if (echoInput) {
    out.write((uint8_t)c);
  }

  if (lineLen < sizeof(lineBuf) - 1) {
    lineBuf[lineLen++] = c;
    lineBuf[lineLen] = '\0';
  } else {
    out.println(F("\n(line too long)"));
    serialResetLine();
  }
}

void serialCliHandleStream(Stream &io, bool echoInput) {
  while (io.available() > 0) {
    serialCliProcessChar((char)io.read(), io, echoInput);
  }
}

void serialCliHandle() { serialCliHandleStream(Serial, true); }

RenderMode serialCliGetRenderMode() { return renderMode; }

void serialCliSetRenderMode(RenderMode mode) {
  renderMode = mode;
  if (renderMode == MODE_SD_ANIM) {
    sdStorageResetAnimationPlayback();
  }
}

uint16_t serialCliGetAnimStepMs() { return animStepMs; }

uint16_t serialCliGetSdAnimFrameMs() { return sdAnimFrameMs; }

uint16_t serialCliAdjustSdAnimFrameMs(int16_t deltaMs) {
  long next = (long)sdAnimFrameMs + (long)deltaMs;
  sdAnimFrameMs = clampSdAnimFrameMs(next);
  return sdAnimFrameMs;
}

uint16_t serialCliGetSdAnimTimedSpeedPct() { return sdAnimTimedSpeedPct; }

uint16_t serialCliAdjustSdAnimTimedSpeedPct(int16_t deltaPct) {
  long next = (long)sdAnimTimedSpeedPct + (long)deltaPct;
  sdAnimTimedSpeedPct = clampSdAnimTimedSpeedPct(next);
  return sdAnimTimedSpeedPct;
}

bool serialCliGetSdAnimTransitionEnabled() { return sdAnimTransitionEnabled; }

uint16_t serialCliGetSdAnimTransitionMs() { return sdAnimTransitionMs; }

uint16_t serialCliGetSdAnimRenderTickMs() {
  if (!sdAnimTransitionEnabled || sdAnimTransitionMs == 0) {
    return sdAnimFrameMs;
  }
  return (sdAnimFrameMs < SD_ANIM_RENDER_TICK_MS) ? sdAnimFrameMs : SD_ANIM_RENDER_TICK_MS;
}

bool serialCliIsDisplayEnabled() { return displayEnabled; }

void serialCliSetDisplayEnabled(bool enabled) { displayEnabled = enabled; }
