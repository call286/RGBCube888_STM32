#ifndef SDFAT_FILE_TYPE
#define SDFAT_FILE_TYPE 3
#endif

#include "sd_storage.h"

#include <SPI.h>
#include <SdFat.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "channel_mapper.h"
#include "cube_config.h"
#include "esp_at_bridge.h"
#include "refresh_engine.h"

namespace {

static SPIClass sdSpi(CubeConfig::SD_SPI_MOSI, CubeConfig::SD_SPI_MISO, CubeConfig::SD_SPI_SCK);
static SdFs sdFs;
static bool sdReady = false;

static const uint8_t MAX_TREE_DEPTH = 4;
static const uint16_t MAX_TREE_ENTRIES = 128;
static const uint16_t MAX_ANIM_FILES = 32;
static const uint32_t VOXELS_PER_FRAME_3D8 = 8UL * 8UL * 8UL;
static const uint16_t BYTES_PER_VOXEL_3D8 = 6; // ASCII hex: RR GG BB
static const uint16_t PLAYBACK_FRAME_HEX_BYTES =
    (uint16_t)(VOXELS_PER_FRAME_3D8 * BYTES_PER_VOXEL_3D8); // 3072
static const uint16_t PLAYBACK_TIMED_FRAME_BYTES = (uint16_t)(PLAYBACK_FRAME_HEX_BYTES + 4u); // +TTTT
static const uint16_t CHANNELS_PER_LAYER = CubeConfig::BYTES_PER_CHAIN * 16u;

static SdAnimationFileInfo animFiles[MAX_ANIM_FILES];
static uint16_t animCount = 0;
static FsFile playbackFile;
static uint16_t playbackAnimIndex = 0;
static uint32_t playbackFrameInAnim = 0;
static uint8_t playbackFrameBuf[PLAYBACK_TIMED_FRAME_BYTES];
struct PlaybackFrameState {
  uint8_t channels[CubeConfig::LAYERS][CHANNELS_PER_LAYER];
  uint16_t animIndex;
  uint32_t frameIndex;
  uint16_t durationMs;
  bool valid;
};
static PlaybackFrameState playbackFromState;
static PlaybackFrameState playbackToState;
static uint32_t playbackPhaseStartMs = 0;
static bool playbackLoggingEnabled = false;
static uint16_t playbackLastReportedAnim = 0xFFFFu;
static uint32_t playbackLastReportedFrame = 0xFFFFFFFFul;
static uint32_t playbackLastReportMs = 0;
static const uint16_t PLAYBACK_REPORT_MS = 200;

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

struct BridgeSettings {
  bool valid;
  char sourcePath[40];
  char ssid[64];
  char pass[96];
  bool hasWifiSsid;
  bool hasWifiPass;
  bool mqttEnabled;
  bool hasMqttEnabled;
  char mqttHost[64];
  uint16_t mqttPort;
  bool hasMqttPort;
  char mqttUser[64];
  char mqttPass[96];
  char mqttPrefix[64];
  char mqttClientId[64];
  bool esphomeEnabled;
  bool hasEsphomeEnabled;
  char esphomeNode[64];
};
static BridgeSettings bridgeSettings;

static bool equalsIgnoreCase(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
      return false;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool startsWithIgnoreCase(const char *s, const char *prefix) {
  while (*prefix != '\0' && *s != '\0') {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
      return false;
    }
    s++;
    prefix++;
  }
  return *prefix == '\0';
}

static const char *lastDot(const char *s) {
  const char *dot = nullptr;
  while (*s != '\0') {
    if (*s == '.') {
      dot = s;
    }
    s++;
  }
  return dot;
}

static bool has3d8Extension(const char *name) {
  const char *dot = lastDot(name);
  return dot != nullptr && equalsIgnoreCase(dot, ".3d8");
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

static bool parseHexByte(char hi, char lo, uint8_t &out) {
  int8_t h = hexNibble(hi);
  int8_t l = hexNibble(lo);
  if (h < 0 || l < 0) {
    return false;
  }
  out = (uint8_t)((h << 4) | l);
  return true;
}

static void copyText(char *dst, size_t dstSize, const char *src) {
  if (dstSize == 0) {
    return;
  }
  size_t i = 0;
  while (src[i] != '\0' && i < dstSize - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static char *ltrimInPlace(char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  return s;
}

static void rtrimInPlace(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

static char *trimInPlace(char *s) {
  char *t = ltrimInPlace(s);
  rtrimInPlace(t);
  return t;
}

static void stripQuotesInPlace(char *s) {
  size_t n = strlen(s);
  if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
}

static void normalizePath(const char *path, char *out, size_t outSize) {
  if (path == nullptr || outSize == 0) {
    return;
  }
  if (path[0] == '/') {
    snprintf(out, outSize, "%s", path);
  } else {
    snprintf(out, outSize, "/%s", path);
  }
}

static bool readLine(FsFile &file, char *line, size_t lineSize) {
  if (lineSize == 0) {
    return false;
  }
  size_t n = 0;
  bool sawAny = false;
  while (true) {
    int c = file.read();
    if (c < 0) {
      break;
    }
    sawAny = true;
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      break;
    }
    if (n + 1 < lineSize) {
      line[n++] = (char)c;
    }
  }
  line[n] = '\0';
  return sawAny || n > 0;
}

static bool isCommentOrEmpty(const char *line) {
  if (line[0] == '\0') {
    return true;
  }
  if (line[0] == ';') {
    return true;
  }
  if (line[0] == '#' && !startsWithIgnoreCase(line, "#define")) {
    return true;
  }
  return line[0] == '/' && line[1] == '/';
}

static bool isSsidKey(const char *key) {
  return equalsIgnoreCase(key, "ssid") || equalsIgnoreCase(key, "wifi_ssid") ||
         equalsIgnoreCase(key, "bridge_wifi_ssid") || equalsIgnoreCase(key, "BRIDGE_WIFI_SSID");
}

static bool isPassKey(const char *key) {
  return equalsIgnoreCase(key, "pass") || equalsIgnoreCase(key, "password") ||
         equalsIgnoreCase(key, "wifi_pass") || equalsIgnoreCase(key, "wifi_password") ||
         equalsIgnoreCase(key, "bridge_wifi_pass") || equalsIgnoreCase(key, "BRIDGE_WIFI_PASS");
}

static bool isMqttHostKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_host") || equalsIgnoreCase(key, "bridge_mqtt_host") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_HOST");
}

static bool isMqttEnabledKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_enabled") || equalsIgnoreCase(key, "bridge_mqtt_enabled") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_ENABLED");
}

