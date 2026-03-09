# STM32F401 Port

> Note: AI was used for the majority of the code in this project.

This folder contains an STM32-compatible port of the RGB cube firmware.

## Target

- MCU: STM32F401CCU6 (84 MHz)
- Framework: Arduino (STM32duino)
- PlatformIO env: `stm32f401cc`

## Build

```bash
pio run -e stm32f401cc
```

## Upload

Default upload protocol is ST-Link.

```bash
pio run -e stm32f401cc -t upload
```

## Pin mapping (STM32 pin names)

- `A0    -> PA0`
- `A1    -> PA1`
- `A2    -> PA2`
- `DAT_A -> PA3`
- `DAT_B -> PA4`
- `OE    -> PA5`
- `CLK   -> PA6`
- `ST/LE -> PA7`

## Wi-Fi/MQTT via SD card (ESP8266 bridge)

The STM32 firmware can load bridge settings from the SD card root and forward
them over UART to the connected ESP8266 bridge firmware.

### SD pins for card reader

- `PB12 -> SD CS` (table label may show `CD`)
- `PB13 -> SD CLK`
- `PB14 -> SD DAT0` (MISO)
- `PB15 -> SD CMD` (MOSI)

### Wi-Fi config files (SD root)

The firmware checks these file names in order and merges recognized settings
from all existing files (later files override earlier ones):

- `/wifi.txt`
- `/wifi.cfg`
- `/wifi.ini`
- `/bridge_wifi.txt`
- `/bridge_mqtt.txt`
- `/bridge_config.txt`
- `/bridge_secrets.h`

### Supported file formats

`key=value` style:

```ini
ssid=YOUR_WIFI_SSID
password=YOUR_WIFI_PASSWORD
```

`bridge_*` style:

```ini
bridge_wifi_ssid=YOUR_WIFI_SSID
bridge_wifi_pass=YOUR_WIFI_PASSWORD
mqtt_host=192.168.1.10
mqtt_port=1883
mqtt_user=your_user
mqtt_pass=your_pass
mqtt_prefix=rgbcube
mqtt_client_id=rgbcube-bridge
esphome_mode=1
esphome_node=rgbcube
```

`#define` style (compatible with esp-bridge secrets header):

```c
#define BRIDGE_WIFI_SSID "YOUR_WIFI_SSID"
#define BRIDGE_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define BRIDGE_MQTT_ENABLED 1
#define BRIDGE_MQTT_HOST "192.168.1.10"
#define BRIDGE_MQTT_PORT 1883
#define BRIDGE_MQTT_USER "your_user"
#define BRIDGE_MQTT_PASS "your_pass"
#define BRIDGE_MQTT_PREFIX "rgbcube"
#define BRIDGE_MQTT_CLIENT_ID "rgbcube-bridge"
#define BRIDGE_ESPHOME_MODE 1
#define BRIDGE_ESPHOME_NODE "rgbcube"
```

Fallback format:

- first non-empty line = SSID
- second non-empty line = password

### Runtime behavior

- On boot, STM32 mounts SD, tries to load Wi-Fi settings, then forwards them to ESP UART.
- Manual reload/push from CLI (connect USB to your PC and open a terminal on the port that appears):

```text
sd wifi
sd mqtt
sd bridge
```

### SD animation playback controls

- `m 9` enables SD animation mode (if `.3D8` files are available in `/animations` on the SD card).
- `sf <20..1000>` sets the base frame time in ms (used by legacy `.3D8` files without embedded timing).
- `ssp <25..400>` sets timed-file speed in percent (`100` = original timing from file, lower = faster).
- `st <0|1>` toggles smooth frame transitions off/on.
- `stt <0..1000>` sets transition time in ms (`0` = hard cut, effective value is capped by `sf`).
- `dp <0|1|t>` controls display output (`0` off/black, `1` on, `t` toggle) without changing the current mode.
- Playback monitor logs (`[SD-PLAY] ...`) are OFF by default. Use `sd log 1` to enable, `sd log 0` to disable.
- In mode `9`, IR remote `+/-` changes:
  - `sf` for legacy `.3D8` files
  - `ssp` for timed `.3D8` files
