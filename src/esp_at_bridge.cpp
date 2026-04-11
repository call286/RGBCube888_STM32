#include "esp_at_bridge.h"

#include <Arduino.h>
#include <string.h>

#include "cube_config.h"
#include "serial_cli.h"
#include "stream_3d8.h"
#include "watchdog.h"

namespace {

// Raw UART bridge to external ESP firmware.
HardwareSerial espUart(CubeConfig::ESP_UART_RX, CubeConfig::ESP_UART_TX);
bool bridgeEnabled = true;

static const uint8_t BIN_MAGIC0 = 0xA5;
static const uint8_t BIN_MAGIC1 = 0x5A;
static const uint8_t BIN_TYPE_FRAME_BEGIN = 0x10;
static const uint8_t BIN_TYPE_FRAME_CHUNK = 0x11;
static const uint8_t BIN_TYPE_FRAME_END = 0x12;
static const uint8_t BIN_TYPE_CTRL = 0x20;
static const uint16_t BIN_PAYLOAD_MAX = 224;
static const uint16_t RGB_FRAME_BYTES = 8u * 8u * 8u * 3u; // 1536

enum BinRxState : uint8_t {
  BIN_SYNC0 = 0,
  BIN_SYNC1,
  BIN_TYPE,
  BIN_LEN0,
  BIN_LEN1,
  BIN_PAYLOAD,
  BIN_CRC0,
  BIN_CRC1
};

BinRxState binState = BIN_SYNC0;
uint8_t binType = 0;
uint16_t binLen = 0;
uint16_t binPos = 0;
uint8_t binPayload[BIN_PAYLOAD_MAX];
uint16_t binCrcCalc = 0xFFFFu;
uint16_t binCrcRx = 0;

bool frameActive = false;
uint16_t frameId = 0;
uint16_t frameDurationMs = 20;
uint16_t frameBytesSeen = 0;
uint8_t frameRgb[RGB_FRAME_BYTES];
uint8_t frameSeen[RGB_FRAME_BYTES];

void sendBinStatus(const char *msg) {
  if (msg == nullptr) {
    return;
  }
  espUart.print(msg);
  espUart.print('\n');
}

static inline uint16_t readLe16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint16_t crc16CcittUpdate(uint16_t crc, uint8_t data) {
  crc ^= (uint16_t)data << 8;
  for (uint8_t i = 0; i < 8; i++) {
    if (crc & 0x8000u) {
      crc = (uint16_t)((crc << 1) ^ 0x1021u);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

uint16_t crc16Ccitt(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFFu;
  for (uint16_t i = 0; i < len; i++) {
    crc = crc16CcittUpdate(crc, buf[i]);
  }
  return crc;
}

void beginFrameAssembly(uint16_t id, uint16_t durationMs) {
  frameActive = true;
  frameId = id;
  frameDurationMs = (durationMs == 0u) ? 1u : durationMs;
  frameBytesSeen = 0;
  memset(frameSeen, 0, sizeof(frameSeen));
}

void applyCtrlPacket(const uint8_t *payload, uint16_t len) {
  if (len < 2) {
    return;
  }
  uint8_t cmd = payload[0];
  uint8_t arg = payload[1];
  if (cmd == 1) { // set render mode
    if (arg <= (uint8_t)MODE_RX_STREAM) {
      serialCliSetRenderMode((RenderMode)arg);
      char msg[48];
      (void)snprintf(msg, sizeof(msg), "[BIN] ctrl mode=%u", (unsigned)arg);
      sendBinStatus(msg);
    }
    return;
  }
  if (cmd == 2) { // display on/off
    serialCliSetDisplayEnabled(arg != 0u);
    sendBinStatus(arg ? "[BIN] ctrl display=1" : "[BIN] ctrl display=0");
    return;
  }
  if (cmd == 3) { // stream on/off
    bool on = (arg != 0u);
    stream3d8SetEnabled(on);
    sendBinStatus(on ? "[BIN] ctrl stream=1" : "[BIN] ctrl stream=0");
  }
}

void handleBinaryPacket(uint8_t type, const uint8_t *payload, uint16_t len) {
  if (type == BIN_TYPE_FRAME_BEGIN) {
    if (len != 6) {
      return;
    }
    uint16_t id = readLe16(&payload[0]);
    uint16_t duration = readLe16(&payload[2]);
    uint16_t total = readLe16(&payload[4]);
    if (total != RGB_FRAME_BYTES) {
      frameActive = false;
      sendBinStatus("[BIN] nack reason=begin_size");
      return;
    }
    beginFrameAssembly(id, duration);
    return;
  }

  if (type == BIN_TYPE_FRAME_CHUNK) {
    if (len < 6) {
      sendBinStatus("[BIN] nack reason=chunk_len");
      return;
    }
    uint16_t id = readLe16(&payload[0]);
    uint16_t off = readLe16(&payload[2]);
    uint16_t clen = readLe16(&payload[4]);
    // Recovery: if FRAME_BEGIN was dropped, start assembly from first seen chunk.
    if (!frameActive) {
      beginFrameAssembly(id, frameDurationMs);
    }
    if (id != frameId || (uint16_t)(6u + clen) != len) {
      sendBinStatus("[BIN] nack reason=chunk_header");
      return;
    }
    if ((uint32_t)off + (uint32_t)clen > (uint32_t)RGB_FRAME_BYTES) {
      sendBinStatus("[BIN] nack reason=chunk_range");
      return;
    }
    const uint8_t *src = &payload[6];
    memcpy(&frameRgb[off], src, clen);
    for (uint16_t i = 0; i < clen; i++) {
      uint16_t idx = (uint16_t)(off + i);
      if (frameSeen[idx] == 0u) {
        frameSeen[idx] = 1u;
        frameBytesSeen++;
      }
    }
    return;
  }

  if (type == BIN_TYPE_FRAME_END) {
    if (len != 4 || !frameActive) {
      sendBinStatus("[BIN] nack reason=end_state");
      return;
    }
    uint16_t id = readLe16(&payload[0]);
    uint16_t sentCrc = readLe16(&payload[2]);
    if (id != frameId || frameBytesSeen != RGB_FRAME_BYTES) {
      frameActive = false;
      char msg[72];
      (void)snprintf(msg, sizeof(msg), "[BIN] nack id=%u reason=missing seen=%u", (unsigned)id,
                     (unsigned)frameBytesSeen);
      sendBinStatus(msg);
      return;
    }
    uint16_t calc = crc16Ccitt(frameRgb, RGB_FRAME_BYTES);
    if (calc != sentCrc) {
      frameActive = false;
      char msg[80];
      (void)snprintf(msg, sizeof(msg), "[BIN] nack id=%u reason=crc calc=%u sent=%u", (unsigned)id,
                     (unsigned)calc, (unsigned)sentCrc);
      sendBinStatus(msg);
      return;
    }

    stream3d8SetEnabled(true);
    serialCliSetRenderMode(MODE_RX_STREAM);
    bool ok = stream3d8CommitRgbFrame(frameRgb, RGB_FRAME_BYTES, frameDurationMs);
    if (ok) {
      char msg[64];
      (void)snprintf(msg, sizeof(msg), "[BIN] ack id=%u frames=%lu", (unsigned)id,
                     (unsigned long)stream3d8GetFrameCount());
      sendBinStatus(msg);
    } else {
      char msg[56];
      (void)snprintf(msg, sizeof(msg), "[BIN] nack id=%u reason=commit", (unsigned)id);
      sendBinStatus(msg);
    }
    frameActive = false;
    return;
  }

  if (type == BIN_TYPE_CTRL) {
    applyCtrlPacket(payload, len);
  }
}

bool feedBinaryByte(uint8_t b) {
  switch (binState) {
  case BIN_SYNC0:
    if (b == BIN_MAGIC0) {
      binState = BIN_SYNC1;
      return true;
    }
    return false;

  case BIN_SYNC1:
    if (b == BIN_MAGIC1) {
      binState = BIN_TYPE;
      binCrcCalc = 0xFFFFu;
      return true;
    }
    binState = BIN_SYNC0;
    return false;

  case BIN_TYPE:
    binType = b;
    binCrcCalc = crc16CcittUpdate(binCrcCalc, b);
    binState = BIN_LEN0;
    return true;

  case BIN_LEN0:
    binLen = b;
    binCrcCalc = crc16CcittUpdate(binCrcCalc, b);
    binState = BIN_LEN1;
    return true;

  case BIN_LEN1:
    binLen |= (uint16_t)b << 8;
    binCrcCalc = crc16CcittUpdate(binCrcCalc, b);
    if (binLen > BIN_PAYLOAD_MAX) {
      binState = BIN_SYNC0;
      return true;
    }
    binPos = 0;
    binState = (binLen == 0u) ? BIN_CRC0 : BIN_PAYLOAD;
    return true;

  case BIN_PAYLOAD:
    binPayload[binPos++] = b;
    binCrcCalc = crc16CcittUpdate(binCrcCalc, b);
    if (binPos >= binLen) {
      binState = BIN_CRC0;
    }
    return true;

  case BIN_CRC0:
    binCrcRx = b;
    binState = BIN_CRC1;
    return true;

  case BIN_CRC1:
    binCrcRx |= (uint16_t)b << 8;
    if (binCrcRx == binCrcCalc) {
      handleBinaryPacket(binType, binPayload, binLen);
    }
    binState = BIN_SYNC0;
    return true;
  }

  binState = BIN_SYNC0;
  return false;
}

bool isCliByte(uint8_t b) {
  if (b == '\r' || b == '\n' || b == '\t') {
    return true;
  }
  return (b >= 32u && b <= 126u);
}

void applyBridgeState(bool enabled) {
  bridgeEnabled = enabled;
  if (bridgeEnabled) {
    espUart.begin(CubeConfig::ESP_UART_BAUD);
    Serial.print(F("[ESP-UART] bridge enabled, baud="));
    Serial.println(CubeConfig::ESP_UART_BAUD);
  } else {
    espUart.end();
    pinMode(CubeConfig::ESP_UART_RX, INPUT);
    pinMode(CubeConfig::ESP_UART_TX, INPUT);
    Serial.println(F("[ESP-UART] bridge disabled (pins high-Z)"));
  }
}

} // namespace

void espAtBridgeInit() { applyBridgeState(true); }

void espAtBridgeHandle() {
  if (!bridgeEnabled) {
    return;
  }
  uint16_t processed = 0;
  while (espUart.available() > 0) {
    uint8_t b = (uint8_t)espUart.read();
    if (feedBinaryByte(b)) {
      if (((++processed) & 0x1Fu) == 0u) {
        watchdogKick();
      }
      continue;
    }
    // Only feed clean ASCII control bytes to CLI fallback.
    // This prevents binary payload bytes from overflowing the text command line buffer.
    if (isCliByte(b)) {
      serialCliProcessChar((char)b, espUart, false);
    }
    if (((++processed) & 0x1Fu) == 0u) {
      watchdogKick();
    }
  }
}

void espAtBridgeSetEnabled(bool enabled) { applyBridgeState(enabled); }

bool espAtBridgeIsEnabled() { return bridgeEnabled; }

bool espAtBridgeSendLine(const char *line) {
  if (!bridgeEnabled || line == nullptr) {
    return false;
  }
  espUart.print(line);
  espUart.print('\n');
  return true;
}