static bool isMqttPortKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_port") || equalsIgnoreCase(key, "bridge_mqtt_port") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_PORT");
}

static bool isMqttUserKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_user") || equalsIgnoreCase(key, "bridge_mqtt_user") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_USER");
}

static bool isMqttPassKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_pass") || equalsIgnoreCase(key, "mqtt_password") ||
         equalsIgnoreCase(key, "bridge_mqtt_pass") || equalsIgnoreCase(key, "BRIDGE_MQTT_PASS");
}

static bool isMqttPrefixKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_prefix") || equalsIgnoreCase(key, "bridge_mqtt_prefix") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_PREFIX");
}

static bool isMqttClientIdKey(const char *key) {
  return equalsIgnoreCase(key, "mqtt_client_id") || equalsIgnoreCase(key, "bridge_mqtt_client_id") ||
         equalsIgnoreCase(key, "BRIDGE_MQTT_CLIENT_ID");
}

static bool isEsphomeModeKey(const char *key) {
  return equalsIgnoreCase(key, "esphome_mode") || equalsIgnoreCase(key, "bridge_esphome_mode") ||
         equalsIgnoreCase(key, "BRIDGE_ESPHOME_MODE");
}

static bool isEsphomeNodeKey(const char *key) {
  return equalsIgnoreCase(key, "esphome_node") || equalsIgnoreCase(key, "bridge_esphome_node") ||
         equalsIgnoreCase(key, "BRIDGE_ESPHOME_NODE");
}

static bool parseBoolLike(const char *value, bool &out) {
  if (equalsIgnoreCase(value, "1") || equalsIgnoreCase(value, "true") ||
      equalsIgnoreCase(value, "on") || equalsIgnoreCase(value, "yes") ||
      equalsIgnoreCase(value, "enabled")) {
    out = true;
    return true;
  }
  if (equalsIgnoreCase(value, "0") || equalsIgnoreCase(value, "false") ||
      equalsIgnoreCase(value, "off") || equalsIgnoreCase(value, "no") ||
      equalsIgnoreCase(value, "disabled")) {
    out = false;
    return true;
  }
  return false;
}

static bool parseDefineLine(const char *line, char *key, size_t keySize, char *value,
                            size_t valueSize) {
  if (!startsWithIgnoreCase(line, "#define")) {
    return false;
  }

  const char *p = line + 7;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p == '\0') {
    return false;
  }

  size_t ki = 0;
  while (*p != '\0' && *p != ' ' && *p != '\t' && ki + 1 < keySize) {
    key[ki++] = *p++;
  }
  key[ki] = '\0';

  while (*p == ' ' || *p == '\t') {
    p++;
  }
  copyText(value, valueSize, p);
  char *trimmed = trimInPlace(value);
  if (trimmed != value) {
    memmove(value, trimmed, strlen(trimmed) + 1);
  }
  stripQuotesInPlace(value);
  // Treat empty values as handled to avoid triggering positional SSID/PASS fallback.
  return key[0] != '\0';
}

static bool parseKeyValueLine(const char *line, char *key, size_t keySize, char *value,
                              size_t valueSize) {
  const char *eq = strchr(line, '=');
  const char *colon = strchr(line, ':');
  const char *sep = nullptr;
  if (eq == nullptr) {
    sep = colon;
  } else if (colon == nullptr) {
    sep = eq;
  } else {
    sep = (eq < colon) ? eq : colon;
  }
  if (sep == nullptr) {
    return false;
  }

  size_t keyLen = (size_t)(sep - line);
  if (keyLen == 0) {
    return false;
  }
  if (keyLen >= keySize) {
    keyLen = keySize - 1;
  }
  memcpy(key, line, keyLen);
  key[keyLen] = '\0';
  char *k = trimInPlace(key);
  if (k != key) {
    memmove(key, k, strlen(k) + 1);
  }

  copyText(value, valueSize, sep + 1);
  char *v = trimInPlace(value);
  if (v != value) {
    memmove(value, v, strlen(v) + 1);
  }
  stripQuotesInPlace(value);
  // Treat empty values as handled to avoid triggering positional SSID/PASS fallback.
  return key[0] != '\0';
}

static void printMaskedPassword(Print &out, const char *pass) {
  size_t n = strlen(pass);
  if (n == 0) {
    out.print(F("<empty>"));
    return;
  }
  out.print(F("***"));
  out.print(n);
  out.print(F(" chars***"));
}

static void appendEscaped(char *dst, size_t dstSize, size_t &pos, const char *src) {
  while (*src != '\0' && pos + 1 < dstSize) {
    char c = *src++;
    if ((c == '"' || c == '\\') && pos + 2 < dstSize) {
      dst[pos++] = '\\';
    }
    if (pos + 1 < dstSize) {
      dst[pos++] = c;
    }
  }
}

static void appendRaw(char *dst, size_t dstSize, size_t &pos, const char *src) {
  while (*src != '\0' && pos + 1 < dstSize) {
    dst[pos++] = *src++;
  }
}

static bool sendBridgeConfigLine(const char *line) {
  if (line == nullptr || line[0] == '\0') {
    return false;
  }
  bool ok = espAtBridgeSendLine(line);
  // ESP side uses SoftwareSerial; spacing commands improves reliability.
  delay(12);
  return ok;
}

static void buildPath(char *out, size_t outSize, const char *basePath, const char *name) {
  if (strcmp(basePath, "/") == 0) {
    snprintf(out, outSize, "/%s", name);
  } else {
    snprintf(out, outSize, "%s/%s", basePath, name);
  }
}

