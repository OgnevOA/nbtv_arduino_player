"""Shared NBTVA 32-line constants and the PCM/WAV stream format.

In the radio architecture the server synthesizes the *finished* NBTV composite
signal (sync + levels + band-limit already baked in) and streams it as lossless
mono 16-bit PCM at 48 kHz. The ESP32 is a thin client: it buffers the PCM,
resamples it for disc-lock, applies gain/invert, and pushes it to the codec.

These constants mirror the calibrated values from the original mtv.py encoder.
"""

from __future__ import annotations

import struct

# --- NBTVA 32-line standard --------------------------------------------------
BASE_RATE = 48000            # native NBTV sample rate (device I2S at speed 1.0)
BASE_FPS = 12.5              # frames/sec at speed 1.0 (= 750 rpm)
N_LINES = 32                 # lines (= disc holes) per frame
SPL = 120                    # samples per line (48000 / (12.5 * 32))
SYNC_SPL = 6                 # sync pulse width in samples
ACTIVE_SPL = SPL - SYNC_SPL  # 114 active (picture) samples per line

COLS = N_LINES               # one "line" == one vertical image column
ROWS = ACTIVE_SPL            # picture rows sampled per line (natural 2:3)

# Signal levels (matches the known-good NBTVA CD track). White positive, black
# at zero, sync ~40% blacker-than-black.
WHITE = 28000
BLACK = 0
SYNC = -int(round(WHITE * 0.40))   # -11200

# --- stream format -----------------------------------------------------------
PCM_CHANNELS = 1
PCM_BITS = 16
BYTES_PER_SAMPLE = PCM_BITS // 8 * PCM_CHANNELS
FRAME_SAMPLES = N_LINES * SPL          # 3840 samples per NBTV frame


def wav_stream_header(rate: int = BASE_RATE, channels: int = PCM_CHANNELS,
                      bits: int = PCM_BITS) -> bytes:
    """A 44-byte canonical WAV header for an *endless* PCM stream.

    The RIFF/data sizes are set to 0xFFFFFFFF so players (ffplay/VLC) accept a
    stream of unknown length. The device simply discards these 44 bytes.
    """
    byte_rate = rate * channels * bits // 8
    block_align = channels * bits // 8
    return b"".join((
        b"RIFF", struct.pack("<I", 0xFFFFFFFF), b"WAVE",
        b"fmt ", struct.pack("<IHHIIHH", 16, 1, channels, rate,
                             byte_rate, block_align, bits),
        b"data", struct.pack("<I", 0xFFFFFFFF),
    ))
