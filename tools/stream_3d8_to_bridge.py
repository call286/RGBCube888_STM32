#!/usr/bin/env python3
"""Stream .3D8 frame data to the ESP bridge TCP socket.

The STM32 expects chunked lines like:
  rx u <hex-chunk>

One full .3D8 frame is 3072 hex chars; timed frames are 3076 (+TTTT ms).
"""

from __future__ import annotations

import argparse
import re
import select
import socket
import string
import sys
import time
from pathlib import Path

FRAME_HEX_CHARS = 8 * 8 * 8 * 6
TIMED_FRAME_HEX_CHARS = FRAME_HEX_CHARS + 4
DEFAULT_HOST = "<IPOrHost>"
DEFAULT_PORT = 7777
DEFAULT_CHUNK = 220
MAX_CHUNK = 240
DEFAULT_FPS = 1.2
DEFAULT_UART_BAUD = 115200
DEFAULT_UART_SAFETY = 1.15
HEX_CHARS = set(string.hexdigits)
ROBUST_CHUNK_HEX = 64
BIN_MAGIC = b"\xA5\x5A"
BIN_TYPE_FRAME_BEGIN = 0x10
BIN_TYPE_FRAME_CHUNK = 0x11
BIN_TYPE_FRAME_END = 0x12
BIN_TYPE_CTRL = 0x20
BIN_CTRL_SET_MODE = 1
BIN_CTRL_SET_DISPLAY = 2
BIN_CTRL_SET_STREAM = 3
BIN_RGB_FRAME_BYTES = 8 * 8 * 8 * 3
DEFAULT_BIN_CHUNK_BYTES = 192
DEFAULT_BIN_ACK_TIMEOUT = 0.20
DEFAULT_BIN_RETRIES = 2
DEFAULT_BIN_PACKET_REDUNDANCY = 2
DEFAULT_BIN_CONTROL_REDUNDANCY = 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stream .3D8 data to STM32 over the ESP bridge TCP socket."
    )
    parser.add_argument("file", type=Path, help=".3D8 file path")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP bridge host (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"ESP bridge TCP port (default: {DEFAULT_PORT})")
    parser.add_argument(
        "--fps",
        type=float,
        default=DEFAULT_FPS,
        help=f"target stream FPS (0 = no pacing, default: {DEFAULT_FPS})",
    )
    parser.add_argument(
        "--timing-mode",
        choices=["auto", "file", "fps"],
        default="auto",
        help="frame timing source: auto (default), file (always use TTTT), fps (always use --fps)",
    )
    parser.add_argument(
        "--file-timing-scale",
        type=float,
        default=1.0,
        help="scale factor for timed .3D8 frame delays (e.g. 10.0 = 10x faster, default: 1.0)",
    )
    parser.add_argument(
        "--chunk",
        type=int,
        default=DEFAULT_CHUNK,
        help=f"hex chars per 'rx u' line (default: {DEFAULT_CHUNK}, max: {MAX_CHUNK})",
    )
    parser.add_argument(
        "--loops",
        type=int,
        default=1,
        help="how many times to loop over file frames (0 = infinite, default: 1)",
    )
    parser.add_argument(
        "--frame-sync",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="send 'rx fs' before and 'rx fe' after each frame (default: on)",
    )
    parser.add_argument(
        "--protocol",
        choices=["robust", "legacy", "binary"],
        default="robust",
        help="stream protocol (default: robust)",
    )
    parser.add_argument(
        "--profile",
        choices=["reliable", "fast"],
        help="binary profile preset (reliable or fast)",
    )
    parser.add_argument(
        "--bin-chunk-bytes",
        type=int,
        default=DEFAULT_BIN_CHUNK_BYTES,
        help=f"binary frame chunk payload bytes (default: {DEFAULT_BIN_CHUNK_BYTES})",
    )
    parser.add_argument(
        "--net-mode",
        type=int,
        help="binary control: set STM render mode (0..10) before streaming",
    )
    parser.add_argument(
        "--net-display",
        choices=["0", "1"],
        help="binary control: set display OFF/ON before streaming",
    )
    parser.add_argument(
        "--control-only",
        action="store_true",
        help="send only requested binary control packets and exit",
    )
    parser.add_argument(
        "--bin-ack-timeout",
        type=float,
        default=DEFAULT_BIN_ACK_TIMEOUT,
        help=f"wait timeout in seconds for binary ACK/NACK after frame end (default: {DEFAULT_BIN_ACK_TIMEOUT})",
    )
    parser.add_argument(
        "--bin-retries",
        type=int,
        default=DEFAULT_BIN_RETRIES,
        help=f"binary frame resend attempts after timeout/NACK (default: {DEFAULT_BIN_RETRIES})",
    )
    parser.add_argument(
        "--bin-packet-redundancy",
        type=int,
        default=DEFAULT_BIN_PACKET_REDUNDANCY,
        help=f"send begin/chunk/end packets this many times per attempt (default: {DEFAULT_BIN_PACKET_REDUNDANCY})",
    )
    parser.add_argument(
        "--bin-control-redundancy",
        type=int,
        default=DEFAULT_BIN_CONTROL_REDUNDANCY,
        help=f"send each binary control packet this many times (default: {DEFAULT_BIN_CONTROL_REDUNDANCY})",
    )
    parser.add_argument(
        "--bin-debug",
        action="store_true",
        help="print binary ACK/NACK/control lines from bridge/STM",
    )
    parser.add_argument(
        "--payload-mode",
        choices=["raw", "rxu"],
        default="raw",
        help="chunk line format: raw hex line or 'rx u <hex>' (default: raw)",
    )
    parser.add_argument(
        "--robust-passes",
        type=int,
        default=1,
        help="number of full chunk passes per frame in robust protocol (default: 1)",
    )
    parser.add_argument(
        "--uart-baud",
        type=int,
        default=DEFAULT_UART_BAUD,
        help=f"UART baud between ESP and STM (default: {DEFAULT_UART_BAUD})",
    )
    parser.add_argument(
        "--uart-safety",
        type=float,
        default=DEFAULT_UART_SAFETY,
        help=f"UART throttle factor (>1 = safer, default: {DEFAULT_UART_SAFETY})",
    )
    parser.add_argument(
        "--strict-length",
        action="store_true",
        help="fail if input length is not a full 3072/3076 frame multiple",
    )
    parser.add_argument(
        "--no-clear",
        action="store_true",
        help="do not send 'rx clr' before streaming",
    )
    parser.add_argument(
        "--no-rx-on",
        action="store_true",
        help="do not send 'rx on' before streaming",
    )
    parser.add_argument(
        "--rx-log",
        choices=["0", "1"],
        help="set STM rx logging state before streaming (0 or 1)",
    )
    parser.add_argument(
        "--status-after",
        action="store_true",
        help="send 'rx p' at the end",
    )
    parser.add_argument(
        "--print-every",
        type=int,
        default=1,
        help="print progress every N sent frames (default: 1)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print plan without opening a socket",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=5.0,
        help="socket connect timeout in seconds (default: 5.0)",
    )
    return parser.parse_args()