static bool readFilePreview(const char *path, char *out, size_t outSize) {
  if (outSize == 0) {
    return false;
  }
  FsFile file = sdFs.open(path, O_RDONLY);
  if (!file || !file.isFile()) {
    out[0] = '\0';
    return false;
  }

  size_t n = 0;
  while (n + 1 < outSize) {
    int c = file.read();
    if (c < 0 || c == '\r' || c == '\n') {
      break;
    }
    out[n++] = (char)c;
  }
  out[n] = '\0';
  file.close();
  return n > 0;
}

static void printIndent(Print &out, uint8_t depth) {
  for (uint8_t i = 0; i < depth; i++) {
    out.print(F("  "));
  }
}

static void clearPlaybackState(PlaybackFrameState &state) {
  memset(state.channels, 0, sizeof(state.channels));
  state.animIndex = 0;
  state.frameIndex = 0;
  state.durationMs = 20;
  state.valid = false;
}

static void closePlaybackFile() {
  if (playbackFile.isOpen()) {
    playbackFile.close();
  }
}

static bool openPlaybackFile(uint16_t index) {
  if (index >= animCount) {
    return false;
  }
  closePlaybackFile();
  if (!playbackFile.open(animFiles[index].path, O_RDONLY) || !playbackFile.isFile()) {
    closePlaybackFile();
    return false;
  }
  playbackAnimIndex = index;
  playbackFrameInAnim = 0;
  return true;
}

static bool openNextPlaybackFile() {
  if (animCount == 0) {
    return false;
  }
  for (uint16_t attempt = 0; attempt < animCount; attempt++) {
    uint16_t nextIndex = (uint16_t)((playbackAnimIndex + 1 + attempt) % animCount);
    if (openPlaybackFile(nextIndex)) {
      return true;
    }
  }
  return false;
}

