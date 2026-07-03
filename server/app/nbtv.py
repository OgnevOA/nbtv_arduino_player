"""Shared NBTVA 32-line constants and the .nbtvf wire/cache format.

These constants define the contract between the server (which produces pixel
frames) and the ESP32 firmware (which turns them into the actual NBTV
waveform). They mirror the calibrated values from the original mtv.py encoder.

Wire model (see PORTABLE-NBTV-PLAN.md sections 2-3):
  * The server transmits *pixels only* - no sync, no levels, no audio.
  * Each frame is 32 lines x 48 rows, 8-bit grey, pre-arranged in scan order.
  * The device adds line sync, frame sync, level mapping and 48->114 interp.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import BinaryIO, Iterable, Iterator

# --- NBTVA 32-line standard --------------------------------------------------
BASE_RATE = 48000            # native NBTV sample rate (device I2S at speed 1.0)
BASE_FPS = 12.5              # frames/sec at speed 1.0 (= 750 rpm)
N_LINES = 32                 # lines (= disc holes) per frame
SPL = 120                    # samples per line (48000 / (12.5 * 32))
SYNC_SPL = 6                 # sync pulse width in samples
ACTIVE_SPL = SPL - SYNC_SPL  # 114 active samples per line (device-side)

# Transmitted picture resolution (device interpolates ROWS_TX -> ACTIVE_SPL).
COLS = N_LINES               # one "line" == one vertical image column
ROWS_TX = 48                 # rows sent per line; natural 2:3 resolution

FRAME_BYTES = COLS * ROWS_TX  # 32 * 48 = 1536 bytes per frame (8-bit)

# --- .nbtvf container format -------------------------------------------------
MAGIC = b"NBTV"
VERSION = 1

STREAM_HEADER_FMT = "<4sBBBB"          # magic, version, lines, rows, flags
STREAM_HEADER_SIZE = struct.calcsize(STREAM_HEADER_FMT)  # 8

FRAME_SYNC = b"\xA5\x5A"               # byte-realignment anchor
FRAME_HEADER_FMT = "<2sHH"             # sync word, frame seq, payload len
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FMT)    # 6

FLAG_PACKED_4BIT = 0x01                # reserved for v2 (unused in v1)


@dataclass
class StreamHeader:
    lines: int = N_LINES
    rows: int = ROWS_TX
    flags: int = 0

    def pack(self) -> bytes:
        return struct.pack(
            STREAM_HEADER_FMT, MAGIC, VERSION, self.lines, self.rows, self.flags
        )

    @classmethod
    def unpack(cls, blob: bytes) -> "StreamHeader":
        magic, version, lines, rows, flags = struct.unpack(
            STREAM_HEADER_FMT, blob[:STREAM_HEADER_SIZE]
        )
        if magic != MAGIC:
            raise ValueError(f"bad magic: {magic!r}")
        if version != VERSION:
            raise ValueError(f"unsupported version: {version}")
        return cls(lines=lines, rows=rows, flags=flags)


def frame_packet(seq: int, payload: bytes) -> bytes:
    """Wrap one frame payload with its per-frame header."""
    if len(payload) != FRAME_BYTES:
        raise ValueError(f"frame must be {FRAME_BYTES} bytes, got {len(payload)}")
    return struct.pack(FRAME_HEADER_FMT, FRAME_SYNC, seq & 0xFFFF, len(payload)) + payload


def write_nbtvf(fp: BinaryIO, frames: Iterable[bytes], flags: int = 0) -> int:
    """Write a full .nbtvf file (stream header + framed payloads). Returns count."""
    fp.write(StreamHeader(flags=flags).pack())
    n = 0
    for frame in frames:
        fp.write(frame_packet(n, frame))
        n += 1
    return n


def read_nbtvf_frames(fp: BinaryIO) -> Iterator[bytes]:
    """Yield raw frame payloads from a .nbtvf file (validates headers)."""
    header = fp.read(STREAM_HEADER_SIZE)
    StreamHeader.unpack(header)
    while True:
        fh = fp.read(FRAME_HEADER_SIZE)
        if len(fh) < FRAME_HEADER_SIZE:
            return
        sync, _seq, length = struct.unpack(FRAME_HEADER_FMT, fh)
        if sync != FRAME_SYNC:
            raise ValueError("lost frame sync in .nbtvf stream")
        payload = fp.read(length)
        if len(payload) < length:
            return
        yield payload