def option_provided(argv: list[str], opt: str) -> bool:
    prefix = opt + "="
    for a in argv:
        if a == opt or a.startswith(prefix):
            return True
    return False


def extract_hex(text: str) -> str:
    return "".join(c for c in text if c in HEX_CHARS)


def load_frames(path: Path, strict_length: bool) -> tuple[list[str], list[float] | None, int, int]:
    if not path.is_file():
        raise FileNotFoundError(f"input file not found: {path}")

    raw = path.read_text(encoding="ascii", errors="ignore")
    hex_data = extract_hex(raw)
    total_hex_chars = len(hex_data)
    timed_frames = total_hex_chars // TIMED_FRAME_HEX_CHARS
    timed_trailing = total_hex_chars % TIMED_FRAME_HEX_CHARS
    durations_s: list[float] | None = None

    if timed_frames > 0 and timed_trailing == 0:
        frames = []
        durations_s = []
        for i in range(timed_frames):
            off = i * TIMED_FRAME_HEX_CHARS
            frame_hex = hex_data[off : off + FRAME_HEX_CHARS]
            frame_ms_hex = hex_data[off + FRAME_HEX_CHARS : off + TIMED_FRAME_HEX_CHARS]
            frame_ms = int(frame_ms_hex, 16)
            if frame_ms <= 0:
                frame_ms = 1
            frames.append(frame_hex + frame_ms_hex)
            durations_s.append(frame_ms / 1000.0)
        return frames, durations_s, 0, total_hex_chars

    full_frames = total_hex_chars // FRAME_HEX_CHARS
    trailing = total_hex_chars % FRAME_HEX_CHARS

    if full_frames == 0:
        raise ValueError("no complete .3D8 frame found (need at least 3072 hex chars)")

    if strict_length and trailing != 0:
        raise ValueError(
            f"input has {trailing} trailing hex chars (not a multiple of {FRAME_HEX_CHARS})"
        )

    if trailing != 0:
        print(
            f"[warn] dropping {trailing} trailing hex chars (not a full frame)",
            file=sys.stderr,
        )

    trimmed = hex_data[: full_frames * FRAME_HEX_CHARS]
    frames = [
        trimmed[i : i + FRAME_HEX_CHARS] for i in range(0, len(trimmed), FRAME_HEX_CHARS)
    ]
    return frames, durations_s, trailing, total_hex_chars