static bool decodeFrameBufferToState(const uint8_t *buf, size_t len, PlaybackFrameState &out) {
  if (len < PLAYBACK_FRAME_HEX_BYTES) {
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
        if (!parseHexByte((char)buf[base + 0], (char)buf[base + 1], r) ||
            !parseHexByte((char)buf[base + 2], (char)buf[base + 3], g) ||
            !parseHexByte((char)buf[base + 4], (char)buf[base + 5], b)) {
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

  out.durationMs = 20;
  if (len >= PLAYBACK_TIMED_FRAME_BYTES) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (parseHexByte((char)buf[PLAYBACK_FRAME_HEX_BYTES + 0], (char)buf[PLAYBACK_FRAME_HEX_BYTES + 1],
                     hi) &&
        parseHexByte((char)buf[PLAYBACK_FRAME_HEX_BYTES + 2], (char)buf[PLAYBACK_FRAME_HEX_BYTES + 3], lo)) {
      uint16_t ms = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
      out.durationMs = (ms == 0) ? 1 : ms;
    }
  }

  out.valid = true;
  return true;
}

static bool readNextPlaybackState(PlaybackFrameState &out) {
  if (!sdReady || animCount == 0) {
    return false;
  }

  for (uint16_t attempt = 0; attempt < (uint16_t)(animCount + 1); attempt++) {
    if (!playbackFile.isOpen()) {
      if (!openPlaybackFile(playbackAnimIndex)) {
        if (!openNextPlaybackFile()) {
          return false;
        }
      }
    }

    uint16_t sourceAnimIndex = playbackAnimIndex;
    uint32_t sourceFrameIndex = playbackFrameInAnim;
    uint16_t frameBytes = animFiles[sourceAnimIndex].frameBytes;
    if (frameBytes == 0) {
      frameBytes = PLAYBACK_FRAME_HEX_BYTES;
    }
    size_t br = playbackFile.read(playbackFrameBuf, frameBytes);
    if (br != frameBytes) {
      if (!openNextPlaybackFile()) {
        return false;
      }
      continue;
    }

    if (!decodeFrameBufferToState(playbackFrameBuf, br, out)) {
      return false;
    }

    out.animIndex = sourceAnimIndex;
    out.frameIndex = sourceFrameIndex;
    out.valid = true;

    playbackFrameInAnim++;
    uint32_t maxFrames = animFiles[sourceAnimIndex].frameCount;
    if (maxFrames == 0 || playbackFrameInAnim >= maxFrames) {
      (void)openNextPlaybackFile();
    }
    return true;
  }

  return false;
}

static void renderPlaybackState(const PlaybackFrameState &state) {
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

static void renderTransitionState(const PlaybackFrameState &from, const PlaybackFrameState &to,
                                  uint16_t progress256) {
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

static void reportPlaybackStatus(bool force, uint8_t blendPercent) {
  if (!playbackLoggingEnabled) {
    return;
  }
  if (!sdReady || animCount == 0 || !playbackFromState.valid || playbackFromState.animIndex >= animCount) {
    return;
  }

  bool animChanged = playbackFromState.animIndex != playbackLastReportedAnim;
  bool frameChanged = playbackFromState.frameIndex != playbackLastReportedFrame;
  uint32_t now = millis();

  if (!force && !animChanged && !frameChanged &&
      (uint32_t)(now - playbackLastReportMs) < PLAYBACK_REPORT_MS) {
    return;
  }

  playbackLastReportMs = now;
  playbackLastReportedAnim = playbackFromState.animIndex;
  playbackLastReportedFrame = playbackFromState.frameIndex;

  const SdAnimationFileInfo &f = animFiles[playbackFromState.animIndex];
  Serial.print(F("[SD-PLAY] anim "));
  Serial.print((uint16_t)(playbackFromState.animIndex + 1));
  Serial.print('/');
  Serial.print(animCount);
  Serial.print(F(" file="));
  Serial.print(f.path);
  Serial.print(F(" sub="));
  Serial.print((uint32_t)(playbackFromState.frameIndex + 1));
  Serial.print('/');
  Serial.print(f.frameCount);

  if (playbackToState.valid && playbackToState.animIndex < animCount) {
    Serial.print(F(" next="));
    Serial.print((uint16_t)(playbackToState.animIndex + 1));
    Serial.print(':');
    Serial.print((uint32_t)(playbackToState.frameIndex + 1));
  }

  Serial.print(F(" blend="));
  Serial.print((uint16_t)blendPercent);
  Serial.println('%');
}

static void printTreeRecursive(Print &out, const char *path, uint8_t depth, uint16_t &count) {
  if (depth > MAX_TREE_DEPTH || count >= MAX_TREE_ENTRIES) {
    return;
  }

  FsFile dir = sdFs.open(path, O_RDONLY);
  if (!dir || !dir.isDir()) {
    return;
  }

  FsFile entry;
  while (count < MAX_TREE_ENTRIES && entry.openNext(&dir, O_RDONLY)) {
    char name[64] = {0};
    entry.getName(name, sizeof(name));

    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      entry.close();
      continue;
    }

    char fullPath[96] = {0};
    buildPath(fullPath, sizeof(fullPath), path, name);

    printIndent(out, depth);
    if (entry.isDir()) {
      out.print(F("[D] "));
      out.println(fullPath);
      count++;
      entry.close();
      printTreeRecursive(out, fullPath, depth + 1, count);
      continue;
    }

    out.print(F("[F] "));
    out.print(fullPath);
    out.print(F(" ("));
    out.print((uint32_t)entry.fileSize());
    out.println(F(" B)"));
    count++;
    entry.close();
  }
  dir.close();
}

static bool parseBridgeFile(const char *path, BridgeSettings &cfg) {
  FsFile file = sdFs.open(path, O_RDONLY);
  if (!file || !file.isFile()) {
    return false;
  }

  cfg.valid = false;
  cfg.ssid[0] = '\0';
  cfg.pass[0] = '\0';
  cfg.hasWifiSsid = false;
  cfg.hasWifiPass = false;
  cfg.mqttEnabled = false;
  cfg.hasMqttEnabled = false;
  cfg.mqttHost[0] = '\0';
  cfg.mqttPort = 1883;
  cfg.hasMqttPort = false;
  cfg.mqttUser[0] = '\0';
  cfg.mqttPass[0] = '\0';
  cfg.mqttPrefix[0] = '\0';
  cfg.mqttClientId[0] = '\0';
  cfg.esphomeEnabled = false;
  cfg.hasEsphomeEnabled = false;
  cfg.esphomeNode[0] = '\0';
  copyText(cfg.sourcePath, sizeof(cfg.sourcePath), path);

  char line[192];
  uint8_t positional = 0;
  bool sawAnySetting = false;
  while (readLine(file, line, sizeof(line))) {
    char *trimmed = trimInPlace(line);
    if (isCommentOrEmpty(trimmed)) {
      continue;
    }

    char key[64] = {0};
    char value[128] = {0};

    bool handled = parseDefineLine(trimmed, key, sizeof(key), value, sizeof(value)) ||
                   parseKeyValueLine(trimmed, key, sizeof(key), value, sizeof(value));

    if (handled) {
      if (isSsidKey(key)) {
        copyText(cfg.ssid, sizeof(cfg.ssid), value);
        cfg.hasWifiSsid = true;
        sawAnySetting = true;
      } else if (isPassKey(key)) {
        copyText(cfg.pass, sizeof(cfg.pass), value);
        cfg.hasWifiPass = true;
        sawAnySetting = true;
      } else if (isMqttHostKey(key)) {
        copyText(cfg.mqttHost, sizeof(cfg.mqttHost), value);
        sawAnySetting = true;
      } else if (isMqttEnabledKey(key)) {
        bool b = false;
        if (parseBoolLike(value, b)) {
          cfg.mqttEnabled = b;
          cfg.hasMqttEnabled = true;
          sawAnySetting = true;
        }
      } else if (isMqttPortKey(key)) {
        long p = strtol(value, nullptr, 10);
        if (p > 0 && p <= 65535) {
          cfg.mqttPort = (uint16_t)p;
          cfg.hasMqttPort = true;
          sawAnySetting = true;
        }
      } else if (isMqttUserKey(key)) {
        copyText(cfg.mqttUser, sizeof(cfg.mqttUser), value);
        sawAnySetting = true;
      } else if (isMqttPassKey(key)) {
        copyText(cfg.mqttPass, sizeof(cfg.mqttPass), value);
        sawAnySetting = true;
      } else if (isMqttPrefixKey(key)) {
        copyText(cfg.mqttPrefix, sizeof(cfg.mqttPrefix), value);
        sawAnySetting = true;
      } else if (isMqttClientIdKey(key)) {
        copyText(cfg.mqttClientId, sizeof(cfg.mqttClientId), value);
        sawAnySetting = true;
      } else if (isEsphomeModeKey(key)) {
        bool b = false;
        if (parseBoolLike(value, b)) {
          cfg.esphomeEnabled = b;
          cfg.hasEsphomeEnabled = true;
          sawAnySetting = true;
        }
      } else if (isEsphomeNodeKey(key)) {
        copyText(cfg.esphomeNode, sizeof(cfg.esphomeNode), value);
        sawAnySetting = true;
      }
      continue;
    }

    // Fallback: first non-empty line = SSID, second = password.
    if (positional == 0) {
      copyText(cfg.ssid, sizeof(cfg.ssid), trimmed);
      cfg.hasWifiSsid = true;
      positional = 1;
      sawAnySetting = true;
    } else if (positional == 1) {
      copyText(cfg.pass, sizeof(cfg.pass), trimmed);
      cfg.hasWifiPass = true;
      positional = 2;
      sawAnySetting = true;
    }
  }

  file.close();
  cfg.valid = cfg.hasWifiSsid && cfg.hasWifiPass && cfg.ssid[0] != '\0' && cfg.pass[0] != '\0';
  if (cfg.mqttHost[0] != '\0') {
    cfg.mqttEnabled = true;
  }
  return sawAnySetting;
}

} // namespace

void sdStorageInit() {
  sdReady = false;
  animCount = 0;
  bridgeSettings.valid = false;
  bridgeSettings.sourcePath[0] = '\0';
  bridgeSettings.ssid[0] = '\0';
  bridgeSettings.pass[0] = '\0';
  bridgeSettings.mqttEnabled = false;
  bridgeSettings.mqttHost[0] = '\0';
  bridgeSettings.mqttPort = 1883;
  bridgeSettings.mqttUser[0] = '\0';
  bridgeSettings.mqttPass[0] = '\0';
  bridgeSettings.mqttPrefix[0] = '\0';
  bridgeSettings.mqttClientId[0] = '\0';
  bridgeSettings.esphomeEnabled = false;
  bridgeSettings.esphomeNode[0] = '\0';

  pinMode(CubeConfig::SD_SPI_CS, OUTPUT);
  digitalWrite(CubeConfig::SD_SPI_CS, HIGH);
  sdSpi.begin();

  SdSpiConfig spiCfg(CubeConfig::SD_SPI_CS, SHARED_SPI, SD_SCK_MHZ(CubeConfig::SD_SPI_MHZ), &sdSpi);
  if (!sdFs.begin(spiCfg)) {
    Serial.println(F("[SD] mount failed"));
    sdFs.initErrorPrint(&Serial);
    return;
  }

  sdReady = true;
  Serial.print(F("[SD] mounted filesystem: "));
  sdFs.printFatType(&Serial);
  Serial.println();

  sdStoragePrintTree(Serial);

  char bootLine[64];
  snprintf(bootLine, sizeof(bootLine), "boot_ms=%lu\n", (unsigned long)millis());
  if (sdStorageWriteTextFile("/stm32_boot.log", bootLine, true)) {
    Serial.println(F("[SD] append /stm32_boot.log OK"));
  } else {
    Serial.println(F("[SD] append /stm32_boot.log FAIL"));
  }

  sdStorageScanAnimations();
  sdStoragePrintAnimations(Serial);

  if (sdStorageLoadBridgeSettings()) {
    (void)sdStoragePushBridgeSettingsToEsp();
  } else {
    Serial.println(F("[SD] no WiFi settings found in SD root"));
  }
}

bool sdStorageIsReady() { return sdReady; }

bool sdStorageHasAnimations() { return sdReady && animCount > 0; }

void sdStoragePrintTree(Print &out) {
  if (!sdReady) {
    out.println(F("[SD] not mounted"));
    return;
  }
  out.println(F("[SD] directory tree:"));
  uint16_t count = 0;
  printTreeRecursive(out, "/", 0, count);
  if (count >= MAX_TREE_ENTRIES) {
    out.println(F("[SD] listing truncated"));
  }
}

bool sdStorageReadTextFile(const char *path, char *out, size_t outSize, size_t *bytesRead) {
  if (!sdReady || path == nullptr || out == nullptr || outSize == 0) {
    if (bytesRead != nullptr) {
      *bytesRead = 0;
    }
    return false;
  }
  char fullPath[96];
  normalizePath(path, fullPath, sizeof(fullPath));

  FsFile file = sdFs.open(fullPath, O_RDONLY);
  if (!file || !file.isFile()) {
    if (bytesRead != nullptr) {
      *bytesRead = 0;
    }
    return false;
  }

  size_t i = 0;
  while (i + 1 < outSize) {
    int c = file.read();
    if (c < 0) {
      break;
    }
    out[i++] = (char)c;
  }
  out[i] = '\0';
  if (bytesRead != nullptr) {
    *bytesRead = i;
  }
  file.close();
  return true;
}

bool sdStorageWriteTextFile(const char *path, const char *text, bool append) {
  if (!sdReady || path == nullptr || text == nullptr) {
    return false;
  }

  char fullPath[96];
  normalizePath(path, fullPath, sizeof(fullPath));

  oflag_t flags = O_WRONLY | O_CREAT;
  flags |= append ? O_APPEND : O_TRUNC;

  FsFile file = sdFs.open(fullPath, flags);
  if (!file) {
    return false;
  }

  size_t len = strlen(text);
  size_t wrote = file.write(text, len);
  bool ok = wrote == len && file.sync();
  file.close();
  return ok;
}

bool sdStorageReadWriteSelfTest(Print &out) {
  if (!sdReady) {
    out.println(F("[SD] not mounted"));
    return false;
  }

  static const char *TEST_PATH = "/stm32_rw_test.txt";
  char line[64];
  snprintf(line, sizeof(line), "rw_test_ms=%lu\n", (unsigned long)millis());

  if (!sdStorageWriteTextFile(TEST_PATH, line, false)) {
    out.println(F("[SD] self-test write FAIL"));
    return false;
  }

  char verify[96];
  size_t n = 0;
  if (!sdStorageReadTextFile(TEST_PATH, verify, sizeof(verify), &n)) {
    out.println(F("[SD] self-test read FAIL"));
    return false;
  }

  out.print(F("[SD] self-test OK path="));
  out.print(TEST_PATH);
  out.print(F(" bytes="));
  out.println((uint32_t)n);
  return true;
}

bool sdStorageLoadWifiSettings() {
  return sdStorageLoadBridgeSettings();
}

bool sdStorageLoadBridgeSettings() {
  if (!sdReady) {
    return false;
  }

  static const char *CANDIDATES[] = {"/wifi.txt", "/wifi.cfg", "/wifi.ini", "/bridge_wifi.txt",
                                     "/bridge_mqtt.txt", "/bridge_config.txt", "/bridge_secrets.h"};

  BridgeSettings merged = {};
  merged.mqttPort = 1883;
  bool sawAnyFile = false;
  for (uint8_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
    BridgeSettings parsed = {};
    if (parseBridgeFile(CANDIDATES[i], parsed)) {
      sawAnyFile = true;
      copyText(merged.sourcePath, sizeof(merged.sourcePath), CANDIDATES[i]);
      if (parsed.hasWifiSsid) {
        copyText(merged.ssid, sizeof(merged.ssid), parsed.ssid);
        merged.hasWifiSsid = true;
      }
      if (parsed.hasWifiPass) {
        copyText(merged.pass, sizeof(merged.pass), parsed.pass);
        merged.hasWifiPass = true;
      }
      if (parsed.hasMqttEnabled) {
        merged.mqttEnabled = parsed.mqttEnabled;
        merged.hasMqttEnabled = true;
      }
      if (parsed.mqttHost[0] != '\0') {
        copyText(merged.mqttHost, sizeof(merged.mqttHost), parsed.mqttHost);
      }
      if (parsed.hasMqttPort) {
        merged.mqttPort = parsed.mqttPort;
        merged.hasMqttPort = true;
      }
      if (parsed.mqttUser[0] != '\0') {
        copyText(merged.mqttUser, sizeof(merged.mqttUser), parsed.mqttUser);
      }
      if (parsed.mqttPass[0] != '\0') {
        copyText(merged.mqttPass, sizeof(merged.mqttPass), parsed.mqttPass);
      }
      if (parsed.mqttPrefix[0] != '\0') {
        copyText(merged.mqttPrefix, sizeof(merged.mqttPrefix), parsed.mqttPrefix);
      }
      if (parsed.mqttClientId[0] != '\0') {
        copyText(merged.mqttClientId, sizeof(merged.mqttClientId), parsed.mqttClientId);
      }
      if (parsed.hasEsphomeEnabled) {
        merged.esphomeEnabled = parsed.esphomeEnabled;
        merged.hasEsphomeEnabled = true;
      }
      if (parsed.esphomeNode[0] != '\0') {
        copyText(merged.esphomeNode, sizeof(merged.esphomeNode), parsed.esphomeNode);
      }
    }
  }

  if (!sawAnyFile) {
    bridgeSettings.valid = false;
    return false;
  }

  if (merged.mqttHost[0] != '\0' && !merged.hasMqttEnabled) {
    merged.mqttEnabled = true;
  }
  merged.valid = merged.hasWifiSsid && merged.hasWifiPass && merged.ssid[0] != '\0' && merged.pass[0] != '\0';
  bridgeSettings = merged;

  Serial.print(F("[SD] bridge config merged from SD root, last source "));
  Serial.println(bridgeSettings.sourcePath);
  Serial.print(F("[SD] WiFi SSID: "));
  Serial.println(bridgeSettings.ssid);
  Serial.print(F("[SD] WiFi PASS: "));
  printMaskedPassword(Serial, bridgeSettings.pass);
  Serial.println();
  if (bridgeSettings.mqttEnabled) {
    Serial.print(F("[SD] MQTT host: "));
    Serial.print(bridgeSettings.mqttHost);
    Serial.print(':');
    Serial.println(bridgeSettings.mqttPort);
  } else {
    Serial.println(F("[SD] MQTT host: <disabled>"));
  }
  Serial.print(F("[SD] ESPHome mode: "));
  Serial.println(bridgeSettings.esphomeEnabled ? F("ON") : F("OFF"));
  return bridgeSettings.valid;
}

bool sdStoragePushWifiSettingsToEsp() {
  return sdStoragePushBridgeSettingsToEsp();
}

bool sdStoragePushBridgeSettingsToEsp() {
  if (!bridgeSettings.valid && !sdStorageLoadBridgeSettings()) {
    return false;
  }

  char wifiSetCmd[240];
  size_t pos = 0;
  const char *prefix = "wifi set \"";
  while (*prefix != '\0' && pos + 1 < sizeof(wifiSetCmd)) {
    wifiSetCmd[pos++] = *prefix++;
  }
  appendEscaped(wifiSetCmd, sizeof(wifiSetCmd), pos, bridgeSettings.ssid);
  if (pos + 4 < sizeof(wifiSetCmd)) {
    wifiSetCmd[pos++] = '"';
    wifiSetCmd[pos++] = ' ';
    wifiSetCmd[pos++] = '"';
  }
  appendEscaped(wifiSetCmd, sizeof(wifiSetCmd), pos, bridgeSettings.pass);
  if (pos + 2 < sizeof(wifiSetCmd)) {
    wifiSetCmd[pos++] = '"';
  }
  wifiSetCmd[pos] = '\0';

  char ssidLine[128];
  char passLine[180];
  snprintf(ssidLine, sizeof(ssidLine), "BRIDGE_WIFI_SSID=%s", bridgeSettings.ssid);
  snprintf(passLine, sizeof(passLine), "BRIDGE_WIFI_PASS=%s", bridgeSettings.pass);

  bool sentAny = false;
  sentAny = sendBridgeConfigLine(wifiSetCmd) || sentAny;
  sentAny = sendBridgeConfigLine(ssidLine) || sentAny;
  sentAny = sendBridgeConfigLine(passLine) || sentAny;
  sentAny = sendBridgeConfigLine(bridgeSettings.mqttEnabled ? "BRIDGE_MQTT_ENABLED=1"
                                                             : "BRIDGE_MQTT_ENABLED=0") ||
            sentAny;
  sentAny = sendBridgeConfigLine(bridgeSettings.esphomeEnabled ? "BRIDGE_ESPHOME_MODE=1"
                                                                : "BRIDGE_ESPHOME_MODE=0") ||
            sentAny;
  // Send critical mode flags twice to survive occasional SoftwareSerial drops.
  sentAny = sendBridgeConfigLine(bridgeSettings.mqttEnabled ? "BRIDGE_MQTT_ENABLED=1"
                                                             : "BRIDGE_MQTT_ENABLED=0") ||
            sentAny;
  sentAny = sendBridgeConfigLine(bridgeSettings.esphomeEnabled ? "BRIDGE_ESPHOME_MODE=1"
                                                                : "BRIDGE_ESPHOME_MODE=0") ||
            sentAny;
  if (bridgeSettings.mqttHost[0] != '\0') {
    char mqttHostLine[96];
    snprintf(mqttHostLine, sizeof(mqttHostLine), "BRIDGE_MQTT_HOST=%s", bridgeSettings.mqttHost);
    sentAny = sendBridgeConfigLine(mqttHostLine) || sentAny;
  }
  {
    char mqttPortLine[40];
    snprintf(mqttPortLine, sizeof(mqttPortLine), "BRIDGE_MQTT_PORT=%u",
             (unsigned int)bridgeSettings.mqttPort);
    sentAny = sendBridgeConfigLine(mqttPortLine) || sentAny;
  }
  if (bridgeSettings.mqttUser[0] != '\0') {
    char mqttUserLine[96];
    snprintf(mqttUserLine, sizeof(mqttUserLine), "BRIDGE_MQTT_USER=%s", bridgeSettings.mqttUser);
    sentAny = sendBridgeConfigLine(mqttUserLine) || sentAny;
  }
  if (bridgeSettings.mqttPass[0] != '\0') {
    char mqttPassLine[140];
    snprintf(mqttPassLine, sizeof(mqttPassLine), "BRIDGE_MQTT_PASS=%s", bridgeSettings.mqttPass);
    sentAny = sendBridgeConfigLine(mqttPassLine) || sentAny;
  }
  if (bridgeSettings.mqttPrefix[0] != '\0') {
    char mqttPrefixLine[96];
    snprintf(mqttPrefixLine, sizeof(mqttPrefixLine), "BRIDGE_MQTT_PREFIX=%s", bridgeSettings.mqttPrefix);
    sentAny = sendBridgeConfigLine(mqttPrefixLine) || sentAny;
  }
  if (bridgeSettings.mqttClientId[0] != '\0') {
    char mqttClientIdLine[96];
    snprintf(mqttClientIdLine, sizeof(mqttClientIdLine), "BRIDGE_MQTT_CLIENT_ID=%s",
             bridgeSettings.mqttClientId);
    sentAny = sendBridgeConfigLine(mqttClientIdLine) || sentAny;
  }
  char esphomeNodeLine[96];
  const char *node = bridgeSettings.esphomeNode[0] != '\0' ? bridgeSettings.esphomeNode : "rgbcube";
  snprintf(esphomeNodeLine, sizeof(esphomeNodeLine), "BRIDGE_ESPHOME_NODE=%s", node);
  sentAny = sendBridgeConfigLine(esphomeNodeLine) || sentAny;
  sentAny = sendBridgeConfigLine(esphomeNodeLine) || sentAny;

  // Atomic fallback commands to mitigate occasional UART character loss.
  char mqttSetCmd[280];
  size_t mpos = 0;
  appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, "mqtt set enabled=");
  appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttEnabled ? "1" : "0");
  if (bridgeSettings.mqttHost[0] != '\0') {
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " host=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttHost);
  }
  {
    char portBuf[16];
    snprintf(portBuf, sizeof(portBuf), "%u", (unsigned int)bridgeSettings.mqttPort);
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " port=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, portBuf);
  }
  if (bridgeSettings.mqttUser[0] != '\0') {
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " user=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttUser);
  }
  if (bridgeSettings.mqttPass[0] != '\0') {
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " pass=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttPass);
  }
  if (bridgeSettings.mqttPrefix[0] != '\0') {
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " prefix=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttPrefix);
  }
  if (bridgeSettings.mqttClientId[0] != '\0') {
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, " client=");
    appendRaw(mqttSetCmd, sizeof(mqttSetCmd), mpos, bridgeSettings.mqttClientId);
  }
  mqttSetCmd[mpos] = '\0';
  sentAny = sendBridgeConfigLine(mqttSetCmd) || sentAny;
  sentAny = sendBridgeConfigLine(mqttSetCmd) || sentAny;

  char esphomeSetCmd[120];
  snprintf(esphomeSetCmd, sizeof(esphomeSetCmd), "esphome set mode=%d node=%s",
           bridgeSettings.esphomeEnabled ? 1 : 0, node);
  sentAny = sendBridgeConfigLine(esphomeSetCmd) || sentAny;
  sentAny = sendBridgeConfigLine(esphomeSetCmd) || sentAny;

  sentAny = sendBridgeConfigLine("mqtt apply") || sentAny;
  sentAny = sendBridgeConfigLine("bridge status") || sentAny;
  sentAny = sendBridgeConfigLine("wifi apply") || sentAny;

  if (!sentAny) {
    Serial.println(F("[SD] WiFi config found but ESP bridge is OFF"));
    return false;
  }

  Serial.print(F("[SD] bridge config pushed to ESP from "));
  Serial.println(bridgeSettings.sourcePath);
  return true;
}