- In mode `9`, IR remote playback controls:
  - `PLAY/PAUSE` toggles SD animation playback (pause keeps current frame displayed)
  - `NEXT` jumps to next animation file
  - `PREV` jumps to previous animation file
- Entering SD mode (`m 9`) defaults to play state.
- Refresh scan stays at `120 Hz`; transition blending is handled in the SD playback layer.

### `.3D8` file format

The firmware supports two `.3D8` layouts:

- `legacy3072`:
  - one frame = `3072` ASCII hex chars
  - payload = `8*8*8` voxels, each voxel as `RRGGBB` (6 hex chars)
  - no embedded timing; frame duration comes from `sf`
- `timed3076` (proprietary format):
  - one frame = `3076` ASCII hex chars
  - first `3072` chars = voxel payload (`RRGGBB` per voxel)
  - next `4` chars = frame duration as big-endian hex `uint16` in milliseconds (`TTTT`)
  - example: `0014` = `20 ms`, `03FC` = `1020 ms`, `0032` = `50 ms`

Your `test3d8/test.3D8` matches `timed3076`:
- total length `30760` chars = `10` frames × `3076`
- per-frame durations: `20, 20, 20, 1020, 20, 20, 20, 20, 20, 50 ms`

### Wi-Fi live stream (`.3D8`-compatible) via ESP bridge

> Attention! That is very very slow and doesn't work very good for animations. More work is needed here!

The ESP bridge forwards TCP bytes to STM32 UART. The STM32 firmware now supports
chunked live `.3D8` streaming commands:

- `rx on` enable live stream mode (`m 10`)
- `rx u <3D8-hex-chunk>` append hex data chunk (`0-9`, `a-f`, `A-F`)
- `rx fs` frame-sync start (clear partial frame)
- `rx fe` frame-sync end (drop frame if incomplete)
- `rx p` print stream status
- `rx log 1` / `rx log 0` enable/disable stream debug logs
- `rx clr` clear partial/full stream buffer
- `rx off` disable stream mode
- robust mode helpers:
  - `rb` reset robust frame buffer
  - `rk <idxHex> <payloadHex> <crcHex>` store one robust chunk (`idx 00..2F` => `64` hex chars, optional `idx 30` => `4` hex chars `TTTT`)
  - `rf` finalize robust frame (requires all `00..2F`, `30` optional)

Frame format compatibility:

- stream payload frame = `3072` hex chars (`8*8*8 voxels * 6 chars`)
- timed stream payload frame = `3076` hex chars (`3072 + TTTT`, big-endian ms duration)
- `TTTT` duration is parsed on STM32 RX path and used for frame phase timing + blend pacing
- separators like spaces/commas/newlines are ignored in chunk payloads

Practical chunk size:

- keep each command line below the CLI buffer limit (`256` chars)
- recommended payload for `rx u ...` is up to about `220` hex chars per line
- at `57600` UART baud, practical live stream rate is about `1..1.5 FPS`

### Python sender tool

You can stream a `.3D8` file over Wi-Fi (TCP -> ESP bridge -> STM32 UART) with:

```bash
python3 tools/stream_3d8_to_bridge.py /path/to/file.3D8 --host rgbcube --port 7777
```

Useful options:

- `--fps 1.2` target frame rate for non-timed `.3D8` files
- `--loops 0` loop forever
- `--chunk 220` hex chars per `rx u ...` command
- `--protocol robust` use robust chunk+CRC mode (default)
- `--frame-sync` / `--no-frame-sync` wrap each frame with `rx fs` / `rx fe`
- `--uart-baud 57600` and `--uart-safety 1.15` tune sender pacing
- `--rx-log 1` enable STM stream logs
- `--status-after` request final `rx p` status from STM

Example (1.2 FPS, infinite loop):

```bash
python3 tools/stream_3d8_to_bridge.py ./test3d8/totem0.3D8 --host rgbcube --fps 1.2 --loops 0
```

## Notes

- Refresh uses `HardwareTimer(TIM2)` at `FRAME_HZ * 8` layer rate.
- OE is active LOW.
- DAT_A and DAT_B are shifted synchronously with one shared CLK.
- Keep logic levels and electrical limits in mind - the cube board is 5V-only.