def send_line(sock: socket.socket, line: str) -> None:
    sock.sendall(line.encode("ascii") + b"\n")


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def send_bin_packet(sock: socket.socket, pkt_type: int, payload: bytes) -> int:
    hdr = bytes([pkt_type]) + len(payload).to_bytes(2, "little")
    crc = crc16_ccitt(hdr + payload).to_bytes(2, "little")
    pkt = BIN_MAGIC + hdr + payload + crc
    sock.sendall(pkt)
    return len(pkt)


def frame_hex_to_rgb_bytes(frame_hex: str) -> bytes:
    pix_hex = frame_hex[:FRAME_HEX_CHARS]
    if len(pix_hex) != FRAME_HEX_CHARS:
        raise ValueError("frame must contain 3072 hex chars of RGB voxel data")
    return bytes.fromhex(pix_hex)


def drain_bridge_logs(
    sock: socket.socket,
    rx_buf: bytearray,
    prefix: str = "[rx]",
    max_lines: int = 50,
    print_lines: bool = True,
) -> None:
    lines = 0
    while lines < max_lines:
        try:
            ready, _, _ = select.select([sock], [], [], 0.0)
        except (ValueError, OSError):
            return
        if not ready:
            return
        try:
            chunk = sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            return
        if not chunk:
            return
        rx_buf.extend(chunk)
        while b"\n" in rx_buf and lines < max_lines:
            line, _, rest = rx_buf.partition(b"\n")
            del rx_buf[: len(line) + 1]
            decoded = line.decode("utf-8", errors="replace").strip()
            if decoded and print_lines:
                print(f"{prefix} {decoded}")
            lines += 1
        if b"\n" not in rx_buf:
            return


def line_uart_time_s(line_bytes: int, uart_baud: int, safety: float) -> float:
    if uart_baud <= 0:
        return 0.0
    # 8N1 -> ~10 bits per UART byte.
    return (line_bytes * 10.0 / float(uart_baud)) * max(1.0, safety)


ACK_RE = re.compile(r"\[BIN\]\s+ack\s+id=(\d+)\b")
NACK_RE = re.compile(r"\[BIN\]\s+nack(?:\s+id=(\d+))?\b")


def wait_for_bin_ack(
    sock: socket.socket,
    rx_buf: bytearray,
    frame_id: int,
    timeout_s: float,
    print_lines: bool,
) -> tuple[str, str]:
    deadline = time.monotonic() + max(0.0, timeout_s)
    while time.monotonic() < deadline:
        rem = max(0.0, deadline - time.monotonic())
        try:
            ready, _, _ = select.select([sock], [], [], rem)
        except (ValueError, OSError):
            return ("timeout", "select failed")
        if not ready:
            continue
        try:
            chunk = sock.recv(4096)
        except BlockingIOError:
            continue
        except OSError:
            return ("timeout", "recv failed")
        if not chunk:
            return ("timeout", "socket closed")
        rx_buf.extend(chunk)
        while b"\n" in rx_buf:
            line, _, _ = rx_buf.partition(b"\n")
            del rx_buf[: len(line) + 1]
            decoded = line.decode("utf-8", errors="replace").strip()
            if not decoded:
                continue
            if print_lines:
                print(f"[rx] {decoded}")
            m_ack = ACK_RE.search(decoded)
            if m_ack:
                ack_id = int(m_ack.group(1))
                if ack_id == frame_id:
                    return ("ack", decoded)
            m_nack = NACK_RE.search(decoded)
            if m_nack:
                grp = m_nack.group(1)
                if grp is None or int(grp) == frame_id:
                    return ("nack", decoded)
    return ("timeout", f"id={frame_id}")