bool sdStorageScanAnimations() {
  animCount = 0;
  sdStorageResetAnimationPlayback();
  if (!sdReady) {
    return false;
  }

  FsFile dir = sdFs.open("/animations", O_RDONLY);
  if (!dir || !dir.isDir()) {
    if (dir) {
      dir.close();
    }
    Serial.println(F("[SD] /animations not present (optional)"));
    return false;
  }

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    if (!entry.isFile()) {
      entry.close();
      continue;
    }

    char name[64] = {0};
    entry.getName(name, sizeof(name));
    if (!has3d8Extension(name)) {
      entry.close();
      continue;
    }

    if (animCount < MAX_ANIM_FILES) {
      SdAnimationFileInfo &slot = animFiles[animCount++];
      snprintf(slot.path, sizeof(slot.path), "/animations/%s", name);
      slot.sizeBytes = (uint32_t)entry.fileSize();
      slot.timedFrames = false;
      slot.frameBytes = PLAYBACK_FRAME_HEX_BYTES;
      if (slot.sizeBytes > 0 && (slot.sizeBytes % PLAYBACK_TIMED_FRAME_BYTES) == 0) {
        slot.timedFrames = true;
        slot.frameBytes = PLAYBACK_TIMED_FRAME_BYTES;
      }
      slot.frameCount = slot.sizeBytes / slot.frameBytes;
      slot.frameAligned = (slot.sizeBytes % slot.frameBytes) == 0;
    }
    entry.close();
  }

  dir.close();
  return true;
}

void sdStoragePrintAnimations(Print &out) {
  if (!sdReady) {
    out.println(F("[SD] not mounted"));
    return;
  }

  out.print(F("[SD] animations .3D8: "));
  out.println(animCount);
  for (uint16_t i = 0; i < animCount; i++) {
    const SdAnimationFileInfo &f = animFiles[i];
    out.print(F("  ["));
    out.print(i);
    out.print(F("] "));
    out.print(f.path);
    out.print(F(" bytes="));
    out.print(f.sizeBytes);
    out.print(F(" frames="));
    out.print(f.frameCount);
    out.print(F(" fmt="));
    out.print(f.timedFrames ? F("timed3076") : F("legacy3072"));
    if (!f.frameAligned) {
      out.print(F(" (size not multiple of "));
      out.print((uint32_t)f.frameBytes);
      out.print(F(")"));
    }
    char preview[17] = {0};
    if (readFilePreview(f.path, preview, sizeof(preview))) {
      out.print(F(" preview="));
      out.print(preview);
    }
    out.println();
  }
}

void sdStorageResetAnimationPlayback() {
  playbackAnimIndex = 0;
  playbackFrameInAnim = 0;
  clearPlaybackState(playbackFromState);
  clearPlaybackState(playbackToState);
  playbackPhaseStartMs = 0;
  playbackLastReportedAnim = 0xFFFFu;
  playbackLastReportedFrame = 0xFFFFFFFFul;
  playbackLastReportMs = 0;
  closePlaybackFile();
}