def xor_payload_crc(payload_hex: str) -> int:
    if len(payload_hex) % 2 != 0:
        raise ValueError("payload hex length must be even")
    crc = 0
    for i in range(0, len(payload_hex), 2):
        crc ^= int(payload_hex[i : i + 2], 16)
    return crc & 0xFF


def stream_frames(
    sock: socket.socket,
    frames: list[str],
    fps: float,
    chunk: int,
    loops: int,
    print_every: int,
    frame_sync: bool,
    uart_baud: int,
    uart_safety: float,
    rx_buf: bytearray,
    payload_mode: str,
    protocol: str,
    robust_passes: int,
    frame_durations_s: list[float] | None,
    bin_chunk_bytes: int,
    bin_ack_timeout: float,
    bin_retries: int,
    bin_packet_redundancy: int,
    bin_debug: bool,
) -> int:
    sent = 0
    loop_idx = 0
    infinite = loops == 0

    if payload_mode == "raw":
        max_payload_line = chunk + 1
    else:
        max_payload_line = len("rx u ") + chunk + 1
    inter_line_sleep = line_uart_time_s(max_payload_line, uart_baud, uart_safety)

    while infinite or loop_idx < loops:
        loop_idx += 1
        for frame_idx, frame in enumerate(frames):
            frame_start = time.monotonic()
            if protocol == "robust":
                send_line(sock, "rb")
                drain_bridge_logs(sock, rx_buf)
                if inter_line_sleep > 0:
                    time.sleep(inter_line_sleep)
                    drain_bridge_logs(sock, rx_buf)

                robust_chunk_size = ROBUST_CHUNK_HEX
                frame_hex_len = len(frame)
                chunk_count = frame_hex_len // robust_chunk_size
                chunk_tail = frame_hex_len % robust_chunk_size
                for _pass in range(max(1, robust_passes)):
                    for idx in range(chunk_count):
                        payload = frame[idx * robust_chunk_size : (idx + 1) * robust_chunk_size]
                        crc = xor_payload_crc(payload)
                        line = f"rk {idx:02X} {payload} {crc:02X}"
                        send_line(sock, line)
                        drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)
                        if inter_line_sleep > 0:
                            time.sleep(line_uart_time_s(len(line) + 1, uart_baud, uart_safety))
                            drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)
                    if chunk_tail > 0:
                        idx = chunk_count
                        payload = frame[chunk_count * robust_chunk_size :]
                        crc = xor_payload_crc(payload)
                        line = f"rk {idx:02X} {payload} {crc:02X}"
                        send_line(sock, line)
                        drain_bridge_logs(sock, rx_buf)
                        if inter_line_sleep > 0:
                            time.sleep(line_uart_time_s(len(line) + 1, uart_baud, uart_safety))
                            drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)

                send_line(sock, "rf")
                drain_bridge_logs(sock, rx_buf)
                if inter_line_sleep > 0:
                    time.sleep(inter_line_sleep)
                    drain_bridge_logs(sock, rx_buf)
            elif protocol == "binary":
                rgb = frame_hex_to_rgb_bytes(frame)
                frame_id = sent & 0xFFFF
                if frame_durations_s is not None and len(frame_durations_s) == len(frames):
                    duration_ms = max(1, int(round(frame_durations_s[frame_idx] * 1000.0)))
                elif fps > 0:
                    duration_ms = max(1, int(round((1.0 / fps) * 1000.0)))
                else:
                    duration_ms = 20

                attempt = 0
                delivered = False
                while attempt <= max(0, bin_retries):
                    begin_payload = (
                        frame_id.to_bytes(2, "little")
                        + duration_ms.to_bytes(2, "little")
                        + BIN_RGB_FRAME_BYTES.to_bytes(2, "little")
                    )
                    for _ in range(max(1, bin_packet_redundancy)):
                        begin_bytes = send_bin_packet(sock, BIN_TYPE_FRAME_BEGIN, begin_payload)
                        drain_bridge_logs(sock, rx_buf)
                        if inter_line_sleep > 0:
                            time.sleep(line_uart_time_s(begin_bytes, uart_baud, uart_safety))
                            drain_bridge_logs(sock, rx_buf)

                    for off in range(0, BIN_RGB_FRAME_BYTES, bin_chunk_bytes):
                        chunk = rgb[off : off + bin_chunk_bytes]
                        payload = (
                            frame_id.to_bytes(2, "little")
                            + off.to_bytes(2, "little")
                            + len(chunk).to_bytes(2, "little")
                            + chunk
                        )
                        for _ in range(max(1, bin_packet_redundancy)):
                            pkt_bytes = send_bin_packet(sock, BIN_TYPE_FRAME_CHUNK, payload)
                            drain_bridge_logs(sock, rx_buf)
                            if inter_line_sleep > 0:
                                time.sleep(line_uart_time_s(pkt_bytes, uart_baud, uart_safety))
                                drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)

                    frame_crc = crc16_ccitt(rgb)
                    end_payload = frame_id.to_bytes(2, "little") + frame_crc.to_bytes(2, "little")
                    for _ in range(max(1, bin_packet_redundancy)):
                        end_bytes = send_bin_packet(sock, BIN_TYPE_FRAME_END, end_payload)
                        drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)
                        if inter_line_sleep > 0:
                            time.sleep(line_uart_time_s(end_bytes, uart_baud, uart_safety))
                            drain_bridge_logs(sock, rx_buf, print_lines=bin_debug)

                    ack_state, ack_msg = wait_for_bin_ack(
                        sock, rx_buf, frame_id, bin_ack_timeout, print_lines=bin_debug
                    )
                    if ack_state == "ack":
                        delivered = True
                        break
                    attempt += 1
                    if attempt <= max(0, bin_retries):
                        if bin_debug:
                            print(
                                f"[warn] binary resend frame id={frame_id} attempt={attempt} cause={ack_msg}"
                            )

                if not delivered:
                    print(f"[warn] binary frame dropped id={frame_id} after retries={bin_retries}")
            else:
                if frame_sync:
                    send_line(sock, "rx fs")
                    drain_bridge_logs(sock, rx_buf)
                    if inter_line_sleep > 0:
                        time.sleep(inter_line_sleep)
                        drain_bridge_logs(sock, rx_buf)

                for i in range(0, len(frame), chunk):
                    payload = frame[i : i + chunk]
                    if payload_mode == "raw":
                        send_line(sock, payload)
                        line_bytes = len(payload) + 1
                    else:
                        send_line(sock, f"rx u {payload}")
                        line_bytes = len("rx u ") + len(payload) + 1
                    drain_bridge_logs(sock, rx_buf)
                    if inter_line_sleep > 0:
                        time.sleep(line_uart_time_s(line_bytes, uart_baud, uart_safety))
                        drain_bridge_logs(sock, rx_buf)

                if frame_sync:
                    send_line(sock, "rx fe")
                    drain_bridge_logs(sock, rx_buf)
                    if inter_line_sleep > 0:
                        time.sleep(inter_line_sleep)
                        drain_bridge_logs(sock, rx_buf)

            sent += 1
            if print_every > 0 and sent % print_every == 0:
                print(f"[tx] sent frame #{sent}")

            if frame_durations_s is not None and len(frame_durations_s) == len(frames):
                frame_period = frame_durations_s[frame_idx]
                elapsed = time.monotonic() - frame_start
                sleep_s = frame_period - elapsed
                if sleep_s > 0:
                    time.sleep(sleep_s)
            elif fps > 0:
                frame_period = 1.0 / fps
                elapsed = time.monotonic() - frame_start
                sleep_s = frame_period - elapsed
                if sleep_s > 0:
                    time.sleep(sleep_s)

    return sent