bool sdStorageRenderNextAnimationFrame(uint16_t frameStepMs, bool transitionEnabled,
                                       uint16_t transitionMs, uint16_t timedSpeedPct) {
  if (!sdReady || animCount == 0) {
    return false;
  }

  if (frameStepMs == 0) {
    frameStepMs = 1;
  }

  if (!playbackFromState.valid) {
    if (!readNextPlaybackState(playbackFromState)) {
      return false;
    }
    if (!readNextPlaybackState(playbackToState)) {
      return false;
    }
    playbackPhaseStartMs = millis();
  }

  uint32_t now = millis();
  uint16_t phaseStepMs = playbackFromState.durationMs;
  if (playbackFromState.animIndex < animCount &&
      !animFiles[playbackFromState.animIndex].timedFrames) {
    phaseStepMs = frameStepMs;
  } else {
    if (timedSpeedPct == 0) {
      timedSpeedPct = 1;
    }
    uint32_t scaled = ((uint32_t)phaseStepMs * (uint32_t)timedSpeedPct + 50u) / 100u;
    phaseStepMs = (scaled == 0u) ? 1u : (uint16_t)scaled;
  }
  if (phaseStepMs == 0) {
    phaseStepMs = 1;
  }
  uint32_t elapsed = (uint32_t)(now - playbackPhaseStartMs);
  while (elapsed >= phaseStepMs) {
    playbackFromState = playbackToState;
    if (!readNextPlaybackState(playbackToState)) {
      return false;
    }
    playbackPhaseStartMs += phaseStepMs;
    phaseStepMs = playbackFromState.durationMs;
    if (playbackFromState.animIndex < animCount &&
        !animFiles[playbackFromState.animIndex].timedFrames) {
      phaseStepMs = frameStepMs;
    } else {
      if (timedSpeedPct == 0) {
        timedSpeedPct = 1;
      }
      uint32_t scaled = ((uint32_t)phaseStepMs * (uint32_t)timedSpeedPct + 50u) / 100u;
      phaseStepMs = (scaled == 0u) ? 1u : (uint16_t)scaled;
    }
    if (phaseStepMs == 0) {
      phaseStepMs = 1;
    }
    elapsed = (uint32_t)(now - playbackPhaseStartMs);
  }

  uint16_t effectiveTransitionMs = 0;
  if (transitionEnabled && transitionMs > 0) {
    effectiveTransitionMs = (transitionMs > phaseStepMs) ? phaseStepMs : transitionMs;
  }

  uint8_t blendPercent = 0;
  if (effectiveTransitionMs == 0 || !playbackToState.valid) {
    renderPlaybackState(playbackFromState);
  } else if (elapsed >= effectiveTransitionMs) {
    blendPercent = 100;
    renderPlaybackState(playbackToState);
  } else {
    uint16_t progress256 = (uint16_t)(((uint32_t)elapsed * 256u) / effectiveTransitionMs);
    if (progress256 > 256u) {
      progress256 = 256u;
    }
    blendPercent = (uint8_t)(((uint32_t)elapsed * 100u) / effectiveTransitionMs);
    renderTransitionState(playbackFromState, playbackToState, progress256);
  }

  reportPlaybackStatus(false, blendPercent);
  return true;
}

bool sdStoragePlaybackIsTimed() {
  if (!sdReady || animCount == 0) {
    return false;
  }
  uint16_t idx = playbackAnimIndex;
  if (playbackFromState.valid && playbackFromState.animIndex < animCount) {
    idx = playbackFromState.animIndex;
  }
  if (idx >= animCount) {
    return false;
  }
  return animFiles[idx].timedFrames;
}

bool sdStorageSelectRelativeAnimation(int8_t delta) {
  if (!sdReady || animCount == 0) {
    return false;
  }

  int32_t base = 0;
  if (playbackFromState.valid && playbackFromState.animIndex < animCount) {
    base = (int32_t)playbackFromState.animIndex;
  } else if (playbackAnimIndex < animCount) {
    base = (int32_t)playbackAnimIndex;
  }

  int32_t next = base + (int32_t)delta;
  while (next < 0) {
    next += animCount;
  }
  if (animCount > 0) {
    next %= animCount;
  }

  if (!openPlaybackFile((uint16_t)next)) {
    return false;
  }

  clearPlaybackState(playbackFromState);
  clearPlaybackState(playbackToState);
  if (!readNextPlaybackState(playbackFromState)) {
    return false;
  }
  if (!readNextPlaybackState(playbackToState)) {
    playbackToState = playbackFromState;
    playbackToState.valid = true;
  }

  playbackPhaseStartMs = millis();
  renderPlaybackState(playbackFromState);
  reportPlaybackStatus(true, 0);
  return true;
}

void sdStorageSetPlaybackLogging(bool enabled) {
  playbackLoggingEnabled = enabled;
  playbackLastReportMs = 0;
  playbackLastReportedAnim = 0xFFFFu;
  playbackLastReportedFrame = 0xFFFFFFFFul;
}

bool sdStorageGetPlaybackLogging() { return playbackLoggingEnabled; }

uint16_t sdStorageGetAnimationCount() { return animCount; }

const SdAnimationFileInfo *sdStorageGetAnimationInfo(uint16_t index) {
  if (index >= animCount) {
    return nullptr;
  }
  return &animFiles[index];
}