def main() -> int:
    args = parse_args()
    argv = sys.argv[1:]

    if args.chunk < 1 or args.chunk > MAX_CHUNK:
        print(f"[err] --chunk must be in range 1..{MAX_CHUNK}", file=sys.stderr)
        return 2
    if args.loops < 0:
        print("[err] --loops must be >= 0", file=sys.stderr)
        return 2
    if args.port < 1 or args.port > 65535:
        print("[err] --port must be in range 1..65535", file=sys.stderr)
        return 2
    if args.print_every < 0:
        print("[err] --print-every must be >= 0", file=sys.stderr)
        return 2
    if args.uart_baud <= 0:
        print("[err] --uart-baud must be > 0", file=sys.stderr)
        return 2
    if args.uart_safety <= 0:
        print("[err] --uart-safety must be > 0", file=sys.stderr)
        return 2
    if args.robust_passes < 1:
        print("[err] --robust-passes must be >= 1", file=sys.stderr)
        return 2
    if args.bin_chunk_bytes < 1 or args.bin_chunk_bytes > 218:
        print("[err] --bin-chunk-bytes must be in range 1..218", file=sys.stderr)
        return 2
    if args.file_timing_scale <= 0:
        print("[err] --file-timing-scale must be > 0", file=sys.stderr)
        return 2
    if args.net_mode is not None and (args.net_mode < 0 or args.net_mode > 10):
        print("[err] --net-mode must be in range 0..10", file=sys.stderr)
        return 2
    if args.control_only and args.protocol != "binary":
        print("[err] --control-only requires --protocol binary", file=sys.stderr)
        return 2
    if args.bin_ack_timeout < 0:
        print("[err] --bin-ack-timeout must be >= 0", file=sys.stderr)
        return 2
    if args.bin_retries < 0:
        print("[err] --bin-retries must be >= 0", file=sys.stderr)
        return 2
    if args.bin_packet_redundancy < 1:
        print("[err] --bin-packet-redundancy must be >= 1", file=sys.stderr)
        return 2
    if args.bin_control_redundancy < 1:
        print("[err] --bin-control-redundancy must be >= 1", file=sys.stderr)
        return 2

    if args.protocol == "binary" and args.profile is not None:
        if args.profile == "reliable":
            if not option_provided(argv, "--fps"):
                args.fps = 4.0
            if not option_provided(argv, "--bin-chunk-bytes"):
                args.bin_chunk_bytes = 128
            if not option_provided(argv, "--bin-packet-redundancy"):
                args.bin_packet_redundancy = 2
            if not option_provided(argv, "--bin-control-redundancy"):
                args.bin_control_redundancy = 2
            if not option_provided(argv, "--bin-ack-timeout"):
                args.bin_ack_timeout = 0.30
            if not option_provided(argv, "--bin-retries"):
                args.bin_retries = 4
            if not option_provided(argv, "--uart-safety"):
                args.uart_safety = 1.25
        elif args.profile == "fast":
            if not option_provided(argv, "--fps"):
                args.fps = 6.0
            if not option_provided(argv, "--bin-chunk-bytes"):
                args.bin_chunk_bytes = 160
            if not option_provided(argv, "--bin-packet-redundancy"):
                args.bin_packet_redundancy = 1
            if not option_provided(argv, "--bin-control-redundancy"):
                args.bin_control_redundancy = 1
            if not option_provided(argv, "--bin-ack-timeout"):
                args.bin_ack_timeout = 0.20
            if not option_provided(argv, "--bin-retries"):
                args.bin_retries = 2
            if not option_provided(argv, "--uart-safety"):
                args.uart_safety = 1.10

    try:
        frames, frame_durations_s, trailing, total_hex_chars = load_frames(args.file, args.strict_length)
    except Exception as exc:  # noqa: BLE001
        print(f"[err] {exc}", file=sys.stderr)
        return 1

    if args.timing_mode == "fps":
        frame_durations_s = None
        timing_mode = "fps"
    elif args.timing_mode == "file":
        if frame_durations_s is None:
            print("[err] --timing-mode file requested, but input has no per-frame TTTT timing", file=sys.stderr)
            return 2
        timing_mode = "file"
    else:
        timing_mode = "file" if frame_durations_s is not None else "fps"

    if frame_durations_s is not None and args.file_timing_scale != 1.0:
        scale = args.file_timing_scale
        frame_durations_s = [max(0.001, d / scale) for d in frame_durations_s]
        timing_mode = f"{timing_mode}*{scale:g}"

    print(
        f"[info] file={args.file} hex={total_hex_chars} frames={len(frames)} trailing={trailing} "
        f"target={args.host}:{args.port} fps={args.fps} chunk={args.chunk} loops={args.loops} "
        f"protocol={args.protocol} frame_sync={args.frame_sync} payload_mode={args.payload_mode} "
        f"robust_passes={args.robust_passes} timing_mode={timing_mode} "
        f"uart_baud={args.uart_baud} uart_safety={args.uart_safety}"
    )

    if args.dry_run:
        return 0

    start = time.monotonic()
    try:
        with socket.create_connection((args.host, args.port), timeout=args.connect_timeout) as sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setblocking(False)
            rx_buf = bytearray()

            if not args.no_rx_on:
                send_line(sock, "rx on")
                drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)
            if not args.no_clear:
                send_line(sock, "rx clr")
                drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)
            if args.rx_log is not None:
                send_line(sock, f"rx log {args.rx_log}")
                drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)

            if args.protocol == "binary":
                if args.net_mode is not None:
                    for _ in range(args.bin_control_redundancy):
                        send_bin_packet(
                            sock,
                            BIN_TYPE_CTRL,
                            bytes([BIN_CTRL_SET_MODE, args.net_mode & 0xFF]),
                        )
                        drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)
                if args.net_display is not None:
                    for _ in range(args.bin_control_redundancy):
                        send_bin_packet(
                            sock,
                            BIN_TYPE_CTRL,
                            bytes([BIN_CTRL_SET_DISPLAY, int(args.net_display)]),
                        )
                        drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)
                for _ in range(args.bin_control_redundancy):
                    send_bin_packet(sock, BIN_TYPE_CTRL, bytes([BIN_CTRL_SET_STREAM, 1]))
                    drain_bridge_logs(sock, rx_buf, print_lines=args.bin_debug)
                if args.control_only:
                    if args.status_after:
                        send_line(sock, "p")
                        time.sleep(0.12)
                        drain_bridge_logs(sock, rx_buf, max_lines=150, print_lines=True)
                        send_line(sock, "rx p")
                        time.sleep(0.12)
                        drain_bridge_logs(sock, rx_buf, max_lines=150, print_lines=True)
                    print("[done] control packets sent")
                    return 0

            sent_frames = stream_frames(
                sock=sock,
                frames=frames,
                fps=args.fps,
                chunk=args.chunk,
                loops=args.loops,
                print_every=args.print_every,
                frame_sync=args.frame_sync,
                uart_baud=args.uart_baud,
                uart_safety=args.uart_safety,
                rx_buf=rx_buf,
                payload_mode=args.payload_mode,
                protocol=args.protocol,
                robust_passes=args.robust_passes,
                frame_durations_s=frame_durations_s,
                bin_chunk_bytes=args.bin_chunk_bytes,
                bin_ack_timeout=args.bin_ack_timeout,
                bin_retries=args.bin_retries,
                bin_packet_redundancy=args.bin_packet_redundancy,
                bin_debug=args.bin_debug,
            )

            if args.status_after:
                send_line(sock, "rx p")
                time.sleep(0.03)
                drain_bridge_logs(sock, rx_buf, max_lines=100)
            if rx_buf:
                # Print final partial line if bridge responded without trailing newline.
                tail = rx_buf.decode("utf-8", errors="replace").strip()
                if tail:
                    print(f"[rx] {tail}")

    except Exception as exc:  # noqa: BLE001
        print(f"[err] streaming failed: {exc}", file=sys.stderr)
        return 1

    dt = time.monotonic() - start
    fps_eff = (sent_frames / dt) if dt > 0 else 0.0
    print(f"[done] sent_frames={sent_frames} elapsed={dt:.2f}s effective_fps={fps_eff:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
